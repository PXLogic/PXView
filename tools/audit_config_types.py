#!/usr/bin/env python3
"""
CI audit script: cross-validate GVariant types used by each hardware driver
against the centralized type table declared in hwdriver.c.

PURPOSE
=======
libsigrok uses GVariant (a dynamically-typed GLib container) to pass config
values between the frontend (PXView) and hardware drivers.  The "official"
type for each SR_CONF_* key is declared in hwdriver.c's sr_key_info_config[]
table (e.g. {SR_CONF_TRIGGER_SLOPE, SR_T_UINT8, ...}).

However, the drivers themselves create/consume GVariants with whatever type
they choose (g_variant_new_byte, g_variant_new_string, etc.).  If a driver
uses a different type than the table declares, the mismatch is NOT caught at
compile time (C has no generics; GVariant* is type-erased) and is only
discovered at runtime — often only via log messages or silent data corruption.

This script scans all driver source files and compares the actual GVariant
type functions used in config_get (g_variant_new_*) and config_set
(g_variant_get_*) against the hwdriver.c table, reporting any mismatches.

USAGE
=====
    python3 tools/audit_config_types.py

Exit code 0 = no fork-driver mismatches found (or only known-acceptable ones).
Exit code 1 = fork-driver mismatches found that should be fixed.

REQUIREMENTS
============
Python 3.6+, no third-party dependencies (stdlib only).
"""

import os
import re
import sys
from pathlib import Path
from collections import defaultdict

# ---------------------------------------------------------------------------
# GVariant function name -> SR_T_* datatype mapping
# ---------------------------------------------------------------------------
# Maps the g_variant_new_* / g_variant_get_* function suffix to the
# corresponding enum sr_datatype value from libsigrok.h.
GVARIANT_FUNC_TO_SRTYPE = {
    'byte':       'SR_T_UINT8',
    'int16':      'SR_T_INT16',
    'uint16':     'SR_T_UINT16',
    'int32':      'SR_T_INT32',
    'uint32':     'SR_T_UINT32',
    'int64':      'SR_T_INT64',   # not in table, but used by some drivers
    'uint64':     'SR_T_UINT64',
    'double':     'SR_T_FLOAT',
    'string':     'SR_T_STRING',
    'boolean':    'SR_T_BOOL',
    'strv':       'SR_T_STRING',  # string array -> list path, treated as string
    'fixed_array': None,          # depends on element type -- skip
}

# std_str_idx() consumes a string GVariant -- maps to SR_T_STRING
STD_STR_IDX_TYPE = 'SR_T_STRING'

# Known acceptable mismatches (driver returns a compatible type that the
# GUI handles via runtime type dispatch).  These are NOT bugs.
# Format: (driver_dir, key_name, actual_type, table_type, reason)
KNOWN_ACCEPTABLE = {
    # pxlogic GET returns int32 for DEVICE_MODE; GUI's get_config_int32()
    # dispatches by actual GVariant type at runtime.
    ('pxlogic', 'DEVICE_MODE', 'SR_T_INT32', 'SR_T_INT16',
     'GUI get_config_int32() dispatches by runtime type'),
}

# Fork drivers actively used by PXView.  Mismatches in these drivers are
# treated as ERRORS (CI failure).  All other drivers are "upstream" and
# treated as warnings -- PXView's fork table may deliberately use different
# types (e.g. SR_T_UINT8 for TRIGGER_SLOPE) that upstream drivers don't match.
FORK_DRIVERS = {
    'demo',
    'dreamsourcelab-dslogic',
    'pxlogic',
}

# Keys whose type was deliberately changed in the PXView fork.  Upstream
# drivers that still use the upstream type are NOT bugs -- they're just
# not used by PXView.  Listed here so the audit can skip them silently.
FORK_CHANGED_KEYS = {
    # Fork uses byte; upstream uses string.  Only demo/DSL/pxlogic are fork.
    'TRIGGER_SLOPE',
    'TRIGGER_SOURCE',
    # Fork uses string; upstream uses bool.
    'FILTER',
    # Fork uses string; upstream uses uint8.
    'BANDWIDTH_LIMIT',
    # Fork uses uint8; upstream uses int32.
    'MAX_HEIGHT_VALUE',
}


