# Vistas nativas de AC6 — 01-11-2026

Aplicación de la referencia DS3 Native autorizada por el usuario al ceder el
PC para continuar. Ejecutable SHA-256
`e003771e5548bca91882c1d7dfe2df3de97cee05c2c989d0c0e34b036ea610d6`.
REA volvió a verificar su identidad. Para no repetir el timeout histórico de
análisis global, las instrucciones se siguieron con Capstone/pefile sobre
el PE identificado. Es evidencia estática local, no pseudocódigo de REA.

## Ruta encontrada en el propio AC6

La cadena de cámara ya conocida permitió seguir la inicialización de la
vista principal. Se encontró una lista nativa, su recorrido de render,
operaciones de alta/baja y una fábrica concreta de vistas de escena.
Estas direcciones proceden de AC6, no de sustituir offsets DS3.

| Tramo | RVA / campos | Evidencia y alcance |
| --- | --- | --- |
| Global del manager | `0x4e23028` | La carga en `0x1292bfb` alimenta la llamada al renderer `0x15194d0` en `0x1292c0a`. |
| Renderer/lista | `0x15194d0` → `0x2466510` | Usa `manager+8` como lista, y un contexto de frame. No demuestra reentrancia ni ausencia de efectos de simulación. |
| Vector de vistas | Lista `+8/+0x10/+0x18` | Begin/end/capacity de punteros de 8 bytes; difiere del layout publicado para DS3. |
| Render de vistas | `0x2466510` | Recoge prioridad vía `0x2466f40` (`view+0x10`), ordena, llama a vtable `+0x18` para targets y `+0x38` para render. |
| Añadir/quitar | `0x2466470` / `0x2466850` | Búsqueda de duplicados, referencias de vista en `+8`, inserción/compactación. No son APIs públicas ni se invocaron. |
| Propietario de vista principal | `manager+0x10` → holder `+0x3c8` | Constructor `0x151b1e0`: crea vista, asigna escena/cámara y la añade a la lista recibida en su campo `+0x3c0`. |
| Fábrica | Vtable `0x3762dd8`, slot 0 `0x22f0c50` | Wrapper `0x22f0bd0`, creación de targets `0x22f10f0` y constructor de vista `0x22efb70`. La fábrica recibe más que una cámara; no se la trata como render de escena repetible. |
| Vista concreta | Vtable `0x3e3e7e0`, tamaño `0x140` | Targets `+0x38`, escena `+0x40`, enlace de cámara `+0x48`; setters `0x2524af0`, `0x2524b00`, `0x2524a90`; render slot 7 `0x2524790`. |
| Cámara de vista principal | Asignación de `0x1b0` bytes | Constructor del propietario inicializa dos registros con separación `0xd0`. No reutilizar el tamaño `0x120`/stride `0x90` de DS3. |
| Targets | Asignación `0xc640` bytes | La fábrica crea el conjunto antes de construir la vista; la independencia de cada textura interna no está probada. |
| Destrucción | `0x25243b0` | Obtiene `view+0x48`, resuelve su allocator y llama al deallocator; también gestiona escena y targets. Compartir el enlace de la vista principal con otra vista que lo posea puede invalidar memoria. |

El tipo se etiqueta por vtable y constructor, sin inventar un nombre RTTI.
El literal `RendMan` existe como etiqueta diagnóstica; su sola presencia no
identifica un objeto en ejecución. La ruta de callbacks CPU tipo `0x25f`
del [informe anterior](NATIVE_RENDER_ROUTE.md) es otra capa y no se equipara
a esta fábrica de vista.

## Comprobación reproducible

[`tools/inspect_native_view_route.py`](../tools/inspect_native_view_route.py)
verifica **36 instrucciones y 9 entradas de vtable**. Decodifica flujo
alcanzable desde entradas explícitas, incluyendo saltos a la segunda sección
`.text`; no sigue llamadas ni ejecuta instrucciones. Maneja ramas `LOOP`,
rechaza streams solapados y conserva límites/indirectos no resueltos.

```powershell
.venv-analysis/Scripts/python.exe tools/inspect_native_view_route.py --output evidence/<nombre-nuevo>.json
```

La build debe coincidir exactamente. El reporte se crea de forma exclusiva.
Resultado revisado privado: `evidence/native-view-route-20261101-reviewed.json`,
36/36 y 9/9, sin limitaciones adicionales en los grafos seleccionados.
Esto no constituye cobertura global del ejecutable ni prueba de ejecución.
Los tests usan instrucciones sintéticas, sin bytes del juego.

## Observador externo y primera ejecución

[`tools/observe_native_views.py`](../tools/observe_native_views.py) abre un
handle **`0x410`**, exclusivamente consulta/lectura. Identidad, hora de
creación, base y módulos se obtienen de ese mismo handle antes/después de
la verificación estática, evitando heredar un preflight por reutilización de
PID. Rechaza EAC conocido, loader ausente y código vivo distinto del PE.
No carga DLL, instala hooks, escribe memoria ni añade comandos GPU.

