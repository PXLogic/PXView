#!/usr/bin/env python3
"""
run_all_tests.py - Run all C decoder tests in parallel.
"""

import argparse
import csv
import json
import os
import re
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed

# Force UTF-8 for stdout/stderr to avoid GBK encoding errors on Windows
if sys.stdout.encoding.lower() != 'utf-8':
    import io
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding='utf-8', errors='replace')

# Configuration (Relative to this script)
TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(os.path.dirname(TESTS_DIR))
TESTDATA_ROOT = os.path.join(TESTS_DIR, 'testdata')
CSV_OUTPUT = os.path.join(TESTS_DIR, 'test_results.csv')

# Find decoder_test.exe
DECODER_TEST_EXE = os.path.join(PROJECT_ROOT, 'build.dir', 'decoder_test.exe')
if not os.path.exists(DECODER_TEST_EXE):
    DECODER_TEST_EXE = os.path.join(PROJECT_ROOT, 'build', 'bin', 'decoder_test.exe')

# Path to Python decoders
PY_DECODERS_PATH = os.path.join(PROJECT_ROOT, 'libsigrokdecode', 'decoders')

TIMEOUT_SECONDS = 30

# ---------------------------------------------------------------------------
#  Comparison Logic
# ---------------------------------------------------------------------------

def parse_all_numerics_with_units(text):
    """Extract all numeric values and their normalized values from a string."""
    text = text.replace('μ', 'µ').replace('u', 'µ')
    pattern = r'([-+]?\d*\.?\d+(?:[eE][-+]?\d+)?)\s*(ns|µs|ms|s|Hz|kHz|MHz|GHz|%)'
    matches = re.findall(pattern, text)
    results = []
    scale = {
        'ns': 1e-9, 'µs': 1e-6, 'ms': 1e-3, 's': 1.0,
        'Hz': 1.0, 'kHz': 1e3, 'MHz': 1e6, 'GHz': 1e9, '%': 1.0
    }
    for val_str, unit in matches:
        try:
            results.append(float(val_str) * scale.get(unit, 1.0))
        except ValueError: continue
    return results

def compare_text_semantic(py_text, c_text):
    if py_text == c_text: return True
    py_vals = parse_all_numerics_with_units(py_text)
    c_vals = parse_all_numerics_with_units(c_text)
    if len(py_vals) > 0 and len(py_vals) == len(c_vals):
        for pv, cv in zip(py_vals, c_vals):
            rel_diff = abs(pv - cv) / max(abs(pv), abs(cv), 1e-12)
            if rel_diff > 0.01: return False
        return True
    def normalize(t): return t.replace(' ', '').replace('μ', 'µ').replace('u', 'µ')
    return normalize(py_text) == normalize(c_text)

def _parse_matches_count(detail):
    """Extract the match count from a detail string like 'All 5 annotations match'."""
    m = re.match(r'All (\d+) annotations', detail)
    return int(m.group(1)) if m else -1

def compare_annotations(py_data, c_data):
    if not isinstance(py_data, dict) or not isinstance(c_data, dict):
        return False, "Output is not a JSON object"
    py_anns = py_data.get('annotations', [])
    c_anns = c_data.get('annotations', [])
    def group_by_sample_class(anns):
        """Group annotations by (start_sample, ann_class), allowing multiple per key."""
        grouped = {}
        for a in anns:
            key = (a.get('start_sample', 0), a.get('ann_class', 0))
            if key not in grouped: grouped[key] = []
            grouped[key].append(a)
        return grouped
    py_map = group_by_sample_class(py_anns)
    c_map = group_by_sample_class(c_anns)
    all_keys = sorted(set(list(py_map.keys()) + list(c_map.keys())))
    mismatches = []; matches_count = 0
    for key in all_keys:
        p_list = py_map.get(key, []); c_list = c_map.get(key, [])
        # Try to match annotations 1-to-1 using a greedy approach
        p_used = [False] * len(p_list); c_used = [False] * len(c_list)
        for i, pa in enumerate(p_list):
            best_j = -1
            for j, ca in enumerate(c_list):
                if c_used[j]: continue
                end_match = abs(pa.get('end_sample', 0) - ca.get('end_sample', 0)) <= 2
                pt, ct = pa.get('texts', []), ca.get('texts', [])
                text_match = len(pt) == len(ct) and all(compare_text_semantic(pt[k], ct[k]) for k in range(len(pt)))
                if end_match and text_match:
                    best_j = j; break
            if best_j >= 0:
                matches_count += 1; p_used[i] = True; c_used[best_j] = True
        # Report unmatched
        s, cls = key
        for i, pa in enumerate(p_list):
            if p_used[i]: continue
            mismatches.append(f"MISSED at sample {s}: Py has class {cls} ({pa.get('texts', [''])[0]}) but C doesn't")
        for j, ca in enumerate(c_list):
            if c_used[j]: continue
            mismatches.append(f"EXTRA at sample {s}: C has class {cls} ({ca.get('texts', [''])[0]}) but Py doesn't")
    if not mismatches:
        if matches_count == 0:
            return True, f"All 0 annotations match (vacuous - no output from either decoder)"
        return True, f"All {matches_count} annotations match"
    report = f"{matches_count} matches, {len(mismatches)} deviations found.\n" + "\n".join(mismatches[:10])
    if len(mismatches) > 10: report += f"\n... and {len(mismatches) - 10} more"
    return False, report

