# Estado verificado — 18-10-2026

Referencia vigente de validación. Los informes conservan el historial de
ensayos; sus pasos antiguos no son el procedimiento actual. Para repetir una
prueba autorizada, usar el [procedimiento de estéreo continuo](VR_TRIAL_RUNBOOK.md).

## Confirmado en PSVR2

La última aceptación física corresponde al código nativo `1dc8403` (PR #8),
ahora integrado en `main`. En misión offline, comparando un borde cercano y
un fondo lejano, el usuario confirmó **escena viva, profundidad fusionable,
sin imagen doble ni congelaciones observadas, y giro de cabeza correcto**:
«Sí, todo correcto durante la prueba».

Perfil explícito: **2560×1440 por ojo / límite de giro 90° / baseline 0,5
unidades del juego**, 27 s visibles y 15 s de seguimiento. Es una aceptación
subjetiva acotada de la ruta predeterminada de reproyección; no prueba de
exactitud geométrica absoluta ni de comodidad prolongada.

| Registro de esa pasada (PID 6844) | Resultado |
| --- | --- |
| Presentación | 1217 envíos visibles, 1061 enfocados, 1192 actualizaciones, 1217 poses válidas |
| Continuidad | 0 pérdidas de captura, 0 pares descartados durante la presentación |
| Productor | 2492 pares, 838 descartes por backpressure; una cola, sin fallo ni ledger inseguro |
| Cámara | 1759 aplicaciones, 0 excepciones |
| Cierre | Tres procesos auxiliares con código 0; hooks/prólogo restaurados, DLLs retenidas inactivas y juego respondiendo |

Los descartes del productor por backpressure son distintos de los descartes
del par durante la presentación. Los contadores respaldan esa ventana; la
fusión y el giro los confirma la observación del usuario.

Evidencia privada conservada localmente:
`out/psvr2-pr8-19588-v13/evidence/pr8-1440p90-v15-6844-physical-acceptance.json`,
con informe `*-result.json`, JSONL y `*-camera.json`. El directorio conserva
el nombre del primer intento; la aceptación es la de **v15 / PID 6844**.
No se distribuyen capturas del juego. SHA-256 del presentador probado:
`5de9e57fa8b557e961ae62d6cb53df8c86dc1408771186daa0ec1cdf64e29da3`.
El registro conserva también hashes de ambas sondas.

Antecedentes válidos dentro de su alcance:

- Escena fija: relieve sin imagen doble; sin actualización del juego ni giro combinado.
- V7: estéreo continuo con cámara movida mediante mando.
- V8: orientación de cabeza junto al estéreo; también se comunicaron baja definición, tirones y rango limitado.
- V11: 2560×1440/90°, «Se veía más claro y sin tirones apreciables» durante una prueba breve. Es la comparación de claridad aquí citada.
- PR #4 (`8b8ee37`): regresión física aceptada con ese perfil; después se aceptó PR #8.

Condiciones, cifras y testimonios en [estéreo continuo](../LIVE_STEREO_FINDINGS.md),
[proyección](../PROJECTION_FINDINGS.md) y [calidad y tiempos](../QUALITY_PACING_FINDINGS.md).

## Probado en host

- Contrato de proyección estática, profundidad normal/invertida, paralaje,
  signos, oclusiones, límites numéricos y rechazo de parámetros inválidos:
  referencias CPU y fixtures del consumidor GPU real.
- Intrínsecos distintos por ojo y conversión de frusta/FOV asimétricos:
  conectados al shader y comparados con referencias independientes,
  **sin selector de presentación ni activación en el visor**. Véase el
  [contrato experimental](EYE_PROJECTION_CONTRACT.md).
- Caducidad/fallo después de copiar/liberar el último ojo: regresión
  determinista con fronteras 500/501 ms. La prueba física ejercitó la ruta
  normal, **sin forzar esa caducidad** ni registrar descartes tardíos.
- Selección `--consumer-exe`, rechazo de rutas inválidas y registro del mismo
  binario ejecutado: orquestación simulada, sin nueva prueba física del
  lanzador incorporado después de PR #8.

Últimos resultados registrados antes de esta limpieza: 44 pruebas Python
correctas y una integración GPU opcional omitida explícitamente; 14 CTests
live con GPU local, 5 CTests XR host y 37 casos del self-test GPU offline en
sus tandas respectivas. No se han repetido para esta edición documental.
Comandos y alcance en [BUILDING](../BUILDING.md) y
[precisión de referencias](DEPTH_REFERENCE_PRECISION.md).
La CI de `665bfa9` (`39131342592`) compiló nueve componentes y ejecutó los
tests configurados sin visor; **CI no ejecuta la validación física ni las
pruebas con GPU local**.

## Contrato y límites actuales

- Defaults: **1280×720 / 25°**. 2560×1440/90° se selecciona explícitamente;
  baseline **0,5 unidades del juego**, sin calibración a metros o IPD.
- Una vista color/depth D3D12, dos imágenes reproyectadas, capas quad OpenXR
  restringidas por ojo y orientación **3DoF**. Sin dos renders geométricos
  independientes, posición 6DoF ni controles Sense.
- Calibración fuente estática observada en el helper el 16-10: fx
  `0.572404087`, fy `1.01760733`, A `-2.02655792e-6`, B `1.00000203`, W `1`.
  Es una observación de procedencia conocida, **no metadata de cada frame**.
- Fences/ledger protegen copia y vida del par; no identifican matriz, jitter
  o cámara que generaron el depth, ni prueban correspondencia temporal con
  color postprocesado/TAA o pose OpenXR.
- Protocolo v1 de **120 bytes**, frescura de 500 ms, validaciones, parada por
  múltiples colas y restauración conservados. Las DLLs inactivas permanecen
  mapeadas hasta cerrar el juego.

## Pendiente

1. **Proyección por fotograma:** observar un recurso/camino real de AC6 y
   demostrar identidad temporal de matriz, cámara, viewport y jitter con el
   envío elegible. Solo después diseñar metadata versionada por secuencia/slot,
   rechazando ausencias/desajustes. No generalizar matrices de una captura.
   Preparación host posterior: [traza opcional de procedencia por envío y
   grabación](FRAME_PROJECTION_PROVENANCE.md), con helper explícitamente sin
   asociación y sin cambios al transporte/presentación. Falta el binding GPU.
   La observación presencial v17 recogió 3 pares y 256 salidas del helper
   (70 direcciones de vista / 10 modelos), pero agotó la traza de listas:
   análisis rechazado. El cierre de ventana al primer backpressure está
   corregido en host; no valida todavía una matriz por par ni cambia VR.
2. **Geometría del visor y escala:** integrar poses/FOV por ojo, establecer
   unidades/IPD y presentación adecuada; validar físicamente antes de cambiar
   la ruta por defecto. La preparación host no acredita esas capacidades.
3. **Cobertura visual y temporal:** huecos/oclusiones, HUD, boost, lock-on,
   combate, transiciones, sesiones prolongadas, tiempos GPU, latencia de
   movimiento a fotón y comodidad. Los intervalos CPU incluyen esperas y no
   prueban mejora de rendimiento o latencia.
4. **Segunda cola:** dos intentos previos se detuvieron correctamente. Tras
   cambiar proceso y condiciones de overlays no reapareció; la causa **no
   está aislada**. Discord nunca tuvo overlay activo según el usuario;
   seguía cargada la DLL de Steam en la prueba exitosa. No se declara
   corregido ni se relaja la parada segura.

La regresión física básica del perfil ya está superada; no hace falta
repetirla por esta edición documental. La siguiente puerta de ingeniería es
demostrar asociación temporal de proyección, sin inventar datos ni introducir
hooks a ciegas. Cambios posteriores en la ruta visible requieren su propia
prueba acotada.

## Integración

PR #4–#9 integrados en `main` el 18-10-2026; `665bfa9` añadió el selector de
consumidor sin modificar la implementación nativa de producción aceptada en
PR #8. Las correcciones posteriores de oráculos son host. La aceptación no se extiende automáticamente
a futuros cambios. El trabajo vigente se consulta en `main`; los nombres de
ramas de los informes son históricos.
