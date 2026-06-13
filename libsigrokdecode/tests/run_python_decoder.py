#!/usr/bin/env python3
"""
run_python_decoder.py - Run Python decoders to generate expected.json reference output.

This script wraps the decoder_test C program with the --python flag to run
Python protocol decoders on test data and produce expected.json files that
serve as reference output for C decoder validation.

Usage:
    python run_python_decoder.py -d <decoder_name> -t <testdata_dir> [-o <output_dir>]
    python run_python_decoder.py --all [-t <testdata_root>]

Examples:
    # Run single decoder test
    python run_python_decoder.py -d spi -t testdata/spi/test1

    # Run with C decoder name (auto-stripped _c suffix)
    python run_python_decoder.py -d spi_c -t testdata/spi/test1

    # Batch mode: generate expected.json for all testdata subdirectories
    python run_python_decoder.py --all -t testdata

    # Specify output directory
    python run_python_decoder.py -d i2c -t testdata/i2c/test1 -o results/
"""

import argparse
import json
import os
import shutil
import subprocess
import sys


# Mapping from C decoder names to Python decoder names.
# Most C decoders follow the pattern <name>_c -> <name>, but some
# Python decoder directories use hyphens instead of underscores.
C_TO_PY_DECODER_MAP = {
    'can_fd_c': 'can-fd',
    'cjtag_oscan0_c': 'cjtag-oscan0',
    'delta_sigma_c': 'delta-sigma',
    'ir_irmp_c': 'ir_irmp',
    'ir_ltto_c': 'ir_ltto',
    'ir_ltto_decode_c': 'ir_ltto_decode',
    'ir_nec_c': 'ir_nec',
    'ir_rc5_c': 'ir_rc5',
    'ir_rc6_c': 'ir_rc6',
    'ir_recoil_c': 'ir_recoil',
    'ir_sirc_c': 'ir_sirc',
    'one_single_wire_c': 'one_single_wire',
    'onewire_link_c': 'onewire_link',
    'onewire_network_c': 'onewire_network',
    'pcfx_ctrlr_c': 'pcfx-ctrlr',
    'rgb_led_spi_c': 'rgb_led_spi',
    'rgb_led_ws281x_c': 'rgb_led_ws281x',
    'rinnai_control_panel_c': 'rinnai-control-panel',
    'sdcard_sd_c': 'sdcard_sd',
    'sdcard_spi_c': 'sdcard_spi',
    'sony_md_c': 'sony_md',
    'sony_md_decode_c': 'sony_md_decode',
    'spi_dual_quad_c': 'spi_dual_quad',
    'spi_fast_c': 'spi-fast',
    'spi_tpm_c': 'spi_tpm',
    'tpm_fifo_tis_c': 'tpm_fifo_tis',
    'tpm_tis_i2c_c': 'tpm_tis_i2c',
    'tpm_tis_spi_c': 'tpm_tis_spi',
    'uart_fast_c': 'uart-fast',
    'usb_power_delivery_c': 'usb_power_delivery',
    'usb_packet_c': 'usb_packet',
    'usb_request_c': 'usb_request',
    'usb_signalling_c': 'usb_signalling',
    'xy2_100_c': 'xy2-100',
    'i2c_packet_c': 'i2c_packet',
    'i2cfilter_c': 'i2cfilter',
    'i2cdemux_c': 'i2cdemux',
    'jtag_avr_c': 'jtag_avr',
    'jtag_ejtag_c': 'jtag_ejtag',
    'jtag_stm32_c': 'jtag_stm32',
    'ps2_keyboard_c': 'ps2_keyboard',
    'ps2_mouse_c': 'ps2_mouse',
}


def c_decoder_to_py(c_name):
    """Convert a C decoder name to the corresponding Python decoder name.

    Handles the common pattern of stripping the '_c' suffix, as well as
    special cases where the Python decoder directory name differs
    (e.g. can_fd_c -> can-fd).

    Args:
        c_name: C decoder ID (e.g. 'spi_c', 'can_fd_c', or already 'spi')

    Returns:
        Python decoder ID (e.g. 'spi', 'can-fd')
    """
    # Check explicit mapping first
    if c_name in C_TO_PY_DECODER_MAP:
        return C_TO_PY_DECODER_MAP[c_name]

    # Strip '_c' suffix if present
    if c_name.endswith('_c'):
        return c_name[:-2]

    # Already a Python name (no _c suffix)
    return c_name


