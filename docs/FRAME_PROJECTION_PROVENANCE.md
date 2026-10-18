# Proyección y procedencia de captura — 18-10-2026

Incremento host desde `34dde0b`. No se ha iniciado AC6, SteamVR ni OpenXR.
No acredita proyección por frame ni una aceptación nueva en PSVR2.

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
- Volcado tras Stop y retirada de hooks; entradas del helper publicadas con
  release/acquire, payload inmutable, slots incompletos omitidos y snapshot
  incompleto señalado sin esperar. Trampoline, memoria y DLL quedan residentes.

La traza es JSONL privado, versión diagnóstica 1, **no un transporte lateral**
ni una extensión del mapping. El protocolo compartido v1 sigue en 120 bytes.
Solo se registra un par tras Signal y publicación de su sequence; esto no
certifica que ready haya completado en ese instante CPU. La reutilización
sigue dependiendo de ambas fences. El consumidor no lee esta traza.

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

El fixture GPU del productor diagnóstico contrasta los cuatro pares con las
copias reales ya verificadas, exige que depth/color tengan distintos envíos
y distingue reejecución de nueva grabación. Se ejecutan también los builds
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
incluye esos negativos. El JSONL del fixture GPU se analizó además con el
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
queda pendiente de la ventana presencial descrita abajo.

## Única observación mínima al volver (requiere presencia)

No necesita visor ni consumidor. Una ventana de **8 s**, sin seguimiento,
en misión cooperativa offline ya confirmada, ejecutable/hash esperado y PID
nuevo; ninguna sonda anterior activa ni nombre de DLL mapeado. Mantener la
vista de misión activa y girar suavemente con el mando desde el inicio.
No ejecutar esta sección sin la presencia/confirmación del usuario.

Build aislado preparado en `out/frame-projection-20261018/diagnostic`, DLL
`ac6_live_pair_probe_v17.dll`. El loader vuelve a comprobar identidad, módulos,
ausencia de EAC y nombre no mapeado; si coincide con una DLL ya cargada,
usar **otra build/directorio/nombre**, nunca sobrescribirla o descargarla.
Ejemplo para esa sesión futura, sustituyendo PID y nombres de evidencia nuevos:

```powershell
.venv-analysis/Scripts/python.exe -B tools/load_camera_probe.py --pid <PID-NUEVO> --single-player-confirmed --dll out/frame-projection-20261018/diagnostic/Release/ac6_live_pair_probe_v17.dll --no-screenshots --capture evidence/frame-trace-new-unused.png --output evidence/frame-trace-new-loader.json
.venv-analysis/Scripts/python.exe -B tools/analyze_frame_trace.py --log out/frame-projection-20261018/diagnostic/Release/live_pair_probe_v17_<PID-NUEVO>.jsonl --output evidence/frame-trace-new-analysis.json
```

Éxito diagnóstico: tres pares publicados (anillo sin consumidor, descartes
posteriores esperados), una cola, fuentes de transición resueltas, muestras
del helper >0 con QPC/pilas, ningún overflow de la traza, snapshot completo,
ocho hooks restaurados y juego respondiendo. Guardar JSON del loader con hash
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
fence cambia. La prueba en juego descrita arriba sigue sin ejecutar.
