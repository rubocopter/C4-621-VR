# Captura continua y reproyección GPU — 16-10-2026

**Estado: estéreo continuo y seguimiento de cabeza confirmados en PSVR2; perfil 1440p/90° observado más claro y sin tirones apreciables en la prueba acotada.** Ruta experimental posterior a la escena fija confirmada por el usuario. Tres buffers DEFAULT compartidos llevan pares color/depth desde AC6 hasta otro proceso D3D12. Ese proceso reconstruye dos ojos en GPU, sin lectura de píxeles en CPU salvo previews diagnósticos. El modelo perspectiva y baseline siguen siendo los de la investigación previa; no hay escala métrica calibrada, proyecciones asimétricas ni tratamiento del HUD.

## Productor y sincronización

`native/live_pair_stream` reutiliza la identificación de recursos y ledger de listas de la sonda de pares. Cada slot tiene listas privadas pregrabadas por backbuffer. En un envío elegible se añade la copia de color/depth a **la misma llamada ExecuteCommandLists**, después de las listas del juego. Depth vuelve a estado 192, color a PRESENT y buffer compartido a COMMON. Una transición nueva de depth es necesaria después de cada captura o descarte.

Dos fences compartidas: ready tras escribir el slot; released tras terminar el consumidor. La reutilización exige ambas completadas para su secuencia previa. Si el anillo está lleno, se omite la captura; nunca se espera a la GPU, se crean recursos, se mapean píxeles ni se escriben archivos dentro del callback de envío. Un único consumidor elige la secuencia más reciente, espera ready en su cola, lee el buffer, lo devuelve a COMMON y señala released. Omitir secuencias anteriores es seguro solo con estas condiciones y un consumidor.

Metadata versionada de 120 bytes, acceso mediante Interlocked a campos compartidos. Offset 36 permite solicitar salida anticipada del worker; límite de 60 s y F12 independientes. El productor publica fallos por resize, cola distinta, dispositivo perdido y agotamiento del ledger. Los recursos, mapping y DLL se conservan inactivos al retirar hooks para no liberar trabajo en vuelo. Una prueba nueva requiere otro nombre de DLL; no se sobrescribe ni recarga una DLL mapeada.

## Consumidor

`native/live_stereo` abre handles con nombre y valida versión, rangos, LUID, descriptores y exclusividad. Soporta entrada R10G10B10A2, RGBA8 y BGRA8 con pitches/offsets. Cuatro fases de compute por ojo, ordenación por depth original e índices acotados antes de Load. Salida RGBA/BGRA 1280×720, huecos negros opacos. `Blit` copia por GPU a una textura RENDER_TARGET y restaura ese estado.

Espera síncrona de GPU en el **proceso consumidor**, acotada a 5 s. Un fallo en vuelo termina ese proceso con recursos retenidos, nunca AC6. Las imágenes caducan a los 500 ms; las publicaciones viejas se retiran sin leer sus píxeles para liberar un anillo envejecido. El nuevo consumidor excluye secuencias ya liberadas por el anterior.

La integración OpenXR usa capas quad como la prueba fija: LOCAL, 2 m, 2.4×1.35 m. `--headtracking` es un ensayo posterior que publica orientación y usa VIEW; no asumir que validar la pantalla valida también esa combinación. La proyección fija no se asocia todavía al constant buffer del mismo snapshot. No equivale a volver a renderizar geometría para dos ojos.

## Verificación antes del visor

