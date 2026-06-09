# Preguntas frecuentes (FAQ)

- ¿Cómo selecciono el perfil de hardware?
- ¿Qué hacer si el sensor de suelo no responde?
- ¿Cómo restablezco el dispositivo a valores de fábrica?
- ¿Cómo reporto un bug?

---

## Control de válvulas — latencia y comandos perdidos

### ¿Por qué la válvula tarda mucho en responder o el comando nunca llega?

El firmware no usa deep sleep. El delay observado (>10 s) tiene tres causas encadenadas:

1. **Clean session (pre-fix)**: PubSubClient conectaba con `cleanSession=true`. Si el ESP
   estaba reconectando WiFi/MQTT cuando el usuario pulsó "abrir válvula", el broker descartaba
   el mensaje QoS 1 en cuanto el cliente desconectaba.

2. **Sin fallback HTTP en modo MQTT**: `checkRelayCommand()` (polling HTTP cada 2 s) solo
   activa en el modo HTTP legacy (`#else` de `USE_MQTT`). En modo MQTT no existe segunda
   oportunidad si el mensaje se perdió.

3. **Re-publicación solo en reboot**: el backend solo re-enviaba el relay deseado al recibir
   una alerta `device_reboot`. Un drop de MQTT sin reinicio no la activaba.

### Solución implementada (junio 2026)

**Firmware (`mqtt_helpers.h`)** — sesión persistente:
```cpp
// cleanSession=false → broker encola QoS 1 mientras el ESP esté offline
mqttClient.connect(client_id, user, pass, NULL, 0, false, NULL, false);
```
Requiere recompilar y flashear el firmware.

**Backend (`mqtt_client.py`)** — re-publicación en cualquier reconexión:
Ahora `_handle_register()` (llamada cada vez que el ESP envía su mensaje de registro al
reconectar MQTT) re-publica todos los relays con `desired=1` en la DB, igual que ya hacía
el handler de `device_reboot`.

**Backend (`app.py`)** — telemetría rápida durante riego:
Al abrir una válvula (`state=true`), el comando MQTT incluye `telemetry_interval_s: 5`
(de 20 s a 5 s). Al cerrarla se restaura a 20 s. El dispositivo aplica el cambio en
`mqttCallback` sin reinicio.

### Flujo de un comando de válvula (estado actual)

```
App → POST /api/relay
        ├─ DB: relay_state.desired = 1
        └─ MQTT publish QoS 1 → aquantia/<finca_id>/cmd
                                  {"relay":0,"state":true,"mac":"...","telemetry_interval_s":5}

ESP conectado:      mqttCallback() → GPIO HIGH → ACK HTTP → datos cada 5 s  (<200 ms)
ESP reconectando:   broker retiene el mensaje (sesión persistente) →
                    en cuanto reconecta recibe el cmd → GPIO HIGH             (<tiempo reconexión)
ESP rebootea:       mqttPublishRegister() → backend re-publica cmd             (segundos)
```
