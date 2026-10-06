#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""hsfmodemd as a process: options, settings file, signals, the serial port
and the status socket.

Most tests use the simulated modem (--backend=sim), which needs neither the
engine nor hardware. Environment:
  HSFMODEMD             the daemon binary
  HSF_TEST_NVM_STATIC   the static NVM tree (for the engine tests)
"""

import grp
import os
import pwd
import select
import signal
import socket
import subprocess
import tempfile
import termios
import time
import tty
import unittest

DAEMON = os.environ.get("HSFMODEMD", "hsfmodemd")
NVM_STATIC = os.environ.get("HSF_TEST_NVM_STATIC")


def wait_for(cond, timeout=5.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if cond():
            return True
        time.sleep(0.02)
    return False


class Port:
    """The serial port, opened raw as a terminal program would."""

    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        tty.setraw(self.fd)
        time.sleep(0.05)  # let the daemon see the open

    def cflag(self, set_=0, clear=0):
        attrs = termios.tcgetattr(self.fd)
        attrs[2] = (attrs[2] | set_) & ~clear
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)

    def expect(self, want, timeout=2.0):
        """Read until `want` appears; returns everything read."""
        got = b""
        end = time.monotonic() + timeout
        while want not in got and time.monotonic() < end:
            r, _, _ = select.select([self.fd], [], [], 0.05)
            if r:
                try:
                    data = os.read(self.fd, 4096)
                except OSError:
                    break
                if not data:
                    break
                got += data
        return got

    def command(self, cmd, want=b"OK\r\n"):
        os.write(self.fd, cmd.encode() + b"\r")
        return self.expect(want)

    def hung_up(self, timeout=2.0):
        p = select.poll()
        p.register(self.fd, select.POLLIN)
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            for _, ev in p.poll(50):
                if ev & select.POLLHUP:
                    return True
        return False

    def close(self):
        os.close(self.fd)


class Daemon:
    def __init__(self, tmp, *args, config=None, rundir=None, dev_link=""):
        self.tmp = tmp
        rundir = rundir or tmp
        self.link = os.path.join(rundir, "ttySHSF0")
        self.control = os.path.join(rundir, "control")
        argv = [DAEMON, "--link", self.link, "--control", self.control, "--dev-link=" + dev_link,
                "--nvm-dynamic", os.path.join(tmp, "dynamic")]
        if NVM_STATIC:
            argv += ["--nvm-static", NVM_STATIC]
        argv += ["--config", config or "/dev/null"] + list(args)
        self.proc = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    def answers(self):
        try:
            return "tty=" in self.request("status")
        except OSError:
            return False

    def ready(self, timeout=10.0):
        """Serving: a stale socket from an earlier run does not count."""
        return wait_for(lambda: self.proc.poll() is not None or self.answers(), timeout) \
            and self.proc.poll() is None

    def request(self, line):
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(5)
            s.connect(self.control)
            s.sendall(line.encode() + b"\n")
            data = b""
            while True:
                chunk = s.recv(4096)
                if not chunk:
                    break
                data += chunk
        return data.decode()

    def status(self):
        lines = self.request("status").splitlines()
        return dict(line.split("=", 1) for line in lines if "=" in line)

    def stop(self, sig=signal.SIGTERM):
        if self.proc.poll() is None:
            self.proc.send_signal(sig)
        try:
            out, _ = self.proc.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            out, _ = self.proc.communicate()
        return self.proc.returncode, out.decode(errors="replace")


class Options(unittest.TestCase):
    def run_daemon(self, *args):
        return subprocess.run([DAEMON, *args], capture_output=True, text=True, timeout=10)

    def test_version_and_help(self):
        r = self.run_daemon("--version")
        self.assertEqual(r.returncode, 0)
        self.assertRegex(r.stdout, r"^hsfmodemd \d")
        r = self.run_daemon("--help")
        self.assertEqual(r.returncode, 0)
        self.assertIn("--backend", r.stdout)

    def test_bad_options(self):
        for args in (["--no-such-option"], ["--backend=nonsense"], ["--backend=replay:"],
                     ["--realtime=200"], ["--bios=floppy"], ["-c", "/dev/null", "stray"]):
            with self.subTest(args=args):
                r = self.run_daemon(*args)
                self.assertEqual(r.returncode, 1, r.stderr)

    def test_settings_file_errors(self):
        with tempfile.NamedTemporaryFile("w", suffix=".conf") as f:
            f.write("# comment\n\nno-such-setting = 1\n")
            f.flush()
            r = self.run_daemon("-c", f.name)
            self.assertEqual(r.returncode, 1)
            self.assertIn(f"{f.name}:3: unknown setting", r.stderr)
        r = self.run_daemon("-c", "/nonexistent/hsfmodemd.conf")
        self.assertEqual(r.returncode, 1)


class Simulated(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.TemporaryDirectory(prefix="hsfmodemd-")
        self.tmp = self.tmpdir.name

    def tearDown(self):
        if hasattr(self, "d"):
            self.d.stop(signal.SIGKILL)
        self.tmpdir.cleanup()

    def start(self, *args, **kw):
        self.d = Daemon(self.tmp, "--backend=sim", *args, **kw)
        if not self.d.ready():
            code, out = self.d.stop()
            self.fail(f"daemon did not start (exit {code}):\n{out}")
        return self.d

    def test_start_and_stop(self):
        d = self.start()
        target = os.readlink(d.link)
        self.assertTrue(target.startswith("/dev/pts/"))
        self.assertEqual(os.stat(target).st_mode & 0o777, 0o660)
        try:
            dialout = grp.getgrnam("dialout")
        except KeyError:
            dialout = None
        if dialout and (dialout.gr_gid in os.getgroups() or os.geteuid() == 0):
            self.assertEqual(os.stat(target).st_gid, dialout.gr_gid, "the port belongs to dialout")
        st = d.status()
        self.assertEqual(st["backend"], "sim")
        self.assertEqual(st["tty"], target)
        self.assertEqual((st["open"], st["dtr"], st["carrier"]), ("0", "0", "0"))
        code, out = d.stop()
        self.assertEqual(code, 0, out)
        self.assertIn("ready", out)
        self.assertIn("shutting down", out)
        self.assertFalse(os.path.lexists(d.link), "the link is removed")
        self.assertFalse(os.path.exists(d.control), "the socket is removed")

    def test_settings_file(self):
        conf = os.path.join(self.tmp, "hsfmodemd.conf")
        with open(conf, "w") as f:
            f.write("backend = alsa  # overridden on the command line\nverbose = yes\nsyslog = no\n")
        d = self.start(config=conf)
        self.assertEqual(d.status()["backend"], "sim")
        code, out = d.stop()
        self.assertEqual(code, 0)

    def test_at_commands(self):
        d = self.start()
        port = Port(d.link)
        self.assertIn(b"simulated modem", port.command("ATI3"))
        self.assertIn(b"ERROR", port.command("AT&Q", b"ERROR"))
        st = d.status()
        self.assertEqual((st["open"], st["dtr"]), ("1", "1"))
        self.assertGreater(int(st["rx_bytes"]), 0)
        port.close()
        self.assertTrue(wait_for(lambda: d.status()["open"] == "0"))
        self.assertEqual(d.status()["dtr"], "0", "the last close drops DTR (HUPCL)")

    def test_call_and_dtr_hangup(self):
        d = self.start()
        port = Port(d.link)
        port.cflag(set_=termios.CLOCAL)
        self.assertIn(b"CONNECT", port.command("ATD5551234", b"CONNECT"))
        self.assertTrue(wait_for(lambda: d.status()["carrier"] == "1"))
        os.write(port.fd, b"ping")
        self.assertIn(b"ping", port.expect(b"ping"))
        port.close()  # DTR drop with &D2: the modem hangs up
        self.assertTrue(wait_for(lambda: d.status()["carrier"] == "0"))

    def test_carrier_loss_hangs_up_terminal(self):
        d = self.start()
        before = os.readlink(d.link)
        port = Port(d.link)
        port.cflag(clear=termios.CLOCAL)
        self.assertIn(b"CONNECT", port.command("ATD*1", b"CONNECT"))
        self.assertTrue(port.hung_up(), "the terminal is hung up when carrier drops")
        port.close()
        self.assertNotEqual(os.readlink(d.link), before)
        self.assertEqual(d.status()["hangups"], "1")
        port = Port(d.link)
        self.assertIn(b"OK", port.command("AT"))
        port.close()

    def test_status_socket(self):
        d = self.start()
        self.assertEqual(d.request("frobnicate").strip(), "error unknown command")
        self.assertEqual(d.request("lastcall").strip(), "error not available")
        self.assertEqual(d.request("flush-nvm").strip(), "error not available")
        self.assertTrue(d.request("status").endswith("\n\n"), "a reply ends with an empty line")

    def test_sighup_keeps_running(self):
        d = self.start()
        d.proc.send_signal(signal.SIGHUP)
        time.sleep(0.2)
        self.assertIsNone(d.proc.poll())
        self.assertEqual(d.status()["backend"], "sim")
        code, out = d.stop(signal.SIGINT)
        self.assertEqual(code, 0)
        self.assertIn("SIGHUP", out)

    def test_restart_replaces_stale_files(self):
        d = self.start()
        d.stop(signal.SIGKILL)  # leaves the link and the socket behind
        self.assertTrue(os.path.lexists(d.link))
        self.start()
        self.assertEqual(self.d.status()["backend"], "sim")


@unittest.skipUnless(os.geteuid() == 0, "needs root")
class Privileges(unittest.TestCase):
    def test_runs_as_user(self):
        user = pwd.getpwnam("nobody")
        with tempfile.TemporaryDirectory(prefix="hsfmodemd-") as tmp:
            os.chmod(tmp, 0o755)
            run = os.path.join(tmp, "run")
            dev_link = os.path.join(tmp, "ttySHSF0")
            d = Daemon(tmp, "--backend=sim", "--user=nobody", rundir=run, dev_link=dev_link)
            try:
                self.assertTrue(d.ready(), "the daemon starts")
                with open(f"/proc/{d.proc.pid}/status") as f:
                    ids = {k: v.split() for k, v in (line.split(":", 1) for line in f) if k in ("Uid", "Gid")}
                self.assertEqual(set(ids["Uid"]), {str(user.pw_uid)}, "all user ids switched")
                self.assertEqual(set(ids["Gid"]), {str(user.pw_gid)}, "all group ids switched")
                self.assertEqual(os.stat(run).st_uid, user.pw_uid, "the run directory belongs to the user")
                self.assertEqual(os.stat(os.path.join(tmp, "dynamic")).st_uid, user.pw_uid)
                self.assertEqual(os.readlink(dev_link), d.link, "root made the static link")
                slave = os.stat(os.readlink(d.link))
                self.assertEqual(slave.st_uid, user.pw_uid)
                dialout = grp.getgrnam("dialout")
                member = "nobody" in dialout.gr_mem or user.pw_gid == dialout.gr_gid
                self.assertEqual(slave.st_gid == dialout.gr_gid, member,
                                 "the port is in group dialout exactly when the user may give it")
                port = Port(d.link)
                self.assertIn(b"OK", port.command("AT"))
                port.close()
            finally:
                code, out = d.stop()
            self.assertEqual(code, 0, out)
            self.assertIn("running as nobody", out)
            if not member:
                self.assertIn("add the service user to 'dialout'", out)
            self.assertFalse(os.path.lexists(d.link), "the user could remove its link")


@unittest.skipUnless(NVM_STATIC, "HSF_TEST_NVM_STATIC is not set")
class Engine(unittest.TestCase):
    def test_fake_codec_fails_cleanly(self):
        with tempfile.TemporaryDirectory(prefix="hsfmodemd-") as tmp:
            d = Daemon(tmp, "--backend=fake")
            try:
                out = d.proc.communicate(timeout=60)[0].decode(errors="replace")
            except subprocess.TimeoutExpired:
                d.stop(signal.SIGKILL)
                self.fail("the daemon did not give up")
            self.assertEqual(d.proc.returncode, 1, out)
            self.assertIn("opening the modem failed", out)
            self.assertIn("--backend=sim", out)
            inst = os.path.join(tmp, "dynamic", "0-HDA-14f12bfa:17aa2150-1")
            with open(os.path.join(inst, "LICENSE_STATUS")) as f:
                self.assertIn("FREE", f.read())
            self.assertFalse(os.path.lexists(d.link))

    def test_missing_recording(self):
        with tempfile.TemporaryDirectory(prefix="hsfmodemd-") as tmp:
            d = Daemon(tmp, "--backend=replay:" + os.path.join(tmp, "missing.rec"))
            out = d.proc.communicate(timeout=30)[0].decode(errors="replace")
            self.assertEqual(d.proc.returncode, 1, out)
            self.assertIn("cannot open recording", out)


if __name__ == "__main__":
    unittest.main(verbosity=2)