def find_decoder_test():
    """Find the decoder_test executable.

    Searches in the following locations:
    1. Same directory as this script
    2. build.dir/ directory (relative to project root)
    3. build/ directory (relative to project root)
    4. System PATH

    Returns:
        Absolute path to decoder_test executable, or None if not found.
    """
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_root = os.path.abspath(os.path.join(script_dir, '..', '..'))

    # Candidate names (Windows and Unix)
    candidates = ['decoder_test.exe', 'decoder_test']

    # Search paths in priority order
    search_dirs = [
        script_dir,
        os.path.join(project_root, 'build.dir'),
        os.path.join(project_root, 'build.dir', 'bin'),
        os.path.join(project_root, 'build'),
        os.path.join(project_root, 'build', 'bin'),
    ]

    for search_dir in search_dirs:
        for name in candidates:
            path = os.path.join(search_dir, name)
            if os.path.isfile(path):
                return path

    # Try system PATH
    path = shutil.which('decoder_test')
    if path:
        return path

    return None


def run_single(decoder_name, testdata_dir, output_dir=None):
    """Run decoder_test for a single test case.

    Args:
        decoder_name: Python decoder ID (e.g. 'spi', 'i2c')
        testdata_dir: Path to directory containing config.json and input.bin
        output_dir: Output directory for expected.json (default: testdata_dir)

    Returns:
        True on success, False on failure.
    """
    decoder_test = find_decoder_test()
    if not decoder_test:
        print("ERROR: decoder_test executable not found.", file=sys.stderr)
        print("", file=sys.stderr)
        print("Searched in:", file=sys.stderr)
        script_dir = os.path.dirname(os.path.abspath(__file__))
        project_root = os.path.abspath(os.path.join(script_dir, '..', '..'))
        print("  - Script directory: {}".format(script_dir), file=sys.stderr)
        print("  - {}/build.dir/".format(project_root), file=sys.stderr)
        print("  - {}/build.dir/bin/".format(project_root), file=sys.stderr)
        print("  - {}/build/".format(project_root), file=sys.stderr)
        print("  - {}/build/bin/".format(project_root), file=sys.stderr)
        print("  - System PATH", file=sys.stderr)
        print("", file=sys.stderr)
        print("Please build the project first, or add decoder_test to your PATH.", file=sys.stderr)
        return False

    testdata_dir = os.path.abspath(testdata_dir)
    if not os.path.isdir(testdata_dir):
        print("ERROR: Test data directory not found: {}".format(testdata_dir), file=sys.stderr)
        return False

    config_path = os.path.join(testdata_dir, 'config.json')
    input_path = os.path.join(testdata_dir, 'input.bin')

    if not os.path.isfile(config_path):
        print("ERROR: config.json not found in {}".format(testdata_dir), file=sys.stderr)
        return False

    if not os.path.isfile(input_path):
        print("ERROR: input.bin not found in {}".format(testdata_dir), file=sys.stderr)
        return False

    if output_dir is None:
        output_dir = testdata_dir
    else:
        output_dir = os.path.abspath(output_dir)
        os.makedirs(output_dir, exist_ok=True)

    # Build the command
    cmd = [
        decoder_test,
        '--python',
        '-d', decoder_name,
        '-t', testdata_dir,
        '--generate-only',
        '-o', output_dir,
    ]

    # Set PY_DECODERS_PATH environment variable for decoder_test
    env = os.environ.copy()
    if 'PY_DECODERS_PATH' not in env:
        # Auto-detect Python decoders directory
        script_dir = os.path.dirname(os.path.abspath(__file__))
        py_dec_candidates = [
            os.path.join(script_dir, '..', 'decoders'),  # libsigrokdecode/decoders/
            os.path.join(script_dir, '..', '..', 'libsigrokdecode', 'decoders'),
        ]
        for candidate in py_dec_candidates:
            candidate = os.path.normpath(candidate)
            if os.path.isdir(candidate):
                env['PY_DECODERS_PATH'] = candidate
                break

    print("Running: {}".format(' '.join(cmd)))

    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=120,
            env=env,
        )
    except FileNotFoundError:
        print("ERROR: Failed to execute decoder_test: {}".format(decoder_test), file=sys.stderr)
        return False
    except subprocess.TimeoutExpired:
        print("ERROR: decoder_test timed out after 120 seconds.", file=sys.stderr)
        return False

    if result.stdout:
        print(result.stdout, end='')
    if result.stderr:
        print(result.stderr, end='', file=sys.stderr)

    if result.returncode != 0:
        print("ERROR: decoder_test exited with code {}".format(result.returncode), file=sys.stderr)
        return False

    # Rename actual.json to expected.json
    actual_path = os.path.join(output_dir, 'actual.json')
    expected_path = os.path.join(output_dir, 'expected.json')

    if not os.path.isfile(actual_path):
        print("WARNING: actual.json not found in {}".format(output_dir), file=sys.stderr)
        print("  decoder_test may not have produced output.", file=sys.stderr)
        return False

    # Validate JSON before renaming
    try:
        with open(actual_path, 'r', encoding='utf-8') as f:
            data = json.load(f)
        annotation_count = len(data) if isinstance(data, list) else 0
        print("Generated expected.json with {} annotations.".format(annotation_count))
    except json.JSONDecodeError as e:
        print("WARNING: actual.json contains invalid JSON: {}".format(e), file=sys.stderr)
        # Still rename it - it may be useful for debugging
    except Exception as e:
        print("WARNING: Could not read actual.json: {}".format(e), file=sys.stderr)

    try:
        if os.path.isfile(expected_path):
            os.remove(expected_path)
        os.rename(actual_path, expected_path)
    except OSError as e:
        print("ERROR: Failed to rename actual.json to expected.json: {}".format(e), file=sys.stderr)
        return False

    return True


