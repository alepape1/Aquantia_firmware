# Preguntas frecuentes (FAQ)

- [¿Cómo selecciono el perfil de hardware?](#cómo-selecciono-el-perfil-de-hardware)
- [El sensor de suelo no responde / da valores incorrectos](#el-sensor-de-suelo-no-responde--da-valores-incorrectos)
- [El caudal no vuelve a cero al parar la bomba](#el-caudal-no-vuelve-a-cero-al-parar-la-bomba)
- [¿Cómo restablezco el dispositivo a valores de fábrica?](#cómo-restablezco-el-dispositivo-a-valores-de-fábrica)
- [La válvula tarda en responder o el comando nunca llega](#control-de-válvulas--latencia-y-comandos-perdidos)

---

## ¿Cómo selecciono el perfil de hardware?

El perfil se pasa en tiempo de compilación con `-DDEVICE_PROFILE=N`. No hay que editar el
código fuente. Ver [Instalación](Instalacion) para los comandos `arduino-cli` por perfil, o
usar el Flash Tool GUI (`tools/flasher_gui.py`) que lo gestiona con un selector desplegable.

| N | Perfil |
|---|--------|
| 1 | METEO (LilyGo TTGO T-Display) |
| 2 | IRRIGATION (ESP32 4-Relay Board) |
| 3 | AQUALEAK (Wemos D1 Mini ESP32) |
| 4 | AQUA_SMART_REMOTE (LilyGO T-SIM7000G) |

---

## El sensor de suelo no responde / da valores incorrectos

El sensor Helissense RS485 necesita que su **dirección Modbus** esté sincronizada con el
firmware. Si cambió de dispositivo, se reconfiguraron el sensor o la NVS se borró, la
dirección guardada puede no coincidir con la del sensor físico.

### Síntomas
- Log serial: `[SOIL] Sin respuesta en addr X`
- Telemetría: `soil_temp`, `soil_humidity`, etc. ausentes o siempre `null`.

### Solución: SoilProvisioner

**Opción 1 — MQTT** (dispositivo encendido y conectado):
```json
{"cmd": "provision_soil"}
```
Publicar en `aquantia/<finca_id>/cmd`. El firmware ejecuta `soilBusScan()` (escanea
direcciones 1–8), encuentra el sensor, lo reasigna a una dirección libre y guarda en NVS.

**Opción 2 — Flash Tool GUI**:
Abrir `tools/flasher_gui.py` → botón **Provision Soil**.

**Opción 3 — `factory_provision.py`**:
```powershell
python tools/factory_provision.py --provision-soil
```

### Verificar el resultado

Tras el provisioning, el log serial debe mostrar:
```
[SoilProvisioner] Sensor encontrado en addr X
[SoilProvisioner] Dirección guardada en NVS: X
```
Y `soilBusLoadAddress()` cargará esa dirección en el próximo boot.

> **Nota:** si el sensor tarda en aplicar la nueva dirección, esperar ~100 ms y verificar
> con `probe(newAddr)` antes de asumir fallo.

---

## El caudal no vuelve a cero al parar la bomba

### Causa

Antes del fix `fix(pipeline): zero _flowLpm on flow stop`, `_flowLpm` podía permanecer
positivo hasta 500 ms después de que la bomba se detuviera, porque el recálculo solo
ocurría en el siguiente intervalo de 500 µs con pulsos.

### Solución implementada (junio 2026)

El firmware incluye ahora un **ghost flow guard** en `pipeline_core.h`:
si `_flowLpm > 0` pero no llegan pulsos en el intervalo actual y ese intervalo supera
2 periodos esperados al caudal previo, `_flowLpm` se zerifica inmediatamente.

Si ves este comportamiento en firmware anterior a `1912520`, flashea la versión actual.

---

## ¿Cómo restablezco el dispositivo a valores de fábrica?

Borra la NVS con el Flash Tool GUI (botón **Erase NVS**) o con `esptool.py`:

```powershell
python -m esptool --port COM11 erase_region 0x9000 0x6000
```

El dispositivo arrancará en modo SoftAP provisioning en el siguiente boot.

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
