# Captura continua y reproyección GPU — 16-10-2026

**Estado: prueba continua en monitor superada; prueba de imagen continua en PSVR2 pendiente.** Ruta experimental posterior a la escena fija confirmada por el usuario. Tres buffers DEFAULT compartidos llevan pares color/depth desde AC6 hasta otro proceso D3D12. Ese proceso reconstruye dos ojos en GPU, sin lectura de píxeles en CPU salvo previews diagnósticos. El modelo perspectiva y baseline siguen siendo los de la investigación previa; no hay escala métrica calibrada, proyecciones asimétricas ni tratamiento del HUD.

## Productor y sincronización

`native/live_pair_stream` reutiliza la identificación de recursos y ledger de listas de la sonda de pares. Cada slot tiene listas privadas pregrabadas por backbuffer. En un envío elegible se añade la copia de color/depth a **la misma llamada ExecuteCommandLists**, después de las listas del juego. Depth vuelve a estado 192, color a PRESENT y buffer compartido a COMMON. Una transición nueva de depth es necesaria después de cada captura o descarte.

Dos fences compartidas: ready tras escribir el slot; released tras terminar el consumidor. La reutilización exige ambas completadas para su secuencia previa. Si el anillo está lleno, se omite la captura; nunca se espera a la GPU, se crean recursos, se mapean píxeles ni se escriben archivos dentro del callback de envío. Un único consumidor elige la secuencia más reciente, espera ready en su cola, lee el buffer, lo devuelve a COMMON y señala released. Omitir secuencias anteriores es seguro solo con estas condiciones y un consumidor.

Metadata versionada de 120 bytes, acceso mediante Interlocked a campos compartidos. Offset 36 permite solicitar salida anticipada del worker; límite de 60 s y F12 independientes. El productor publica fallos por resize, cola distinta, dispositivo perdido y agotamiento del ledger. Los recursos, mapping y DLL se conservan inactivos al retirar hooks para no liberar trabajo en vuelo. Una prueba nueva requiere otro nombre de DLL; no se sobrescribe ni recarga una DLL mapeada.

## Consumidor

`native/live_stereo` abre handles con nombre y valida versión, rangos, LUID, descriptores y exclusividad. Soporta entrada R10G10B10A2, RGBA8 y BGRA8 con pitches/offsets. Cuatro fases de compute por ojo, ordenación por depth original e índices acotados antes de Load. Salida RGBA/BGRA 1280×720, huecos negros opacos. `Blit` copia por GPU a una textura RENDER_TARGET y restaura ese estado.

Espera síncrona de GPU en el **proceso consumidor**, acotada a 5 s. Un fallo en vuelo termina ese proceso con recursos retenidos, nunca AC6. Las imágenes caducan a los 500 ms; las publicaciones viejas se retiran sin leer sus píxeles para liberar un anillo envejecido. El nuevo consumidor excluye secuencias ya liberadas por el anterior.

La integración OpenXR usa capas quad como la prueba fija: LOCAL, 2 m, 2.4×1.35 m. `--headtracking` es un ensayo posterior que publica orientación y usa VIEW; no asumir que validar la pantalla valida también esa combinación. La proyección fija no se asocia todavía al constant buffer del mismo snapshot. No equivale a volver a renderizar geometría para dos ojos.

## Verificación antes del visor

- Tres CTests del productor: límites/frescura/propiedad; pares GPU cambiantes con saturación, descarte, reanudación y resize; gate GPU pendiente, envío único, rechazo de color sin depth nuevo y publicación de fallo. El gate observa lotes simulados para comprobar la decisión, y el otro fixture comprueba copias reales.
- Dos CTests del consumidor: procesos y dispositivos D3D12 separados, formatos/padding, colores/previews/blits exactos, paralaje y oclusión, fences retrasadas, saltos, reconexión, stale/Stopped y terminación acotada en vuelo. Debug layer no disponible en el entorno.
- Revisión del productor: se corrigió el agotamiento de ledger que dejaba el estado Ready sin capturas. La regresión se observó fallar sin la corrección y pasar con ella. Revisión de gates sin nuevos hallazgos de seguridad.
- Dos tests Python nuevos comprueban solicitud de stop limitada al protocolo/PID y lectura de logs parciales. No se reutilizan direcciones heap de otras sesiones.

Ensayo monitor `evidence/live-stereo-monitor-v1-5440-result.json`, misión offline por cooperativo, PID 5440, build SHA-256 `e003771e5548bca91882c1d7dfe2df3de97cee05c2c989d0c0e34b036ea610d6`:

