#!/bin/bash

# Extract the list of dependencies for a given binary using ldd
deps=$(ldd "$1" 2>/dev/null)
if [ -z "$deps" ]; then
    echo "WARNING: ldd produced no output for $1 — DLL dependencies may be incomplete"
    exit 0
fi

# Filter the dependencies to include only those with the prefix /mingw64
deps_to_copy=$(echo "$deps" | grep "$2" | awk '{print $3}')

# Copy each dependency to the current directory
copied=0
failed=0
for dep in $deps_to_copy
do
    if [ -f "$dep" ]; then
        cp "$dep" .
        copied=$((copied + 1))
    else
        echo "WARNING: Dependency not found: $dep"
        failed=$((failed + 1))
    fi
done
echo "  copy-deps: $copied DLLs copied, $failed not found (target: $1)"
