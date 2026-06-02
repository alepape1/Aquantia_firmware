# Aquantia Firmware - Quick Diagnostics

1. Perfil afectado: `METEO`, `IRRIGATION`, `AQUALEAK`, `AQUA_SMART_REMOTE`.
2. Sintoma principal: MQTT/TLS, sensores, pipeline, provisioning o flash.
3. Leer solo 2-4 archivos clave, no todo el sketch.
4. MQTT SIM7000G: revisar `networkTask` y `mqttConnect` en `.ino`.
5. Confirmar host/port/protocol parity con broker/backend (`1883` vs `8883`).
6. Si hay `-4` en `8883`: validar TLS path antes de tocar sensores.
7. TLS SIM: confirmar SNI, timeout extendido y contexto SSL (`ctx 0/1`).
8. Verificar que TLS client se prepara en cada intento de conexion.
9. Revisar recovery tras timeout: refresh PDP activo.
10. MQTT buffers esperados: client `2048`, serializacion telemetry `1280`.
11. Alertas MQTT: edge-triggered, sin spam.
12. Pipeline/fugas: leer `LeakDetector.h` + bloque rapido de pipeline.
13. XDB401: sentinel invalido debe ser `NAN` (no `-1.0f`).
14. I2C estable: `Wire.setTimeOut(200)` y frecuencia 50 kHz.
15. Estado cross-core: no usar `String` compartido; proteger writes con `dataMutex`.
16. `secrets.h` es solo dev/local, no produccion.
17. No abrir backend salvo mismatch de topic/payload/endpoint.
18. Si hace falta backend: revisar `../app_meteo/backend/mqtt_client.py` y `app.py`.
19. No leer rutas pesadas: `wiki/assets`, `.claude/worktrees`, binarios.
20. Compilar con flags custom en `compiler.cpp.extra_flags`.
21. Ejecutar compile del perfil afectado y validar sin errores.
22. Confirmar en monitor serial el escenario exacto antes/despues del cambio.
23. Hacer cambio minimo verificable; evitar refactor amplio.
24. Actualizar `CHANGELOG.md` en `[Unreleased]`.
