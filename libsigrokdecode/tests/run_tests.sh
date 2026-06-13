#!/bin/bash
# run_tests.sh - Batch test runner for C decoder tests
#
# Usage:
#   run_tests.sh [decoder_name] [--tolerance N] [--testdata-dir <path>]
#
# Exit code: 0 if all tests pass, 1 if any fail

set -euo pipefail

# ---- Defaults ----
TOLERANCE=0
TESTDATA_DIR="./testdata"
FILTER_DECODER=""

# ---- Parse arguments ----
while [[ $# -gt 0 ]]; do
    case "$1" in
        --tolerance)
            TOLERANCE="$2"
            shift 2
            ;;
        --testdata-dir)
            TESTDATA_DIR="$2"
            shift 2
            ;;
        -*)
            echo "Unknown option: $1" >&2
            exit 2
            ;;
        *)
            if [[ -z "$FILTER_DECODER" ]]; then
                FILTER_DECODER="$1"
            else
                echo "Unexpected argument: $1" >&2
                exit 2
            fi
            shift
            ;;
    esac
done

# ---- Find decoder_test executable ----
find_decoder_test() {
    local exe_name="decoder_test"
    if [[ -f "./$exe_name" ]] && [[ -x "./$exe_name" ]]; then
        echo "./$exe_name"
        return 0
    fi
    if [[ -f "../build.dir/bin/$exe_name" ]]; then
        echo "../build.dir/bin/$exe_name"
        return 0
    fi
    if [[ -f "../build.dir/$exe_name" ]]; then
        echo "../build.dir/$exe_name"
        return 0
    fi
    if [[ -f "../build/bin/$exe_name" ]]; then
        echo "../build/bin/$exe_name"
        return 0
    fi
    # Check PATH
    if command -v "$exe_name" >/dev/null 2>&1; then
        echo "$exe_name"
        return 0
    fi
    return 1
}

DECODER_TEST=$(find_decoder_test)
if [[ -z "$DECODER_TEST" ]]; then
    echo "ERROR: decoder_test executable not found." >&2
    echo "Searched: ./, ../build.dir/bin/, ../build.dir/, ../build/bin/, PATH" >&2
    exit 2
fi

echo "Using decoder_test: $DECODER_TEST"
echo "Testdata directory: $TESTDATA_DIR"
echo "Tolerance: $TOLERANCE"
echo ""

# ---- Counters ----
PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0
ERROR_COUNT=0

FAILED_TESTS=()
SKIPPED_DECODERS=()

# ---- Build tolerance arg ----
TOLERANCE_ARG=""
if [[ "$TOLERANCE" -ne 0 ]]; then
    TOLERANCE_ARG="--tolerance $TOLERANCE"
fi

# ---- Collect all known C decoder names from source ----
C_DECODERS_DIR="../c_decoders"
ALL_DECODERS=()
if [[ -d "$C_DECODERS_DIR" ]]; then
    for f in "$C_DECODERS_DIR"/*_c.c; do
        [[ -f "$f" ]] || continue
        base=$(basename "$f" .c)
        ALL_DECODERS+=("$base")
    done
fi

# ---- Determine which decoders to test ----
DECODERS_TO_TEST=()
if [[ -n "$FILTER_DECODER" ]]; then
    DECODERS_TO_TEST=("$FILTER_DECODER")
else
    # Use all known decoders; fall back to testdata subdirs
    if [[ ${#ALL_DECODERS[@]} -gt 0 ]]; then
        DECODERS_TO_TEST=("${ALL_DECODERS[@]}")
    elif [[ -d "$TESTDATA_DIR" ]]; then
        for d in "$TESTDATA_DIR"/*/; do
            [[ -d "$d" ]] || continue
            DECODERS_TO_TEST+=("$(basename "$d")")
        done
    fi
fi

# ---- Run tests ----
for decoder in "${DECODERS_TO_TEST[@]}"; do
    decoder_dir="$TESTDATA_DIR/$decoder"

    # Check if testdata directory exists for this decoder
    if [[ ! -d "$decoder_dir" ]]; then
        SKIP_COUNT=$((SKIP_COUNT + 1))
        SKIPPED_DECODERS+=("$decoder")
        continue
    fi

    # Find test case subdirectories
    found_case=0
    for case_dir in "$decoder_dir"/*/; do
        [[ -d "$case_dir" ]] || continue
        case_name=$(basename "$case_dir")

        # Validate: must have config.json AND input.bin
        if [[ ! -f "$case_dir/config.json" ]] || [[ ! -f "$case_dir/input.bin" ]]; then
            continue
        fi

        found_case=1
        # Run the test
        cmd="$DECODER_TEST -d $decoder -t $case_dir $TOLERANCE_ARG"
        output=$($cmd 2>&1) || true
        exit_code=$?

        if [[ $exit_code -eq 0 ]]; then
            PASS_COUNT=$((PASS_COUNT + 1))
            echo "  PASS: $decoder/$case_name"
        elif [[ $exit_code -eq 1 ]]; then
            FAIL_COUNT=$((FAIL_COUNT + 1))
            # Extract a short failure reason from output
            reason=$(echo "$output" | grep -i -E "(mismatch|fail|error|expected|got)" | head -1 | sed 's/^[[:space:]]*//')
            if [[ -n "$reason" ]]; then
                FAILED_TESTS+=("$decoder/$case_name: $reason")
            else
                FAILED_TESTS+=("$decoder/$case_name")
            fi
            echo "  FAIL: $decoder/$case_name"
        else
            ERROR_COUNT=$((ERROR_COUNT + 1))
            FAILED_TESTS+=("$decoder/$case_name: error (exit code $exit_code)")
            echo "  ERROR: $decoder/$case_name (exit code $exit_code)"
        fi
    done

    if [[ $found_case -eq 0 ]]; then
        SKIP_COUNT=$((SKIP_COUNT + 1))
        SKIPPED_DECODERS+=("$decoder")
    fi
done

TOTAL=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT + ERROR_COUNT))

# ---- Print summary ----
echo ""
echo "============================================"
echo "C Decoder Test Results"
echo "============================================"
echo "PASS:  $PASS_COUNT"
echo "FAIL:  $FAIL_COUNT"
echo "SKIP:  $SKIP_COUNT (no test data)"
echo "ERROR: $ERROR_COUNT"
echo "TOTAL: $TOTAL"

if [[ ${#FAILED_TESTS[@]} -gt 0 ]]; then
    echo ""
    echo "Failed tests:"
    for t in "${FAILED_TESTS[@]}"; do
        echo "  - $t"
    done
fi

if [[ ${#SKIPPED_DECODERS[@]} -gt 0 ]]; then
    echo ""
    echo "Skipped (need upstream decoder):"
    for d in "${SKIPPED_DECODERS[@]}"; do
        echo "  - $d"
    done
fi

echo "============================================"

# Exit code
if [[ $FAIL_COUNT -gt 0 ]] || [[ $ERROR_COUNT -gt 0 ]]; then
    exit 1
fi
exit 0
