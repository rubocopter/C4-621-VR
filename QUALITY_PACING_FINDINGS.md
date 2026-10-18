# Definición, giro y tiempos de presentación — 18-10-2026

**Perfil de referencia confirmado por el usuario: 2560×1440 por ojo y límite de giro de 90°.** Ensayo acotado de 27 s, con 15 s de seguimiento: «Se veía más claro y sin tirones apreciables». Los valores por defecto del programa se conservan en 720p/25°; seleccionar expresamente el perfil de referencia al repetir. No hay validación de comodidad prolongada ni de latencia de movimiento a fotón.

La regresión física posterior de PR #8 aceptó el mismo perfil ante borde
cercano/fondo lejano; no fue una nueva comparación de calidad/rendimiento.
[Estado y pendientes](docs/STATUS.md) · [procedimiento vigente](docs/VR_TRIAL_RUNBOOK.md).
Las medidas siguientes conservan las condiciones de cada pasada histórica.

## Motivo y cambios acotados

En el ensayo conjunto V8 el usuario pudo mirar con la cabeza, pero informó baja definición, tirones y rango reducido. La sonda limitaba deliberadamente la orientación a 25°; no se ha demostrado una restricción equivalente del cuerpo del AC. La salida estéreo era 1280×720 por ojo, ampliada a un quad de unos 120° horizontales.

Se conserva la ruta de reproyección y sus valores por defecto. Se añaden presets de salida 1280×720, 1920×1080 y 2560×1440; las dimensiones del par fuente no cambian. El productor no necesita modificar el juego ni variar la resolución de su ventana. La selección se aplica a buffers, shaders y swapchains del consumidor. No equivale a añadir detalle que no exista en el render de origen ni a renderizar geometría desde cada ojo.

La orientación publicada admite un límite configurable de 1 a 180°, con el mismo centrado inicial, reflexión Z y aplicación en ejes locales. Las comparaciones posteriores usaron 90°; esto no gira el cuerpo ni añade seguimiento de posición. Se mantienen 15 segundos activos, F12 y restauración automática. Una DLL de cámara con nombre y log nuevos evita sobrescribir/reutilizar la anterior, que sigue mapeada e inactiva.

## Medición y comparación

El presentador registra tiempos CPU en milisegundos para espera de frame OpenXR, actualización del par/reproyección, blit de cada ojo, transacción estéreo completa, EndFrame, intervalo entre frames y edad de la captura. Incluyen las esperas GPU/runtime correspondientes; **no son timestamps GPU ni latencia de movimiento a fotón**. Los resúmenes contienen cuenta, media, p50, p95 y máximo. Los percentiles usan hasta las primeras 32768 muestras y publican cuántas se retuvieron.

Durante las mediciones se omiten las dos capturas auxiliares de ventana que antes arrancaban ffmpeg durante el seguimiento. Es una posible fuente de carga adicional, no una causa confirmada de los tirones. El consumidor y la sonda publican sus resultados normales.

Comparación realizada en aquella tanda, misma misión y escena tranquila: A = 1280×720/25°, B = 1920×1080/90°, ambas sin capturas auxiliares y con el mismo presentador instrumentado. Cada pasada duró 27 s visibles y 15 s de seguimiento. Se registraron la valoración del usuario y los tiempos, sin atribuir un cambio entre pasadas a una sola variable: B cambia resolución y rango. La medida temporal localiza costes, pero no demuestra por sí sola confort ni mejora visual.

## Verificación previa

- Pruebas de giro ampliado: el test falló al no existir el parámetro; 90° conserva el giro pedido y el valor por defecto sigue limitado a 25°.
- Validación de presets/rango y estadísticos conocidos; rechazo de dimensiones mal formadas, rango no finito o fuera de límites, y configuración incompatible con el modo elegido.
- Test de orquestación comprueba el paso de dimensiones/rango al consumidor correcto y rechaza combinaciones inválidas antes de cargar una sonda.
- Verificación previa de esa tanda del presentador: cinco CTests correctos. Python: 26 pruebas correctas. Consumidor: cinco CTests GPU correctos, incluidos píxeles/bytes, color, paralaje, tamaños de blit y rechazo de resoluciones no admitidas. Se observó RED antes de implementar 1080p y la validación. Revisiones independientes de integración y cambio GPU sin bloqueantes.

## Uso vigente

Seguir el [procedimiento aislado](docs/VR_TRIAL_RUNBOOK.md), con PID actual,
sondas nuevas y consumidor elegido explícitamente. Se retira de aquí la
invocación antigua que podía seleccionar una build fija distinta de la deseada.

## Referencia medida (A)

`evidence/quality-baseline-v9-30252-result.json` y su JSONL, PID30252, 1280×720/25°, sin capturas auxiliares: 1128 frames visibles/enfocados, 1110 actualizaciones, cero pérdidas/fuentes descartadas; los tres procesos terminaron en código 0 y los hooks quedaron restaurados. Productor: 2365 pares y 1414 descartes. Presentador SHA-256 `df9c44403b4a10f3c9c8f20be72cc4bbc25b19ba7fee9ba17828d1ce42167a90`.

| Etapa (tiempo CPU con esperas, ms) | Media A | p95 A | Media B | p95 B |
| --- | ---: | ---: | ---: | ---: |
| WaitFrame | 2.249361 | 12.288200 | 0.876061 | 2.703200 |
| Actualización/reproyección | 10.776382 | 22.236100 | 7.587956 | 13.849800 |
| Blit izquierdo | 5.847843 | 16.382000 | 1.474863 | 7.153400 |
| Blit derecho | 3.727757 | 12.100300 | 0.642505 | 5.225200 |
| Transacción estéreo completa | 10.804539 | 23.172900 | 3.151373 | 8.520600 |
| EndFrame (incluido en la transacción) | 1.116682 | 1.419200 | 0.943341 | 1.130300 |
| Intervalo entre frames | 24.009254 | 40.773200 | 11.749987 | 18.119700 |
| Edad de la captura tras Update | 16.387411 | 31.000000 | 13.536892 | 16.000000 |

