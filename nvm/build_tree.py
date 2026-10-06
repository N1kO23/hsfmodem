#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build the static NVM parameter tree used by the HDA modem profile.

    build_tree.py --cvtinf cvtinf.pl --inf-dir DIR --out DIR --version V [--stamp FILE]

Reproduces what the legacy nvm/makefile did for the "hsfhda" profile:
  hsfpcibasic2       from hsfpcibasic2.inf + hsf.cty (owns Region/ and Profile/)
  hsfpcibasic2smart  from hsfpcibasic2smart.inf, Region/Profile -> hsfpcibasic2
  hsfhda             from hsfhda.inf, Region/Profile -> hsfpcibasic2smart
Each profile also gets COUNTRY_CODE_LIST: the region codes whose _NAME file
holds no '*', without leading zeros, comma-separated and quoted.
The .inf files are converted by the unchanged Linuxant cvtinf.pl.
"""
import argparse
import os
import shutil
import subprocess
import sys

PROFILES = [
    # name, inf, extra input, link Region/Profile to
    ("hsfpcibasic2", "hsfpcibasic2.inf", "hsf.cty", None),
    ("hsfpcibasic2smart", "hsfpcibasic2smart.inf", None, "hsfpcibasic2"),
    ("hsfhda", "hsfhda.inf", None, "hsfpcibasic2smart"),
]


def substitute(text, version):
    return (text.replace("@CNXTLINUXVERSION@", version)
                .replace("@CNXTTARGET@", "hsf")
                .replace("@CNXTDRVDSC@", "Conexant HSF softmodem"))


def convert(cvtinf, source, outdir, version):
    with open(source, encoding="latin-1") as f:
        data = substitute(f.read(), version)
    env = dict(os.environ, LC_ALL="C")
    subprocess.run(["perl", cvtinf, outdir], input=data.encode("latin-1"), check=True, env=env)


def country_code_list(profile_dir):
    region = os.path.join(profile_dir, "Region")
    codes = []
    for name in sorted(os.listdir(region)):
        if not name.endswith("_NAME"):
            continue
        with open(os.path.join(region, name), encoding="latin-1") as f:
            if "*" in f.read():
                continue
        code = name[:-len("_NAME")]
        for _ in range(2):	# sed 's/^0//' twice
            if code.startswith("0"):
                code = code[1:]
        codes.append(code)
    with open(os.path.join(profile_dir, "COUNTRY_CODE_LIST"), "w") as f:
        f.write('"' + ",".join(codes) + '"\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cvtinf", required=True)
    ap.add_argument("--inf-dir", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--version", required=True, help="e.g. 7.80.02.05x86_64full")
    ap.add_argument("--stamp")
    args = ap.parse_args()

    if os.path.exists(args.out):
        shutil.rmtree(args.out)
    os.makedirs(args.out)
    for name, inf, extra, link_to in PROFILES:
        target = os.path.join(args.out, name)
        convert(args.cvtinf, os.path.join(args.inf_dir, inf), target, args.version)
        if extra:
            convert(args.cvtinf, os.path.join(args.inf_dir, extra), target, args.version)
        if link_to:
            for sub in ("Profile", "Region"):
                if os.path.isdir(os.path.join(args.out, link_to, sub)):
                    os.symlink(os.path.join("..", link_to, sub), os.path.join(target, sub))
        country_code_list(target)
    if args.stamp:
        with open(args.stamp, "w") as f:
            f.write(args.out + "\n")


if __name__ == "__main__":
    sys.exit(main())