# ---------------------------------------------------------------------------
#  Runner
# ---------------------------------------------------------------------------

# Hardcoded mapping for IDs that cannot be derived heuristically
HARDCODED_ID_MAP = {
    'cjtag_oscan0': 'cjtag_oscan1',
    'eth_an': 'eth_auto_negotiation',
    'qspi': 'smart_qspi',
    'one_single_wire': 'OneSingleWire',
    'pcfx_ctrlr': 'pcfx_cntrlr',
    'onewire': 'onewire_link',
    'delta-sigma': 'delta_sigma',
}

def c_decoder_to_py(name):
    # Strip _c suffix
    base = name[:-2] if name.endswith('_c') else name
    # Check hardcoded map first
    if base in HARDCODED_ID_MAP:
        return HARDCODED_ID_MAP[base]
    return base

def run_decoder_test(decoder_name, testdata_dir, python_mode=False):
    filename = 'expected_py.json' if python_mode else 'actual_c.json'
    actual_json_path = os.path.join(testdata_dir, filename)
    
    # Base command
    cmd = [DECODER_TEST_EXE, "-d", decoder_name, "-t", testdata_dir, "-f", actual_json_path, "--tolerance", "2"]
    
    # ALWAYS use generate-only when creating our baselines to avoid spurious exit code 1
    # if an old expected.json exists in the directory.
    cmd.append("--generate-only")
    
    if python_mode:
        cmd.append("--python")
    
    # Try a few ID variants if the first one fails
    ids_to_try = [decoder_name]
    if python_mode:
        # Sigrok Python decoders often have IDs with different casing/dashes
        # variants: can-fd, can_fd, CAN, CAN-FD, Can-Fd etc.
        base = decoder_name
        ids_to_try.append(base.replace('_', '-'))
        ids_to_try.append(base.replace('-', '_'))
        ids_to_try.append(base.capitalize())
        ids_to_try.append(base.replace('_', '-').capitalize())
        ids_to_try.append(base.upper())
        ids_to_try.append(base.replace('_', '-').upper())
        # Deduplicate while preserving order
        ids_to_try = list(dict.fromkeys(ids_to_try))
    
    for d_id in ids_to_try:
        current_cmd = list(cmd)
        # Find index of -d and update d_id
        idx_d = current_cmd.index("-d")
        current_cmd[idx_d + 1] = d_id
        
        for attempt in range(3):
            try:
                proc = subprocess.run(current_cmd, capture_output=True, text=True, timeout=TIMEOUT_SECONDS, encoding='utf-8', errors='replace')
                if proc.returncode == 0:
                    # Success!
                    for read_attempt in range(5):
                        try:
                            with open(actual_json_path, 'r', encoding='utf-8', errors='replace') as f:
                                return json.load(f), None
                        except: time.sleep(0.1)
                
                # If it's a "decoder not found" error, try next ID
                if "decoder" in proc.stderr and "not found" in proc.stderr:
                    break # Break attempt loop, try next d_id
                
                if attempt == 2: return None, f"Exited with code {proc.returncode}: {proc.stderr}"
                time.sleep(0.2)
            except Exception as e:
                if attempt == 2: return None, str(e)
                time.sleep(0.2)
                
    return None, f"Decoder '{decoder_name}' not found (tried variants)"

