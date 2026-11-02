# Recursos de la vista nativa — 01-11-2026

Continuación de [la ruta de vistas](NATIVE_VIEW_ROUTE.md). La build permanece
`e003771e5548bca91882c1d7dfe2df3de97cee05c2c989d0c0e34b036ea610d6`.
Se añadieron **41 anclas de instrucciones** a las 36 y nueve slots anteriores;
el verificador comprueba ambos grupos por separado. Todas coinciden. Es
análisis local Capstone/pefile; REA verificó otra vez identidad, sin repetir
el análisis global que agotó tiempo en octubre.

## Ownership y recursos encontrados

| Objeto/campo | Ruta local verificada | Implicación |
| --- | --- | --- |
| Targets `0xc640` | Constructor `0x2319220`, inicializador `0x23277a0` vía `0x231ead0` | Contiene bundles, recursos auxiliares y configuración; no es una sola textura. |
| Salidas principales | Global `0x4f71b70` → `+0x1c8`, color `+0xf0`, depth `+0xe8` | La fábrica incorpora esas referencias a sus parámetros. Crear otro target-set no garantiza texturas propias. |
| Bundle principal | Targets `+0x248`; color `+0x260`, depth `+0x268` | `0x24a7f20` puede retener referencias prestadas con refcount, o crear recursos según sus argumentos. |
| Velocidad | Bundle `+0x798`, referencia `+0x7b0` | El inicializador asigna etiqueta `VelocityRT`. No prueba cobertura de objetos, unidades, signos ni jitter. |
| Depth interno | Referencia `+0xa88` | Etiquetado `DepthTexture`; no se equipara al depth principal ni a la captura del presentador. |
| Estado de render de la vista | Owner `view+0x130`, estado `view+0x138` | `0x22e4ea0` toma un objeto del pool o asigna `0x340` bytes e inicializa uno nuevo. El destructor lo devuelve vía `0x22eacb0`. No copiarlo como memoria plana. |
| Global del owner | `0x4f71c70`, argumento RDX en `0x22f0d6b` | Distinto de `0x4f71cc8`, usado como argumento allocator en otra rama. El mapa corregido está anclado a la llamada real al constructor. |

El pool contiene estado de render; su presencia no identifica por sí sola un
historial TAA/DLSS. La cámara mantiene su ownership separado descrito en el
informe anterior. Las rutas de inicialización/release son evidencia estática;
esta pasada no invoca constructores ni destructores.

## Medición externa en PID 18948

[`native_view_resources.py`](../tools/native_view_resources.py) amplía el
observador con `--resources`, manteniendo el handle de solo lectura `0x410`,
fingerprint, módulos y comparación de código vivo. Repite campos seleccionados
y el censo de vista; rechaza cambios, lecturas parciales y punteros inválidos.
Lee como máximo `0xa90` bytes por operación y no sigue los objetos GPU.
Referencias nulas no cuentan como alias; direcciones distintas tampoco prueban
allocations distintas. No establece atomicidad, ausencia de ABA o vida GPU.

Backup previo adicional:
`C:/Users/Oni/.universal-modder/backups/ac6-view-resources-20261101/20261101-163130.zip`
(7 archivos, 117,6 MB). Se verificó visualmente **Sin conexión** en el título,
con el launcher cooperativo instalado y sin EAC conocido. Sin multijugador,
SteamVR ni nuevas DLLs durante este censo.

| Ventana válida | Muestras coherentes | Color/depth principales | Recursos que cambian al pasar a prueba |
| --- | --- | --- | --- |
| Garaje corregido | 31/31 | Ambas referencias iguales a las salidas globales | — |
| PRUEBA DE AC cargada | 31/31 | Misma igualdad; ambas referencias persisten | Target-set, velocidad y depth interno |

Vista, cámara y estado de render de la vista conservaron identidad entre esas
ventanas. El depth etiquetado es distinto del depth principal en ambas. Estas
son relaciones entre punteros CPU, no pruebas de subrecursos, backing memory,
contenido o consumo efectivo por la GPU.

