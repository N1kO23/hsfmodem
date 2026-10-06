#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""hsfmodem-config: regions, licenses, set-up checks and daemon queries.

Environment:
  HSFMODEM_CONFIG       the configured tool
  HSFMODEMD             the daemon (for the status tests)
  HSF_TEST_NVM_STATIC   the static NVM tree
"""

import importlib.machinery
import importlib.util
import os
import signal
import subprocess
import tempfile
import time
import unittest

TOOL = os.environ.get("HSFMODEM_CONFIG", "hsfmodem-config")
DAEMON = os.environ.get("HSFMODEMD")
NVM_STATIC = os.environ.get("HSF_TEST_NVM_STATIC")


def load_tool():
    loader = importlib.machinery.SourceFileLoader("hsfmodem_config", TOOL)
    spec = importlib.util.spec_from_loader("hsfmodem_config", loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


tool = load_tool()


class Tool(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.TemporaryDirectory(prefix="hsfmodem-config-")
        self.tmp = self.tmpdir.name
        self.dyn = os.path.join(self.tmp, "dynamic")
        self.inst = os.path.join(self.dyn, "0-HDA-14f12bfa:17aa2150-1")
        os.makedirs(self.inst)
        for name, value in (("HARDWARE_ID", "21CC3BB2"), ("COUNTRY_CODE", "00B5"),
                            ("LICENSE_STATUS", '"FREE (max 14.4kbps data only)"')):
            with open(os.path.join(self.inst, name), "w") as f:
                f.write(value + "\n")

    def tearDown(self):
        self.tmpdir.cleanup()

    def run_tool(self, *args):
        argv = [TOOL, "--nvm-dynamic", self.dyn]
        if NVM_STATIC:
            argv += ["--nvm-static", NVM_STATIC]
        return subprocess.run(argv + list(args), capture_output=True, text=True, timeout=30)

    def read(self, name):
        with open(os.path.join(self.inst, name)) as f:
            return f.read()


@unittest.skipUnless(NVM_STATIC, "HSF_TEST_NVM_STATIC is not set")
class Regions(Tool):
    def test_list(self):
        r = self.run_tool("regions")
        self.assertEqual(r.returncode, 0, r.stderr)
        rows = dict(line.split() for line in r.stdout.splitlines())
        self.assertEqual(rows["FINLAND"], "003C")
        self.assertEqual(rows["USA"], "00B5")
        self.assertGreater(len(rows), 50)

    def test_show_and_set(self):
        r = self.run_tool("region")
        self.assertIn("USA (00B5)", r.stdout)
        for spelling in ("finland", "FINLAND", "3c", "003C"):
            with self.subTest(spelling=spelling):
                r = self.run_tool("region", spelling)
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertEqual(self.read("COUNTRY_CODE"), "003C\n")
        self.assertTrue(os.path.exists(os.path.join(self.inst, ".region_set")))
        self.assertIn("FINLAND (003C)", self.run_tool("region").stdout)
        self.assertIn("Restart hsfmodemd", self.run_tool("region", "USA").stdout)

    def test_unknown_region(self):
        r = self.run_tool("region", "Atlantis")
        self.assertEqual(r.returncode, 1)
        self.assertIn("unknown region", r.stderr)
        self.assertEqual(self.read("COUNTRY_CODE"), "00B5\n", "unchanged")

    def test_no_modem_state(self):
        r = self.run_tool("--unit", "7", "region", "USA")
        self.assertEqual(r.returncode, 1)
        self.assertIn("start hsfmodemd once first", r.stderr)

    def test_region_from_time_zone(self):
        zi = os.path.join(self.tmp, "zoneinfo")
        os.makedirs(os.path.join(zi, "Europe"))
        open(os.path.join(zi, "Europe", "Helsinki"), "w").close()
        with open(os.path.join(zi, "zone.tab"), "w") as f:
            f.write("# comment\nFI\t+6010+02458\tEurope/Helsinki\n")
        lt = os.path.join(self.tmp, "localtime")
        os.symlink(os.path.join(zi, "Europe", "Helsinki"), lt)
        self.assertEqual(tool.timezone_region(zoneinfo=zi, localtime=lt), "003C")


class License(Tool):
    KEY = "1234ABCD"

    def display(self):
        return tool.key_to_display(self.KEY)

    def test_key_format(self):
        disp = self.display()
        self.assertRegex(disp, r"^([0-9A-F]{2}-){5}[0-9A-F]{2}$")
        self.assertTrue(disp.replace("-", "").startswith(self.KEY))
        self.assertEqual(tool.display_to_key(disp), self.KEY)
        self.assertEqual(tool.display_to_key(disp.lower().replace("-", " ")), self.KEY)
        self.assertEqual(tool.display_to_key("free"), tool.FREE_KEY)
        self.assertEqual(tool.key_to_display(tool.FREE_KEY), "FREE")

    def test_bad_keys(self):
        disp = self.display()
        wrong = disp[:-1] + ("0" if disp[-1] != "0" else "1")
        for bad in (wrong, "12-34", "ZZ-ZZ-ZZ-ZZ-ZZ-ZZ"):
            with self.subTest(key=bad):
                with self.assertRaises(tool.Error):
                    tool.display_to_key(bad)

    def test_show(self):
        r = self.run_tool("license")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("registration ID: 21CC3BB2", r.stdout)
        self.assertIn("status:          FREE (max 14.4kbps data only)", r.stdout)
        self.assertIn("key:             FREE", r.stdout)

    def test_set(self):
        r = self.run_tool("license", f"Me@Example.org/{self.display()}")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.read("LICENSE_OWNER"), '"me@example.org"\n')
        self.assertEqual(self.read("LICENSE_KEY"), self.KEY + "\n")
        self.assertIn(self.display(), self.run_tool("license").stdout)

    def test_set_rejects(self):
        for arg in ("nobody/" + self.display(), self.display(), "me@example.org/00-11-22-33-44-55"):
            with self.subTest(arg=arg):
                r = self.run_tool("license", arg)
                self.assertEqual(r.returncode, 1)
        self.assertFalse(os.path.exists(os.path.join(self.inst, "LICENSE_KEY")))


class Setup(Tool):
    def sysfs(self, subsystem):
        dev = os.path.join(self.tmp, "sys", "0000:00:1b.0")
        os.makedirs(dev)
        for name, value in (("class", "0x040300"), ("subsystem_vendor", f"0x{subsystem[0]:04x}"),
                            ("subsystem_device", f"0x{subsystem[1]:04x}")):
            with open(os.path.join(dev, name), "w") as f:
                f.write(value + "\n")
        return os.path.dirname(dev)

    def test_thinkpad_x60(self):
        conf = os.path.join(self.tmp, "hsfmodem.conf")
        r = self.run_tool("setup", "--sysfs", self.sysfs((0x17AA, 0x2010)), "--modprobe-conf", conf)
        self.assertEqual(r.returncode, 0, r.stderr)
        with open(conf) as f:
            self.assertIn("options snd-hda-intel probe_mask=3\n", f.read())

    def test_other_machine(self):
        conf = os.path.join(self.tmp, "hsfmodem.conf")
        r = self.run_tool("setup", "--sysfs", self.sysfs((0x1028, 0x01f3)), "--modprobe-conf", conf)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("nothing to set up", r.stdout)
        self.assertFalse(os.path.exists(conf))


@unittest.skipUnless(DAEMON, "HSFMODEMD is not set")
class Daemon(Tool):
    def test_queries(self):
        ctl = os.path.join(self.tmp, "control")
        d = subprocess.Popen([DAEMON, "--backend=sim", "-c", "/dev/null", "--dev-link=",
                              "--link", os.path.join(self.tmp, "tty"), "--control", ctl,
                              "--nvm-dynamic", self.dyn],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.monotonic() + 10
            while self.run_tool("--control", ctl, "status").returncode and time.monotonic() < deadline:
                time.sleep(0.05)
            r = self.run_tool("--control", ctl, "status")
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("backend=sim", r.stdout)
            r = self.run_tool("--control", ctl, "lastcall")
            self.assertEqual(r.returncode, 1)
            self.assertIn("not available", r.stderr)
        finally:
            d.send_signal(signal.SIGTERM)
            d.wait(timeout=10)
        r = self.run_tool("--control", ctl, "status")
        self.assertEqual(r.returncode, 1)
        self.assertIn("not answering", r.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
