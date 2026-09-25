#!/usr/bin/env sh
#
# Runs the unit tests natively; the store and registry need only glib.
#
#   sh tests/run.sh

set -eu

cd "$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"

CC=${CC:-cc}

# Homebrew often lacks pkg-config but has the .pc file, so fall back to its prefix.
if command -v pkg-config >/dev/null 2>&1; then
	GLIB_CFLAGS=$(pkg-config --cflags glib-2.0)
	GLIB_LIBS=$(pkg-config --libs glib-2.0)
elif command -v brew >/dev/null 2>&1 && [ -d "$(brew --prefix glib)" ]; then
	GLIB=$(brew --prefix glib)
	GLIB_CFLAGS="-I$GLIB/include/glib-2.0 -I$GLIB/lib/glib-2.0/include"
	GLIB_LIBS="-L$GLIB/lib -lglib-2.0"
else
	echo "glib development files not found; install pkg-config and glib" >&2
	exit 1
fi

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

# shellcheck disable=SC2086
$CC -Wall -Wextra -Werror -g -O1 $GLIB_CFLAGS \
	-o "$OUT/test_store" \
	tests/test_store.c app/store.c app/metrics.c \
	$GLIB_LIBS -lm

"$OUT/test_store"
