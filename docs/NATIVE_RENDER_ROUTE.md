# Ruta nativa de constantes y dibujo — 21-10-2026

**Continuación 01-11-2026:** [lista y fábrica de vistas de escena AC6](NATIVE_VIEW_ROUTE.md),
con verificación estática reproducible y primer censo externo de identidades.
Esta entrada es más amplia que el callback CPU tipo 0x25f descrito aquí;
abre la investigación de dos vistas nativas sin declarar cerrado el vínculo
entre constantes y depth. Este documento conserva la evidencia del 21-10.

Aplicación de la investigación externa para dejar de depender de la cobertura
del contexto CPU de v28. Se usaron el ejecutable identificado, los registros
existentes y una lectura acotada de la tabla de tipos del proceso 21860.
No se cargó otra sonda, no se escribieron datos del juego y no se abrió SteamVR.
El usuario autorizó continuar con ensayos automáticos en la zona de prueba
mientras estaba fuera del PC; esa autorización no sustituye una observación VR.

## Resultado que cambia el siguiente paso

Se ha identificado una ruta de implementación desde un bloque de matrices
del paquete CPU hasta el uploader del motor y una ruta de dibujo. Además,
se ha resuelto la fábrica y el destino virtual `slot8` **para el tipo 0x25f**
sin esperar otra invocación del contexto instrumentado.

Esto permite continuar por el consumidor de constantes del renderer. La
investigación de por qué v28 no obtuvo llamadas queda como secundaria;
**no se requiere repetir v28 ni recuperar cobertura de ese punto para seguir**.
Falta observar el binding efectivo, versión de bytes y draw del depth elegido.
No se declara `frame_associated:true`, estéreo geométrico ni proyección por frame.

## Evidencia y procedencia

- Ejecutable SHA-256:
  `e003771e5548bca91882c1d7dfe2df3de97cee05c2c989d0c0e34b036ea610d6`.
- REA abrió y verificó ese archivo. El dossier Ghidra agotó su plazo de
  arranque/análisis de 330 s; el fallback Rizin devolvió
  `capability_unavailable`. Se cerró la sesión. **No se obtuvo pseudocódigo
  ni Evidence de REA que demuestre las rutas siguientes.**
- Las instrucciones se examinaron con Capstone/pefile, usando el directorio
  completo de excepciones y recorridos acotados de saltos cuando una rutina
  está fragmentada o es leaf. Una sola entrada `.pdata` no siempre contiene
  toda la rutina; los saltos indirectos siguen explícitamente sin resolver.
- Lectura del proceso con `OpenProcess(0x410)` y `ReadProcessMemory`, sin
  permisos de escritura: registro global `base+0x4fe9ef0`, array de 25 entradas
  de 0x48 bytes. Cabecera y array coincidieron entre dos lecturas sucesivas.
  Es coherencia de la copia de metadata, no inmovilidad futura de objetos.
- Se comprobaron 19 sitios de instrucciones, el puntero de fábrica y el
  destino de la vtable contra el PE identificado. El informe privado conserva
  las limitaciones y mantiene `gpu_binding_observed:false`.

Evidencia privada: `evidence/native-render-registry-21860.json` y
`evidence/native-render-route-20261021.json`. SHA-256 del snapshot de registro:
`334f9b8d4cd189edbcced8bf0727e52841ca9389ac714e6e16fc73225e007710`.
Desensamblados privados en
`C:/Users/Oni/AppData/Local/ac6-vr-analysis/static-routes-20261021/`;
scripts exploratorios desechables en `out/projection-bindings-20261018/`.
Los volcados y desensamblados permanecen privados. El observador conserva
solo firmas mínimas de prólogo para verificar los cinco puntos de entrada.

## Cadena localizada

