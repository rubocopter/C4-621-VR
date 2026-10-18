# Proyección observada y reproyección estéreo offline — 16-10-2026

> **Status update (18 October 2026):** The current PSVR2 prototype has already demonstrated **live depth-reprojected stereo combined with calibrated head orientation**. A bounded 2560×1440-per-eye, 90° trial was reported clearer and without noticeable stutter. This remains a time-limited proof of concept, not a complete VR conversion or proof of sustained comfort. See [README.md](README.md), [LIVE_STEREO_FINDINGS.md](LIVE_STEREO_FINDINGS.md) and [QUALITY_PACING_FINDINGS.md](QUALITY_PACING_FINDINGS.md). Historical procedures and pending gates below document the state at the time of each experiment.


Se ha observado la matriz perspectiva real que devuelve un helper de AC6, reconstruido profundidad en unidades del juego y generado dos vistas distintas de una captura guardada. Existe una referencia NumPy y un diagnóstico D3D12 independiente. El par fijo ya se ha mostrado en OpenXR y el usuario confirmó profundidad sin imagen doble. **Actualización posterior, 18-10-2026:** el estéreo continuo reproyectado ya se observó en PSVR2 y se combinó con orientación de cabeza durante ventanas acotadas; el perfil de referencia es 2560×1440 por ojo / 90°. Esta nota conserva la evidencia de la fase offline. Véanse [LIVE_STEREO_FINDINGS.md](LIVE_STEREO_FINDINGS.md) y [QUALITY_PACING_FINDINGS.md](QUALITY_PACING_FINDINGS.md).

## Identidad y método

Ejecutable local `armoredcore6.exe`, recurso PE 1.9.0.0, SHA-256 `e003771e5548bca91882c1d7dfe2df3de97cee05c2c989d0c0e34b036ea610d6`. Misión individual offline confirmada por el usuario, launcher cooperativo, PID 5440. El PID y las direcciones heap no son reutilizables entre sesiones.

REA abrió y verificó este ejecutable; después se cerró la sesión. Para evitar repetir el timeout de análisis global, la inspección enfocada se hizo con Capstone/pefile sobre ventanas privadas del binario. Esas salidas no son Evidence de REA ni se publican. Se contrastaron las instrucciones y llamadas relevantes; una decodificación lineal de esta build puede incluir bytes inalcanzables por ofuscación.

La sonda `native/projection_probe` engancha durante cinco segundos el helper RVA `0x23a7790`. ABI observado: RCX apunta al objeto de vista, RDX a la matriz de salida, R8b indica jitter. El detour llama al original y después copia campos/matriz a slots acotados. No modifica esos datos. Filtra perspectiva, dimensiones, valores finitos y parámetros de cámara compatibles; no hace I/O desde el callback. F12 termina antes. Trampoline y almacenamiento permanecen residentes e inactivos tras retirar el hook.

El cargador exige la identidad esperada, launcher y ausencia de módulos EAC conocidos. Eso no demuestra por sí solo el estado de red. No recargar ni sobrescribir una DLL que ya esté mapeada.

## Campos y coeficientes

La cámara fuente conserva la cadena descrita en `CAMERA_FINDINGS.md`. Wrappers separados 0x24 bytes desde `+0x50`; getter RVA `0x6f0350`: modo 1 lee `+8`, modo 0 lee `+0x1c`. En esta sesión los valores fueron FOV Y 1.553343 rad, aspecto 1.7777778, near 1, far 500000 y desplazamientos 0.

La vtable de esta cámara tenía update RVA `0x12904d0` en slot `+0x18`. La ruta copia al objeto de vista:

| Campo de vista | Uso observado |
| --- | --- |
| +0x50 / +0x54 / +0x58 | FOV Y / near / far |
| +0x5c / +0x60 / +0x6c | width / height / aspecto |
| +0x74 / +0x78 | jitter X / Y en píxeles |
| +0x94 | modo de proyección; perspectiva 0 |
| +0x98 / +0x9c | desplazamientos adicionales, aquí 0 |

El helper observado usa RVA `0x23a84b0` para construir perspectiva y puede aplicar después una transformación de convención. El ensayo recuperó **160 muestras**, 19 objetos de vista compatibles, un modelo estable de escala/profundidad y tres matrices completas por el jitter. Los contadores de llamadas al helper **no son FPS**. Varios objetos comparten parámetros: esto no identifica por sí solo el constant buffer del draw que escribió el depth capturado.

