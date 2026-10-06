# Serial port specification

This document describes how `hsfmodemd` presents the modem as a serial port.
It is the clean-room reference for `daemon/serial/`. It was derived from the
behaviour of the legacy GPL serial driver (`legacy/modules/GPL/serial_cnxt.c`)
and the legacy device glue (`legacy/modules/cnxthw*_common.c`); no code was
taken from either.

## The port

The legacy driver registered a `serial_core` UART (`/dev/ttySHSF0`). The daemon
instead owns the master side of a pseudo-terminal and publishes its slave:

```text
/dev/ttySHSF0 -> /run/hsfmodem/ttySHSF0 -> /dev/pts/N
```

- `/run/hsfmodem/ttySHSF0` is replaced atomically (`rename`) whenever the
  pseudo-terminal is recreated, so the daemon can update it after dropping
  privileges.
- `/dev/ttySHSF0` is static. The daemon creates it at start-up when it runs as
  root; packages can create it with tmpfiles.d instead.
- The slave gets mode 0660 and the configured group (default `dialout`).

## Engine I/O

All `ComCtrl_Read`, `ComCtrl_Write` and `ComCtrl_Control` calls run as one work
item on the shared modem thread (`OsMdmThread`), as in the legacy driver. That
work item, the _pump_, does the following:

1. Applies pending modem-control changes in order: DTR/RTS changes, the port
   configuration, and a receive flush.
2. **Engine -> terminal:** reads up to 256 bytes at a time with `ComCtrl_Read` and
   writes them to the master without blocking. If the master is full, the
   unwritten bytes are kept and the pump resumes when the master is writable.
3. **Terminal -> engine:** reads from the master and offers the bytes to
   `ComCtrl_Write`. Bytes the engine does not accept are kept, and the pump
   retries on the next engine transmit event (or after 10 ms).
4. Repeats steps 2 and 3 for at most 256 passes, as the legacy interrupt loop
   did, then requeues itself if work remains.

`ComCtrl_Read`/`ComCtrl_Write` return values above `INT32_MAX` are errors, which
are logged and treated as 0.

## Engine events

The engine calls the event handler with a `COMCTRL_EVT_*` mask, possibly in
atomic context. The handler only records state and queues work:

| Event                        | Action                                                                                               |
| ---------------------------- | ---------------------------------------------------------------------------------------------------- |
| `RXCHAR`, `BREAK`, `RXOVRN`  | queue the pump. Break and overrun are counted and logged; a pseudo-terminal cannot express them      |
| `TXEMPTY`, `TXCHAR`          | queue the pump (pending transmit bytes)                                                              |
| `CTS`, `DSR`, `RLSD`, `RING` | the matching `*S` bit is the new line state; update the modem-status word and wake the serial thread |

The modem-status word starts at DSR on, everything else off, until the engine
reports otherwise (legacy behaviour).

## DTR and the terminal's open state

A pseudo-terminal has no modem-control lines, so the daemon derives DTR from
whether any process holds the slave open:

- **Open.** The first open, seen through inotify `IN_OPEN` on the slave, raises
  DTR and RTS like `serial_core` does at port start-up. The daemon then:
  - discards whatever the engine received while the port was closed (legacy
    start-up flush);
  - sends `COMCTRL_CONTROL_PORTCONFIG` from the slave's termios.
- **Close.** Last close shows as `POLLHUP` on the master, or `IN_CLOSE` followed
  by `POLLHUP`. If the slave's termios has `HUPCL`, DTR and RTS are dropped. The
  engine hangs up according to `AT&D`. Without `HUPCL`, DTR stays up, as on a
  real serial port. While closed, the master is not polled, because `POLLHUP` is
  level-triggered.
- **Defaults.** A new pseudo-terminal defaults to no `HUPCL`, unlike a serial
  port, so the daemon sets `HUPCL` once when it creates the pseudo-terminal.
  Settings a program makes persist across opens, as they do for serial ports.

## Port configuration

`COMCTRL_CONTROL_PORTCONFIG` carries the DTE speed, parity, data bits and
CTS/RTS flow control. It is sent at open, and again whenever the slave's termios
changes while the port is open; the daemon checks for changes on every wake-up
and at least every 500 ms. A pseudo-terminal forces `CS8` and clears `PARENB`,
so parity is always "none" and data bits always 8.

## Carrier loss

When `RLSD` drops while the slave is open and its termios lacks `CLOCAL`, the
daemon hangs up the terminal, as `uart_handle_dcd_change` does for a serial
port:

1. Create a new pseudo-terminal and point the `/run` link at it.
2. Close the old master. Its slave sees end-of-file and `POLLHUP`, and the
   process holding it (pppd) ends the session.
3. Handle the port as closed; by `HUPCL`, DTR drops.

The order matters: the link never points at a freed `/dev/pts` number that
another program could receive.

With `CLOCAL`, carrier changes are only logged.

## Power management

This follows legacy `cnxthw_DevMgrPMControl` and the HDA suspend and resume
handlers.

- **`hwInUse` counter.** The engine reports power transitions through the
  device node's `pmControl` callback. Every transition is passed to the engine:
  - D3 -> D0: the counter goes up before the call and back down if the call fails.
  - D0 -> D3: the counter goes down if the call succeeds and the counter is
    non-zero.
- **Suspend.** If `hwInUse` is non-zero, the suspend is vetoed (`-EBUSY`).
  Otherwise the daemon runs `COMCTRL_CONTROL_SLEEP`, then acknowledges.
- **Resume.** The daemon runs `COMCTRL_CONTROL_WAKEUP`.

## Status socket

`/run/hsfmodem/control` is a UNIX stream socket that replaces
`/proc/hsfmodem/*`. Each request is one line; the reply ends with an empty line.

| Request       | Reply                                                                                                   |
| ------------- | ------------------------------------------------------------------------------------------------------- |
| `status`      | `key=value` lines: instance, name, profile, revision, backend, codec, tty, dtr, carrier, dsr, cts, ring |
| `lastcall`    | the engine's `#UG` last-call report (`COMCTRL_MONITOR_POUND_UG`)                                        |
| `flush-nvm`   | writes pending NVM values (`NVM_WriteFlushList(TRUE)`), then `ok`                                       |
| anything else | `error unknown command`                                                                                 |

## Deliberately not emulated

- **BREAK.** A pseudo-terminal cannot send or receive BREAK
  (`tcsendbreak` on the slave has no effect on the master).
- **Modem-status ioctls on the slave.** `TIOCMGET`/`TIOCMSET`/`TIOCMIWAIT`
  fail on a pseudo-terminal. Programs see carrier through result codes and
  hang-ups instead.
- **Blocking open until carrier.** Opening the slave without `O_NONBLOCK`
  never waits for carrier.
