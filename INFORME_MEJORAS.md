# Informe: cómo convertir Forja SX2 en un emulador de PS2 decente para PS4

*14 de octubre de 2026. Basado en mediciones reales en la PS4 Pro de Joseph (Mega Man X Command Mission), en el código fuente de los tres ports existentes y en las fuentes que aparecen al final.*

---

## 0. Resumen en un minuto

- **El límite es el CPU de la PS4**, no la gráfica. Cada núcleo Jaguar es 6-8 veces más lento que el de una PC moderna, y PCSX2 pone casi todo el trabajo de la PS2 en 3 o 4 hilos.
- **Lo que hoy logramos con Mega Man:**
  - 3D a **30 FPS fijos**;
  - videos de 15 a **~25 FPS**;
  - la resolución se aplica desde Ajustes;
  - la palanca está arreglada.
- **Defectos que quedan:**
  - tirones de un cuadro al cambiar de escena (efectos que se compilan la primera vez);
  - videos todavía por debajo de 30;
  - estabilidad al volver a la biblioteca (el arreglo está en la versión 13 y falta probarlo).
- **Tu idea ("forzar 60 y que nunca baje de 30") tiene una forma real de hacerse:** la **generación de cuadros** (interpolación AMD FSR 3), que PS5SX2 ya implementó.
  - Muestra **60 FPS** en un juego que corre estable a 30.
  - No gasta CPU: usa la gráfica, que en la PS4 está al ~25 %.
  - **No hace que el juego corra más rápido:** que nunca baje de 30 depende de las mejoras de CPU de las secciones 4.1-4.5.
- **Plan:** 3 fases.
  - **A.** Arreglos rápidos y estabilidad (1-2 sesiones).
  - **B.** Generación de cuadros y precarga de efectos (3-5 sesiones).
  - **C.** Optimización profunda del CPU (audio, videos, perfiles automáticos).

---

## 1. Cómo está hoy (medido en tu PS4 Pro, Modo optimizado activado)

| Parte del juego | Antes (PS4SX2 original) | Ahora (Forja SX2 v12-13) | Qué lo frena |
|---|---|---|---|
| Exploración | 30 FPS | 30 FPS fijos | Nada |
| **Batallas** | Caídas a 19-21 FPS | **30 FPS fijos** | Se quitó el "clamp" lento del VU1 (3D) que la base de datos de PCSX2 impone a este juego |
| **Videos (intro)** | 55-72 % de velocidad (~15-17 FPS) | **~82-96 % (~25 FPS)** | Hilo EE al 100 %: el juego consulta al decodificador ~130 mil veces/s, más el audio y las transferencias |
| Cambio de escena → batalla | Tirón | Tirón de un cuadro (~50-70 ms) | Efectos gráficos nuevos que se compilan la primera vez |
| Resolución 4x | 30 FPS con caídas | Igual | +6 ms de espera por cuadro a la gráfica; en 2x-3x hay más margen |
| Volver a la biblioteca | A veces pantalla negra | Arreglado en la v13 (sin probar) | Cuenta de cuadros "fantasma" en la salida de video |

---

## 2. ¿Por qué el emulador de Sony (los PKG de PS2 Classics) va mejor?

1. **Está hecho a la medida.** Sony tiene la documentación completa de la PS2 y afina el emulador **juego por juego**. Cada PKG trae su propia configuración y sus parches (los archivos `config-emu-ps4.txt` y `.lua` que ya conoces).
2. **PCSX2 (nuestra base) es general:** quiere correr el 98 % de los juegos de forma fiel en una **PC**, donde sobra CPU. Esa fidelidad cuesta: por ejemplo, el "clamp" del VU1 que le quitamos a Mega Man.
3. **El resultado:** para un juego concreto, Sony siempre va a tener ventaja. Nosotros podemos acercarnos con **perfiles por juego**, con **optimizaciones específicas para el CPU Jaguar** y con trucos que Sony no tiene, como la generación de cuadros.

---

## 3. Defectos encontrados y su estado

