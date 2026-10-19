# SPDX-License-Identifier: GPL-3.0+
# Forja SX2 (2026-10-19): the language files' helper. The emulator's texts are the Spanish ones in the code, inside T("..."),
# TF("...") (a printf format) and N_("...") (a text kept in a table), plus the names and descriptions of assets/huellas.ini.
# Each <code>.lng has, per text, an "es:" line (that Spanish) and a "tr:" line (its translation); \n is a new line.
#
#   python idiomas.py                    checks every .lng: missing texts, %s/%u that differ from the Spanish, spaces at the ends
#   python idiomas.py --plantilla        writes plantilla.lng, every text with an empty translation, for a new language
#   python idiomas.py --importar F.txt   writes the .lng files from F.txt: blocks of "es: <Spanish>" and "<code>: <translation>"
#   python idiomas.py --letras           prints the letters the translations use (assets/fonts/fuentes.py reads them)
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
VM = HERE.parent.parent
LANGUAGES = {"en": "English", "pt": "Português (Brasil)", "fr": "Français", "de": "Deutsch", "it": "Italiano", "ru": "Русский",
             "zh": "简体中文", "ja": "日本語", "ko": "한국어", "ar": "العربية", "tr": "Türkçe", "pl": "Polski"}
# The picker's names, in ForjaLang.h, are drawn too.
NATIVE_NAMES = ["Español"] + list(LANGUAGES.values())
CALL = re.compile(r'\b(T|TF|N_)\(\s*"((?:[^"\\\n]|\\.)*)"\s*\)')
FIRST_FILES = ["GameMenu.cpp", "main.cpp", "UIVM.cpp", "ReleaseCheck.cpp", "DiscCopy.cpp", "CoverDownload.cpp", "PhysicalDisc.cpp"]


def c_unescape(text):
    out, i = [], 0
    while i < len(text):
        if text[i] == "\\" and i + 1 < len(text):
            nxt = text[i + 1]
            out.append({"n": "\n", "t": "\t", '"': '"', "\\": "\\", "'": "'"}.get(nxt, "\\" + nxt))
            i += 2
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def lng_escape(text):
    return text.replace("\\", "\\\\").replace("\n", "\\n")


def lng_unescape(text):
    out, i = [], 0
    while i < len(text):
        if text[i] == "\\" and i + 1 < len(text) and text[i + 1] in "n\\":
            out.append("\n" if text[i + 1] == "n" else "\\")
            i += 2
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def conversions(text):
    return re.findall(r"%[-+ #0-9.hlzjt]*[diouxXeEfFgGcsp%]", text)


def source_texts():
    """The Spanish texts in order, with whether one is a printf format and where it is."""
    files = [VM / name for name in FIRST_FILES]
    files += sorted(p for p in VM.glob("*.cpp") if p.name not in FIRST_FILES)
    files += sorted(VM.glob("*.h"))
    texts, formats, where = [], set(), {}
    for path in files:
        if path.name == "ForjaLang.h":
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8").split("\n"), 1):
            if line.lstrip().startswith("//"):
                continue
            for match in CALL.finditer(line):
                text = c_unescape(match.group(2))
                if match.group(1) == "TF":
                    formats.add(text)
                if text not in where:
                    where[text] = f"{path.name}:{number}"
                    texts.append(text)
    rules = HERE.parent / "huellas.ini"
    if rules.exists():
        for line in rules.read_text(encoding="utf-8").split("\n"):
            m = re.match(r"\s*(nombre|descripcion)\s*=\s*(.+?)\s*$", line)
            if m and m.group(2) not in where:
                where[m.group(2)] = "huellas.ini"
                texts.append(m.group(2))
    return texts, formats, where


def read_lng(path):
    pairs, key = {}, None
    for line in path.read_text(encoding="utf-8").split("\n"):
        line = line.rstrip("\r")
        if line.startswith("es: "):
            key = lng_unescape(line[4:])
        elif line.startswith("tr: ") and key is not None:
            pairs[key] = lng_unescape(line[4:])
            key = None
    return pairs


