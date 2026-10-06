# Third-party: Conexant HSF engine (Linuxant hsfmodem 7.80.02.05)

These files come unchanged from the Linuxant hsfmodem package and are
covered by [`LICENSE`](LICENSE).

| Path              | Contents                                                                                                                                                                                                                   |
| ----------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `blobs/*.O`       | Closed relocatable objects for i386 and x86_64. The HD-Audio modem uses `hsfengine` and `hsfhda`. `SHA256SUMS` records their checksums; the build refuses other files.                                                     |
| `include/`        | The ABI headers the blobs were compiled against. Also `oshda.h` and `osresour_ex.h` from the original driver's glue, which define structures shared with the blobs (`OS_DEVNODE`, `HDAOSHAL`, the `OsHdaCodec*` pointers). |
| `inf/`            | Modem and country parameters (Windows `.inf`/`.cty` format), converted to the NVM tree at build time.                                                                                                                      |
| `makeflags-*.mak` | The compiler flags and defines the blobs were built with; the meson build applies the same defines.                                                                                                                        |

Do not modify these files. Changes in behaviour belong in the OS layer
(`daemon/os/`).
