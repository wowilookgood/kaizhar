#!/bin/sh
# Builds and runs the host-side test with the Mac's own compiler. Nothing of the emulator's
# build folders is used; the program goes to a temporary folder.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
clang++ -std=c++20 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined \
    "$here/idbe_test.cpp" "$here/../idbe_passthrough.cpp" -o "$out/idbe_test"
"$out/idbe_test"
