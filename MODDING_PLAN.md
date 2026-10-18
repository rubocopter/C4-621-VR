# Hoja de ruta VR — 18-10-2026

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

## Orden de trabajo pendiente

| Puerta | Evidencia necesaria para superarla |
| --- | --- |
| **Proyección y fotograma** | Identificar matriz/cámara/viewport/jitter del envío color/depth real, sin generalizar una captura. Probar identidad temporal antes de transportar metadata versionada. |
| **Escala y ojos del visor** | Establecer unidades/IPD y usar poses/FOV por ojo en una presentación adecuada. El contrato opcional probado CPU/GPU es preparación host, desactivada en presentación. |
| **Oclusiones y HUD** | Evaluar bordes, huecos, elementos cercanos y HUD; la captura única no contiene superficies ocultas. |
| **Continuidad y comodidad** | Tiempos GPU y latencia medidos, comparaciones repetidas con una variable, después combate/boost/lock-on/transiciones y mayor duración. |
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