Evidencia privada válida:

- `evidence/native-view-resources-18948-garage-corrected.jsonl`, SHA-256
  `55626883c0877e4ea5b5a7ff2c3b8a483bfec8d6d9f67fe366572c58ccf5c254`.
- `evidence/native-view-resources-18948-ac-verified.jsonl`, SHA-256
  `4382eba5899b97049252bf7ccaf2b96c82db56cc90fa780d5004cda66ee29158`.
- `evidence/native-view-resource-route-20261101-factory-dependency.json`: 36/36
  anclas anteriores, 41/41 ownership y nueve slots; todo verificado. El informe
  `*-final.json` anterior conserva las primeras 38 anclas de esta continuación.

No usar como referencia el primer `*-garage.jsonl`: comparaba el owner con el
global de allocator equivocado, corregido al seguir el argumento del
constructor y repetir los tests/lectura. `*-ac-test.jsonl` se tomó aún ante el
diálogo del garaje; su nombre no acredita escena. La ventana `*-ac-verified`
se tomó después de ver la sala cargada y registra el cambio real de targets.
Los intentos se conservan, sin sobrescritura ni promoción de su alcance.

## Captura natural de la fábrica

[`native/native_view_factory_trace/`](../native/native_view_factory_trace/README.md)
observa una sola llamada a `0x22f0c50`. Desde el título, donde el censo veía
ausente el manager de vista principal, se cargó una vez la sonda y se pulsó
Continuar. Registró descriptor de `0x70` bytes, auxiliar de `0x18` y parámetros
de targets de `0x78`, sin alterar argumentos ni repetir la llamada.

Resultado en PID 18948: **una llamada admitida/completada, mask 15**, ningún
callback pendiente, hook restaurado. El resultado coincidió con el holder de
vista principal en **21/21** muestras externas posteriores. La lectura externa
compara también el prólogo de fábrica contra el PE. El analizador exige mismo
PID/FILETIME de creación/base y un tick monotónico de inicio de muestra
estrictamente posterior al tick de cierre; no acepta el censo previo del garaje.
La cámara era nula al retornar la fábrica y se conecta después en el owner.

El descriptor contiene una dependencia en `+0x68` que coincide con el objeto
embebido `owner+0x6d8`. Las anclas `0x151b2dc` / `0x151bc00` y la lectura en
`0x22f0e1f` respaldan esa relación. No es un bloque de parámetros portable:
reutilizar las direcciones guardadas en otro proceso sería inválido. Falta
determinar el comportamiento/vida compartida de esa dependencia y los pases
que la reciben antes del ensayo de construcción adicional.

Evidencia privada:

- DLL probada SHA-256 `177adc7385d353beb3ae6f3384fbcc2299b1d1280f8a9381eb45775be9d661c9`,
  build aislada `out/native-view-factory-20261101/`.
- Log `Release/render_probe_18948.jsonl`, SHA-256
  `b013064c124da701f4483df9a1a22f9aa831bf65e188b8f2ca387e41895ca6cb`.
- Censo `evidence/native-view-factory-18948-postflight.jsonl`, SHA-256
  `3486f9c523e3c6940b90fa74100e71285d3e7e23c8b232d36878aa1d31d00eaf`.
- Carga/análisis `evidence/native-view-factory-18948-loader.json` y
  `evidence/native-view-factory-18948-analysis.json`.

Diez CTests ejercitan forwarding, excepciones C++/SEH, lectura parcial/guard,
admisión única concurrente, MinHook local, cierre pendiente y JSON. La revisión
del callback no encontró bloqueos; la del analizador detectó la falta de orden
postflight y se corrigió con ticks y regresiones. Son nueve tests Python de su
análisis. No se creó una segunda vista ni se acreditó consumo GPU.

## Continuación estrictamente offline: vida del descriptor `+0x68`