def run_batch(testdata_root, output_root=None):
    """Run decoder_test for all test data subdirectories.

    Scans all subdirectories under testdata_root that contain both
    config.json and input.bin, and generates expected.json for each.

    Args:
        testdata_root: Root directory containing test data subdirectories.
        output_root: Root output directory (default: same as testdata_root).

    Returns:
        Tuple of (success_count, failure_count).
    """
    testdata_root = os.path.abspath(testdata_root)
    if not os.path.isdir(testdata_root):
        print("ERROR: Test data root directory not found: {}".format(testdata_root), file=sys.stderr)
        return 0, 0

    success_count = 0
    failure_count = 0
    skipped_count = 0

    # Walk through all subdirectories looking for config.json + input.bin
    for dirpath, dirnames, filenames in os.walk(testdata_root):
        if 'config.json' in filenames and 'input.bin' in filenames:
            # Determine decoder name from config.json
            config_path = os.path.join(dirpath, 'config.json')
            try:
                with open(config_path, 'r', encoding='utf-8') as f:
                    config = json.load(f)
            except (json.JSONDecodeError, OSError) as e:
                print("SKIP: Cannot read config.json in {}: {}".format(dirpath, e))
                skipped_count += 1
                continue

            # Get decoder name from config
            c_decoder = config.get('decoder', '')
            if not c_decoder:
                # Try to infer from directory structure
                # e.g. testdata/spi/test1 -> decoder = spi
                rel_path = os.path.relpath(dirpath, testdata_root)
                parts = rel_path.replace('\\', '/').split('/')
                if len(parts) >= 2:
                    c_decoder = parts[0] + '_c'
                else:
                    print("SKIP: No decoder name in config.json: {}".format(dirpath))
                    skipped_count += 1
                    continue

            py_decoder = c_decoder_to_py(c_decoder)

            # Determine output directory
            if output_root:
                rel_path = os.path.relpath(dirpath, testdata_root)
                out_dir = os.path.join(output_root, rel_path)
            else:
                out_dir = dirpath

            print("\n--- Processing: {} (decoder: {} -> {}) ---".format(
                dirpath, c_decoder, py_decoder))

            if run_single(py_decoder, dirpath, out_dir):
                success_count += 1
            else:
                failure_count += 1

    print("\n" + "=" * 60)
    print("Batch complete: {} succeeded, {} failed, {} skipped".format(
        success_count, failure_count, skipped_count))

    return success_count, failure_count


def main():
    parser = argparse.ArgumentParser(
        description='Run Python decoders to generate expected.json reference output.',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Run single decoder test
  python run_python_decoder.py -d spi -t testdata/spi/test1

  # Use C decoder name (auto-stripped _c suffix)
  python run_python_decoder.py -d spi_c -t testdata/spi/test1

  # Batch mode: generate expected.json for all testdata subdirectories
  python run_python_decoder.py --all -t testdata

  # Specify output directory
  python run_python_decoder.py -d i2c -t testdata/i2c/test1 -o results/
""")

    parser.add_argument('-d', '--decoder',
                        help='Python decoder ID (e.g. "spi" - without _c suffix). '
                             'C decoder names with _c suffix are auto-converted.')
    parser.add_argument('-t', '--testdata',
                        help='Test data directory containing config.json and input.bin, '
                             'or root directory in batch mode.')
    parser.add_argument('-o', '--output',
                        help='Output directory for expected.json (default: same as testdata)')
    parser.add_argument('--all', action='store_true',
                        help='Batch mode: scan all testdata subdirectories and '
                             'generate expected.json for each')

    args = parser.parse_args()

    if args.all:
        if not args.testdata:
            parser.error('--all mode requires -t/--testdata to specify the root directory')
        run_batch(args.testdata, args.output)
    else:
        if not args.decoder:
            parser.error('-d/--decoder is required in single mode')
        if not args.testdata:
            parser.error('-t/--testdata is required in single mode')

        # Convert C decoder name to Python decoder name
        py_decoder = c_decoder_to_py(args.decoder)
        if py_decoder != args.decoder:
            print("Mapping C decoder '{}' -> Python decoder '{}'".format(
                args.decoder, py_decoder))

        success = run_single(py_decoder, args.testdata, args.output)
        sys.exit(0 if success else 1)


if __name__ == '__main__':
    main()
