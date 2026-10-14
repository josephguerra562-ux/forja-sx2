# -*- coding: utf-8 -*-
"""Analiza los registros de Forja SX2 bajados de la PS4 (/data/ForjaSX2/logs) y dice, con números,
cómo corrió cada sesión: FPS, velocidad, tirones y qué núcleo fue el cuello de botella.

Uso:  python analizar_logs.py "carpeta de logs"      (busca en subcarpetas también)
Salida: el informe en pantalla y en INFORME_RENDIMIENTO.md dentro de esa carpeta.

El emulador escribe cada 5 s una línea "Performance: ..." en PCSX2-vm-boot.log con:
PS2_Hz (cuadros de la PS2 por segundo), Speed_pct (velocidad: 100 = como una PS2 real),
Game_FPS (FPS del juego), Frame_max_ms (el peor cuadro del intervalo) y el % de uso de los hilos
EE (código del juego), GS (dibujo) y VU1 (3D)."""
import pathlib
import re
import statistics
import sys

CAMPO = re.compile(r"(\w+)=([-\d.]+)")
REFRESCO_MS = 1000 / 59.94          # un cuadro de PS2 NTSC
TIRON_MS = REFRESCO_MS * 2.5        # un cuadro que tarda 2,5 veces lo normal se nota como tirón


def leer(ruta):
    return ruta.read_text(encoding="utf-8", errors="replace").splitlines()


def analizar_sesion(ruta):
    """Devuelve un diccionario con los datos de una sesión (un PCSX2-vm-boot.log)."""
    s = {"archivo": ruta, "perf": [], "afinidad": [], "juego": None, "errores": [], "cache": 0,
         "ee_rate": None, "build": None}
    for linea in leer(ruta):
        if linea.startswith("Performance:"):
            d = {k: float(v) for k, v in CAMPO.findall(linea)}
            if d.get("PS2_Hz", 0) > 0:              # 0 = en pausa o cargando
                s["perf"].append(d)
        elif "thread affinity" in linea:
            s["afinidad"].append(linea.split("affinity:", 1)[1].strip())
        elif "BUILD:" in linea and s["build"] is None:
            s["build"] = linea.strip()
        elif "VM runtime EE cycle profile" in linea or linea.startswith("EE cycle rate:"):
            s["ee_rate"] = linea.strip()
        elif re.search(r"Game menu: image metadata .* serial=(\S+)", linea):
            s["juego"] = re.search(r"serial=(\S+)", linea).group(1)
        elif "pipeline cache saved" in linea:
            s["cache"] += 1
        elif any(p in linea for p in ("FAILED", "failed", "crash", "Crash", "fatal")):
            s["errores"].append(linea.strip()[:160])
    return s


def resumen(s):
    """Convierte una sesión en texto con veredicto."""
    p = s["perf"]
    out = [f"### {s['archivo'].parent.name}/{s['archivo'].name}"]
    if s["build"]:
        out.append(f"- Versión: `{s['build']}`")
    if s["juego"]:
        out.append(f"- Juego (serial): **{s['juego']}**")
    if s["ee_rate"]:
        out.append(f"- Ciclos del EE: `{s['ee_rate']}`")
    if not p:
        out.append("- **Sin datos de rendimiento**: el juego no llegó a correr (revisa los errores de abajo y crash.log).")
    else:
        vel = [d.get("Speed_pct", 0) for d in p]
        fps = [d.get("Game_FPS", d.get("PS2_Hz", 0)) for d in p]
        peor = [d.get("Frame_max_ms", 0) for d in p]
        ee = statistics.mean(d.get("EE_pct", 0) for d in p)
        gs = statistics.mean(d.get("GS_pct", 0) for d in p)
        vu = statistics.mean(d.get("VU1_pct", 0) for d in p)
        lentos = sum(1 for v in vel if v < 95)
        tirones = sum(1 for m in peor if m > TIRON_MS)
        minutos = len(p) * 5 / 60
        out += [
            f"- Tiempo jugado medido: {minutos:.1f} min ({len(p)} muestras de 5 s)",
            f"- Velocidad: media **{statistics.mean(vel):.0f} %**, mínima {min(vel):.0f} %"
            f" (100 % = igual que una PS2). Intervalos por debajo de 95 %: {lentos} de {len(p)}",
            f"- FPS del juego: media **{statistics.mean(fps):.1f}**, mínimo {min(fps):.1f}",
            f"- Peor cuadro: {max(peor):.0f} ms. Intervalos con tirón (cuadro > {TIRON_MS:.0f} ms): {tirones}",
            f"- Uso medio de los hilos: EE {ee:.0f} % · GS {gs:.0f} % · VU1 {vu:.0f} %",
        ]
        cuello = max((ee, "EE (código del juego)"), (gs, "GS (dibujo)"), (vu, "VU1 (3D)"))
        if statistics.mean(vel) >= 98 and tirones == 0:
            veredicto = "Corre a velocidad completa y sin tirones. Se puede probar subir la resolución."
        elif cuello[0] >= 90:
            consejo = {
                "EE (código del juego)": "bajar la Velocidad del CPU de la PS2 (L3+R3) a 60 % o 50 %; si tiene parche de 60 FPS, usar la versión de 30.",
                "GS (dibujo)": "bajar la resolución a 1x; si ya está en 1x, probar nucleos.txt con GS en otro núcleo.",
                "VU1 (3D)": "quitar el parche de 60 FPS; el 3D casi no tiene otros ajustes.",
            }[cuello[1]]
            veredicto = f"Lento por **{cuello[1]}** ({cuello[0]:.0f} %). Probar: {consejo}"
        elif tirones:
            veredicto = ("Velocidad buena pero con tirones sueltos: normalmente es la compilación de efectos "
                         "la primera vez; deben bajar en la segunda sesión (caché guardada).")
        else:
            veredicto = "Ningún hilo llega al 90 % pero no alcanza 100 %: probar subir la Velocidad del CPU de la PS2."
        out.append(f"- **Veredicto:** {veredicto}")
    fallos = [a for a in s["afinidad"] if "sce_result=0x0" not in a and "left to scheduler" not in a]
    if s["afinidad"]:
        out.append(f"- Reparto de núcleos: {len(s['afinidad'])} hilos fijados"
                   + (f", **{len(fallos)} rechazados por la PS4**: {fallos[:3]}" if fallos else ", todos aceptados por la PS4."))
    if s["cache"]:
        out.append(f"- Caché de efectos guardada en segundo plano {s['cache']} veces.")
    if s["errores"]:
        out.append("- Líneas de error (primeras 5):")
        out += [f"  - `{e}`" for e in s["errores"][:5]]
    return "\n".join(out)


def main():
    sys.stdout.reconfigure(encoding="utf-8")   # acentos bien en la consola de Windows
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    carpeta = pathlib.Path(sys.argv[1])
    sesiones = sorted(carpeta.rglob("PCSX2-vm-boot.log"), key=lambda r: r.stat().st_mtime)
    if not sesiones:
        raise SystemExit(f"No encontré PCSX2-vm-boot.log en {carpeta}")
    partes = ["# Informe de rendimiento de Forja SX2", ""]
    for ruta in sesiones:
        partes += [resumen(analizar_sesion(ruta)), ""]
    crash = list(carpeta.rglob("crash.log"))
    if crash:
        partes += ["## crash.log encontrado", "```", "\n".join(leer(crash[0])[-15:]), "```"]
    texto = "\n".join(partes)
    print(texto)
    (carpeta / "INFORME_RENDIMIENTO.md").write_text(texto, encoding="utf-8")


if __name__ == "__main__":
    main()