# ---------------------------------------------------------------------------
# 1. Parse hwdriver.c type table
# ---------------------------------------------------------------------------

def parse_hwdriver_table(hwdriver_path):
    """Parse sr_key_info_config[] from hwdriver.c.

    Returns dict: { 'KEY_NAME': 'SR_T_DATATYPE', ... }
    e.g. { 'TRIGGER_SLOPE': 'SR_T_UINT8', 'FILTER': 'SR_T_STRING', ... }
    """
    text = hwdriver_path.read_text(encoding='utf-8', errors='replace')

    # Match lines like: {SR_CONF_TRIGGER_SLOPE, SR_T_UINT8, "triggerslope",
    # Also handles multi-line entries where the type is on a continuation line.
    # Pattern: {SR_CONF_<KEY>, SR_T_<TYPE>,
    pattern = re.compile(
        r'\{SR_CONF_(\w+)\s*,\s*SR_T_(\w+)\s*,',
        re.MULTILINE
    )

    table = {}
    for m in pattern.finditer(text):
        key_name = m.group(1)
        type_name = 'SR_T_' + m.group(2)
        table[key_name] = type_name

    return table


# ---------------------------------------------------------------------------
# 2. Scan driver source files for GVariant type usage
# ---------------------------------------------------------------------------

def find_case_blocks(text):
    """Find all 'case SR_CONF_*:' blocks in a source file.

    Returns list of (key_name, start_line, body_text) tuples.
    The body_text extends from the case line to the next 'break;',
    'case ', 'default:', or closing brace at the same or lower indent.
    """
    lines = text.split('\n')
    results = []

    case_re = re.compile(r'\bcase\s+SR_CONF_(\w+)\s*:')

    for i, line in enumerate(lines):
        m = case_re.search(line)
        if not m:
            continue

        key_name = m.group(1)
        # Collect lines until we hit break; / case / default / } at
        # the same or lower indentation level.
        base_indent = len(line) - len(line.lstrip())
        body_lines = []
        for j in range(i + 1, min(i + 30, len(lines))):  # max 30 lines per case
            next_line = lines[j]
            stripped = next_line.strip()
            if not stripped:
                continue
            # Stop at break;
            if stripped.startswith('break;') or stripped.startswith('break ;'):
                break
            # Stop at next case/default at same or lower indent
            curr_indent = len(next_line) - len(next_line.lstrip())
            if curr_indent <= base_indent:
                if stripped.startswith('case ') or stripped.startswith('default:'):
                    break
                if stripped == '}':
                    break
            body_lines.append(next_line)

        body = '\n'.join(body_lines)
        results.append((key_name, i + 1, body))

    return results


def extract_gvariant_types(body):
    """Extract GVariant type info from a case block body.

    Returns (get_types, set_types) where each is a set of SR_T_* strings.
    get_types: types created via g_variant_new_* (config_get path)
    set_types: types consumed via g_variant_get_* (config_set path)
    """
    get_types = set()
    set_types = set()

    # g_variant_new_<suffix>(...)  -> config_get creates data
    for m in re.finditer(r'g_variant_new_(\w+)\s*\(', body):
        suffix = m.group(1)
        sr_type = GVARIANT_FUNC_TO_SRTYPE.get(suffix)
        if sr_type:
            get_types.add(sr_type)

    # g_variant_get_<suffix>(...)  -> config_set reads data
    for m in re.finditer(r'g_variant_get_(\w+)\s*\(', body):
        suffix = m.group(1)
        sr_type = GVARIANT_FUNC_TO_SRTYPE.get(suffix)
        if sr_type:
            set_types.add(sr_type)

    # std_str_idx(data, ...) -> consumes a string GVariant
    if 'std_str_idx' in body:
        set_types.add(STD_STR_IDX_TYPE)

    # g_variant_new_fixed_array with G_VARIANT_TYPE("y") -> byte array
    if re.search(r'g_variant_new_fixed_array\s*\(\s*G_VARIANT_TYPE\s*\(\s*"y"', body):
        get_types.add('SR_T_UINT8')

    return get_types, set_types


