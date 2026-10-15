# Cámara AC6: firmas, control nativo y seguimiento PSVR2

Fecha: 15-10-2026. Binario inspeccionado: `armoredcore6.exe`, versión PE 1.9.0.0.
SHA-256: `e003771e5548bca91882c1d7dfe2df3de97cee05c2c989d0c0e34b036ea610d6`.

**Estado más reciente:** imagen de AC6 dentro de PSVR2 y seguimiento de orientación combinado, con horizontal y vertical corregidos y confirmados por el usuario. Prueba temporal de 15 s con límite de 25°, desactivada al terminar. Sin estereoscopía de la geometría del juego. Véase `VR_DISPLAY_FINDINGS.md`; las secciones de este archivo describen la investigación y la primera prueba en monitor.

## Resultado

El código público de IGCS para AC6 contiene firmas que **siguen coincidiendo de forma única** con tres puntos relevantes del ejecutable instalado:

| Etiqueta de la referencia | RVA dentro del módulo | Inicio del intervalo unwind que contiene el punto |
| --- | --- | --- |
| Lectura del objeto de cámara | `0x151ece6` | `0x151e890` |
| Copia de matriz de cámara | `0x6bfef6` | `0x6bfe50` |
| Escritura identificada como FOV por IGCS | `0x6f038b` | Sin entrada unwind contenedora |

Las direcciones efectivas en ejecución son `base real del módulo + RVA`; no reutilizar la base preferida PE `0x140000000` como si fuera una dirección de proceso observada. Los intervalos unwind proceden de `.pdata` y no establecen por sí solos los límites semánticos completos de una función.

En el primer punto, los operandos reales coinciden con el interceptor de referencia: carga desde `[rsi+0x28]` y lectura de cuatro bloques de 16 bytes en `+0x10`, `+0x20`, `+0x30` y `+0x40`. En el segundo punto, se observan los operandos para copiar esos bloques de `rbx` a `rdi`. Esto corrobora la forma de los datos descrita por la referencia, pero no identifica todavía qué instancia corresponde a la cámara de gameplay.

La referencia propone matriz de transformación en `camera+0x10`, posición en `camera+0x40` y FOV en `camera+0x58`. **Son hipótesis para validar dinámicamente**. La firma FOV coincide con cuatro stores a `[rcx+8]`, `[rcx+0xc]`, `[rcx+0x1c]`, `[rcx+0x20]`; por sí sola no prueba que `rcx` sea el objeto de cámara ni verifica `+0x58`.

También hay coincidencias únicas para varias firmas auxiliares de IGCS; la de viñeta tiene **dos coincidencias**. No usar el resultado completo como autorización para instalar IGCS sin adaptar: su selección de primera coincidencia no garantiza la función correcta en esta build.

## Procedencia y repetición

