# Armored Core VI — investigación de mod VR (actualizado 16-10-2026)

> **Estado actualizado (18-10-2026):** el prototipo PSVR2 ya ha demostrado **estéreo continuo reproyectado desde profundidad combinado con orientación de cabeza calibrada**. En una prueba acotada de 2560×1440 por ojo y 90° se observó mayor nitidez y ningún tirón apreciable. Esto no acredita una conversión VR completa ni comodidad prolongada. El estado vigente está en [README.md](README.md), [LIVE_STEREO_FINDINGS.md](LIVE_STEREO_FINDINGS.md) y [QUALITY_PACING_FINDINGS.md](QUALITY_PACING_FINDINGS.md). Los procedimientos y gates anteriores que siguen a continuación se conservan como historial.

## Objetivo

Construir una adaptación VR de *Armored Core VI: Fires of Rubicon* para PSVR2 en PC mediante SteamVR/OpenXR. Primera meta: cámara en primera persona con orientación de cabeza, imagen en el visor y presentación estereoscópica correcta. Conservar el mando tradicional al principio; controles VR y HUD después de validar el renderizado.

## Instalación inspeccionada

- Juego Steam: `C:\Program Files (x86)\Steam\steamapps\common\ARMORED CORE VI FIRES OF RUBICON\Game`.
- Ejecutable: `armoredcore6.exe`, Windows x64 nativo, versión del recurso PE `1.9.0.0` (no prueba por sí sola la versión exacta de regulación ni compatibilidad de mods).
- Motor: tecnología nativa de FromSoftware; identificación de Universal Modder: `fromsoft`, confianza 95 %.
- Gráficos: el ejecutable importa **`d3d12.dll` y `dxgi.dll`**, según la tabla PE inspeccionada localmente.
- OpenXR activo en Windows: `C:\Program Files (x86)\Steam\steamapps\common\SteamVR\steamxr_win64.json`.
- EAC presente en la instalación estándar. Hay un launcher cooperativo que, según la documentación de su autor, inicia sesiones modificadas sin EAC y sin emparejamiento oficial de FromSoftware. Por tanto, el laboratorio se limitará al launcher cooperativo y juego individual; nunca a las partidas oficiales con EAC.
- El modo cooperativo usa partidas separadas. La compatibilidad de sus hooks con un futuro módulo VR debe probarse; el launcher por sí solo no implementa un cargador OpenXR.
- **Última sesión de laboratorio:** launcher cooperativo, misión offline confirmada por el usuario, PID `5440`; identidad y módulos comprobados por el cargador. Los PID `17628`, `32616` y `4904` corresponden a sesiones anteriores ya cerradas. La presencia de un launcher no verifica por sí sola el estado de red.

## Mods existentes

- `mods/FIrst Person Camera/regulation.bin`: **Enhanced First Person Camera**, mod 305 de Liquidation, preset de **120° horizontal / 89° vertical** según el autor, paquete de archivos 1.0.1 subido el 05-08-2026 y descrito como compatible con regulación 1.09. El fichero local (1 124 432 bytes) coincide byte a byte con el `Game/regulation.bin` activo: SHA-256 `71b06ff97e9d8792e50e874e990ee4878cc1d7234f6a75d4a170fdea11a72b67`. **El usuario confirma que ha jugado en primera persona sin problemas aparentes mediante el launcher cooperativo.** El ángulo exacto no se ha medido.
- Copia previa del usuario `Game/regulation.bin.bck`: SHA-256 `1ee3d3451bd524f571f606b5e3f869c230f7a14b17392e97d507ad9403013fa7`, 861 104 bytes. El cambio de tamaño implica diferencias sustanciales; no se han decodificado los parámetros.
- `mods/Armored Core VI co-op/`: launcher `ac6_for_coop_launcher.exe`, `AC6Coop/ac6_coop.dll`, ajustes y traducciones. Esos ejecutables también aparecen en el directorio del juego.
- `AC6Coop/ac6_coop_settings.ini`: a fecha de inspección, `allow_evil_guest = 1` habilita las invasiones configuradas por el mod. Para ensayos individuales aislados, revisar el ajuste antes de lanzar y no participar en sesiones con terceros.

## Investigación de mods existentes