def scan_driver_file(filepath, driver_name):
    """Scan a single driver source file.

    Returns list of dicts with keys:
        driver, file, line, key, get_types, set_types
    """
    text = filepath.read_text(encoding='utf-8', errors='replace')

    # Quick check: does this file have any SR_CONF cases?
    if 'SR_CONF_' not in text:
        return []

    results = []
    for key_name, line_no, body in find_case_blocks(text):
        get_types, set_types = extract_gvariant_types(body)
        results.append({
            'driver': driver_name,
            'file': str(filepath),
            'line': line_no,
            'key': key_name,
            'get_types': get_types,
            'set_types': set_types,
        })

    return results


# ---------------------------------------------------------------------------
# 3. Cross-validate and report
# ---------------------------------------------------------------------------

def cross_validate(table, driver_results):
    """Compare driver usage against the table.

    Returns (fork_mismatches, upstream_mismatches) -- fork mismatches are
    CI-blocking errors; upstream mismatches are informational warnings.
    """
    fork_mismatches = []
    upstream_mismatches = []

    for entry in driver_results:
        key = entry['key']
        table_type = table.get(key)

        if table_type is None:
            # Key not in table -- can't validate, skip
            continue

        # Skip config_list strv returns for keys whose type was deliberately
        # changed in the fork -- upstream drivers returning strv for LIST is
        # not a bug.
        if key in FORK_CHANGED_KEYS and entry['driver'] not in FORK_DRIVERS:
            continue

        is_fork = entry['driver'] in FORK_DRIVERS
        target_list = fork_mismatches if is_fork else upstream_mismatches

        # Check GET path (g_variant_new_* types vs table)
        for actual_type in entry['get_types']:
            if actual_type != table_type:
                is_acceptable = any(
                    entry['driver'] == ka[0] and key == ka[1]
                    and actual_type == ka[2] and table_type == ka[3]
                    for ka in KNOWN_ACCEPTABLE
                )
                if not is_acceptable:
                    target_list.append({
                        **entry,
                        'path': 'GET',
                        'table_type': table_type,
                        'actual_type': actual_type,
                    })

        # Check SET path (g_variant_get_* types vs table)
        for actual_type in entry['set_types']:
            if actual_type != table_type:
                is_acceptable = any(
                    entry['driver'] == ka[0] and key == ka[1]
                    and actual_type == ka[2] and table_type == ka[3]
                    for ka in KNOWN_ACCEPTABLE
                )
                if not is_acceptable:
                    target_list.append({
                        **entry,
                        'path': 'SET',
                        'table_type': table_type,
                        'actual_type': actual_type,
                    })

    return fork_mismatches, upstream_mismatches


