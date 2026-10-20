"""Generate Talk/inbox LVGL fonts from Material Symbols Rounded.

Usage: python3 tools/gen_talk_icons.py MaterialSymbolsRounded.ttf /path/to/lv_font_conv.js
Requires fontTools and Node. Talk uses a custom walkie-talkie outline at U+E029;
the inbox keeps Material Symbols' mail glyph. Both retain the UI's filled,
weight-500 style at their matching optical sizes.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

from fontTools.pens.reverseContourPen import ReverseContourPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont


def walkie_talkie():
    pen = TTGlyphPen(None)

    def rounded_rect(x, y, w, h, r, hole=False):
        p = ReverseContourPen(pen) if hole else pen
        p.moveTo((x + r, y))
        p.lineTo((x + w - r, y))
        p.qCurveTo((x + w, y), (x + w, y + r))
        p.lineTo((x + w, y + h - r))
        p.qCurveTo((x + w, y + h), (x + w - r, y + h))
        p.lineTo((x + r, y + h))
        p.qCurveTo((x, y + h), (x, y + h - r))
        p.lineTo((x, y + r))
        p.qCurveTo((x, y), (x + r, y))
        p.closePath()

    # Antenna, top dial and side push-to-talk button sit flush with the body.
    rounded_rect(260, 690, 80, 190, 30)
    rounded_rect(540, 690, 100, 80, 20)
    rounded_rect(160, 420, 80, 150, 20)
    rounded_rect(240, 80, 500, 610, 80)
    rounded_rect(320, 470, 340, 130, 24, hole=True)  # display
    for y in (220, 310):
        rounded_rect(340, y, 300, 40, 20, hole=True)  # speaker grille
    return pen.glyph()


source, converter = sys.argv[1:]
assets = Path(__file__).resolve().parents[1] / 'firmware/s3/src/ui/assets'
with tempfile.TemporaryDirectory(prefix='audioknob-walkie-') as tmp:
    for size in (20, 48):
        font = instantiateVariableFont(TTFont(source),
                                      {'FILL': 1, 'wght': 500, 'GRAD': 0, 'opsz': size})
        name = font.getBestCmap()[0xE029]
        font['glyf'][name] = walkie_talkie()
        font['hmtx'][name] = (960, 160)
        ttf = Path(tmp) / f'talk-{size}.ttf'
        font.save(ttf)
        target = assets / f'font_icons_talk_{size}.c'
        subprocess.run(['node', converter, '--font', str(ttf), '-r', '0xe029,0xe158',
                        '--size', str(size), '--bpp', '4', '--format', 'lvgl', '--no-compress',
                        '--lv-font-name', f'icons_talk_{size}', '--lv-include', 'lvgl.h',
                        '-o', str(target)], check=True)
        # Keep generated headers reproducible across machines and temp directories.
        text = target.read_text().replace(str(ttf), f'talk-{size}.ttf').replace(str(target), target.name)
        target.write_text(text.rstrip() + '\n')
