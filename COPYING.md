# Licensing

This repository combines code under different licenses.

| Path                                                                        | License                                                                                                |
| --------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------ |
| `daemon/`, `tests/`, `nvm/build_tree.py`, `ci/`, build files, `docs/`       | MIT ([`LICENSES/MIT.txt`](LICENSES/MIT.txt))                                                           |
| `kernel/` (HD-Audio codec driver, when added)                               | GPL-2.0 ([`LICENSES/GPL-2.0.txt`](LICENSES/GPL-2.0.txt))                                               |
| `include/uapi/` (kernel/userspace interface, when added)                    | GPL-2.0 WITH Linux-syscall-note ([`LICENSES/Linux-syscall-note.txt`](LICENSES/Linux-syscall-note.txt)) |
| `third_party/linuxant/` (Conexant blobs, their headers, `.inf`/`.cty` data) | Linuxant/Conexant license ([`third_party/linuxant/LICENSE`](third_party/linuxant/LICENSE))             |
| `nvm/cvtinf.pl`                                                             | Linuxant/Conexant license (used unchanged)                                                             |
| `legacy/`                                                                   | as marked in each file: the Linuxant license, or GPL for `legacy/modules/GPL/`                         |

Things to keep in mind:

- **The userspace daemon links the proprietary blobs.** It therefore contains
  no GPL code; its own code is MIT, and it talks to the kernel only through
  system calls.
- **The blobs may not be reverse engineered.** This project uses only their
  symbol tables, section headers and relocations, plus tracing of calls into
  its own OS layer.
- **Redistribution of the Linuxant software is restricted.** The license allows
  redistributing only unmodified releases marked "free". Binaries built here
  link prepared (renamed-section) copies of the blobs, so do not publish them,
  and CI uploads no artifacts.