La actualización y ambas copias incluyen esperas síncronas de fence. Los datos muestran costes apreciables en esos tramos; aún no separan ejecución GPU de espera/scheduling. Los p95 de etapas distintas no se suman. No se deduce de esta medida que las capturas auxiliares fueran la causa ni que subir/bajar resolución solucione los tirones.

## Segunda pasada medida (B)

`evidence/quality-1080p-v10-30252-result.json` y su JSONL: 1920×1080/90°, mismo presentador instrumentado y sin capturas auxiliares. Productor: 3149 pares, 156 descartes, una cola y sin fallos. Presentador: 2304 frames visibles/enfocados, 2152 actualizaciones, 2304 poses válidas, cero pérdidas/fuentes descartadas. Cámara: 1759 aplicaciones, cero excepciones y restauración correcta. Los tres procesos terminaron en código 0 y los hooks quedaron restaurados.

El intervalo medio de esta pasada fue 11.749987 ms frente a 24.009254 ms en A, pero hubo picos: intervalo máximo 72.0254 ms y EndFrame máximo 70.2423 ms. Es una observación de estas ejecuciones, sin atribución causal a la resolución, sin equivaler a frecuencia física del visor ni dar por solucionados los tirones. Faltan comparaciones repetidas manteniendo una sola variable y timestamps GPU si se quiere optimizar el tramo dominante.

Respuesta a B: «podia girar mas, pero tampoco es que note mucho cambio de resolucion, supongo que aun asi es bastante bajo, no he notado especialmente deformaciones o imagen doble». Se confirma mayor rango; no se declara mejor definición ni ausencia de tirones, sobre los que no respondió explícitamente.

Consulta de solo lectura a OpenXR, sin crear sesión, con el mismo nombre de aplicación: recomienda 2804×2860 por ojo, máximo 8192×8192, una muestra. La recomendación corresponde a la vista completa; las capas usadas aquí son quads panorámicos, por lo que no se equiparan directamente sus píxeles. SteamVR publica `supersampleScale=0.68` en su configuración local; no se ha cambiado. Evidencia privada `evidence/xr-view-recommendation-30252.json`. El registro B acredita las dimensiones de nuestra salida, no la resolución efectiva del compositor. La comparación C se realizó después a 2560×1440/90°, misma aplicación y fuente, variando solo el tamaño de salida; se registra a continuación.

## Tercera pasada medida (C): perfil de referencia

`evidence/quality-1440p-v11-30252-result.json`, su JSONL y `*-camera.json`: 2560×1440/90°, mismo presentador y sin capturas auxiliares. Productor: 3101 pares, 174 descartes, una cola y sin fallos. Presentador: 2287 frames visibles/enfocados, 2170 actualizaciones, 2287 poses válidas, cero pérdidas/fuentes descartadas. Cámara: 1752 aplicaciones, cero excepciones, retirada y prologue correctos. Los tres procesos terminaron en código 0; AC6 seguía respondiendo.

| Etapa (tiempo CPU con esperas, ms) | Media C | p95 C |
| --- | ---: | ---: |
| WaitFrame | 0.591422 | 2.385400 |
| Actualización/reproyección | 7.607238 | 12.986000 |
| Blit izquierdo | 1.811086 | 7.183400 |
| Blit derecho | 0.660904 | 5.190400 |
| Transacción estéreo completa | 3.502181 | 8.485700 |
| EndFrame | 0.941596 | 1.114300 |
| Intervalo entre frames | 11.833966 | 18.831600 |
| Edad de la captura tras Update | 14.843463 | 16.000000 |

Al comparar claridad y tirones con B, el usuario respondió «Se veía más claro y sin tirones apreciables». Se adopta como perfil de referencia explícito para pruebas posteriores; el mayor rango ya se había confirmado en B. No se convierten esos comentarios en una garantía de fluidez: C también registró un intervalo máximo de 72.3932 ms y EndFrame máximo de 70.6284 ms. Quedan por medir sesiones prolongadas, tiempos GPU y latencia perceptual; la escala y proyecciones por ojo siguen sin calibrar.

No se modifica la calibración empírica de baseline, escala ni proyecciones OpenXR.

## Disponibilidad al enviar: caso de caducidad probado en host

La [comprobación final del par](LIVE_STEREO_FINDINGS.md#frescura-al-enviar-el-par--corrección-host-del-18-10-2026)
rechaza una fuente que caduca o falla después de la última copia/liberación,
antes de EndFrame. Mantiene el presupuesto de 500 ms y la instrumentación
existente; los descartes tardíos se cuentan como `source_discarded_during_frame`.
No se han vuelto a medir los tiempos anteriores ni se atribuye una mejora de
pacing o latencia a este cambio. Las esperas del runtime posteriores a la
consulta siguen fuera de esa comprobación de edad.

La ruta normal con esta comprobación se aceptó después en la regresión física
PR #8: 1217 envíos visibles, sin pérdidas ni descartes durante frame. No se
forzó caducidad después del último ojo en PSVR2. Los intervalos CPU de aquella
pasada fueron media 23.442012 ms, p95 34.6638 ms y máximo 1560.4146 ms con
esperas; no son una comparación controlada con V11 ni una mejora demostrada.
