# SPDX-License-Identifier: GPL-3.0+
# -*- coding: utf-8 -*-
"""Forja SX2 (2026-11-14): las revisiones rápidas que corre GitHub antes de compilar (build.yml), y que se pueden correr en la PC.

Salen de la auditoría de ese día (informes/AUDITORIA_INGENIERIA_2026-11-14.md del laboratorio): errores que hasta ahora se veían recién en
la PS4 o en una compilación fallida, atrapados en segundos y sin dependencias (solo Python 3).

  textos   cada texto del código en los 12 idiomas, con los mismos %s/%u (ps4/vm/assets/lang/idiomas.py)
  letras   cada letra de las traducciones de ruso, árabe, chino, japonés y coreano está en su fuente (assets/fonts/*.ttf), y las de
           los idiomas latinos dentro de lo que cubre Gontserrat (latín básico, Latin-1 y Latin Extendido-A) o de lo que ya usa el español
  perfiles los perfiles de fábrica (assets/perfiles/*.ini): sin comentarios al final de una línea (el lector de PCSX2 no los acepta),
           sin claves repetidas fuera de las listas, claves de [Forja] que el emulador lee, números donde van números
  extern   cada variable compartida (g_forja_*, g_orbis_*) declarada con "extern" en un archivo tiene el mismo tipo que donde se define
           (si no coincide, compila y enlaza igual, y falla en la PS4; la 134 no enlazó por una de estas)

Uso: python3 tools/ci/forja_revisiones.py [textos|letras|perfiles|extern ...]   (sin nada: todas). Sale con 1 si alguna falla."""
import re
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VM = ROOT / "ps4" / "vm"
LANG = VM / "assets" / "lang"
try:
    sys.stdout.reconfigure(encoding="utf-8")
except AttributeError:
    pass


def textos():
    r = subprocess.run([sys.executable, str(LANG / "idiomas.py")], cwd=str(LANG), capture_output=True, text=True, encoding="utf-8")
    malos = [l for l in r.stdout.splitlines() if re.search(r"[1-9]\d* (faltan|con problemas)", l)]
    for l in malos[:12]:
        print("   " + l)
    if r.returncode != 0 and not malos:
        print("   " + (r.stdout + r.stderr).strip().splitlines()[-1] if (r.stdout + r.stderr).strip() else "   idiomas.py falló")
    return r.returncode == 0


def cmap(path):
    """Los caracteres de una fuente TrueType/OpenType (tabla cmap, formatos 4 y 12), sin librerías."""
    data = path.read_bytes()
    num = struct.unpack(">H", data[4:6])[0]
    tables = {}
    for i in range(num):
        tag, _, off, length = struct.unpack(">4sIII", data[12 + 16 * i:28 + 16 * i])
        tables[tag] = (off, length)
    base = tables[b"cmap"][0]
    count = struct.unpack(">H", data[base + 2:base + 4])[0]
    chars = set()
    for i in range(count):
        _, _, off = struct.unpack(">HHI", data[base + 4 + 8 * i:base + 12 + 8 * i])
        sub = base + off
        fmt = struct.unpack(">H", data[sub:sub + 2])[0]
        if fmt == 4:
            segx2 = struct.unpack(">H", data[sub + 6:sub + 8])[0]
            ends = struct.unpack(f">{segx2 // 2}H", data[sub + 14:sub + 14 + segx2])
            starts = struct.unpack(f">{segx2 // 2}H", data[sub + 16 + segx2:sub + 16 + 2 * segx2])
            deltas = struct.unpack(f">{segx2 // 2}h", data[sub + 16 + 2 * segx2:sub + 16 + 3 * segx2])
            ro_at = sub + 16 + 3 * segx2
            ros = struct.unpack(f">{segx2 // 2}H", data[ro_at:ro_at + segx2])
            for k, (s, e) in enumerate(zip(starts, ends)):
                for c in range(s, e + 1):
                    if c == 0xFFFF:
                        continue
                    if ros[k] == 0:
                        g = (c + deltas[k]) & 0xFFFF
                    else:
                        at = ro_at + 2 * k + ros[k] + 2 * (c - s)
                        g = struct.unpack(">H", data[at:at + 2])[0]
                        g = (g + deltas[k]) & 0xFFFF if g else 0
                    if g:
                        chars.add(c)
        elif fmt == 12:
            groups = struct.unpack(">I", data[sub + 12:sub + 16])[0]
            for j in range(groups):
                s, e, g = struct.unpack(">III", data[sub + 16 + 12 * j:sub + 28 + 12 * j])
                chars.update(range(s, e + 1))
    return chars


