# TLS / Certificados MQTT

El broker MQTT (`meteo.aquantialab.com:8883`) requiere TLS con verificación de CA.
El firmware implementa TLS de dos formas distintas según el perfil de hardware.

---

## Arquitectura por perfil

### Perfil WiFi — `WiFiClientSecure` (mbedTLS)

```
broker :8883
    ↑ TLS 1.2 (mbedTLS, stack WiFi del ESP32)
WiFiClientSecure  ←── mqtt_cert.h  (setCACert)
    ↑ TCP / WiFi
```

- El archivo [`mqtt_cert.h`](../ESP_monitor_server/mqtt_cert.h) contiene el bundle PEM con dos certificados: el intermediario **R13** y la raíz **ISRG Root X1**.
- `setCACert(MQTT_CA_CERT_PEM)` se llama antes de cada intento de conexión.
- La verificación del CN del broker es automática — no se usa `setInsecure()`.

### Perfil SIM — `SSLClient` / BearSSL (AQUA_SMART_REMOTE)

```
broker :8883
    ↑ TLS 1.2/1.3 (BearSSL corriendo en el ESP32)
SSLClient  ←── trust_anchors.h  (struct br_x509_trust_anchor)
    ↑ TCP plano (puerto 8883)
TinyGsmClient (mux 1)
    ↑ AT+CIPOPEN / GPRS
SIM7000G  →  SIM Onomondo  →  LTE-M / 2G
```

- El SIM7000G R1529 **no soporta** `AT+CSSLCFG="authmode"` ni `AT+CSSLCFG="cacert"` — el stack SSL hardware del modem está descartado.
- `SSLClient` ([OPEnSLab-OSU/SSLClient](https://github.com/OPEnSLab-OSU/SSLClient)) hace el handshake BearSSL completamente en el ESP32.
- El archivo [`trust_anchors.h`](../ESP_monitor_server/trust_anchors.h) contiene el trust anchor en formato BearSSL (arrays C con DN DER, módulo RSA y exponente).
- SNI configurado con `gsmTLSClient.setHostname(mqtt_server)`.
- Timeout de handshake: **75 segundos** (LTE-M/2G puede tardar 20–40 s).
- Pin `A0` (flotante) como semilla de entropía para BearSSL.

---

## Archivos de certificado

| Archivo | Ruta | Perfil | Descripción |
|---------|------|--------|-------------|
| `mqtt_cert.h` | `ESP_monitor_server/` | WiFi | Bundle PEM: R13 + ISRG Root X1 |
| `trust_anchors.h` | `ESP_monitor_server/` | SIM | Trust anchor BearSSL: solo ISRG Root X1 |
| `isrg_root_x1_official.pem` | `tools/` | — | Fuente oficial del cert raíz |
| `r13_official.pem` | `tools/` | — | R13 extraído del broker en vivo |
| `gen_trust_anchor.py` | `tools/` | — | Script para regenerar `trust_anchors.h` |

---

## Raíz de confianza — ISRG Root X1

| Campo | Valor |
|-------|-------|
| Subject | `CN=ISRG Root X1, O=Internet Security Research Group, C=US` |
| Fingerprint SHA-256 | `96:BC:EC:06:26:49:76:F3:74:60:77:9A:CF:28:C5:A7:CF:E8:A3:C0:AA:E1:1A:8F:FC:EE:05:C0:BD:DF:08:C6` |
| Expira | 2035-06-04 |
| Fuente oficial | https://letsencrypt.org/certs/isrgrootx1.pem |

La raíz **no rota con los certs del servidor**. Normalmente no necesita actualizarse hasta 2035 salvo que Let's Encrypt cambie de raíz.

---

## Cuándo regenerar los archivos de certificado

| Situación | Acción necesaria |
|-----------|-----------------|
| Renovación normal del cert del servidor (cada 90 días) | **Nada** — la raíz no cambia |
| Let's Encrypt cambia de raíz (ej. abandona ISRG Root X1) | Regenerar ambos archivos (ver pasos abajo) |
| Cambio de broker / dominio | Verificar que el nuevo cert esté firmado por ISRG Root X1; si usa otra CA, sustituir la raíz |
| El handshake TLS falla con error de verificación | Comprobar fingerprint (ver diagnóstico abajo) |

---

## Regenerar `trust_anchors.h` (perfil SIM)

```bash
# 1. Descargar ISRG Root X1 oficial
curl -o tools/isrg_root_x1_official.pem https://letsencrypt.org/certs/isrgrootx1.pem

# 2. Verificar fingerprint (debe coincidir con el de la tabla anterior)
openssl x509 -in tools/isrg_root_x1_official.pem -noout -fingerprint -sha256

# 3. Generar trust_anchors.h
python tools/gen_trust_anchor.py tools/isrg_root_x1_official.pem \
       ESP_monitor_server/trust_anchors.h
```

En Windows (PowerShell):
```powershell
python tools/gen_trust_anchor.py tools/isrg_root_x1_official.pem `
       ESP_monitor_server/trust_anchors.h
```

> **Dependencia Python:** `pip install cryptography`

---

## Regenerar `mqtt_cert.h` (perfil WiFi)

```bash
# Extraer la cadena completa del broker en vivo
openssl s_client -connect meteo.aquantialab.com:8883 -showcerts 2>/dev/null \
  | awk '/BEGIN CERT/,/END CERT/' > tools/chain_live.pem

# Verificar que el último cert de la cadena sea ISRG Root X1
openssl x509 -in tools/isrg_root_x1_official.pem -noout -fingerprint -sha256
```

Luego editar `mqtt_cert.h` manteniendo el formato:
```
MQTT_CA_CERT_PEM = R"CERT(
-----BEGIN CERTIFICATE-----
<R13 — intermediario>
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
<ISRG Root X1 — raíz>
-----END CERTIFICATE-----
)CERT";
```

---

## Diagnóstico — handshake TLS falla en SIM

Si el log muestra `state=-4` (timeout) después de `[MQTT] Intentando conectar`:

```
# 1. Verificar que el broker acepta conexiones TLS
openssl s_client -connect meteo.aquantialab.com:8883

# 2. Comprobar la cadena del broker
openssl s_client -connect meteo.aquantialab.com:8883 -showcerts 2>/dev/null \
  | openssl x509 -noout -fingerprint -sha256

# 3. Verificar que trust_anchors.h tiene el fingerprint correcto
#    El módulo RSA en TA0_RSA_N debe coincidir con la clave pública del cert
openssl x509 -in tools/isrg_root_x1_official.pem -noout -text | grep -A2 "Public-Key"
```

Si el handshake llega a establecerse pero MQTT sigue fallando, el problema es de credenciales o ACL del broker, no de TLS.

---

## Instalar SSLClient (Arduino Library Manager)

El perfil SIM requiere la librería **SSLClient de OPEnSLab-OSU**:

1. Arduino IDE → **Herramientas → Administrar bibliotecas**
2. Buscar: `SSLClient`
3. Autor: **OPEnSLab-OSU**
4. Instalar (versión ≥ 1.6.11)