Matriz row-vector, con vector de vista multiplicado por la izquierda:

```text
0.572404087   0            0                0
0             1.01760733   0                0
jx            jy          -0.00000202655792 1
0             0            1.00000203       0
```

En consecuencia `w=z`, `deviceDepth=A+B/z` y **`z=B/(deviceDepth-A)`**. Es reverse-Z, con Z positivo hacia delante. El FOV derivado es **89.0000035° vertical / 120.4260035° horizontal**, aspecto ~1.77777789. Los coeficientes observados son la referencia para invertir profundidad; no sustituirlos por una matriz ideal fabricada con el nombre «120°» del preset.

Near derivado ~1.000000003 y extremo far ~493448.53 unidades del juego. El campo nominal far es 500000: la diferencia se reproduce con redondeo float32 al formar y convertir los coeficientes (`1-f/(f-n)` sufre cancelación). No demuestra un cambio de configuración. Tampoco convierte unidades del juego a metros.

Con jitter habilitado: `P[8]=2*jitterX/width`, `P[9]=-2*jitterY/height`. Residuo máximo observado ~1.13e-11 NDC; desplazamiento máximo de eje ~0.25 píxel. Cuando el helper deshabilita jitter, la matriz no incorpora los campos aunque estos no sean cero. La sonda de proyección y la captura de depth se ejecutaron en intervalos distintos: **el jitter de ese frame no está asociado**.

## Reconstrucción del par privado

Se repitió la captura V4 sin recompilar ni sobrescribir DLLs mapeadas: color/depth 2560×1440, plano 0 de profundidad, una lista privada en un único envío, estados restaurados y siete hooks retirados. Informe local `evidence/pair-projection-5440.json`.

| Dato | SHA-256 |
| --- | --- |
| Color raw | `34fd0d53b728e2b03ea4de7110b2c5d8b2fbad45ad650aa045c5eb91e0ed6055` |
| Depth raw | `117c14fd5fffc72c0e46d0a5124d76d461afb41ff86f7c035ec50214db9aa4d1` |
| Informe de proyección | `9e08bf8a57fda56a61660829968040220e78f447bc48c224636928a607c10cc9` |

`tools/analyze_projection.py` verifica el informe y reconstruye Z y puntos desde centros de píxel. Genera NPY, PLY coloreado, JSON y opcionalmente PNG con Matplotlib. En esta captura todos los valores reconstruyen delante de la cámara; percentiles de distancia Z [mínimo, 1 %, mediana, 99 %, máximo]: **[5.3221, 5.7187, 31.2954, 178.8712, 416.3541] unidades del juego**. Se inspeccionó la nube de 57600 puntos, con estructuras de la misión reconocibles. El color contiene HUD y no está calibrado para HDR.

El roundtrip algebraico tiene error máximo ~1.11e-16. Valida consistencia de las fórmulas, no es una medida independiente del error geométrico ni de sincronización con el render final.

## Referencia estéreo y ruta GPU offline

`tools/stereo_reprojection.py` usa dos cámaras paralelas desplazadas en X. Reconstruye profundidad, proyecta los centros a la imagen destino, conserva el más próximo y deja una máscara explícita de huecos. Ambos ojos usan la misma proyección de origen, por lo que sus offsets se cancelan algebraicamente. Esta simplificación no cubre las proyecciones asimétricas de OpenXR.

La prueba utiliza **baseline experimental de 0.5 unidades del juego**, ojos en -0.25/+0.25; no es un IPD medido ni una escala validada para comodidad. Entrada 2560×1440, salida 1280×720 por ojo. Cobertura: **97.4478 % izquierda, 97.8952 % derecha**, 95.5494 % cubierta en ambos. Imágenes distintas con paralaje; huecos conservados. HUD, transparencias y superficies ocultas requieren tratamiento adicional. No hay relleno ni recuperación de geometría ausente.

La revisión detectó que la implementación inicial multiplicaba/dividía por Z antes de discretizar: al ampliar resolución, el redondeo podía desplazar un centro un píxel dependiendo del depth aun con traslación cero. Se reprodujo con un test y se corrigió cancelando esos términos antes del cálculo de píxel.

