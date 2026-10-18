# Orientación de cabeza — referencia histórica

El ensayo del 15-10-2026 confirmó seguimiento de cámara en el **monitor**, sin
imagen en el visor. Su publicación Python headless usaba `flip_z=false`; la
calibración posterior corrigió ejes y combinó imagen y orientación en una
misma sesión gráfica OpenXR. Aquella secuencia no es el procedimiento actual.

Usar la [prueba vigente de estéreo continuo](docs/VR_TRIAL_RUNBOOK.md), con
build aislada, sondas nuevas/no mapeadas, consumidor explícito y perfil
2560×1440/90°. El [estado verificado](docs/STATUS.md) recoge la aceptación
posterior en PSVR2 y sus límites.

La investigación original se conserva en [cámara](CAMERA_FINDINGS.md),
[presentación monoscópica histórica](VR_DISPLAY_FINDINGS.md),
[MODLOG](MODLOG.md) e historial Git de esta página. Se retiran los comandos
antiguos de inyección para evitar confundirlos con la ruta vigente; no se
eliminan herramientas ni evidencia original.