El 01-11-2026 se amplió
[`inspect_native_view_route.py`](../tools/inspect_native_view_route.py) sin
abrir AC6 ni cargar sondas. La ruta se verifica solamente para el SHA-256 de
este informe. Resultado: **36/36** anclas de vista, **41/41** de recursos,
**23/23** nuevas de dependencia, **9/9** slots de vista/fábrica y **10/10**
slots de las dos tablas de la dependencia. Informe privado:
`evidence/native-view-dependency-teardown-20261101.json`.

| Paso | Evidencia estática exacta | Interpretación acotada |
| --- | --- | --- |
| Construcción embebida | `0x151b2dc`: `owner+0x6d8`; `0x151b2e6` llama `0x1517de0`; salto a `0x654633b` | Subobjeto inicializado dentro del owner; el salto entre secciones ejecutables es parte del PE analizado. |
| Tipo y estado inicial | Tablas `0x3ae0870` y `0x3ae0898` (cinco entradas cada una); al terminar se instala la segunda y se ponen a cero dos bytes en `subobjeto+8` | La tabla final contiene `0x1517e60`, `0x1517ee0`, `0x1517f30`, `0x2471930` y `0x1517f20`. Sin identificación de símbolos originales. |
| Paso al descriptor | `0x151bc00`: dirección en `descriptor+0x68`; copia `0x22f0cbb` / `0x22f0cbf`; carga `0x22f0e1f` | El puntero pasa a la fábrica y acaba en `r14`, sin inferir ownership por el transporte. |
| Retención en auxiliares | `0x22f0e67`, `0x22f0f08`, `0x22f0fae` escriben `r14` en `+0x18`; `0x22f105f` en `+0x30` | Cuatro puntos que almacenan la dirección en estructuras auxiliares. El refcount observado en `0x2524640` se aplica al wrapper pasado a la lista, no acredita un refcount propio del subobjeto. |
| Métodos de la tabla final | `0x1517f30` lee `subobjeto+8`; `0x1517f20` comprueba `subobjeto+9` antes de delegar a `0x24719e0`; `0x2471930` consulta datos espaciales | Evidencia de callbacks/consultas condicionadas, sin demostrar qué ruta se invoca en un fotograma ni su estado GPU. |
| Desmontaje del owner | `0x151c609`: desregistro de vista con `0x2466850`; `0x151c617`: baja del refcount de la vista; `0x151c9cb`: `0x1517e20` desmonta el subobjeto | El orden local de instrucciones libera la referencia de vista antes de desmontar la dependencia embebida. No prueba ausencia de callbacks retenidos o en vuelo. |

La diferencia `owner+0x6e8` es un **indicador del registro de la vista** que el
constructor escribe y el destructor limpia; no es el campo de flags del
subobjeto (`owner+0x6e0/+0x6e1`). La tabla de métodos y la referencia capturada
no autorizan transportar ese puntero a otro owner, proceso o tiempo de vida.
Tampoco acreditan que la fábrica registre/desregistre todos los auxiliares de
forma síncrona. Para una segunda vista es necesario demostrar ese contrato de
callbacks y la vida de cada registro; después resolver targets compartidos,
orden de los pases y sincronización GPU.

La nueva comprobación de slots falla de forma controlada ante un target
inexistente o no ejecutable y tiene una regresión sintética. Ninguno de estos
tests abre proceso, juego ni visor.

### Cierre de auxiliares: ruta condicional verificada offline

La revisión ampliada del mismo PE añade **23/23** comprobaciones de instrucciones
alcanzables y **5/5** slots adicionales de destructores virtuales. El informe
privado final es `evidence/native-view-nested-aux-allocator-static-20261101.json`:
conserva las 36/36 anclas de vista, 41/41 de ownership, 23/23 de dependencia,
9/9 slots de vista/fábrica y 10/10 slots de dependencia. Todas coinciden con
el ejecutable identificado, sin ejecución del juego.

