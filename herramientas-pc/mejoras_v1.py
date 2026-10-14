# -*- coding: utf-8 -*-
"""Mejoras de la versión 1 de Forja SX2 (se aplican una sola vez sobre el repo).
Uso: python mejoras_v1.py <carpeta del repo emulador>"""
import pathlib
import sys

raiz = pathlib.Path(sys.argv[1])


def rep(rel, pares):
    """Reemplazo exacto; se detiene si un texto no aparece (para no aplicar a ciegas)."""
    f = raiz / rel
    t = f.read_text(encoding="utf-8")
    for a, b in pares:
        if a not in t:
            raise SystemExit(f"No encontré en {rel}: {a!r}")
        t = t.replace(a, b)
    f.write_text(t, encoding="utf-8", newline="\n")
    print("ok", rel)


# 1) Biblioteca: también ISO comprimidas. El motor ya compila CsoFileReader (CSO y ZSO) y GzippedFileReader.
rep("ps4/vm/GameMenu.cpp", [
    ('if (extension != ".iso" && extension != ".bin" && extension != ".chd")',
     'if (extension != ".iso" && extension != ".bin" && extension != ".chd" && extension != ".cso" &&\n'
     '\t\t\t\t\t    extension != ".zso" && extension != ".gz")'),
    ('found %zu .iso/.bin/.chd files in', 'found %zu .iso/.bin/.chd/.cso/.zso/.gz files in'),
])

# 2) Aviso de actualizaciones y nombre en las peticiones web: nuestro repo, no el de Phoenixx.
rep("ps4/vm/ReleaseCheck.cpp", [
    ('"https://api.github.com/repos/Phoenixx1202/PS4SX2/releases/latest"',
     '"https://api.github.com/repos/josephguerra562-ux/forja-sx2/releases/latest"'),
    ('"PS4SX2/1.00 release-checker"', '"ForjaSX2/1.00 release-checker"'),
])
rep("ps4/vm/CoverDownload.cpp", [('"PS4SX2/1.00 cover-downloader"', '"ForjaSX2/1.00 cover-downloader"')])
rep("ps4/vm/main.cpp", [('"PS4SX2 Emu 1.00 CHD (AI-assisted) BUILD: PS4SX2-EMU-1.00-CHD-SMC-TELEMETRY\\n"',
                          '"Forja SX2 1.00 (base PS4SX2 1.00) BUILD: FORJASX2-1.00\\n"')])
rep("ps4/vm/PlatformVM.cpp", [('"PS4SX2-EMU-1.00-CHD-SMC (AI-assisted)"', '"FORJASX2-1.00 (base PS4SX2-EMU-1.00)"')])

# 3) param.sfo: permitir el séptimo núcleo del CPU (0x00010000, "cpu_mode_7cpu").
#    El modo PS4 Pro (0x00800000) queda como opción de compilación (-ModoPro), apagado:
#    nadie lo ha probado con este driver Vulkan y en la Pro el "Modo Boost" da la misma velocidad de CPU.
rep("ps4/vm/build-package.ps1", [
    ("  [string]$Eboot = (Join-Path $PSScriptRoot 'build/eboot.bin')\n)",
     "  [string]$Eboot = (Join-Path $PSScriptRoot 'build/eboot.bin'),\n"
     "  # Forja SX2: 0x00010000 = séptimo núcleo (65536 decimal); -ModoPro suma 0x00800000 (total 8454144) (modo PS4 Pro, sin probar).\n"
     "  [switch]$ModoPro\n)"),
    ("@('ATTRIBUTE', 'Integer', '4', '0')",
     "@('ATTRIBUTE', 'Integer', '4', $(if ($ModoPro) { '8454144' } else { '65536' }))"),
])