| # | Defecto | Dónde | Estado |
|---|---|---|---|
| 1 | El PKG original no pedía el **séptimo núcleo** del CPU | param.sfo | ✅ Arreglado (v1) |
| 2 | La **caché de efectos** se perdía al cerrar con el botón PS (tirones en cada sesión) | VKShaderCache | ✅ Arreglado: se guarda cada 30 s en segundo plano (v5) |
| 3 | **Hilos secundarios** (lectura del disco, registros) en los núcleos del juego | ThreadAffinity | ✅ Movidos a los núcleos 0-1 (v5) |
| 4 | El recompilador del EE escribía al disco cada 100 bloques | iR5900 | ✅ Quitado (v5) |
| 5 | **Palanca:** X caminaba con la palanca a tope | Conversión DS4→DS2 | ✅ Sensibilidad del 133 %, como en PCSX2 de PC (v7) |
| 6 | La pestaña **Ajustes** no hacía nada | GameMenu | ✅ 9 opciones (v8-v11) |
| 7 | Los ajustes **no se aplicaban** al juego de esa misma sesión | main.cpp | ✅ Se releen al elegir el juego (v10) |
| 8 | **Videos** lentos (CPU del juego saturado) | EE/IPU | 🟡 Mejorado con el modo video automático (v11); falta más |
| 9 | **VU1** saturado en batallas (Mega Man) | Clamp del GameDB | ✅ Desactivado por juego (ini). Hacen falta perfiles para otros juegos |
| 10 | Cuenta de **cuadros pendientes** descuadrada (cierres fallidos, posible pantalla negra, un búfer fantasma) | VulkanVideoOut | ✅ Arreglado en la v13, **falta probar** |
| 11 | La imagen de fondo de carga nunca se cargaba | GameMenu | ✅ Arreglado (v13) |
| 12 | Hilos críticos con prioridad normal (700), igual que los secundarios | ThreadAffinity | 🟡 Opción lista (v12), **falta medir** |
| 13 | **Tirones al cambiar de escena** (primera compilación de efectos) | GS/Vulkan | ❌ Pendiente: precarga de efectos (4.3) |
| 14 | El **audio** de la PS2 se mezcla en el mismo núcleo que el juego | SPU2 | ❌ Pendiente: medir y separar (4.4) |
| 15 | Un juego a 30 FPS se ve a 30 aunque la gráfica tenga margen | Presentación | ❌ Pendiente: generación de cuadros (4.7) |

---

## 4. Plan de mejoras por componente

Cada punto dice: **qué es**, **el problema**, **la solución**, **esfuerzo**, **impacto** y **riesgo**.

### 4.1 CPU del juego (EE: Emotion Engine)
- **Problema:** es el hilo más cargado. En videos y escenas pesadas va al 100 %.
- **Ya hecho:**
  - modo video automático (EE al 50 % solo durante videos);
  - velocidad del CPU de la PS2 por juego.
- **Siguiente: atajo para las consultas a la IPU.** El reproductor de video de muchos juegos pregunta "¿ya terminaste?" decenas de miles de veces por segundo.
  - PS5SX2 ya tiene una regla para saltarse **bucles que solo leen memoria** ("wider wait-loop rule").
  - Extenderla a los bucles que leen los registros del decodificador de video ahorraría buena parte del ~50 % que hoy gasta el juego en los videos.
  - *Esfuerzo:* medio. *Impacto:* alto en videos (todos los juegos con FMV). *Riesgo:* medio (hay que validarlo juego por juego).

### 4.2 3D (VU1)
- **Problema:** en batallas con muchos efectos llega al ~92 %.
- **Hecho:** quitar el clamp "extra + signo" que el GameDB de PCSX2 fuerza en Mega Man. Fue la mejora más grande en batallas.
- **Siguiente: perfiles por juego.** Para cada juego probado se guarda qué clamp, velocidad del CPU y resolución le van mejor, como hace PS5SX2 con su botón "Recommended".
  - *Esfuerzo:* bajo por juego. *Impacto:* alto. *Riesgo:* bajo (se revierte por juego).

