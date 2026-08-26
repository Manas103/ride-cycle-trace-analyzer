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
echo "--- build log tail ---"
tail -n 40 /tmp/rcta_cmake_out.txt

echo "--- test run ---"
./build/trace_tests | tee /tmp/rcta_test_output.txt

echo "--- sanitizer build ---"
cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON > /tmp/rcta_san_cmake_out.txt 2>&1
cmake --build build-san --target trace_tests_asan -j4 >> /tmp/rcta_san_cmake_out.txt 2>&1
setarch -R ./build-san/trace_tests_asan | tee /tmp/rcta_asan_output.txt

echo "--- copying outputs back ---"
cp /tmp/rcta_test_output.txt "$SRC/docs/test_output.txt"
cp /tmp/rcta_asan_output.txt "$SRC/docs/asan_ubsan_clean_run.txt"
cp "$BUILD_HOME/build/trace_parser_cli" /tmp/trace_parser_cli_linux
echo ALL_DONE
