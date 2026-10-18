# Prueba acotada de orientación PSVR2

> **Status update (18 October 2026):** The current PSVR2 prototype has already demonstrated **live depth-reprojected stereo combined with calibrated head orientation**. A bounded 2560×1440-per-eye, 90° trial was reported clearer and without noticeable stutter. This remains a time-limited proof of concept, not a complete VR conversion or proof of sustained comfort. See [README.md](README.md), [LIVE_STEREO_FINDINGS.md](LIVE_STEREO_FINDINGS.md) and [QUALITY_PACING_FINDINGS.md](QUALITY_PACING_FINDINGS.md). Historical procedures and pending gates below document the state at the time of each experiment.


**Documento histórico del ensayo en monitor.** Para la prueba conjunta dentro del visor y la conversión de ejes corregida, usar `VR_DISPLAY_FINDINGS.md` y `tools/run_vr_trial.py`. La ruta Python de este documento usa `flip_z=false` por defecto y no incorpora automáticamente la calibración posterior.

Resultado del 15-10-2026: el usuario vio que la cámara del juego seguía el visor en el monitor. No se envía imagen al visor. La prueba termina sola; no instala un mod permanente.

## Condiciones y límites

- Solo el ejecutable con SHA-256 `e003771e5548bca91882c1d7dfe2df3de97cee05c2c989d0c0e34b036ea610d6`, launcher cooperativo y misión individual. El cargador comprueba ruta, hash, módulo cooperativo y prólogo; no puede determinar el estado de red.
- SteamVR activo, PSVR2 con seguimiento válido y OpenXR de SteamVR seleccionado. Mantener el AC y los controles de vista quietos durante la comparación.
- Centro en la primera pose válida, giro máximo 25°, sin desplazamiento de cabeza. Sonda: 2 s de observación, 15 s activa, 3 s finales y desactivación. F12 cancela.
- Las DLL quedan mapeadas e inactivas hasta cerrar el juego. Para repetir este experimento usar una sesión nueva del juego y su PID actual; no reutilizar `32616` por defecto ni forzar `FreeLibrary`. Si el PID se reutiliza y existe el log nativo con ese número, conservarlo con otro nombre antes del ensayo.
- No recompilar DLL que el juego tenga cargadas. El código fuente y las evidencias se mantienen en el proyecto; no se copiaron ficheros a la instalación del juego durante las sondas.

## Compilación y pruebas

Desde `E:\Proyectos\ac6_621`, con MSVC Build Tools y CMake disponibles:

```powershell
cmake -S native\camera_probe -B native\camera_probe\build -G 'Visual Studio 17 2022' -A x64
cmake --build native\camera_probe\build --config Release --target ac6_headtracking_probe test_yaw
ctest --test-dir native\camera_probe\build -C Release --output-on-failure
python -B -m unittest discover -s tests -v
```

## Repetición coordinada

Elegir nombres nuevos para todas las evidencias. En una primera terminal, sustituir `<PID>` y arrancar el productor. Mantener el visor inmóvil hasta que exista la primera muestra en el JSONL; esa pose define el centro.

```powershell
python -B tools\probe_openxr_headless.py --seconds 40 --publish-pid <PID> --output evidence\openxr-retest-01.jsonl
```

Mientras el productor siga activo, ejecutar en otra terminal:

```powershell
python -B tools\load_camera_probe.py --pid <PID> --single-player-confirmed --dll native\camera_probe\build\Release\ac6_headtracking_probe.dll --capture evidence\headtracking-retest-01.png --output evidence\headtracking-retest-01.json
```

Los marcadores `<PID>` deben reemplazarse por un número; no son sintaxis PowerShell ejecutable. Tras los 2 s iniciales, girar suavemente el visor observando el monitor. El cargador toma dos capturas, espera la desactivación y lee el prólogo original. Comprobar `auto_disabled=true`, `prologue_restored=true` y ausencia de excepciones, además de la observación visual. Conservar también el log `headtracking_probe_<PID>.jsonl` junto a la DLL.

No interpretar el número de aplicaciones como FPS o latencia. El resultado solo valida esta ventana de orientación; faltan pruebas prolongadas, transiciones, salida al visor y dos vistas por ojo.
