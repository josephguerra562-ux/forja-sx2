# -*- coding: utf-8 -*-
"""Crea la animación de inicio propia de Forja SX2 (reemplaza la del logo de PlayStation 2).
Mismo formato que espera el emulador: 96 cuadros JPG 1280x720 a 30 fps + audio PCM s16le 48 kHz estéreo.
Uso: python intro_forja.py <carpeta ps4/vm/assets del repo>"""
import math
import pathlib
import struct
import sys

from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from logo_forja import logo  # noqa: E402  (reutiliza el mismo logo)

assets = pathlib.Path(sys.argv[1])
intro = assets / "intro"
CUADROS, FPS, ANCHO, ALTO = 96, 30, 1280, 720
TASA = 48000

# --- Imagen: negro -> logo aparece (cuadros 1-30) -> se queda -> se apaga (últimos 18) ---
marca = logo(900, 300)
for f in intro.glob("frame_*.jpg"):
    f.unlink()
for i in range(CUADROS):
    if i < 30:
        alfa = (i / 29) ** 1.6
    elif i >= CUADROS - 18:
        alfa = (CUADROS - 1 - i) / 17
    else:
        alfa = 1.0
    escala = 0.94 + 0.06 * min(1.0, i / 40)          # leve acercamiento al aparecer
    m = marca.resize((int(marca.width * escala), int(marca.height * escala)), Image.LANCZOS)
    capa = Image.new("RGBA", (ANCHO, ALTO), (0, 0, 0, 255))
    mm = m.copy()
    mm.putalpha(mm.getchannel("A").point(lambda v: int(v * alfa)))
    capa.alpha_composite(mm, ((ANCHO - m.width) // 2, (ALTO - m.height) // 2))
    capa.convert("RGB").save(intro / f"frame_{i + 1:03d}.jpg", quality=88)

# --- Sonido: campanita propia (dos notas con eco suave), 3,22 s como el original ---
muestras = 154624
notas = [(0.10, 659.25), (0.32, 987.77), (0.32, 1318.5)]   # Mi5, luego Si5 + Mi6
datos = bytearray()
for n in range(muestras):
    t = n / TASA
    v = 0.0
    for inicio, freq in notas:
        if t >= inicio:
            d = t - inicio
            v += math.sin(2 * math.pi * freq * d) * math.exp(-d * 2.2) * 0.22
            v += math.sin(2 * math.pi * freq * 2 * d) * math.exp(-d * 4.0) * 0.05   # brillo
    if t > 2.6:                                   # se apaga junto con la imagen
        v *= max(0.0, (3.22 - t) / 0.62)
    s = max(-32767, min(32767, int(v * 32767)))
    datos += struct.pack("<hh", s, s)
(intro / "audio_s16le_48k_stereo.pcm").write_bytes(bytes(datos))

(intro / "manifest.txt").write_text(
    f"frames={CUADROS}\nfps={FPS}\nwidth={ANCHO}\nheight={ALTO}\naudio_rate={TASA}\naudio_channels=2\naudio_bytes={len(datos)}\n",
    encoding="ascii", newline="\n")
print("intro lista:", CUADROS, "cuadros,", len(datos), "bytes de audio")
