#!/usr/bin/env bash
# Verify that the per-tool copies of bspfile.h / cmdlib.h / mathlib.h are
# byte-identical between vendor/qbsp, vendor/light, vendor/vis. The build
# only compiles the qbsp .c versions and forces light/vis TUs to link
# against them via -include qbsp_namespace.h. If a contributor patches
# vendor/light/bspfile.h thinking it's the only copy, light's TU sees
# one struct layout and the qbsp .c it links against sees another --
# silent ABI mismatch with no compiler diagnostic.
#
# Run from anywhere; resolves paths relative to the repo root.

set -euo pipefail

cd "$(dirname "$0")/.."

fail=0
for h in bspfile.h cmdlib.h mathlib.h; do
    q="sdlquake/vendor/qbsp/$h"
    l="sdlquake/vendor/light/$h"
    v="sdlquake/vendor/vis/$h"
    if ! cmp -s "$q" "$l"; then
        echo "ERROR: $l drifted from $q -- vendored headers must stay in sync" >&2
        diff -u "$q" "$l" | head -40 >&2
        fail=1
    fi
    if ! cmp -s "$q" "$v"; then
        echo "ERROR: $v drifted from $q -- vendored headers must stay in sync" >&2
        diff -u "$q" "$v" | head -40 >&2
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    echo >&2
    echo "Fix: edit only sdlquake/vendor/qbsp/<header>, then copy the file" >&2
    echo "into vendor/light/ and vendor/vis/ so all three remain identical." >&2
    exit 1
fi

exit 0
