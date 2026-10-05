#!/usr/bin/env python3
"""Package the PlatformIO build in DIYFlasher's firmware manifest format."""

import argparse
import configparser
import json
import re
import shutil
from pathlib import Path

from version_info import firmware_version


def package(project: Path, output: Path) -> Path:
    config = configparser.ConfigParser(interpolation=None)
    config.read(project / 'platformio.ini')
    environment = config['platformio']['default_envs'].split()[0]
    board = config[f'env:{environment}']['board']
    version = firmware_version(project)
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]*', version):
        raise ValueError(f'Firmware version is not safe for a folder name: {version!r}')
    folder = output / f'{version}_{board}'
    build = project / '.pio' / 'build' / environment
    framework = project / '.build-cache/platformio/packages/framework-arduinoespressif32'
    images = [
        ('0x0', build / 'bootloader.bin'),
        ('0x8000', build / 'partitions.bin'),
        ('0xe000', framework / 'tools/partitions/boot_app0.bin'),
        ('0x10000', build / 'firmware.bin'),
    ]
    for _, source in images:
        if not source.is_file() or source.stat().st_size == 0:
            raise FileNotFoundError(f'Missing or empty flash image: {source}')
    folder.mkdir(parents=True, exist_ok=True)
    files = []
    for address, source in images:
        destination = folder / source.name
        shutil.copyfile(source, destination)
        files.append({
            'address': address,
            'url': f'{folder.name}/{source.name}',
            'name': source.name,
        })
    manifest = [{
        'value': folder.name,
        'label': f'{board} ({version})',
        'firmwareVersion': version,
        'board': board,
        'variants': [],
        'baudrate': 460800,
        'manualBootloader': False,
        'useStub': True,
        'files': files,
    }]
    output.mkdir(parents=True, exist_ok=True)
    manifest_path = output / 'firmwares-nostr-pager.json'
    manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest_path


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path('dist'))
    args = parser.parse_args()
    project = Path(__file__).resolve().parents[1]
    print(package(project, args.output.resolve()))
