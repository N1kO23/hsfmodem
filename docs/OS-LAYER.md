# OS layer specification

The Conexant blobs call nothing but the functions below (symbol prefix
`cnxthsf_7800205x86_64full_`, or `cnxthsf_7800205full_` on i386). Each entry
states the behaviour the blobs were built against: the semantics of the
original kernel glue, Linuxant hsfmodem 7.80.02.05, `legacy/modules/`. The
implementation is in `daemon/os/` and `daemon/nvm/`; `tests/unit/` and
`tests/contract/` check these rules.

All functions are defined with `__shimcall__` (`HSF_EXPORT`), which is
`regparm(0)` on i386 to match the blob headers.

## Execution context

The kernel distinguished process context from atomic context (interrupts
disabled, or a timer softirq). The daemon models it with a per-thread depth:

- **Raised:** while a critical section is held, and while a one-shot timer
  callback runs (the "softirq" thread).
- **Effects in atomic context:**
  - `OsEventWaitTime(timeout > 0)` returns `OSEVENT_WAIT_ERROR` immediately.
  - `OsSleep` busy-waits.
  - `OsGetCurrentThread` returns 0 inside one-shot timer callbacks
    (interrupt context).
  - Each of these is reported as a problem, except a zero-timeout poll.

## Memory

| Function                                        | Behaviour                                                                                                           |
| ----------------------------------------------- | ------------------------------------------------------------------------------------------------------------------- |
| `OsAllocate(size)`                              | Zeroed block, aligned to the next power of two between 16 bytes and one page (as kmalloc). NULL on failure.         |
| `OsFree(p)`                                     | Frees an `OsAllocate` block. NULL, foreign pointers and overruns are reported, never executed.                      |
| `OsMemDMAAllocate(order, &phys, &handle, &buf)` | `4096 << order` bytes, 64 KiB aligned. `phys = buf = handle`. Used internally by the engine even with HDA hardware. |
| `OsMemDMAFree(handle, order)`                   | Frees it; FALSE for NULL.                                                                                           |

## Strings

`OsMemSet/Cpy/Move/Cmp`, `OsStrCpy/nCpy/Cat/nCat/Cmp/nCmp/Len`,
`OsToupper/Tolower/IsDigit` behave like libc and the ASCII ctype functions.
Exceptions:

- `OsAtoi` parses **hexadecimal**: every character shifts the value left 4
  bits, and non-hex characters add 0.
- `OsSprintf`/`OsVSprintf` are unbounded `vsprintf`.

`OsMemCmp` is also how the engine scans the BIOS window (see below).

## Logging

| Function                 | Behaviour                                    |
| ------------------------ | -------------------------------------------- |
| `OsErrorPrintf`          | Warning-level log line, prefixed `engine:`.  |
| `OsDebugPrintf`          | Debug-level log line (visible with tracing). |
| `OsDebugBreakpoint(msg)` | Logged as a problem; execution continues.    |

## Atomics

`OsAtomicAdd/Increment/Decrement` return the new value.
`OsAtomicCompareAndSwapEx(old, new, addr, size)` works for sizes 1, 2, 4
and 8. It returns TRUE when it stores; other sizes are reported and return
TRUE (legacy).

## Critical sections

A critical section stands in for an interrupt-disabling spinlock:

- `Acquire` nests on the owning thread and enters atomic context.
- `Release` undoes one level; the last release frees the section and leaves
  atomic context.
- Releasing from a non-owner is reported.
- NULL handles are ignored.

## OsLock

A binary semaphore with an owner and a nesting count:

- `OsLockLock` nests for the owner and blocks for others; it may not be
  called in atomic context.
- `OsLockTry` succeeds for the owner (nesting) or when the lock is free.
- `OsLockUnlock` and `OsLockTryUnlock` undo one level. **Any thread may
  release the lock** (semaphore semantics); unlocking a free lock is reported.

## Events

An event is a sticky flag:

- `OsEventSet` and `OsEventClear` return the previous state; `OsEventState`
  reads it.
- `OsEventWaitTime(ms)` returns `OSEVENT_WAIT_OK` as soon as the flag is set,
  consuming it. It returns `OSEVENT_WAIT_TIMEOUT` after `ms` milliseconds.
  With `ms == 0` it only polls.
