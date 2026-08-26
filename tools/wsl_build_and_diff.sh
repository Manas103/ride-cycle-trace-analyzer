#!/usr/bin/env bash
set -euo pipefail
SRC="/mnt/c/Users/Manas/Downloads/Total Job Application process flow/projects/ride-cycle-trace-analyzer"
BUILD_HOME="/tmp/ride-cycle-trace-analyzer-src"

rm -rf "$BUILD_HOME"
mkdir -p "$BUILD_HOME"
cp -r "$SRC/CMakeLists.txt" "$SRC/include" "$SRC/src" "$SRC/tests" "$BUILD_HOME/"
cd "$BUILD_HOME"

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release > /tmp/rcta_cmake_out.txt 2>&1
cmake --build build -j4 >> /tmp/rcta_cmake_out.txt 2>&1

echo "--- raw CLI output on fault_00 ---"
./build/trace_parser_cli "$SRC/data/sessions/fault_00.csv" /tmp/v.csv
cat /tmp/v.csv

echo "--- diff harness ---"
python3 "$SRC/tools/diff_harness.py" ./build/trace_parser_cli | tee /tmp/rcta_diff_output.txt
cp /tmp/rcta_diff_output.txt "$SRC/docs/diff_harness_output.txt"
echo ALL_DONE
