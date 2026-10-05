#!/usr/bin/env python3
"""Render the real firmware UI on the host with synthetic messages (GCC/G++, Pillow)."""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import subprocess
import argparse
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--check-only", action="store_true", help="Run UI checks without writing screenshots")
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
out = root / '.build-cache/preview'
out.mkdir(parents=True, exist_ok=True)
images = root / 'assets/previews'
images.mkdir(parents=True, exist_ok=True)
flags = ['-O1', '-I' + str(root / 'scripts/preview'), '-I' + str(root / 'include'),
         '-I' + str(root / 'lib/lvgl'), '-I'+str(root/'lib/ArduinoJson/src'), '-DLV_CONF_INCLUDE_SIMPLE']
sources = list((root / 'lib/lvgl/src').rglob('*.c')) + [root / 'src/ui.cpp', root / 'src/brand.cpp', root/'src/key_codec.cpp', root / 'scripts/preview/main.cpp']
headers = [root / 'include/lv_conf.h', root / 'include/pager.h', root / 'include/contacts.h', root / 'include/key_codec.h', root / 'include/brand.h', root / 'include/version.h', root / 'scripts/preview/Arduino.h']
stamp = max(path.stat().st_mtime for path in headers)
def compile_source(source):
    obj = out / ('_'.join(source.relative_to(root).parts) + '.o')
    if not obj.exists() or obj.stat().st_mtime < max(source.stat().st_mtime, stamp):
        subprocess.run(['gcc' if source.suffix == '.c' else 'g++', *flags, '-c', str(source), '-o', str(obj)], check=True)
    return str(obj)
with ThreadPoolExecutor(max_workers=6) as pool:
    objects = list(pool.map(compile_source, sources))
subprocess.run(['g++', *objects, '-lm', '-o', str(out / 'ui-preview')], check=True)
subprocess.run([str(out / 'ui-preview'), *(['--check-only'] if args.check_only else [])], cwd=root, check=True)
if args.check_only:
    raise SystemExit(0)
from PIL import Image, ImageDraw
names = ['inbox', 'message', 'composer', 'setup', 'empty', 'read-only', 'web-access','contacts','contact-edit','reset-confirm','delete-confirm','message-delete-confirm']
contact = Image.new('RGB', (3 * 344 + 24, ((len(names)+2)//3) * 530 + 24), '#080c14')
draw = ImageDraw.Draw(contact)
for index, name in enumerate(names):
    ppm = images / (name + '.ppm')
    image = Image.open(ppm).convert('RGB')
    image.save(images / (name + '.png'))
    x, y = 24 + index % 3 * 344, 32 + index // 3 * 530
    draw.text((x, y - 19), name.upper(), fill='#92a2bb')
    contact.paste(image, (x, y))
    ppm.unlink()
contact.save(images / 'overview.png')
print('Screenshots: assets/previews/overview.png')
