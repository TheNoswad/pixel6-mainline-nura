#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copy this repository's packages over a pmaports checkout.
#
#   scripts/apply.sh [--any-commit] <pmaports-checkout>
#
# The packages are tested against the pmaports commit in UPSTREAM. Seven
# upstream files are replaced outright (device-google-oriole,
# linux-postmarketos-gs101, bootmac, postmarketos-base-ui and libcamera
# APKBUILDs etc.), so on any other commit upstream changes to those
# packages are silently overwritten: the script refuses unless
# --any-commit is given. Afterwards the checkout has uncommitted changes;
# point pmbootstrap's "aports" at it.
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
any=
[ "$1" = --any-commit ] && { any=1; shift; }
[ $# -eq 1 ] || { sed -n '3,12s/^# \{0,1\}//p' "$0"; exit 1; }
dst=$1
[ -f "$dst/pmaports.cfg" ] || { echo "$dst is not a pmaports checkout" >&2; exit 1; }

want=$(cat "$here/UPSTREAM")
have=$(git -C "$dst" rev-parse HEAD)
if [ "$have" != "$want" ] && [ -z "$any" ]; then
	echo "$dst is at $have, these packages are tested on $want." >&2
	echo "Run: git -C $dst checkout $want   (or pass --any-commit)" >&2
	exit 1
fi

for d in device main temp; do
	cp -R "$here/$d/." "$dst/$d/"
done
echo "Applied onto $dst ($have)."
git -C "$dst" status --short | awk '{print $1}' | sort | uniq -c
