# Testing on hardware

Everything up to here was tested without a modem. These sessions on a
ThinkPad R60 or X60s settle what only hardware can tell, and their
recordings become replay tests that run in CI from then on. Each step says
what to collect; send all of it back.

## What you need

- A ThinkPad R60 or X60s with its modem daughter card (MDC), running any Linux
  with **kernel 6.17 or newer** and its headers or build tree. A live USB stick
  is enough for step 1.
- To build: `meson`, `ninja`, `gcc`, `python3`, `perl`, `binutils`. On an
  i386-only machine, build there or in an i386 container.
- From step 6 on, a telephone line: a landline socket, an analogue telephone
  adapter (FXS port) on a VoIP box, or a line simulator. With two ThinkPads,
  one modem can call the other through the line.

## 1. Find the modem

```sh
grep -qw lm /proc/cpuinfo && echo 64-bit || echo 32-bit-only
cat /sys/class/dmi/id/product_name /sys/class/dmi/id/product_version
lspci -nn | grep -i audio
```

These models' audio controller ignores codec slot 1, where the modem sits.
Make it look, then reboot or reload the sound driver:

```sh
sudo tools/hsfmodem-config setup           # writes /etc/modprobe.d/hsfmodem.conf
# or add snd_hda_intel.probe_mask=3 to the kernel command line
```

```sh
dmesg | grep -i -e hda -e codec
ls /proc/asound/card0/                     # codec#0 (audio) and codec#1 (modem)?
cat /proc/asound/card0/codec#1
```

**Expected:** `codec#1` reports `Vendor Id: 0x14f12bfa` (or `0x14f12c06`) and a
"Modem Function Group". Check that speaker and headphone audio still work.

**Collect:** the CPU line, the DMI strings, `lspci -nn`, the `dmesg` lines, and
`codec#1` in full.

**If there is no `codec#1`:** the machine probably has no modem card. The
quirk exists because empty modem slots upset the controller, so if audio
misbehaves, undo the setting. Remove `/etc/modprobe.d/hsfmodem.conf`.

## 2. Build and load the codec driver

```sh
make -C kernel                     # KDIR=/path/to/kernel/build for another kernel
sudo make -C kernel install        # signs the module if the kernel requires it
sudo depmod -a
sudo modprobe snd-hda-codec-hsfmodem
```

The codec may already be bound to the generic driver (`snd_hda_codec_generic`)
from before our module existed. Move it over:

```sh
ls -l /sys/bus/hdaudio/devices/hdaudioC0D1/driver
echo hdaudioC0D1 | sudo tee /sys/bus/hdaudio/drivers/snd_hda_codec_generic/unbind
echo hdaudioC0D1 | sudo tee /sys/bus/hdaudio/drivers/snd_hda_codec_hsfmodem/bind
```

**Expected:**

- `/proc/asound/card0/codec#1` names "HSF HDA D330 MDC V.92 Modem".
- `/proc/asound/hwdep` lists "HSF Modem".
- `/dev/snd/hwC0D*` and `/dev/snd/pcmC0D6{p,c}` exist.

**Collect:** `dmesg` and `/proc/asound/hwdep`. Then check that the module
unloads and reloads cleanly 20 times:

```sh
for i in $(seq 20); do sudo modprobe -r snd-hda-codec-hsfmodem && sudo modprobe snd-hda-codec-hsfmodem; done
dmesg | tail
```

## 3. First run, recorded

Build, then run the daemon in the foreground. The `record` backend writes the
complete codec dialogue to a file:

```sh
meson setup build && meson test -C build
sudo build/daemon/hsfmodemd --backend=record:/tmp/hsf-first.rec --record-positions \
     --nvm-static=build/nvm/nvm-static --verbose 2>&1 | tee /tmp/hsf-first.log
```

**Expected:** "modem HDA-14f12bfa:… ready", then "ready: /run/hsfmodem/ttySHSF0".
If the engine fails to open the modem, stop here and send the log and the
recording.

