#!/bin/sh
# usage: build.sh <AnyPS5 checkout> <PS4 11.00 libSceFont.sprx> [output directory]
set -eu
[ $# -ge 2 ] || { echo "usage: $0 <AnyPS5 checkout> <PS4 11.00 libSceFont.sprx> [output directory]" >&2; exit 2; }
repo=$1
module=$2
here=$(cd "$(dirname "$0")" && pwd)
out=${3:-$here/out}
mkdir -p "$out"
source="$repo/core/libs/prx/libSceFont/src/WritingLine.cpp"
test="$repo/core/libs/tests/GuestFontWritingLine.cpp"
flags="-std=c++20 -O1 -g -I$repo/core/libs"

g++ $flags -o "$out/diff" "$here/diff.cpp" "$source"
g++ $flags -o "$out/diff2" "$here/diff2.cpp" "$source"

grep -q '^#define Require(value) Check((value), __LINE__)$' "$test"
sed 's/^#define Require(value) Check((value), __LINE__)$/#define Require(value) (TraceLine(__LINE__), Check((value), __LINE__))/' "$test" > "$out/test-traced.cpp"
g++ $flags -I"$here" -include "$here/trace.hpp" -o "$out/vectors" "$out/test-traced.cpp" "$here/vectors.cpp"

echo "== the repository test run against the PS4 module (vectors.txt)"
PS4_LIBSCEFONT="$module" "$out/vectors" > "$out/vectors.txt"
tail -1 "$out/vectors.txt"
echo "== differential runs: repository implementation against the PS4 module"
"$out/diff" "$module" 20000
"$out/diff2" "$module" 50000 127 0 1 80 1 1
"$out/diff2" "$module" 100000 127 1 1 80 2 1
"$out/diff2" "$module" 20000 127 2 1 80 3 0
