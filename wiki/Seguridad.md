# Seguridad del Firmware Aquantia

> Para el informe de auditoría completo con hallazgos, plan de remediación y checklist ENS Básico, ver [SECURITY.md](../SECURITY.md).

---

## Modelo de amenazas

El sistema Aquantia opera en entorno exterior no vigilado (IP68) conectado a infraestructura de agua. Las superficies de ataque principales son:

| Vector | Descripción | Perfil afectado |
|--------|-------------|-----------------|
| **Red WiFi local** | Acceso a la misma red que el dispositivo | METEO, IRRIGATION, AQUALEAK |
| **Red celular (LTE)** | MITM en red 1NCE/Onomondo | AQUA_SMART_REMOTE |
| **Acceso físico** | Conexión UART/JTAG, volcado de flash | Todos |
| **Broker MQTT** | Suscripción a topics de otros dispositivos | Todos |
| **OTA por red** | Firmware malicioso vía ArduinoOTA | METEO, IRRIGATION, AQUALEAK |

---

## Identidad del dispositivo

Cada dispositivo tiene una identidad única derivada de hardware que no puede ser modificada sin acceso a la fábrica:

```
eFuse (hardware) → MAC WIFI_STA
       ↓
device_serial_get() en provisioning.h
       ↓
"AQ-{12 hex chars}"   ej: AQ-FCB467F37748
```

En **modo producción** (`!DEV_MODE`), este serial se usa como `mqtt_user`. El `mqtt_pass` es un token de 43 caracteres URL-safe generado en fábrica, hasheado con bcrypt (coste 12) en el backend, y almacenado en NVS del dispositivo.

```
factory_provision.py
  secrets.token_urlsafe(32) → token en claro → grabado en NVS del ESP32
  bcrypt.hashpw(token, gensalt(12)) → hash → almacenado en base de datos
```

En **modo desarrollo** (`DEV_MODE` en `secrets.h`), todos los dispositivos comparten las credenciales del archivo de configuración local. Nunca usar en producción.

---

## Comunicaciones TLS

### Path WiFi (perfiles METEO, IRRIGATION, AQUALEAK)

El firmware usa `WiFiClientSecure` (BearSSL, incluido en el core Arduino ESP32) para MQTT sobre TLS 1.2 en el puerto 8883.

```
ESP32 → WiFiClientSecure → TLS 1.2 → Broker Mosquitto (puerto 8883)
```

**Certificado CA:** Let's Encrypt R13 (intermedia) + ISRG Root X1 (raíz), embebidos en `mqtt_cert.h`. Válidos hasta 2027. La verificación de CA es obligatoria (`setCACert(MQTT_CA_CERT_PEM)`). No se usa `setInsecure()` en ningún path WiFi.

Para más detalle sobre la implementación TLS, ver [TLS.md](TLS.md).

### Path celular (perfil AQUA_SMART_REMOTE / SIM7000G)

El módem SIM7000G gestiona TLS internamente mediante comandos AT (`+CSSLCFG`). El certificado CA se carga al módem antes de la conexión via `sim7000g_uploadCACert()` en `gsm_modem.h`.

> **Nota de seguridad:** La validación de CA en el path celular (`authmode`) debe estar configurada en valor `1` para verificar el servidor. Ver `SECURITY.md` ítem C3.

### Clock y validez de certificados

Antes de cualquier handshake TLS, el firmware sincroniza NTP:
- `ESP_monitor_server.ino`: sincronización NTP en `setup()` antes de lanzar `networkTask`
- `tlsClockReady()`: verifica que el epoch sea > 1700000000 antes de iniciar el handshake
- Retry cada 60s si NTP falla

---

## OTA (Over-The-Air Updates)

El firmware usa **ArduinoOTA** sobre UDP (puerto 3232) en la red WiFi local.

**Características de seguridad actuales:**
- Contraseña OTA: configurable vía `#define OTA_PASSWORD` en `secrets.h` (pendiente de activar — ver `SECURITY.md` ítem C2)
- Fail-safe durante actualización: todos los relays se apagan (`LOW`) antes de flashear
- Rollback automático: el bootloader ESP32 revierte a la partición anterior si el firmware nuevo no arranca

