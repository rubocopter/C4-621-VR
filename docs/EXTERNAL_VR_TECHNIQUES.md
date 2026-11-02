# Referencias externas para desbloquear AC6 VR — 21-10-2026

**Referencia posterior, 02-11-2026:** [Elden Ring VR, segundo pase nativo,
propagación de cámara y riesgos de reentrancia](ELDEN_RING_VR_REFERENCE.md).
Se inspeccionó código de otro juego de FromSoftware: repetir la cola de tareas
solo con grupos seleccionados obliga a comprobar cámaras, acciones, GPU,
`Present` y transiciones. No demuestra que el renderer de AC6 sea reentrante.

**Comparación posterior, 01-11-2026:** [dos rutas DS3 VR](DS3_NATIVE_STEREO_REFERENCE.md).
Junglejuz añade una vista en la lista nativa del motor, sin geo-11; scubasteve
delega el estéreo a geo-11. La primera concreta la búsqueda de lista/fábrica
de vistas en AC6 como investigación independiente de cerrar todo el CBV.
Ambas son D3D11 y ninguna demuestra directamente esa ruta en nuestro AC6.

**Referencia posterior, 30-10-2026:** [análisis de DS3-VR sugerido por el
usuario](DS3_VR_REFERENCE.md), fijado a un commit del 29-10. Aporta exportación
de cámara, layout de constantes, presentación OpenXR y separación HUD/lock-on.
Su estéreo depende de geo-11/D3D11; no demuestra una ruta D3D12 ni render
nativo reentrante en AC6. El informe siguiente conserva la revisión del 21-10.

Investigación **de fuentes** sobre proyectos VR que resolvieron problemas
comparables. Se examinaron archivos de implementación, no solo READMEs.
Durante la investigación inicial no se ejecutó Armored Core VI ni SteamVR,
ni se cargaron sondas o modificó la ruta de producción. Después, con el
operador presente y misión offline confirmada, se ejecutó la única prueba
v28 pendiente; su resultado se incorpora debajo. Los repositorios de consulta están en `reference/`
(ignorado por Git). Los hallazgos son **patrones de ingeniería**, no pruebas
de que las direcciones, hooks o estructuras de otros motores sirvan en AC6.
La base de conocimiento de Universal Modder no devolvió un caso relevante
para `Armored Core VI` ni para `d3d12 vr`; no se abrió sesión REA sobre
binarios del juego, porque esta fase examinó implementaciones ajenas y
evidencia ya obtenida. REA queda disponible para seguir el callsite concreto.

## Bloqueo y objetivo verificable

**Aplicación posterior en AC6:** [ruta nativa de constantes y dibujo](NATIVE_RENDER_ROUTE.md).
Se identificaron fábrica/slot8 para el tipo 0x25f, un bloque de matriz que
llega al uploader y wrappers D3D12 de binding, sin cargar otra sonda.
La cobertura del contexto v28 pasa a ser secundaria; el siguiente trabajo
es resolver descriptores y versión de bytes usados por el draw.

La ruta aceptada en PSVR2 reconstruye **dos ojos mediante reproyección de un
único par color/profundidad D3D12**, con orientación 3DoF. La prueba acotada
del 18-10-2026 aceptó 2560×1440 por ojo / 90° / baseline experimental 0,5;
no demuestra estéreo geométrico, escala métrica ni proyección por fotograma.
Véase [STATUS.md](STATUS.md).

El cuello de botella es asignar a **un par GPU específico** la cámara,
matriz/proyección, viewport y jitter que consumió realmente el pase que generó
su profundidad. v18 trazó 3 pares y 214 listas sin overflow; v22 asoció 128
muestras del helper con 64 invocaciones de contexto **CPU** y 3 pares con
procedencia de captura, pero no identificó binding GPU. v26 observó cero
invocaciones de ese contexto durante su ventana. La DLL v28 confirmó después
`window_started:true` y `no_coverage` en PID 21860, con tres pares válidos y
nueve hooks restaurados; no observó matriz/binding GPU.

La prueba v28 de 8 s ya se completó. La continuación identificó una ruta de
upload nativo y un destino virtual por tipo sin repetirla. El siguiente paso
es seguir el consumidor de descriptores de esa [ruta concreta](NATIVE_RENDER_ROUTE.md).

## Qué enseñan los proyectos revisados

