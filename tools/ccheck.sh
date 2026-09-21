#!/usr/bin/env bash
# Compile every translation unit and report failures. Used during development
# and by `make check-compile`; the real build is in the Makefile.
set -u
cd "$(dirname "$0")/.."
mkdir -p build-native/obj
fail=0
for f in src/engine/*.cpp src/detectors/*.cpp src/validation/*.cpp src/api/*.cpp tests/*.cpp; do
    [ -e "$f" ] || continue
    out="build-native/obj/$(basename "$f" .cpp).o"
    if ! g++ -std=c++20 -O1 -Wall -Wextra -Wno-unused-parameter -Iinclude -Isrc -c "$f" -o "$out"; then
        echo "FAILED $f"
        fail=1
    fi
done
exit $fail