- [IGCS-GITC de ghostinthecamera](https://github.com/ghostinthecamera/IGCS-GITC), commit `3f39db9ff45cd749b3aaa8cd8b3a3323a9bec14d`, copia en `reference/IGCS-GITC/`. Los comentarios de copyright/licencia originales se conservan.
- Firmas: `Cameras/Armored Core 6/InjectableGenericCameraSystem/InterceptorHelper.cpp`.
- Forma de los operandos: `Interceptor.asm`; offsets y escritura de matriz: `GameConstants.h` y `CameraManipulator.cpp`.
- [ac6-freecam de vswarte](https://github.com/vswarte/ac6-freecam), commit `5340615d158c8c089e0985eec9f6d247f5e1cedc`, copia en `reference/ac6-freecam/`. Su README limita compatibilidad a 1.06.1 y usa direcciones fijas; no se ha instalado ni ejecutado.
- Repetir: `python -B tools/inspect_camera_signatures.py --output evidence/camera-signatures.json`.

El script solo lee el ejecutable y genera un informe en el proyecto. El informe conserva offsets de fichero, RVA, bytes de contexto y el hash del fichero de firmas. Ninguna de estas observaciones de Python se atribuye a REA.

## Prueba del usuario y configuración

El usuario confirma haber iniciado por el launcher cooperativo y jugado en primera persona sin problemas. La lectura local de `GraphicsConfig.xml` confirma `DepthOfField=DISABLE`, fullscreen configurado a 2560×1440 y `MotionBlur=HIGH`. No se ha cambiado el XML. En el momento del reconocimiento no había proceso `armoredcore6` activo.

## Siguiente validación

Obtener el objeto capturado por la primera firma durante gameplay, observar una matriz finita/ortonormal que responda a la cámara y distinguirla de menús/fotomodo. Después, validar una composición reversible de la orientación de cabeza con la cámara base. Siguen pendientes sincronización con el renderer, sesiones OpenXR y dos proyecciones por ojo: una coincidencia estática no demuestra VR funcional.

## Análisis nativo directo con Ghidra 12.1.4

La búsqueda de cadenas de REA agotó su límite MCP de 300 segundos durante el análisis automático. `close_binary` terminó correctamente y liberó su proceso Java. A continuación se importó el mismo binario con Ghidra instalado, **sin análisis global**, y se ejecutó `tools/ghidra/InspectCameraCandidates.java`. Este resultado es de Ghidra ejecutado directamente, no Evidence de REA.

El comando terminó con código 0 y generó ambos informes con `DecompileComplete=true`. Salida privada fuera del proyecto:

- `C:/Users/Oni/AppData/Local/ac6-vr-analysis/camera_20261015_140331-output/candidate_6bfe50.txt`.
- `C:/Users/Oni/AppData/Local/ac6-vr-analysis/camera_20261015_140331-output/candidate_151e890.txt`.

El segundo informe corrobora la relación entre ambos candidatos: en RVA `0x151ec29` se llama al candidato `0x6bfe50`, usando como destino el objeto en `[rsi+0x28]`. Su pseudocódigo contiene la copia de los 64 bytes de transformación entre `+0x10` y `+0x4f`.

**Hallazgo importante sobre el punto de captura de IGCS:** la firma `0x151ece6` está precedida por una comprobación del global RVA `0x4e29820`. Si es nulo, se salta esa captura. Por ello no debe asumirse que un interceptor colocado allí se ejecutará durante toda la partida.

En cambio, en RVA `0x151ed13` se vuelve a cargar `[rsi+0x28]` después de esa rama. Las instrucciones siguientes copian los cuatro bloques de la transformación al objeto destino obtenido mediante este recorrido estático:

1. Cargar el puntero global en `base del módulo + 0x4f71c70`.
2. Cargar el puntero situado en `global + 0xc6a8`.
3. Leer los 64 bytes en `destino + 0x110` (bloques `+0x110`, `+0x120`, `+0x130`, `+0x140`).

Este recorrido ofrece un **candidato para telemetría externa de solo lectura**, sin necesitar todavía un hook para obtener `[rsi+0x28]`. No demuestra que el destino sea el constant buffer final de GPU, que exista en todos los estados, ni que escribirlo controle la cámara. Se debe comprobar con el juego en ejecución.

Limitaciones del descompilador: los rangos suministrados proceden de `.pdata`; hay saltos a fragmentos fuera de esos rangos (p. ej. `0x6bfe7f` salta a `0x772238`). El listado inicial cuenta 14 y 286 instrucciones respectivamente, no todo el flujo. Ghidra puede seguir/reconstruir esos saltos al producir pseudocódigo; **que complete la descompilación no prueba cobertura completa ni ejecución real**. La relación global anterior sí aparece en instrucciones desensambladas dentro del segundo rango.

Para repetir el experimento con la instalación actual: `python -B tools/run_camera_analysis.py`. No se instala nada en el juego. El script Java comprueba el hash esperado antes de analizar.

## Lectura en vivo: misión mediante ModEngine 2

Sesión del 15-10-2026 aproximadamente 16:13–16:15 (Europe/Madrid). El usuario inició `C:/Users/Oni/Desktop/ModEngine-2.1.0.0-win64/launchmod_armoredcore6.bat`, entró en una misión y comunicó fallo de conexión con los servidores de Epic. El estado offline no se midió independientemente. Se observó:

- PID `15060`, `armoredcore6.exe` en la instalación seleccionada, base real `0x140000000`.
- `modengine2.dll` cargado desde la instalación indicada; no se detectó `ac6_coop.dll`, módulo EAC en AC6 ni proceso EAC en la consulta realizada.
- Log de ModEngine: versión `2.1.0-0924ab36f1311bf84e7e626740b4bf2c0d54909a`, configuración AC6 leída, carpeta `mod` registrada, tres hooks aplicados. `external_dlls=[]`, `scylla_hide=false`.
- El `regulation.bin` de `mod/`, el del proyecto y el de `Game/` tienen el mismo hash `71b06ff9...11a72b67`. Por eso el mismo aspecto en los dos launchers es esperable; esta comparación no identifica cuál de las copias suministró los datos cargados.

Se ejecutó `tools/observe_camera.py` con derechos **PROCESS_QUERY_INFORMATION | PROCESS_VM_READ**. No se solicitó permiso de escritura ni se instalaron hooks, DLL o depurador. Antes de leer la cadena se verificaron la identidad del proceso, SHA-256 del ejecutable y opcode/desplazamiento RIP del global en memoria.

### Resultados

La cadena estática **sí se puede recorrer en esta misión**. Valores específicos de esta sesión (no reutilizar en futuros procesos): global `0x7ff3f919a4e0`, destino `0x7ff46ea78170`, matriz `0x7ff46ea78280`.

- Captura inicial, 3 segundos: 13/13 muestras finitas, 12 cambios de matriz, determinante aproximadamente 1 y columna homogénea correcta.
- Prueba guiada de 20 segundos: el usuario indicó que dejó quieto el AC; se pidió mantener fija la vista 5 s, girarla durante 10 s y mantenerla fija otros 5 s. Se obtuvieron **200/200 muestras finitas**, 163 cambios, determinante entre `0.9999999349` y `1.0000001782`, error máximo de ortogonalidad `1.79e-7`.
- En los primeros 5 s no cambió la orientación medible. La variación máxima respecto a la orientación inicial fue **58.57°** durante la captura completa. La dirección de la matriz se mantuvo estable.
- Las lecturas inmediatamente repetidas coincidieron en 200/200 muestras. Esto es una comprobación de estabilidad puntual, no sincronización con el frame.
- La traslación varió hasta aproximadamente 8.39/2.65/23.67 unidades del juego por eje. No se midió la posición del mech; no se atribuye esa variación exclusivamente a movimiento del personaje o de la cámara.

Datos: `evidence/camera-live-20261015-me2-first.jsonl`, `evidence/camera-live-20261015-me2-look-only.jsonl` y `evidence/camera-live-summary.json`. Los tiempos de control manual fueron aproximados y no se registraron imagen/input: el movimiento no quedó restringido exactamente a la ventana intermedia.

**Conclusión delimitada:** se ha validado en ejecución una transformación viva, coherente y compatible con la cámara durante la misión. La asociación se apoya en el flujo estático de copia, la estructura de la matriz y la respuesta de orientación en la prueba guiada. Falta verificar si modificar esta copia afecta al renderer o si es solo un espejo actualizado por el juego. No se ha escrito en ella ni implementado seguimiento de cabeza/estereoscopía.

Repetición: `python -B tools/observe_camera.py --pid <PID actual> --single-player-confirmed --seconds 20 --output evidence/<nombre-nuevo>.jsonl`. Elegir una misión individual con el launcher de mods y comprobar el estado de conexión; la sonda no lo determina por la presencia de ModEngine. El programa se detiene tras tres errores consecutivos y nunca sobreescribe una captura existente.

## Control nativo en la sesión cooperativa

PID `32616`, primera misión, launcher cooperativo confirmado por el usuario y `ac6_coop.dll` observado. No se detectó EAC en las consultas realizadas. Antes de escribir se guardaron partidas/configuración en `C:/Users/Oni/.universal-modder/backups/ac6-before-camera-control/20261015-162810.zip`.

- El getter RVA `0x6ef560` contiene `48 8b 41 08 c3`: devuelve `[rcx+8]`. La fuente de cámara se obtiene de `manager=*(base+0x4e09e80)`, `camera=*(manager+8)`. Matriz en `camera+0x10`; FOV observado en `+0x58`, 1.553343 rad (~89° verticales).
- Escrituras externas repetidas: el espejo no produjo giro visible; escribir la fuente produjo deformaciones observadas por el usuario. No usar esta ruta como controlador.
- Hook MinHook v1.3.4 en el comienzo de la copia RVA `0x6bfe50`, después de ejecutar el original. Solo procesa la fuente anterior y fija el primer destino coincidente. En esta sesión, destino `0x7ff46b7e6bf0`; no reutilizar este puntero heap. Se modifican los primeros 48 bytes de la matriz de destino en `+0x10`, conservando posición y FOV.
- Ensayo yaw fijo: 10° durante 3 s, 360 aplicaciones, 0 excepciones; el usuario confirmó movimiento a un punto y vuelta sin glitches. `evidence/camera-native-probe-audit.json` confirma prólogo restaurado. El primer lector tuvo un error de compartición del log CRT; la auditoría se realizó después de cerrar el worker. La versión posterior comparte lectura del archivo.

## OpenXR conectado a la cámara: prueba de 15 segundos

SteamVR/OpenXR creó una sesión `XR_MND_headless` y proporcionó poses PSVR2 reales. El productor Python publica la rotación relativa a la orientación inicial, limitada a 25°, por memoria compartida con mutex local al PID. El callback nativo usa adquisición sin espera y rechaza datos de más de 250 ms; no llama a OpenXR. La prueba usa `flip_z=false`, sin traslación.

- Registro del productor: **1267/1267 poses válidas** durante su ventana de 26 s; `evidence/openxr-headtracking-live.jsonl` y `evidence/openxr-headtracking-publisher.log`.
- Ventana activa nativa: **15 000 ms**, **1767 aplicaciones**, **0 excepciones registradas**. Las aplicaciones por segundo no son una medición de FPS presentados ni latencia de movimiento a imagen.
- `MH_DisableHook=MH_OK`; el lector verificó los 16 bytes originales del prólogo. Informe: `evidence/headtracking-native-result.json`. La DLL queda mapeada e inactiva; no descargarla con `FreeLibrary` durante la sesión.
- SHA-256 de la DLL ensayada: `9c85fed0420c242e31f4738d63e1a38bf005f6a3c5d3f061438de5f412110be7`. Fuente: `native/camera_probe/probe.cpp`; productor: `tools/probe_openxr_headless.py`, transporte: `tools/pose_publication.py`.
- Capturas Universal Modder: `evidence/coop-headtracking-active.png`, `evidence/coop-headtracking-active_later.png` y `evidence/coop-headtracking-after.png`. Muestran diferencias de orientación; no son una medición temporal continua.
- Confirmación del usuario: «lo he visto en monitor moverse cuando yo movia el visor si, funcionaba bien». Confirma el seguimiento percibido en esta prueba; no acredita todos los ejes por separado, comodidad prolongada ni comportamiento en combate.

**Alcance:** orientación de cabeza (3DoF) aplicada a la imagen convencional del juego. No se ha creado salida gráfica OpenXR, swapchains, dos renders por ojo, corrección del HUD ni traslación (6DoF). El seguimiento temporal ya se desactivó. Repetición y límites en `HEADTRACKING_RUNBOOK.md`.

## Actualización posterior: presentación y calibración dentro de PSVR2

La etapa posterior sí creó salida gráfica D3D12/OpenXR y mostró imagen viva de AC6 al usuario. En el primer ensayo conjunto, este detectó inversión de izquierda/derecha y arriba/abajo. La aceptación anterior en monitor no había comprobado de forma explícita la dirección de ambos ejes.

Se añadió conversión `R_game = S · R_xr_filas · S`, con `S=diag(1,1,-1)`: invierte yaw/pitch y conserva roll. El productor gráfico usa esta conversión después de recentrar y limitar el cuaternión; no se modifica el callback de copia. La prueba matemática específica falló antes de implementar el cambio y pasó después.

Ensayo `evidence/calibrated-headtracking-native-result.json`: 1794 aplicaciones, 0 excepciones, desactivación y prólogo restaurado. OpenXR: 2425 frames enviados, 2424 enfocados, 747 capturas y 2424 poses publicadas válidas. El usuario confirmó ambos ejes correctos; observó el alcance limitado, explicado por el tope deliberado de 25°. No se ha validado separadamente roll ni seguimiento posicional. El informe de presentación y repetición está en `VR_DISPLAY_FINDINGS.md`.