`native/stereo_reprojection` es un ejecutable **independiente** que carga esa captura desde disco. Cuatro dispatches HLSL: limpiar; seleccionar profundidad más cercana con atomic min; seleccionar índice origen mínimo en empates; resolver color. Hay barreras UAV entre fases y lectura CPU solo tras fence. Usa hardware D3D12, sin WARP, handles del juego, hooks ni sesión OpenXR. El límite de cada imagen es 65535 grupos × 256 píxeles, con ejes hasta 16384.

La primera ordenación GPU por Z reconstruido float32 colapsaba algunos valores depth adyacentes y elegía otro origen. Se añadió un fixture que fallaba y se corrigió ordenando por el depth original, con signo según forward/reverse-Z. Esto conserva la precisión del recurso. La salida se contrasta con índices de referencia CPU y con el color exacto del origen elegido.

La revisión también encontró que FXC adelantaba el `Load` de un ternario, leyendo con índice `0xffffffff` en huecos. Se limita el índice antes de construir la dirección de lectura; el bytecode final inspeccionado contiene `umin` antes del `ld_raw`. Los root descriptors no comprueban límites automáticamente. El caso de ordenación de profundidades adyacentes y los huecos están en las fixtures conservadas.

La medición de GPU abarca esos cuatro dispatches y sus barreras. Excluye carga de archivos, compilación, creación de recursos, upload, readback, transporte vivo y compositor. No es latencia VR ni un benchmark sostenido. El debug layer D3D12 no está disponible en esta máquina; no se afirma validación de esa capa.

Ensayo final privado `evidence/stereo-gpu-verified-5440-b05-summary.json`, RTX 4070 Ti: 0.240448 ms izquierda / 0.10736 ms derecha en ese alcance de timestamps. Misma cobertura que CPU, color exactamente correspondiente al índice elegido. Quedaron 28/48 índices diferentes entre CPU float64 y GPU float32 (menos del 0.006 % de cada imagen); en todos los casos algún candidato estaba a menos de 0.000058 píxel de un límite entero. No se declara igualdad bit a bit de los dos rasterizadores. Se inspeccionaron las imágenes finales por ojo y la comparación.

Verificación: 23 tests Python, 12 tests CTest de los siete proyectos nativos (incluyendo 17 casos GPU dentro del test estéreo), análisis sintáctico de los 16 scripts y `git diff --check` correctos. La auditoría de solo lectura `evidence/projection-stereo-final-audit-5440.json` confirmó juego respondiendo, prólogos originales de cámara/proyección y ambas sondas inactivas. No se modificaron archivos del juego ni su resolución.

## Repetición local

MinHook en `reference/minhook`, MSVC/SDK y CMake para la sonda. NumPy/Pillow para análisis, Matplotlib opcional. Las dependencias de análisis están en `.venv-analysis`, excluido del repo.

```powershell
cmake -S native\projection_probe -B native\projection_probe\build -G 'Visual Studio 17 2022' -A x64
cmake --build native\projection_probe\build --config Release
ctest --test-dir native\projection_probe\build -C Release --output-on-failure
python -B tools\load_camera_probe.py --pid <PID-NUEVO> --single-player-confirmed --dll native\projection_probe\build\Release\ac6_projection_probe.dll --capture evidence\projection-new-screen.png --output evidence\projection-new.json
```

Solo en una sesión nueva sin esa DLL cargada. Para analizar el par guardado, suministrar el informe correspondiente a los raws y una proyección compatible; los ejemplos conservan nombres de la sesión local, no datos distribuibles:

```powershell
.\.venv-analysis\Scripts\python.exe -B tools\analyze_projection.py --projection-report evidence\projection-native-5440.json --pair-report evidence\pair-projection-5440.json --color native\frame_pair_probe\build\Release\frame_pair_probe_v4_5440-color.raw --depth native\frame_pair_probe\build\Release\frame_pair_probe_v4_5440-depth.raw --output-prefix evidence\reconstruction-new --plot
cmake -S native\stereo_reprojection -B native\stereo_reprojection\build -G 'Visual Studio 17 2022' -A x64
cmake --build native\stereo_reprojection\build --config Release
ctest --test-dir native\stereo_reprojection\build -C Release --output-on-failure
.\.venv-analysis\Scripts\python.exe -B tools\reproject_stereo_pair.py --projection-report evidence\projection-native-5440.json --pair-report evidence\pair-projection-5440.json --color native\frame_pair_probe\build\Release\frame_pair_probe_v4_5440-color.raw --depth native\frame_pair_probe\build\Release\frame_pair_probe_v4_5440-depth.raw --output-prefix evidence\stereo-new --baseline-game-units 0.5 --gpu-exe native\stereo_reprojection\build\Release\ac6_offline_stereo.exe
```