In a second terminal:

```sh
build/tools/hsfmodem-config --nvm-static=build/nvm/nvm-static status
build/tools/hsfmodem-config --nvm-static=build/nvm/nvm-static license
sudo minicom -D /dev/ttySHSF0
```

In minicom, type each command and note the reply:

| Command       | Meaning                              |
| ------------- | ------------------------------------ |
| `ATI`, `ATI3` | identification                       |
| `AT&V`        | current settings                     |
| `ATX3`        | dial without waiting for a dial tone |
| `ATH1`        | go off-hook: the line relay clicks   |
| `ATH0`        | on-hook again                        |

Then quit minicom (which drops DTR) and stop the daemon with Ctrl-C.

**Collect:** `/tmp/hsf-first.log`, `/tmp/hsf-first.rec`, the minicom
transcript, and the status and license output. Then run the replay check:

```sh
build/daemon/hsfmodemd --backend=replay:/tmp/hsf-first.rec --nvm-static=build/nvm/nvm-static \
     --link=/tmp/ttyREPLAY --control=/tmp/replay.ctl --dev-link= --nvm-dynamic=/tmp/replay-nvm -c /dev/null
```

**Expected:** it reaches "ready" without hardware.

## 4. Timing

Leave the modem off-hook (`ATH1`) for 10 minutes with the daemon running
`--verbose`. Then repeat with `--realtime`.

**Expected:** no warning (`W`) or error (`E`) lines, in particular no event
overflow from the driver and no invariant violations at exit.

**Collect:** both logs and `uptime` (load), plus `chrt -p $(pidof hsfmodemd)` for
the real-time run. If real-time is refused, collect the warning; it explains
whether the kernel's RT group scheduling or the limits are the cause.

## 5. Suspend, resume and crashes

With the daemon running and the modem idle:

1. Suspend and resume 5 times (`echo mem | sudo tee /sys/power/state`). After
   each resume, `AT` should still answer `OK`.
2. Go off-hook with `ATH1` and try to suspend. The suspend should be refused
   ("refusing to suspend: the modem is in use").
3. Go off-hook again and kill the daemon hard: `sudo pkill -9 hsfmodemd`. The
   line relay should click back, and `dmesg` should show the driver resetting
   the codec.

**Collect:** the daemon log and `dmesg` for each step.

## 6. On a line

With a line connected:

| Command                           | Expected                                                       |
| --------------------------------- | -------------------------------------------------------------- |
| `ATX4DT<number>`                  | with a dial tone: dials, then `BUSY`, `NO ANSWER` or `CONNECT` |
| `ATX4DT` with the cable unplugged | `NO DIALTONE`                                                  |
| `ATA` while the line rings        | `RING` lines, then answers                                     |

Then try a data call and pppd. With modem-to-modem through a line simulator,
`ATA` on one machine and `ATDT<number>` on the other. Without a Linuxant
license the engine runs in FREE mode: connections top out at 14.4 kbps.

**Collect:** transcripts, `hsfmodem-config lastcall` after each call, and a
recording (`--backend=record:…`) of one successful connection.

## Troubleshooting

| Symptom                                    | Cause, fix                                                                                                                       |
| ------------------------------------------ | -------------------------------------------------------------------------------------------------------------------------------- |
| no `codec#1`                               | `probe_mask` not active (`cat /sys/module/snd_hda_intel/parameters/probe_mask`), or no modem card                                |
| "no modem found" from the daemon           | the codec is bound to the generic driver: see step 2                                                                             |
| module refuses to load: "Key was rejected" | sign it: `make -C kernel install` uses the kernel's signing key; with your own key, use `scripts/sign-file` from the kernel tree |
| "cannot lock memory" / real-time refused   | start as root (the daemon raises the limits before switching user), or set `rt_group_sched` budgets                              |
| HARDWARE_ID differs between runs           | it depends on the BIOS image; the daemon reads `/dev/mem` only as root and only without kernel lockdown                          |