Lee un máximo de 32 vistas y compara identidades, cabeceras y punteros entre
lecturas. Devuelve `identity_changed` ante cambios observados. Igualdad entre
lecturas no excluye ABA ni convierte la muestra en atómica. No observa las
llamadas al render, no asocia matrices/depth y siempre conserva
`frame_associated:false`.

Antes del arranque se guardaron partidas/configuración en
`C:/Users/Oni/.universal-modder/backups/ac6-native-views-20261101/20261101-132406.zip`
(7 archivos, 117,6 MB). Se usó el launcher cooperativo instalado. El aviso
visible del mod declara que no conecta al matchmaking oficial; se observó
`ac6_coop.dll`, sin módulos EAC conocidos. En el menú se seleccionó inicio
**Sin conexión** y se volvió al título para aplicarlo, antes de continuar.
No se abrió ni se unió a sesión multijugador.

Primera ventana de tres segundos, **PID 9428 / garaje**: **31/31 muestras
`identities_match`**, una vista. Vtable esperada y correspondencia entre
holder, lista, cámara y escena. Capturas de contexto privadas en
`out/native-views-20261101/`; log `evidence/native-views-9428-garage.jsonl`.
Segunda ventana, **PRUEBA DE AC individual**: otras **31/31 muestras
`identities_match`**, también una vista. Vista/cámara/escena conservaron
identidad en esa transición; el puntero al conjunto de targets cambió.
Log `evidence/native-views-9428-ac-test.jsonl`. Los hashes SHA-256 son
`cf87ed6f66bbaed3fef8c40a5e58c2c05cc7ff2b4b7c652fc7761527af8808ee`
(garaje) y
`44c831ba805661d00ebe5de66a5634546c73d2a735b2eaf969995db2fe5f8c0a`
(prueba). La identidad de targets no debe darse por persistente al cambiar
de escena. Ninguna ventana prueba llamadas al render, fotogramas GPU ni PSVR2.

## Recorrido nativo observado en ejecución

Sonda separada [`native/native_view_trace/`](../native/native_view_trace/README.md),
dos hooks observacionales en `0x2466510` y `0x2524790`, instalada una vez en
PID 9428 durante PRUEBA DE AC individual. **Cuatro recorridos completos y
cuatro renders de la vista principal**, ocho eventos, ordinales 1–16 y corte
17. Cada hijo tiene token de padre explícito, mismo hilo/contexto/lista y los
cuatro campos leídos. Cero overflow, pérdida, llamadas sin padre/lista ajena o
callbacks pendientes. Esto acredita llamadas CPU, no fotogramas GPU.

Ambos prólogos restaurados según DLL y comprobación externa posterior contra
el PE; también intacto el prólogo de copia de cámara. La lectura externa
posterior obtuvo 11/11 identidades coherentes y el juego seguía respondiendo.
DLL retenida inactiva, sin descarga forzada. SHA-256 de la DLL probada:
`658d2725b44e7f66aef7e7b4e1e7f612a543d8552849e0707a1e55edd63c3980`.
Log privado `out/native-view-trace-20261101/Release/render_probe_9428.jsonl`,
SHA-256 `759e562d41c3743fcc8369a8b755419c4d1318476f68b1368ceba1d64c6be9f3`.
Carga/análisis: `evidence/native-view-trace-9428-loader.json` y
`evidence/native-view-trace-9428-final-analysis.json`.

El [analizador](../tools/analyze_native_view_trace.py) exige prefijo ordinal
completo, raíces planas disjuntas por hilo, intervalos válidos y restauración.
Rechaza claves JSON duplicadas y registros parciales. La revisión detectó
falsas aceptaciones de intervalos/huecos/JSON, corregidas con regresiones.
Completions concurrentes pueden exceder el umbral de cierre y un hijo puede
quedar serializado sin padre elegible tras excepción/corte: ambos casos se
rechazan, sin convertirlos en captura completa. El binario probado se conserva.
`gpu_execution_proven` y `frame_associated` permanecen **false**.

## DLSS y siguiente puerta

[`native/dlss_contract/`](../native/dlss_contract/README.md) añade un contrato
C++17 independiente de NGX/GPU: exige color/depth/movimiento/vista asociados
por ojo, proyección y jitter coherentes y commit temporal conjunto. Conserva
historias separadas y resetea ambas tras fallo parcial, discontinuidad o
cambio incompatible. El stream v1 actual no aporta esa asociación y no
tiene adaptador que pueda declararla artificialmente. Son ocho grupos de
tests host; **no es DLSS activo ni acredita mejora de rendimiento**.

La nueva puerta es distinguir recursos propios/compartidos del conjunto de
targets y preparación/culling/submission, y preparar un
control de segunda vista sin diferencia solo cuando su ownership esté
resuelto. La fábrica ya localizada evita seguir buscando un render-view en
callbacks CPU que solo preparan constantes. No duplicar el renderer completo
ni compartir cámaras destruidas por otra vista. La procedencia GPU sigue
siendo necesaria para certificar fotogramas y alimentar DLSS correctamente.
