# Seguridad — Firmware Aquantia

> **Auditoría realizada:** 2026-06-09 · **Rama base:** `feat/per-profile-device-ids` · **Versión firmware:** `0.3.0-beta`
> **Puntuación global: 4.8/10** — No apto para piloto con administración pública en estado actual.

---

## Tabla de puntuaciones por área

| Área | Puntuación | Estado |
|------|:----------:|--------|
| 1. Credenciales y secretos | **4/10** | Fallo crítico: secretos en historial git |
| 2. TLS / Comunicaciones | **5/10** | TLS condicional; sin validación CA en celular |
| 3. Autenticación de dispositivo | **6/10** | Correcto en PROD; DEV_MODE comparte credenciales |
| 4. Control de acceso MQTT | **5/10** | Topic compartido por finca; validación solo en payload |
| 5. Seguridad OTA | **2/10** | Sin contraseña, sin firma, sin TLS |
| 6. Secure Boot / Flash Encryption | **0/10** | Ambos completamente deshabilitados |
| 7. Validación de inputs | **7/10** | Sólido con matices en tamaño de JSON |
| 8. Watchdog y resiliencia | **8/10** | Bien implementado |
| 9. Gestión de memoria | **7/10** | Buena base; heap dinámico en paths críticos |
| 10. Logging e información | **6/10** | DEBUG correcto; excepciones en AP y Serial |
| 11. Seguridad física / hardware | **2/10** | JTAG activo, UART siempre on |
| 12. Dependencias y librerías | **6/10** | Sin versiones fijadas |

---

## Auditoría PRE-implementación (2026-06-09)

### Área 1 — Credenciales y secretos

**❌ FALLA CRÍTICA** — Credenciales WiFi y MQTT en historial git
`ESP_monitor_server/secrets - copia.h` fue committeado en la rama `develop`. El `.gitignore` cubre `secrets.h` y variantes, pero no cubría `secrets - copia.h`. Contiene en claro: `WIFI_PASSWORD`, `MQTT_USER "backend"`, `MQTT_PASS "aquantia_159"`, `MQTT_PORT 1883`. Aunque se elimine el archivo, las credenciales permanecen en el historial y son recuperables con `git log -p`.

**⚠️ PARCIAL** — `secrets.h` no trackeado pero tiene `DEV_MODE` activo
El `secrets.h` real está correctamente ignorado por `.gitignore`. Sin embargo, incluye `#define DEV_MODE` sin comentar, lo que implica que un build directo desde el repositorio sin editar este archivo compila en modo desarrollo con `MQTT_PORT 1883` (TCP plano, sin TLS).

**⚠️ PARCIAL** — Contraseña SoftAP hardcodeada igual en todos los dispositivos
`provisioning.h:329`: `WiFi.softAP(ap_ssid, "aquantia1")` — contraseña fija universal. El SSID `Aquantia-XXXXXX` es visible en cualquier escaneo WiFi. Cualquier persona puede conectarse al portal de provisioning sin acceso físico al hardware.

**✅ CUMPLE** — NVS para credenciales en producción
`provisioning.h`: en modo `!DEV_MODE` las credenciales se almacenan en NVS (Preferences) con `strlcpy` y tamaños correctos.

**❌ FALLA** — NVS no cifrado
`build/sdkconfig`: `# CONFIG_FLASH_ENCRYPTION_ENABLED is not set`. El NVS almacena el `mqtt_token` y el WiFi password en flash sin cifrar. Con acceso físico y `esptool`, se puede volcar la flash entera y leer todos los secretos.

