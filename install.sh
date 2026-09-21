#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PREFIX=${PREFIX:-/usr/local}
DESTDIR=${DESTDIR:-}
MODE=auto
BUILD_DIR=

usage() {
    cat <<'EOF'
Usage: ./install.sh [OPTIONS]

Install fsx and its manual page.

Options:
  --prefix DIR         installation prefix (default: /usr/local)
  --static             install the bundled static Linux x86-64 binary
  --dynamic            install the bundled dynamic Linux x86-64 binary
  --build-from-source  build a native release binary before installing
  -h, --help           show this help

The static binary is used automatically on Linux x86-64. On other systems,
the installer builds from source. Set DESTDIR to stage an installation for
packaging, and PREFIX to set the default prefix without an option.
EOF
}

die() {
    echo "install.sh: $*" >&2
    exit 1
}

cleanup() {
    if [ -n "$BUILD_DIR" ] && [ -d "$BUILD_DIR" ]; then
        rm -rf -- "$BUILD_DIR"
    fi
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

while [ "$#" -gt 0 ]; do
    case $1 in
        --prefix)
            [ "$#" -ge 2 ] || die "--prefix requires a directory"
            PREFIX=$2
            shift 2
            ;;
        --static)
            MODE=static
            shift
            ;;
        --dynamic)
            MODE=dynamic
            shift
            ;;
        --build-from-source)
            MODE=source
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "unknown option: $1 (try --help)"
            ;;
    esac
done

case $PREFIX in
    /*) ;;
    *) die "installation prefix must be an absolute path: $PREFIX" ;;
esac
if [ -n "$DESTDIR" ]; then
    case $DESTDIR in
        /*) ;;
        *) die "DESTDIR must be an absolute path: $DESTDIR" ;;
    esac
fi

command -v install >/dev/null 2>&1 || die "the POSIX install utility is required"

os=$(uname -s)
arch=$(uname -m)
if [ "$MODE" = auto ]; then
    case "$os:$arch" in
        Linux:x86_64|Linux:amd64) MODE=static ;;
        *) MODE=source ;;
    esac
fi

verify_binary() {
    binary_name=$1
    sums=$ROOT/bin/SHA256SUMS
    [ -f "$sums" ] || die "checksum file is missing: $sums"
    expected=$(awk -v name="$binary_name" '$2 == name { print $1 }' "$sums")
    [ -n "$expected" ] || die "no checksum found for $binary_name"

    if command -v sha256sum >/dev/null 2>&1; then
        actual=$(sha256sum "$ROOT/bin/$binary_name" | awk '{ print $1 }')
    elif command -v shasum >/dev/null 2>&1; then
        actual=$(shasum -a 256 "$ROOT/bin/$binary_name" | awk '{ print $1 }')
    else
        die "sha256sum or shasum is required to verify the bundled binary"
    fi
    [ "$actual" = "$expected" ] || die "checksum verification failed for $binary_name"
}

case $MODE in
    static|dynamic)
        case "$os:$arch" in
            Linux:x86_64|Linux:amd64) ;;
            *) die "bundled binaries require Linux x86-64; use --build-from-source" ;;
        esac
        if [ "$MODE" = static ]; then
            binary_name=fsx-linux-x86_64-static
        else
            binary_name=fsx-linux-x86_64
        fi
        [ -f "$ROOT/bin/$binary_name" ] || die "bundled binary is missing: bin/$binary_name"
        verify_binary "$binary_name"
        binary=$ROOT/bin/$binary_name
        ;;
    source)
        command -v cmake >/dev/null 2>&1 || die "CMake is required for a source build"
        BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/fsx-install.XXXXXX")
        cmake -S "$ROOT" -B "$BUILD_DIR" \
            -DCMAKE_BUILD_TYPE=Release \
            -DFSX_BUILD_TESTS=OFF
        jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)
        case $jobs in
            ''|*[!0-9]*) jobs=1 ;;
        esac
        cmake --build "$BUILD_DIR" --parallel "$jobs"
        binary=$BUILD_DIR/fsx
        [ -x "$binary" ] || die "source build did not produce an fsx binary"
        ;;
esac

target_bin=${DESTDIR}${PREFIX}/bin
target_man=${DESTDIR}${PREFIX}/share/man/man8
permission_root=${DESTDIR}${PREFIX}
probe=$permission_root
while [ ! -e "$probe" ]; do
    parent=$(dirname -- "$probe")
    [ "$parent" != "$probe" ] || break
    probe=$parent
done

SUDO=
if [ ! -w "$probe" ]; then
    [ "$(id -u)" -ne 0 ] || die "cannot write to $permission_root"
    command -v sudo >/dev/null 2>&1 || \
        die "$permission_root is not writable and sudo is unavailable"
    SUDO=sudo
    echo "Installing to $PREFIX with administrator privileges..."
fi

$SUDO install -d "$target_bin" "$target_man"
$SUDO install -m 0755 "$binary" "$target_bin/fsx"
$SUDO install -m 0644 "$ROOT/docs/fsx.8" "$target_man/fsx.8"

echo "Installed fsx to $target_bin/fsx"
echo "Installed manual page to $target_man/fsx.8"
