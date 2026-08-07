#!/bin/bash
# copy-deps.sh — Copy MinGW DLL dependencies for a given binary
#
# Usage: copy-deps.sh <binary> <mingw_prefix>
#   e.g.: copy-deps.sh PXView.exe /mingw64
#
# Uses ldd to resolve dependencies, then copies those that reside
# under the specified MinGW prefix to the current directory.

BINARY="$1"
MINGW_PREFIX="${2:-/mingw64}"

if [ -z "$BINARY" ]; then
    echo "ERROR: copy-deps.sh: no binary specified"
    echo "Usage: copy-deps.sh <binary> [mingw_prefix]"
    exit 1
fi

if [ ! -f "$BINARY" ]; then
    echo "ERROR: copy-deps.sh: binary not found: $BINARY"
    exit 1
fi

# Extract the list of dependencies for a given binary using ldd
deps=$(ldd "$BINARY" 2>/dev/null)

if [ -z "$deps" ]; then
    echo "WARNING: copy-deps.sh: ldd returned no dependencies for $BINARY"
    exit 0
fi

# Filter the dependencies to include only those with the prefix /mingw64
deps_to_copy=$(echo "$deps" | grep "$MINGW_PREFIX" | awk '{print $3}')

# Copy each dependency to the current directory
count=0
for dep in $deps_to_copy
do
    if [ -f "$dep" ]; then
        cp "$dep" .
        count=$((count + 1))
    else
        echo "WARNING: copy-deps.sh: dependency not found: $dep"
    fi
done

echo "copy-deps: $count DLLs copied for $BINARY (from $MINGW_PREFIX)"