def letras():
    fuentes = {"ru": "Forja-Cirilico.ttf", "ar": "NotoSansArabic-Forja.ttf", "ja": "NotoSansJP-Forja.ttf", "ko": "NotoSansKR-Forja.ttf",
               "zh": "NotoSansSC-Forja.ttf"}
    # Lo de Gontserrat (la fuente latina del sistema, no está en el repositorio): latín básico, Latin-1, Latin Extendido-A y la
    # puntuación de uso común.
    latin = set(range(0x20, 0x7F)) | set(range(0xA0, 0x180)) | {0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026,
                                                               0x20AC, 0x2192, 0x00B7}
    # Y lo que usa el español (el idioma base, que ya se ve en la PS4: por ejemplo el botón □).
    espanol = {ord(c) for l in (LANG / "en.lng").read_text(encoding="utf-8").splitlines() if l.startswith("es:") for c in l[3:]}
    ok = True
    for lng in sorted(LANG.glob("*.lng")):
        code = lng.stem
        textos_tr = [l[3:].strip() for l in lng.read_text(encoding="utf-8").splitlines() if l.startswith("tr:")]
        tiene = latin | espanol
        if code in fuentes:
            tiene |= cmap(VM / "assets" / "fonts" / fuentes[code])
        faltan = sorted({c for t in textos_tr for c in t if ord(c) > 0x7E and ord(c) not in tiene})
        if faltan:
            ok = False
            print(f"   {code}: letras que su fuente no tiene: {' '.join(faltan[:40])}"
                  + (" (rehacer las fuentes: ps4/vm/assets/fonts/fuentes.py, o cambiar la palabra)" if code in fuentes else ""))
    return ok


def claves_forja():
    """Las claves de [Forja] que el emulador lee o escribe (ForjaGameProfile.h, main.cpp y los demás)."""
    claves = set()
    for p in [ROOT / "pcsx2" / "ForjaGameProfile.h"] + list(VM.glob("*.cpp")) + list((ROOT / "pcsx2").glob("*.cpp")):
        t = p.read_text(encoding="utf-8", errors="replace")
        claves |= set(re.findall(r'"Forja",\s*"([A-Za-z0-9]+)"', t))
        if p.name == "ForjaGameProfile.h":
            claves |= set(re.findall(r'read_int\("([A-Za-z0-9]+)"', t))
            claves |= set(re.findall(r'\{"([A-Z][A-Za-z0-9]+)",\s*&v\.', t))
    return claves


def perfiles():
    conocidas = claves_forja()
    texto_libre = {"Nucleos", "Huellas", "GSQuitar", "VU1Quitar"}  # valores que no son un número
    listas = {"Patches", "Cheats"}  # secciones donde una clave repetida es una lista
    ok = True
    for ini in sorted((VM / "assets" / "perfiles").glob("*.ini")):
        seccion, vistas = None, {}
        for n, linea in enumerate(ini.read_text(encoding="utf-8").splitlines(), 1):
            t = linea.strip()
            if not t or t[0] in ";#":
                continue
            donde = f"   {ini.name}:{n}:"
            if t.startswith("["):
                if not t.endswith("]"):
                    print(donde, "sección mal cerrada:", t)
                    ok = False
                seccion = t.strip("[]")
                vistas = {}
                continue
            if "=" not in t or seccion is None:
                print(donde, "línea que no es 'clave = valor':", t[:60])
                ok = False
                continue
            clave, valor = (x.strip() for x in t.split("=", 1))
            if re.search(r"\s[;#]", valor):
                print(donde, f"comentario al final de la línea (el lector de PCSX2 lo toma como parte del valor): {t[:60]}")
                ok = False
            if clave in vistas and seccion not in listas:
                print(donde, f"'{clave}' repetida en [{seccion}] (antes en la línea {vistas[clave]})")
                ok = False
            vistas.setdefault(clave, n)
            if seccion == "Forja":
                if clave not in conocidas:
                    print(donde, f"[Forja] {clave}: el emulador no lee esa clave (¿mal escrita?)")
                    ok = False
                elif clave not in texto_libre and not re.fullmatch(r"-?\d+", valor):
                    print(donde, f"[Forja] {clave} = {valor}: tiene que ser un número")
                    ok = False
    return ok