### 4.3 Dibujo (GS) y efectos
- **Problema 1:** **tirones la primera vez que aparece un efecto.** Vulkan tiene que "compilar" cada combinación de efecto (pipeline) y eso congela el cuadro.
  - **Solución:** **precarga de efectos.** Se guarda la lista de efectos que usó cada juego y se compilan todos **al arrancarlo**, durante la pantalla de carga. Dolphin hace lo mismo ("compilar shaders antes de empezar").
  - *Esfuerzo:* medio. *Impacto:* alto (quita los tirones de transición). *Riesgo:* bajo (solo alarga la carga).
- **Problema 2:** **4x cuesta ~6 ms por cuadro de espera a la gráfica.**
  - **Solución:** recomendar **2x-3x** en la PS4 Pro y **1x-2x** en la Slim, por perfil de consola (4.9).

### 4.4 Audio de la PS2 (SPU2)
- **Problema:** la mezcla de las 48 voces de la PS2 se hace en el **mismo núcleo que el juego**. En la intro, el bloque "IOP + audio" costó ~27 % de ese núcleo.
- **Solución en dos pasos:**
  1. **Medir** exactamente cuánto cuesta el audio (agregar un medidor, como hicimos con el video).
  2. Si pesa >10 %, **mezclar el audio en el hilo de audio** (núcleo 5, casi libre).
- **Riesgo:** alto. PCSX2 ya tuvo un modo "Async Mix" (el antiguo SPU2-X) que desincronizaba audio y video y colgaba algunos juegos. Hay que hacerlo con cuidado y como opción.
- *Esfuerzo:* alto. *Impacto:* medio-alto en **todos** los juegos.

### 4.5 Videos (IPU)
- **Hecho:** modo video automático.
- **Siguiente:** el atajo de 4.1 y revisar el costo de las transferencias (DMA) durante el video (~13 %).

### 4.6 Envío de imagen a la tele (VideoOut)
- **Hecho (v13):** la PS4 dice cuántos cuadros hay pendientes (antes se contaban a mano y se descuadraba).
- **Siguiente:** **ritmo parejo de cuadros.** Que un juego a 30 FPS muestre cada cuadro exactamente 2 veces a 60 Hz, sin alternar 1-3. Se puede medir con el registro de cuadros y corregir con la cola de VideoOut.

### 4.7 Tu idea: "forzar 60 FPS y que nunca baje de 30"
Hay **tres formas** de acercarse. Siendo honestos con cada una:

| Forma | Qué hace | Costo en la PS4 | ¿Sirve? |
|---|---|---|---|
| **Parches de 60 FPS** (.pnach) | El juego **calcula** 60 cuadros por segundo | **Duplica el trabajo del CPU** ("A doubled frame rate doubles the work", PS5SX2) | Solo para juegos ligeros. En Mega Man 60 FPS el VU1 iba al 97-100 % |
| **Generación de cuadros** (FSR 3) | La gráfica **inventa** el cuadro intermedio entre dos reales: 30 se ven como 60 | Usa la GPU (hoy al ~25 %). **No gasta CPU** | ✅ **La mejor opción para tu idea** en juegos de 30 FPS |
| **Optimizar CPU** (4.1-4.5) | Que el juego no baje de su velocidad real | — | ✅ Es lo único que garantiza el "nunca menos de 30" |

**Cómo funcionaría la generación de cuadros en Forja SX2:**
- Usa la **interpolación de AMD FSR 3** (código libre, licencia MIT). Calcula el movimiento entre dos cuadros con "optical flow" en la gráfica y genera el cuadro del medio.
- **PS5SX2 ya la implementó** (tres núcleos de trabajo en `ps5_framegen.cpp`, ~1.300 líneas, más los shaders). Corre en Vulkan, igual que nosotros. Su código está en la copia de iHaiDeeZ que ya descargamos.
- PS5SX2 la activa **solo cuando el juego va estable**, para evitar cuadros raros cuando el ritmo varía. Nosotros haríamos lo mismo:
  - juego estable a 30 → se ve a **60**;
  - si cae → se apaga sola y se ve el ritmo real.
