# SPDX-License-Identifier: GPL-3.0+
# Forja SX2 (2026-10-19): makes the fonts the languages need (UIVM.cpp merges them into Gontserrat, which has the Latin
# letters): Cyrillic from Montserrat (Gontserrat's own family) and Arabic, Chinese, Japanese and Korean from Noto, all OFL
# (assets/licenses/fuentes-*-OFL.txt). Each one is the regular weight of Google Fonts' variable font, cut to the letters of
# the language files (assets/lang/*.lng) plus the common ones of its script, so a new text rarely needs a new font.
#
#   pip install fonttools
#   python fuentes.py [folder for the downloaded sources]
import io
import sys
import urllib.request
from pathlib import Path

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "lang"))
import idiomas  # noqa: E402

GOOGLE = "https://github.com/google/fonts/raw/main/ofl/"
SOURCES = {
    # Gontserrat's "Regular" stems are 93 units wide, like Montserrat's at weight 490: the Cyrillic letters match the Latin ones.
    "Forja-Cirilico.ttf": ("montserrat/Montserrat%5Bwght%5D.ttf", {"wght": 490}),
    "NotoSansArabic-Forja.ttf": ("notosansarabic/NotoSansArabic%5Bwdth,wght%5D.ttf", {"wght": 400, "wdth": 100}),
    "NotoSansSC-Forja.ttf": ("notosanssc/NotoSansSC%5Bwght%5D.ttf", {"wght": 400}),
    "NotoSansJP-Forja.ttf": ("notosansjp/NotoSansJP%5Bwght%5D.ttf", {"wght": 400}),
    "NotoSansKR-Forja.ttf": ("notosanskr/NotoSansKR%5Bwght%5D.ttf", {"wght": 400}),
}
GONTSERRAT_SOURCE = "Gontserrat-Regular.ttf"  # the OpenOrbis sample's, the base font (not made here)


def block(first, last):
    return {chr(c) for c in range(first, last + 1)}


def decoded(encoding, rows, cells=range(0xA1, 0xFF)):
    chars = set()
    for hi in rows:
        for lo in cells:
            try:
                chars.add(bytes([hi, lo]).decode(encoding))
            except UnicodeDecodeError:
                pass
    return chars


def cjk_common():
    # Punctuation, full-width forms, shapes (□ △ ○) and arrows, which the CJK texts use.
    return block(0x3000, 0x303F) | block(0xFF00, 0xFFEF) | block(0x25A0, 0x25FF) | block(0x2190, 0x21FF) | block(0x2460, 0x24FF)


def wanted(name, letters):
    if name == "Forja-Cirilico.ttf":
        return block(0x0400, 0x052F) | {c for c in letters if 0x0400 <= ord(c) <= 0x052F}
    if name == "NotoSansArabic-Forja.ttf":
        # The basic letters and Presentation Forms-B, the shapes ForjaLang::Visual() writes.
        return block(0x0600, 0x06FF) | block(0xFE70, 0xFEFF)
    cjk_letters = {c for c in letters if ord(c) >= 0x2E80}
    if name == "NotoSansSC-Forja.ttf":
        return decoded("gb2312", range(0xB0, 0xD8)) | cjk_letters | cjk_common()  # GB2312 level 1: 3755 hanzi
    if name == "NotoSansJP-Forja.ttf":
        return decoded("euc_jp", range(0xB0, 0xD0)) | block(0x3040, 0x30FF) | cjk_letters | cjk_common()  # JIS level 1 + kana
    return decoded("euc_kr", range(0xB0, 0xC9)) | block(0x3130, 0x318F) | cjk_letters | cjk_common()  # KS X 1001 hangul


def source(cache, path):
    local = cache / path.replace("/", "_").replace("%5B", "[").replace("%5D", "]")
    if not local.exists():
        print("bajando", path)
        with urllib.request.urlopen(GOOGLE + path) as response:
            local.write_bytes(response.read())
    return local


def main():
    cache = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE / "fuentes-originales"
    cache.mkdir(parents=True, exist_ok=True)
    letters = idiomas.letters()
    covered = {}
    for name, (path, axes) in SOURCES.items():
        font = instancer.instantiateVariableFont(TTFont(source(cache, path)), axes)
        cmap = font.getBestCmap()
        chars = {c for c in wanted(name, letters) if ord(c) in cmap}
        options = subset.Options()
        options.layout_features = []  # ImGui's stb_truetype draws glyph by glyph
        options.hinting = False
        options.name_IDs = ["*"]
        options.notdef_outline = True
        options.recalc_timestamp = False
        subsetter = subset.Subsetter(options)
        subsetter.populate(unicodes=sorted(ord(c) for c in chars))
        subsetter.subset(font)
        # The same letters give the same file: the date of the source font stays (no "modified" of today).
        font.recalcTimestamp = False
        out = io.BytesIO()
        font.save(out)
        (HERE / name).write_bytes(out.getvalue())
        covered[name] = chars
        print(f"{name}: {len(chars)} letras, {len(out.getvalue()) // 1024} KB")
    base = cache / GONTSERRAT_SOURCE
    if base.exists():
        base_cmap = TTFont(base).getBestCmap()
        merged = {chr(c) for c in base_cmap} | set().union(*covered.values())
        missing = [c for c in letters if c not in merged and not c.isspace()]
        print("letras de las traducciones sin fuente:", "".join(missing) if missing else "ninguna")
    else:
        print(f"(copia {GONTSERRAT_SOURCE} en {cache} para revisar que ninguna letra quede sin fuente)")


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    main()
