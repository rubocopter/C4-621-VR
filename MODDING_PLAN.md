# Hoja de ruta VR — revisada 01-11-2026

Objetivo: avanzar hacia estereoscopía geométricamente correcta y estable en
PSVR2/OpenXR, preservando el prototipo que funciona. El
[estado verificado](docs/STATUS.md) distingue aceptación física, pruebas host
y supuestos; la [prueba vigente](docs/VR_TRIAL_RUNBOOK.md) sustituye las
secuencias antiguas de orientación headless o pantalla monoscópica.

## Base que se conserva

Estéreo continuo reproyectado desde una captura color/depth D3D12, transporte
GPU entre procesos y orientación de cabeza 3DoF. El usuario aceptó la última
prueba acotada del PR #8 (`1dc8403`) en misión offline, con borde cercano y
fondo lejano, a 2560×1440 por ojo/90°. En V11 había comunicado mayor nitidez
y ningún tirón apreciable. No equivale a exactitud métrica, dos renders
geométricos ni comodidad prolongada.

`main` contiene PR #4–#9 y la selección explícita de consumidor. Los nombres
de ramas, PID, instalación y configuración de mods de las primeras
inspecciones son históricos: no acreditan el entorno de una sesión nueva.

**Próximo paso concreto:** completar el análisis de callbacks auxiliares y
su liberación en la dependencia `descriptor+0x68` → `owner+0x6d8` antes de
construir una segunda vista de control sin diferencia. La
[continuación offline](docs/NATIVE_VIEW_RESOURCES.md#continuación-estrictamente-offline-vida-del-descriptor-0x68)
verificó constructor, dos tablas virtuales, cuatro stores de la referencia,
cinco inserciones de wrappers y la limpieza condicional de su lista al destruir
la vista. Queda demostrar que los callbacks pendientes no sobreviven al
desmontaje: el owner solo destruye la vista si agota su refcount; también
pueden existir wrappers retenidos externamente. Ya se capturaron
los parámetros de una creación natural y se contrastó el resultado con 21
muestras posteriores.
El [censo de recursos](docs/NATIVE_VIEW_RESOURCES.md) confirmó que color/depth
principales apuntan a las salidas globales, mientras cambian targets y recursos
auxiliares entre escenas. Hay que decidir salidas propias o copia por ojo antes
de sobrescribir, con vida/barreras GPU e historiales resueltos. La
[ruta nativa](docs/NATIVE_VIEW_ROUTE.md) ya tiene cuatro recorridos CPU
observados con restauración. En paralelo, conservar la
[procedencia de constantes](docs/NATIVE_RENDER_ROUTE.md) como requisito para
certificar los datos usados por GPU. El puente de descriptores ya se resolvió;
faltan versiones efectivas y vínculo al depth. No repetir cobertura CPU v28.

Comparación del código de otros cinco proyectos VR y dos vías técnicas
posteriores — vincular constantes al draw GPU o encontrar un render de escena
reentrante — en [investigación externa del 21-10-2026](docs/EXTERNAL_VR_TECHNIQUES.md).
La [inspección de Elden Ring VR del 02-11-2026](docs/ELDEN_RING_VR_REFERENCE.md)
añade condiciones verificables: cierre de tareas de simulación, propagación de
cámara tras su finalizador, dos `Present` identificables, protección de
transiciones y vida de recursos GPU. Antes de duplicar el render de AC6,
identificar estáticamente la frontera de frame y sus efectos laterales.
Ambas siguen en fase de investigación; la [comparación de DS3 VR](docs/DS3_NATIVE_STEREO_REFERENCE.md)
ya ha orientado la ruta nativa AC6. No volver al ciclo de una sonda presencial
por cada salto CPU.

La [inspección offline de contexto/render](docs/NATIVE_FRAME_BOUNDARY.md)
verifica 66 sitios del PE identificado. Además de las escrituras de estado,
el prepaso puede sobrescribir un bloque de parámetros del target, modificar
cuatro slots de estado y consultar flags globales. Hay un despacho auxiliar
condicional que usa otro objeto global. Es necesario aclarar ownership y
secuenciación antes de repetirlo; el vínculo entre llamadas y frame y la
seguridad de un segundo ojo **siguen pendientes**.

La [continuación nativa](docs/NATIVE_RENDER_ROUTE.md) ya resuelve el puente de
CBV y el layout de tablas. Un ensayo automático único produjo 604 asociaciones
candidatas; la pérdida por capacidad mantiene cerrada la aceptación de esa
ventana. El [observador y analizador](native/native_constants_trace/README.md)
son código separado y comprobable. La fuente posterior cierra una muestra
breve en vez de agotar ocho segundos de buffers; solo está validada en host.
Siguiente tramo: versiones de command list/constantes y vínculo al depth real,
sin volver a investigar cobertura CPU v28 como requisito.

## Orden de trabajo pendiente

| Puerta | Evidencia necesaria para superarla |
| --- | --- |
| **Vista nativa por ojo** | Lista/fábrica y recorrido CPU observados; falta resolver recursos de cámara/targets/historias y misma simulación, después un control sin diferencia y dos vistas con salidas identificadas. |
| **Proyección y fotograma** | Identificar matriz/cámara/viewport/jitter del envío color/depth real, sin generalizar una captura. Probar identidad temporal antes de transportar metadata versionada. |
| **Escala y ojos del visor** | Establecer unidades/IPD y usar poses/FOV por ojo en una presentación adecuada. El contrato opcional probado CPU/GPU es preparación host, desactivada en presentación. |
| **Oclusiones y HUD** | Evaluar bordes, huecos, elementos cercanos y HUD; la captura única no contiene superficies ocultas. |
| **Continuidad y comodidad** | Tiempos GPU y latencia medidos, comparaciones repetidas con una variable, después combate/boost/lock-on/transiciones y mayor duración. |
| **DLSS por ojo** | Contrato host implementado; falta adapter NGX/D3D12 con color/depth/movimiento/jitter coherentes, historia por ojo y beneficio GPU/visual medido. No está activo en VR. |
| **Capacidades posteriores** | Valorar posición 6DoF, recentrado durante uso, cockpit y Sense después de resolver la base; no son capacidades jugables implementadas/validadas. |

La siguiente mejora de código debe resolver una limitación concreta mediante
regresión determinista primero. Si falta evidencia del recurso del juego,
trabajar en la parte independiente sin inventar matrices por frame ni añadir
hooks especulativos. No cambiar defaults ni sustituir la ruta aceptada por
una alternativa pendiente de visor.

## Condiciones que no se relajan

- Baseline 0,5 unidades del juego; defaults 1280×720/25°, perfil de referencia
  2560×1440/90° explícito. Sin equivalencia establecida a metros/IPD.
- Protocolo compartido v1 de 120 bytes, límites temporales, frescura,
  sincronización, restauración y protecciones GPU conservados.
- Parada ante múltiples colas no verificadas. La última prueba no reprodujo
  la segunda cola anterior; su causa sigue sin aislar.
- Solo build identificada y misión individual offline explícitamente
  confirmada, sin anti-cheat activo ni matchmaking oficial. El launcher
  cooperativo por sí solo no demuestra estado de red.
- Binarios aislados, nombres nuevos y rechazo de DLL ya mapeada. Tras la
  desactivación quedan inactivas hasta cerrar el juego; no sobrescribirlas
  ni forzar su descarga.

## Investigación y procedencia

Las primeras inspecciones identificaron ejecutable x64 nativo/D3D12, launcher
cooperativo y preset de cámara en primera persona de Liquidation. Permitieron
los experimentos; no acreditan compatibilidad general de mods ni sustituyen
el fingerprint de cada sesión. Hashes, rutas y observaciones originales en
[MODLOG](MODLOG.md), [cámara](CAMERA_FINDINGS.md),
[captura](RENDER_CAPTURE_FINDINGS.md) e historial Git de este plan. Se retiran
los pasos obsoletos del plan vigente.

Referencias de la investigación inicial:
[Enhanced First Person Camera, mod 305](https://www.nexusmods.com/armoredcore6firesofrubicon/mods/305?tab=files),
[mod 3](https://www.nexusmods.com/armoredcore6firesofrubicon/mods/3),
[mod 30](https://www.nexusmods.com/armoredcore6firesofrubicon/mods/30) y
[notas oficiales 1.09](https://en.bandainamcoent.eu/armored-core/news/armored-core-vi-fires-of-rubicon-patch-notes-109).
Son referencias históricas de procedencia; no acreditan compatibilidad con
versiones posteriores. [GPLv3 y derechos de terceros](COPYRIGHT_AND_REUSE.md)
permanecen sin cambios.
