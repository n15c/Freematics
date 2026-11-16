#!/bin/sh
# Host unit tests for the hardware independent parts (protocol, parsers, math).
# Usage: sh test/run_tests.sh
set -e
cd "$(dirname "$0")/.."
mkdir -p .pio/test
c++ -std=c++17 -Wall -Wextra -Itest/stub -Isrc test/test_util.cpp src/util.cpp src/osmand.cpp -o .pio/test/test_util
.pio/test/test_util
