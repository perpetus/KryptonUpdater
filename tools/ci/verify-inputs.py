#!/usr/bin/env python3
# Copyright 2026 The Krypton Authors
# Use of this source code is governed by the GPL-3.0 license in LICENSE.
"""Verify browser/UI CI artifacts before a signing runner interprets them."""

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--run-id', required=True)
    parser.add_argument('--commit')
    args = parser.parse_args()
    browser = args.directory / 'browser'
    runtime = args.directory / 'ui-runtime'
    record = json.loads((browser / 'browser-build.json').read_text(encoding='utf-8'))
    ui = json.loads((runtime / 'build-info.json').read_text(encoding='utf-8'))
    manifest = json.loads((runtime / 'runtime-manifest.json').read_text(encoding='utf-8'))
    if (record['repository'] != 'perpetus/Krypton'
            or record['run_repository'] != 'perpetus/krypton-windows'
            or record['run_id'] != args.run_id
            or (args.commit and record['commit'] != args.commit)
            or ui['repository'] != 'perpetus/KryptonUI' or record['ui_commit'] != ui['commit']
            or not re.fullmatch(r'[0-9a-f]{40}', record['commit'])
            or not re.fullmatch(r'[0-9a-f]{40}', record['ui_commit'])):
        raise ValueError('Unexpected build provenance')
    for name, expected in record['files'].items():
        relative = PurePosixPath(name)
        if relative.is_absolute() or '..' in relative.parts or '\\' in name or ':' in name:
            raise ValueError('Unsafe browser artifact filename')
        path = browser / name
        if (path.is_symlink() or not path.resolve().is_relative_to(browser.resolve())
                or digest(path) != expected):
            raise ValueError(f'Browser artifact hash mismatch: {name}')
    actual = {path.relative_to(browser).as_posix() for path in browser.rglob('*')
              if path.is_file() and path.name != 'browser-build.json'}
    if actual != set(record['files']):
        raise ValueError('Browser artifact contains unrecorded or missing files')
    required = ('out/Default/chrome.exe', 'out/Default/chrome.dll',
                'out/Default/setup.exe', 'out/Default/KryptonPatch.exe',
                'chrome/installer/mini_installer/chrome.release', 'LICENSE')
    if not all(name in record['files'] for name in required):
        raise ValueError('Browser artifact lacks required packaging inputs')
    if digest(runtime / 'abi.h') != manifest['abi_sha256']:
        raise ValueError('UI artifact C ABI hash mismatch')
    expected_files = {'krypton_ui.dll', 'Microsoft.WindowsAppRuntime.Bootstrap.dll',
                      *(f'assets/settings/{name}.png' for name in (
                          'hr_classic', 'hr_compact', 'hr_dyn', 'vt_compact',
                          'vt_sidebar', 'vt_sidebar_title'))}
    if set(manifest['files']) != expected_files:
        raise ValueError('Incomplete UI runtime manifest')
    for name, expected in manifest['files'].items():
        if (digest(runtime / name) != expected
                or record['files'].get(f'out/Default/{name}') != expected
                or record['runtime'].get(name) != expected):
            raise ValueError(f'Browser/UI runtime mismatch: {name}')
    print(f'Verified browser {record["product_version"]} and UI {record["ui_commit"]}.')


if __name__ == '__main__':
    main()
