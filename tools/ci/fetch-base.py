#!/usr/bin/env python3
# Copyright 2026 The Krypton Authors
# Use of this source code is governed by the GPL-3.0 license in LICENSE.
"""Retrieve an exact signed published base for bounded Zucchini generation."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import urllib.request

ROOT = Path(__file__).resolve().parents[2]


def download(url, path, limit):
    with urllib.request.urlopen(url, timeout=120) as response, path.open('wb') as output:
        total = 0
        while chunk := response.read(1024 * 1024):
            total += len(chunk)
            if total > limit:
                raise ValueError('Published asset exceeds its size limit')
            output.write(chunk)


def verify(path):
    subprocess.run(['pwsh', '-NoProfile', '-File', str(ROOT / 'tools/release-signing.ps1'),
                    'verify', '-InputFile', str(path)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version', required=True)
    args = parser.parse_args()
    if not re.fullmatch(r'\d+\.\d+\.\d+\.\d+', args.version):
        parser.error('Expected a four-part base version')
    directory = ROOT / 'out/packages' / ('published-base-' + args.version)
    directory.mkdir(parents=True, exist_ok=False)
    base = 'https://github.com/perpetus/krypton-windows/releases/download/v' + args.version + '/'
    metadata = directory / 'KryptonUpdate-win-x64.json'
    download(base + metadata.name, metadata, 2 * 1024 * 1024)
    download(base + metadata.name + '.sig', Path(str(metadata) + '.sig'), 1024)
    verify(metadata)
    record = json.loads(metadata.read_text(encoding='utf-8'))
    full = record['full']
    name = f'KryptonUpdate-{args.version}-win-x64.7z'
    if (record['repository'] != 'perpetus/krypton-windows' or record['version'] != args.version
            or record['channel'] != 'stable' or full['name'] != name
            or not 0 < full['size'] <= 2 * 1024**3):
        raise ValueError('Unexpected signed base release')
    archive = directory / name
    download(base + name, archive, full['size'])
    with archive.open('rb') as stream:
        actual = hashlib.file_digest(stream, 'sha256').hexdigest()
    if actual != full['sha256'] or archive.stat().st_size != full['size']:
        raise ValueError('Published base archive size/hash mismatch')
    manifest = directory / 'browser.7z.manifest.json'
    manifest.write_bytes(record['package_manifest'].encode('utf-8'))
    Path(str(manifest) + '.sig').write_bytes(bytes.fromhex(record['package_signature']))
    verify(manifest)
    (directory / 'package.json').write_text(json.dumps({
        'browser_version': args.version, 'archive': str(archive), 'manifest': str(manifest),
        'archive_sha256': actual,
    }, indent=2) + '\n', encoding='utf-8')
    print('Verified exact published delta base:', args.version)


if __name__ == '__main__':
    main()
