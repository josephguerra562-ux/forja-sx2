# -*- coding: utf-8 -*-
"""Le da identidad propia al emulador (nombre, ID y carpeta en la PS4)
y traduce al español los textos que se ven en pantalla.
Uso: python espanol_y_nombre.py <carpeta del repo emulador>"""
import pathlib
import sys

raiz = pathlib.Path(sys.argv[1])

NOMBRE = "Forja SX2"          # nombre visible en la PS4 (provisional)
TITLE_ID = "FRJA00002"        # 4 letras + 5 números
CONTENT_ID = "IV0000-FRJA00002_00-FORJASX2EMULADOR"
CARPETA = "/data/ForjaSX2"    # carpeta propia: no choca con el PS4SX2 original

# Textos de GameMenu.cpp: (inglés exacto, español)
TEXTOS = [
    ('"Could not start disc detection."', '"No se pudo iniciar la detección del disco."'),
    ('"No disc detected"', '"No se detectó ningún disco"'),
    ('"No recent games"', '"No hay juegos recientes"'),
    ('"No favorite games"', '"No hay juegos favoritos"'),
    ('"Settings are coming soon"', '"Los ajustes llegan pronto"'),
    ('"No games found"', '"No se encontraron juegos"'),
    ('"Loading..."', '"Cargando..."'),
    ('"Checking physical DVD..."', '"Revisando el DVD..."'),
    ('"Disc detection failed"', '"Falló la detección del disco"'),
    ('"Reading the drive. You can continue browsing."', '"Leyendo la unidad. Puedes seguir navegando."'),
    ('" Cover unavailable. Triangle to retry."', '" Portada no disponible. Triángulo para reintentar."'),
    ('" | Dual-layer DVD | Press X to start."', '" | DVD doble capa | Pulsa X para iniciar."'),
    ('" | DVD | Press X to start."', '" | DVD | Pulsa X para iniciar."'),
    ('"Physical DVD unavailable"', '"DVD no disponible"'),
    ('" Downloading cover..."', '" Descargando portada..."'),
    ('"Removed from favorites."', '"Quitado de favoritos."'),
    ('"Added to favorites."', '"Agregado a favoritos."'),
    ('"Could not save favorites."', '"No se pudieron guardar los favoritos."'),
    ('"Physical disc support is being prepared."', '"El soporte de disco físico está en preparación."'),
    ('"Settings options will be added here."', '"Aquí irán las opciones de ajustes."'),
    ('"No game is available in this view."', '"No hay juegos en esta vista."'),
    ('"File unavailable. Press Triangle to refresh the library."', '"Archivo no disponible. Pulsa Triángulo para actualizar."'),
    ('"Cannot open game: "', '"No se puede abrir el juego: "'),
    ('"Starting..."', '"Iniciando..."'),
    ('"Connect a controller to choose a game."', '"Conecta un control para elegir un juego."'),
    ('"Finishing cover request..."', '"Terminando de bajar la portada..."'),
    ('"Finishing disc detection..."', '"Terminando de detectar el disco..."'),
    ('"Preparing emulator..."', '"Preparando el emulador..."'),
    ('"Emulator menu"', '"Menú del emulador"'),
    ('"GAME PAUSED"', '"JUEGO EN PAUSA"'),
    ('"Internal resolution:  < %ux >%s"', '"Resolución interna:  < %ux >%s"'),
    ('"  (native)"', '"  (nativa)"'),
    ('{"Stretch", "Auto", "4:3", "16:9", "10:7"}', '{"Estirar", "Auto", "4:3", "16:9", "10:7"}'),
    ('"Aspect ratio:  < %s >"', '"Relación de aspecto:  < %s >"'),
    ('"Performance overlay:  < %s >"', '"Contador de rendimiento:  < %s >"'),
    ('show_performance_overlay ? "On" : "Off"', 'show_performance_overlay ? "Sí" : "No"'),
    ('"Resume game"', '"Volver al juego"'),
    ('"Return to main menu"', '"Volver a la biblioteca"'),
    ('"Exit application"', '"Salir de la aplicación"'),
    ('"Continue emulation from the current frame"', '"Seguir jugando desde donde quedó"'),
    ('"Increase internal rendering quality for this session"', '"Más calidad de imagen en esta sesión (consume más)"'),
    ('"Choose the display shape used by the GS renderer"', '"Forma de la imagen en pantalla"'),
    ('"Show or hide FPS, speed and timing indicators"', '"Mostrar u ocultar FPS, velocidad y tiempos"'),
    ('"Close the VM and reopen the PS4SX2 game library"', '"Cerrar el juego y volver a la biblioteca"'),
    ('"Close the VM and exit PS4SX2"', f'"Cerrar el juego y salir de {NOMBRE}"'),
    ('"ADJUST" : (row == 2 ? "SELECT" : "TOGGLE")', '"AJUSTAR" : (row == 2 ? "ELEGIR" : "CAMBIAR")'),
    ('"Left/Right: change value   |   Resolution range: 1x-%ux"', '"Izq./Der.: cambiar valor   |   Resolución: 1x-%ux"'),
    ('"3x and above is experimental and intended for PS4 Pro only."', '"3x o más es experimental y solo para PS4 Pro."'),
    ('"Up/Down: select   Left/Right: change   X: confirm   Circle: resume"', '"Arriba/Abajo: elegir   Izq./Der.: cambiar   X: aceptar   Círculo: volver"'),
    ('"Confirm"', '"Aceptar"'),
    ('"Resume"', '"Volver"'),
    ('"PLAYSTATION 2 EMULATOR"', '"EMULADOR DE PLAYSTATION 2"'),
    ('"Update available: %s"', '"Actualización disponible: %s"'),
    ('{"Library", "Disc", "Recent", "Favorites", "Settings"}', '{"Biblioteca", "Disco", "Recientes", "Favoritos", "Ajustes"}'),
    ('"Physical PS2 DVD"', '"DVD de PS2"'),
    ('"Disc detected. Press X to start."', '"Disco detectado. Pulsa X para iniciar."'),
    ('"Insert a DVD, then press Triangle to refresh."', '"Mete un DVD y pulsa Triángulo para actualizar."'),
    ('"Start"', '"Jugar"'),
    ('"Remove favorite" : "Add favorite"', '"Quitar favorito" : "Agregar favorito"'),
    ('"Refresh disc" : "Refresh library"', '"Revisar disco" : "Actualizar biblioteca"'),
    ('"Exit"', '"Salir"'),
    ('"Close PS4SX2?"', f'"¿Cerrar {NOMBRE}?"'),
    ('"Are you sure you want to exit the application?"', '"¿Seguro que quieres salir de la aplicación?"'),
    ('"Exit PS4SX2"', f'"Salir de {NOMBRE}"'),
    ('"Cancel"', '"Cancelar"'),
    ('"Back"', '"Atrás"'),
]


