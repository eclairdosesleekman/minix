#!/bin/sh
# Run the portable MFS4 allocator/block-map regression tests on a host.
set -eu

srcdir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/mfs4-blockmap.XXXXXX")
trap 'rm -rf "$tmpdir"' 0 HUP INT TERM

${CC:-cc} -std=c99 -Wall -Wextra -Werror -pedantic -O2 \
    -I"$srcdir/../../../include" -I"$srcdir/.." \
    "$srcdir/../mfs4_blockmap.c" \
    "$srcdir/mfs4_blockmap_test.c" \
    -o "$tmpdir/mfs4_blockmap_test"
"$tmpdir/mfs4_blockmap_test"
