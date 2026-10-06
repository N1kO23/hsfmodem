# Architecture

## Components

```text
 applications (minicom, pppd)
        │  /dev/ttySHSF0 -> /run/hsfmodem/ttySHSF0 -> /dev/pts/N
┌-------┴-------------------- hsfmodemd (userspace, MIT) ----------------------┐
│ serial bridge   pty ⇄ ComCtrl_Read/Write; DTR/HUPCL/carrier emulation        │
│ engine glue     OS_DEVNODE, ComCtrl lifecycle, event handler                 │
│ Conexant blobs  hsfengine.O (DSP engine) + hsfhda.O (HDA HAL), unchanged     │
│ OS layer        daemon/os: threads, timers, locks, events, memory, FPU, ...   │
│ NVM store       daemon/nvm: per-device parameters, legacy file format        │
│ HDA backend     alsa (PCM + hwdep) | fake | record | replay                   │
└-------┬---------------------------------------┬------------------------------┘
        │ ALSA PCM hw:X,6 (modem stream, mmap)   │ hwdep (verbs, events, tags, position)
┌-------┴--------------------- kernel ----------┴------------------------------┐
│ snd-hda-codec-hsfmodem.ko (GPL-2.0)   snd-hda-intel (probe_mask=3 on X/T/R60) │
└------------------------------------------------------------------------------┘
```

**Why userspace?** The blobs only call an abstraction layer we provide, so
they don't care where they run. In the kernel they would need GPL-only
interfaces reached through wrapper modules, in-kernel FPU juggling and
`ibt=off` on newer CPUs, and they would break with kernel updates. In a
process they can be tested on any machine, and a crash cannot take the system
down. The kernel side shrinks to a small GPL codec driver, modelled on the
in-tree `si3054` modem driver.

## Threads in the daemon

| Thread        | Kernel equivalent                         | Runs                                                                                             |
| ------------- | ----------------------------------------- | ------------------------------------------------------------------------------------------------ |
| `softirq`     | timer softirq                             | one-shot timer callbacks (atomic context); expiry of periodic timers                             |
| `modem`       | the "modem" kthread worker                | every periodic timer callback (the DSP), and the serial pump: ComCtrl reads, writes and controls |
| `hdacaesarif` | a blob thread (`OsThreadCreate`)          | the HDA HAL's own work                                                                           |
| `hda-events`  | ALSA unsolicited-event work, PM callbacks | codec unsolicited responses -> blob callback; suspend/resume handshake                           |
| `serial`      | `serial_core` + tty layer                 | the pseudo-terminal: opens, closes, termios, carrier hangups ([SERIAL.md](SERIAL.md))            |
| `control`     | `/proc/hsfmodem`                          | the status socket                                                                                |
| main          | process context                           | start-up, privilege switch, signals                                                              |

## Repository layout

| Path                    | Contents                                                                                                 |
| ----------------------- | -------------------------------------------------------------------------------------------------------- |
| `daemon/os/`            | OS layer (spec: [OS-LAYER.md](OS-LAYER.md))                                                              |
| `daemon/nvm/`           | NVM store                                                                                                |
| `daemon/hda/`           | HD-audio backends behind the blob's `OsHdaCodec*` pointers: alsa, fake, record, replay                   |
| `daemon/engine/`        | engine glue: device node, ComCtrl lifecycle, events, power management                                    |
| `daemon/serial/`        | serial bridge and the simulated modem (spec: [SERIAL.md](SERIAL.md))                                     |
| `daemon/*.c`            | `hsfmodemd`: settings, status socket, privileges, start-up                                               |
| `daemon/blob/`          | blob preparation and verification (`blobprep.py`), name-table generator, linker fragment                 |
| `kernel/`               | `snd-hda-codec-hsfmodem.ko` (GPL-2.0)                                                                    |
| `include/uapi/`         | the hwdep interface between the two                                                                      |
| `nvm/`                  | static NVM tree generation from the Conexant `.inf` files                                                |
| `tests/unit/`           | OS layer, NVM, backends, serial bridge (TAP)                                                             |
| `tests/contract/`       | the real blobs against the OS layer and the fake/record/replay backends                                  |
| `tests/integration/`    | `hsfmodemd` and `hsfmodem-config` as processes: options, signals, serial port, status socket, privileges |
| `tools/`                | `hsfmodem-config`: region, license, status, `probe_mask` set-up                                          |
| `data/`                 | settings file, OpenRC and systemd services, sysusers.d                                                   |
| `packaging/gentoo/`     | live ebuild (linux-mod-r1 + meson) and the service account                                               |
| `third_party/linuxant/` | blobs, ABI headers, `.inf` data (unchanged)                                                              |
| `legacy/`               | the original 2.4/2.6 driver, for reference                                                               |

## Roadmap

| Phase | Content                                             | Status                                                               |
| ----- | --------------------------------------------------- | -------------------------------------------------------------------- |
| 0     | Feasibility: blobs in userspace (x86_64, i386)      | done ([BLOB-INTEGRATION.md](BLOB-INTEGRATION.md))                    |
| 0a    | ThinkPad inventory: CPU, `probe_mask=3`, codec dump | needs hardware                                                       |
| 1     | Repository layout, meson build, CI                  | done                                                                 |
| 2     | OS layer with unit tests                            | done; virtual clock pending                                          |
| 3     | NVM store, golden tests                             | store done; golden tests pending                                     |
| 4     | Kernel codec driver and uapi                        | builds W=1-clean on 6.17–7.2; load, PM and crash tests need hardware |
| 5     | HDA backends: alsa, fake, record/replay             | done; the alsa backend is untested until hardware                    |
| 6     | Daemon, serial bridge, integration tests            | done; suspend/resume needs hardware                                  |
| 0b    | First hardware run: AT commands, a recording        | next, needs hardware                                                 |
| 7     | ThinkPad bring-up and line lab                      | needs hardware                                                       |
| 8     | Configuration tool, services, packaging             | tool, services and ebuild done; legacy removal after hardware parity |
