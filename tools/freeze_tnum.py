# Remaps the cmap so plain digits and punctuation point at Inter's tabular
# (tnum) glyphs; lv_font_conv applies no OpenType features itself.
# Usage: python3 tools/freeze_tnum.py inter-latin-700-normal.woff out.ttf
import sys
from fontTools.ttLib import TTFont
src, dst = sys.argv[1:3]
f = TTFont(src)
gsub = f["GSUB"].table
idx = {fr.FeatureTag: [] for fr in gsub.FeatureList.FeatureRecord}
for fr in gsub.FeatureList.FeatureRecord:
    idx[fr.FeatureTag] += fr.Feature.LookupListIndex
subs = {}
for li in idx.get("tnum", []):
    for st in gsub.LookupList.Lookup[li].SubTable:
        st = getattr(st, "ExtSubTable", st)
        subs.update(getattr(st, "mapping", {}))
for table in f["cmap"].tables:
    for cp, g in list(table.cmap.items()):
        if g in subs:
            table.cmap[cp] = subs[g]
f.flavor = None
f.save(dst)