Omitir `--gpu-exe` para la referencia CPU. Elegir siempre prefijos nuevos. El wrapper genera archivos de entrada binarios privados con firma `AC6RPRJ1`, Params de 48 bytes (`<4I6f2I`: sw/sh/tw/th, fx/fy/A/B/Wsign/eyeX, dos reservas), depth float32 contiguo y color RGBA8. El ejecutable devuelve índices uint32 y color RGBA8, con `0xffffffff` / color cero para huecos. Las máscaras y el JSON permiten inspeccionar cobertura, tiempos y diferencias. Los datos y capturas permanecen en rutas ignoradas por Git.

## Desarrollo de presentación binocular: historial de la fase inicial

### Prueba de escena fija en PSVR2

Se amplió el diagnóstico OpenXR existente con `--stereo-left` y `--stereo-right`, dos raws RGBA8 de 1280×720. El lector exige ambos archivos completos, conserva orden/canales y hace opacos los huecos negros. Las opciones son incompatibles con captura del PID y seguimiento de cámara. Se presentan dos capas quad con visibilidad LEFT/RIGHT, situadas en LOCAL a 2 m, tamaño 2.4×1.35 m. Son una pantalla estereoscópica, sin adaptación a FOV asimétricos del visor ni movimiento de cámara en el juego.

Ensayo `evidence/xr-static-stereo-5440-01.jsonl`: 20 segundos desde VISIBLE; 1608 envíos, 1607 visibles y 1448 enfocados. Sesión cerrada, código 0. El usuario confirmó: **«Se veía con profundidad y sin imagen doble»**. Esto valida percepción de relieve y fusión de esa escena fija; no acredita comodidad prolongada, escala real ni estereoscopía en combate.

La auditoría de entradas y previews verifica RGB exacto por ojo. SHA-256 del ejecutable utilizado: `d12b671e3d2e4fd63d0164cf9f73894cc7181ffd9eb9414b42c187ab60b132a1`. No se cargaron sondas ni se enviaron controles al juego; AC6 seguía respondiendo al finalizar. Tres tests del presentador pasan, incluido el nuevo test de orden, alfa y tamaño del par (fallo inicial observado antes de implementar).

```powershell
native\xr_display_probe\build\Release\ac6_xr_display_probe.exe --seconds 20 --output evidence\xr-static-stereo-new.jsonl --stereo-left evidence\stereo-gpu-verified-5440-b05-left-gpu.rgba.raw --stereo-right evidence\stereo-gpu-verified-5440-b05-right-gpu.rgba.raw
```

Ponte el visor y cierra el panel SteamVR antes de empezar. F12 cancela. Hay un margen de 30 segundos para alcanzar VISIBLE; usar un prefijo nuevo para el log y ambos BMP de preview. No requiere que el juego siga abierto. Los raws son la captura privada descrita arriba, no archivos incluidos en el repositorio.

### Integración continua: transición posterior del monitor al visor

El productor D3D12 ya comparte pares continuos color/depth mediante recursos y fences; otro proceso genera dos ojos en GPU sin readback en la ruta normal. El ensayo de monitor ha superado esta parte técnica (ver `LIVE_STEREO_FINDINGS.md`). **Actualización del 18-10-2026:** el paso al visor ya está confirmado con profundidad y escena viva (V7), junto con orientación de cabeza (V8) y prueba de salida 2560×1440/90° (V11). Sigue pendiente asociar proyección/jitter y cámara al mismo snapshot, establecer metros por unidad, reproyectar con poses y FOV asimétricos de cada ojo, tratar HUD/oclusiones y medir latencia/comodidad. La orientación de cabeza confirmada previamente no sustituye estas validaciones.

Referencias primarias usadas para la ruta GPU: [InterlockedMin y ausencia de barrera implícita](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/interlockedmin), [barreras UAV y estados D3D12](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12), [root descriptors y ausencia de comprobación de límites](https://learn.microsoft.com/en-us/windows/win32/direct3d12/root-signatures-overview).
