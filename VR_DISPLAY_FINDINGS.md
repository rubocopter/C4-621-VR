# AC6 en PSVR2: imagen y orientación experimental

15-10-2026, SteamVR/OpenXR y adaptador oficial PC. Se mostró el juego en el visor y el usuario confirmó seguimiento horizontal/vertical correcto tras calibrar los ejes. Es un diagnóstico temporal monoscópico, no una adaptación VR terminada.

**Actualización posterior:** existe una ruta opcional de captura por GPU, una muestra real de profundidad y una corrección para seguir la cabeza en sesiones VISIBLE sin exigir foco de inputs. Estado y pruebas actuales en `RENDER_CAPTURE_FINDINGS.md`. Las cifras y la política FOCUSED descritas abajo corresponden a los primeros ensayos.

**16-10-2026:** una captura fija reproyectada por profundidad ya se presentó con imágenes distintas por ojo en PSVR2. El usuario confirmó volumen sin imagen doble (1607 frames visibles, sesión cerrada). El juego vivo sigue monoscópico. Implementación, comando y límites en [PROJECTION_FINDINGS.md](PROJECTION_FINDINGS.md#prueba-de-escena-fija-en-psvr2).

## Qué funciona y cómo se comprobó

| Ensayo | Evidencia | Resultado |
| --- | --- | --- |
| Patrón D3D12 por ojo | `evidence/xr-display-worn-01.jsonl` | 1791 frames enviados, 1699 enfocados; usuario confirmó L/R correctas |
| Ventana del juego en pantalla virtual | `evidence/xr-game-window-02.jsonl` | 1799 frames enviados, 563 capturas; usuario confirmó imagen viva sin problemas apreciables |
| Imagen y orientación, primera integración | `evidence/xr-combined-headtracking-01.jsonl` y `combined-headtracking-native-result.json` | 2391 frames enviados, 737 capturas, 1761 aplicaciones de cámara; usuario detectó inversión horizontal/vertical |
| Imagen y orientación calibrada | `evidence/xr-combined-calibrated-01.jsonl` y `calibrated-headtracking-native-result.json` | 2425 frames enviados, 2424 enfocados, 747 capturas, 1794 aplicaciones de cámara; usuario confirmó ambos ejes correctos |

Las dos sondas conjuntas registraron 0 excepciones, se desactivaron solas y restauraron el prólogo original de la copia de cámara. El juego seguía respondiendo. La comprobación inicial `xr-game-window-01` obtuvo captura pero no visibilidad en SteamVR; no cuenta como prueba visual superada.

La última confirmación del usuario fue: «si estan correctos, lo unico que tengo como muy limitado hasta donde puedo mirar». El límite de **25° de rotación total respecto al centro** es deliberado y se aplica al cuaternión, no por separado a cada eje. No es una limitación medida del visor.

## Implementación y límites

- Aplicación independiente `native/xr_display_probe/`: crea dispositivo/cola D3D12 en el adaptador LUID exigido por OpenXR; usa dos swapchains sRGB de 1280×720. Fences protegen reutilización de buffers y las texturas vuelven a `RENDER_TARGET` antes de liberarse a OpenXR.
- Windows Graphics Capture selecciona exclusivamente la ventana de `armoredcore6.exe` del PID indicado. Captura de ventana sin enviar input. Hace lectura GPU→CPU y subida CPU→GPU; se limita a ~30 capturas/s. Los ~90 envíos/s a SteamVR **no equivalen a 90 imágenes nuevas del juego/s**. Latencia aún sin medir.
- Patrón y primera imagen: capas quad en espacio LOCAL. Prueba conjunta: quad fijado al espacio VIEW, ancho 4.5 m a 1.3 m (~120° horizontales) y proporción 16:9. Usa la misma imagen del juego en ambos ojos; carece de paralaje del mundo y de profundidad estereoscópica de AC6.
- La sesión gráfica también publica orientación relativa por el transporte local del PID. Conversión calibrada `S R S`, `S=diag(1,1,-1)`, después de recentrar/limitar; invierte yaw/pitch frente a la primera prueba y conserva roll. Roll no se validó por separado en el visor.
- `ac6_headtracking_calibrated_probe.dll` consume la rotación en el hook de cámara previamente validado. Ventana de seguimiento 15 s, 2 s iniciales y 3 s finales. F12 cancela ambas piezas. Pose caduca tras 250 ms; no se aplica traslación.
- La aplicación espera hasta 30 s para colocarse el visor y cuenta la duración desde estado VISIBLE. Un ensayo sin frames FOCUSED devuelve resultado no satisfactorio. Al perder captura durante más de 500 ms oculta la imagen e invalida la orientación, registra la pérdida y devuelve resultado no satisfactorio.
- Ante un fallo de sincronización GPU con dispositivo aún activo, se termina solo el proceso de presentación sin liberar objetos COM en uso durante el desenrollado; el sistema operativo retira su contexto. Esa ruta excepcional no se ha provocado en hardware. Ninguna de estas operaciones termina el juego.
- No se modificaron archivos del juego. Las cuatro DLL diagnósticas cargadas en PID 32616 permanecen mapeadas e inactivas hasta cerrar esa sesión; no forzar su descarga.

## Repetir en una sesión nueva del juego

Compilar solo cuando el juego no tenga cargada la DLL de destino:

```powershell
cd E:\Proyectos\ac6_621
cmake -S native\xr_display_probe -B native\xr_display_probe\build -G 'Visual Studio 17 2022' -A x64
cmake --build native\xr_display_probe\build --config Release --target ac6_xr_display_probe test_frame test_head_rotation
cmake --build native\camera_probe\build --config Release --target ac6_headtracking_calibrated_probe
```

Iniciar AC6 por el launcher cooperativo en misión individual. Obtener su PID actual, elegir un nombre nuevo para evidencias y ejecutar:

```powershell
python -B tools\run_vr_trial.py --pid <PID> --single-player-confirmed --tag vr-retest-01
```

Reemplazar `<PID>` por el número real. Mantener el AC quieto, colocarse el visor mirando al frente y esperar 3 s tras aparecer la imagen antes de girar suavemente. La aplicación dura unos 27 s desde VISIBLE; el script espera FOCUSED para cargar la sonda, que aplica orientación durante 15 s y se retira. El script reutiliza la secuencia ya ensayada; su archivo se añadió después de las pruebas interactivas y se comprobó por sintaxis/ayuda, sin otra inyección de juego.

El cargador comprueba ruta/hash, cooperativo y prólogo del juego, pero la presencia del launcher no determina el estado de red. No reutilizar offsets con otra build. Para repetir la sonda es necesaria una sesión nueva: el script rechaza una DLL ya mapeada y nunca sobreescribe sus logs. Conservar las evidencias antes de mover un log si el sistema reutiliza un PID.

Verificación final: 8 tests Python y 3 tests nativos pasan; sintaxis de `tools/*.py`, ayuda del script y bloqueo de recarga comprobados. `evidence/vr-calibrated-final-audit.json` confirma nuevamente el prólogo original, los módulos diagnósticos y el hash del ejecutable de presentación posterior a la revisión. La comprobación de captura posterior a esa revisión obtuvo 902 capturas y 0 pérdidas, pero ningún frame enfocado; no se cuenta como otra prueba visual. Los dos hallazgos de revisión quedaron resueltos por inspección.

## Trabajo pendiente

Acceso al renderer D3D12 del juego para capturar sin la ruta CPU y obtener dos vistas/proyecciones independientes, validando escala y profundidad. Después: ampliar giro, recentrado durante el uso, seguimiento de posición, combate/transiciones, HUD, comodidad y mod persistente. La prueba actual no acredita estas capacidades.
