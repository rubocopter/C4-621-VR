# Prueba acotada vigente de estéreo continuo

Reproduce el perfil aceptado en PSVR2. Consultar el [estado verificado](STATUS.md);
una edición documental no requiere otra prueba física. Ejecutar únicamente
con prueba acordada, usuario presente, misión individual offline, visor
preparado y SteamVR ya activo. No iniciar juego ni SteamVR automáticamente.

## Preparar binarios aislados

Desde la raíz del repositorio, con dependencias de [BUILDING](../BUILDING.md).
Cada intento usa directorio y nombres de sondas nuevos. No recompilar, copiar
encima, reutilizar ni descargar DLLs que puedan estar mapeadas. No modificar
archivos del juego. Estos comandos solo compilan; no cargan sondas:

```powershell
$ErrorActionPreference = 'Stop'
$trialVersion = Get-Date -Format yyyyMMddHHmmss
$trialRoot = Join-Path (Get-Location) "out/trial-$trialVersion"
if (Test-Path -LiteralPath $trialRoot) { throw 'El directorio ya existe: elegir otro intento.' }

cmake -S native/xr_display_probe -B "$trialRoot/display" -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE) { throw 'Fallo de configuracion del presentador.' }
cmake --build "$trialRoot/display" --config Release --target ac6_xr_display_probe --parallel 2
if ($LASTEXITCODE) { throw 'Fallo de compilacion del presentador.' }

cmake -S native/live_pair_stream -B "$trialRoot/producer" -G 'Visual Studio 17 2022' -A x64 "-DLIVE_PAIR_PROBE_NAME=ac6_live_pair_probe_v$trialVersion"
if ($LASTEXITCODE) { throw 'Fallo de configuracion del productor.' }
cmake --build "$trialRoot/producer" --config Release --target ac6_live_pair_probe --parallel 2
if ($LASTEXITCODE) { throw 'Fallo de compilacion del productor.' }

cmake -S native/camera_probe -B "$trialRoot/camera" -G 'Visual Studio 17 2022' -A x64 "-DHEADTRACKING_CALIBRATED_NAME=ac6_headtracking_calibrated_probe_v$trialVersion"
if ($LASTEXITCODE) { throw 'Fallo de configuracion de camara.' }
cmake --build "$trialRoot/camera" --config Release --target ac6_headtracking_calibrated_probe --parallel 2
if ($LASTEXITCODE) { throw 'Fallo de compilacion de camara.' }
```

Los cargadores comprueban de nuevo ruta/hash, módulo cooperativo esperado,
EAC conocido y prólogo. El launcher no acredita el estado de red: confirmar
misión individual offline, nunca matchmaking oficial. No adaptar offsets ni
eludir rechazos para otra build. Usar PID actual, no el de un informe antiguo.

## Ejecutar una sola ventana

Conservar las variables de la terminal anterior. El Python del ejemplo es el
entorno del proyecto; otro intérprete requiere las mismas dependencias.
Después de confirmar las condiciones anteriores:

```powershell
Get-Process armoredcore6 | Select-Object Id, ProcessName
[int]$gamePid = Read-Host 'PID actual de la mision offline confirmada'
.\.venv-analysis\Scripts\python.exe -B tools/run_live_stereo_trial.py --pid $gamePid --single-player-confirmed --dll "$trialRoot/producer/Release/ac6_live_pair_probe_v$trialVersion.dll" --consumer-exe "$trialRoot/display/Release/ac6_xr_display_probe.exe" --camera-dll "$trialRoot/camera/Release/ac6_headtracking_calibrated_probe_v$trialVersion.dll" --tag "trial-$trialVersion" --mode vr --headtracking --eye-width 2560 --eye-height 1440 --rotation-limit-deg 90
```

`--consumer-exe` elige la build aislada y registra su ruta y SHA-256; rechaza
una ruta inválida sin recurrir a otro ejecutable. Se comprobó con orquestación
host después de la última aceptación física; aquella usó staging del consumidor.

La ventana dura 27 s desde VISIBLE, con margen de 30 s para obtener visibilidad
y 15 s de seguimiento (2 s iniciales y 3 s finales de observación). Cerrar el
panel SteamVR, mantener la cabeza quieta al establecer el centro y girar
despacio entre borde cercano y fondo. **F12 cancela**. Sin traslación; girar
no mueve el cuerpo del AC. Usar mando convencional, sin movimiento automático
ni controles Sense.

## Aceptar o detener

Registrar literalmente si hay escena viva con profundidad fusionable, sin
imagen doble ni congelaciones, y giro correcto, incluyendo defectos/molestias.
Un código de salida correcto no sustituye esa observación. No inferir escala
métrica, geometría exacta, rendimiento o comodidad prolongada de esta ventana.

Conservar evidencia con prefijo nuevo: `*-result.json`, JSONL de presentación,
resumen nativo y `*-camera.json`; ruta/hash deben corresponder al consumidor
elegido. Comprobar códigos de salida, desactivación, restauración de hooks y
prólogo, ausencia de excepciones y juego respondiendo. Las DLLs quedan
retenidas inactivas: **no forzar FreeLibrary ni sobrescribirlas**. No publicar
capturas/datos propietarios.

Ante múltiples colas no verificadas, fallo del ledger, identidad inválida,
pérdida de captura o restauración fallida, detener y conservar registros.
No repetir cargas a ciegas ni quitar límites, frescura o protecciones para
conseguir imagen. La segunda cola sigue sin causa aislada; desactivar
overlays no es una corrección demostrada. Una nueva prueba exige revisar el
estado y usar recursos nuevos no mapeados.