- **Contras:**
  - +1 cuadro de retraso en los controles (~16 ms);
  - posibles "fantasmas" en movimientos muy rápidos o en el texto del menú (FSR 3 deja quietos los elementos que no cambian, como el HUD);
  - hay que medir cuánto tarda la GPU de la PS4. En la PS5 sobra; en la PS4 Pro debería alcanzar a 1080p, y en la Slim puede que no.
- *Esfuerzo:* alto (portar y adaptar el código). *Impacto:* muy alto en lo que se **ve**. *Riesgo:* medio (opción por juego, apagada por defecto).

**En resumen:** "nunca menos de 30" se logra con las mejoras de CPU; "que se vea a 60" se logra con la generación de cuadros. Las dos juntas son tu idea.

### 4.8 Núcleos y prioridades
- **Cómo lo hacen los juegos nativos de PS4:** pocos hilos pesados, cada uno fijo en su núcleo, con prioridad sobre los secundarios.
  - Naughty Dog (The Last of Us Remastered a 60 FPS) usó "fibers" fijos por núcleo.
  - Otro estudio describe "3 hilos de control + 3 trabajadores + 1 tolerante a la latencia".
- **Lo nuestro hoy:**
  - juego (EE) en el núcleo 2, 3D (VU1) en el 3, dibujo (GS) en el 4, audio en el 5;
  - secundarios en el 0-1;
  - séptimo núcleo pedido.
- **Pendiente: medir** en tu PS4 dos cosas:
  - el **reparto en un solo grupo de núcleos** (EE, VU1 y GS juntos en 1-2-3, que comparten caché);
  - las **prioridades** (opción lista en la v12).

### 4.9 Perfiles automáticos (por juego y por consola)
- **Detectar la consola:** medir la velocidad del CPU al arrancar (2,1 GHz = Pro con Modo optimizado; 1,6 GHz = Slim o normal) y elegir los valores de inicio: resolución, velocidad del CPU de la PS2 y generación de cuadros.
- **Base de datos propia de perfiles por juego**, que crece con cada juego que probemos (Mega Man ya tiene el suyo).

### 4.10 Estabilidad
- Probar el arreglo de la v13 (volver a la biblioteca) varias veces.
- **"Vigilante":** si el dibujo o el juego se quedan trabados más de unos segundos, guardar en el registro dónde se quedó y salir limpio, en vez de dejar la pantalla negra.

### 4.11 Comodidades que faltan (vienen de PS5SX2)
- **Estados guardados** (guardar y cargar en cualquier punto).
- Parches de pantalla ancha y 60 FPS desde el menú.
- Página de ajustes desde el celular.

---

## 5. Hoja de ruta

| Fase | Qué | Sesiones aprox. | Resultado esperado |
|---|---|---|---|
| **A** (ya) | Probar la v13 (biblioteca); medir núcleos y prioridades; perfil de Mega Man final; vigilante | 1-2 | Estable, sin pantalla negra, el mejor reparto de núcleos como predeterminado |
| **B** | **Precarga de efectos** (adiós tirones al cambiar de escena) + **generación de cuadros 30→60** (opcional por juego) | 3-5 | Transiciones suaves; juegos de 30 FPS vistos a 60 |
| **C** | Medidor de audio y audio en su propio núcleo; atajo para las consultas a la IPU; perfiles por consola | 4-8 | Videos a 30; +10-20 % de CPU para todos los juegos |
| **D** | Estados guardados, parches 60 FPS/widescreen desde el menú, más juegos probados | continuo | Emulador completo y cómodo |

**Siendo realistas:**
- Con A+B+C, los juegos ligeros y medianos deberían ir **estables y verse fluidos** en tu PS4 Pro.
- Los mundos abiertos pesados (GTA SA, God of War) van a mejorar, pero el CPU Jaguar sigue siendo el techo. Ningún emulador de PS2 en PS4 los corre perfecto hoy.