| Dato | Resultado |
| --- | --- |
| Productor V1 | 721 pares publicados, 120 descartes; una cola, ledger íntegro |
| Consumidor, ventana de 6 s | 414 actualizaciones, secuencias 4–718 |
| Entrada / salida | 2560×1440 / 1280×720 por ojo |
| Diferencias RGB primera/última izquierda | 26.4379 % de píxeles |
| Diferencias RGB entre ojos, última | 53.6206 % de píxeles |
| Salida / retirada | Ambos procesos código 0, siete hooks restaurados, DLL V1 inactiva |
| SHA-256 DLL V1 | `7830840a2ada681223910b04d307a0b936719fbf13d03d7657bf5d65de88e82a` |
| SHA-256 monitor | `02b12e56e1070fdc4bf5702176e6626efe068a047b8aa5ed60b7bb8a25dfb098` |

Se inspeccionaron previews de ambos ojos al principio y al final. Los contadores no representan frecuencia del visor ni miden latencia de movimiento; el consumidor puede saltar secuencias. Se leen píxeles solo para esos previews.

## Repetición

### Corrección al reanudar (18-10-2026)

La revisión independiente detectó una carrera de frescura: la imagen podía caducar después de la comprobación del frame mientras OpenXR esperaba un swapchain. El antiguo `Blit` lanzaba excepción, dejando pendiente la liberación del ojo y el cierre del frame. `TryBlit` ahora devuelve false sin trabajo GPU para una fuente no disponible; el presentador libera cada imagen esperada y descarta **los dos ojos**, llamando a EndFrame con cero capas. También invalida la pose publicada si se descarta la fuente. Los errores de render liberan la imagen y cierran el frame antes de propagarse.

Se observaron fallos de regresión antes de implementar tanto en el fixture GPU de imagen caducada como en el test de transacción de ojos. Verificación posterior: tres CTests del productor, dos del consumidor, cuatro del presentador y 25 tests Python correctos; análisis sintáctico de 17 scripts. Esta reanudación aún no añade un resultado visual de streaming en PSVR2. Contrato de referencia: [gestión de imágenes y frames de OpenXR](https://raw.githubusercontent.com/KhronosGroup/OpenXR-Docs/main/specification/sources/chapters/rendering.adoc).

### Construcción y ejecución

Compilar consumidor, presentador y una DLL **aún no mapeada**:

```powershell
cmake -S native\live_stereo -B native\live_stereo\build -G 'Visual Studio 17 2022' -A x64
cmake --build native\live_stereo\build --config Release
ctest --test-dir native\live_stereo\build -C Release --output-on-failure
cmake -S native\live_pair_stream -B native\live_pair_stream\build -G 'Visual Studio 17 2022' -A x64 -DLIVE_PAIR_PROBE_NAME=ac6_live_pair_probe_v2
cmake --build native\live_pair_stream\build --config Release
cmake --build native\xr_display_probe\build --config Release --target ac6_xr_display_probe
```

PID, versión y tag deben corresponder a la sesión real. El script valida identidad/cooperativo/EAC conocido, espera al productor, abre el consumidor y solicita retirar la captura al terminar. No modifica archivos del juego ni fuerza descarga de DLLs.

```powershell
.\.venv-analysis\Scripts\python.exe -B tools\run_live_stereo_trial.py --pid <PID> --single-player-confirmed --dll native\live_pair_stream\build\Release\ac6_live_pair_probe_v2.dll --tag live-new --mode monitor
```

Para el **siguiente gate de visor**, repetir la invocación anterior con un nombre de DLL nuevo, tag nuevo y `--mode vr`, con visor puesto y SteamVR sin paneles. La imagen dura 20 s desde VISIBLE. **Criterio de éxito:** ambos ojos reciben una escena que cambia al moverse el juego, la profundidad resulta fusionable sin imagen doble y no se observan congelaciones ni fallos de sesión durante la ventana. Conservar el `*-result.json`, `*.jsonl` del presentador y el resumen nativo, junto con una breve descripción literal de lo observado por el usuario (incluidas molestias o defectos); los contadores por sí solos no acreditan comodidad/fusión. `--headtracking` añade la sonda de cámara de 15 s y exige que esta tampoco esté mapeada; no activarla hasta comprobar primero la imagen viva estéreo. Datos del juego, logs y capturas quedan fuera del repositorio.

Referencias primarias: [handles compartidos D3D12 y namespace](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createsharedhandle), [apertura por nombre](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-opensharedhandlebyname).
