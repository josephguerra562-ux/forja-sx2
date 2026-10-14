# Auditoría de rendimiento de Forja SX2 (14-oct-2026)

## 1. Expectativa realista: Forja SX2 frente a un PS2 Classic en PKG

| | PS2 Classic (emulador de Sony en PKG) | Forja SX2 (PCSX2 portado) |
|---|---|---|
| Quién lo hizo | Sony, con la documentación completa de la PS2 | Comunidad (PCSX2 → PS5SX2 → PS4SX2 → nosotros) |
| Ajuste | Afinado juego por juego (config + lua) | Genérico, más los arreglos de la base de datos de PCSX2 |
| Velocidad en una PS4 normal | Muy buena en los juegos que Sony probó | Juegos ligeros: completa. Juegos pesados (GTA SA): ~50 % |
| Compatibilidad | Fuera de la lista de Sony es una lotería | La de PCSX2 (más del 98 % en PC); en PS4 la limita el CPU |
| Cambiar de juego | Un PKG por juego | Cualquier ISO, sin convertir |
| Podemos mejorarlo | No (código cerrado) | Sí (código abierto) |

**Conclusión honesta:** hoy Forja SX2 **no** va a correr todo igual o mejor que un PS2 Classic. El emulador de Sony es más liviano porque está hecho a la medida. Donde sí puede igualarlo:
- en juegos ligeros a 30 FPS, como MMXCM;
- en una **PS4 Pro con Modo Boost** (+33 % de CPU).

Donde lo supera:
- en juegos que el emulador de Sony no corre o corre mal;
- en que no hay que convertir nada.

Para acercarnos más hacen falta mediciones reales en tu consola. Por eso esta versión trae herramientas para medir.

## 2. Hallazgos y acciones

| # | Área | Hallazgo | Acción |
|---|---|---|---|
| 1 | Núcleo del emulador | Las optimizaciones de fondo de PS5SX2 (bucles de espera, búfer del GIF, memoria de texturas, DMA instantáneo) **ya están**. Lo que trae de más iHaiDeeZ es solo de PS5 | No hace falta mezclar núcleos |
| 2 | Compilación | Release -O3, `-march=btver2` (Jaguar exacto), sin AVX2/BMI2 | Correcto, sin cambios |
| 3 | Caché de efectos (shaders) | Solo se guardaba al salir desde el menú o al pausar. **Cerrar con el botón PS la perdía** y los tirones volvían en cada sesión | ✅ Se guarda sola cada 30 s, en segundo plano y con escritura segura |
| 4 | Hilos secundarios | El lector del disco (que también descomprime CSO/CHD), el registro y la telemetría podían caer en los núcleos del juego. Los núcleos 0-1 quedaban libres | ✅ Fijados a los núcleos 0-1 |
| 5 | Recompilador del EE | `printf` + `fflush` al disco cada 100 bloques nuevos, toda la partida | ✅ Solo al arrancar |
| 6 | Registro | Ya escribe en un hilo aparte; la espera de grabación no toca el juego | Correcto (falsa alarma revisada) |
| 7 | Perfiles de depuración | Vienen apagados por defecto | Correcto |
| 8 | Séptimo núcleo | El original no lo pedía | ✅ Lo pide (versión anterior) |
| 9 | Modo Pro (PKG) | Nadie lo ha medido con este driver | Apagado. En la Pro, usar el **Modo Boost** |
| 10 | Ajustes gráficos | Los estándar de PCSX2. La GPU no es el límite | Sin cambios; la resolución sigue en 1x por defecto |
| 11 | Velocidad del CPU de la PS2 | Por defecto 75 % (alivia el CPU) | ✅ Ajustable en L3+R3 y guardada por juego |
| 12 | Reparto de núcleos | EE 2, VU1 3 / GS 4, Audio 5 (dos grupos de caché) | ✅ `nucleos.txt` para comparar en la consola |
| 13 | Mensajes de las unidades vectoriales | Limitados a 40 o solo ante errores | Correcto |

## 3. Plan de las primeras pruebas (en tu PS4)

Antes de empezar: si es Pro, activa el **Modo Boost**. Pasa la carpeta `ForjaSX2` a `/data/` y copia la ISO de MMXCM de 30 FPS a `roms/`.

**Prueba 1. ¿Arranca y cómo va? (10 min)**
1. Abre Forja SX2 y luego Mega Man (versión de 30 FPS).
2. Activa el contador con L3+R3. Juega la intro, camina un poco, pelea una batalla y mira una cinemática.
3. Anota: ¿hay sonido? ¿se ven bien los textos en español? ¿cuántos FPS hay en la batalla?
4. Sal con L3+R3 > "Volver a la biblioteca".

**Prueba 2. Caché de efectos (5 min).** Juega otra vez la misma parte. Los tirones de la primera vez deben bajar o desaparecer. Esta vez cierra con el **botón PS**.

**Prueba 3. Velocidad del CPU de la PS2.** En la misma batalla, prueba 50 %, 60 %, 75 % y 100 % desde L3+R3. Anota los FPS de cada una y si el juego se ve raro (animaciones lentas o cortadas).

**Prueba 4. Reparto de núcleos (opcional).** Crea `/data/ForjaSX2/nucleos.txt` con el Plan B de la guía y repite la batalla.

**Al terminar:**
1. Baja por FTP la carpeta `/data/ForjaSX2/logs`.
2. Arrástrala encima de `Herramientas PC\ANALIZAR LOGS (arrastra la carpeta aqui).bat`.
3. Mándame el `INFORME_RENDIMIENTO.md` que se crea, más tus notas. Con esos números decido la siguiente optimización.

## 4. Siguientes optimizaciones (dependen de lo que midamos)
- Si el EE va al 100 %: perfil del EE con `vm-profile-perf` para encontrar las funciones más pesadas de MMXCM, y ajustes o parches para ese juego.
- Si hay tirones al compilar efectos: precompilar los shaders más comunes al iniciar.
- Si el audio se corta: ajustar el búfer o pasar el audio al séptimo núcleo.
- Si la PS4 rechaza núcleos (lo dice el informe): ajustar el reparto.
