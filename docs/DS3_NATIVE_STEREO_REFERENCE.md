# Dos referencias DS3 VR y su aplicación a AC6 — 01-11-2026

El usuario propuso comparar estos dos proyectos. Se inspeccionó su código
mediante GitHub, sin compilar ni ejecutar mods, abrir AC6/SteamVR o cargar
sondas. Las afirmaciones sobre funcionamiento y rendimiento en DS3 son de
sus autores; esta revisión acredita contenido de fuentes, no validación física.

| Referencia | Commit revisado | Arquitectura encontrada |
| --- | --- | --- |
| [junglejuz/Dark-Souls-III-VR-Native](https://github.com/junglejuz/Dark-Souls-III-VR-Native) | [`441792c4d63ee8c945913d65aa434839263fca39`](https://github.com/junglejuz/Dark-Souls-III-VR-Native/commit/441792c4d63ee8c945913d65aa434839263fca39), 01-11-2026 | Segunda vista de escena en la lista nativa del motor, cámaras por ojo, D3D11, proyección descentrada y separación de salidas. |
| [scubasteve2365/DS3-VR](https://github.com/scubasteve2365/DS3-VR) | [`14909f48da7ebb709af6a38c33fbc27cc91fd4a6`](https://github.com/scubasteve2365/DS3-VR/commit/14909f48da7ebb709af6a38c33fbc27cc91fd4a6), 29-10-2026 | Estéreo delegado a geo-11/fix Helifax, cámara modificada antes de exportación y presentación OpenXR. |

El HEAD de scubasteve sigue siendo el revisado el 30-10: se conserva el
[análisis anterior](DS3_VR_REFERENCE.md). El de junglejuz es una referencia
nueva, publicada el 01-11, con dos commits al consultar su historial.
Las consultas de issues abiertos/cerrados devolvieron cero en ambos casos.

## Aportación nueva: crear una vista de escena

En junglejuz, `addresses.h` identifica `RendManImp`, su lista, `RenderAllViews`,
`AddView`, `RemoveView`, una fábrica de vistas y setters para escena, cámara
y targets. `list_own` en `stereo.cpp` llama al constructor con el descriptor
observado de la vista principal, conserva una referencia, asigna escena y
un enlace de cámara propio, y añade la vista a la lista del motor.
La cámara principal se desplaza al ojo izquierdo y la añadida al derecho.

Fuentes fijadas: [direcciones y layouts DS3](https://github.com/junglejuz/Dark-Souls-III-VR-Native/blob/441792c4d63ee8c945913d65aa434839263fca39/src/game/ds3/addresses.h#L40-L97),
[creación y listado](https://github.com/junglejuz/Dark-Souls-III-VR-Native/blob/441792c4d63ee8c945913d65aa434839263fca39/src/game/ds3/stereo.cpp#L423-L467),
[actualización por ojo](https://github.com/junglejuz/Dark-Souls-III-VR-Native/blob/441792c4d63ee8c945913d65aa434839263fca39/src/game/ds3/stereo.cpp#L255-L291).

Esto hace más concreta la línea de investigación de render nativo por ojo
que ya figuraba en [las técnicas externas](EXTERNAL_VR_TECHNIQUES.md).
No exige resolver primero todo el seguimiento de CBV de nuestra reproyección
para **buscar en estático** una lista/fábrica equivalente. Sí exige demostrar
identidad, ownership, lifetime, simulación y salidas antes de modificarla.
Una función que prepara constantes o emite un draw no equivale a esa fábrica.

La implementación conserva también experimentos antiguos. En particular,
`RendMan::Draw` ejecutado dos veces se documenta como fallo por agotamiento
de un array del frame y se fuerza desactivado al instalar. Hay otro modo
experimental que repite `RenderAllViews`; su mera presencia no lo convierte
en la ruta principal ni demuestra reentrancia en AC6.
Fuente: [desactivación explícita de DRAWTWICE](https://github.com/junglejuz/Dark-Souls-III-VR-Native/blob/441792c4d63ee8c945913d65aa434839263fca39/src/game/ds3/stereo.cpp#L2033-L2044).

Los comentarios del proyecto mencionan lecciones de AC6/Sekiro y notas
privadas como `RE_NOTES`. Son contexto del autor, **no evidencia de que esas
estructuras o resultados estén identificados en nuestro AC6**. Los RVA y
layouts DS3 no se trasladan a nuestro ejecutable fingerprinted.

## Qué estudiar de cada implementación

| Problema de AC6 | Referencia más concreta | Alcance real de la aportación |
| --- | --- | --- |
| Dos renders geométricos | junglejuz: lista, fábrica y cámara por vista | Patrón para localizar una entrada del motor por encima de D3D12; no es un hook compatible ya localizado. |
| Proyección por ojo | junglejuz: `offaxis.cpp`, `lens.h` | Reutiliza el constructor descentrado del motor, relacionado con captura por tiles; interviene en preparación de vista y pasadas derivadas. Es una hipótesis de búsqueda para AC6, no prueba de que exista allí. |
| Salidas color/depth | junglejuz: `picsplit.cpp` | Sus vistas comparten targets finales y depth; copia cada ojo antes de que el otro lo sobrescriba. Crear dos vistas no garantiza dos texturas finales independientes. |
| Constantes y matrices | scubasteve: `dlss.cpp`; junglejuz: `scene_capture_vp` | Documentan `cbSceneParam` y vista-proyección en filas 24–27. Junglejuz corrige coordenadas relativas a cámara y transpone para su matemática. Debe contrastarse layout y convención en shaders AC6. |
| Sombras, reflejos, culling | junglejuz: `picsplit.cpp`, `offaxis.cpp`, `camera.cpp` | Muestra dependencias compartidas y reparaciones por pase. La proyección de render y la de visibilidad deben distinguirse; ampliar el frustum equivocado puede deformar la imagen. |
| Historiales y DLSS | junglejuz: `picsplit.cpp`, `dlss.cpp` | Contiene mecanismos de historiales por vista y un feature DLSS por ojo; algunos son opcionales. No asumir que todo mecanismo presente está activo o validado en todos los perfiles. |
| HUD y lock-on | ambos | Separación del panel y tratamiento específico de marcadores del mundo, con reglas y shaders particulares de DS3. |
| Reproyección como alternativa | junglejuz: `afw.cpp` | Dibuja un ojo y sintetiza el otro con profundidad; alterna el ojo real e incluye relleno de huecos/historia. Es diferente de renderizar ambos ojos y puede servir para comparar nuestra ruta actual. |

Fuentes adicionales fijadas:
[proyección descentrada](https://github.com/junglejuz/Dark-Souls-III-VR-Native/blob/441792c4d63ee8c945913d65aa434839263fca39/src/game/ds3/offaxis.cpp#L284-L356),
[lentes](https://github.com/junglejuz/Dark-Souls-III-VR-Native/blob/441792c4d63ee8c945913d65aa434839263fca39/src/game/ds3/lens.h),
[matriz relativa y selección](https://github.com/junglejuz/Dark-Souls-III-VR-Native/blob/441792c4d63ee8c945913d65aa434839263fca39/src/game/ds3/picsplit.cpp#L479-L578),
[historiales](https://github.com/junglejuz/Dark-Souls-III-VR-Native/blob/441792c4d63ee8c945913d65aa434839263fca39/src/game/ds3/picsplit.cpp#L855-L859),
[DLSS por ojo](https://github.com/junglejuz/Dark-Souls-III-VR-Native/blob/441792c4d63ee8c945913d65aa434839263fca39/src/game/ds3/dlss.h),
[AFW](https://github.com/junglejuz/Dark-Souls-III-VR-Native/blob/441792c4d63ee8c945913d65aa434839263fca39/src/game/ds3/afw.cpp).

## DLSS como hito propio después de establecer la ruta VR

El usuario destacó que ambas implementaciones DLSS son una referencia
independiente del método de estéreo. Se registra como hito de calidad y
rendimiento posterior a disponer de vistas e inputs temporalmente coherentes.
No se limita a sustituir el escalado de la imagen final.

| Pieza a estudiar | Referencia y requisito para AC6 |
| --- | --- |
| Estado temporal por ojo | Junglejuz mantiene un `Eye` por ojo con feature NGX, matriz anterior y reset propios. Impedir que la evaluación izquierda herede historia derecha. |
| Jitter | Comparar el jitter aplicado a constantes en scubasteve con el aplicado a viewports en junglejuz. La fase se coordina entre ojos y el desplazamiento declarado debe coincidir con el realmente renderizado, en signo y unidades. |
| Profundidad y movimiento | Junglejuz reconstruye movimiento de cámara desde depth y matrices, con opciones para datos del juego. El movimiento de cámara por sí solo no describe enemigos, AC, proyectiles o partículas animadas: evaluar ghosting y disponibilidad de velocidades reales. |
| Punto de inserción | Scubasteve escala la imagen estéreo terminada después de los efectos del juego; su README reconoce suavidad adicional. Estudiar en AC6 una inserción que conserve escena, HUD y postprocesado coherentes, sin asumir equivalencia entre puntos del pipeline. |
| Resets y fallos | Referencias para cambios de resolución/modo, historia inválida y diagnóstico individual de entradas/salidas. Junglejuz mide color, movimiento y profundidad por ojo para detectar negro, ruido y valores inválidos. |
| Evaluación | Comparar resolución nativa, DLAA y reescalado con cámara/escena equivalentes; medir tiempo GPU total de ambos ojos, estabilidad temporal, HUD y comodidad. Las cifras DS3 no acreditan una ganancia en AC6. |

Fuentes fijadas: [estado temporal, movimiento y diagnóstico nativos](https://github.com/junglejuz/Dark-Souls-III-VR-Native/blob/441792c4d63ee8c945913d65aa434839263fca39/src/game/ds3/dlss.cpp),
[contrato por ojo](https://github.com/junglejuz/Dark-Souls-III-VR-Native/blob/441792c4d63ee8c945913d65aa434839263fca39/src/game/ds3/dlss.h),
[DLSS de scubasteve](https://github.com/scubasteve2365/DS3-VR/blob/14909f48da7ebb709af6a38c33fbc27cc91fd4a6/src/dlss.cpp)
y [limitación de escalado tras efectos](https://github.com/scubasteve2365/DS3-VR/blob/14909f48da7ebb709af6a38c33fbc27cc91fd4a6/README.md).
La observación sobre movimiento de objetos es una consecuencia de reconstruir
solo el cambio de cámara, no una medición propia de artefactos de esos mods.

La adaptación requiere NGX/D3D12 y sincronización con los recursos AC6.
No basta con ejecutar dos evaluaciones sobre una imagen ampliada ni con
activar DLSS sin depth, jitter y movimiento correspondientes a cada ojo.

## Límites que impiden adoptar el código como solución terminada

Ambos proyectos usan D3D11; nuestro transporte/renderer usa D3D12. Parte de
la investigación a nivel de motor puede orientar aunque cambie la API, pero
targets, command lists, heaps, barreras y fences requieren implementación y
prueba específicas en AC6.

Junglejuz tampoco certifica la cadena que falta en nuestro productor:

- La selección de VP comprueba posición/orientación y forma de lente; no es
  un vínculo versionado de constantes con un draw y submission D3D12.
- La separación de imágenes incluye un modo predeterminado con índices de
  escrituras elegidos por votación de la estructura de frames anteriores.
  No trasladar esa heurística como prueba inequívoca de ownership.
- El enlace de cámara del segundo ojo se marca mediante una diferencia de
  FOV; el código actual usa `+0.001` y tolerancia. Algunos comentarios de
  cabecera conservan descripciones anteriores: prima la implementación.
- El README reconoce ojos negros/ruido con DLSS y congelaciones tras quitar
  y volver a poner el visor. Sus cifras de rendimiento son una comparación
  del autor en un PC/escena con resoluciones distintas; no predicen AC6.

Fuentes: [votación de capturas](https://github.com/junglejuz/Dark-Souls-III-VR-Native/blob/441792c4d63ee8c945913d65aa434839263fca39/src/game/ds3/picsplit.cpp#L4538-L4566),
[marcado de cámara](https://github.com/junglejuz/Dark-Souls-III-VR-Native/blob/441792c4d63ee8c945913d65aa434839263fca39/src/game/ds3/stereo.cpp#L266-L274),
[limitaciones publicadas](https://github.com/junglejuz/Dark-Souls-III-VR-Native/blob/441792c4d63ee8c945913d65aa434839263fca39/README.md).

## Consecuencia para AC6

La recomendación es estudiar dos preguntas concretas: **qué objeto/lista del
motor representa una vista completa de escena** y **qué constantes consume
realmente el pase del depth capturado**. Junglejuz orienta la primera;
ambos proyectos aportan semántica para la segunda. Son líneas compatibles:
encontrar una fábrica de vistas permitiría investigar estéreo geométrico sin
convertir la resolución completa del CBV en requisito de esa búsqueda.

Las condiciones para modificar o aceptar una ruta nueva permanecen: misma
simulación, cámaras efectivas por ojo, ownership y vida de recursos comprobados,
color/depth coherentes y cierre seguro. Esta revisión no introduce hooks,
metadata VR ni cambios al presentador aceptado, y no requiere repetir v28.

## Procedencia

- Búsqueda KB: `um kb search "Dark Souls III native stereo"`, sin resultados.
- GitHub: metadata e historial de ambos repositorios, árbol recursivo nativo,
  issues `state=all`, README y fuentes citadas fijadas a los commits anteriores.
- No hubo screenshots, benchmark propio, análisis de binarios DS3 ni prueba
  de headset. Comentarios sobre ensayos privados siguen atribuidos al autor.
- Se incorporan análisis y enlaces, no código de terceros. Los avisos de
  junglejuz describen componentes MIT/BSD/Apache, condiciones de Luma y NVIDIA;
  no hay un `LICENSE` raíz entre los archivos consultados. La etiqueta MIT de
  metadata no basta para atribuir una licencia uniforme al código nuevo.
  Véanse [avisos](https://github.com/junglejuz/Dark-Souls-III-VR-Native/blob/441792c4d63ee8c945913d65aa434839263fca39/THIRD_PARTY_NOTICES.md)
  y [licencia explícita de scubasteve](https://github.com/scubasteve2365/DS3-VR/blob/14909f48da7ebb709af6a38c33fbc27cc91fd4a6/LICENSE).