def print_report(table, fork_mismatches, upstream_mismatches, driver_results):
    """Print the audit report.

    Fork driver mismatches are ERRORS (exit 1).
    Upstream driver mismatches are WARNINGS (informational only).
    """
    print("=" * 72)
    print("  libsigrok Config Key Type Audit")
    print("=" * 72)
    print()

    # Summary
    total_keys = len(table)
    total_entries = len(driver_results)

    print(f"  hwdriver.c type table entries  : {total_keys}")
    print(f"  Driver case blocks scanned     : {total_entries}")
    print(f"  Fork driver mismatches (ERROR) : {len(fork_mismatches)}")
    print(f"  Upstream mismatches (WARN)     : {len(upstream_mismatches)}")
    print()

    # Fork driver mismatches (these are real bugs)
    if fork_mismatches:
        print("  FORK DRIVER MISMATCHES (must fix):")
        print("  " + "-" * 68)
        for m in fork_mismatches:
            print(f"  {m['driver']}/{Path(m['file']).name}:{m['line']}")
            print(f"    Key:      SR_CONF_{m['key']}")
            print(f"    Path:     {m['path']}")
            print(f"    Table:    {m['table_type']}")
            print(f"    Actual:   {m['actual_type']}")
            if m['path'] == 'GET' and m['get_types']:
                print(f"    All GET:  {', '.join(sorted(m['get_types']))}")
            if m['path'] == 'SET' and m['set_types']:
                print(f"    All SET:  {', '.join(sorted(m['set_types']))}")
            print()
    else:
        print("  \u2713 All fork driver GVariant types match the hwdriver.c table.")
        print()

    # Upstream driver mismatches (informational)
    if upstream_mismatches:
        print("  UPSTREAM DRIVER MISMATCHES (informational -- PXView fork table")
        print("  deliberately uses different types for some keys):")
        print("  " + "-" * 68)
        # Group by key for readability
        by_key = {}
        for m in upstream_mismatches:
            by_key.setdefault(m['key'], []).append(m)
        for key in sorted(by_key):
            drivers = sorted(set(m['driver'] for m in by_key[key]))
            example = by_key[key][0]
            print(f"  SR_CONF_{key}: table={example['table_type']}, "
                  f"upstream uses {example['actual_type']} "
                  f"({len(drivers)} drivers: {', '.join(drivers[:5])}"
                  f"{'...' if len(drivers) > 5 else ''})")
        print()

    # Known acceptable mismatches
    if KNOWN_ACCEPTABLE:
        print("  KNOWN ACCEPTABLE MISMATCHES (not reported as bugs):")
        print("  " + "-" * 68)
        for driver, key, actual, table_t, reason in KNOWN_ACCEPTABLE:
            print(f"  {driver}/SR_CONF_{key}: {actual} (actual) vs {table_t} (table)")
            if reason:
                print(f"    Reason: {reason}")
        print()

    if fork_mismatches:
        print("  To fix a fork driver mismatch, either:")
        print("    1. Change the hwdriver.c table to match the driver's actual type")
        print("    2. Change the driver to use the type declared in the table")
        print("    3. Add an entry to KNOWN_ACCEPTABLE if the mismatch is intentional")
        print("       and handled by the GUI's runtime type dispatch")
        print()

    return 1 if fork_mismatches else 0


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    # Locate the repo root from this script's location
    script_dir = Path(__file__).resolve().parent
    repo_root = script_dir.parent

    hwdriver_path = repo_root / 'libsigrok' / 'src' / 'hwdriver.c'
    hw_dir = repo_root / 'libsigrok' / 'src' / 'hardware'

    if not hwdriver_path.exists():
        print(f"ERROR: Cannot find {hwdriver_path}", file=sys.stderr)
        return 2

    if not hw_dir.exists():
        print(f"ERROR: Cannot find {hw_dir}", file=sys.stderr)
        return 2

    # 1. Parse the type table
    table = parse_hwdriver_table(hwdriver_path)
    print(f"Parsed {len(table)} config key types from hwdriver.c")

    # 2. Scan all driver source files
    driver_results = []
    for driver_dir in sorted(hw_dir.iterdir()):
        if not driver_dir.is_dir():
            continue
        driver_name = driver_dir.name
        for c_file in sorted(driver_dir.glob('*.c')):
            entries = scan_driver_file(c_file, driver_name)
            driver_results.extend(entries)

    print(f"Scanned {len(driver_results)} case blocks across "
          f"{len(set(r['driver'] for r in driver_results))} drivers")
    print()

    # 3. Cross-validate
    fork_mismatches, upstream_mismatches = cross_validate(table, driver_results)

    # 4. Report (exit 1 only for fork driver mismatches)
    return print_report(table, fork_mismatches, upstream_mismatches,
                        driver_results)


if __name__ == '__main__':
    sys.exit(main())
