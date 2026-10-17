# Forja SX2: guía de uso en la PS4

## 1. Instalar (una sola vez)
1. Copia `IV0000-FRJA00002_00-FORJASX2EMULADOR.pkg` a una USB en exFAT.
2. En la PS4 con GoldHEN, entra a Ajustes > Ajustes de depuración > Juego > Instalador de paquetes e instálalo.
3. Abre Forja SX2 una vez y ciérralo. Así crea su carpeta `/data/ForjaSX2/`.

### Actualizar a una compilación nueva
- Desde la compilación 31, cada PKG trae su número de versión (la 31 es `01.31`, la 32 es `01.32`). La nueva debería
  instalarse **encima** de la vieja con el mismo Instalador de paquetes, sin borrar nada. Si la PS4 dice que ya está
  instalada, borra la app vieja y vuelve a instalar: tus juegos, ajustes y perfiles de `/data/ForjaSX2/` no se borran.
- **Actualizador dentro de la app:** cuando salga una compilación más nueva que la tuya **en tu canal**, la biblioteca lo
  pregunta una vez (**Descargar** / **Ahora no**); **OPTIONS** lo vuelve a preguntar. Baja el PKG a
  `/data/pkg/ForjaSX2-compilacion-N.pkg`, lo comprueba con el `SHA256SUMS.txt` de la versión y, si no coincide, lo borra.
  Arriba a la derecha se ve el avance. Cuando diga "lista en /data/pkg", cierra Forja y en GoldHEN abre el
  **Package Installer**: el PKG aparece ahí.
