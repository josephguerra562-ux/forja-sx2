# Forja SX2: guía de uso en la PS4

## 1. Instalar (una sola vez)
1. Copia `IV0000-FRJA00002_00-FORJASX2EMULADOR.pkg` a una USB en exFAT.
2. En la PS4 con GoldHEN, entra a Ajustes > Ajustes de depuración > Juego > Instalador de paquetes e instálalo.
3. Abre Forja SX2 una vez y ciérralo. Así crea su carpeta `/data/ForjaSX2/`.

**Si tienes PS4 Pro:** activa Ajustes > Sistema > **Modo Boost**. El CPU sube de 1,6 a ~2,1 GHz (+33 %). Como el emulador está limitado por el CPU, es la mejora más grande que existe y no cuesta nada.

## 2. Carpetas en la PS4 (`/data/ForjaSX2/`)
| Carpeta | Qué va ahí |
|---|---|
| `bios/` | BIOS de PS2 (`.bin` + `.mec` + `.nvm`), de la misma región que el juego. También la encuentra en una USB, en `bios/` |
| `roms/` | Juegos: **.iso, .bin, .chd, .cso, .zso, .gz** (las comprimidas ocupan menos) |
| `covers/` | Portadas: mismo nombre que el juego + `.jpg` o `.png` (opcional) |
| `gamesettings/` | Ajustes POR JUEGO (sección 4) |
| `patches/` | Parches `.pnach` por juego (sección 5) |
| `logs/` | Registros: bajarlos por FTP cuando algo falle |

Los archivos se pasan por FTP (GoldHEN, puerto 2121, con FileZilla) o con PS4 Xplorer desde la USB.

## 3. Controles
- **Biblioteca:** X = jugar, Cuadrado = favorito, Triángulo = actualizar, Círculo = salir.
- **En el juego:** OPTIONS = Start, clic en el touchpad = Select, **L3+R3 = menú del emulador**.
- **Todo lo que cambies en L3+R3 se guarda solo para ese juego** (como las propiedades por juego de Dolphin), menos el
  contador de FPS. Se aplica al instante:
  - **Resolución interna** y **relación de aspecto**.
  - **Velocidad del CPU de la PS2:** de 50 % a 300 %; por defecto, 75 %. Menos % = más FPS en la PS4, pero el juego puede perder cuadros.
  - **Modo video:** No / 60 % / 50 %. Es el CPU de la PS2 durante las cinemáticas.
  - **Generación de cuadros:** Sí / No.
  - **Núcleos de la CPU:** Global / Normal / Plan B. Ver la sección 7.
  - **Restablecer este juego:** con X borra tus cambios de ese juego. Queda una copia `.bak`.
- Cada fila dice de dónde sale su valor ahora: **global**, **perfil de fábrica** o **tu perfil**.

## 3b. Pestaña Ajustes (para todos los juegos)
En la biblioteca, menú lateral > **Ajustes**. Arriba/Abajo eligen, Izq./Der. cambian, X pasa al siguiente valor y Círculo vuelve al menú.
Se guarda al momento en `/data/ForjaSX2/ajustes.ini` y se aplica al iniciar el siguiente juego:
velocidad del CPU de la PS2, resolución, relación de aspecto, contador de FPS, **sensibilidad de la palanca** (133 % por defecto,
como el PCSX2 de PC; arregla que X caminara con la palanca a tope), cargas rápidas, saltar el logo,
lecturas de la GPU (readbacks), **Modo video automático** (sube la velocidad mientras se reproduce un video del juego) y
**Generación de cuadros (30 a 60 FPS)**, que es EXPERIMENTAL, viene apagada e inventa el cuadro intermedio en juegos que van estables a 30.
Lo que se guarde para un juego en L3+R3 manda sobre esto.

## 4. Perfiles por juego (como Dolphin)
Cada juego arma sus ajustes en tres capas. La de más abajo gana:
1. **Global:** la pestaña Ajustes y `nucleos.txt`.
2. **Perfil de fábrica:** viene dentro del PKG (`resources/perfiles/SERIAL.ini`), con lo que medimos en la consola. Hoy hay para:
   - Mega Man X Command Mission (`SLUS-20903`): clamp normal del VU1.
   - God of War II PAL (`SCES-54206`): atajo del VU1 y Plan B.
   - Tales of the Abyss (`SLUS-21386`): CPU al 100 %, cinemáticas al 60 % y núcleos normales.
3. **Tu perfil:** `gamesettings/SERIAL_CRC.ini`. Lo escribe el menú L3+R3, y también se puede escribir a mano.

Lo que diga un perfil **también le gana a la base de datos de PCSX2 (GameIndex), clave por clave**. Ya no hace falta
`EnableGameFixes = false`, que apagaba todas sus correcciones de golpe. Sigue funcionando, si ya lo tenías.
Las correcciones gráficas del GameIndex (`gsHWFixes`) siguen aplicándose siempre.

Claves propias de Forja, en la sección `[Forja]`:
```ini
[Forja]
; Modo video: 1 = sí, 0 = no
VideoMode = 1
; CPU de la PS2 durante las cinemáticas: -3 (50 %), -2 (60 %), -1 (75 %), 0 (100 %)
VideoEECycleRate = -2
; Generación de cuadros: 1 = sí, 0 = no
FrameGen = 0
; Núcleos de este juego, con la misma forma que nucleos.txt en una línea ("global" = usar nucleos.txt)
Nucleos = EE=1 VU1=2 GS=3 AUDIO=5 AYUDANTES=4
```

