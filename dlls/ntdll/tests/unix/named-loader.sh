#!/bin/sh
# Native macOS regression test, independent of the PE test runner and desktop.
# Usage: sh path/to/named-loader.sh /path/to/configured/wine/build
set -eu
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/wine-named-test.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/wine.app/Contents/MacOS"
cc -Wall -Wextra -Werror -I"${1:?Wine build directory required}/include" \
    -I"$here/../../unix" "$here/named-loader.c" "$here/../../unix/named_loader.c" \
    -o "$tmp/wine.app/Contents/MacOS/wine"
"$tmp/wine.app/Contents/MacOS/wine" "$tmp"
