#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Install the generated static NVM tree, keeping its relative symlinks."""

import os
import shutil
import sys


def main():
    src, dest = sys.argv[1], sys.argv[2]
    destdir = os.environ.get("DESTDIR", "")
    if destdir:
        dest = os.path.join(destdir, os.path.relpath(dest, "/"))
    if os.path.lexists(dest):
        shutil.rmtree(dest)
    shutil.copytree(src, dest, symlinks=True)
    for root, dirs, files in os.walk(dest):
        for d in dirs:
            os.chmod(os.path.join(root, d), 0o755)
        for f in files:
            path = os.path.join(root, f)
            if not os.path.islink(path):
                os.chmod(path, 0o644)
    print(f"Installed the static NVM tree to {dest}")


if __name__ == "__main__":
    main()
