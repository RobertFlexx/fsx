#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
WORK=${FSX_RELEASE_WORK:-${TMPDIR:-/tmp}/fsx-release-gate.$$}
KEEP=${FSX_KEEP_RELEASE_WORK:-0}

cleanup() {
    if [ "$KEEP" = 0 ]; then rm -rf "$WORK"; else echo "release work kept at: $WORK"; fi
}
trap cleanup EXIT HUP INT TERM
mkdir -p "$WORK"

run_gate() {
    name=$1
    compiler=$2
    build_type=$3
    shift 3
    b="$WORK/$name"
    echo "== $name: configure =="
    cmake -S "$ROOT" -B "$b" -G Ninja \
        -DCMAKE_BUILD_TYPE="$build_type" \
        -DCMAKE_CXX_COMPILER="$compiler" "$@"
    echo "== $name: build =="
    cmake --build "$b" -j"$JOBS"
    echo "== $name: tests =="
    case "$name" in
        clang-sanitize) ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ctest --test-dir "$b" --output-on-failure ;;
        gcc-tsan) TSAN_OPTIONS=halt_on_error=1 ctest --test-dir "$b" --output-on-failure ;;
        *) ctest --test-dir "$b" --output-on-failure ;;
    esac
    echo "== $name: integration =="
    case "$name" in
        clang-sanitize) (cd "$ROOT" && ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 FSX="$b/fsx" ./scripts/integration.sh >/dev/null) ;;
        gcc-tsan) (cd "$ROOT" && TSAN_OPTIONS=halt_on_error=1 FSX="$b/fsx" ./scripts/integration.sh >/dev/null) ;;
        *) (cd "$ROOT" && FSX="$b/fsx" ./scripts/integration.sh >/dev/null) ;;
    esac
}

echo "== source style =="
"$ROOT/scripts/check-style.py"

command -v g++ >/dev/null 2>&1 || { echo "g++ not found" >&2; exit 1; }
command -v clang++ >/dev/null 2>&1 || { echo "clang++ not found" >&2; exit 1; }
command -v ninja >/dev/null 2>&1 || { echo "ninja not found" >&2; exit 1; }

run_gate gcc-release g++ Release -DFSX_WARNINGS_AS_ERRORS=ON
run_gate clang-release clang++ Release -DFSX_WARNINGS_AS_ERRORS=ON
run_gate clang-sanitize clang++ RelWithDebInfo -DFSX_ENABLE_ASAN=ON -DFSX_ENABLE_LTO=OFF
if [ "${FSX_SKIP_TSAN:-0}" != 1 ]; then
    run_gate gcc-tsan g++ RelWithDebInfo -DFSX_ENABLE_LTO=OFF \
        -DCMAKE_CXX_FLAGS=-fsanitize=thread\ -fno-omit-frame-pointer \
        -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread
fi

b="$WORK/gcc-assertions"
echo "== gcc-assertions: configure/build/test =="
cmake -S "$ROOT" -B "$b" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER=g++ -DCMAKE_CXX_FLAGS=-D_GLIBCXX_ASSERTIONS \
    -DFSX_WARNINGS_AS_ERRORS=ON
cmake --build "$b" -j"$JOBS"
ctest --test-dir "$b" --output-on-failure
(cd "$ROOT" && FSX="$b/fsx" ./scripts/integration.sh >/dev/null)

if [ "${FSX_SKIP_STATIC:-0}" != 1 ]; then
    run_gate gcc-static g++ Release -DCMAKE_EXE_LINKER_FLAGS=-static -DFSX_WARNINGS_AS_ERRORS=ON
    if command -v ldd >/dev/null 2>&1 && ldd "$WORK/gcc-static/fsx" 2>&1 | grep -qv 'not a dynamic executable'; then
        echo "static gate produced a dynamic executable" >&2
        exit 1
    fi
fi

echo "release gate: PASS"
