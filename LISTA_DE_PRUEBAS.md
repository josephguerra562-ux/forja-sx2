# Forja SX2: lo que se hizo y lo que hay que probar

*Versión con las fases A, B y C (compilación 14 en adelante). Para probar cuando tengas tiempo.*

## Qué se hizo

### Arreglos anteriores (versión 13), todavía sin probar
1. **Salida de video:** la cuenta de cuadros pendientes ahora la da la PS4. Debería arreglar la **pantalla negra al volver a la biblioteca** y dar un poco más de margen contra tirones.
2. **Fondo de la pantalla de carga** funcionando.

### Fase A: estabilidad y diagnóstico
3. **Detector de tirones:** cada vez que un cuadro tarda más de 50 ms, el registro anota la causa: código nuevo del juego, efectos nuevos, lectura del disco o cambio de ajustes.
4. **Vigilante:** si la biblioteca o el juego se quedan trabados más de 8 segundos, anota dónde se trabó.
5. **Detección de consola:** mide si es una Pro con Modo optimizado (~2,1 GHz) o una normal/Slim (~1,6 GHz). La Pro arranca en 2x si nunca se guardaron Ajustes.

### Fase B: que se vea más fluido
6. **Generación de cuadros (30 a 60 FPS):** opción nueva en **Ajustes**, **apagada** por defecto. Viene de PS5SX2 (interpolación de AMD FSR 3). Con un juego estable a 30 FPS, la gráfica inventa el cuadro intermedio y se ve a 60. Si el juego no va estable, se apaga sola.

### Fase C: CPU
7. **Medidor del audio:** dice cuánto del núcleo del juego se lleva la mezcla de audio de la PS2. Con ese dato se decide si vale la pena moverlo a otro núcleo.

## Qué probar (en orden; ~20 minutos en total)

**Antes:** borra Forja SX2 e instala el PKG nuevo de la USB.

| # | Prueba | Cómo | Qué decirme |
|---|---|---|---|
| 1 | Volver a la biblioteca | Entra a Mega Man, juega 1 minuto y sal con L3+R3 > Volver a la biblioteca. **Repite 3 veces** | ¿La biblioteca vuelve siempre bien? |
| 2 | Juego normal | Juega 5 minutos como siempre (una batalla y una cinemática) | ¿Va igual o mejor que antes? ¿Algún error raro? |
| 3 | **Generación de cuadros** | Ajustes > Generación de cuadros: **Sí**. Entra a Mega Man y juega 5 minutos (explorar y batalla) | ¿Se ve más fluido? ¿Ves "fantasmas" o bordes raros al moverte rápido? ¿Sientes el control más lento? |
| 4 | Generación de cuadros en el video | Con la opción en Sí, mira la intro | ¿Se ve bien o raro? |
| 5 | Apagarla | Ajustes > Generación de cuadros: **No** | Para dejarla como estaba si no te gustó |

Yo veo todo en vivo por la red. Al terminar, escribe **"listo"** y bajo los registros: ahí sale qué causa cada tirón, cuánto pesa el audio y si la generación de cuadros se activó.

## Qué hago yo con los resultados
- **Si los tirones son por código nuevo del juego:** precompilación o caché de código en el arranque.
- **Si son por efectos nuevos:** precarga de efectos al iniciar el juego.
- **Si el audio pesa 10 % o más:** moverlo a su propio núcleo (Fase C, parte 2).
- **Si la generación de cuadros se ve bien:** dejarla activada en Mega Man y en los juegos de 30 FPS.
- **Si se ve mal:** ajustar el ritmo o dejarla solo para quien la quiera.

## Las pruebas de núcleos (cuando haya tiempo, opcional)
Yo cambio el archivo `nucleos.txt` por FTP entre partidas; tú solo juegas la misma batalla 3 veces:
- **Ronda 1:** reparto actual.
- **Ronda 2:** juego, 3D y dibujo en el mismo grupo de núcleos.
- **Ronda 3:** la ronda 2 más prioridad alta.
