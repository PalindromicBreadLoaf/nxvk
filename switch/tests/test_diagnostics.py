# SPDX-License-Identifier: GPL-2.0-or-later
"""Native checks for embedded provenance and isolation from the regular suite."""
import hashlib
import importlib.util
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('build_info', ROOT / 'switch/diagnostics/make-build-info.py')
build_info = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build_info)


class DiagnosticTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name)
        self.revision = subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip()

    def test_embedded_metadata_roundtrips_as_c(self):
        info = self.path / 'BUILDINFO.txt'
        info.write_text(f'Commit: {self.revision}\nToolchain: "quoted" \\ path ©\n')
        archive = self.path / 'libnvk.a'
        archive.write_bytes(b'archive fixture')
        header, text, build_id = build_info.generate(ROOT, info, archive)
        self.assertEqual(build_id, hashlib.sha256(text.encode()).hexdigest())
        self.assertIn(hashlib.sha256(archive.read_bytes()).hexdigest(), text)
        (self.path/'build.h').write_text(header)
        (self.path/'test.c').write_text('#include <stdio.h>\n#include "build.h"\nint main(void) { fputs(NVK_BUILD_INFO_TEXT, stdout); }\n')
        exe = self.path/'metadata'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Werror', str(self.path/'test.c'), '-o', str(exe)], check=True)
        self.assertEqual(subprocess.check_output([str(exe)], text=True), text)

    def test_old_build_metadata_is_rejected(self):
        info = self.path/'BUILDINFO.txt'
        info.write_text('Commit: wrong-revision\n')
        with self.assertRaises(ValueError):
            build_info.generate(ROOT, info, None)

    def test_missing_metadata_is_explicit_not_silent(self):
        header, text, build_id = build_info.generate(ROOT, self.path/'missing', None)
        self.assertIn('local / CI toolchain inventory unavailable', text)
        self.assertIn(self.revision, text)
        self.assertIn('unavailable (standalone runner or unbuilt driver)', text)
        self.assertRegex(build_id, r'^[0-9a-f]{64}$')

    def test_invalid_control_bytes_are_rejected(self):
        info = self.path/'BUILDINFO.txt'
        info.write_text(f'Commit: {self.revision}\nBad: \0\n')
        with self.assertRaises(ValueError):
            build_info.generate(ROOT, info, None)

    def runner_apps(self, define):
        # Preprocessing checks the real runner array without needing libnx types.
        (self.path/'switch.h').write_text('/* SDK not needed by the preprocessor */\n')
        code = subprocess.check_output([os.environ.get('CC', 'cc'), '-E', '-P', '-I'+str(self.path), '-D'+define,
                                        str(ROOT/'switch/smoke/nvk_runner.c')], text=True)
        array = re.search(r'const char \*const apps\[\] = \{(.*?)\};', code, re.S).group(1)
        return re.findall(r'"([a-z0-9_]+)"', array)

    def test_diagnostic_runner_has_only_two_probes(self):
        self.assertEqual(self.runner_apps('NVK_DIAGNOSTIC'), ['nvk_subtile_diag', 'nvk_subtile_nocache'])

    def test_regular_vulkan_suite_is_still_22_apps(self):
        listed = subprocess.check_output(['bash', str(ROOT/'switch/build/build-vulkan-tests.sh'), '--list'], text=True).splitlines()
        expected = sorted(a for a in listed if a != 'nvk_runner')
        self.assertEqual(len(expected), 22)
        self.assertEqual(sorted(self.runner_apps('NVK_VULKAN_ONLY')), expected)
        self.assertFalse(any('diag' in a or 'nocache' in a for a in expected))

    def test_probe_and_runner_c_syntax(self):
        # Use the actual Vulkan headers and test code with SDK declarations.
        # Dummy shader words are for parsing only; nothing is linked/executed.
        tree = self.path / 'switch'
        smoke, diag = tree / 'smoke', tree / 'diagnostics'
        smoke.mkdir(parents=True)
        diag.mkdir()
        for src in (ROOT / 'switch/smoke').glob('*.h'):
            shutil.copyfile(src, smoke / src.name)
        shutil.copyfile(ROOT / 'switch/smoke/nvk_subtile.c', smoke / 'nvk_subtile.c')
        for src in (ROOT / 'switch/diagnostics').glob('*'):
            if src.suffix in ('.h', '.c'):
                shutil.copyfile(src, diag / src.name)
        (smoke / 'shaders').mkdir()
        for name in ('fullscreen_vert', 'alu_frag', 'alu_comp'):
            (smoke / 'shaders' / (name + '.h')).write_text('static const uint32_t ' + name + '_spv[] = {0};\n')
        flags = [os.environ.get('CC', 'cc'), '-fsyntax-only', '-std=gnu11',
                 '-Wall', '-Werror', '-Wno-unused-function', '-D_GNU_SOURCE',
                 '-DVK_USE_PLATFORM_VI_NN',
                 '-I'+str(ROOT/'switch/tests/syntax-stubs'),
                 '-I'+str(ROOT/'switch/tests/stubs'), '-I'+str(ROOT/'include')]
        for defines in ([], ['-DNVK_DIAG_NO_CACHE']):
            subprocess.run(flags + defines + [str(diag/'nvk_subtile_diag.c')], check=True)
        for define in ('NVK_DIAGNOSTIC', 'NVK_VULKAN_ONLY'):
            subprocess.run(flags + ['-D'+define, str(ROOT/'switch/smoke/nvk_runner.c')], check=True)
        subprocess.run(flags + [str(smoke/'nvk_subtile.c')], check=True)

    def test_diagnostic_builder_list_needs_no_sdk(self):
        listed = subprocess.check_output(['bash', str(ROOT/'switch/diagnostics/build.sh'), '--list'], text=True).splitlines()
        self.assertEqual(listed, ['nvk_subtile_diag', 'nvk_subtile_nocache', 'nvk_diag_runner'])


if __name__ == '__main__':
    unittest.main()