| Tramo | Hecho observado en el artefacto; límite |
| --- | --- |
| `0x26e0d70` → `0x2412df0` | En `0x26e1608` se toma la dirección del callback `0x26e17f0`; se pasa con dos argumentos a `0x2412df0`. Esta función solicita un comando tipo 4 y guarda la función y sus dos argumentos. No demuestra cuándo ni con qué frecuencia se ejecuta ese comando. |
| Registro `0x4fe9ef0`, entrada `0x25f` | La fábrica está en el campo `+0x20`: RVA `0x26d1ce0`. Otras entradas usan otras fábricas, o no tienen esa operación. No identificar el primer qword entero como código: contiene el type ID y bytes de padding. |
| `0x26d1ce0` → `0x26d18c0` | La fábrica valida el type ID 0x25f y construye una implementación. El constructor instala la vtable `0x3fe0e78`; su `slot8` es `0x26d1f50`. Es un destino condicional al tipo, **no la identidad de la hija concreta observada en v22**. |
| `0x26d1f50` → `0x27369d0` | El handler prepara parámetros y llama al preparador compartido. El recorrido de este último incluye `0x273ed40`; hay condiciones y dispatch indirecto pendientes. No equivale a una función reentrante de render de escena completa. |
| `0x273bfa0` y `0x273ed40` | El escritor ya conocido conserva una matriz de 64 bytes en paquete `+0x930`. `0x273ed40` entrega **80 bytes desde +0x930** al uploader `0x2462320`: matriz y 16 bytes adicionales cuyo significado no se presume. |
| `0x2462320` → `0x28ed220` → `0x28eddc0` | La rama D3D12 pide una asignación de upload y copia los bytes fuente al puntero devuelto. La otra rama usa `0x28fe540`. La existencia del memcpy no demuestra por sí sola consumo GPU ni versión efectiva al ejecutar. |
| `0x273ed40` → `0x237ecd0` | Tras obtener la asignación se conservan descriptor nativo y puntero de upload en el contexto y se registra el binding lógico con selector 1, índice 0, cantidad 1. Estos números **no se equiparan** a root parameter o registro HLSL sin resolver el layout. |
| `0x26d1f50` → `0x2378fd0` → `0x28ec830` | La rama de dibujo prepara el estado y llega a `DrawInstanced` (vtable byte offset 0x60), con condiciones. Esto no identifica el depth de nuestros pares. |

También se localizaron rutas de upload desde paquete `+0x110` (0x290 bytes)
y `+0x5f0` (0x340 bytes), en `0x273eec0` / `0x273edc0`, al mismo uploader.
Hay matrices derivadas/copias entre campos: comparar únicamente números de
un helper con un buffer sin conocer el layout seguiría siendo insuficiente.

## Consumidor D3D12 independiente del contexto v28

`0x28edaf0` resuelve una dirección GPU y llama al método del command list
correspondiente a `SetGraphicsRootConstantBufferView` (0x130) o su variante
compute (0x128), según modo. La ruta con puntero de upload usa `0x2906be0`,
que calcula `GPU base + (puntero CPU - base CPU mapeada)`; la base GPU procede
de `ID3D12Resource::GetGPUVirtualAddress` (0x58).

`0x28eda40` selecciona `SetGraphicsRootDescriptorTable` (0x100) o compute
(0xf8). Es una ruta adicional necesaria para buffers enlazados por tabla.
Los slots se contrastaron con el SDK local D3D12; buscar solo `call [vtable
+0x130]` produce falsos positivos, porque ese offset en `ID3D12Device`
corresponde a `GetCopyableFootprints`. La identidad del receptor importa.

El único caller directo localizado de `0x28edaf0` es `0x17d29e0`; eso no
demuestra que ese wrapper cubra la cámara principal ni los bindings de tablas.
No se elige como nuevo hook por el mero parecido del método API.

## Cómo se aplican las referencias

- **Cyberpunk VR:** seguir el upload real y el binding consumidor, con
  identidad de recurso/rango y versión, en vez de atribuir un frame a una
  muestra CPU por cercanía. El registro de tipos ha permitido resolver un
  dispatch sin otra sonda de contexto. Se han adoptado ideas, no copiado código.
- **Dying Light 2 / UEVR:** separar preparación de vista, comando diferido,
  consumo y draw antes de plantear un segundo render. El callback tipo 4
  localizado no se acepta como entrada de escena que pueda llamarse dos veces.
- **Halo MCC:** si se alcanza el render por ojo, presupuestar historiales
  temporales separados. Las cargas de matrices derivadas no bastan para
  asegurar que TAA/SSR/blur conservan identidad de ojo.

## Consumidor y correspondencia de descriptores resueltos

El análisis estático acotado del consumidor `0x237ec20 → 0x237fe70`
resuelve la tabla nativa: parámetros de 24 bytes y rangos de 6 bytes.
El índice del bit sucio seleccionado se convierte en el root parameter;
el selector lógico del productor no es ese índice. El serializador
`0x2912c40` conserva el orden y traduce la visibilidad al enum D3D12.
Se contrastaron 49 sitios/entradas/slots del SDK, con resultados privados en
`descriptor-consumer-20261021/verified-static-evidence.json` bajo la carpeta
local de análisis indicada anteriormente.

El selector 1 del upload de 80 bytes selecciona **HS, registro 0**;
sus campos `ctx+0x1608/+0x1678` corresponden a ese bloque de etapa.
Su consumo está condicionado al shader HS en `ctx+0x17b8`. Esto describe
la selección del motor; no identifica una cámara ni prueba que el shader
consuma esa matriz. Los otros productores localizados seleccionan VS/PS.

El puente `0x28f8a50` recibe allocation, índice, cantidad, array de registros
nativos y **un quinto argumento** con los punteros CPU de upload. La allocation
contiene CPU descriptor base `+0`, GPU descriptor base `+8`, stride `+0x10`,
cantidad `+0x18` y pool `+0x20`. Para el elemento i, el handle es
`base + (índice+i)*stride`; un handle de descriptor no es una GPU VA.

