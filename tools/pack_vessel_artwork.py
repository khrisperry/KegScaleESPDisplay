"""Authoring only: pack transparent Chromium PNG layers as lossless build data.

Requires Pillow. Re-import with import_web_artwork.cjs before running this tool.
"""
import base64
import hashlib
import json
from pathlib import Path
import sys
import zlib
from PIL import Image

folder, destination = map(Path, sys.argv[1:3])
geometry = json.loads((folder / 'geometry.json').read_text())
result = {'source_repository': 'khrisperry/KegScaleESP',
          'source_commit': '1e9553931d8d9dde991d32ee82f9656ff7dd2b30',
          'format': 'RGB565A8 little endian RGB plane followed by alpha plane', 'assets': {}}
raw_total = 0
for kind in ['Can', 'Pint', 'Cup', 'Crowler', 'Growler', 'Keg']:
    asset = dict(geometry[kind], layers={})
    for name in ['base', 'liquid', 'foam', 'front']:
        image = Image.open(folder / f'{kind}-{name}.png').convert('RGBA')
        box = image.getchannel('A').getbbox()
        if box is None:
            raise ValueError(f'{kind}-{name}: empty artwork layer')
        cropped = image.crop(box)
        rgb = bytearray()
        alpha = bytearray()
        for r, g, b, a in cropped.getdata():
            pixel = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3) if a else 0
            rgb.extend((pixel & 255, pixel >> 8))
            alpha.append(a)
        pixels = bytes(rgb + alpha)
        raw_total += len(pixels)
        asset['layers'][name] = {'x': box[0], 'y': box[1], 'width': cropped.width, 'height': cropped.height,
                                 'sha256': hashlib.sha256(pixels).hexdigest(),
                                 'pixels': base64.b64encode(zlib.compress(pixels, 9)).decode('ascii')}
    result['assets'][kind] = asset
# Keep base64 payloads on separate lines for small diffs when one image changes.
destination.parent.mkdir(parents=True, exist_ok=True)
destination.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
print(f'Artwork pixels: {raw_total:,} bytes in flash; source data: {destination.stat().st_size:,} bytes')