### Archivo de tu perfil (`gamesettings/`)
El nombre del archivo es `SERIAL_CRC.ini`. Para sacarlo de cualquier ISO:
```
python herramientas\crc_juego.py "ruta\del\juego.iso"
```
Mega Man X Command Mission (nuestras ISO):
- versión de 30 FPS: `SLUS-20903_0EA58EED.ini`
- versión de 60 FPS: `SLUS-20903_12E1728B.ini`

Ejemplo de contenido (son los nombres de ajustes del PCSX2 de PC; solo pon lo que quieras cambiar).
Los comentarios van **en su propia línea** empezando con `;`, nunca al final de un valor:
```ini
[EmuCore/Speedhacks]
; Ciclos del EE: -3 a 3. Por defecto -1 (75 %). 0 = 100 %, como una PS2 real.
; Negativo = menos trabajo para la PS4 (más FPS, pero el juego puede perder cuadros).
; Positivo = más trabajo (solo para juegos que van lentos "por dentro", p. ej. con parche de 60 FPS).
EECycleRate = 0
; 0 a 3. Sube la velocidad, pero el juego puede ir "en cámara lenta".
EECycleSkip = 0
; Cargas más rápidas.
fastCDVD = true
```
En la carpeta `Forja SX2 - Para la PS4` del Escritorio ya están los .ini de Mega Man con las opciones apagadas, listas para probar.
Lo que diga este archivo **manda sobre** los interruptores generales, solo para ese juego.

## 5. Parches (`patches/`)
Archivo `SERIAL_CRC.pnach` (mismo nombre que el .ini pero con `.pnach`), en formato de PCSX2.
Sirve para pantalla ancha, 60 FPS o trucos.

## 6. Interruptores generales (archivos vacíos en `/data/ForjaSX2/`)
Afectan a TODOS los juegos. Si el archivo existe, la opción se activa. Reinicia la app después de ponerlo o quitarlo.
- `vm-ee-cycle-normal`, `vm-ee-cycle-underclock`, `vm-ee-cycle-skip`: velocidad del CPU de la PS2
- `vm-fast-cdvd`: cargas rápidas · `vm-no-fast-boot`: mostrar el logo de la BIOS
- `vm-no-audio`, `vm-audio-legacy-stretch`: audio
- `vm-no-fastmem`: si se cuelga al arrancar · `vm-verbose-log`: registro detallado para mandarme
- `vm-no-thread-affinity`: dejar que la PS4 reparta los núcleos sola
- `vm-sync-flip`: modo antiguo de mandar la imagen a la tele (más lento; solo si ves parpadeos o imagen cortada)

## 7. Reparto de núcleos (`nucleos.txt`): para experimentar
El CPU de la PS4 son **dos grupos de 4 núcleos** (0-3 y 4-7). Cada grupo tiene su propia memoria rápida (caché L2) y pasar datos de un grupo al otro es lento.
El emulador tiene 4 trabajadores pesados: **EE** (el juego), **VU1** (el 3D), **GS** (el dibujo) y **Audio**.

Por defecto: `EE=2, VU1=3` (grupo 1) y `GS=4, Audio=5` (grupo 2).
Para probar otro reparto, crea `/data/ForjaSX2/nucleos.txt`:
```
# Plan B: EE, VU1 y GS en el mismo grupo
EE=1
VU1=2
GS=3
Audio=5
```
Los hilos secundarios (lectura del disco, registro, guardado de la caché) van a los núcleos 0-1, que están libres;
con `AYUDANTES=0` los dejas en uno solo.
Compara los FPS (L3+R3 > contador) en la MISMA escena con y sin el archivo. El núcleo 6 también se puede usar
(Forja SX2 pide el séptimo núcleo a la PS4), pero el sistema lo comparte.

**Por juego:** la fila "Núcleos de la CPU" del menú L3+R3, o `Nucleos =` en `[Forja]`, cambia el reparto solo para ese juego.
Las opciones son "Global" (usa `nucleos.txt`), "Normal" (el reparto de arriba) y "Plan B". EE, VU1, GS y Audio se mueven al
momento. Algunos hilos secundarios solo cambian al volver a abrir el juego, así que para medir, reabre el juego.

## 8. Medir una prueba
Baja `/data/ForjaSX2/logs` por FTP y arrástrala encima de `Herramientas PC\ANALIZAR LOGS (arrastra la carpeta aqui).bat`.
Te dice los FPS, la velocidad, los tirones y qué parte frenó el juego, y crea `INFORME_RENDIMIENTO.md`.

## 9. Si algo falla
- **Vuelve a la biblioteca al ver el logo de PS2:** casi siempre es la BIOS. Usa una de la misma región que el juego.
- **Error al arrancar:** prueba `vm-no-fastmem`.
- **Audio roto:** prueba `vm-audio-legacy-stretch`.
- Para cualquier otra cosa: baja `/data/ForjaSX2/logs/` y `crash.log` por FTP y pásamelos.
