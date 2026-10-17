"""Expand checked-in lossless RGB565+A8 pixels into LVGL 9.5 C++ data.

Normal firmware builds need only Python's standard library. The browser importer
is an authoring tool and is never invoked by CMake or downloaded at build time.
"""
import argparse
import base64
import json
import hashlib
from pathlib import Path
import zlib


def generate(source: Path, output: Path) -> None:
    document = json.loads(source.read_text(encoding="utf-8"))
    names = ('Can', 'Pint', 'Cup', 'Crowler', 'Growler', 'Keg')
    layers = ('base', 'liquid', 'foam', 'front')
    if set(document['assets']) != set(names):
        raise ValueError('Artwork must contain exactly the six supported image sets')
    lines = ['// Generated from web artwork; do not edit.', '#include "vessel_artwork.h"',
             'namespace vessel_artwork {', 'namespace {',
             'constexpr lv_image_dsc_t image(const uint8_t *pixels, uint16_t w, uint16_t h, uint32_t size) {',
             '  lv_image_dsc_t result{};',
             '  result.header.magic = LV_IMAGE_HEADER_MAGIC;',
             '  result.header.cf = LV_COLOR_FORMAT_RGB565A8;',
             '  result.header.w = w; result.header.h = h; result.header.stride = w * 2;',
             '  result.data_size = size; result.data = pixels; return result;', '}',
             ]
    for name in names:
        asset = document['assets'][name]
        for layer_name in layers:
            layer = asset['layers'][layer_name]
            symbol = name.lower() + '_' + layer_name
            data = zlib.decompress(base64.b64decode(layer['pixels']))
            expected = layer['width'] * layer['height'] * 3
            if len(data) != expected:
                raise ValueError(f'{symbol}: expected {expected} bytes, got {len(data)}')
            if hashlib.sha256(data).hexdigest() != layer['sha256']:
                raise ValueError(f'{symbol}: pixel checksum mismatch')
            lines.append(f'alignas(4) const uint8_t {symbol}_pixels[] = {{')
            for offset in range(0, len(data), 24):
                lines.append('  ' + ','.join(f'0x{x:02x}' for x in data[offset:offset+24]) + ',')
            lines += ['};', f'const lv_image_dsc_t {symbol} = image({symbol}_pixels, {layer["width"]}, {layer["height"]}, sizeof({symbol}_pixels));']
    lines += ['} // namespace', 'const Asset assets[] = {']
    for name in names:
        asset = document['assets'][name]
        entries = []
        for layer_name in layers:
            layer = asset['layers'][layer_name]
            entries.append('{&' + name.lower() + '_' + layer_name + f', {layer["x"]}, {layer["y"]}' + '}')
        lines.append('  {' + ', '.join(entries) + f', {asset["width"]}, {asset["height"]}, {asset["top"]}, {asset["bottom"]}, {asset["foam"]}' + '},')
    lines += ['};', '} // namespace vessel_artwork', '']
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text('\n'.join(lines), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    generate(args.source, args.output)
