#!/bin/bash

# Extract the list of dependencies for a given binary using ldd
deps=$(ldd $1)

# Filter the dependencies to include only those with the prefix /mingw64
deps_to_copy=$(echo "$deps" | grep "$2" | awk '{print $3}')

# Copy each dependency to the current directory
for dep in $deps_to_copy
do
    cp $dep .
done