def run_test(c_decoder, testdata_dir):
    try:
        config_path = os.path.join(testdata_dir, 'config.json')
        with open(config_path, 'r', encoding='utf-8') as f: config = json.load(f)
        if config.get('needs_upstream', False): return 'SKIP', "needs_upstream=true", 0
        
        t0 = time.time()
        py_data, py_err = run_decoder_test(c_decoder_to_py(c_decoder), testdata_dir, python_mode=True)
        if py_err: return 'ERROR', f"Py error: {py_err}", time.time() - t0
        
        c_data, c_err = run_decoder_test(c_decoder, testdata_dir, python_mode=False)
        if c_err: return 'ERROR', f"C error: {c_err}", time.time() - t0
        
        passed, detail = compare_annotations(py_data, c_data)
        if passed and _parse_matches_count(detail) == 0:
            return 'WARN', detail, time.time() - t0
        
        if not passed and config.get('expected_deviations', False):
            return 'DEVIATION', detail, time.time() - t0
            
        return ('PASS' if passed else 'FAIL'), detail, time.time() - t0
    except Exception as e: return 'ERROR', str(e), 0

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--all', action='store_true')
    parser.add_argument('--decoder')
    parser.add_argument('--jobs', type=int, default=16)
    # parser.add_argument('--jobs', type=int, default=os.cpu_count())

    args = parser.parse_args()

    test_cases = []
    if args.decoder:
        d_root = os.path.join(TESTDATA_ROOT, args.decoder)
        if os.path.isdir(d_root):
            for sub in os.listdir(d_root):
                if os.path.isdir(os.path.join(d_root, sub)): test_cases.append((args.decoder, os.path.join(d_root, sub)))
    elif args.all:
        for d in sorted(os.listdir(TESTDATA_ROOT)):
            d_root = os.path.join(TESTDATA_ROOT, d)
            if not os.path.isdir(d_root): continue
            for sub in os.listdir(d_root):
                p = os.path.join(d_root, sub)
                if os.path.isdir(p) and os.path.exists(os.path.join(p, 'config.json')): test_cases.append((d, p))

    if not test_cases:
        print("No test cases found."); return

    print(f"Running {len(test_cases)} tests in parallel ({args.jobs} jobs)...")
    results = []; counts = {'PASS': 0, 'WARN': 0, 'DEVIATION': 0, 'FAIL': 0, 'ERROR': 0, 'SKIP': 0}
    
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        f_to_test = {executor.submit(run_test, d, p): (d, p) for d, p in test_cases}
        for i, f in enumerate(as_completed(f_to_test), 1):
            d, p = f_to_test[f]
            status, detail, elapsed = f.result()
            results.append((d, p, status, detail, elapsed))
            counts[status] += 1
            print(f"[{i:3}/{len(test_cases):3}] {d:25} | {status:5} | {elapsed:4.1f}s")
            if status in ('FAIL', 'ERROR', 'WARN'): print(f"      -> {detail.splitlines()[0]}")

    with open(CSV_OUTPUT, 'w', newline='', encoding='utf-8') as f:
        writer = csv.writer(f)
        writer.writerow(['decoder', 'testdata', 'status', 'detail', 'elapsed_s'])
        for r in results: writer.writerow(r)

    # Generate Markdown Dashboard
    dashboard_path = os.path.join(PROJECT_ROOT, 'Dashboard.md')
    with open(dashboard_path, 'w', encoding='utf-8') as f:
        f.write("# CI Verification Dashboard\n\n")
        f.write("## Summary\n\n")
        f.write("| Total | PASS 🟢 | DEVIATION 🟡 | WARN 🟠 | FAIL 🔴 | ERROR 💥 | SKIP ⚪ |\n")
        f.write("|---|---|---|---|---|---|---|\n")
        f.write(f"| {len(results)} | {counts['PASS']} | {counts['DEVIATION']} | {counts['WARN']} | {counts['FAIL']} | {counts['ERROR']} | {counts['SKIP']} |\n\n")
        
        f.write("## Failures & Errors\n\n")
        if counts['FAIL'] == 0 and counts['ERROR'] == 0:
            f.write("No failures or errors!\n\n")
        else:
            for r in results:
                d, p, status, detail, elapsed = r
                if status in ('FAIL', 'ERROR'):
                    f.write(f"### {d}\n")
                    f.write(f"**Status**: {status} ({elapsed:.1f}s)\n\n")
                    f.write("```text\n")
                    f.write(f"{detail}\n")
                    f.write("```\n\n")

        f.write("## All Decoders\n\n")
        f.write("| Decoder | Status | Time (s) | Detail |\n")
        f.write("|---|---|---|---|\n")
        # Sort by status priority (FAIL, ERROR, WARN, DEVIATION, PASS, SKIP)
        status_priority = {'FAIL': 0, 'ERROR': 1, 'WARN': 2, 'DEVIATION': 3, 'PASS': 4, 'SKIP': 5}
        sorted_results = sorted(results, key=lambda x: (status_priority[x[2]], x[0]))
        for r in sorted_results:
            d, p, status, detail, elapsed = r
            emoji = {'PASS': '🟢', 'DEVIATION': '🟡', 'WARN': '🟠', 'FAIL': '🔴', 'ERROR': '💥', 'SKIP': '⚪'}[status]
            short_detail = detail.splitlines()[0] if detail else ""
            f.write(f"| {d} | {status} {emoji} | {elapsed:.1f} | {short_detail} |\n")

    print("\n" + "="*70 + f"\nSUMMARY\n" + "="*70)
    print(f"  Total: {len(results)}  PASS: {counts['PASS']}  DEVIATION: {counts['DEVIATION']}  WARN: {counts['WARN']}  FAIL: {counts['FAIL']}  ERROR: {counts['ERROR']}  SKIP: {counts['SKIP']}")
    print(f"  Dashboard generated at: {dashboard_path}")
    if counts['FAIL'] > 0 or counts['ERROR'] > 0: sys.exit(1)

if __name__ == '__main__':
    main()
