# Proyección y procedencia de captura — 18-10-2026

Incremento desde `34dde0b`. La primera tanda fue exclusivamente host; la
observación posterior del 18-10 se registra debajo. No acredita proyección
por frame ni una aceptación nueva en PSVR2. No se ha iniciado SteamVR/OpenXR.

## Punto de asociación localizado

`StreamCapture::Execute` añade una lista privada de copia al envío elegible,
señala ready y publica tick seguido de sequence para el slot. Sin embargo,
`executed_depth_state` / `depth_since_capture` sobreviven a envíos anteriores:
la transición de depth puede proceder de **otro envío**. El fixture GPU ya
terminaba depth y color en envíos separados y reejecutaba una grabación. Antes
no había un identificador de grabación ni un registro por par que distinguiera
esos casos. La dirección COM de una lista tampoco distingue Reset/reutilización.

El hook de cámara `0x6bfe50` aplica orientación a la copia de transformación;
no observa bindings GPU. La investigación anterior localizó el update de vista
en `0x12904d0` y el helper de perspectiva en `0x23a7790` (ver
[campos y ABI](../PROJECTION_FINDINGS.md#campos-y-coeficientes)). Sus campos
incluyen dimensiones, FOV, near/far, offsets y jitter. Esos valores pertenecen
al objeto CPU: **width/height no son una observación de RSSetViewports**.
No se ha localizado aquí el recurso/offset del constant buffer que llega al
draw, su escritura/upload ni la vista que produjo cada región del depth.

No basta leer una GPU VA o buscar una matriz parecida al terminar un envío:
los root arguments y la memoria apuntada tienen reglas de vida diferentes.
Hay que observar el binding efectivo por draw y la versión de los bytes que
leyó la GPU. Tampoco se interpreta el orden de listas de un lote como tiempos
GPU: el driver puede solapar trabajo dentro de un mismo ExecuteCommandLists.
Fuentes primarias: [root signatures](https://learn.microsoft.com/en-us/windows/win32/direct3d12/root-signatures-overview),
[resource binding y sincronización de memoria](https://microsoft.github.io/DirectX-Specs/d3d/ResourceBinding.html),
[ExecuteCommandLists](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-executecommandlists).

## Implementación

`AC6_FRAME_TRACE=OFF` por defecto. Solo el build diagnóstico añade:

- Ledger: identidad monotónica de cada Reset observado y ordinal de transición
  **de recursos seguidos**, no ordinal de draw. Close sella esa grabación.
- Productor: ordinal de envío bajo el mutex existente, identidad de cola/lista,
  índice en el lote, grabación/transición de depth y color, recursos,
  backbuffer, copy_submit y sequence/slot/tick publicados. La transición de
  depth conserva el envío anterior real; capturas y descartes la retiran.
- Helper conocido: hook de lectura adicional, mismo ABI/prólogo validado;
  se arma después de preparar el stream. Conserva hasta 256 muestras con
  vista/salida, flag de jitter, campos/matriz completos, QPC antes/después,
  hilo y ocho direcciones de pila. No selecciona solo el FOV histórico ni
  deduce una cámara principal. Son salidas CPU sin prueba de uso GPU.
- Almacenamiento fijo: 512 pares / 8192 listas relevantes. Las listas selladas
  sin transiciones seguidas cuentan en el ordinal pero se omiten del log;
  **no** se declaran libres de acceso a esos recursos. Overflow se informa.
  No hay creación de recursos, lectura de píxeles, espera GPU o I/O nueva
  dentro de los callbacks.
- Corrección posterior a v17: el registro se cierra al primer descarte por
  backpressure, reteniendo el prefijo completo hasta ese envío. El productor
  sigue su ruta normal; un par publicado tras reanudar se cuenta en
  `omitted_pairs` y el analizador rechaza cobertura completa. No se reinicia
  ni se reconstruye una identidad desde el slot reciclado.
- Volcado tras Stop y retirada de hooks; entradas del helper publicadas con
  release/acquire, payload inmutable, slots incompletos omitidos y snapshot
  incompleto señalado sin esperar. Trampoline, memoria y DLL quedan residentes.

La traza es JSONL privado, versión diagnóstica 1, **no un transporte lateral**
ni una extensión del mapping. El protocolo compartido v1 sigue en 120 bytes.
Solo se registra un par tras Signal y publicación de su sequence; esto no
certifica que ready haya completado en ese instante CPU. La reutilización
sigue dependiendo de ambas fences. El consumidor no lee esta traza.
Los campos aditivos `window_closed`, `closed_submit` y `omitted_pairs`
documentan el cierre de esa ventana. Un log v1 antiguo conserva su rechazo
por overflow; no se le aplica retroactivamente una cobertura que no declaró.

`tools/analyze_frame_trace.py` contrasta cada fuente con una entrada exacta
de envío/lista/índice/grabación/transición y rechaza historias incompletas,
ambiguas, fallidas o truncadas. Comprueba por separado forma de perspectiva
y fórmula de jitter para el caso conocido sin shifts. Un shift distinto de
cero se conserva raw y se cuenta como candidato no admitido por ese análisis.
**Nunca elige una muestra por cercanía**, nunca exporta calibración ni afirma
asociación, incluso con matrices válidas: `missing_raster_binding`.
La ventana de 256 muestras es acotada: al llenarse se cierra la admisión,
`projection_window_full` lo indica y `projection_admitted_overflow` cuenta solo
llamadas ya admitidas que compitieron por el último slot. Las llamadas después
de cerrar la ventana no se cuentan. No cubre necesariamente todos los pares
o cambios posteriores.

El lookup C++ de procedencia diagnóstica exige sequence, slot, sequence
actualmente publicada, estado Ready/Stopped y edad 0–500 ms. No es una API de
consumo GPU. Failed/Preparing, ausencia, publicación futura o edad501 fallan;
Stopped permite únicamente el dato todavía fresco, igual que el stream.

## Evidencia host y límites

Tests CPU: Reset/reutilización COM, grabación futura ya preparada, reejecución
de la misma grabación en otro envío, depth en envío anterior, slot reciclado,
secuencia anterior/siguiente, publicación ausente, caducidad 500/501, fallo y
cierre; capacidad, concurrencia y publicación incompleta del helper.
Fixtures del analizador incluyen matrices razonables anteriores/posteriores,
otra vista, FOV distinto, jitter incorrecto, NaN/Inf y forma incompatible.
Todos siguen sin poder ascender a metadata de frame. **No son tests positivos
de correspondencia de matrices con AC6**: no existe todavía ese observador.

El fixture GPU del productor diagnóstico contrasta los pares con las copias
reales ya verificadas, exige que depth/color tengan distintos envíos
y distingue reejecución de nueva grabación. Tras corregir el cierre de ventana
retiene tres pares y declara omitido el cuarto publicado al reanudar; las
capturas y fences del stream continúan. Se ejecutan también los builds
predeterminados del productor, consumidor y presentador host.
Logs privados en `out/frame-projection-20261018/`. Los RED iniciales son de
API/módulo ausentes, no una regresión visual del prototipo. El desbordamiento
de pila del primer fixture nuevo (`0xc00000fd`) se corrigió moviendo su
almacenamiento fijo fuera de la pila. No se sobrescriben builds anteriores.

RED conductual adicional: el analizador aceptaba citar una transición antigua
válida aunque otra lista la sustituyese por DEPTH_WRITE antes de copiar.
`superseded-depth-red.log` registra el fallo; se corrige reconstruyendo el
estado en orden envío/índice, invalidándolo por aliasing/lista no fiable o
cambio de cola y retirándolo tras capturar. `superseded-depth-green.log`
incluye esos negativos. En la primera tanda, el JSONL del fixture GPU se analizó con el
mismo código: cuatro fuentes resueltas y únicamente `producer_failure`,
esperado porque ese fixture termina provocando Resize para comprobar la parada.
También se reprodujo y cerró RED → GREEN el caso de dos pares inventados
para un solo copy_submit (`duplicate-copy-red.log`); se exige orden creciente
y unicidad antes de construir el mapa de replay.

Resultados finales locales: 52 tests Python (51 correctos, una integración
GPU opcional omitida explícitamente); cinco CTests del productor OFF y cinco
ON, cuatro del par de captura, 14 del consumidor GPU y cinco XR host, todos
correctos. La CI compila también el productor ON; los fixtures GPU se han
ejecutado localmente, no se convierten en ensayos de visor. La sonda del helper
se prueba por entradas CPU y publicación concurrente; su detour real en AC6
se ejercitó después en v17, como se registra a continuación.

## Observación presencial v17 — PID 11284

Confirmación del usuario: «todo abierto», en respuesta a la petición de misión
cooperativa offline y presencia para la sonda. Loader verificó ejecutable/hash,
`ac6_coop.dll`, ausencia de módulos EAC y nombre de DLL no mapeado. Sin visor,
consumidor ni seguimiento. La sonda finalizó por su plazo de 8 s, código 0;
ocho hooks restaurados, DLL retenida inactiva y juego respondiendo.

| Evidencia | Resultado |
| --- | --- |
| Captura | 3 pares, 689 descartes por backpressure, una cola, sin fallo ni ledger inseguro |
| Fuentes observadas | copy_submit 54/82/110; en estos tres casos, depth y color pertenecen a listas distintas del mismo envío |
| Helper | 256 muestras, 70 direcciones de vista, 10 modelos fx/fy/A/B/W; 0 inválidas, 0 slots incompletos |
| Ventana del helper | 9,4601 ms de QPC; todas las muestras terminan antes del primer envío capturado |
| Capacidad de listas | 8192 entradas; última retenida en envío 4152, 30941 omisiones posteriores por overflow |
| Análisis original | `capture_provenance_complete:false`, `unknown_or_truncated_trace`; `frame_associated:false` |

No son 70 cámaras identificadas ni 256 frames del stream. Varias muestras
comparten fx/fy y difieren en A/B: filtrar por FOV tampoco identifica el pase
correcto. Las pilas dan callsites concretos para investigar el uso de la salida
del helper, pero no observan un binding GPU. El resultado no permite seleccionar
matriz/jitter para ninguno de los tres pares.

Evidencia privada: `evidence/frame-trace-v17-11284-loader.json`,
`evidence/frame-trace-v17-11284-analysis.json`,
`evidence/frame-trace-v17-11284-observation.json` y JSONL nativo
`out/frame-projection-20261018/diagnostic/Release/live_pair_probe_v17_11284.jsonl`.
SHA-256 DLL: `25bd56e28181ed920f55e2f710f609ce8ff6cde8d6eb282822e2afb6399c6a6c`.
SHA-256 JSONL: `5c50b9e47b8d5574051382cc7ac5fc699d3a69169a4d150101e2131c84b82b35`.
No se publican capturas, direcciones heap ni los datos raw del proceso.

El overflow se reprodujo en host manteniendo el anillo lleno y generando más
de 8192 envíos tras el primer descarte: RED con
`Post-backpressure tail overflowed the captured trace window`. El cierre de
ventana lo corrige sin aumentar capacidad ni relajar el analizador. Los tests
incluyen reanudación: conserva tres pares y declara el cuarto omitido. Logs
RED/GREEN en `out/frame-projection-window-20261018/`. El build nuevo v18 usa otro
directorio; v17 permanece intacto. Cinco CTests ON correctos y 53 tests Python
(52 correctos, una integración GPU opcional omitida); también pasan los cinco
CTests OFF en otro directorio nuevo. La pasada con el build corregido se
registra a continuación.

## Verificación presencial v18 — PID 11284

Usuario presente y confirmación expresa para repetir solo los 8 s con v18.
Build de `4d1556a`, otro directorio/nombre, conservando v17 intacta y mapeada
inactiva. Misma misión cooperativa offline, sin visor, consumidor o seguimiento.
Loader volvió a verificar identidad, módulos y nombre nuevo. Código 0;
ocho hooks restaurados y juego respondiendo. Ambas DLLs quedan inactivas.

| Evidencia v18 | Resultado |
| --- | --- |
| Captura | 3 pares, 715 descartes esperados, una cola, sin fallo ni ledger inseguro |
| Pares 1 / 2 / 3 | envíos 26 / 54 / 82; cada depth en lista índice 1 y color en índice 2 del mismo lote |
| Ventana retenida | 214 listas; cierre en envío 110; 0 overflow, 0 pares omitidos; 20104 envíos totales observados por el contador |
| Helper | 256 muestras, 77 direcciones de vista y 11 modelos; 0 inválidas/incompletas |
| Ventana del helper | 1,1214 ms; todas terminan antes del envío del primer par |
| Analizador | `capture_provenance_complete:true`, sin errores; `frame_associated:false`, `missing_raster_binding` |

Se verifica la cadena de transiciones observadas → envío → copia publicada
por sequence/slot para esos tres pares. No se demuestra último draw/escritor,
binding de cámara ni versión de matriz. El contador total continúa después
del cierre, pero no se presenta esa cola de eventos como una traza completa.
El resultado de v17 sigue rechazado; no se reinterpretó ni se sobrescribió.

Evidencia privada: `evidence/frame-trace-v18-11284-loader.json`,
`evidence/frame-trace-v18-11284-analysis.json`,
`evidence/frame-trace-v18-11284-observation.json` y JSONL
`out/frame-projection-window-20261018/diagnostic/Release/live_pair_probe_v18_11284.jsonl`.
SHA-256 DLL: `c4501972e5515cb91483a8b681730e2ba2ba3fd2d8011e4d9e433d6d6e729da8`.
SHA-256 JSONL: `ff0f5530f73c788fa6e81082a5da7706828967f7b092d66dfbee388c9a6d2658`.
No se ha probado ni cambiado la presentación PSVR2. No hace falta repetir
esta sonda para validar su cierre: el siguiente trabajo debe localizar el
binding/bytes de GPU a partir de los callsites recogidos.

## Procedimiento para una sesión nueva (no es una prueba pendiente)

No necesita visor ni consumidor. Una ventana de **8 s**, sin seguimiento,
en misión cooperativa offline ya confirmada, ejecutable/hash esperado y PID
nuevo; ninguna sonda anterior activa ni nombre de DLL mapeado. Mantener la
vista de misión activa y girar suavemente con el mando desde el inicio.
No ejecutar esta sección sin la presencia/confirmación del usuario.

Build aislado preparado en `out/frame-projection-window-20261018/diagnostic`, DLL
`ac6_live_pair_probe_v18.dll`. El loader vuelve a comprobar identidad, módulos,
ausencia de EAC y nombre no mapeado; si coincide con una DLL ya cargada,
usar **otra build/directorio/nombre**, nunca sobrescribirla o descargarla.
Ejemplo para esa sesión futura, sustituyendo PID y nombres de evidencia nuevos:

```powershell
.venv-analysis/Scripts/python.exe -B tools/load_camera_probe.py --pid <PID-NUEVO> --single-player-confirmed --dll out/frame-projection-window-20261018/diagnostic/Release/ac6_live_pair_probe_v18.dll --no-screenshots --capture evidence/frame-trace-new-unused.png --output evidence/frame-trace-new-loader.json
.venv-analysis/Scripts/python.exe -B tools/analyze_frame_trace.py --log out/frame-projection-window-20261018/diagnostic/Release/live_pair_probe_v18_<PID-NUEVO>.jsonl --output evidence/frame-trace-new-analysis.json
```

Éxito diagnóstico: tres pares publicados (anillo sin consumidor, descartes
posteriores esperados), una cola, fuentes de transición resueltas, muestras
del helper >0 con QPC/pilas, ningún overflow de la traza, snapshot completo,
ventana cerrada y cero pares omitidos, ocho hooks restaurados y juego
respondiendo. Guardar JSON del loader con hash
de DLL, JSONL nativo completo y análisis. F12 cancela; cualquier fallo conserva
los registros y detiene el ensayo. No es una prueba de fusión/comfort.

El resultado permite elegir las grabaciones y callsites exactos para localizar
el upload/binding. **No superará por sí solo el gate de proyección por frame.**
Falta observar DSV/pase y draw(s) que escriben ese recurso, root constants o
CBV/descriptor + recurso/rango/versión efectiva, transformación de vista,
viewport/scissor reales y relación del color final con TAA/historia, resolución
interna, cámaras auxiliares y postprocesado. Una matriz del pase de sombras,
una proyección sin jitter o una historia TAA anterior no se admiten por su FOV.
Antes de transportar proyección hará falta ese testigo GPU y rechazo de
ambigüedad/múltiples vistas por recurso; esta sesión no inventa ese testigo.

## Repetición host

```powershell
cmake -S native/live_pair_stream -B out/frame-projection-new -G 'Visual Studio 17 2022' -A x64 -DAC6_FRAME_TRACE=ON -DLIVE_PAIR_PROBE_NAME=ac6_live_pair_probe_v99
cmake --build out/frame-projection-new --config Release --parallel 2
ctest --test-dir out/frame-projection-new -C Release --output-on-failure --no-tests=error
.venv-analysis/Scripts/python.exe -B -m unittest discover -s tests -v
```

Estos comandos solo compilan/ejecutan fixtures propios. El default OFF conserva
el productor de 60 s y siete hooks; el diagnóstico es de 8 s con ocho hooks.
Ninguna opción de presentación, baseline, preset, límite de giro, frescura o
fence cambia. La pasada v18 se registra arriba. No recargar v17/v18 en el
PID 11284 ni sobrescribir sus binarios; para otro ensayo en ese proceso se
necesitarían nombre/directorio nuevos y otra comprobación presencial.