- El usuario ha instalado y probado *Enhanced First Person Camera* (mod 305), preset 120°. La prueba confirma jugabilidad en primera persona, sin cubrir necesariamente todos los cambios de cámara documentados por el autor (pulse/assault armor, melee y cambios bruscos de momento).
- **Depth of Field ya está desactivado:** comprobado en `GraphicsConfig.xml` (`DepthOfField=DISABLE`). Configuración leída: fullscreen 2560×1440 y motion blur HIGH. La lectura no mide rendimiento ni verifica la resolución realmente presentada. **Mod Engine 2 sí se probó** en una misión el 15-10-2026 (véase `MODLOG.md`), con `modengine2.dll` presente. VorpX, ReShade y Smithbox no se han validado en estas pruebas.
- VorpX es la vía VR recomendada por el autor de este mod; no se da por instalado ni validado. Nuestro objetivo continúa siendo investigar integración **OpenXR + D3D12** directamente, y no depender de VorpX para la prueba técnica inicial.
- Mod Engine 2 ya se utilizó en una sesión separada (sin el DLL cooperativo detectado); la coexistencia simultánea con el launcher cooperativo no se ha validado. Smithbox sigue siendo una opción para editar parámetros, sin prueba de integración.

Referencias de terceros:

- https://www.nexusmods.com/armoredcore6firesofrubicon/mods/3
- https://www.nexusmods.com/armoredcore6firesofrubicon/mods/30
- https://www.nexusmods.com/armoredcore6firesofrubicon/mods/305?tab=files
- https://en.bandainamcoent.eu/armored-core/news/armored-core-vi-fires-of-rubicon-patch-notes-109

## Arquitectura propuesta y puertas de validación

1. **Puerta A: prueba funcional básica superada por el usuario.** Arranque por launcher cooperativo y juego en primera persona sin problemas aparentes; capturas posteriores de la misión obtenidas con Universal Modder. Quedan mediciones de FPS, versión visible y transiciones especiales.
2. **Puerta B: orientación demostrada también dentro del visor.** Hook nativo en copia de cámara y orientación de la misma sesión gráfica OpenXR. En la prueba conjunta se detectó inversión horizontal/vertical; convertir coordenadas mediante reflexión de Z la corrigió, confirmado por el usuario en ambos ejes. Último ensayo: 15 s, límite total de 25°, 1794 aplicaciones, 0 excepciones registradas, desactivación y prólogo restaurado. No cubre estabilidad prolongada, roll por separado ni transiciones. Véase `VR_DISPLAY_FINDINGS.md`.
3. **Puerta C: presentación temporal conseguida.** Aplicación independiente con OpenXR/D3D12 y swapchains de 1280×720 por ojo. La ruta opcional `--gpu-capture` ya transporta la ventana WGC mediante texturas/fences compartidas, sin leer sus píxeles en CPU; diagnóstico medido ~60 capturas/s frente al límite de 30 de la ruta anterior. El usuario confirmó imagen y orientación en el visor. Latencia de movimiento a imagen aún sin medir; sigue siendo una capa quad monoscópica, no una integración directa del renderer ni un mod persistente.
4. **Puerta D: estereoscopía con reproyección (superada como prueba acotada, no como render geométrico independiente).** Se capturaron color/depth coherentes en la cola D3D12, se reconstruyeron vistas por ojo con GPU y se presentaron continuamente en PSVR2. En V7 se confirmó visualmente actualización de escena, profundidad y ausencia de imagen doble con mando convencional; V8 combinó la ruta con orientación del visor y V11 confirmó una mejora percibida con el perfil 2560×1440/90°. El shader usa coeficientes de proyección observados, pero **aún falta** asociar matriz y jitter al mismo frame, calibrar baseline/IPD/escala, proyecciones OpenXR asimétricas, oclusiones y HUD. La salida es estéreo reproyectado desde una sola captura, **no dos pases geométricos independientes**. Ver `LIVE_STEREO_FINDINGS.md` y `QUALITY_PACING_FINDINGS.md`.
5. **Puerta E: juego básico.** Seguimiento 6DoF (orientación y posición) estable e independiente del movimiento del AC, sin desalinear armas/objetivos, latencia aceptable y sin perder la cámara en boosts, lock-on o transiciones. Adaptar HUD para lectura cómoda y conservar input de gamepad. El prototipo actual solo aplica orientación (3DoF).
6. **Puerta F: experiencia VR.** Tratamiento de efectos de postprocesado, oclusión y geometría cercana, comodidad (vibraciones, horizonte, aceleraciones, snap/recenter), y mapeo de mandos PSVR2 Sense si aporta valor.