def cambiar(ruta, pares):
    """Reemplaza textos exactos y avisa si alguno no aparece."""
    texto = ruta.read_text(encoding="utf-8")
    for viejo, nuevo in pares:
        if viejo not in texto:
            print(f"  AVISO: no encontré {viejo!r} en {ruta.name}")
        texto = texto.replace(viejo, nuevo)
    ruta.write_text(texto, encoding="utf-8", newline="\n")


# 1) Traducción del menú
cambiar(raiz / "ps4/vm/GameMenu.cpp", TEXTOS)

# 2) Carpeta propia en la PS4 (código de ps4/ y herramientas de PC)
for ruta in list((raiz / "ps4").rglob("*")) + list((raiz / "tools").rglob("*")):
    if ruta.is_file() and ruta.suffix in {".cpp", ".h", ".c", ".py", ".md", ".json"}:
        t = ruta.read_text(encoding="utf-8", errors="surrogateescape")
        if "/data/PS4SX2" in t:
            ruta.write_text(t.replace("/data/PS4SX2", CARPETA), encoding="utf-8",
                            errors="surrogateescape", newline="\n")
            print("  carpeta ->", ruta.relative_to(raiz))

# 3) Identidad del PKG de la app
cambiar(raiz / "ps4/vm/build-package.ps1", [
    ("$titleId = 'PCSX20007'", f"$titleId = '{TITLE_ID}'"),
    ("$contentId = 'IV0000-PCSX20007_00-PS4SX2EMU0000001'", f"$contentId = '{CONTENT_ID}'"),
    ("'PS4SX2 Emu'", f"'{NOMBRE}'"),
])
print("Listo.")