**Particiones OTA** (`partitions.csv`):
```
app0     (0x180000) — partición activa
app1     (0x180000) — partición OTA
otadata  (0x2000)   — metadatos de arranque (gestiona rollback)
coredump (0x10000)  — volcado en caso de crash (diagnóstico)
```

---

## Portal de provisioning (SoftAP)

En el primer arranque (o tras factory reset), el dispositivo levanta un punto de acceso WiFi para recibir las credenciales de red y el token MQTT.

```
Arranque sin NVS → SoftAP "Aquantia-{MAC6}"
       ↓
Usuario conecta su móvil al AP
       ↓
Portal web (WebServer + DNSServer en 192.168.4.1)
       ↓
Formulario: SSID + Password WiFi + Token MQTT
       ↓
Guardado en NVS (Preferences, namespace "aquantia")
       ↓
ESP.restart() → arranque normal con credenciales
```

**Factory reset:** mantener pulsado GPIO0 (botón BOOT) durante 3 segundos en el arranque borra el NVS y reinicia el portal.

> **Nota de seguridad:** La contraseña del AP (`"aquantia1"`) es actualmente la misma en todos los dispositivos. Ver `SECURITY.md` ítem A2 para la remediación planificada.

---

## Secure Boot y Flash Encryption

Estas protecciones se configuran a nivel de hardware (eFuse) y sdkconfig, y son permanentes una vez activadas.

| Protección | Descripción | Estado actual |
|---|---|:---:|
| **Flash Encryption** | Cifra todo el contenido de la flash (código, NVS, filesystem). Solo el chip que generó la clave puede leer la flash. | ❌ Deshabilitado |
| **Secure Boot V2** | El bootloader verifica la firma criptográfica del firmware antes de arrancar. Firmware no firmado no arranca. | ❌ Deshabilitado |
| **JTAG disable** | Deshabilita permanentemente el acceso de depuración por JTAG. | ❌ Habilitado |

La habilitación de estas protecciones está prevista en el proceso de provisioning de fábrica (`factory_provision.py`). Ver `SECURITY.md` ítem A1 para el plan de implementación.

---

## Cumplimiento ENS Básico

El sistema Aquantia está orientado a un piloto con administración pública española. El Esquema Nacional de Seguridad (ENS, RD 311/2022) en categoría **Básica** requiere, entre otros:

| Medida | Descripción | Estado |
|---|---|:---:|
| `[mp.com.1]` | TLS en todas las conexiones | ⚠️ |
| `[mp.si.2]` | TLS 1.2+ con CA verificada | ✅ (WiFi) |
| `[op.exp.4]` | OTA autenticado | ❌ |
| `[mp.if.3]` | Cifrado en reposo (Flash Encryption) | ❌ |
| `[mp.per.3]` | Sin secretos en repositorio | ❌ |

Ver [SECURITY.md](../SECURITY.md) para la tabla completa y el checklist de pre-entrega.

---

## Referencias

- [SECURITY.md](../SECURITY.md) — Informe de auditoría completo, plan de remediación, checklist ENS
- [TLS.md](TLS.md) — Implementación de certificados MQTT por perfil
- [`provisioning.h`](../ESP_monitor_server/provisioning.h) — SoftAP y almacenamiento NVS
- [`gsm_modem.h`](../ESP_monitor_server/gsm_modem.h) — TLS en SIM7000G
- [`mqtt_cert.h`](../ESP_monitor_server/mqtt_cert.h) — Bundle de CA Let's Encrypt
- [`factory_provision.py`](../tools/factory_provision.py) — Generación y registro de tokens de fábrica
- [ESP32 Security Features](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/security/index.html) — Documentación oficial Espressif
- [ENS — RD 311/2022](https://www.boe.es/buscar/act.php?id=BOE-A-2022-7191) — Marco normativo español
