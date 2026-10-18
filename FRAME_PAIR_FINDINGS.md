# Par de color y profundidad D3D12

15-10-2026, AC6 1.9.0.0, sesión individual offline confirmada por el usuario, PID 17628. Se recuperaron **color y profundidad en una sola lista privada de GPU**, añadida al mismo envío gráfico del juego. Es un diagnóstico puntual de ocho segundos, no una ruta persistente de estéreo.

Informe de una captura histórica; [estado vigente](docs/STATUS.md) y
[procedimiento de estéreo continuo](docs/VR_TRIAL_RUNBOOK.md).

## Resultado real

La versión final V4 escribió `evidence/pair-live-v4-17628.json` y dos raws privados. El cargador terminó con código 0.

| Propiedad | Resultado |
| --- | --- |
| Resolución | 2560×1440 en ambos recursos |
| Color | Backbuffer `R10G10B10A2_UNORM` (24) |
| Depth | Plano 0 de `R32G8X24_TYPELESS` (19), footprint `R32_TYPELESS` (39) |
| Tamaño | 14 745 600 bytes por raw, pitch 10 240 |
| Punto de copia | Final de un envío gráfico que termina un backbuffer en PRESENT |
| Estado depth restaurado | 192: PIXEL_SHADER_RESOURCE + NON_PIXEL_SHADER_RESOURCE |
| Envíos privados | Uno, cola DIRECT única observada |
| Depth | 100 % finito y en [0,1], rango ~0.00055–0.05905 |
| Retirada | Siete hooks restaurados, DLL inactiva, cámara original y juego responde |
| SHA-256 DLL V4 | `7ec908c65f55d374a515554061c9a2d1d42e8f238069bf46f7f4182a0deaf202` |

Se inspeccionaron el color, el mapa normalizado y los contornos de profundidad superpuestos. Pilares, barandillas y bordes del suelo se corresponden cualitativamente con la escena. No se ha medido el error de alineación. HUD y transparencias no tienen por qué estar representados en este depth.

Color SHA-256: `b38f7601b9af71ceaa99cb5ca2c4bd208360fd8c9bd9a3bb5e676a77d7870e38`. Depth SHA-256: `7a90f232b2f0898de1267627a2b13add61f363926f767c129d736fc684391ec4`. La auditoría local final es `evidence/frame-pair-final-audit-17628.json`.

## Por qué hace falta seguir las listas

La primera aproximación perdía observaciones por un mutex compartido entre todos los grabadores. V2 eliminó esa contención, pero observó 579536 transiciones de depth y 724 finales de color sin encontrar ambos en una misma lista. La captura se omitió y los hooks se retiraron. AC6 distribuye esos trabajos entre listas distintas.

La implementación final conserva resúmenes por generación de lista: Reset inicia una grabación conocida; Close la sella; grabaciones desconocidas, aliasing o una dirección reutilizada sin Reset no suministran estados fiables. Al ejecutar se aplican los resúmenes en orden. Un efecto desconocido invalida el estado de profundidad hasta que una transición explícita vuelva a establecerlo. Otra cola impide capturar.

Un mutex breve serializa **el encolado CPU real y la reconstrucción de estados** durante este diagnóstico. Todos los callbacks lo respetan, también en el estado Capturing. Una regresión con un segundo hilo reprodujo el bypass anterior y pasó tras corregirlo. El mutex no cubre creación de recursos, esperas GPU, Map ni escritura de archivos.

La lista privada ya está preparada antes de armar la captura. Solo se añade cuando los estados observados coinciden con sus barreras y se validan descriptor e identidad actual del backbuffer. Se envía en la **misma llamada ExecuteCommandLists** después de las listas del juego: no hay una llamada separada en la que pueda adelantarse otro hilo. Ambas texturas se copian y sus estados se restauran en esa lista. Las listas originales del juego no se modifican ni heredan copias al reutilizarse.

Una fence confirma el final de la copia. El worker comprueba que ya no puede repetirse, que salieron los callbacks y que no se ha registrado fallo antes de leer. Un fallo queda latched y no puede deshacerse publicando un estado anterior. Cambiar tamaño cancela la preparación; no se retienen referencias GetBuffer. Ante un fallo de GPU se conservan los recursos necesarios en vez de forzar su descarga.

La revisión de código detectó problemas de concurrencia y retirada que se corrigieron. La última revisión acotada no encontró bloqueos en el mecanismo final. Esto no equivale a validación del debug layer ni a estabilidad prolongada.

## Repetición

Windows x64, CMake, MSVC/Windows SDK y el checkout local de MinHook en `reference/minhook` son necesarios. El nombre por defecto sirve en una sesión nueva; no sobrescribir ni recargar una DLL ya mapeada.

```powershell
cmake -S native\frame_pair_probe -B native\frame_pair_probe\build -G 'Visual Studio 17 2022' -A x64 -DFRAME_PAIR_PROBE_NAME=ac6_frame_pair_probe
cmake --build native\frame_pair_probe\build --config Release --target test_pair_gate test_pair_ledger test_frame_pair ac6_frame_pair_probe
ctest --test-dir native\frame_pair_probe\build -C Release --output-on-failure
python -B tools\load_camera_probe.py --pid <PID> --single-player-confirmed --dll native\frame_pair_probe\build\Release\ac6_frame_pair_probe.dll --capture evidence\pair-new-screen.png --output evidence\pair-new.json
```

Elegir PID, nombres de evidencias y sesión individual correctos. El cargador verifica ejecutable/hash, launcher cooperativo, ausencia de módulos EAC conocidos, prólogo y que la DLL no esté ya cargada. No cambia archivos del juego. F12 permite terminar antes.

Para analizar el par, con NumPy/Pillow en el entorno local:

```powershell
.\.venv-analysis\Scripts\python.exe -B tools\analyze_frame_pair.py --report evidence\pair-new.json --color native\frame_pair_probe\build\Release\frame_pair_probe_<PID>-color.raw --depth native\frame_pair_probe\build\Release\frame_pair_probe_<PID>-depth.raw --output-prefix evidence\pair-new-preview
```

Se guardan PNG de color/depth/contornos, una comparación, depth `.npy` sin normalizar y JSON de metadatos/hashes. La gamma y los percentiles se aplican solo a la previsualización. Los raws, capturas y datos del juego permanecen excluidos del repositorio.

## Límites y siguiente etapa

La copia conjunta demuestra una muestra coherente de los dos recursos en ese punto de la cola. No identifica el número de frame interno del motor, ni prueba que no haya más dibujo después del envío elegido o que un efecto use datos temporales. Una investigación posterior observó el modelo de proyección y la fórmula de jitter, pero no los asoció al mismo frame de esta captura. Profundidad métrica, escala del AC/IPD y colorimetría/HDR siguen pendientes.

En aquella etapa la imagen VR en vivo aún era monoscópica y se generaban vistas separadas offline en CPU/D3D12; véase [PROJECTION_FINDINGS](PROJECTION_FINDINGS.md). Posteriormente se aceptó estéreo continuo reproyectado con orientación en PSVR2. La asociación temporal de proyección/jitter y las poses/FOV asimétricos del visor siguen pendientes; el [estado actual](docs/STATUS.md) delimita su alcance.

Referencias primarias: [estados y barreras D3D12](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12), [ExecuteCommandLists](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-executecommandlists), [grabación, reutilización y referencias](https://learn.microsoft.com/en-us/windows/win32/direct3d12/recording-command-lists-and-bundles), [ResizeBuffers](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-resizebuffers).
