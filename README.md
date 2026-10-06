# hsfmodem

A modernisation of the Linuxant/Conexant **HSF softmodem** driver
(hsfmodem 7.80.02.05), so that Conexant HD-Audio modems work on current
Linux kernels. The primary targets are the ThinkPad R60/X60s family, with a
Conexant "HDA D330 MDC V.92" modem (codec `14f1:2bfa`).

> **Status: work in progress, not yet tried on hardware.** The closed modem
> engine runs in userspace on x86_64 and i386 under a tested OS layer. The
> kernel codec driver, the HD-Audio backends, the daemon and its serial port
> are written and tested without a modem. What remains is the first run on a
> ThinkPad, then packaging. See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

## How it works

The original driver ran Conexant's closed DSP engine inside the kernel and
stopped working after Linux 2.6. This version runs the same unmodified engine
in a userspace daemon:

```text
 minicom / pppd --> /dev/ttySHSF0 -> /run/hsfmodem/ttySHSF0 (pty)
 hsfmodemd -- Conexant engine + HDA HAL blobs, on a userspace OS layer
     │ ALSA PCM (modem stream)   │ hwdep (codec verbs, events)
 snd-hda-codec-hsfmodem.ko (small GPL codec driver) -- snd-hda-intel
```

[`docs/BLOB-INTEGRATION.md`](docs/BLOB-INTEGRATION.md) explains what it takes
to run kernel-built objects in a process, and
[`docs/SERIAL.md`](docs/SERIAL.md) how the daemon behaves as a serial port.

## Building and testing

You need `meson`, `ninja`, a C compiler, `python3`, `perl` and `binutils`.

```sh
meson setup build
meson test -C build                 # unit, contract and integration tests
meson setup build-asan -Db_sanitize=address,undefined -Db_lundef=false
make -C kernel KDIR=/lib/modules/$(uname -r)/build    # the codec driver
```

The build picks the blobs that match the host CPU (`-Darch=x86_64|i386`). The
i386 tests run in a container:

```sh
docker run --rm --platform linux/386 -v "$PWD":/src -w /src i386/debian:trixie \
    sh -c 'ci/deps-debian.sh && ci/run-tests.sh /tmp/build'
```

## Trying the daemon without a modem

`--backend=sim` replaces the engine and the codec with a simulated modem, so
the serial port can be exercised anywhere:

```sh
build/daemon/hsfmodemd --backend=sim --link=/tmp/ttySHSF0 --control=/tmp/hsfmodem.ctl \
    --dev-link= --nvm-dynamic=/tmp/hsfmodem-nvm -c /dev/null &
minicom -D /tmp/ttySHSF0        # ATI3, ATD123 (connects), +++, ATH
echo status | socat - UNIX-CONNECT:/tmp/hsfmodem.ctl
```

On a machine with the modem, `hsfmodemd` (default `--backend=alsa`) finds the
codec through `snd-hda-codec-hsfmodem`; `--backend=record:FILE` saves the
codec dialogue so it can be replayed in tests later. Run it as root with
`--user=` set to a service account in the `dialout` and `audio` groups: it
needs root only to start.

## Installing

```sh
meson setup build --prefix=/usr --sysconfdir=/etc -Dopenrc=true   # or -Dsystemd=true
meson install -C build
make -C kernel && sudo make -C kernel install && sudo depmod -a
sudo hsfmodem-config setup          # ThinkPad X/T/R60: let snd-hda-intel see the modem
sudo hsfmodem-config region AUTO    # or a name from "hsfmodem-config regions"
```

Create the service account (`hsfmodem`, in the `audio` and `dialout`
groups; systemd's sysusers.d file does this), then start `hsfmodemd`. Gentoo
users can use the ebuild in [`packaging/gentoo/`](packaging/gentoo/).
[`docs/HARDWARE-TESTING.md`](docs/HARDWARE-TESTING.md) walks through the first
run on a ThinkPad.

## Hardware notes

- **ThinkPad X/T/R60 hide the modem codec from Linux.** A quirk in
  `snd-hda-intel` probes only codec slot 0 on these models. Load it with
  `snd_hda_intel.probe_mask=3` (kernel command line or modprobe.d).
- **Without a Linuxant license key the engine runs in FREE mode**: 14.4 kbps,
  data only, no fax. Keys are bound to a hardware ID the engine computes; this
  project does not bypass that.

## Licensing

- The new code is MIT licensed, except the kernel module
  ([`kernel/`](kernel/)), which is GPL-2.0, and its uapi header, which is
  GPL-2.0 WITH Linux-syscall-note.
- The Conexant/Linuxant blobs, their headers and the `.inf` data in
  [`third_party/linuxant/`](third_party/linuxant/) remain under their own
  restrictive [license](third_party/linuxant/LICENSE).
- [`COPYING.md`](COPYING.md) has the details.
- The original 2.4/2.6 driver is kept for reference in [`legacy/`](legacy/).
