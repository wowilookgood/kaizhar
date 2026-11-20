#!/bin/sh
# Host check of the shared ETC1 decode: see test.cpp. Run from anywhere.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
out=${TMPDIR:-/tmp}/hen-etc1-test
c++ -std=c++23 -O2 -Wall -pthread -I "$root/src" -I "$root/externals/boost" \
    -I "$root/externals/fmt/include" -DFMT_HEADER_ONLY \
    "$here/test.cpp" -o "$out"
"$out"
