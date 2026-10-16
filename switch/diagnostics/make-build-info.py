#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Embed build provenance in NROs, so uploading only runtime logs is sufficient."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def generate(source, info, archive):
    source = source.resolve()
    def git(*args):
        return subprocess.check_output(
            ['git', '-c', f'safe.directory={source}', '-C', str(source), *args],
            text=True,
        ).strip()

    revision = git('rev-parse', 'HEAD')
    if info.is_file():
        text = info.read_text()
        commits = [s.removeprefix('Commit: ').strip() for s in text.splitlines()
                   if s.startswith('Commit: ')]
        if commits != [revision]:
            raise ValueError('BUILDINFO commit does not match the source; rebuild or supply matching metadata')
    else:
        text = (f'NXVK locally built NRO\nCommit: {revision}\n'
                'Build run: local / CI toolchain inventory unavailable\n')
    if any(ord(c) < 32 and c not in '\n\r\t' for c in text):
        raise ValueError('Unsupported control character in BUILDINFO')
    text = text.rstrip() + '\n'
    text += 'Source tree: ' + ('dirty' if git('status', '--porcelain') else 'clean') + '\n'
    if archive and archive.is_file():
        text += 'Linked libnvk.a SHA256: ' + hashlib.sha256(archive.read_bytes()).hexdigest() + '\n'
    else:
        text += 'Linked libnvk.a SHA256: unavailable (standalone runner or unbuilt driver)\n'
    build_id = hashlib.sha256(text.encode()).hexdigest()
    header = ('/* Generated build provenance; do not edit. */\n'
              '#ifndef NVK_GENERATED_BUILD_INFO_H\n#define NVK_GENERATED_BUILD_INFO_H\n'
              f'#define NVK_BUILD_ID "{build_id}"\n'
              f'#define NVK_BUILD_INFO_TEXT {json.dumps(text)}\n#endif\n')
    return header, text, build_id


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--info', type=Path, required=True)
    parser.add_argument('--archive', type=Path)
    parser.add_argument('--header', type=Path, required=True)
    parser.add_argument('--output-info', type=Path)
    args = parser.parse_args()
    header, text, build_id = generate(args.source, args.info, args.archive)
    args.header.parent.mkdir(parents=True, exist_ok=True)
    args.header.write_text(header)
    if args.output_info:
        args.output_info.write_text(text)
    print('Embedded build ID:', build_id)


if __name__ == '__main__':
    main()