| Referencia inspeccionada | Evidencia concreta en el código | Utilidad en AC6 | Límite |
| --- | --- | --- | --- |
| **Cyberpunk VR port**, [`ConstantWriteTimeline.hpp`](https://github.com/dariulone/cyberpunk-vr-port/blob/9aa2601073dd27886cdb873be1e26112e2a6889b/include/Render/ConstantWriteTimeline.hpp) y [`ConstantUploadRegistry.hpp`](https://github.com/dariulone/cyberpunk-vr-port/blob/9aa2601073dd27886cdb873be1e26112e2a6889b/include/Render/ConstantUploadRegistry.hpp) | Orden de grabación y comandos, escrituras con rangos, envíos a colas, fences, aliasing y estados de origen ambiguo; identidad `ID3D12Resource` más GPU virtual address observada directamente. | **Prioridad alta** para diseñar un rastreador *observacional* entre constantes realmente enlazadas, draw y par capturado. | No descubre por sí mismo la constante correcta. Un puntero CPU, bytes parecidos o una GPU VA sin lifetime no prueban asociación. Los estados `Pending`/`CrossQueue`/`UnknownWrite` han de ser rechazo, no selección del candidato próximo. |
| **Dying Light 2 VR**, [`dx12/src/probe.cpp`](https://github.com/farmerarmor/DyingLight2VR/blob/4e746bdcc2f7d199159ca4fc10b175f8eb95d8e9/dx12/src/probe.cpp) | Intercepta entrada de escena, guarda cámara/proyección/frustum, renderiza cada ojo dentro del mismo contador de simulación, copia salidas, verifica identidad y restaura estado; suspende ante discrepancias. | **Prioridad estratégica alta**: investigar un punto nativo de render de escena repetible en AC6 para abandonar la reproyección. | Exige descubrir esa entrada y probar que una segunda evaluación no avanza simulación, no reutiliza targets peligrosos y produce realmente otro render. Los RVA de DL2 no se trasladan. |
| **UEVR**, [`FFakeStereoRenderingHook.cpp`](https://github.com/praydog/UEVR/blob/4ee5c6b6162dee2291fc75f9dfc57667f6d45a2d/src/mods/vr/FFakeStereoRenderingHook.cpp) | Ejemplo de sincronización de tick del motor, viewport y una vista adicional sin duplicar actualización de mundo. | Apoya la separación conceptual **actualización de simulación / render por ojo**. | Es una ruta de Unreal Engine, no un hook reutilizable para el motor de FromSoftware. Código con restricciones propias de licencia; solo referencia conceptual. |
| **Halo MCC VR**, [`src/dll/game.cpp`](https://github.com/dafurman/halo-mcc-vr/blob/202f026d6b5789972bc0916dda5388b4a8bf38ea/src/dll/game.cpp) | Identifica ghosting porque ambos ojos heredaban una textura de historial temporal del último ojo renderizado; conserva y sustituye historial **por ojo** con `CopyResource`. | **Prioridad alta después de lograr dos renders**: trazar TAA/SSR/blur y recursos «previous frame» para impedir contaminación entre ojos. | Ejemplo D3D11/engine-specific; copiar la textura incorrecta podría introducir halos o carreras. Hay que demostrar identidad y lifetime de cada historial de AC6. |
| **Resident Evil 5 VR**, [`constant_probe.cpp`](https://github.com/SpreadTheBirb/ResidentEvil5VR/blob/0c621b52a8b77e4ef6a272e837b65c8dae6a9192/RE5VR/src/hooks/constant_probe.cpp) | Cuenta registros de constantes (`SetVertexShaderConstantF`) durante una ventana de frames y los relaciona con draws; registra también direcciones origen. El código advierte que una constante puede cambiar según el pase (sombras/UI/reflejos). | **Prioridad media** como metodología: múltiples frames, asociación al **draw consumidor** y filtrado de pasadas, evitando inferir cámara por «matriz parecida». | D3D9 usa constantes de shader explícitas; AC6 usa D3D12 root signatures/CBV/descriptor heaps y puede tener datos fuera de CBV. No portar hooks D3D9. |

La documentación de Cyberpunk sobre [factibilidad de replay D3D12](https://github.com/dariulone/cyberpunk-vr-port/blob/9aa2601073dd27886cdb873be1e26112e2a6889b/docs/cp2077-d3d12-replay-feasibility.md)
observó **en ese juego** 72–80 pases dependientes de vista, 35–40 render
targets y cuatro colas. Es una advertencia de coste y sincronización de
`ExecuteCommandLists` replay, **no una medida de AC6** ni un argumento para
modificar sus listas sin evidencia.

## Dos líneas de trabajo, en orden de coste y evidencia

### A. Identidad del dato de cámara que llega al GPU (primera opción)

1. **Cerrar v28 antes de ampliar hooks (completado: `no_coverage`).** Con `window_started:true` y
   candidatos completos, analizar estáticamente el destino de la vtable hija
   `slot8`. Con `no_coverage`, investigar si se ejecuta el punto de entrada
   del contexto durante el pase capturado; con `window_not_started`, corregir
   primero el inicio de ventana. En todos los casos conservar informe, JSONL,
   una sola cola y nueve hooks restaurados. Cero eventos no autoriza a
   seleccionar matrices vecinas en el tiempo.
2. **Seguir la cadena CPU conocida** mediante análisis estático/REA: paquete
   creado en `0x26e17f0..0x26e1da4`, entrega a `context.vtable[1]`
   (`0x2b9d140`) y posible forwarding a `child.vtable[8]`. Determinar si la
   siguiente función empaqueta o escribe constantes, enumera pases o prepara
   listas. Registrar el siguiente límite observable real antes de añadir
   instrumentación; las direcciones solo aplican al PE fingerprint conocido.
3. **Diseñar una traza D3D12 mínima, opt-in y de solo observación** cuando
   exista candidato concreto: identidad de recurso y GPU VA, `Map`/writes o
   `CopyBufferRegion` pertinente, `CreateConstantBufferView` y copias de
   descriptores, `SetGraphicsRootConstantBufferView` / descriptor tables,
   root signature, viewport y draw del pase que escribe el depth elegido.
   Algunos motores usan root constants, SRV, `ExecuteIndirect` o uploads
   persistentes; **comprobar uso real** antes de especializar la traza.
   Mantener estados de root/descriptor por command list y generación de
   grabación; integrar la procedencia de submission/fences ya existente.
   Instrumentar una ventana con capacidad acotada, sin I/O ni esperas GPU
   en callbacks y sin promover a `frame_associated:true` por mera cercanía.
4. **Criterio de salida:** una cadena comprobable
   `par/depth → raster pass/draw → root binding → recurso y rango de
   constantes → versión de contenido → matriz/viewport/jitter`, con orden
   temporal inequívoco frente al envío GPU y cambios de contenido. Si falta
   un eslabón, devolver `unresolved` (incluidos aliasing, regrabación,
   múltiples colas o escrituras simultáneas), sin cambiar protocolo v1 ni
   generar matrices sintéticas. Solo entonces diseñar metadata versionada
   por secuencia y slot, con validación de consumidor que rechace ausencias.

### B. Segundo render nativo por ojo (investigación independiente)

1. Investigar **en estático** un callsite de escena/render-view de AC6 y
   distinguir preparación, culling, ejecución del render graph, presentación
   y avance de simulación. Priorizar un caller que tenga vista, targets,
   estado de cámara y contador de frame observables. No inferir que «doblar
   una función» equivale a dos imágenes geométricas.
2. Primera prueba futura, tras identificación y autorización presencial:
   **solo contar** invocaciones de esa entrada, su hilo, escena/frame,
   recursos destino y cambios de estado. Ninguna llamada duplicada hasta
   demostrar reentrancia, sincronización y ausencia de efectos secundarios.
3. Si los contadores y targets lo sostienen, diseñar render de dos vistas
   preservando **un único tick de simulación**. Aislar camera constants,
   output targets, barreras, recursos transitorios, shadow/SSR/TAA y
   restauración aun en fallo. Contar dos renders y verificar que pertenecen
   a la misma simulación. El ejemplo de Halo muestra por qué los historiales
   temporales deben comprobarse separadamente por ojo.
4. **Criterio de salida:** dos renders geométricamente independientes,
   matriz y frustum efectivos por ojo, dos capturas coherentes, sin
   contaminación temporal, sin avance duplicado de simulación y sin
   degradación insegura de colas/fences. La prueba actual de reproyección
   permanece como referencia visual y funcional.

La segunda línea **no depende** de resolver primero el CBV exacto si se
encuentra una API nativa de render-view que acepte cámara por ojo. Pero ambas
requieren atribución a la vista realmente usada en la GPU para certificar el
resultado, y por ahora no hay evidencia de una entrada reentrante de AC6.

## Decisiones y límites

- **Ahora:** seguir el uploader y consumidor de descriptores ya localizados,
  sin repetir v28. Su cobertura queda como cuestión secundaria.
- **Prueba v28 completada:** ventana abierta sin invocaciones; la apertura
  queda probada en esa pasada, y la causa de falta de cobertura sigue abierta.
- **Después:** instrumentar el vínculo GPU concreto o seguir análisis
  estático de la entrada nativa de escena. No saltar a replay genérico de
  comandos D3D12 ni doble invocación sin pruebas de seguridad.
- **Fallback útil:** mejoras aisladas en intrínsecos por ojo y corrección de
  oclusiones basadas en [contrato experimental](EYE_PROJECTION_CONTRACT.md)
  y pruebas host; requieren validación en visor y no resuelven la ausencia de
  superficies ocultas ni constituyen estéreo geométrico.
- **Conservar gates:** defaults 1280×720/25°, baseline sin calibración,
  protocolo v1 de 120 bytes, parada ante segunda cola y rechazo de DLL ya
  mapeada. Solo build y misión offline verificados al reanudar.

Referencias reproducibles: DL2VR `4e746bd`, UEVR `4ee5c6b`, Halo MCC VR
`202f026`, RE5VR `0c621b5`, Cyberpunk VR port `9aa2601`. De este último se
verificó que los archivos locales `ConstantWriteTimeline.hpp`,
`ConstantUploadRegistry.hpp` y `cp2077-d3d12-replay-feasibility.md` coinciden
exactamente con las versiones remotas de ese commit. DL2VR y Halo MCC VR
declaran licencia MIT; otros componentes deben evaluarse por archivo y
licencia antes de reutilizar código. Este informe adopta **ideas**, no copia
código tercero. Revisar [derechos y procedencia](../COPYRIGHT_AND_REUSE.md).