**Actualización host, 18-10-2026:** el consumidor ya recibe una calibración
estática validada y usa la misma función HLSL de proyección que el offline.
Se verifican cuatro modelos alternativos con GPU real, sin habilitarlos en el
presentador. El perfil histórico sigue por defecto, ahora registrado con
procedencia y `frame_associated:false`. No cambia el protocolo v1 de 120 bytes,
baseline, frescura, fences ni presentación. Véase el
[contrato y pruebas de proyección](PROJECTION_FINDINGS.md#contrato-de-proyección-verificable-en-host--18-10-2026).
No se ha añadido una validación física a los ensayos de visor descritos abajo.

- Tres CTests del productor: límites/frescura/propiedad; pares GPU cambiantes con saturación, descarte, reanudación y resize; gate GPU pendiente, envío único, rechazo de color sin depth nuevo y publicación de fallo. El gate observa lotes simulados para comprobar la decisión, y el otro fixture comprueba copias reales.
- Dos CTests del consumidor: procesos y dispositivos D3D12 separados, formatos/padding, colores/previews/blits exactos, paralaje y oclusión, fences retrasadas, saltos, reconexión, stale/Stopped y terminación acotada en vuelo. Debug layer no disponible en el entorno.
- Revisión del productor: se corrigió el agotamiento de ledger que dejaba el estado Ready sin capturas. La regresión se observó fallar sin la corrección y pasar con ella. Revisión de gates sin nuevos hallazgos de seguridad.
- Dos tests Python nuevos comprueban solicitud de stop limitada al protocolo/PID y lectura de logs parciales. No se reutilizan direcciones heap de otras sesiones.

Ensayo monitor `evidence/live-stereo-monitor-v1-5440-result.json`, misión offline por cooperativo, PID 5440, build SHA-256 `e003771e5548bca91882c1d7dfe2df3de97cee05c2c989d0c0e34b036ea610d6`:

| Dato | Resultado |
| --- | --- |
| Productor V1 | 721 pares publicados, 120 descartes; una cola, ledger íntegro |
| Consumidor, ventana de 6 s | 414 actualizaciones, secuencias 4–718 |
| Entrada / salida | 2560×1440 / 1280×720 por ojo |
| Diferencias RGB primera/última izquierda | 26.4379 % de píxeles |
| Diferencias RGB entre ojos, última | 53.6206 % de píxeles |
| Salida / retirada | Ambos procesos código 0, siete hooks restaurados, DLL V1 inactiva |
| SHA-256 DLL V1 | `7830840a2ada681223910b04d307a0b936719fbf13d03d7657bf5d65de88e82a` |
| SHA-256 monitor | `02b12e56e1070fdc4bf5702176e6626efe068a047b8aa5ed60b7bb8a25dfb098` |

Se inspeccionaron previews de ambos ojos al principio y al final. Los contadores no representan frecuencia del visor ni miden latencia de movimiento; el consumidor puede saltar secuencias. Se leen píxeles solo para esos previews.

## Repetición

### Corrección al reanudar (18-10-2026)

La revisión independiente detectó una carrera de frescura: la imagen podía caducar después de la comprobación del frame mientras OpenXR esperaba un swapchain. El antiguo `Blit` lanzaba excepción, dejando pendiente la liberación del ojo y el cierre del frame. `TryBlit` ahora devuelve false sin trabajo GPU para una fuente no disponible; el presentador libera cada imagen esperada y descarta **los dos ojos**, llamando a EndFrame con cero capas. También invalida la pose publicada si se descarta la fuente. Los errores de render liberan la imagen y cierran el frame antes de propagarse.

Se observaron fallos de regresión antes de implementar tanto en el fixture GPU de imagen caducada como en el test de transacción de ojos. Verificación posterior: tres CTests del productor, dos del consumidor, cuatro del presentador y 25 tests Python correctos; análisis sintáctico de 17 scripts. Esta reanudación aún no añade un resultado visual de streaming en PSVR2. Contrato de referencia: [gestión de imágenes y frames de OpenXR](https://raw.githubusercontent.com/KhronosGroup/OpenXR-Docs/main/specification/sources/chapters/rendering.adoc).

### Diagnóstico de colas en la nueva sesión (18-10-2026)

La primera prueba continua de visor en el PID 30252 se detuvo tras un par: el productor detectó una segunda cola y publicó Failed antes de que el consumidor pudiera abrir el stream. La repetición diagnóstica V3 reprodujo el fallo. Ambas colas eran DIRECT del mismo dispositivo; la segunda tenía una lista sellada sin transiciones de los recursos seguidos por el ledger. Esto **no demuestra** que esa lista no acceda a ellos, por lo que se conserva la parada ante otra cola.

Se añade un diagnóstico acotado a ocho identidades de cola: tipo, dispositivo, contadores de listas y una pila de hasta doce direcciones por identidad. Los callbacks acumulan datos en memoria; los módulos y offsets se resuelven y escriben después de retirar los hooks. No se relaja ningún gate de captura.

La captura monitor V4 de esta misma sesión terminó correctamente: 724 pares, 59 descartes y 485 actualizaciones del consumidor, secuencias 3–721. Solo se observó la cola del render de AC6. Esto confirma que la segunda cola es intermitente; aún no identifica su origen ni valida el estéreo continuo dentro de PSVR2. Evidencia privada: `evidence/live-stereo-vr-v2-30252-result.json`, `evidence/live-stereo-queue-diagnostic-v3-30252-result.json` y `evidence/live-stereo-queue-stack-v4-30252-result.json`. Los tres ensayos retiraron correctamente los hooks.

La observación V5 durante 60 s tampoco reprodujo otra cola: 194460 envíos, tres pares y 6942 descartes al llenar el anillo sin consumidor. Se verificó la retirada de hooks. La pila de la única cola incluye DXGI, Steam y Discord; esto no permite atribuir la segunda cola anterior a ninguno de ellos. Evidencia privada: `evidence/queue-observe-v5-30252.json`.

La repetición V6 en PSVR2 terminó con ambos procesos en código 0: 2346 pares, 147 descartes del productor y 1630 actualizaciones del consumidor. SteamVR contabilizó 1695 frames enviados, visibles y enfocados; cero eventos de pérdida de captura y cero fuentes descartadas durante el frame. Se restauraron los siete hooks y la sonda quedó inactiva. El seguimiento de cabeza estaba desactivado. Evidencia privada: `evidence/live-stereo-vr-v6-30252-result.json` y `evidence/live-stereo-vr-v6-30252.jsonl`. Estos datos acreditan envío y consumo, pendiente aún la valoración visual del usuario.

Primera descripción del usuario: «veia una imagen como si fuese una ventana, en la cual habia elementos en diferentes profundidades como si fuese un parallax». La ventana corresponde a los quads LOCAL elegidos para comparar con el ensayo fijo. Se solicita confirmar actualización al girar con mando y ausencia de duplicación/congelaciones antes de activar seguimiento de cabeza con esta ruta.

El usuario no había podido mover la vista con Sense y el ratón. Este prototipo no traduce Sense a controles del juego. Conectó su mando convencional, confirmó su funcionamiento en monitor y repitió con AC6 en primer plano. V7: 2197 pares, 294 descartes, 1349 actualizaciones y 1408 frames visibles/enfocados, sin pérdidas ni fuentes descartadas; ambos procesos en código 0 y hooks restaurados. Respondió «Sí, se actualizaba con profundidad y sin verse doble» a la comprobación de giro con stick, profundidad, duplicación y congelaciones. Esto supera el gate de pantalla estéreo viva; no acredita todavía seguimiento unido a estéreo, escala métrica ni comodidad prolongada. Evidencia: `evidence/live-stereo-controller-v7-30252-result.json` y `evidence/live-stereo-controller-v7-30252.jsonl`.

### Prueba conjunta de estéreo y orientación (18-10-2026)

V8/PID30252 usa la misma reproyección GPU, con los quads amplios en VIEW y publicación de pose de esa sesión OpenXR. Captura durante 27 s visibles y sonda de cámara calibrada durante 15 s, límite de 25° conservado. Productor: 2460 pares, 790 descartes, una cola y sin fallos. Presentador: 1185 frames visibles/enfocados, 1133 actualizaciones, 1185 poses publicadas válidas, cero pérdidas de captura/fuentes descartadas. Cámara: 1717 aplicaciones, cero excepciones y prologue restaurado. Los tres procesos terminaron en código 0 y se retiraron los hooks de captura. Evidencia privada: `evidence/live-stereo-tracking-v8-30252-result.json`, `evidence/live-stereo-tracking-v8-30252.jsonl` y `evidence/live-stereo-tracking-v8-30252-camera.json`. Valoración visual conjunta pendiente; los contadores no acreditan fluidez ni comodidad.

La revisión del diagnóstico detectó conversión CRT dependiente del locale en nombres Unicode de DLL. El test falló con `%ls`; se sustituyó por escapes JSON ASCII que conservan unidades UTF-16 y pares sustitutos. Se comprueban errores de escritura y flush de los nuevos registros tras retirar hooks. Tres CTests del productor correctos y revisión posterior sin bloqueantes. DLL V8 SHA-256 `a72440afc15bc4d9f7f27b21a25f5366e695669f6b321df6c3d0de816bee3947`; presentador SHA-256 `f71e384b0c4ddf48613b741469f15aab6c2018ec629b3fba0b52dca636e65f33`.

Respuesta a V8: «podia mirar con la cabeza aunque la resolucion es super baja, notaba algun tiron y no podia mirar todo el rango que podia con el mando, aunque supongo que va atado a la \"cabeza\" del cuerpo y el cuerpo no se mueve». Se confirma movimiento por cabeza, sin dar por validada comodidad/fluidez. El límite de 25° es del prototipo y no acredita una restricción del cuerpo. La salida es 1280×720 por ojo ampliada al quad de ~120°; aún no se separan costes de espera OpenXR, reproyección, copia y compositor. Siguiente trabajo: rango y resolución configurables, instrumentación temporal y una nueva comparación acotada.

Seguimiento de estos problemas: [definición, giro y tiempos de presentación](QUALITY_PACING_FINDINGS.md). Se implementan presets, límite seleccionable y tiempos CPU con esperas; las pasadas V9 y V10 completaron correctamente 720p/25° y 1080p/90° respectivamente. La respuesta visual comparativa se registra en ese documento; no asumir que más resolución resuelve los tirones.

### Construcción y ejecución

Compilar consumidor, presentador y una DLL **aún no mapeada**:

```powershell
cmake -S native\live_stereo -B native\live_stereo\build -G 'Visual Studio 17 2022' -A x64
cmake --build native\live_stereo\build --config Release
ctest --test-dir native\live_stereo\build -C Release --output-on-failure
cmake -S native\live_pair_stream -B native\live_pair_stream\build -G 'Visual Studio 17 2022' -A x64 -DLIVE_PAIR_PROBE_NAME=ac6_live_pair_probe_v2
cmake --build native\live_pair_stream\build --config Release
cmake --build native\xr_display_probe\build --config Release --target ac6_xr_display_probe
```

PID, versión y tag deben corresponder a la sesión real. El script valida identidad/cooperativo/EAC conocido, espera al productor, abre el consumidor y solicita retirar la captura al terminar. No modifica archivos del juego ni fuerza descarga de DLLs.

```powershell
.\.venv-analysis\Scripts\python.exe -B tools\run_live_stereo_trial.py --pid <PID> --single-player-confirmed --dll native\live_pair_stream\build\Release\ac6_live_pair_probe_v2.dll --tag live-new --mode monitor
```

Para el **siguiente gate de visor**, repetir la invocación anterior con un nombre de DLL nuevo, tag nuevo y `--mode vr`, con visor puesto y SteamVR sin paneles. La imagen dura 20 s desde VISIBLE. **Criterio de éxito:** ambos ojos reciben una escena que cambia al moverse el juego, la profundidad resulta fusionable sin imagen doble y no se observan congelaciones ni fallos de sesión durante la ventana. Conservar el `*-result.json`, `*.jsonl` del presentador y el resumen nativo, junto con una breve descripción literal de lo observado por el usuario (incluidas molestias o defectos); los contadores por sí solos no acreditan comodidad/fusión. `--headtracking` añade la sonda de cámara de 15 s y exige que esta tampoco esté mapeada; no activarla hasta comprobar primero la imagen viva estéreo. Datos del juego, logs y capturas quedan fuera del repositorio.

Referencias primarias: [handles compartidos D3D12 y namespace](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createsharedhandle), [apertura por nombre](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-opensharedhandlebyname).

## Frescura al enviar el par — corrección host del 18-10-2026

Base `0202774` (PR #7), rama `codex/stereo-submit-freshness-20261018`.
La comprobación previa a cada `TryBlit` no cubría el tiempo que tarda la
última copia GPU y `xrReleaseSwapchainImage`: ambos ojos podían devolver éxito
y aun así superar los 500 ms antes de construir el envío de dos capas.
También faltaba comprobar otra vez un fallo del productor en ese intervalo.

Se reproduce con el helper anterior: reloj simulado a edad499 al empezar,
ambas copias exitosas y edad501 después de la derecha. El test falla con
`Pair expired during final copy/release was submitted`. No requiere esperas
reales, GPU bloqueada ni sesión OpenXR.

La transacción queda así:

```text
acquire/wait L -> copy L -> release L
acquire/wait R -> copy R -> release R
validate current source -> EndFrame(two layers or zero)
```

`PresentStereoFrame` exige ahora un callback de validación final. Se llama
una sola vez después de ambas liberaciones, exclusivamente si ambas copias
tuvieron éxito. Una respuesta false descarta ambos ojos; una excepción cierra
el frame con cero capas antes de propagarse. Los errores y descartes anteriores
conservan sus liberaciones/cierre; los frames ocultos omiten la nueva consulta.

`LiveStereo::HasFreshFrame()` expone, sin trabajo GPU, las mismas comprobaciones
que ya usaba `TryBlit`: frame consumido, estado Ready/Stopped, layout inmutable
y edad no futura de como máximo 500 ms. `TryBlit` reutiliza esa consulta. El
presentador guarda el ID después de Update y exige que siga siendo el mismo
en la validación final. En el modo live-pair, un rechazo usa el contador/evento
existente `source_discarded_during_frame` e invalida la pose publicada. Las rutas
estáticas y la captura WGC/bridge mantienen su política previa.

### Evidencia y reproducción

- RED determinista observado antes de modificar la implementación.
- Los cinco CTests del presentador pasan: límites edad500/501, expiración en
  copia o release derechos, fuente invalidada, orden de liberación, descarte
  de todo el par, excepción del validador y un único EndFrame.
- Los doce CTests del consumidor pasan en RTX4070Ti (sin debug layer), incluidos
  previews/blits exactos, modelos de profundidad, intrínsecos/frusta/FOV,
  reconexión, caducidad y timeout. El fixture GPU verifica la nueva consulta
  antes del primer frame, después de consumir, ante Failed y tras caducar.
- Revisión independiente del diff: sin hallazgos. Logs locales ignorados en
  `out/stereo-submit-freshness-20261018/`: `submit-expiry-red.log`,
  `build-green.log`, `xr-green.log`, `live-build.log`, `live-green.log`.

```powershell
cmake -S native/xr_display_probe -B out/stereo-submit-freshness-20261018/xr_display_probe -G 'Visual Studio 17 2022' -A x64
cmake --build out/stereo-submit-freshness-20261018/xr_display_probe --config Release --parallel 2
ctest --test-dir out/stereo-submit-freshness-20261018/xr_display_probe -C Release --output-on-failure --no-tests=error
cmake -S native/live_stereo -B out/stereo-submit-freshness-20261018/live_stereo -G 'Visual Studio 17 2022' -A x64
cmake --build out/stereo-submit-freshness-20261018/live_stereo --config Release --parallel 2
ctest --test-dir out/stereo-submit-freshness-20261018/live_stereo -C Release --output-on-failure --no-tests=error
```

### Límites

Es una comprobación de disponibilidad en un instante CPU posterior a ambas
copias/liberaciones. El runtime puede esperar dentro de EndFrame y componer
después; no garantiza edad en el instante físico de presentación ni impide
un fallo del productor posterior a la consulta. El tick sigue siendo el de
publicación del stream, sin asociación nueva de matriz/jitter/cámara al render.
No añade timestamps GPU, medición de latencia ni sincronización con poses.

No cambia shader, protocolo v1/120 bytes, fences, hooks, límite de 500 ms,
baseline0.5, presets o rango de orientación. Se mantiene la presentación de
pares válidos y se rechaza el caso tardío antes admitido. Sin inicio de
AC6/SteamVR/OpenXR ni prueba física en esta tanda; los resultados de visor
anteriores pertenecen a sus respectivos builds. No se afirma una mejora visual,
de rendimiento o comodidad. Próxima regresión física, cuando se retome el visor:
20 s con 2560x1440/90°, giro lento frente a borde cercano/fondo lejano, comprobar
escena viva fusionable sin congelación/duplicación; sin forzar bloqueos GPU.

## Regresión física posterior del PR #8 — 18-10-2026

Se probó `1dc8403` a2560x1440/90°, 27s visibles y 15s de seguimiento. Los dos
intentos iniciales en PID19588, productor v13/v14, activaron la parada por
múltiples colas antes de ofrecer una prueba visual válida (0 y1 pares). Ambos
restauraron hooks y quedaron inactivos; no se cargó la sonda de cámara.
La pila de la segunda cola incluía Steam Overlay y DiscordHook. Esa presencia
no acredita que Discord tuviera su overlay activo ni aísla quién creó la cola.
El log de Steam también registró grabación y reinicialización de captura D3D12.

El usuario desactivó Steam Overlay para AC6, cerró Discord y reinició el juego
en misión offline. En PID6844 no estaba DiscordHook; seguía cargada la DLL de
Steam. Se usaron productor v15 y cámara calibrada v6 nuevos/no mapeados, con
el mismo presentador compilado de `1dc8403`. Una cola, 2492 pares y838 descartes
por backpressure, sin ledger inseguro ni fallo. Presentador:1217 envíos visibles,
1061 enfocados,1192 actualizaciones,1217 poses válidas, cero pérdidas de captura
y cero pares descartados durante frame. Cámara:1759 aplicaciones, cero
excepciones. Los tres procesos terminaron en0, hooks/prólogo restaurados y
DLLs retenidas inactivas; juego respondiendo.

El usuario confirmó «Sí, todo correcto durante la prueba» a escena viva,
profundidad fusionable sin imagen doble/congelación y giro de cabeza correcto
al comparar borde cercano y fondo lejano. Se registra aceptación subjetiva
acotada del PR #8. No se forzó caducidad en el visor: el rechazo tardío concreto
lo prueban los tests deterministas, no esta pasada sin descartes.

La segunda cola no se reprodujo en esta nueva sesión; su causa no está aislada,
porque se cambiaron varias condiciones a la vez. La política de parada sigue
intacta. Intervalo CPU medio23.442012ms, p95 34.6638ms y máximo1560.4146ms,
incluidas esperas; no se atribuye una mejora de rendimiento/latencia/comodidad.
Esta aceptación tampoco valida matriz/jitter por frame, escala métrica ni los
modos opcionales de intrínsecos/FOV asimétricos, que no se activaron.

Evidencia privada en
`out/psvr2-pr8-19588-v13/evidence/pr8-1440p90-v15-6844-physical-acceptance.json`,
su `*-result.json`, JSONL y `*-camera.json`. Los intentos bloqueados se conservan
en `pr8-19588-physical-trial-blocked.json` bajo el mismo directorio. Esta nota
se registra en la rama posterior; el PR #8 no se modifica ni se fusiona.