def _tipo(t):
    t = re.sub(r"\s+", " ", t.replace("static ", "").strip())
    for a, b in (("std::uint32_t", "u32"), ("uint32_t", "u32"), ("std::uint64_t", "u64"), ("uint64_t", "u64"),
                 ("std::int32_t", "s32"), ("int32_t", "s32"), ("std::int64_t", "s64"), ("int64_t", "s64"),
                 ("std::uint8_t", "u8"), ("uint8_t", "u8"), ("std::size_t", "size_t"), ("unsigned int", "unsigned")):
        t = re.sub(r"\b" + re.escape(a) + r"\b", b, t)
    return t.replace(" <", "<").replace("< ", "<").replace(" >", ">")


def extern():
    archivos = [p for d in ("pcsx2", "ps4") for p in (ROOT / d).rglob("*")
                if p.suffix in (".cpp", ".h", ".inl") and "deps" not in p.parts and "prefix" not in p.parts]
    decl = re.compile(r"^\s*extern\s+(?!\"C\")([\w:<>,\s]+?)\s+((?:g_(?:forja|orbis)_\w+\s*(?:\[[^\]]*\])?\s*,\s*)*"
                      r"g_(?:forja|orbis)_\w+\s*(?:\[[^\]]*\])?)\s*;")
    defi = re.compile(r"^(?:static\s+)?([\w:<>,]+(?:\s+[\w:<>,]+)*?)\s+((?:g_(?:forja|orbis)_\w+\s*(?:\[[^\]]*\])?\s*"
                      r"(?:[{=][^,;]*)?\s*,\s*)*g_(?:forja|orbis)_\w+)\s*(?:\[[^\]]*\])?\s*(?:\{|=|;)")
    externs, defs = {}, {}
    for p in archivos:
        rel = p.relative_to(ROOT).as_posix()
        for linea in p.read_text(encoding="utf-8", errors="replace").splitlines():
            m = decl.match(linea)
            if m:
                for nombre in re.findall(r"g_(?:forja|orbis)_\w+", m.group(2)):
                    externs.setdefault(nombre, set()).add((_tipo(m.group(1)), rel))
                continue
            if linea[:1] in (" ", "\t") or "extern" in linea or linea.lstrip().startswith(("//", "return", "#")) or "(" in linea.split("=")[0]:
                continue
            m = defi.match(linea)
            if m:
                for nombre in re.findall(r"g_(?:forja|orbis)_\w+", m.group(2)):
                    defs.setdefault(nombre, set()).add((_tipo(m.group(1)), rel))
    ok = True
    for nombre, usos in sorted(externs.items()):
        definidas = defs.get(nombre)
        if not definidas:
            continue  # definida de otra forma (dentro de una macro o en una línea partida): no se juzga
        tipos_def = {t for t, _ in definidas}
        for t, rel in sorted(usos):
            if t not in tipos_def:
                ok = False
                print(f"   {nombre}: '{t}' en {rel}, pero se define como '{' / '.join(sorted(tipos_def))}' en "
                      f"{' / '.join(sorted({r for _, r in definidas}))}")
    return ok


REVISIONES = {"textos": (textos, "Textos en los 12 idiomas"), "letras": (letras, "Letras de las traducciones en sus fuentes"),
              "perfiles": (perfiles, "Perfiles de fábrica"), "extern": (extern, "Variables compartidas con el mismo tipo")}


def main():
    pedidas = sys.argv[1:] or list(REVISIONES)
    fallas = []
    for nombre in pedidas:
        funcion, titulo = REVISIONES[nombre]
        print(f"== {titulo}")
        bien = funcion()
        print("   bien" if bien else "   FALLA")
        if not bien:
            fallas.append(titulo)
    print("\nResultado: " + ("BIEN" if not fallas else "FALLA en " + ", ".join(fallas)))
    sys.exit(1 if fallas else 0)


if __name__ == "__main__":
    main()