- **Dos canales de actualización:**
  - **Estable (el normal, para todos):** solo avisa de las compilaciones que ya probé en mi PS4 Pro y aprobé. En
    [Releases](https://github.com/josephguerra562-ux/forja-sx2/releases) son las que **no** dicen "Pre-release"; la
    más nueva dice "Latest". Mientras no haya ninguna aprobada, este canal no avisa de nada.
  - **De pruebas:** avisa de **cada** compilación apenas sale de GitHub, aunque nadie la haya probado todavía. Puede
    venir rota. Para entrar, crea el archivo vacío `/data/ForjaSX2/canal-pruebas` (sección 6); para volver al estable,
    bórralo. Se lee al abrir la app.
  - En `logs/`, la línea `Release check: channel=estable` (o `channel=pruebas`) dice en qué canal estás.
  - La 31 todavía avisa de todas las compilaciones. Los canales empiezan en la compilación siguiente.
- **Cómo apruebo una compilación:** en GitHub, Actions > **Aprobar compilación** > Run workflow, y el número. Eso le
  quita la marca de pre-release y desde ese momento el canal estable la ofrece.

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
| `logs/sesiones/` | Desde la 31: **las últimas 10 sesiones de juego**, una carpeta por sesión (`FECHA-HORA_SERIAL`, hora UTC = Ecuador + 5 h). Una prueba ya no borra la anterior |
| `sstates/` | Los estados guardados con L3+R3 (sección 3) |

Los archivos se pasan por FTP (GoldHEN, puerto 2121, con FileZilla) o con PS4 Xplorer desde la USB.

## 3. Controles
- **Biblioteca:** X = jugar, Cuadrado = favorito, Triángulo = actualizar, Círculo = salir.
- **En el juego:** OPTIONS = Start, clic en el touchpad = Select, **L3+R3 = menú del emulador**.
- **El menú L3+R3 va en 4 pestañas** (desde la 35, como el menú rápido de Dolphin). **L1 y R1** cambian de pestaña,
  Arriba/Abajo eligen dentro de ella e Izq./Der. cambian el valor:
  - **Partida:** volver al juego, guardar estado, cargar estado y reiniciar el juego.
  - **Imagen:** resolución interna, relación de aspecto, generación de cuadros y contador de FPS.
  - **Rendimiento:** velocidad del CPU de la PS2, **CPU automático**, modo video y núcleos.
  - **Sistema:** restablecer los ajustes del juego, volver a la biblioteca y salir.
- **Todo lo que cambies en L3+R3 se guarda solo para ese juego** (como las propiedades por juego de Dolphin), menos el
  contador de FPS. Se aplica al instante:
  - **Resolución interna** y **relación de aspecto**.
  - **Velocidad del CPU de la PS2:** de 27 % a 300 %; por defecto, 75 %. Menos % = más FPS en la PS4, pero el juego puede perder cuadros.
    **Desde la 36, EXPERIMENTAL: 36 %, 31 % y 27 %** (marcados "exp."). Sirven para juegos que con 50 % siguen dibujando
    todos sus cuadros: con menos CPU el juego dibuja menos cuadros y el VU1 hace menos trabajo por segundo. Si el juego
    calcula su movimiento por tiempo, va a velocidad normal. Si se ve en cámara lenta, ese juego no sirve con esto: vuelve
    a 50 % o más. **God of War II va al 27 % de fábrica desde la 37** (medido: 98 % de velocidad media, contra ~70 % en
    las peleas al 100 %).
  - **CPU automático** (desde la 35): No / Hasta 75 % / Hasta 60 % / Hasta 50 % (desde la 36 también hasta 36, 31 y 27 %,
    experimental). Si el juego va por debajo del 92 % unos 3 segundos, baja un paso el CPU de la PS2. Cuando hay margen
    otra vez, vuelve a subir. Cada cambio da un tironcito, así que hay como mucho uno cada 6 s. La fila dice a qué velocidad
    va **ahora**. En God of War II viene apagado desde la 36: medido, las peleas pesadas iban igual al 50, 60, 75 y 100 %.
  - **Modo video:** No / 60 % / 50 %. Es el CPU de la PS2 durante las cinemáticas.
  - **Generación de cuadros:** No / Sí / **Fluida** (desde la 40). "Sí" dobla un juego estable a 30. "Fluida" rellena con un
    cuadro generado cada refresco en que la tele repetiría el anterior: es para los juegos PAL (50 cuadros en una tele de
    60 Hz: sin esto, 1 de cada 5 se queda el doble) y para los que alternan entre dos velocidades, como God of War II al 27 %,
    que la trae de fábrica. Cuesta trabajo a la gráfica: mejor en 1x o 2x.
- **Salida a 50 Hz para juegos PAL (desde la 37):** la PS4 manda la imagen a 60 Hz, y un juego PAL hace 50 cuadros por
  segundo. 50 en 60 no reparte parejo: 1 de cada 5 cuadros queda el doble en pantalla y se nota un tironcito 10 veces por
  segundo. Forja le pide 50 Hz a la PS4 al empezar un juego PAL, pero una PS4 Pro probada lo rechaza
  (`0x80290016`, modo no soportado) y se queda a 60 sin que se note nada. Para eso está la generación de cuadros "Fluida".
  - **Si la tele se queda en negro:** espera unos segundos; si no vuelve, cierra la app con el botón PS. La próxima vez
    Forja queda sola a 60 Hz (escribe `/data/ForjaSX2/vm-sin-50hz`). Para volver a probar, borra ese archivo.
  - El registro lo cuenta en las líneas `Output rate:` y `VideoOut: the TV now at ... Hz`.
  - **Núcleos de la CPU:** Global / Normal / Plan B. Ver la sección 7.
  - **Restablecer ajustes de este juego:** con X borra tus cambios de ese juego. Queda una copia `.bak`.
- **Estados guardados (desde la 31, EXPERIMENTAL):** las filas 2 a 4 del menú L3+R3.
  - **Guardar estado:** Izq./Der. eligen la ranura (1 a 5) y X guarda ahí el momento exacto del juego (tarda ~1 s).
  - **Cargar estado:** X vuelve a ese momento y el menú se cierra. Si la ranura está vacía, la fila lo dice.
  - **Reiniciar juego:** X empieza el juego de nuevo sin volver a la biblioteca. Lo que no hayas guardado se pierde.
  - Sirven para probar (repetir la misma pelea con otro ajuste, por ejemplo). Sigue guardando en la **memory card** del
    juego: un estado de una compilación puede no cargar en la siguiente.
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

**Parches que vienen en el PKG** (desde la 35, en `resources/parches/`). Por ahora son para God of War II (PAL):
- **Saltar cinemáticas** (de la comunidad): con cualquier botón de acción. Viene activo de fábrica.
- **Menos efectos 1, 2 y 3 (Sony):** del emulador de Sony para God of War II USA, pasados a la PAL. Quitan algunos efectos
  a cambio de velocidad en las peleas. Con el 2 y el 3, el promedio pasó de ~66 % a ~87 %. El 1 todavía está en prueba.
- **Pantalla ancha 16:9** (de la comunidad): muestra más escena, así que cuesta algo más.

Para activarlos, en `gamesettings/SCES-54206_44A8A22A.ini`:
```ini
[Patches]
Enable = Saltar cinemáticas
Enable = Menos efectos 2 (Sony)
Enable = Menos efectos 3 (Sony)
```
Si tu archivo tiene su propia sección `[Patches]`, esa lista manda sobre la de fábrica, así que agrega ahí también
"Saltar cinemáticas". Un parche con el mismo nombre en `/data/ForjaSX2/patches` gana al que viene en el PKG.

## 6. Interruptores generales (archivos vacíos en `/data/ForjaSX2/`)
Afectan a TODOS los juegos. Si el archivo existe, la opción se activa. Reinicia la app después de ponerlo o quitarlo.
- `vm-ee-cycle-normal`, `vm-ee-cycle-underclock`, `vm-ee-cycle-skip`: velocidad del CPU de la PS2
- `vm-fast-cdvd`: cargas rápidas · `vm-no-fast-boot`: mostrar el logo de la BIOS
- `vm-no-audio`, `vm-audio-legacy-stretch`: audio
- `vm-sin-50hz`: la tele siempre a su frecuencia normal, también con juegos PAL (ver "Salida a 50 Hz")
- **Medidor del VU1** (desde la 35, siempre activo): cada 5 s el registro anota en `VU1 programs:` los 8 programas del VU1
  que más tiempo toman. Así se ve qué efecto de un juego cuesta más, para quitarlo juego por juego.
- `vm-spu2-mismo-hilo`: deja la mezcla del sonido en el hilo del juego (EE), como hasta la 31. Desde la 32 la mezcla
  (voces, reverberación y salida) corre en el núcleo del audio y el EE queda más libre; el registro lo cuenta en la línea
  `Audio SPU2 thread:` (qué parte mezcló el hilo nuevo y cuánto esperó el EE). Si un juego suena raro o se cuelga, prueba con este archivo.
  Desde la 35 las lecturas que no tocan las voces no esperan la mezcla, y desde la 36 tampoco las del volumen y la envolvente
  de cada voz (God of War II las leía ~11 600 veces por segundo, y cada una paraba al EE). Solo esperan la posición de
  lectura de una voz, su inicio de bucle y ENDX. El registro las cuenta en `reads_free_per_second` y `reads_mixed_per_second`.
- `vm-audio-cortes-antes`: el audio como en la compilación 30, para comparar. Desde la 31, cuando el juego va lento y el
  sonido se queda sin datos, la cola de audio crece sola (hasta 200 ms, y baja otra vez tras 15 s sin cortes) y cada corte
  se apaga y vuelve con un fundido corto en vez de un chasquido. El registro lo cuenta en `PS4 audio queue: ... underruns=`
  (cortes), `soft_cuts=` (suavizados) y `cushion=` (tamaño de la cola, 100 % = normal).
- `vm-no-fastmem`: si se cuelga al arrancar · `vm-verbose-log`: registro detallado para mandarme
- `vm-no-thread-affinity`: dejar que la PS4 reparta los núcleos sola
- `vm-cpu-flip`: manda la imagen a la tele desde el CPU, como antes de la compilación 30. Desde la 30, el cuadro se encola en
  la GPU y el CPU ya no espera (como hace DolphinPS4). Si la app se colgó en los primeros segundos con el flip por GPU,
  Forja crea este archivo sola al abrir la siguiente vez. Bórralo para volver a probar.
- `vm-sync-flip`: modo antiguo de mandar la imagen a la tele (más lento; solo si ves parpadeos o imagen cortada)
- `canal-pruebas`: el actualizador avisa de **todas** las compilaciones nuevas, también de las que nadie probó (canal de
  pruebas, sección 1). Sin este archivo solo avisa de las aprobadas (canal estable).

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
momento (desde la 31 también el VU1 en escenas pesadas: antes se quedaba en su núcleo viejo y chocaba con el GS).
Algunos hilos secundarios solo cambian al volver a abrir el juego, así que para medir, reabre el juego.

## 8. Medir una prueba
Baja `/data/ForjaSX2/logs` por FTP y arrástrala encima de `Herramientas PC\ANALIZAR LOGS (arrastra la carpeta aqui).bat`.
Te dice los FPS, la velocidad, los tirones y qué parte frenó el juego, y crea `INFORME_RENDIMIENTO.md`.
Para una sesión anterior, usa su carpeta de `logs/sesiones/` (trae los mismos archivos, y `samples.log` si el muestreador estaba encendido).
`herramientas\linea_de_tiempo.py` saca la línea de tiempo cada 5 s; desde la 31 incluye las columnas de cortes de audio.

## 9. Si algo falla
- **Vuelve a la biblioteca al ver el logo de PS2:** casi siempre es la BIOS. Usa una de la misma región que el juego.
- **Error al arrancar:** prueba `vm-no-fastmem`.
- **Audio roto:** prueba `vm-audio-legacy-stretch`. Si lo nuevo de la 31 suena peor, prueba `vm-audio-cortes-antes` y avísame.
- **Un estado no carga:** pasa si es de otra compilación. Usa la memory card del juego.
- Para cualquier otra cosa: baja `/data/ForjaSX2/logs/` y `crash.log` por FTP y pásamelos.
