#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Embed build provenance in NROs, so uploading only runtime logs is sufficient."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def generate(source, info, archives):
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
    # One line per archive the .nro links, so an upload can be compared against
    # a rebuild of any single archive instead of only the driver directory.
    if archives is None:
        archives = []
    elif isinstance(archives, (str, Path)):
        archives = [archives]
    archives = [a for a in archives if a is not None]
    if not archives:
        archives = [None]
    for archive in archives:
        if archive is not None and Path(archive).is_file():
            digest = hashlib.sha256(Path(archive).read_bytes()).hexdigest()
        else:
            digest = 'unavailable (standalone runner or unbuilt driver)'
        label = Path(archive).name if archive is not None else 'libnvk.a'
        text += f'Linked {label} SHA256: {digest}\n'
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
    parser.add_argument('--archive', type=Path, action='append',
                        help='an archive the .nro links; repeat once per archive')
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