**✅ CUMPLE** — No hay claves privadas embebidas
`mqtt_cert.h`: solo contiene el bundle de CA (Let's Encrypt R13 + ISRG Root X1). Ninguna clave privada en el código.

**⚠️ PARCIAL** — `devices_registry.csv` con MACs reales
`tools/devices_registry.csv` contiene MACs en claro de todos los dispositivos provisionados con rutas locales del operador. No son credenciales, pero son superficie de reconocimiento.

---

### Área 2 — Seguridad TLS / Comunicaciones

**⚠️ PARCIAL** — TLS condicional según puerto, no forzado
`ESP_monitor_server.ino:~1861`:
```cpp
if (mqtt_port == 8883) {
    prepareSecureClient(mqttTLSClient, 10000);
    mqttClient.setClient(mqttTLSClient);
} else {
    mqttClient.setClient(mqttTCPClient);  // TCP plano
}
```
La activación de TLS depende del valor de `MQTT_PORT` en `secrets.h`. Con `MQTT_PORT 1883` (estado actual del fichero de desarrollo), el firmware se conecta por TCP plano.

**✅ CUMPLE** — Validación de CA cuando TLS está activo (path WiFi)
`prepareSecureClient()`: `client.setCACert(MQTT_CA_CERT_PEM)` carga el bundle de Let's Encrypt. No se llama a `setInsecure()` en ningún path WiFi. Bundle válido hasta 2027 (R13).

**❌ FALLA CRÍTICA** — TLS sin validación de CA en path celular (SIM7000G)
Rama `feat/SIM_MODEM`, `ESP_monitor_server.ino:~1244`:
```cpp
// "Prueba temporal" que quedó en el código:
snprintf(sslCmd, ..., "+CSSLCFG=\"authmode\",%u,0", sslCtx);
// authmode=0 = sin verificación de CA ≡ setInsecure()
```
Adicionalmente `ignorertctime=1` desactiva la validación del timestamp del certificado.

**✅ CUMPLE** — NTP sincronizado antes del handshake TLS
`ESP_monitor_server.ino:~2543`: Se sincroniza NTP antes de la tarea de red. `tlsClockReady()` verifica epoch > 1700000000 antes del handshake.

**⚠️ PARCIAL** — HTTP sin token de autenticación del dispositivo
`http_client.h`: `checkRelayCommand()` identifica el dispositivo solo por MAC en query string, sin token firmado. La seguridad depende enteramente del servidor.

---

### Área 3 — Autenticación de dispositivo

**✅ CUMPLE** — Identidad única por dispositivo basada en MAC (eFuse)
`provisioning.h:~34`: `device_serial_get()` lee la MAC desde `esp_read_mac(..., ESP_MAC_WIFI_STA)` directamente de eFuse. Formato `AQ-{MAC12hex}`, único por hardware.

**✅ CUMPLE** — MQTT user = MAC en producción (per-device)
`ESP_monitor_server.ino:~2464`: En `!DEV_MODE`, `mqtt_user` se sobreescribe con la MAC del dispositivo y `mqtt_pass` con el token de NVS. Cada dispositivo tiene credenciales únicas.

**⚠️ PARCIAL** — Token sin expiración ni rotación
El token generado en `factory_provision.py` es un `secrets.token_urlsafe(32)` hasheado con bcrypt (coste 12). No hay mecanismo de rotación ni expiración. Un token comprometido permanece válido indefinidamente.

**❌ FALLA** — En DEV_MODE todos los dispositivos comparten usuario/contraseña
`secrets.h` (DEV_MODE): `MQTT_USER "backend"`, mismo password para todos. Rompe el modelo de identidad per-device.

**⚠️ PARCIAL** — Validación de destino de comandos MQTT por MAC en payload
`mqttCallback:~1692`: La validación es `if (targetMac != selfMac) return`. El campo `mac` viene en el payload JSON, no en el topic. Si se omite el campo `mac`, el comando se ejecuta en todos los dispositivos suscritos.

---

### Área 4 — Control de acceso a topics MQTT

**⚠️ PARCIAL** — Topic de comandos compartido por finca, no por dispositivo
`ESP_monitor_server.ino:~1775`: `subscribe("aquantia/%s/cmd", finca_id)`. Todos los dispositivos de la misma finca suscriben al mismo topic. La discriminación por MAC está en el payload (ver Área 3).

**⚠️ PARCIAL** — Topics de telemetría exponen datos internos
Topic `aquantia/{finca_id}/telemetry` incluye `mac_address`, `ip_address`, `free_heap`, `relay_active`, `firmware_version`. Información de reconocimiento si el broker no aplica ACLs de lectura.

**➖ NO APLICA** — ACLs del broker Mosquitto
La configuración de `mosquitto-go-auth` está en el backend y fuera del alcance de este firmware.

---

### Área 5 — Seguridad OTA

**❌ FALLA CRÍTICA** — ArduinoOTA sin contraseña
`ESP_monitor_server.ino:~2491`:
```cpp
#ifdef OTA_PASSWORD
    ArduinoOTA.setPassword(OTA_PASSWORD);
#endif
```
`OTA_PASSWORD` no está definido. Cualquier equipo en la misma red WiFi puede flashear firmware arbitrario sin autenticación.

**❌ FALLA** — Sin verificación de integridad ni firma del binario
ArduinoOTA transfiere el binario sin SHA256 ni firma digital. Un atacante con acceso a la red puede inyectar firmware malicioso.

**❌ FALLA** — OTA sobre UDP sin cifrado de transporte
`ota_flash.sh` usa `espota.py` directamente. La transferencia del binario va en claro.

**⚠️ PARCIAL** — Comportamiento fail-safe durante OTA
`ArduinoOTA.onStart()`: apaga todos los relays (`digitalWrite(RELAY_PINS[i], LOW)`) antes de flashear. Correcto para infraestructura de agua.

**✅ CUMPLE** — Rollback automático via partición OTA dual
`partitions.csv`: `app0` + `app1` + `otadata`. Si el firmware nuevo no arranca, el bootloader ESP32 revierte a la partición anterior.

**❌ FALLA** — Hostname OTA estático y predecible
`ESP_monitor_server.ino:~2490`: `ArduinoOTA.setHostname("meteostation-esp32")` — igual en todos los dispositivos. Localizable por mDNS desde cualquier equipo en la red.

---

### Área 6 — Secure Boot y Flash Encryption

**❌ FALLA CRÍTICA** — Secure Boot no habilitado
`build/sdkconfig`: `# CONFIG_SECURE_BOOT is not set`. El chip soporta Secure Boot V1 (`CONFIG_SECURE_BOOT_V1_SUPPORTED=y`) pero no está habilitado. Cualquier persona con acceso físico puede flashear firmware no autorizado.

**❌ FALLA CRÍTICA** — Flash Encryption no habilitada
`build/sdkconfig`: `# CONFIG_FLASH_ENCRYPTION_ENABLED is not set`. La flash completa (código, NVS con tokens y credenciales WiFi, filesystem) es legible con `esptool` desde un portátil.

**❌ FALLA** — JTAG habilitado
`build/sdkconfig`: `CONFIG_ESP_DEBUG_OCDAWARE=y`. Un atacante con sonda JTAG puede leer memoria RAM en ejecución, extraer claves y ejecutar código arbitrario.

**⚠️ PARCIAL** — Tabla de particiones tiene coredump separado
`partitions.csv`: partición `coredump` en `0x3F0000`. Sin Flash Encryption, puede contener información sensible de memoria (tokens, keys en RAM).

---

### Área 7 — Validación de inputs y datos

**✅ CUMPLE** — Validación de rangos de sensores
`ESP_monitor_server.ino`: funciones como `readBMP280Temperature`, `xdb401_read`, `hdc1080_readTemp` incluyen validación de rangos físicos. `xdb401`: `pb >= -0.5f && pb <= fs_bar * 1.05f`.

**✅ CUMPLE** — Whitelist de valores para comandos MQTT
`mqttCallback`: `pipeline_scenario` y `pipeline_mode` validados contra whitelist explícita con `strcmp`. No se acepta ningún valor arbitrario.

**✅ CUMPLE** — Validación de bitmask de relay
`parseRelayBitmask`: verifica que la respuesta sea numérica y esté en rango `[0, 2^RELAY_COUNT - 1]`.

**⚠️ PARCIAL** — `StaticJsonDocument<256>` podría truncar payloads diseñados
`ESP_monitor_server.ino:~1687`: buffer MQTT es 1024 bytes pero el documento JSON solo procesa 256 bytes. Un payload diseñado cuidadosamente podría colocar el campo `relay` antes del campo `mac` y procesarse sin validación de destino.

**✅ CUMPLE** — No se usa `strcpy` / `sprintf` sin límites
Búsqueda exhaustiva: todos los usos de copia usan `strlcpy(dst, src, sizeof(dst))` o `snprintf(buf, sizeof(buf), ...)`.

---

### Área 8 — Watchdog y resiliencia

**✅ CUMPLE** — Hardware watchdog configurado
`networkTask:~1843`: `esp_task_wdt_config_t wdt_cfg = { .timeout_ms = 30000, .trigger_panic = true }`. Watchdog a 30s con panic+reboot en caso de hang.

**✅ CUMPLE** — Reconexión WiFi con backoff exponencial
`ESP_monitor_server.ino:~1880`: backoff de 500ms a 5s con `wifiRetryDelayMs *= 2`. Reset del backoff tras 10s de conexión estable.

**✅ CUMPLE** — Reconexión MQTT con backoff
`ESP_monitor_server.ino:~1905`: backoff MQTT de 2s incrementando hasta 15s máximo.

**✅ CUMPLE** — Fail-safe de válvulas en estados de error
`setup()` y `onStart()` OTA: `digitalWrite(RELAY_PINS[i], HIGH)` siempre inicializa en OFF. Las válvulas quedan en estado seguro en reset.

**✅ CUMPLE** — Timeout en operaciones de red
`httpGet`: `http.setTimeout(10000ms)`. `prepareSecureClient`: `setHandshakeTimeout(seconds)`.

---

### Área 9 — Gestión de memoria

**✅ CUMPLE** — Buffers estáticos con tamaños correctos
Uso consistente de `char buf[N]` con `snprintf`/`strlcpy`. Buffers `prov_ssid[64]`, `prov_password[64]`, `prov_mqtt_token[72]`.

**✅ CUMPLE** — ISR en `IRAM_ATTR`
`flowPulseISR()` declarado con `IRAM_ATTR`.

**⚠️ PARCIAL** — `String` de Arduino en paths HTTP
`syncPipelineScenario` y `httpGet` usan `String body = http.getString()` que aloca en heap. En sesiones largas con muchas reconexiones puede fragmentar el heap. No hay límite al tamaño de respuesta HTTP aceptada.

**✅ CUMPLE** — Variables de estado cross-core como `char[]`
`pipelineScenario` y `pipelineMode` son `char[16]`, no `String`. Decisión de diseño explícita para evitar races entre cores.

---

### Área 10 — Logging y exposición de información

**✅ CUMPLE** — Macros `DEBUG_MODE` con no-op en producción
`ESP_monitor_server.ino:~26`: `DLOGF`/`DLOGLN`/`DLOG` son no-op cuando `DEBUG_MODE` no está definido.

**⚠️ PARCIAL** — `mqtt_user` se loga en DEBUG_MODE
`ESP_monitor_server.ino:~2486`: `DLOGF("[MQTT] Auth user: %s | Serial: %s\n", mqtt_user, ...)`. En campo con DEBUG_MODE activo, la MAC/usuario MQTT aparece en Serial. El password nunca se loga.

**⚠️ PARCIAL** — Contraseña AP siempre visible por Serial
`provisioning.h:~321`: `Serial.printf("[PROV] ... Pass: aquantia1\n", ap_ssid)`. No está bajo `DLOGF`, se imprime siempre independientemente de DEBUG_MODE.

---

### Área 11 — Seguridad física y de hardware

**❌ FALLA** — JTAG no deshabilitado
`sdkconfig`: `CONFIG_ESP_DEBUG_OCDAWARE=y`. Ver Área 6.

**❌ FALLA** — Serial/UART de debug activo en producción
`setup()`: `Serial.begin(115200)` se ejecuta siempre. El puerto UART está activo y cualquier `Serial.printf` no guarnecido por macros es capturble con un adaptador USB-Serial.

**⚠️ PARCIAL** — Factory reset por GPIO0 sin notificación al servidor
`provisioning.h`: el factory reset requiere 3s pulsados en GPIO0. No hay logging del evento al servidor antes del borrado de NVS.

---

### Área 12 — Dependencias y librerías

**⚠️ PARCIAL** — Versiones de librería no fijadas
El proyecto usa Arduino IDE directamente, sin `platformio.ini`. Las versiones de PubSubClient, ArduinoJson, Adafruit_BMP280, etc. no están fijadas. Una actualización del Library Manager puede cambiar silenciosamente el comportamiento.

**✅ CUMPLE** — Librerías de fuentes verificadas
PubSubClient, ArduinoJson, Adafruit_BMP280, DHTesp son librerías establecidas con mantenimiento activo.

**⚠️ PARCIAL** — TinyGSM sin mantenimiento activo desde 2023
La rama `feat/SIM_MODEM` usa TinyGSM con soporte limitado para SIM7000SSL, lo que contribuyó a la solución `authmode=0`.

---

## Top 5 Vulnerabilidades Críticas

### C1 — Credenciales WiFi y MQTT en repositorio git
**CVSS:** 9.1 (AV:N/AC:L/PR:N/UI:N/S:U/C:H/I:H/A:N)
**Archivo:** `ESP_monitor_server/secrets - copia.h` (rama `develop`)
**Riesgo:** Las credenciales `WIFI_PASSWORD`, `MQTT_USER`, `MQTT_PASS` están en el historial git. Son recuperables por cualquier persona con acceso al repositorio o a cualquier clon, ahora o en el futuro. Si el repo estuvo accesible (GitHub, Gitea), las credenciales deben considerarse comprometidas.

### C2 — ArduinoOTA sin contraseña ni autenticación
**CVSS:** 8.8 (AV:A/AC:L/PR:N/UI:N/S:U/C:H/I:H/A:H)
**Archivo:** `ESP_monitor_server.ino:~2491`
**Riesgo:** Cualquier equipo en la misma WiFi puede flashear firmware arbitrario en los dispositivos desplegados. El endpoint UDP 3232 responde a `espota.py` sin ninguna verificación. Permite persistencia, modificación del comportamiento de válvulas, o brickeo.

### C3 — TLS sin validación de CA en path celular (SIM7000G)
**CVSS:** 8.8 (AV:N/AC:H/PR:N/UI:N/S:U/C:H/I:H/A:N)
**Archivo:** `feat/SIM_MODEM:ESP_monitor_server.ino:~1244`
**Riesgo:** `authmode=0` desactiva la verificación del certificado del servidor en el path GSM. MITM sobre red 1NCE/Onomondo permite capturar toda la telemetría e inyectar comandos de válvulas remotamente. En infraestructura de agua esto permite inundar o cortar el suministro.

### C4 — Flash Encryption y Secure Boot deshabilitados
**CVSS:** 7.8 (AV:P/AC:L/PR:N/UI:N/S:U/C:H/I:H/A:H)
**Archivo:** `build/sdkconfig`
**Riesgo:** Con acceso físico (realista en despliegue exterior) se puede: (a) volcar toda la flash con `esptool` y extraer tokens MQTT y credenciales WiFi del NVS; (b) flashear firmware backdoored sin restricción.

### C5 — Contraseña SoftAP universal en todos los dispositivos
**CVSS:** 7.5 (AV:A/AC:L/PR:N/UI:N/S:U/C:H/I:H/A:N)
**Archivo:** `provisioning.h:~329`
**Riesgo:** La contraseña `aquantia1` es igual en todos los dispositivos. El SSID `Aquantia-XXXXXX` es visible en cualquier escaneo. Cualquier persona puede conectarse al portal de provisioning y redirigir el dispositivo a su infraestructura sin acceso físico.

---

## Plan de Remediación

### CRÍTICA — Semana 1

| ID | Acción | Archivos afectados | Esfuerzo |
|----|--------|--------------------|----------|
| **C1** | Eliminar `secrets - copia.h` del historial git con `git filter-repo` + rotación de todas las credenciales comprometidas | `.gitignore`, historial git | 2h + infra |
| **C2** | Definir `OTA_PASSWORD` único por dispositivo en `secrets.h` **o** migrar a HTTPS OTA pull con verificación SHA256 | `ESP_monitor_server.ino`, `secrets.h` | 1-2 días |
| **C3** | Cambiar `authmode=0` → `authmode=1` en `prepareGsmTLSClient()` + eliminar `ignorertctime=1` | `gsm_modem.h` / `ESP_monitor_server.ino` (feat/SIM_MODEM) | 2-4h + test HW |

### ALTA — Semanas 2-3

| ID | Acción | Archivos afectados | Esfuerzo |
|----|--------|--------------------|----------|
| **A1** | Habilitar Flash Encryption + Secure Boot V2 en proceso de fábrica | `sdkconfig`, `factory_provision.py` | 2-3 días |
| **A2** | Contraseña AP derivada del serial del dispositivo: `AQ-{últimos 8 hex del MAC}` | `provisioning.h:~329` | 2-3h |
| **A3** | Forzar TLS en producción con `static_assert(MQTT_PORT == 8883)` | `ESP_monitor_server.ino:~1861` | 1h |
| **A4** | Aumentar `StaticJsonDocument<512>` en `mqttCallback` + rechazar payloads sin campo `mac` | `ESP_monitor_server.ino:~1687` | 1h |

### MEDIA — Sprint siguiente

| ID | Acción | Archivos afectados | Esfuerzo |
|----|--------|--------------------|----------|
| **M1** | Deshabilitar JTAG via eFuse + no inicializar `Serial` en builds de producción | `sdkconfig`, `ESP_monitor_server.ino` | 4h + test |
| **M2** | Endpoint de rotación de tokens MQTT (backend + firmware) | `factory_provision.py`, backend | 1-2 días |
| **M3** | Topic MQTT de comandos por dispositivo: `aquantia/{finca}/{mac}/cmd` | `ESP_monitor_server.ino:~1775` | 2h + ACLs broker |
| **M4** | Fijar versiones de librerías (migrar a PlatformIO o `arduino_library_list.txt`) | `platformio.ini` (nuevo) | 4h |

### BAJA — Backlog

| ID | Acción |
|----|--------|
| **B1** | Añadir límite de tamaño a respuestas HTTP en `httpGet` |
| **B2** | Logging del factory reset al servidor antes de borrar NVS |
| **B3** | Eliminar o pseudonimizar MACs reales en `tools/devices_registry.csv` |
| **B4** | Eliminar rutas absolutas del desarrollador en `devices_registry.csv` |
| **B5** | Añadir mínimo > 0 para `display_timeout_s` en `syncPipelineScenario` |

---

## Checklist ENS Básico — Piloto CIAL (Administración pública española)

> **Marco normativo:** Esquema Nacional de Seguridad (ENS) — RD 311/2022, categoría **Básica**.

| Medida ENS | Descripción | Estado | Bloqueante |
|---|---|:---:|:---:|
| `[mp.com.1]` | Perímetro seguro — TLS en todas las conexiones a internet | ❌ | Sí |
| `[mp.com.2]` | Protección de confidencialidad — cifrado de datos en tránsito | ❌ | Sí |
| `[mp.com.3]` | Autenticación del canal — validación CA del servidor | ⚠️ | Sí |
| `[mp.si.2]` | Criptografía — algoritmos y protocolos actualizados (TLS 1.2+) | ✅ | — |
| `[org.4]` | Proceso de autorización — control de acceso a funciones críticas | ⚠️ | Sí |
| `[mp.per.3]` | Gestión de secretos — no credenciales en repositorio | ❌ | Sí |
| `[op.exp.2]` | Configuración bastionada del hardware (Secure Boot, Flash Enc.) | ❌ | Sí |
| `[op.exp.4]` | Actualizaciones seguras de software — OTA autenticado | ❌ | Sí |
| `[op.exp.9]` | Registro de actividad — trazabilidad de acciones en dispositivos | ⚠️ | No |
| `[mp.if.3]` | Protección de los equipos — cifrado en reposo | ❌ | Sí |
| `[op.pl.1]` | Análisis de riesgos — identificación y tratamiento de amenazas | ⚠️ | No |

**Resultado:** 6 de 11 medidas en fallo o parcial. Los ítems **C1, C2, C3, A1, A3** son bloqueantes para cualquier entrega formal a administración pública.

---

## Auditoría POST-implementación

> **Estado:** 🔲 Pendiente — completar tras aplicar las remediaciones del plan anterior.
> Fecha estimada de revisión: una vez implementados C1, C2, C3, A1, A2, A3, A4.

| Área | Puntuación PRE | Resultado POST | Notas |
|------|:--------------:|:--------------:|-------|
| 1. Credenciales y secretos | 4/10 | — | |
| 2. TLS / Comunicaciones | 5/10 | — | |
| 3. Autenticación de dispositivo | 6/10 | — | |
| 4. Control de acceso MQTT | 5/10 | — | |
| 5. Seguridad OTA | 2/10 | — | |
| 6. Secure Boot / Flash Encryption | 0/10 | — | |
| 7. Validación de inputs | 7/10 | — | |
| 8. Watchdog y resiliencia | 8/10 | — | |
| 9. Gestión de memoria | 7/10 | — | |
| 10. Logging e información | 6/10 | — | |
| 11. Seguridad física / hardware | 2/10 | — | |
| 12. Dependencias y librerías | 6/10 | — | |
| **Global** | **4.8/10** | **—** | |

### Checklist de cierre por remediación

| ID | Descripción | Estado PRE | Estado POST | Verificado |
|----|-------------|:----------:|:-----------:|:----------:|
| C1 | Secretos eliminados del historial git + credenciales rotadas | ❌ | — | |
| C2 | OTA con autenticación + verificación de integridad | ❌ | — | |
| C3 | TLS con validación CA en path celular (authmode=1) | ❌ | — | |
| A1 | Flash Encryption + Secure Boot habilitados en fábrica | ❌ | — | |
| A2 | Contraseña SoftAP única por dispositivo | ⚠️ | — | |
| A3 | TLS forzado en producción (static_assert port == 8883) | ⚠️ | — | |
| A4 | StaticJsonDocument<512> + rechazo de payloads sin MAC | ⚠️ | — | |
| M1 | JTAG deshabilitado + Serial deshabilitado en PROD | ❌ | — | |
| M2 | Rotación de tokens implementada | ⚠️ | — | |
| M3 | Topic de comandos por dispositivo (no por finca) | ⚠️ | — | |
| M4 | Versiones de librerías fijadas | ⚠️ | — | |