- La fábrica llama cinco veces a `0x2524640` desde `0x22f0e1a`,
  `0x22f0ebf`, `0x22f0f60`, `0x22f100d` y `0x22f10b0` para agregar
  *wrappers* auxiliares a la lista de la vista (`view+0x20`/`+0x28`).
  La rutina incrementa el contador del **wrapper** en `0x2524688`.
  Cinco inserciones no implican cinco punteros directos a `owner+0x6d8`:
  siguen siendo cuatro los stores de esa dirección identificados.
- El destructor base de la vista `0x25243b0`, alcanzado desde el destructor
  derivado `0x22efee0` mediante `0x22eff25`, llama a `0x15180d0` en
  `0x25243f7`. El recorrido de la lista decrementa el contador de cada
  wrapper (`0x15180ff`), invoca su destructor virtual **solo cuando llega a
  cero** (`0x1518118`) y borra la referencia local (`0x1518128`). La vista
  restablece el final de la lista en `0x2524401`.
- El owner decrementa el refcount de la vista en `0x151c617`, pero salta
  sobre su destructor virtual cuando queda una referencia adicional
  (`0x151c61b` → `0x151c643`). Por tanto, el orden de limpieza anterior
  se ejecuta antes de desmontar `owner+0x6d8` **solo si la vista alcanza
  efectivamente su destrucción durante esa liberación**.
- El wrapper con vtable `0x3e3e790` (slot 0 → `0x22eff80`) llama a
  `0x2466e80` desde `0x22eff8f`. Esa rutina libera de forma condicional
  sus dos referencias de helper en `wrapper+0x18` y `wrapper+0x20`: llama
  a sus destructores virtuales (`0x2466ec3` y `0x2466efa`), después al
  método `+0x68` de sus respectivos allocators (`0x2466ed0` y `0x2466f07`)
  y finalmente pone a cero ambos campos (`0x2466ed5` y `0x2466f0c`).
  También se verificaron los slots 0
  de las tablas `0x3e3e8a8`, `0x3e3e8e8`, `0x3e3ea68` y `0x3e3e928`:
  apuntan respectivamente a `0x22f00f0`, `0x22f0020`, `0x22efff0` y
  `0x22efeb0`. La existencia de estas rutas no prueba que se ejecuten
  mientras el owner siga vivo ni que no haya referencias externas.

Los cuatro destructores de helper comprobados reescriben su propia vtable y
solo llaman a su liberador de memoria si se solicita destrucción con ese flag.
En el desmontaje anidado se invocan con `edx=0` y la llamada al allocator viene
después. Ninguno de esos cuatro cuerpos contiene una llamada explícita para
desregistrar la dependencia `owner+0x6d8`; esto **no demuestra** ausencia de
registro o cancelación en otras rutas.

Queda abierta la posibilidad de que otros propietarios mantengan viva la vista
o alguno de sus wrappers, y no está probada la cancelación de callbacks externos
ni la ausencia de callbacks en vuelo. La ruta estática no justifica todavía
construir otra vista nativa reutilizando la dependencia embebida.

## Siguiente control

La copia plana de una vista no preserva pool, refcounts ni cámara. Para el
control sin diferencia ya se capturaron los parámetros efectivos de la fábrica
en una creación natural. Se conoce ahora el orden estático de construcción y
las rutas condicionales de desmontaje de la dependencia y los auxiliares;
falta acreditar que todos se liberen a tiempo, construir una vista con su ciclo
propio y verificar
qué outputs se comparten. Solo después se puede decidir entre salidas propias
o copiar cada ojo antes de sobrescribir los buffers compartidos, con barreras y
vida GPU correctas. Ninguna opción se activa por este lector.

Encontrar `VelocityRT` orienta la investigación DLSS, pero sigue faltando su
semántica y asociación efectiva con color/depth/jitter. El contrato DLSS host
permanece separado, sin adapter NGX ni inputs certificados por estas muestras.