## Riesgos técnicos

Actualización de la medición del preset: la matriz observada con el mod activo deriva FOV Y ~89.0000° y FOV X ~120.4260°. La fórmula reverse-Z ya permite reconstruir unidades del juego; no está establecida su equivalencia a metros. La referencia estéreo offline y el diagnóstico GPU se detallan en `PROJECTION_FINDINGS.md`.

- El juego no ofrece API oficial de VR. El prototipo ya presenta estéreo continuo reproyectado en PSVR2 usando OpenXR, pero todavía deben verificarse latencia, artefactos de oclusión y si una sola captura color/depth resulta suficiente frente a renderizar dos vistas geométricas independientes.
- `regulation.bin` es un mod de datos: cambia la cámara y el FOV, pero no garantiza pose de cabeza, profundidad estereoscópica, timings de VR o HUD adecuado.
- La cámara puede reasignarse durante acciones y cinemáticas. El objetivo del análisis es encontrar el punto estable usado por el renderer y validar la restauración.
- La build instalada y el archivo de parámetros del mod 305 tienen tamaños distintos al archivo original. La jugabilidad básica está confirmada por el usuario; no equivale a una comparación completa de parámetros.
- La combinación con el cooperativo puede generar conflictos entre hooks o comportamiento de red. Hacer todas las pruebas en sesiones individuales y separadas de matchmaking oficial.
- El rendimiento en VR puede requerir dos renderizados de una escena compleja. Medir costes y problemas visuales antes de asumir viabilidad.

## Estado real

- **El usuario ha probado el preset de 120° mediante el launcher cooperativo y ha jugado sin problemas aparentes.** Resultado registrado como testimonio del usuario; el agente no ha lanzado el juego.
- Fingerprint local, ruta D3D12 y registro OpenXR identificados; véase `tools/fingerprint_ac6.py`.
- REA reconoce el ejecutable; tanto su resumen anterior como la búsqueda actual agotaron el límite durante el análisis automático. La sesión se cerró. Un análisis acotado con **Ghidra directo** produjo pseudocódigo de dos candidatos y corroboró una copia de matriz a una cadena global. Esa cadena se ha recorrido posteriormente en vivo durante una misión; límites y resultados en `CAMERA_FINDINGS.md`.
- La copia espejo permitió telemetría, pero escribirla no produjo control visual. Escribir la fuente externamente produjo artefactos; el hook nativo después de la copia sí dio un giro limpio y después seguimiento de cabeza confirmado por el usuario.
- `tools/probe_openxr_headless.py` y `tools/pose_publication.py` conservan el primer diagnóstico. La aplicación más reciente `native/xr_display_probe/` obtiene imagen y orientación en una sola sesión gráfica y publica la matriz con conversión Z; `native/camera_probe/` la consume sin esperar a OpenXR en el hilo del juego.
- **Estado actualizado 18-10-2026:** estéreo continuo reproyectado en PSVR2 y orientación de cabeza combinados en pruebas acotadas; V11 a 2560×1440 por ojo / 90° observado más claro y sin tirones apreciables. No equivale a render geométrico binocular ni 6DoF. Métrica, matrices por frame, latencia y comodidad prolongada siguen pendientes.
- **Siguiente gate técnico:** asociar a cada frame de color/depth su matriz (incluido jitter), determinar la escala y usar proyecciones/IPD OpenXR sin perder la restauración segura de los hooks. A continuación, validar visualmente combate, boost y transiciones en sesiones más largas.
- Las sondas se desactivan automáticamente y sus DLL quedan inertes en el proceso hasta cerrar el juego. No existe un mod persistente instalable. El procedimiento nuevo y sus límites están en `LIVE_STEREO_FINDINGS.md` y `tools/run_live_stereo_trial.py`; la anterior prueba monoscópica sigue documentada en `VR_DISPLAY_FINDINGS.md` / `tools/run_vr_trial.py`.
