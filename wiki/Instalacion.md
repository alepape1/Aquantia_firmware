# Instalación

Guía para compilar y flashear el firmware Aquantia en ESP32 con **arduino-cli** (recomendado)
o con el **Flash Tool GUI** (`flasher_gui.py`).

Consulta [FAQ](FAQ) si tienes problemas.

---

## Requisitos

- **arduino-cli** ≥ 0.35 — [instalación oficial](https://arduino.github.io/arduino-cli/latest/installation/)
- **Python 3.10+** — para `flasher_gui.py` y scripts de tools/
- Core ESP32: `esp32:esp32` (Espressif)
- Librería **SSLClient** (OPEnSLab-OSU) — solo para `PROFILE_AQUA_SMART_REMOTE`

### Instalar core ESP32

```powershell
arduino-cli core update-index
arduino-cli core install esp32:esp32
```

### Instalar librerías necesarias

```powershell
arduino-cli lib install "Adafruit BMP280 Library"
arduino-cli lib install "Adafruit DHT sensor library"
arduino-cli lib install "SparkFun MicroPressure Library"
arduino-cli lib install "SSLClient"          # solo AQUA_SMART_REMOTE
arduino-cli lib install "TinyGSM"            # solo AQUA_SMART_REMOTE
arduino-cli lib install "TFT_eSPI"           # solo PROFILE_METEO
```

---

## Compilar por perfil

El perfil se pasa como flag de compilación `-DDEVICE_PROFILE=N`. No hay que editar el código.

### PROFILE_METEO (1) — LilyGo TTGO T-Display

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32 `
  --build-path ./build --build-cache-path ./build-cache `
  --build-property "compiler.cpp.extra_flags=-DDEVICE_PROFILE=1 -DDEBUG_MODE=1" `
  ESP_monitor_server/
```

### PROFILE_IRRIGATION (2) — ESP32 4-Relay Board

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32 `
  --build-path ./build --build-cache-path ./build-cache `
  --build-property "compiler.cpp.extra_flags=-DDEVICE_PROFILE=2 -DDEBUG_MODE=1" `
  ESP_monitor_server/
```

### PROFILE_AQUALEAK (3) — Wemos D1 Mini ESP32

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32 `
  --build-path ./build --build-cache-path ./build-cache `
  --build-property "compiler.cpp.extra_flags=-DDEVICE_PROFILE=3 -DDEBUG_MODE=1" `
  ESP_monitor_server/
```

### PROFILE_AQUA_SMART_REMOTE (4) — LilyGO T-SIM7000G

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32 `
  --build-path ./build --build-cache-path ./build-cache `
  --build-property "compiler.cpp.extra_flags=-DDEVICE_PROFILE=4 -DDEBUG_MODE=1 -DUSE_MQTT" `
  ESP_monitor_server/
```

---

## Flashear

```powershell
arduino-cli upload -p COM11 --fqbn esp32:esp32:esp32 --input-dir ./build
```

Sustituye `COM11` por el puerto serie del dispositivo (`arduino-cli board list` para verlos).

---

## Flash Tool GUI (recomendado para producción)

La herramienta `tools/flasher_gui.py` gestiona compilación, flash, provisioning NVS y OTA
desde una interfaz gráfica. Permite seleccionar el perfil, la finca, el puerto y ejecutar
compilaciones incrementales.

```powershell
python tools/flasher_gui.py
```

Para provisionar un dispositivo nuevo desde cero (registro + NVS):

```powershell
python tools/factory_provision.py
```

---

## Credenciales dev — `secrets.h`

En modo `DEV_MODE`, el firmware lee host/puerto/token desde `secrets.h`. Este archivo
**nunca se commitea** (está en `.gitignore`). Copia la plantilla y rellena tus valores:

```powershell
Copy-Item ESP_monitor_server/secrets.h.example ESP_monitor_server/secrets.h
```

En producción, las credenciales se almacenan en NVS vía `factory_provision.py`.

---

## Monitor serie

```powershell
arduino-cli monitor -p COM11 --config baudrate=115200
```

---

## Primer arranque sin `DEV_MODE`

Si el dispositivo no tiene credenciales en NVS, arranca en modo **SoftAP provisioning**:

1. Conectarse a la red WiFi `Aquantia-Setup` (contraseña: `aquantia123`).
2. Abrir `http://192.168.4.1` en el navegador.
3. Introducir SSID, contraseña WiFi, host/puerto MQTT y token.
4. El dispositivo guarda los valores en NVS y reinicia.
