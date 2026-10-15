# Forja SX2: lo que se hizo y lo que hay que probar

*Compilación 18. En la biblioteca, abajo a la izquierda, debe decir "Compilación 18" (la 16 y la 17 no compilaron).*

## Qué ya se probó (versiones 14 y 15)
- **Volver a la biblioteca:** funciona, sin pantalla negra.
- **Generación de cuadros con el cálculo de PS5:** colgaba la gráfica de la PS4. Con la 15 encontramos el culpable: el **paso 27 de 27**, el que arma el cuadro final. Los otros 26 funcionan bien.
- **Audio de la PS2:** se come el 12-13 % del núcleo del juego, y en la intro el 15-16 %.

## Qué trae la 18
1. **Paso 27 propio** para los 30 a 60 FPS. Es liviano y está hecho para la PS4. Por cada grupo de 4 píxeles prueba el movimiento de su zona y el de las vecinas, y mezcla el cuadro anterior con el actual.
2. **Protección:** si la generación de cuadros se cuelga, al volver a abrir el emulador se apaga sola.
3. **Biblioteca:** el DVD solo se busca cuando te quedas en "Disco", no cada vez que pasas por ahí. Así se evita el cierre que hubo en Ajustes.
4. **Audio:**
   - búsqueda rápida del estiramiento, que ayuda cuando el juego va lento;
   - medidor detallado: voces, eco y salida;
   - prueba opcional de "voces rápidas", apagada por defecto.
5. **Campanita de la intro de Forja:** más grave y suave, sin el pitido.

## Qué probar (unos 20 minutos)

**Antes:** borra Forja SX2 e instala el PKG de la USB.

| # | Prueba | Cómo | Qué decirme |
|---|---|---|---|
| 1 | Sonido de inicio | Abre Forja SX2 | ¿Ya no suena el pitido feo? |
| 2 | Intro normal | Entra a Mega Man y mira la intro hasta la primera escena jugable | ¿Cuántos FPS ves? Antes iba entre 17 y 26 |
| 3 | Intro con "voces rápidas" | Me avisas, yo lo prendo desde aquí, y repites la intro | ¿Va más fluida? ¿El sonido se escucha bien? |
| 4 | 30 a 60 FPS | Ajustes > Generación de cuadros: **Sí**. Entra a Mega Man y juega 3 minutos | ¿Se ve más fluido? ¿Ves bordes raros o "fantasmas"? ¿Se colgó? |
| 5 | Biblioteca | Pasa varias veces por "Disco" en el menú lateral y luego entra a Ajustes | ¿Se cerró la app? |

Al terminar escribe **"listo"** y bajo los registros.

## Si algo falla
- **Si se cuelga con la generación de cuadros:** espera 15 segundos y reinicia. El emulador la apaga solo, y yo bajo el informe con los marcadores.
- **Si el sonido de la intro con "voces rápidas" suena raro o el juego se traba:** yo la apago desde aquí.
