#!/bin/sh
# SPDX-License-Identifier: MIT
# Build dependencies on Debian/Ubuntu (also used for the i386 container).
set -eu
export DEBIAN_FRONTEND=noninteractive
apt-get -qq update
apt-get -qq install -y --no-install-recommends \
	gcc libc6-dev binutils make meson ninja-build python3 perl ca-certificates >/dev/null
