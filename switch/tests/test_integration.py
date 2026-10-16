# SPDX-License-Identifier: GPL-2.0-or-later
"""Package composition/configuration checks. Real SDK linking is a separate CI step."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class IntegrationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.pkg = self.root / 'pkg'
        for name in ('lib/libnvk.a', 'lib/libnvk_support.a', 'include/vulkan/vulkan.h'):
            p = self.pkg / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text('configuration-only fixture\n')
        (self.pkg / 'lib/pkgconfig').mkdir()
        # Execute the actual Makefile printf recipe, rather than a hand-written
        # substitute .pc file. No driver/compiler/container commands are run.
        make = (ROOT / 'Makefile').read_text()
        start = make.index("\tprintf '%s\\n' ", make.index('package: driver'))
        marker = '> $(PKGDIR)/lib/pkgconfig/nxvk.pc'
        end = make.index(marker, start) + len(marker)
        (self.root/'pc.mk').write_text(
            f'PKGDIR := {self.pkg}\nPKG_VERSION := 26.2.2\nPORTLIB := /wrong/installed/prefix\n'
            'package:\n' + make[start:end] + '\n')
        subprocess.run(['make', '-f', str(self.root/'pc.mk'), 'package'],
                       check=True, capture_output=True, text=True)
        subprocess.run(['bash', str(ROOT/'switch/integration/stage.sh'), str(self.pkg)],
                       check=True, capture_output=True, text=True)
        relocated = self.root / 'relocated sdk'
        self.pkg.rename(relocated)
        self.pkg = relocated

    def test_package_has_standalone_sources_not_runner_glue(self):
        example = self.pkg/'examples/switch-minimal'
        self.assertEqual({p.name for p in example.iterdir()}, {
            'main.c', 'example_compat.c', 'build.sh', 'CMakeLists.txt',
            'switch-toolchain.cmake', 'README.md'})
        self.assertEqual((example/'example_compat.c').read_bytes(),
                         (ROOT/'switch/smoke/nvk_compat.c').read_bytes())
        text = (example/'main.c').read_text()
        self.assertNotIn('nvk_harness.h', text)
        self.assertNotIn('nvk_chain.c', text)
        self.assertIn('NXVK_EXAMPLE_ALLOW_LIMITED_COMPAT', (example/'CMakeLists.txt').read_text())
        self.assertIn('NXVK::Driver', (self.pkg/'lib/cmake/NXVK/NXVKConfig.cmake').read_text())
        self.assertNotIn('example_compat', (self.pkg/'lib/cmake/NXVK/NXVKConfig.cmake').read_text())

    @unittest.skipUnless(shutil.which('pkg-config'), 'pkg-config needed for relocation check')
    def test_pc_relocates_without_manual_prefix_override(self):
        env = dict(os.environ, PKG_CONFIG_PATH='', PKG_CONFIG_SYSROOT_DIR='',
                   PKG_CONFIG_LIBDIR=str(self.pkg/'lib/pkgconfig'))
        prefix = subprocess.check_output(['pkg-config', '--variable=prefix', 'nxvk'],
                                         env=env, text=True).strip()
        self.assertEqual(Path(prefix).resolve(), self.pkg)
        libs = subprocess.check_output(['pkg-config', '--libs', 'nxvk'], env=env, text=True)
        self.assertIn('-lnvk_support', libs)
        self.assertIn('-Wl,--whole-archive', libs)
        self.assertNotIn('/wrong/installed/prefix', libs)
        self.assertNotIn('-lEGL', libs)

    @unittest.skipUnless(shutil.which('cmake'), 'CMake needed for host configuration-only check')
    def test_cmake_target_is_relocatable_and_rejects_incomplete_package(self):
        dkp = self.root/'sdk'
        (dkp/'libnx/include').mkdir(parents=True)
        (dkp/'libnx/include/switch.h').write_text('/* configuration fixture */\n')
        source = self.root/'consumer'; source.mkdir()
        (source/'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.16)
project(package_config NONE)
find_package(NXVK CONFIG REQUIRED)
get_target_property(libs NXVK::Driver INTERFACE_LINK_LIBRARIES)
file(WRITE "${CMAKE_BINARY_DIR}/target.txt" "${NXVK_INCLUDE_DIR}\\n${libs}\\n")
''')
        def configure(out):
            return subprocess.run(['cmake', '-S', str(source), '-B', str(out),
                '-DNXVK_DIR='+str(self.pkg/'lib/cmake/NXVK'), '-DDEVKITPRO='+str(dkp)],
                capture_output=True, text=True)
        result = configure(self.root/'ok')
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        target = (self.root/'ok/target.txt').read_text()
        self.assertIn(str(self.pkg/'include'), target)
        self.assertIn(str(self.pkg/'lib/libnvk.a'), target)
        self.assertIn(str(self.pkg/'lib/libnvk_support.a'), target)
        (self.pkg/'lib/libnvk_support.a').unlink()
        result = configure(self.root/'missing')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Incomplete NXVK distribution', result.stdout+result.stderr)

    def test_standalone_app_and_public_header_syntax(self):
        flags = ['-fsyntax-only', '-D_GNU_SOURCE', '-D__SWITCH__', '-DVK_USE_PLATFORM_VI_NN',
                 '-Wall', '-Wextra', '-Werror', '-I'+str(ROOT/'include'),
                 '-I'+str(ROOT/'switch/integration/include'),
                 '-I'+str(ROOT/'switch/tests/syntax-stubs'), '-I'+str(ROOT/'switch/tests/stubs')]
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', *flags,
                        str(ROOT/'switch/integration/example/main.c')], check=True)
        cpp = self.root/'header.cpp'
        cpp.write_text('#define VK_NO_PROTOTYPES\n#include <nxvk/nxvk_static.h>\n'
                       'PFN_vkGetInstanceProcAddr resolver = vk_icdGetInstanceProcAddr;\n')
        subprocess.run(['g++', '-std=c++17', *flags, str(cpp)], check=True)

    def test_build_wrapper_rejects_missing_archives(self):
        (self.pkg/'lib/libnvk.a').unlink()
        result = subprocess.run(['bash', str(self.pkg/'examples/switch-minimal/build.sh'),
                                 str(self.pkg), str(self.root/'bad')], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('incomplete package', result.stderr)
        self.assertFalse((self.root/'bad').exists())


if __name__ == '__main__':
    unittest.main()