def write_lng(code, texts, pairs):
    name = LANGUAGES[code]
    lines = [f"# Forja SX2 — {name} ({code}). Cada texto: \"es:\" el español del código, \"tr:\" su traducción. \\n es otra línea.",
             "# Each text: \"es:\" the Spanish in the code, \"tr:\" its translation. \\n is a new line. Check: python idiomas.py", ""]
    for text in texts:
        if text in pairs:
            lines += [f"es: {lng_escape(text)}", f"tr: {lng_escape(pairs[text])}", ""]
    (HERE / f"{code}.lng").write_text("\n".join(lines), encoding="utf-8", newline="\n")


def check():
    texts, formats, where = source_texts()
    print(f"{len(texts)} textos en el código ({len(formats)} formatos printf)")
    problems = 0
    for code in LANGUAGES:
        path = HERE / f"{code}.lng"
        pairs = read_lng(path) if path.exists() else {}
        missing = [t for t in texts if t not in pairs]
        extra = [t for t in pairs if t not in where]
        bad = []
        for text in texts:
            if text not in pairs:
                continue
            tr = pairs[text]
            if text in formats and conversions(text) != conversions(tr):
                bad.append(f"formato distinto: {text!r} -> {tr!r}")
            if (text[:1] == " ") != (tr[:1] == " ") or (text[-1:] == " ") != (tr[-1:] == " "):
                bad.append(f"espacios en los extremos: {text!r} -> {tr!r}")
            if not tr.strip():
                bad.append(f"vacío: {text!r}")
        print(f"{code}: {len(pairs)} traducidos, {len(missing)} faltan, {len(extra)} sobran, {len(bad)} con problemas")
        for t in missing[:20]:
            print(f"   falta ({where[t]}): {t!r}")
        for b in bad[:20]:
            print("   " + b)
        problems += len(missing) + len(bad)
    return problems


def import_master(master):
    texts, _, where = source_texts()
    # An editor may drop the spaces at a line's end: a text of the code that ends in spaces is found without them too.
    trimmed = {t.rstrip(" "): t for t in texts if t.endswith(" ")}
    blocks, current, tail = {}, None, ""
    for line in Path(master).read_text(encoding="utf-8").split("\n"):
        line = line.rstrip("\r")
        m = re.match(r"^([a-z]{2}): (.*)$", line)
        if not m:
            continue
        # Invisible direction marks are not drawn by the menus' fonts.
        code, value = m.group(1), re.sub("[​‎‏]", "", lng_unescape(m.group(2)))
        if code == "es":
            current, tail = value, ""
            if current not in where and current in trimmed:
                current = trimmed[current]
                tail = current[len(current.rstrip(" ")):]
            if current not in where:
                print("no está en el código:", repr(current))
            blocks.setdefault(current, {})
        elif current is not None and code in LANGUAGES:
            blocks[current][code] = value if value.endswith(" ") or not tail else value + tail
    for code in LANGUAGES:
        path = HERE / f"{code}.lng"
        pairs = read_lng(path) if path.exists() else {}
        for text, translations in blocks.items():
            if code in translations:
                pairs[text] = translations[code]
        write_lng(code, texts, pairs)
    print(f"importados {len(blocks)} textos")


def letters():
    chars = set("".join(NATIVE_NAMES))
    for code in LANGUAGES:
        path = HERE / f"{code}.lng"
        if path.exists():
            chars.update("".join(read_lng(path).values()))
    return "".join(sorted(c for c in chars if ord(c) > 0x7f))


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    if "--plantilla" in sys.argv:
        texts, _, _ = source_texts()
        lines = ["# Forja SX2 — plantilla: copia este archivo como <código>.lng y escribe cada tr:", ""]
        for t in texts:
            lines += [f"es: {lng_escape(t)}", "tr: ", ""]
        (HERE / "plantilla.lng.txt").write_text("\n".join(lines), encoding="utf-8", newline="\n")
        print(f"plantilla.lng.txt: {len(texts)} textos")
    elif "--importar" in sys.argv:
        import_master(sys.argv[sys.argv.index("--importar") + 1])
        sys.exit(1 if check() else 0)
    elif "--letras" in sys.argv:
        print(letters())
    else:
        sys.exit(1 if check() else 0)
