# Armored Core VI — investigación de mod VR (15-10-2026)

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
- **Última sesión de laboratorio:** launcher cooperativo, misión offline confirmada por el usuario, PID `17628`; identidad y módulos comprobados por el cargador. Los PID `32616` y `4904` corresponden a sesiones anteriores ya cerradas. La presencia de un launcher no verifica por sí sola el estado de red.

## Mods existentes

- `mods/FIrst Person Camera/regulation.bin`: **Enhanced First Person Camera**, mod 305 de Liquidation, preset de **120° horizontal / 89° vertical** según el autor, paquete de archivos 1.0.1 subido el 05-08-2026 y descrito como compatible con regulación 1.09. El fichero local (1 124 432 bytes) coincide byte a byte con el `Game/regulation.bin` activo: SHA-256 `71b06ff97e9d8792e50e874e990ee4878cc1d7234f6a75d4a170fdea11a72b67`. **El usuario confirma que ha jugado en primera persona sin problemas aparentes mediante el launcher cooperativo.** El ángulo exacto no se ha medido.
- Copia previa del usuario `Game/regulation.bin.bck`: SHA-256 `1ee3d3451bd524f571f606b5e3f869c230f7a14b17392e97d507ad9403013fa7`, 861 104 bytes. El cambio de tamaño implica diferencias sustanciales; no se han decodificado los parámetros.
- `mods/Armored Core VI co-op/`: launcher `ac6_for_coop_launcher.exe`, `AC6Coop/ac6_coop.dll`, ajustes y traducciones. Esos ejecutables también aparecen en el directorio del juego.
- `AC6Coop/ac6_coop_settings.ini`: a fecha de inspección, `allow_evil_guest = 1` habilita las invasiones configuradas por el mod. Para ensayos individuales aislados, revisar el ajuste antes de lanzar y no participar en sesiones con terceros.

## Investigación de mods existentes

- El usuario ha instalado y probado *Enhanced First Person Camera* (mod 305), preset 120°. La prueba confirma jugabilidad en primera persona, sin cubrir necesariamente todos los cambios de cámara documentados por el autor (pulse/assault armor, melee y cambios bruscos de momento).
- **Depth of Field ya está desactivado:** comprobado en `GraphicsConfig.xml` (`DepthOfField=DISABLE`). Configuración leída: fullscreen 2560×1440 y motion blur HIGH. La lectura no mide rendimiento ni verifica la resolución realmente presentada. El usuario no ha utilizado el resto de la guía (VorpX, ReShade, Mod Engine 2, Smithbox).
- VorpX es la vía VR recomendada por el autor de este mod; no se da por instalado ni validado. Nuestro objetivo continúa siendo investigar integración **OpenXR + D3D12** directamente, y no depender de VorpX para la prueba técnica inicial.
- Mod Engine 2 / me3 y Smithbox son opciones para combinar datos y evitar sobrescribir `regulation.bin`; evaluar coexistencia con el launcher cooperativo antes de instalar nada.

Referencias de terceros:

- https://www.nexusmods.com/armoredcore6firesofrubicon/mods/3
- https://www.nexusmods.com/armoredcore6firesofrubicon/mods/30
- https://www.nexusmods.com/armoredcore6firesofrubicon/mods/305?tab=files
- https://en.bandainamcoent.eu/armored-core/news/armored-core-vi-fires-of-rubicon-patch-notes-109

## Arquitectura propuesta y puertas de validación

1. **Puerta A: prueba funcional básica superada por el usuario.** Arranque por launcher cooperativo y juego en primera persona sin problemas aparentes; capturas posteriores de la misión obtenidas con Universal Modder. Quedan mediciones de FPS, versión visible y transiciones especiales.
2. **Puerta B: orientación demostrada también dentro del visor.** Hook nativo en copia de cámara y orientación de la misma sesión gráfica OpenXR. En la prueba conjunta se detectó inversión horizontal/vertical; convertir coordenadas mediante reflexión de Z la corrigió, confirmado por el usuario en ambos ejes. Último ensayo: 15 s, límite total de 25°, 1794 aplicaciones, 0 excepciones registradas, desactivación y prólogo restaurado. No cubre estabilidad prolongada, roll por separado ni transiciones. Véase `VR_DISPLAY_FINDINGS.md`.
3. **Puerta C: presentación temporal conseguida.** Aplicación independiente con OpenXR/D3D12 y swapchains de 1280×720 por ojo. La ruta opcional `--gpu-capture` ya transporta la ventana WGC mediante texturas/fences compartidas, sin leer sus píxeles en CPU; diagnóstico medido ~60 capturas/s frente al límite de 30 de la ruta anterior. El usuario confirmó imagen y orientación en el visor. Latencia de movimiento a imagen aún sin medir; sigue siendo una capa quad monoscópica, no una integración directa del renderer ni un mod persistente.
4. **Puerta D: estereoscopía real.** Generar y mostrar views/projections independientes de ojo izquierdo y derecho, con pose de cabeza e IPD procedentes de OpenXR. Determinar si hace falta repetir el render pass, reutilizar draw calls o implementar una vía de reproyección limitada. Una sola captura monoscópica duplicada en los dos ojos no supera esta puerta.

   Se capturó una muestra real de profundidad D3D12 de 2560×1440, finita y visualizable, tras corregir el manejo de planos depth/stencil y resolución variable. Falta sincronizarla con color y verificar proyección, escala y alineación antes de generar estereoscopía; detalles en `RENDER_CAPTURE_FINDINGS.md`.
5. **Puerta E: juego básico.** Seguimiento 6DoF (orientación y posición) estable e independiente del movimiento del AC, sin desalinear armas/objetivos, latencia aceptable y sin perder la cámara en boosts, lock-on o transiciones. Adaptar HUD para lectura cómoda y conservar input de gamepad. El prototipo actual solo aplica orientación (3DoF).
6. **Puerta F: experiencia VR.** Tratamiento de efectos de postprocesado, oclusión y geometría cercana, comodidad (vibraciones, horizonte, aceleraciones, snap/recenter), y mapeo de mandos PSVR2 Sense si aporta valor.

## Riesgos técnicos

- El juego no ofrece API oficial de VR ni integración visible con OpenXR. El mayor riesgo es obtener dos vistas renderizadas por frame a través de su renderer D3D12 propietario.
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
- **Prototipo monoscópico de imagen + orientación conseguido; VR completa pendiente.** Hay imagen del juego en PSVR2 y seguimiento correcto, temporal y limitado a 25°. No hay renderizado binocular de la geometría ni traslación de cabeza. Siguiente etapa técnica: acceso al renderer D3D12 para reducir latencia y obtener profundidad/dos vistas (D), con validación de proyecciones y escala.
- Las sondas se desactivaron automáticamente; sus DLL quedan inertes en el proceso hasta cerrar el juego. No son todavía un mod persistente instalable. Para repetir la prueba conjunta, consultar `VR_DISPLAY_FINDINGS.md` y `tools/run_vr_trial.py`. `HEADTRACKING_RUNBOOK.md` documenta la prueba anterior en monitor.
