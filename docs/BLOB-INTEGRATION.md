# Running the Conexant blobs in userspace

This document records what it takes to link the closed Conexant HSF objects
(`hsfengine-*.O`, `hsfhda-*.O`) into an ordinary Linux process. A Phase 0
feasibility spike established each point on both x86_64 and i386; the
results now live in `daemon/blob/` and are checked by `tests/contract/`. The blobs are
treated as opaque: only ELF metadata (symbols, section headers, relocations)
and our own tracing of calls into the `Os*` layer were used.

## Result of the feasibility spike

| Check                                                         | x86_64                     | i386                        |
| ------------------------------------------------------------- | -------------------------- | --------------------------- |
| Blob preparation verified (symbols and relocations unchanged) | 34,065 + 4,888 relocations | 25,361 + 4,421 relocations  |
| Non-PIE executable links without warnings                     | yes                        | yes (Debian i386 container) |
| `HsfEngineInit`/`HsfEngineExit` 100 times                     | no leaks, no faults        | no leaks, no faults         |
| `ComCtrl_Create`/`Configure`/`Open` reaches the HDA HAL       | yes, 162 codec verbs       | yes, 162 codec verbs        |
| Teardown after the (expected) open failure with a fake codec  | clean, no leaks            | clean, no leaks             |

`ComCtrl_Open` fails with status 2049 against the fake codec, because the HAL
polls vendor registers that only real hardware answers. Getting further needs
recorded responses from a ThinkPad (Phase 0b).

## Required adaptations

### 1. Section renames (`daemon/blob/blobprep.py`)

- **`.gnu.linkonce.*` sections.** The engine and HDA blobs reuse 12 obfuscated
  linkonce names (`.gnu.linkonce.t.10`, ...). ld keeps only one section per
  linkonce name across all inputs, which would silently replace one blob's code
  with the other's. Each linkonce section is renamed by its flags to
  `.text|.data|.rodata.<blob>.lo.<suffix>`.
- **`.ctors`.** `HsfEngineInit` walks this table itself; it has relocations
  against `.ctors+0` and `+8`. The first word is the literal value 1 and is not
  a constructor. Under its original name, ld would turn the table into
  `.init_array` entries and glibc would call address 0x1, so it is renamed to
  `.hsf.<blob>.ctors`.
- **Executable-stack note.** The i386 engine marks `.note.GNU-stack` executable.
  The flag is cleared, and the link also uses `-z noexecstack`.

`blobprep.py` re-reads the result and fails unless every allocated section
(name, size, flags), every symbol and every relocation (offset, type, target,
addend) matches the original apart from the renames.
`objcopy --update-section` is not usable here, because it silently drops the
updated section's relocations.

### 2. The constructor table needs a NULL terminator

The engine walks its constructor table until it reads a NULL entry. In the
kernel module, and by luck in the x86_64 userspace link, alignment padding
after the table supplied that zero. In the i386 link the HDA blob's data
followed immediately, and the engine called into it. `daemon/blob/hsf-ctors.ld` places the
renamed table in its own output section followed by an explicit
`QUAD(0)`:

```ld
SECTIONS { .hsf.ctors : ALIGN(8) { KEEP(*(.hsf.hsfengine.ctors)) QUAD(0) } }
INSERT AFTER .data;
```

### 3. Non-PIE link

The x86_64 blobs were built with `-mcmodel=kernel`, so they use
`R_X86_64_32S` absolute relocations, which need every symbol below 2 GiB.
GCC on this system defaults to PIE, and meson's `b_pie=false` does not add
`-no-pie`, so the link passes `-no-pie` explicitly. The build asserts that the
ELF type is `EXEC`. i386 needs the same, because of `R_386_32` relocations in
code.

### 4. Calling convention on i386

The blobs were built without `-mregparm`. The blob headers mark everything the
blob calls with `__shimcall__` (`regparm(0)`), which is the default convention in
userspace. GCC still treats an annotated function type as distinct from an
unannotated one, so every `Os*` definition and every callback stored in an
annotated pointer must carry `__shimcall__`. `OsKernelUsesRegParm` returns
FALSE, so the blobs pass plain cdecl callbacks. i386 code is built with
`-mstackrealign`, since blob callers keep only 4-byte stack alignment.

### 5. Physical memory: the BIOS window

`OS_DEVNODE.osPageOffset` was `PAGE_OFFSET` in the kernel. The engine reads
physical memory as `osPageOffset + phys`. During `ComCtrl_Open` it compares
the BIOS area 0xE0000–0xFFFFF in 16-byte steps against a 5-byte signature:
8,192 `OsMemCmp` calls per open, consistent with a `_DMI_` entry-point scan.

With `osPageOffset = 0`, the daemon maps a read-only window at the identity
address 0xE0000 (allowed with the default `vm.mmap_min_addr` of 65536):

- **Hardware.** Fill it from `/dev/mem` (root, kernel lockdown off), or
  synthesise an entry point and table from `/sys/firmware/dmi/tables/`. If the
  engine follows the entry point to the DMI structure table, that table must be
  mapped at its physical address too.
- **Tests.** Zeros, or a synthetic table, so results stay deterministic and
  contain no machine serial numbers.

**This matters for licensing.** The engine computes `HARDWARE_ID`, which a
Linuxant key is bound to, without any key present. With a zero BIOS window and
the fake codec it writes `21CC3BB2` on both architectures. On a ThinkPad the
real DMI data will change it. For an existing key to stay valid, the window must
show the same data the kernel driver saw.

### 6. Execution-context model

The legacy kernel glue refused to sleep in atomic context. The daemon tracks a
per-thread atomic depth, raised inside critical sections and in one-shot timer
callbacks (the kernel's softirq context), and reports any violation. None
occurred in the spike runs.

## Observed blob behaviour

- **NVM written by the engine:** `HARDWARE_ID`, `LICENSE_STATUS`
  (`"FREE (max 14.4kbps data only)"` without a key), `COUNTRY_CODE` (default
  `00B5`, USA) and `PREVIOUS_COUNTRY_CODE` (`FFFF`). It reads 213 parameters per
  open.
- **Threads:**
  - `modem`: the shared periodic-timer worker; first timer 1000 ms.
  - `hdacaesarif`: the HDA HAL thread, created with high priority.
- **HDA path:**
  - `OsHdaCodecOpenDMA(4096)` per direction, i.e. 2,048 samples = 128 ms at
    16 kHz.
  - The blob programs converter nodes 0x71/0x72 itself: `SET_STREAM_FORMAT`
    0x0210 (16 kHz, 16-bit, mono) and `SET_CHANNEL_STREAMID` from the tags
    returned by `OsHdaCodecDMAInfo`.
  - It enables unsolicited responses on node 0x02 with the tag from
    `OsHdaCodecSetEventCallback`, and drives vendor node 0x70 through verbs
    0x6xx/0x9xx/0xExx.
- **Other calls:** two `OsMemDMAAllocate(order 1)` buffers even on HDA (internal
  use; no device DMA). No `OsPciConfig*` calls on the open path.
- **Call inventory:** 51 distinct `Os*` functions in one init/open/close/exit
  cycle (`hsf_inventory_dump()`).