---

## 6. Lo que necesito de ti en cada fase
- **Fase A:** probar la v13 (entrar y volver a la biblioteca 2-3 veces) y las 3 rondas de núcleos con la misma batalla.
- **Fase B:** probar la generación de cuadros en Mega Man y decirme si ves "fantasmas" o retraso en los controles.
- **Siempre:** decirme qué juegos quieres que vayan bien (para hacerles perfil), y si algo se ve raro.

---

## 7. Fuentes

**Emuladores en los que se basa este trabajo (código fuente)**
- PCSX2 (emulador original): https://github.com/PCSX2/pcsx2
- Base de datos de juegos de PCSX2 (GameIndex, clamps por juego): https://github.com/PCSX2/pcsx2/blob/master/bin/resources/GameIndex.yaml
- Parches oficiales de PCSX2 (widescreen, etc.): https://github.com/PCSX2/pcsx2_patches
- PS5SX2 (PCSX2 en PS5; ajustes de rendimiento, parches de 60 FPS, perfiles "Recommended"): https://github.com/Swordpdf/PS5SX2
- PS4SX2 de iHaiDeeZ (incluye la generación de cuadros de PS5SX2 en `ps5/coreorbis/orbis-shims/ps5_framegen.h`): https://github.com/iHaiDeeZ/PS4SX2
- PS4SX2 de Phoenixx1202 (nuestra base): https://github.com/Phoenixx1202/PS4SX2
- Nuestro emulador: https://github.com/josephguerra562-ux/forja-sx2
- Kit para compilar programas de PS4 (OpenOrbis): https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain

**Generación de cuadros (AMD FSR 3)**
- AMD FidelityFX FSR 3 (página oficial): https://gpuopen.com/fidelityfx-superresolution-3/
- Manual de la interpolación de cuadros: https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-interpolation/
- FSR 3 publicado como código libre (MIT): https://www.phoronix.com/news/AMD-FSR3-Open-Source-MIT

**El hardware de la PS4**
- Microarquitectura AMD Jaguar (Agner Fog): https://www.agner.org/optimize/microarchitecture.pdf
- Jaguar en detalle (RealWorldTech): https://www.realworldtech.com/jaguar/6
- Séptimo núcleo para juegos: https://pcper.com/2015/11/sony-unlocks-one-more-jaguar-core-in-the-ps4/
- Modo optimizado (Boost) de la PS4 Pro: https://www.playstation.com/es-mx/support/hardware/ps4-pro-boost-mode/
- Campos del param.sfo (modo Pro, séptimo núcleo): https://github.com/FennyFatal/param_sfo_tools

**Cómo usan los núcleos los juegos de consola**
- Naughty Dog, "Parallelizing the Naughty Dog Engine Using Fibers" (GDC 2015): https://gdcvault.com/play/1022186/
- Reparto de hilos en PS4 y Xbox One (entrevista con desarrolladores): https://www.tomshardware.com/uk/reviews/multi-core-cpu-scaling-directx-11,4768-4.html

**Ajustes de PCSX2 y emulación en hardware débil**
- Opciones de rendimiento de PCSX2 (núcleo libretro: MTVU, Instant VU1, EE Cycle Rate/Skip): https://docs.libretro.com/library/pcee2/
- Antiguo "Async Mix" del audio de PCSX2 (riesgos de desincronización): https://wiki.pcsx2.net/SPU2-X
- NetherSX2 en Nintendo Switch (control de frecuencia de cuadros en hardware débil): https://gbatemp.net/threads/nethersx2-switch-port.683038/

**Nuestras mediciones** (en la PC de Joseph)
- `C:\Users\Usuario\ps4sx2-lab\pruebas\` (registros de cada prueba en la PS4)
- `C:\Users\Usuario\ps4sx2-lab\AUDITORIA.md` y `HOJA_DE_RUTA_RENDIMIENTO.md`
