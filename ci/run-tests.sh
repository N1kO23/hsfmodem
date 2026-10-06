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
# meson resolves a relative DESTDIR from inside the build directory
stage=$(cd "$builddir" && pwd)/stage
DESTDIR="$stage" meson install -C "$builddir" --no-rebuild --quiet
test -x "$stage/usr/local/sbin/hsfmodemd"
test -f "$stage/usr/local/share/hsfmodem/nvm/hsfhda/Region/003C_NAME"