- `OsEventWait` waits without a timeout.

## Threads and work items

- **`OsThreadCreate(name, highestprio, &pid)`:** starts a worker and returns
  its thread id in `pid`. High priority means SCHED_FIFO when real-time
  scheduling is available.
- **`OsThreadScheduleInit(storage, func, data)`:** prepares a work item inside
  blob-owned `OSSCHED` storage (32 bytes on i386, 64 on x86_64).
- **`OsThreadSchedule(thread, storage)`:** queues the item and returns 1, or 0
  if it is already queued. Items run one at a time in FIFO order. An item is
  marked unqueued just before it runs, so it may queue itself again.
- **`OsThreadScheduleDone`:** no effect (it was a module reference drop).
- **`OsThreadDestroy`:** stops and joins the worker; queued items are dropped.

## Periodic timers

`OsCreatePeriodicTimer(ms, cb, alloc, free, ref, &tid)`:

- **Storage:** allocates through `alloc(size, ref)` when given (freed with
  `free(p, ref)`), otherwise through `OsAllocate`.
- **The modem thread:** all periodic callbacks, together with work scheduled on
  `OsMdmThread`, run serialized on one shared worker ("modem"). That worker is
  created with the first timer and lives until shutdown, and `tid` is its id.
- **First expiry:** immediate when `ms != 0`.
- **Re-arming:** after the callback returns, the timer re-arms for `ms` later
  (fixed delay, not fixed rate).

Related calls:

- `OsSetPeriodicTimer(h, ms)`: a new period restarts the timer `ms` from now;
  0 stops it and returns FALSE; an unchanged period does nothing.
- `OsImmediateTimeOut(h)` fires as soon as possible.
- `OsDestroyPeriodicTimer(h)`: no callback runs after it returns. A timer may
  destroy itself from its own callback; it is then freed when the callback
  returns.

## One-shot timers

These are used by the HDA HAL.

- `OsCreateTimer(ms, cb, ref)` creates a stopped timer.
- `OsSetTimer` arms it for `ms` from now.
- `OsCancelTimer` disarms it without waiting.
- `OsChangeTimerTimeOut` changes `ms` for the next `OsSetTimer`.
- `OsDestroyTimer` disarms it, waits for a running callback, and frees it.

Callbacks run on the "softirq" thread in atomic context.

## Time

| Function              | Behaviour                                           |
| --------------------- | --------------------------------------------------- |
| `OsGetSystemTime`     | Milliseconds since start-up (wraps at 2^32).        |
| `OsReadCpuCnt`        | Low 32 bits of the TSC.                             |
| `OsGetProcessorFreq`  | TSC frequency in MHz, calibrated at start-up.       |
| `OsGetCurrentThread`  | Thread id; 0 in one-shot timer callbacks.           |
| `OsSleep(ms)`         | Sleeps at least `ms`; busy-waits in atomic context. |
| `OsKernelUsesRegParm` | FALSE: callbacks use plain cdecl.                   |

## FPU

The blobs bracket their floating-point sections with these calls:

- **`OsFloatPrefix`:** saves the caller's FPU/SSE state (`fxsave`), then
  initialises the x87 (`fninit`) and MXCSR (`0x1f80`). It returns the nesting
  level.
- **`OsFloatSuffix(level)`:** restores the saved state. A level that doesn't
  match is reported and refused.
- **Stray x87 values:** a non-empty x87 register stack at Prefix is reported.

## PCI configuration and physical memory

- **`OsPciReadConfig{b,w,dw}(dev, off, &val)`:** served from a 256-byte
  snapshot that a backend registered for the opaque handle `dev`. Unknown
  handles are reported and read as 0.
- **`OsPciWriteConfig*`:** logged and ignored. The controller belongs to
  `snd-hda-intel`.
- **Physical memory:** read as `OS_DEVNODE.osPageOffset + phys`. The daemon
  sets `osPageOffset = 0` and maps the BIOS area 0xE0000–0xFFFFF read-only at
  its identity address, filled from `/dev/mem`, an image file or zeros. See
  `BLOB-INTEGRATION.md` for why this matters for `HARDWARE_ID`.

