#!/bin/sh
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
src="$here/../src"
for file in eportal-request.c eportal-request.h; do
	if [ ! -f "$src/$file" ]; then
		printf 'Missing standalone ePortal encoder: %s\n' "$file" >&2
		exit 1
	fi
done

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc -Wall -Wextra -Werror -I"$src" "$here/test_encrypt.c" \
	"$src/eportal-request.c" -lcrypto -o "$work/test-request"
LC_ALL=C "$work/test-request"
