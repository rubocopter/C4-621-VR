# AC6 en PSVR2: imagen y orientación experimental

> **Informe histórico de la presentación monoscópica del 15-10-2026.** El estéreo continuo con giro de cabeza se aceptó posteriormente en PSVR2. Consultar [estado verificado](docs/STATUS.md) y [procedimiento vigente](docs/VR_TRIAL_RUNBOOK.md); las cifras y limitaciones de esta página describen aquellos ensayos.

15-10-2026, SteamVR/OpenXR y adaptador oficial PC. Se mostró el juego en el visor y el usuario confirmó seguimiento horizontal/vertical correcto tras calibrar los ejes. Es un diagnóstico temporal monoscópico, no una adaptación VR terminada.

**Actualización posterior:** existe una ruta opcional de captura por GPU, una muestra real de profundidad y una corrección para seguir la cabeza en sesiones VISIBLE sin exigir foco de inputs. Estado y pruebas actuales en `RENDER_CAPTURE_FINDINGS.md`. Las cifras y la política FOCUSED descritas abajo corresponden a los primeros ensayos.

**16-10-2026:** una captura fija reproyectada por profundidad ya se presentó con imágenes distintas por ojo en PSVR2. El usuario confirmó volumen sin imagen doble (1607 frames visibles, sesión cerrada). En ese momento el juego vivo seguía monoscópico. Implementación y límites históricos en [PROJECTION_FINDINGS.md](PROJECTION_FINDINGS.md#prueba-de-escena-fija-en-psvr2).

**Avance posterior del 16-10-2026:** una secuencia continua color/depth del juego produjo dos vistas cambiantes mediante GPU en un ensayo **solo de monitor**. En ese momento aún faltaba la verificación visual del estéreo vivo en PSVR2; se superó después, como registra [LIVE_STEREO_FINDINGS.md](LIVE_STEREO_FINDINGS.md).

## Qué funciona y cómo se comprobó

| Ensayo | Evidencia | Resultado |
| --- | --- | --- |
| Patrón D3D12 por ojo | `evidence/xr-display-worn-01.jsonl` | 1791 frames enviados, 1699 enfocados; usuario confirmó L/R correctas |
| Ventana del juego en pantalla virtual | `evidence/xr-game-window-02.jsonl` | 1799 frames enviados, 563 capturas; usuario confirmó imagen viva sin problemas apreciables |
| Imagen y orientación, primera integración | `evidence/xr-combined-headtracking-01.jsonl` y `combined-headtracking-native-result.json` | 2391 frames enviados, 737 capturas, 1761 aplicaciones de cámara; usuario detectó inversión horizontal/vertical |
| Imagen y orientación calibrada | `evidence/xr-combined-calibrated-01.jsonl` y `calibrated-headtracking-native-result.json` | 2425 frames enviados, 2424 enfocados, 747 capturas, 1794 aplicaciones de cámara; usuario confirmó ambos ejes correctos |

Las dos sondas conjuntas registraron 0 excepciones, se desactivaron solas y restauraron el prólogo original de la copia de cámara. El juego seguía respondiendo. La comprobación inicial `xr-game-window-01` obtuvo captura pero no visibilidad en SteamVR; no cuenta como prueba visual superada.

La confirmación del usuario en aquel ensayo fue: «si estan correctos, lo unico que tengo como muy limitado hasta donde puedo mirar». El límite de **25° de rotación total respecto al centro** es deliberado y se aplica al cuaternión, no por separado a cada eje. No es una limitación medida del visor.

## Implementación y límites de aquella ruta

- Aplicación independiente `native/xr_display_probe/`: crea dispositivo/cola D3D12 en el adaptador LUID exigido por OpenXR; usa dos swapchains sRGB de 1280×720. Fences protegen reutilización de buffers y las texturas vuelven a `RENDER_TARGET` antes de liberarse a OpenXR.
- Windows Graphics Capture selecciona exclusivamente la ventana de `armoredcore6.exe` del PID indicado. Captura de ventana sin enviar input. Hace lectura GPU→CPU y subida CPU→GPU; se limita a ~30 capturas/s. Los ~90 envíos/s a SteamVR **no equivalen a 90 imágenes nuevas del juego/s**. Latencia aún sin medir.
- Patrón y primera imagen: capas quad en espacio LOCAL. Prueba conjunta: quad fijado al espacio VIEW, ancho 4.5 m a 1.3 m (~120° horizontales) y proporción 16:9. Usa la misma imagen del juego en ambos ojos; carece de paralaje del mundo y de profundidad estereoscópica de AC6.
- La sesión gráfica también publica orientación relativa por el transporte local del PID. Conversión calibrada `S R S`, `S=diag(1,1,-1)`, después de recentrar/limitar; invierte yaw/pitch frente a la primera prueba y conserva roll. Roll no se validó por separado en el visor.
- `ac6_headtracking_calibrated_probe.dll` consume la rotación en el hook de cámara previamente validado. Ventana de seguimiento 15 s, 2 s iniciales y 3 s finales. F12 cancela ambas piezas. Pose caduca tras 250 ms; no se aplica traslación.
- La aplicación espera hasta 30 s para colocarse el visor y cuenta la duración desde estado VISIBLE. Un ensayo sin frames FOCUSED devuelve resultado no satisfactorio. Al perder captura durante más de 500 ms oculta la imagen e invalida la orientación, registra la pérdida y devuelve resultado no satisfactorio.
- Ante un fallo de sincronización GPU con dispositivo aún activo, se termina solo el proceso de presentación sin liberar objetos COM en uso durante el desenrollado; el sistema operativo retira su contexto. Esa ruta excepcional no se provocó en el visor en aquel ensayo. Ninguna de estas operaciones termina el juego.
- No se modificaron archivos del juego. Las cuatro DLL diagnósticas del PID 32616 quedaron mapeadas e inactivas hasta cerrar aquella sesión; no se forzó su descarga.

## Repetición actual

Usar la [prueba de estéreo continuo](docs/VR_TRIAL_RUNBOOK.md). Se retiran los
comandos antiguos de build fija y `run_vr_trial.py` de esta página para evitar
arrancar la ruta monoscópica como si fuera la actual. La implementación y
secuencia originales siguen en Git y en la herramienta histórica.

Verificación final: 8 tests Python y 3 tests nativos pasan; sintaxis de `tools/*.py`, ayuda del script y bloqueo de recarga comprobados. `evidence/vr-calibrated-final-audit.json` confirma nuevamente el prólogo original, los módulos diagnósticos y el hash del ejecutable de presentación posterior a la revisión. La comprobación de captura posterior a esa revisión obtuvo 902 capturas y 0 pérdidas, pero ningún frame enfocado; no se cuenta como otra prueba visual. Los dos hallazgos de revisión quedaron resueltos por inspección.

## Puertas superadas después y pendientes reales

El transporte GPU sin readback normal, las dos imágenes reproyectadas en vivo
y la selección explícita de mayor resolución/rango se implementaron y se
probaron después. La aceptación física PR #8 usó 2560×1440/90° con giro de
cabeza. Los [resultados de calidad](QUALITY_PACING_FINDINGS.md) conservan las
comparaciones y sus límites.

Siguen pendientes matriz/jitter ligados a la captura, escala/IPD, poses/FOV de
cada ojo, oclusiones/HUD, posición 6DoF, combate/transiciones, latencia y
comodidad prolongada. El [estado vigente](docs/STATUS.md) distingue esas
puertas de los hitos ya aceptados.
