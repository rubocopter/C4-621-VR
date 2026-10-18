# Intrínsecos distintos por ojo: contrato experimental de host

Base: `8b8ee37` (PR #4), validada después en una prueba física acotada de
PSVR2 a 2560×1440/90°. Este incremento se desarrolla en otra rama y **no añade
un resultado visual** al modo nuevo. No modifica la rama ni los binarios de
esa prueba. No añade opciones al presentador, hooks, sesiones VR ni datos por
fotograma.

## Qué limitación elimina

La calibración estática del PR #4 permitía cambiar el modelo de profundidad,
pero ambos ojos debían usar los intrínsecos de la captura. Ahora la API C++
puede especificar escalas focales y punto principal distintos por ojo. Los
coeficientes recorren la ruta real `LiveStereo::Update` → root constants →
shader; hay pruebas GPU en procesos separados. La referencia NumPy acepta
el argumento opcional `target_matrix`.

Solo se modelan **rayos de cámaras paralelas trasladadas en X**, expresados en
los mismos ejes de vista. No hay rotación relativa de los ojos, traslación Y/Z,
clipping near/far de destino, render geométrico independiente ni integración
con `XrCompositionLayerProjection`. OpenXR sigue usando los quads anteriores.

## Matemática y convenciones

Para una matriz row-vector de la forma ya admitida, los intrínsecos son:

```text
fx = P[0], fy = P[5], cx = P[8]/Wsign, cy = P[9]/Wsign
NDCsourceEye = (fx*(X-eyeX)/forwardDistance + cx,
               fy*Y/forwardDistance + cy)
scale = targetFocal/sourceFocal
offset = targetPrincipal - scale*sourcePrincipal
NDCtarget = scale*NDCsourceEye + offset
```

En píxeles, partiendo del resultado de proyectar el ojo con la matriz fuente:

```text
targetX = (sourceProjectedX-width/2)*scaleX + (1+offsetX)*width/2
targetY = (sourceProjectedY-height/2)*scaleY + (1-offsetY)*height/2
```

El signo distinto en Y viene de la imagen con origen arriba y NDC positivo
hacia arriba. Escalar X **después del desplazamiento estéreo** convierte también
el paralaje de fx fuente a fx destino. La transformación identidad omite estas
operaciones para conservar la discretización anterior junto a límites de píxel.
Se conservan ordenación por device depth original, desempates y huecos explícitos.

## API, validación y compatibilidad

`ProjectionCalibration::eyeProjections` es opcional. Cuando existe, contiene
`RayIntrinsics` de la fuente y un array de dos destinos, izquierda/derecha.
`RayIntrinsics::FromRowMatrix` conserva el punto principal normalizado por el
signo de W. La fuente debe coincidir con fx/fy/Wsign del modelo de profundidad.
Los tres juegos de intrínsecos deben compartir ejes/Wsign.

Se rechazan escalas nulas/negativas/no finitas/subnormales, offsets no finitos o
subnormales, ratios no representables en float32, ojos fuera de 0/1 y fuentes
inconsistentes. La validación se realiza antes de abrir/claimar el stream. Las
transformaciones se calculan una vez al construir el consumidor y su calibración
se conserva por valor e inmutable.

La procedencia histórica no autoriza activar destinos distintos: se exige
`ExplicitUnassociated` y una fuente completa declarada por el llamador. Esto no
acredita que dichos datos pertenezcan al frame de AC6. El JSON conserva
`frame_associated:false` y `metric_scale_calibrated:false`; añade `image_intrinsics`
solo al configurar esa opción e informa correctamente si los ojos comparten
los intrínsecos fuente.

Sin esa opción se conserva el perfil histórico y la identidad.
El compilador solo habilita la transformación parametrizada cuando se configura
explícitamente: una prueba de reflexión DXBC comprueba que el shader por defecto
no lee ninguna de las cuatro constantes nuevas y el alternativo sí. La prueba
se observó fallar antes de aislar los modos y también se ejecuta en CI sin GPU.

Root constants
privadas: 72 bytes / 18 DWORDs, con offsets comprobados, empaquetadas como
escalares HLSL. El stream v1 de **120 bytes** y `AC6RPRJ1` de **48 bytes** no
cambian. El ejecutable offline sigue usando la sobrecarga de intrínsecos iguales;
no se ha añadido ni reinterpretado un formato binario.

## Evidencia reproducible

RED: la nueva llamada CPU no admitía `target_matrix`; el fixture GPU con
intrínsecos distintos falló en frame 3/ojo 0/píxel 2664 frente a reconstrucción
independiente. Otra regresión mostró un ratio no representable que producía
silenciosamente huecos en CPU; ahora se rechaza.

La revisión independiente encontró un Y no finito que podía eludir la
comparación de límites. Se reprodujo RED compilando el helper de producción con
IEEE estricto/sin optimización: intrínsecos finitos producen NaN intermedio.
El compilador optimizado rechazaba accidentalmente esos mismos valores por
reasociación. El shader ahora exige X **e Y** finitos antes de formar índices.
El kernel de regresión solo escribe 16 bytes en una dirección fija, nunca en
el índice calculado, para mantener segura la reproducción del fallo.

El cierre local pasa 33 pruebas Python y 16 CTests pertinentes (10 del
consumidor, uno offline con 25 casos GPU internos y cinco del presentador).
Los casos Python nuevos comparan contra
inversión/multiplicación 4×4, ambos signos de Z, forward/reverse-Z, dos ojos y
traslación cero, tres tamaños destino, offsets de ambas matrices, oclusiones,
huecos e identidad explícita. Los fixtures D3D12 comparan píxeles/previews/blits
exactos con reconstrucción CPU de puntos en vista y proyección destino: cuatro
modelos distintos y una identidad explícita, además de los perfiles anteriores.
Las suites del consumidor, offline y presentador comprueban también las rutas
de expiración, fences, reconexión y timeout. GPU local RTX 4070 Ti; sin debug
layer. No se afirma igualdad bit a bit fuera de los fixtures entre rasterizado
float64 y float32 junto a límites de píxel.

Una pasada local de la suite registró fallos en `live_stereo_output_1080p` y
`live_stereo_output_1440p`: caducó la fuente sintética antes de completar las
verificaciones de blit. No hubo discrepancia de píxeles ni se ampliaron los
500 ms. La comparación aislada posterior de 1440p pasó tanto con el binario
del PR #4 como con el nuevo. Estos fixtures conservan una dependencia del
tiempo real de verificación bajo carga; los logs de esa pasada se conservan.

```powershell
cmake -S native/live_stereo -B out/eye-projection-20261018/live_stereo -G 'Visual Studio 17 2022' -A x64
cmake --build out/eye-projection-20261018/live_stereo --config Release
ctest --test-dir out/eye-projection-20261018/live_stereo -C Release --output-on-failure
.venv-analysis/Scripts/python.exe -B -m unittest discover -s tests -v
```

Los logs RED/GREEN locales están en `out/eye-projection-20261018/`. No hace
falta otra prueba física mientras el modo siga sin activarse en presentación.
Para conectarlo al visor faltan una fuente de matriz/jitter asociada a captura,
escala/IPD verificados, poses/FOV destino y una presentación de escena adecuada.
