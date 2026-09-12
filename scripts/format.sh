#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
command -v clang-format >/dev/null 2>&1 || {
    echo "clang-format is required to reformat FSX source" >&2
    exit 1
}
find "$ROOT/src" "$ROOT/include" "$ROOT/tests" -type f \
    \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.c' \) \
    -print0 | xargs -0 clang-format -i --style=file
"$ROOT/scripts/check-style.py"
