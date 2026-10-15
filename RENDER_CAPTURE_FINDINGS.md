# D3D12: captura de profundidad y transporte por GPU

Estado del 15-10-2026. Esta etapa repara el trabajo interrumpido y conserva las pruebas anteriores. Imagen y orientación siguen siendo monoscópicas: el mapa de profundidad todavía no se usa para generar vistas estereoscópicas.

**Avance posterior:** ya existe una captura conjunta de color/profundidad en una sola lista privada de GPU. Véase [FRAME_PAIR_FINDINGS.md](FRAME_PAIR_FINDINGS.md). La muestra independiente y sus límites descritos abajo corresponden al primer ensayo de profundidad.

## Profundidad: bloqueo reproducido y corregido

Los intentos anteriores preparaban el readback pero rechazaban todas las copias, o no elegían ningún recurso. Se encontraron dos problemas en la sonda:

1. Rechazaba varias transiciones del mismo recurso aunque afectaran a planos distintos. En un recurso depth/stencil, el plano 1 no cambia el estado del plano 0 que se copia. Se mantiene el rechazo de transiciones que puedan alterar otra vez el plano 0 y de lotes con aliasing.
2. Exigía 2560×1440. La sesión remota llegó a utilizar un recurso de 2560×1444. La selección ahora observa durante los dos segundos iniciales y elige el candidato de mayor área entre las texturas del formato soportado, sin fijar esa resolución.

La prueba sintética reproduce ambos casos: textura de 2560×1444, transiciones de los planos 0/1 y un buffer ajeno en el mismo lote, depth inicializado a 0.25. Falló antes de cada corrección y pasó después. La selección es una heurística de diagnóstico, no una identificación universal del depth principal.

En la sesión individual confirmada por el usuario, PID **17628**, se obtuvo una muestra real:

| Propiedad | Resultado |
| --- | --- |
| Informe local | `evidence/depth-adaptive-17628.json` |
| Recurso elegido | 2560×1440, `R32G8X24_TYPELESS` (19), un sample y un mip |
| Copia | Plano 0, lote de 3 transiciones, footprint `R32_TYPELESS` (39) |
| Readback | 14 745 600 bytes; pitch 10 240 |
| Valores | 100 % finitos y en [0, 1]; rango observado ~0.00134–0.35318 |
| Retirada | Hooks restaurados y prólogo de cámara intacto; juego responde |

La visualización muestra geometría de la escena. No se ha demostrado todavía alineación exacta por píxel con el color, equivalencia temporal del frame, proyección inversa ni escala métrica. Las muestras de color de referencia se tomaron después; no constituyen un par sincronizado color/depth. Hace falta resolver esto antes de una reproyección binocular correcta.

El raw queda privado en `native/render_probe/build/Release/depth_batch_capture_17628.raw`; no se distribuye. Para visualizar una muestra nueva:

```powershell
python -m venv .venv-analysis
.\.venv-analysis\Scripts\python.exe -m pip install numpy pillow
.\.venv-analysis\Scripts\python.exe -B tools\analyze_depth_capture.py --report evidence\depth-adaptive-17628.json --raw native\render_probe\build\Release\depth_batch_capture_17628.raw --output-prefix evidence\depth-new-preview
```

El PNG normaliza percentiles y aplica gamma solo para visualizar; el `.npy` conserva los valores originales. El script rechaza la sobrescritura de resultados.

## Captura de ventana sin lectura de píxeles por CPU

`native/gpu_capture_bridge/gpu_window_bridge.cpp` había quedado como un archivo vacío de implementación al interrumpirse el trabajo delegado. Se completó e integró con la sesión OpenXR mediante `--gpu-capture`.

- WGC captura únicamente la ventana del PID AC6 seleccionado. D3D11 copia a una textura compartida mediante handle NT; D3D12 la importa y escala en GPU hasta el swapchain OpenXR. Una fence compartida protege productor y consumidor.
- El frame WGC permanece retenido hasta completar la lectura GPU. No se hace `Map` de sus píxeles en la ruta de producción. Los tests sí usan readback para comprobar colores y guardar una imagen de diagnóstico.
- La prueba sintética valida RGBA/BGRA, destinos typed/typeless, gamma, proporción, bandas negras y orden de fences. La prueba de ventana real obtuvo **300 capturas en 5.00646 s (~59.92 Hz)**; se inspeccionó la imagen final. Esto no mide latencia de movimiento a imagen ni garantiza esa tasa en cualquier escena.
- El tamaño de la ventana puede ser distinto; si cambia durante una sesión de captura se devuelve un error claro y hay que reiniciarla. No se cambia la resolución del usuario.

Repetir el diagnóstico de ventana, sin abrir una sesión VR ni cargar una sonda de cámara:

```powershell
cmake -S native\gpu_capture_bridge -B native\gpu_capture_bridge\build -G 'Visual Studio 17 2022' -A x64
cmake --build native\gpu_capture_bridge\build --config Release
.\native\gpu_capture_bridge\build\Release\gpu_bridge_live_test.exe <PID> evidence\gpu-window-new
```

Reemplazar `<PID>` y elegir un prefijo nuevo. La lectura final para guardar BMP pertenece solo a este diagnóstico.

## Bloqueo de seguimiento por foco OpenXR

El primer ensayo por GPU mostraba la imagen fijada al visor pero no movía la cámara: el lanzador esperaba `FOCUSED` y nunca cargó la sonda. La sesión sí estaba `VISIBLE`. El foco para inputs XR y la pose de cabeza obtenida del espacio VIEW son condiciones distintas; véanse las secciones oficiales de [sesión](https://github.com/KhronosGroup/OpenXR-Docs/blob/main/specification/sources/chapters/session.adoc) y [espacios](https://github.com/KhronosGroup/OpenXR-Docs/blob/main/specification/sources/chapters/spaces.adoc).

La nueva condición exige sesión visible, `shouldRender`, imagen reciente y orientación válida **y seguida**. El lanzador espera la confirmación `tracking_ready` emitida por esa misma sesión antes de cargar la sonda. Sigue rechazando estado oculto, pose inválida y captura vieja. Test de regresión observado fallar y pasar.

Ensayo final `evidence/gpu-vr-ready-17628-result.json`:

- Lanzador y presentación terminaron con código 0; la sonda se cargó con pose lista aunque todavía no hubiera foco de inputs.
- 2248 frames enviados, 1542 nuevas capturas, 2247 poses publicadas válidas y 0 pérdidas de captura. No se midieron cambios de píxel por CPU en esta ruta.
- Sonda: 1779 aplicaciones durante 15 s, 0 excepciones, retirada automática y prólogo restaurado.
- El usuario confirmó: «Sí, ahora funciona correctamente».

Repetición conjunta en una sesión nueva del juego y con evidencias nuevas:

```powershell
python -B tools\run_vr_trial.py --pid <PID> --single-player-confirmed --gpu-capture --tag vr-gpu-new
```

Persisten el límite de 25° y la retirada automática. La sonda ya cargada no debe forzarse a descargar ni recargarse en ese proceso. El siguiente trabajo es obtener color y profundidad sincronizados, validar proyección/escala y producir vistas independientes por ojo.
