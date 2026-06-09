# Uso del firmware

Explicación de los modos de operación, perfiles soportados y comandos principales.

- Cambiar perfil de hardware
- Configuración de red y MQTT
- Lectura de sensores
- Actualización OTA

---

## Control de relays / válvulas

Los relays se controlan exclusivamente por MQTT en modo `USE_MQTT`. No existe polling HTTP
de relay en este modo — `checkRelayCommand()` solo opera en el modo HTTP legacy.

### Comando de apertura/cierre

El backend publica en `aquantia/<finca_id>/cmd`:
```json
{
  "relay": 0,
  "state": true,
  "mac": "FC:B4:67:F3:77:48",
  "telemetry_interval_s": 5
}
```
- `mac` opcional — si presente, el dispositivo descarta el mensaje si no coincide con su MAC.
- `telemetry_interval_s` — el backend lo incluye automáticamente: 5 s al abrir, 20 s al cerrar.
  El dispositivo lo aplica en caliente en `mqttCallback` sin reinicio.

### Sesión persistente

`mqttConnect()` usa `cleanSession=false`. El broker (Mosquitto) almacena los mensajes QoS 1
dirigidos a este cliente mientras está offline y los entrega al reconectar. El client ID
`aquantia-{MAC_sin_colons}` es estable entre reconexiones.

> **Importante:** si se reinicia el broker o se cambia el client ID, la sesión persistente
> se pierde. El mecanismo de seguridad en ese caso es la re-publicación que hace el backend
> cuando el dispositivo envía su mensaje `register` al reconectar.

### Confirmación (ACK)

Al detectar un cambio en el relay, el dispositivo hace `POST /api/relay/ack` con el bitmask
real de todos los relays. El backend actualiza `relay_state.actual` con ese valor.

### Intervalo de telemetría dinámico

| Situación | `telemetryIntervalMs` |
|---|---|
| Reposo | 20 000 ms (20 s) |
| Válvula abierta | 5 000 ms (5 s) |
| Válvula cerrada | 20 000 ms restaurado por comando |

El valor se puede cambiar en cualquier momento enviando `telemetry_interval_s` en cualquier
mensaje al topic `cmd`. Rango permitido: 5–3600 s.
