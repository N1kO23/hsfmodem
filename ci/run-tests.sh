#!/bin/sh
# SPDX-License-Identifier: MIT
# Configure, build, test and stage an install in a fresh build directory.
#   ci/run-tests.sh BUILDDIR [meson setup options...]
set -eu
builddir=$1
shift
rm -rf "$builddir"
meson setup "$builddir" -Dopenrc=true -Dsystemd=true "$@"
meson compile -C "$builddir"
meson test -C "$builddir" --print-errorlogs
DESTDIR="$builddir/stage" meson install -C "$builddir" --no-rebuild --quiet
test -x "$builddir/stage/usr/local/sbin/hsfmodemd"
test -f "$builddir/stage/usr/local/share/hsfmodem/nvm/hsfhda/Region/003C_NAME"
