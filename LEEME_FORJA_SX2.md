# Forja SX2: guía de uso en la PS4

**Comunidad:** únete al [Discord de Forja SX2](https://discord.gg/YVCQF5XFFd): novedades, ayuda para instalar, juegos probados y el foro
**reportar-errores** para avisar de un problema.

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
  `/data/pkg/ForjaSX2-Emulador-v<versión>-<compilación>.pkg` (por ejemplo `ForjaSX2-Emulador-v1.03-58.pkg`; hasta la 57 se
  llamaba `ForjaSX2-compilacion-N.pkg`), lo comprueba con el `SHA256SUMS.txt` de la versión y, si no coincide, lo borra.
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
- **En el juego:** OPTIONS = Start, clic en el touchpad = Select, **L3+R3 = menú del emulador**. Desde la 53, en
  Ajustes > **Botón del menú del emulador** eliges L3+R3, la **mitad derecha del panel táctil** (la mitad izquierda sigue
  siendo el Select de la PS2, que algunos juegos usan) o los dos.
- **Dos jugadores (desde la 43):** prende el segundo control. La PS4 pregunta "¿Quién usa este control?": elige otro
  usuario o **Usuario nuevo > Jugar**, que es el usuario temporal de la PS4 y no queda guardado. En un segundo Forja lo
  conecta al puerto 2 de la PS2, sin tocar nada más. Si ese usuario cierra sesión, el puerto 2 queda libre para el siguiente.
  El menú L3+R3 lo maneja solo el jugador 1. (Tres o cuatro jugadores, con el multitap, todavía no.)
- **El menú L3+R3 va en 6 pestañas** (desde la 35, como el menú rápido de Dolphin; la 6.ª, No tocar, desde la 53). **L1 y R1** cambian de pestaña,
  Arriba/Abajo eligen dentro de ella e Izq./Der. cambian el valor:
  - **Partida:** volver al juego, guardar estado, cargar estado, reiniciar el juego y **cambiar disco** (desde la 44, para
    juegos de varios discos: ponlos en roms/ como "Juego (Disco 1).iso", "Juego (Disco 2).iso"; ←/→ elige y X lo cambia
    cuando el juego lo pida).
  - **Imagen:** resolución interna, relación de aspecto, **nitidez** (desde la 44), generación de cuadros y contador de FPS.
    **Nitidez:** Suave (como antes), Nítida (bilineal nítido, sin costo), CAS y CAS fuerte (el realce de AMD, de la familia
    del FSR que usa Dolphin para PS4; cuesta muy poco) y Píxeles (sin filtro, estilo retro). Se guarda por juego.
  - **Partida** (desde la 58 también): **Avance rápido** (X lo prende o lo apaga: el juego corre lo más rápido que aguante
    la PS4, para cargas y diálogos; no se guarda) y, en Guardar/Cargar estado, **la fecha y la hora** en que se guardó cada
    ranura.
  - **Rendimiento:** velocidad del CPU de la PS2, **CPU automático**, modo video, núcleos y **Saltar cuadros** (de vuelta
    aquí desde la 75; de la 59 a la 74 estuvo en No tocar).
  - **Extras** (se llamaba Parches; desde la 44 trae más; desde la 53 los filtros están en **No tocar**):
    - **Parche:** los parches de ese juego, uno por uno. ←/→ elige, **X** lo pone o lo quita y se guarda para ese juego.
      Algunos piden reiniciar el juego para quitarse del todo.
      **Huellas de Forja** (desde la 55): al final de la lista pueden aparecer parches propios de Forja, marcados "(Forja)".
      Son efectos que Forja reconoce **por cómo se dibujan** (tipo de textura, mezcla, figura, dónde se dibujan), no por
      direcciones de memoria, así que sirven con cualquier copia o región del juego y con cualquier otro juego que dibuje
      igual. Forja mide cada huella mientras juegas y la ofrece en el juego donde aparece (o en los juegos que la huella
      nombra). Vienen apagadas: tú las prendes. Las huellas están en `huellas.ini` y llegan por el centro de ajustes, así
      que se pueden agregar nuevas sin otro PKG. La primera es **"Menos efectos: sombreado de personajes"**: en los Dragon
      Ball Z Budokai Tenkaichi 2, 3 y el 4 (mod) quita la capa de sombras y brillos de la ropa y los personajes; se ve con
      menos detalle, pero en las peleas del Tenkaichi 4 la velocidad pasó de ~45 % a ~85 % (de 13 a 25-30 cuadros por segundo).
      Desde la 56 hay otra, **"Menos efectos: pasadas de pantalla (experimental)"**: brillos y desenfoques que un juego
      dibuja en imágenes aparte; solo aparece donde pesa mucho. Y el parche automático del **motor Alchemy** (el de Crash
      Nitro Kart) sirve también con los juegos de ese motor que ya fijan su propio límite: 30 a 60 FPS (25 a 50 en PAL).
    - **Trucos** (desde la 58): como los parches, pero tuyos: pon un archivo de trucos de PCSX2 en
      `/data/ForjaSX2/cheats/<SERIAL>_<CRC>.pnach` (el menú te dice el nombre exacto) y aquí los prendes uno por uno con X.
    - **Texturas HD:** No / Sí / Sí, precargadas. El pack va por FTP en `/data/ForjaSX2/textures/<SERIAL>/replacements`
      (como en PCSX2). Precargar evita tirones pero usa más memoria: con packs enormes, mejor "Sí".
    - **Ajustes recomendados:** de dónde vienen los ajustes de fábrica de ese juego: del centro de ajustes (GitHub, con
      su fecha), del PKG, o ninguno. **X** los busca otra vez. Al empezar cada juego Forja baja de su GitHub el perfil y
      los parches recomendados de ese juego (desde la 54, cada vez que empieza el juego; antes, una vez al día), así las mejoras
      llegan sin un PKG nuevo. Lo que llega se usa desde la siguiente vez que abras ese juego. `vm-sin-centro` lo apaga.
      Desde la 1.06 cada versión baja sus propios ajustes: la versión publicada, los de su rama `centro-v1.06` (o los
      que traía su compilación, si esa rama todavía no existe); el canal de pruebas, los más nuevos.
  - **No tocar** (desde la 53): los experimentos con los que medimos y mejoramos los juegos, aparte de los ajustes para que
    nadie los confunda con mejoras. Si no sabes qué hace algo, déjalo como está.
    - **Filtro VU1 (experimental):** un laboratorio de efectos para cualquier juego. Muestra los programas del VU1 que más
      tiempo gastaron en los últimos segundos; **X** apaga el elegido: lo que dibuja deja de verse y su tiempo se ahorra.
      Si desaparece algo importante (o todo el 3D), X lo vuelve a poner. Lo apagado se guarda para ese juego. Sirve para
      buscar qué efecto, sombra o fondo lejano cuesta más en cada juego.
    - **Filtro GS (experimental, desde la 51):** lo mismo, pero para los juegos que frena el **GS** (el dibujo), como Dragon
      Ball Z Budokai Tenkaichi. Junta los dibujos del juego en grupos (dónde dibujan, con qué textura, si son
      transparentes) y muestra los 8 que más tiempo del GS gastaron en los últimos segundos, con su % y cuántos dibujos por
      segundo son. **X** apaga el elegido: no se dibuja y su tiempo se ahorra. Si la imagen se rompe o falta algo importante,
      X lo devuelve. Se guarda para ese juego (`GSQuitar` en su perfil). Ojo: un grupo es "ese tipo de dibujo en ese lugar de
      la memoria", así que en otro escenario puede tocar otra cosa; si algo falta en otra pelea, vuelve aquí y devuélvelo.
      Desde la 52: lo que se dibuja en las dos imágenes que la tele muestra por turnos es un solo grupo ("En la pantalla";
      en la 51 salía dos veces, una por imagen, y quitar una sola lo quitaba un cuadro sí y otro no); un grupo quitado se
      queda en su lugar de la lista con el % que tenía; y **Cuadrado** devuelve todos de una vez (también en el Filtro VU1).
      Desde la 54: cada grupo también dice el formato y el tamaño de su textura ("textura 03300 (256x256, 256 colores)"),
      porque un juego puede poner texturas distintas en el mismo lugar de la memoria (en God of War II un grupo quitaba el
      cuerpo de Kratos y cambiaba el color del Coloso); y **la lista no se reordena**: cada grupo se queda en su número
      mientras juegas (antes el 1 y el 2 podían cambiarse de lugar al volver a abrir el menú). Lo quitado con la 51-53 se
      devuelve solo al pasar a la 54.
  - **Sistema:** **Reportar este juego** (desde la 77), restablecer los ajustes del juego, volver a la biblioteca y salir.
    En "Reportar este juego" eliges con Izquierda/Derecha cómo te va (desde la 79: Pésimo, Mal, Regular, Bien, Perfecto)
    y X lo manda al canal de reportes del Discord de Forja: el resumen de la partida, los ajustes del juego, los medidores
    de los últimos minutos y el `crash.log` si hay. **Nada de tu cuenta de PSN.** Te da un número (por ejemplo `F-3A2C`)
    para mencionarlo en el Discord. Si no hay internet, el reporte queda en `/data/ForjaSX2/reportes/`. Uno por minuto.
- **Todo lo que cambies en L3+R3 se guarda solo para ese juego** (como las propiedades por juego de Dolphin), menos el
  contador de FPS. Se aplica al instante:
  - **Resolución interna** y **relación de aspecto**.
  - **Velocidad del CPU de la PS2:** de 27 % a 300 %. **Desde la 58 todo juego empieza al 36 % con CPU automático hasta 27 %** (de la 45 a la 57, 27 %; antes, 75 %): tiene más
    CPU de la PS2 donde la PS4 aguanta y baja solo en las escenas pesadas. Menos % = más
    FPS en la PS4, pero el juego puede perder cuadros. Si uno va en cámara lenta, súbelo aquí: queda guardado para ese juego.
    **Desde la 36, EXPERIMENTAL: 36 %, 31 % y 27 %** (marcados "exp."). Sirven para juegos que con 50 % siguen dibujando
    todos sus cuadros: con menos CPU el juego dibuja menos cuadros y el VU1 hace menos trabajo por segundo. Si el juego
    calcula su movimiento por tiempo, va a velocidad normal. Si se ve en cámara lenta, ese juego no sirve con esto: vuelve
    a 50 % o más. **God of War II va al 27 % de fábrica desde la 37** (medido: 98 % de velocidad media, contra ~70 % en
    las peleas al 100 %).
  - **CPU automático** (desde la 35): No / Hasta 75 % / Hasta 60 % / Hasta 50 % (desde la 36 también hasta 36, 31 y 27 %,
    experimental). Si el juego va por debajo del 92 % unos 3 segundos, baja un paso el CPU de la PS2. Cuando hay margen
    otra vez, vuelve a subir. Cada cambio da un tironcito, así que hay como mucho uno cada 6 s. La fila dice a qué velocidad
    va **ahora**. En God of War II viene apagado desde la 36: medido, las peleas pesadas iban igual al 50, 60, 75 y 100 %.
    Desde la 45, un juego sin ajuste propio usa el de la pestaña Ajustes (de fábrica, "Hasta 27 %"): si le subes el CPU y se
    pone lento en una escena pesada, baja solo, como mucho hasta el 27 %. Solo **baja**: si el CPU de la PS2 del juego ya
    está igual o más bajo que el límite, no tiene nada que hacer, y desde la 76 la fila lo dice ("No actúa…").
  - **Modo video:** No / **Auto** / 100 % / 75 % / 60 % / 50 % / 36 %. Es el CPU de la PS2 durante las cinemáticas.
    **Auto** busca solo el punto justo de cada video: desde la 51 mide cuántos cuadros salen, prueba un paso más arriba
    (y si no mejora, uno más abajo) y se queda con el que da más cuadros, entre 31 y 100 %. Vuelve a medir cada 40 s y
    recuerda dónde quedó para el siguiente video del juego. Arranca desde el último valor fijo que elegiste para el juego:
    si sabes que tu juego va mejor al 75 %, ponlo en 75 % y luego en Auto. Cada prueba da un tironcito en el video
    (el CPU de la PS2 se recompila). Con 100 % o 75 % fijos todo el video puede ir más lento si la PS4 no da
    abasto: en Tales of the Abyss, al 100 % salían 14 cuadros por segundo y al 60 %, 23. God of War II lo trae de fábrica. Desde la 48 también lo **sube**: con el
    juego al 27 %, los videos de God of War II se decodificaban con muy poco CPU y salían a 17-21 cuadros en vez de 25.
    **Desde la 76 también baja cuando le sobran imágenes:** si el video saca un 30 % más de las que necesita y va bajo el
    95 % de velocidad, prueba un paso abajo y se queda si no pierde imágenes y va 4 puntos más rápido (Resident Evil 4 sacaba
    el doble al 50 % y su video iba al 85 %).
  - **Saltar cuadros (GS)** (desde la 47; en Rendimiento desde la 75, ya sin "experimental"): No / Auto / 1 de 2. Para juegos en los que el GS de la PS4 no da
    abasto (Dragon Ball Z Budokai Tenkaichi 4: ~2.100 dibujos por cuadro en las peleas, 48 % de velocidad). El GS dibuja
    1 de cada 2 cuadros del juego y la tele sigue mostrando el último dibujado: el juego, su música y sus movimientos van
    a su velocidad, con la mitad de cuadros en pantalla. **Auto** (desde la 1.06) lo prende solo cuando el GS es el que
    frena unos 6 s (velocidad bajo 95 % con el GS al 80 % o más y el EE y el VU1 al 75 % o menos), sube de a un paso,
    baja cuando sobra o cuando el que frena es el EE o el VU1, y **no arranca mientras la generación de cuadros está
    generando**: las dos se estorban. Desde la 76 solo la espera si de verdad genera: prendida pero sin generar (un juego a
    50 cuadros en una tele de 60 Hz con "Sí", o en una pausa por lenta) ya no la frena; y si la generación de cuadros se
    apagó sola por lenta (desde la 76, a la segunda vez), el Auto trabaja como siempre. **Desde la 76, si en ~15 s prendido
    no salta ni un cuadro** (el juego dibuja su imagen aparte y la copia a la pantalla, como Shadow of the Colossus o God
    of War II), se apaga solo para ese juego y la fila lo dice. **Guardián** (desde la 1.06, en
    Auto y en "1 de 2"): los cuadros saltados tienen que quedar ocultos en la tele. Los revisa de a 3, un momento después
    (Dragon Ball muestra cada cuadro un cuadro más tarde); si la tele no los ocultó (una pantalla de victoria, un menú, el
    inicio de una pelea), Saltar cuadros se pausa 2 s, después 8 s y después 30 s. Después de una
    carga espera un segundo de escena pesada antes de saltar. Solo funciona en juegos que anuncian cada cuadro con los
    registros de pantalla, como Dragon Ball; en los demás dibuja todo. Puede dejar algún efecto a medias en juegos que
    dibujan algo en una imagen aparte y lo reusan: si pasa, ponlo en No. **"1 de 2" salta siempre**, aunque el GS no lo
    necesite: casi siempre conviene más Auto, que solo salta cuando hace falta. **En God of War II, mejor en No** (su
    perfil ya lo trae así): el que frena es el VU1, y además no salta ningún cuadro porque arma su imagen de otra forma.
    **Dragon Ball Z Budokai Tenkaichi 4 lo trae en Auto de fábrica desde la 75** (en la 73: 100 % de velocidad a 30
    cuadros en las peleas).
  - **Generación de cuadros:** No / Sí / **Fluida** (desde la 40). "Sí" dobla un juego estable a 30. "Fluida" rellena con un
    cuadro generado cada refresco en que la tele repetiría el anterior: es para los juegos PAL (50 cuadros en una tele de
    60 Hz: sin esto, 1 de cada 5 se queda el doble) y para los que alternan entre dos velocidades, como God of War II al 27 %
    (su perfil trae "Sí"). Cuesta trabajo a la gráfica: mejor en 1x o 2x. Si el juego va lento, se pausa sola; si sin ella
    el juego va más rápido, se apaga sola para ese juego (desde la 1.06 a la tercera vez, desde la 76 a la segunda; para
    traerla de vuelta, ponla en No y otra vez en Sí). En los videos, "Sí" pasa a "Fluida" (desde la 65),
    salvo en los juegos PAL con la tele a 60 Hz (desde la 1.06): sus 25 imágenes por segundo no caen parejas y la imagen
    generada se notaba, así que esos videos se ven con sus propias imágenes, como sin la generación de cuadros.
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
velocidad del CPU de la PS2 (desde la 45, de 27 % a 300 %; de fábrica 36 % desde la 58, antes 27 %), **CPU automático** (desde la 45; de fábrica
"Hasta 27 %"), resolución (de fábrica la nativa 1x en todas las PS4; antes una Pro con Boost empezaba en 2x), relación de aspecto, contador de FPS, **sensibilidad de la palanca** (133 % por defecto,
como el PCSX2 de PC; arregla que X caminara con la palanca a tope), cargas rápidas, saltar el logo,
lecturas de la GPU (readbacks), **Modo video automático** (sube la velocidad mientras se reproduce un video del juego) y
**Generación de cuadros (30 a 60 FPS)**, que es EXPERIMENTAL, viene apagada e inventa el cuadro intermedio en juegos que van estables a 30,
y desde la 53 **Botón del menú del emulador** (L3+R3, panel táctil derecho o los dos).
Lo que se guarde para un juego en L3+R3 manda sobre esto.

## 3c. Tus discos de PS2: jugar y copiarlos a la PS4 (desde la 41)
- Mete el DVD de PS2 en la PS4 y ve a **Disco** en la biblioteca. La unidad de la PS4 lee DVD, no CD: los juegos de PS2
  en CD (los de disco azul) no se pueden usar.
- **X:** jugar desde el disco. **□:** copiarlo a la PS4. La copia va a `/data/ForjaSX2/roms/<juego> (<serial>).iso` y
  tarda según la unidad (un DVD de 4 GB, unos 15-30 min). Puedes seguir en la biblioteca mientras copia; la línea de abajo
  dice el porcentaje, la velocidad y lo que falta. **□ otra vez la cancela** y no deja nada a medias. No saques el disco
  mientras copia.
- **Con la copia hecha, el disco es la llave:** cada vez que lo metas, X en Disco abre la copia de la PS4, que carga más
  rápido y sin ruido de la unidad. La copia también sale en la Biblioteca como cualquier juego.
- Si un sector del disco no se puede leer (un rayón), va en ceros y el aviso final dice cuántos. El registro anota el
  CRC-32 de la copia (`Disc copy: done`), para compararlo con redump.org.
- Basado en la copia de discos de PS5SX2 (swordpdf, GPL-3.0).

## 4. Perfiles por juego (como Dolphin)
Cada juego arma sus ajustes en tres capas. La de más abajo gana:
1. **Global:** la pestaña Ajustes y `nucleos.txt`.
2. **Perfil de fábrica:** viene dentro del PKG (`resources/perfiles/SERIAL.ini`), con lo que medimos en la consola. Hoy hay para:
   - Mega Man X Command Mission (`SLUS-20903`): clamp normal del VU1.
   - God of War II PAL (`SCES-54206`): atajo del VU1, CPU al 27 %, generación de cuadros en "Sí", Plan B y Saltar cuadros
     en No (desde 2026-10-21).
   - Tales of the Abyss (`SLUS-21386`): CPU al 100 %, cinemáticas al 60 % y núcleos normales.
   - Dragon Ball Z Budokai Tenkaichi 4, el mod (`SLUS-21978`): la huella "Menos efectos: sombreado de personajes",
     generación de cuadros en No y Saltar cuadros en Auto (desde la 75).
   - **Desde la 79**, medidos el 2026-10-23: Downhill Domination (`SLES-52202`: CPU 31 %, generación de cuadros en "Sí",
     que lo lleva de 25 a 50 en pantalla), Resident Evil 4 (`SLES-53702`: CPU 27 %, sin generación de cuadros), Need for
     Speed Most Wanted (`SLES-53559`: CPU 31 %, sin generación de cuadros, Saltar cuadros en Auto), Shadow of the Colossus
     (`SCES-53326`) y Guitar Hero III (`SLES-54974`): CPU 31 %, sin generación de cuadros ni Saltar cuadros. Los dos
     últimos todavía no van completos en la PS4.
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
; Saltar cuadros: 0 = no, 1 = Auto, 2 = 1 de 2
SaltarCuadros = 0
```
En un perfil de fábrica, `SaltarCuadrosConGuardian` (desde la 75) es el valor para las versiones con el guardián de
Saltar cuadros, y le gana a su `SaltarCuadros` (nunca a tu perfil). Las versiones anteriores a la 1.06 bajan los perfiles
de fábrica de main y solo leen `SaltarCuadros`: así Dragon Ball sigue en No para ellas, porque su Auto no tiene el guardián.

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
; Ciclos del EE: -6 a 3 (-6 27 %, -5 31 %, -4 36 %, -3 50 %, -2 60 %, -1 75 %). Por defecto, el de la pestaña Ajustes
; (desde la 45, -6). 0 = 100 %, como una PS2 real.
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
Sirve para pantalla ancha, 60 FPS o trucos. **Desde la 42 se ponen y se quitan en L3+R3 > Extras**, sin editar archivos.

**Cómo sabe Forja que un parche es de tu juego:** por el serial del disco y el CRC del programa del juego, que van en el
nombre del archivo (`SCES-54206_44A8A22A`). Otra región u otra versión tiene otro CRC, así que no carga parches ajenos.

**Parches de la comunidad de PCSX2 (desde la 42):** al empezar un juego, Forja baja de
[github.com/PCSX2/pcsx2_patches](https://github.com/PCSX2/pcsx2_patches) el archivo de ese juego exacto a
`patches/<SERIAL>_<CRC>_comunidad.pnach` (pantalla ancha, sin entrelazado, saltar videos...). Necesita internet y lo vuelve
a bajar a los 7 días. Ninguno se activa solo: los eliges en L3+R3 > Extras. No vienen dentro del PKG porque ese repositorio
no tiene licencia; los baja tu consola desde su fuente. `vm-sin-parches-comunidad` apaga la descarga.

**Más cuadros automáticos para el motor Alchemy (desde la 47).** Activision usó ese motor en muchos juegos de PS2 (el de
Crash Nitro Kart). Cuando un juego arranca, Forja busca en su memoria la configuración del motor ("[INSIGHT]") y, si el
juego no tiene ya un parche de FPS, le escribe uno en `patches/<SERIAL>_<CRC>_alchemy.pnach`: aparece en L3+R3 > Extras
como "50 FPS (motor Alchemy, experimental)" (60 en juegos NTSC), apagado. Se activa ahí y vale desde Partida > Reiniciar
juego. Si el juego va acelerado (su lógica cuenta cuadros), apágalo.

**Crash Nitro Kart (PAL), desde la 46: "50 FPS (experimental)".** El juego va a 25 cuadros por segundo en carrera. Su
motor (Alchemy) trae su configuración escrita dentro del juego; el parche le agrega "un cuadro por refresco" y "hasta 50
cuadros" antes de que arranque. Sin probar en la consola: se activa en L3+R3 > Extras y vale desde Partida > Reiniciar juego.

**Dragon Ball Z Budokai Tenkaichi 4 (mod de fans, SLUS-21978), desde la 46:** la base de datos de juegos lo reconoce y le
aplica los arreglos del Tenkaichi 3 (usa su mismo ejecutable). Sin ellos el GS leía de vuelta de la gráfica los dibujos
de paleta de los personajes en cada cuadro (33-45 ms por cuadro en las peleas) y los personajes salían con fallas. Las
notas del mod piden no activar parches de pantalla ancha: ya viene en 16:9.

**Parches que vienen en el PKG** (desde la 35, en `resources/parches/`). Para God of War II (PAL):
- **Menos efectos 1, 2 y 3 (Sony):** del emulador de Sony para God of War II USA, pasados a la PAL. Quitan algunos efectos
  a cambio de velocidad en las peleas. Con el 2 y el 3, el promedio pasó de ~66 % a ~87 %. Probados uno por uno en el
  Coloso (2026-10-18, CPU de la PS2 al 27 %): el 1 con el 2 dio 97 % aun a 3x, lo más rápido; desde la 44 esos dos vienen
  encendidos de fábrica (se apagan en L3+R3 > Extras).
- Saltar cinemáticas y la pantalla ancha: desde la 44 ya no van repetidos aquí; llegan con el archivo de la comunidad
  ("Skip Cutscenes", que viene activo de fábrica, y "Widescreen 16:9").
- **Menos efectos 4 (de God of War 1, experimental)** (desde la 43): Sony apaga esa función en God of War 1 y en God of
  War II existe la misma. En el Coloso no sumó velocidad en ninguna combinación: mejor apagado.

**Los parches de rendimiento de Sony, traducidos (desde la 43).** De los 39 scripts que Sony usa en su emulador de PS2 para
PS4, solo dos ganan velocidad: los de God of War y God of War II (EE. UU.), que apagan funciones de efectos. Los demás
arreglan fallos de su propio emulador (una puerta que no abre, música que se corta) que PCSX2 no tiene. Forja los escribe
como parches propios, "dinámicos": en vez de una dirección fija, buscan el principio exacto de cada función (8 instrucciones
sacadas de God of War II PAL) y la apagan donde la encuentren. Así sirven en otra región sin el disco a mano, y si la
función no es idéntica, no hacen nada. Vienen para:
- **God of War II USA** (`SCUS-97481_2F123FD8`): Menos efectos 1, 2, 3 y 4. Sin probar en esta versión. Su perfil de fábrica
  trae el CPU al 27 %, vuFlagHack y los Menos efectos 1 y 2, igual que la PAL.
- **God of War USA y PAL** (`SCUS-97399_D6385328`, `SCES-53133_FB0E6D72`): Menos efectos A y B (dos de las cuatro funciones
  de Sony; las otras dos no se pueden reconocer sin ese disco). Sin probar.
Se ponen en L3+R3 > Extras. Los dinámicos valen desde el próximo arranque del juego (Partida > Reiniciar juego).

**Arreglos de Sony (desde la 43):** de los demás scripts de Sony, los que cambian una instrucción en una dirección fija se
escribieron como parches condicionales ("Arreglo de Sony"): solo cambian algo si en esa dirección está la instrucción que
Sony esperaba. Corrigen fallos del emulador de Sony (choques, parpadeos, cuelgues), que en Forja quizá no pasen: ponlos
solo si el juego tiene ese problema. Vienen para 17 juegos (20 versiones): Ace Combat (Squadron Leader, The Belkan War,
The Unsung War, Zero), Oni, Soul Reaver 2, Legacy of Kain: Defiance, Ghost in the Shell: Stand Alone Complex, Tomb Raider
Legend y Anniversary, Raw Danger y otros. La lista completa, con todos los parches de la comunidad por juego, está en
`Catalogo de parches.html`.

Para activarlos a mano, en `gamesettings/SCES-54206_44A8A22A.ini`:
```ini
[Patches]
Enable = Skip Cutscenes
Enable = Menos efectos 1 (Sony)
Enable = Menos efectos 2 (Sony)
```
Si tu archivo tiene su propia lista `Enable`, esa lista manda sobre la de fábrica, así que agrega ahí también
"Skip Cutscenes". El menú L3+R3 > Extras escribe esa lista por ti. Un parche con el mismo nombre en `/data/ForjaSX2/patches` gana al que viene en el PKG.

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
Compara los FPS (L3+R3 > contador) en la MISMA escena con y sin el archivo. Usa los núcleos 0 a 5: desde la versión 1.05
Forja SX2 ya no pide el séptimo núcleo (el 6), que el sistema comparte; con él, en las PS4 con firmware 9.00 la app se
cerraba.

**Por juego:** la fila "Núcleos de la CPU" del menú L3+R3, o `Nucleos =` en `[Forja]`, cambia el reparto solo para ese juego.
Las opciones son "Global" (usa `nucleos.txt`), "Normal" (el reparto de arriba) y "Plan B". EE, VU1, GS y Audio se mueven al
momento (desde la 31 también el VU1 en escenas pesadas: antes se quedaba en su núcleo viejo y chocaba con el GS).
Algunos hilos secundarios solo cambian al volver a abrir el juego, así que para medir, reabre el juego.

## 8. Medir una prueba
**Desde la 1.06, dos ayudas para cualquiera:**
- **El medidor en pantalla marca entre corchetes lo que frena el juego:** `[GS 18.3 ms]`, `[VU1 ...]`, `[EE ...]`, o
  `[GPU ...]` en lugar de los Hz de la PS2. Sin corchetes, el juego va a velocidad completa. Qué hacer con cada uno:
  - **GS:** Saltar cuadros en Auto, una huella (Extras) o apagar la generación de cuadros. Bajar la resolución casi no
    ayuda: el GS gasta en preparar cada dibujo, no en pintarlo.
  - **VU1 o EE:** bajar la CPU de la PS2 (Rendimiento) o el CPU automático.
  - **GPU:** bajar la resolución.
- **`logs/resumen.txt`:** una página con cómo fue la partida: velocidad, cuadros por segundo, qué frenó, tirones por
  causa, cortes de sonido, memoria libre, generación de cuadros, Saltar cuadros y videos. Se reescribe cada minuto y al
  terminar el juego, y se guarda con cada sesión en `logs/sesiones/`. Para reportar un problema, es lo primero que hay
  que mandar. Desde la 76 un tirón es un cuadro de más de 50 ms **y** del doble de los de alrededor: un juego que va lento
  pero parejo ya no cuenta cada cuadro como tirón (Guitar Hero III tenía 1.250 en 7 minutos). Desde la 79 se compara con
  lo que dura un cuadro del juego, no un refresco de la PS2: con "1 de 2" o con el GS atrasado (Shadow of the Colossus) ya
  no cuenta de más. También dice cuántos cortes de sonido fueron en videos y **cuánto crece el heap por minuto** después del
  primero (si pasa de 10 MB por minuto, avisa: es una fuga, como la de Crash Nitro Kart). Desde la 77 dice también
  **cuánto más barato tenía que ser lo que frenó** para ir al 100 % (Shadow of the Colossus: "GS 41 %"): cuanto más alto,
  más lejos está ese juego de ir completo en la PS4.
- **Reportar este juego** (L3+R3 > Sistema, desde la 77): manda todo eso al Discord de Forja sin FTP.

Baja `/data/ForjaSX2/logs` por FTP y arrástrala encima de `Herramientas PC\ANALIZAR LOGS (arrastra la carpeta aqui).bat`.
Te dice los FPS, la velocidad, los tirones y qué parte frenó el juego, y crea `INFORME_RENDIMIENTO.md`.
Para una sesión anterior, usa su carpeta de `logs/sesiones/` (trae los mismos archivos, y `samples.log` si el muestreador estaba encendido).
`herramientas\linea_de_tiempo.py` saca la línea de tiempo cada 5 s; desde la 31 incluye las columnas de cortes de audio.

## 9. Si algo falla
- **Vuelve a la biblioteca al ver el logo de PS2:** casi siempre es la BIOS. Usa una de la misma región que el juego.
- **Error al arrancar:** prueba `vm-no-fastmem`.
- **Audio roto:** prueba `vm-audio-legacy-stretch`. Si lo nuevo de la 31 suena peor, prueba `vm-audio-cortes-antes` y avísame.
- **Un estado no carga:** pasa si es de otra compilación. Usa la memory card del juego.
- Para cualquier otra cosa: baja `/data/ForjaSX2/logs/` y `crash.log` por FTP y pásamelos, o abre un tema en el foro
  **reportar-errores** del [Discord](https://discord.gg/YVCQF5XFFd) con `logs/resumen.txt`.
