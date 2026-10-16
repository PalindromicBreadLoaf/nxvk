# SPDX-License-Identifier: GPL-2.0-or-later
"""Check real Makefile dependency edges without needing a Switch toolchain."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SYSROOT = "switch/rust/sysroot/.stamp"
CROSS = "switch/build/cross/build.ninja"
TARGET = "switch/rust/aarch64-switch-horizon.json"
WRAPPER = "switch/rust/rustc-switch.sh"
STD_SCRIPT = "switch/rust/build-std-sysroot.sh"


class BuildDependencies(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        shutil.copyfile(ROOT / "Makefile", self.root / "Makefile")
        (self.root / "VERSION").write_text("26.2.2\n")
        inputs = [
            TARGET, WRAPPER, STD_SCRIPT,
            "switch/build/configure-mesa.sh",
            "switch/crossfiles/switch.cross", "switch/crossfiles/rust.cross",
            "switch/crossfiles/native.txt",
            "switch/build/native-tools/bin/mesa_clc",
        ]
        for name in inputs + [SYSROOT, CROSS]:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()
            self.timestamp(name, 10 if name in (SYSROOT, CROSS) else 1)

    def timestamp(self, name, delta):
        # Deterministic old timestamps avoid sleeps and filesystem clock skew.
        t = 1_000_000_000 + delta
        os.utime(self.root / name, (t, t))

    def plan(self):
        return subprocess.check_output(
            ["make", "--no-print-directory", "-n", "CONTAINER=", CROSS],
            cwd=self.root, text=True,
        )

    def test_unchanged_build_is_not_reconfigured(self):
        plan = self.plan()
        self.assertNotIn("bash " + STD_SCRIPT, plan)
        self.assertNotIn("bash switch/build/configure-mesa.sh", plan)

    def test_target_change_rebuilds_std_and_reconfigures(self):
        self.timestamp(TARGET, 20)
        plan = self.plan()
        self.assertIn("bash " + STD_SCRIPT, plan)
        self.assertIn("touch " + SYSROOT, plan)
        self.assertIn("bash switch/build/configure-mesa.sh", plan)

    def test_sysroot_script_change_rebuilds_std_and_reconfigures(self):
        self.timestamp(STD_SCRIPT, 20)
        plan = self.plan()
        self.assertIn("bash " + STD_SCRIPT, plan)
        self.assertIn("bash switch/build/configure-mesa.sh", plan)

    def test_new_sysroot_reconfigures_cross_build(self):
        self.timestamp(SYSROOT, 20)
        self.assertIn("bash switch/build/configure-mesa.sh", self.plan())

    def test_wrapper_change_does_not_rebuild_std(self):
        self.timestamp(WRAPPER, 20)
        plan = self.plan()
        self.assertNotIn("bash " + STD_SCRIPT, plan)
        self.assertIn("bash switch/build/configure-mesa.sh", plan)

    def test_rust_target_has_the_switch_cpu_default(self):
        spec = json.loads((ROOT / TARGET).read_text())
        self.assertEqual(spec["arch"], "aarch64")
        self.assertEqual(spec["cpu"], "cortex-a57")
        self.assertEqual(spec["os"], "horizon")
        self.assertIn("+neon", spec["features"].split(","))
        self.assertIn("+crc", spec["features"].split(","))


if __name__ == "__main__":
    unittest.main()