Con registro y CPU upload no nulos, el puente llama a `0x2906be0` y crea
el CBV en `0x28f8b30`, con tamaño alineado a 256 desde `nativeBuffer+0x28`.
La alternativa copia el descriptor almacenado `+0x38`, o un CBV nulo,
mediante `CopyDescriptorsSimple` en `0x28f8b54`. Después `0x28f8ea0`
transfiere el handle GPU e invalida los campos de la allocation: hay que
observarlos antes de esa transferencia. El consumidor pasa el handle a
`0x28eda40`, con root signature en `wrapper+0x70` y modo en `+0x78`.
Esta ruta procesa tablas y no establece el consumo por el wrapper separado
de root CBV `0x28edaf0`.

## Prueba automática única — PID 21860

Bajo la autorización vigente de misión offline y zona de prueba, se ejecutó
una observación de ocho segundos con los cinco puntos anteriores. No se
inició SteamVR, no se añadieron comandos GPU ni se modificaron matrices.
La comprobación independiente posterior con handle de lectura `0x410`
confirmó los cinco prólogos y el de copia de cámara restaurados. El proceso
siguió abierto. Las DLL v28 y del experimento permanecen inactivas y mapeadas.

| Observación | Resultado y alcance |
| --- | --- |
| Bytes de upload | 4096 registros completos; fuente antes y destino después coinciden byte a byte. |
| Eventos guardados | 8486 resoluciones GPUVA, 363 scopes de draw nativo, 5641 tablas raíz, 3077 escrituras de tabla y 15201 elementos. |
| Bloques localizados | 105 uploads de 80 bytes desde retorno `0x273ed66`; 108 de 656 desde `0x273ef4d`; 107 de 832 desde `0x273ee6e`. Otros 20 uploads de 80 tienen distinto caller. |
| Asociaciones candidatas | 604; incluyen 64 del productor de 80 bytes a root 7, 64 de 656 a root 0 y 64 de 832 a root 28. Root signature y command list son identidades muestreadas. |
| Cierre | `MH_OK`, cinco prólogos restaurados, cero callbacks pendientes al volcado; sin nueva carga de v28. |
| Límite decisivo | Se descartaron 759796 uploads y 4925798 eventos por capacidad. **No es una ventana completa ni una asociación de frame aceptada.** |

Log privado: `out/native-constants-trace-20261021/build/Release/render_probe_21860.jsonl`,
SHA-256 `3f8071c69f5e7a164fb501a9442826305defaca177bec42ad452f9842b571e12`.
DLL ejecutada: SHA-256
`cc8187aeea1d9f7b090fbe4bd09288c862c5db0f19a06b9810fddfb56cee4358`.
Informes `evidence/native-constants-21860-{preflight,loader,postflight,analysis-reviewed}.json`.
El analizador conserva `clean_recording_window:false`,
`gpu_execution_proven:false` y `frame_associated:false`.

Se conserva código revisable en
[native_constants_trace](../native/native_constants_trace/README.md), separado
del productor/presentador aceptados. La versión de fuente posterior cierra
admisión tras una muestra breve con un scope nativo completo; también cierra
al primer overflow, registra un cutoff y rechaza páginas guardadas. Esas
mejoras pasan pruebas de host y **no se han vuelto a cargar en esta partida**.
No se sobrescribe la DLL mapeada ni se inicia otra ronda de sondas.

## Próximo trabajo

La prioridad pasa a **conectar las identidades de constantes y tablas con la
generación del command list que escribe el depth capturado**, incluyendo
draws indexados, orden de cola y versiones efectivas de los bytes. El
consumidor de tablas y la ruta de los tres uploads ya están localizados;
no ampliar el scope CPU antiguo para demostrar únicamente que se ejecuta.
Comparación acotada de los binarios v22/v28: ambos constructores
inicializan el contexto en pausa; no se ha establecido que el cambio de
constructor explique la diferencia de cobertura.

La integración futura con el ledger del productor debe producir una
sola evidencia útil de `upload → recurso/rango/GPU VA → binding → draw → depth
capturado`, incluyendo regrabación de listas y orden de submission/fences.
Aplicar rechazo de versiones ambiguas, escrituras parciales y aliasing como
en la referencia de Cyberpunk; **no activar** metadata VR con una cadena rota.

El analizador ya rechaza reutilización solapada observada, identidades
muestreadas inestables, tablas ajenas, lecturas parciales, múltiples matches,
scopes/hilos incompatibles y pérdida de eventos. Faltan las versiones reales
de heaps/descriptores y escrituras ajenas a estos hooks; no se consideran
resueltos mediante la mera igualdad de punteros.
La observación en la zona de prueba puede ser automática y acotada bajo la
autorización actual. Una aceptación visual en PSVR2 seguirá necesitando al
usuario. El transporte, defaults y código de presentación aceptado no se
han cambiado en esta investigación.
