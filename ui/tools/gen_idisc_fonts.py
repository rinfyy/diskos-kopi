#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""iDisc theme fonts (Inter, SIL OFL 1.1, see ui/fonts/idisc/README.md) converted with lv_font_conv 1.5.3.
Output: ui/font_id_<face>_<px>.c with LVGL name font_id_<face>_<px>.
Usage: python3 ui/tools/gen_idisc_fonts.py [--converter path/to/lv_font_conv.js] [--only font_id_text_16]"""
import argparse, subprocess
from pathlib import Path
UI = Path(__file__).resolve().parent.parent
TEXT = '0x20-0x7E,0xA0-0xFF,0x2013,0x2014,0x2018-0x201D,0x2026,0x2022,0x203A,0x2212'
CLOCK = '0x20,0x25,0x2D,0x2E,0x2F,0x30-0x3A,0x41,0x4D,0x50,0xB0'     # space % - . / 0-9 : A M P degree
FACES = {
    'text':  ('Inter-Regular.otf', [(12, TEXT), (14, TEXT), (16, TEXT), (18, TEXT), (20, TEXT), (22, TEXT), (24, TEXT), (28, TEXT)]),
    'semi':  ('Inter-SemiBold.otf', [(14, TEXT), (16, TEXT), (18, TEXT), (20, TEXT), (22, TEXT), (24, TEXT), (28, TEXT), (34, TEXT)]),
    'clock': ('InterDisplay-SemiBold.otf', [(80, CLOCK), (100, CLOCK)]),
}
def main():
    p = argparse.ArgumentParser(); p.add_argument('--converter', type=Path, default=UI.parent / 'node_modules/lv_font_conv/lv_font_conv.js')
    p.add_argument('--only'); a = p.parse_args()
    for face, (src, sizes) in FACES.items():
        ttf = UI / 'fonts/idisc' / src
        for px, rng in sizes:
            fn = f'font_id_{face}_{px}'
            if a.only and a.only != fn: continue
            dest = UI / f'{fn}.c'
            subprocess.run(['node', str(a.converter), '--font', str(ttf), '--size', str(px), '--bpp', '4', '--format', 'lvgl',
                            '--range', rng, '--no-compress', '--force-fast-kern-format', '--lv-font-name', fn, '-o', str(dest)], check=True)
            dest.write_text(dest.read_text().replace(str(ttf), f'fonts/idisc/{src}').replace(str(dest), dest.name).rstrip() + '\n')
            print(f'made {fn}', flush=True)
if __name__ == '__main__': main()