## DCP and diagnostics

| Function                            | Behaviour                                                      |
| ----------------------------------- | -------------------------------------------------------------- |
| `DcpCreate`                         | Returns a valid handle (legacy: NULL only when out of memory). |
| `DcpCallback(h, data, size)`        | Delivers call-progress audio (counted for now).                |
| `DcpSetVolume`, `DcpDestroy`        | Accepted.                                                      |
| `OsDiagMgrOpen`                     | Returns a valid handle.                                        |
| `OsDiagMgrNotify`, `OsDiagMgrClose` | Accepted.                                                      |

## NVM

Parameters are stored one file per value:

| Kind    | Path                                                                                         | Access     |
| ------- | -------------------------------------------------------------------------------------------- | ---------- |
| static  | `<static>/<hwProfile>/<NAME>`                                                                | read-only  |
| dynamic | `<dynamic>/<hwInstNum>-<hwInstName>/<NAME>`                                                  | read/write |
| country | `<static>/<hwProfile>/Region/<T35><suffix>`, falling back to `…/Profile/<reference><suffix>` | read-only  |

**Names.** `NAME` is the `CFGMGR_*` enumerator without its prefix. When
several enumerators share a value (`NRINGS_TO_ANSWER = SREG+0`), the first
one wins. Eight codes have no name and are never found.

**Dynamic codes.** `PROFILE_STORED`, `POUND_UD`, `BLACK_LIST`,
`COUNTRY_CODE`, `PREVIOUS_COUNTRY_CODE`, `VRID_*`, `QC_PROFILE`,
`PCI_VENDOR_ID`, `PCI_DEVICE_ID`, `HARDWARE_PROFILE`, `HARDWARE_ID` and
`LICENSE_OWNER/KEY/STATUS`. `NVM_Write` always writes to the dynamic
directory.

**Formats.** Writes are atomic (temporary file and rename, mode 0600).

| Format    | Codes                                                 | Encoding                         |
| --------- | ----------------------------------------------------- | -------------------------------- |
| HEXSHORTS | `COUNTRY_CODE`, `PREVIOUS_COUNTRY_CODE`, `PCI_*_ID`   | `%04X` values                    |
| HEXLONGS  | `HARDWARE_ID`, `LICENSE_KEY`                          | `%08X` values                    |
| STRING    | `HARDWARE_PROFILE`, `LICENSE_OWNER`, `LICENSE_STATUS` | `"text"` (at most 80 characters) |
| HEXBYTES  | everything else                                       | `%02X` values                    |

Values are separated by commas, with a newline after every 16 bytes of data,
and the file ends with a newline.

**Reading.**

- Text between double quotes is copied verbatim.
- Every other hex token produces **one byte**, its low byte; the character
  after a token is skipped.
- A file of exactly 5 (or 9) bytes, one 4 (or 8) digit line, read into a
  2 (or 4) byte buffer, is stored as a native 16-bit (or 32-bit) integer.
- The rest of the buffer is zeroed, also when the file is missing.

**Defaults** when a file is missing:

- `COUNTRY_CODE`: 0x00B5 (USA).
- `PROFILE_STORED` and `PROFILE_FACTORY`: the Hayes factory profile
  (E1 L1 M1 V1 X3 &C1 &D2, S2=43 S3=13 S4=10 S5=8 S6=2 S7=50 S8=2 S10=14
  S11=95 S12=50 S29=70).

**`NVM_Read(CFGMGR_COUNTRY_STRUCT)`.** The T.35 country code arrives in place
of the size pointer. The call reads `OEM_DAATYPE`, then fills a
`CtryPrmsStruct` field by field:

- Country parameters are read through the Region/Profile fallback; the
  reference comes from `Region/<T35>/REFERENCE`.
- A missing REFERENCE leaves it zero, so the fallback is `Profile/0000`
  (legacy).
- Si3054/55 DAAs adjust the dial TX levels (−9 dB, legacy).

**`NVM_Open(devnode)`** creates the dynamic directory (mode 0700) and adopts a
stored `HARDWARE_PROFILE`.
