# DS3-VR como referencia para AC6 — 30-10-2026

**Comparación posterior, 01-11-2026:** [DS3-VR Native de junglejuz frente a
DS3-VR de scubasteve](DS3_NATIVE_STEREO_REFERENCE.md). El primero incorpora
una segunda vista del motor sin geo-11; el análisis siguiente se refiere
exclusivamente al proyecto de scubasteve, cuyo HEAD sigue siendo el mismo.

Referencia sugerida por el usuario: [scubasteve2365/DS3-VR](https://github.com/scubasteve2365/DS3-VR).
Revisión de fuentes fijada al commit
[`14909f48da7ebb709af6a38c33fbc27cc91fd4a6`](https://github.com/scubasteve2365/DS3-VR/commit/14909f48da7ebb709af6a38c33fbc27cc91fd4a6),
del 29-10-2026. El repositorio se creó el 22-10, después de nuestra investigación
externa del 21-10. Se consultaron README, código, licencia e historial;
la consulta de issues abiertos/cerrados devolvió cero resultados.
No se compiló ni ejecutó DS3-VR, no se inició AC6/SteamVR y no se cargaron
sondas. Las capacidades de DS3 descritas aquí proceden del código y de sus
autores; no son una aceptación física nuestra.

## Conclusión

Es una referencia prioritaria para estudiar cámara, constantes, presentación
OpenXR y HUD en otro juego de FromSoftware. **No proporciona una ruta D3D12
lista para AC6 ni demuestra una entrada nativa reentrante de render por ojo.**
DS3-VR consume el estéreo producido por geo-11 y el fix de Helifax/masterotaku;
el renderer estereoscópico subyacente no está incluido en este repositorio.
El [README fijado](https://github.com/scubasteve2365/DS3-VR/blob/14909f48da7ebb709af6a38c33fbc27cc91fd4a6/README.md)
atribuye expresamente a esa dependencia la generación de ambos ojos.
AC6 usa D3D12 y actualmente reproyecta una sola captura color/depth.

## Hallazgos comprobados en las fuentes

| Área | Implementación DS3 | Aplicación y límite en AC6 |
| --- | --- | --- |
| Exportación de cámara | `camhook.cpp` sustituye el slot 3 de una copia privada de la vtable del objeto de cámara. Aplica orientación, traslación y FOV antes de llamar al exportador original; ajusta aspecto en su salida. | Comparar el papel de exportación con nuestra copia de cámara y objetos de vista. No trasladar RVA, slot ni estructura sin comprobarlos en AC6. |
| Constantes de escena | `dlss.cpp` registra buffers enlazados a VS `b8` e intercepta `Map`/`Unmap` y `UpdateSubresource(1)` para leer/modificar sus datos. Documenta `cbSceneParam`: tamaño en `c1`, posición en `c3`, proyección en `c16..19`, vista-proyección en `c24..27`. | Es una pista concreta para examinar layouts y shaders de AC6. Esos registros y desplazamientos solo están identificados en DS3; nuestro binding D3D12 sigue pendiente. |
| Identificación de escena | Detecta candidatos color/depth y selecciona el color que lee el postprocesado, con el depth que más se enlazó con él. | Seguir uso posterior de recursos ayuda a distinguir escena de sombras/reflejos. El criterio de frecuencia no prueba procedencia temporal inequívoca. |
| Presentación VR | `xr.cpp` abre la textura estéreo compartida de geo-11 por la interfaz Katanga y la envía con `XrCompositionLayerProjection`, dos poses/FOV y subrectángulos por ojo. | Referencia para una presentación futura que sustituya nuestros quads de escena. Hay que describir la proyección realmente renderizada; cambiar el tipo de capa por sí solo no corrige geometría. |
| Escala y seguimiento | Obtiene IPD del runtime, usa una proyección simétrica que cubre el FOV necesario y compensa el desplazamiento de geo-11. Escala traslación de cabeza junto a escala del mundo. | Útil para estudiar coherencia entre movimiento y baseline. La convergencia base de DS3 es una calibración específica de esa integración; no convierte unidades de AC6 a metros. |
| HUD y lock-on | `hud.cpp` separa parte del HUD en una textura propia, conserva marcadores del mundo y entrega a shaders del marcador la transformación entre cámara del juego y cámara VR. | Referencia especialmente relevante para lock-on en AC6. Sus reglas de draws/texturas y sus parches de shader son específicos de DS3. |

Fuentes fijadas:
[cámara](https://github.com/scubasteve2365/DS3-VR/blob/14909f48da7ebb709af6a38c33fbc27cc91fd4a6/src/camhook.cpp#L16-L26),
[layout de constantes](https://github.com/scubasteve2365/DS3-VR/blob/14909f48da7ebb709af6a38c33fbc27cc91fd4a6/src/dlss.cpp#L240-L269),
[actualizaciones de buffers](https://github.com/scubasteve2365/DS3-VR/blob/14909f48da7ebb709af6a38c33fbc27cc91fd4a6/src/dlss.cpp#L409-L486),
[selección de escena](https://github.com/scubasteve2365/DS3-VR/blob/14909f48da7ebb709af6a38c33fbc27cc91fd4a6/src/dlss.cpp#L1460-L1499),
[presentación por ojo](https://github.com/scubasteve2365/DS3-VR/blob/14909f48da7ebb709af6a38c33fbc27cc91fd4a6/src/xr.cpp#L1042-L1093),
[HUD](https://github.com/scubasteve2365/DS3-VR/blob/14909f48da7ebb709af6a38c33fbc27cc91fd4a6/src/hud.cpp#L230-L274),
[inserciones de marcador](https://github.com/scubasteve2365/DS3-VR/blob/14909f48da7ebb709af6a38c33fbc27cc91fd4a6/installer/shader_inserts.h),
[escala](https://github.com/scubasteve2365/DS3-VR/blob/14909f48da7ebb709af6a38c33fbc27cc91fd4a6/src/worldscale.cpp#L14-L24).

## Qué no resuelve de nuestro bloqueo

DS3-VR también encuentra múltiples cámaras. En `OnCameraUpload` agrupa matrices
y cuenta uploads; `BeginFrame` elige la de mayor frecuencia y conserva la
anterior cuando falta una nueva. La pose de presentación se recupera por un
`PoseLag` configurable; el emparejamiento DLSS admite captura actual/anterior.
Son decisiones operativas de esa implementación, **no un testigo de identidad
por recurso, generación de command list, submission y fence**.

Fuentes: [candidatos](https://github.com/scubasteve2365/DS3-VR/blob/14909f48da7ebb709af6a38c33fbc27cc91fd4a6/src/dlss.cpp#L291-L333),
[cámara seleccionada](https://github.com/scubasteve2365/DS3-VR/blob/14909f48da7ebb709af6a38c33fbc27cc91fd4a6/src/dlss.cpp#L911-L930),
[emparejamiento DLSS](https://github.com/scubasteve2365/DS3-VR/blob/14909f48da7ebb709af6a38c33fbc27cc91fd4a6/src/dlss.cpp#L1247-L1258),
[consulta de pose](https://github.com/scubasteve2365/DS3-VR/blob/14909f48da7ebb709af6a38c33fbc27cc91fd4a6/src/camhook.cpp#L411-L418).
Por tanto no sustituye la cadena de evidencia que falta en
[nuestro renderer](NATIVE_RENDER_ROUTE.md#próximo-trabajo).

Hay similitud parcial entre estructuras, pero también diferencias concretas:
DS3 exporta FOV/near/far desde `out+0x50` y aspecto en `out+0x64`;
la vista AC6 documentada tiene FOV/near/far en `+0x50/+0x54/+0x58`,
aspecto en `+0x6c` y jitter en `+0x74/+0x78`.
La cámara fuente AC6 usa wrappers y FOV observado en `+0x58`.
Véanse [proyección AC6](../PROJECTION_FINDINGS.md#campos-y-coeficientes) y
[cámara AC6](../CAMERA_FINDINGS.md). No son objetos intercambiables.

## Investigación que habilita

La aportación inmediata es una hipótesis de layout/semántica de constantes:
contrastar en shaders y uploads ya identificados de AC6 qué bloques contienen
proyección, vista-proyección, posición, tamaño y jitter, y cómo los consume
el pase del depth elegido. Los bloques AC6 de 80/656/832 bytes **no se
identifican como `cbSceneParam` por tamaño o por similitud numérica**.
Un layout encontrado debe conservar el vínculo con recurso/rango, versión,
binding, draw y submission; la igualdad de matrices no basta.

Esta referencia refuerza estudiar shaders/constantes y el consumidor real.
No justifica repetir cobertura CPU v28 ni saltar a dos llamadas de render.
La presentación por ojo y la separación HUD/lock-on quedan como referencias
para etapas posteriores. No se altera el protocolo, los defaults ni la
aceptación física existente por esta revisión de fuentes.

## Procedencia y búsquedas

- Consulta KB: `um kb search "Dark Souls 3 VR"`; devolvió una nota de Elden
  Ring sin solución específica de DS3-VR/AC6 VR.
- GitHub: metadata, árbol recursivo de `main`, cinco commits recientes,
  issues `state=all` y archivos citados, fijados al commit anterior.
- Credibilidad: código primario verificado por lectura; comentarios sobre
  desensamblado/mediciones DS3 son afirmaciones del autor, no verificaciones
  independientes del binario. No hubo capturas ni pruebas visuales.
- La [licencia propia es MIT](https://github.com/scubasteve2365/DS3-VR/blob/14909f48da7ebb709af6a38c33fbc27cc91fd4a6/LICENSE).
  geo-11, el fix y otras dependencias tienen condiciones separadas según
  README/avisos. Esta revisión incorpora análisis y enlaces, no código tercero
  ni archivos del juego. No se descargaron instaladores ni dependencias.
