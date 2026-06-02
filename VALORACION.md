# Aquantia — Análisis de horas y valoración económica del proyecto

> Basado exclusivamente en los archivos README, CHANGELOG, BOM y documentación de los repositorios `app_meteo` y `weather-station-ESP`. Fecha del análisis: 1 junio 2026.

---

## Contexto del proyecto

**Aquantia** es un sistema IoT completo para monitorización meteorológica, control de riego y detección de fugas en instalaciones domésticas/agrícolas. Está compuesto por:

- **Firmware ESP32** (C++/Arduino, FreeRTOS dual-core) — 4 perfiles de hardware, 12+ sensores, MQTT/TLS, provisioning SoftAP, pantalla TFT, detector de fugas con aprendizaje EMA
- **Backend** (Python/Flask + TimescaleDB + Mosquitto) — 30+ endpoints REST, MQTT broker con autenticación delegada, sistema multi-dispositivo, alertas, JWT, email de verificación
- **Frontend** (React 18 + Vite + Tailwind + ECharts) — 10+ vistas, polling incremental en tiempo real, escáner QR, gráficas animadas
- **Hardware** — Estación METEO completa, placa IRRIGATION 4-relay, sistema AquaLeak con PCB custom, sensor de suelo Helissense RS485

El proyecto abarca desde el primer commit operativo (~25 marzo 2026) hasta la versión `v0.2.0` (15 mayo 2026) con trabajo activo en ramas `feat/helissense-sensor` y `feature/aqualeak-profile`.

---

## 1. Estimación de horas de trabajo

### 1.1 Firmware ESP32

| Área | Horas |
|------|------:|
| Arquitectura FreeRTOS dual-core + sincronización lock-free (queues, mutex, spinlocks) | 20 |
| MQTT/TLS + certificado CA en PROGMEM + autenticación DEV/PROD | 20 |
| Provisioning SoftAP (captive portal, NVS, factory reset) | 15 |
| Sistema de perfiles en tiempo de compilación (METEO / IRRIGATION / AGROMETEO / AQUA_SMART_REMOTE) | 12 |
| Drivers de sensores I2C desde cero (HTU2x, XDB401 desde datasheet, AHT20, INA219, HDC1080) | 30 |
| Driver RS485 Modbus RTU Helissense (protocolo completo + CRC-16) | 15 |
| Clase LeakDetector con EMA + warm-up + 4 estados de detección | 20 |
| Pantalla TFT 4 vistas + doble buffer (sin parpadeo) + botones con debounce | 18 |
| Flash Tool GUI Tkinter (flash USB, OTA, DEV/PROD, Factory Provision, mDNS, QR, NVS borrar) | 30 |
| Anemómetro (media móvil) + veleta (promedio vectorial) | 8 |
| Reconexión WiFi robustecida (backoff exponencial, reset stack, watchdog) | 10 |
| Perfil AQUA_SMART_REMOTE con SIM7000G/GSM (TLS, PDP, contexto SSL, SNI) | 25 |
| Simulación de sensores + fallback determinista | 8 |
| Sistema OTA seguro (apaga relays, progreso real) | 8 |
| Debug mode + WDT heartbeat + diagnóstico en RTC RAM | 10 |
| Alertas MQTT edge-triggered + cooldown 12h | 8 |
| Corrección bugs y estabilización (race conditions, GPIO16 TFT_DC, BMP280 ruido, relays activo-HIGH) | 25 |
| **Subtotal firmware** | **282 h** |

### 1.2 Backend (Flask + TimescaleDB + MQTT)

| Área | Horas |
|------|------:|
| Setup inicial HTTP legacy + SQLite (pre-MQTT) | 15 |
| Migración a TimescaleDB + Docker Compose + deploy.sh | 15 |
| Integración MQTT (Mosquitto go-auth + webhook autenticación) | 20 |
| API REST: 30+ endpoints (auth, sensores, dispositivos, relays, riego, alertas, pipeline) | 35 |
| Sistema multi-dispositivo (filtrado por MAC, selector ECU) | 12 |
| JWT + verificación de email + roles (user/admin) + eliminación de cuenta | 15 |
| Device provisioning + Flash Tool integration (register_factory, claim, QR flow) | 15 |
| Simulador pipeline_sim.py + 4 algoritmos detección fugas (umbral, dP/dt, EWMA) | 25 |
| Estadísticas de riego: sesiones, historial, ahorro vs. baseline, litros por tipo | 15 |
| Sistema de alertas (badge, acknowledge, filtros, cooldown anti-spam reconexión) | 10 |
| Auditoría de seguridad + hardening (IDOR, SQL injection, MQTT auth, rate limiting) | 20 |
| Suite de tests backend — 285 tests (pytest, 12 módulos) | 30 |
| migrate_sqlite_to_pg.py + compatibilidad SQLite→PostgreSQL | 8 |
| Script monitor.sh (tmux 3 paneles) | 4 |
| **Subtotal backend** | **239 h** |

### 1.3 Frontend (React + Vite + ECharts)

| Área | Horas |
|------|------:|
| App.jsx: layout, routing, guard JWT, sidebar responsive móvil | 15 |
| useWeatherData hook: polling incremental 15s, append sin re-render innecesario | 12 |
| Vista Meteorología: gráficos ECharts (área, línea, scatter), filtros de fecha, presets | 20 |
| Vista Pipeline: gauges animados ECharts, selectores escenario/modo/irrigación, estados detección | 18 |
| Vista Riego: control relays, temporizador cierre automático, animación gota, KPI strip, consumo sesión | 25 |
| Vista Plantación: NPK, pH, EC, temperatura radicular (perfil AGROMETEO) | 10 |
| Vista Alertas: badge contador, severidad, acknowledge, filtros pendientes/todas | 12 |
| Vista Dispositivos + ClaimDeviceView (QR con cámara iOS Safari + entrada manual serial) | 15 |
| Vista Estado del Dispositivo: RSSI, heap, uptime, chip info, batería animada | 10 |
| SettingsView: configuración + zona de peligro eliminación cuenta | 8 |
| LoginView rediseñada (panel hero) + AuthContext JWT completo | 12 |
| Sidebar responsive: overlay móvil, filtros fecha, selector ECU, logout confirm modal | 12 |
| StatCard con ECharts + WeatherChart memoizado (evita re-renders forzados) | 10 |
| Diseño futurista-light: sistema de tokens, animaciones CSS, componentes Gadget/FaucetStage | 15 |
| Suite de tests frontend — 141 tests (Vitest + React Testing Library) | 20 |
| **Subtotal frontend** | **214 h** |

### 1.4 Hardware, soldadura y ensamblaje de prototipos

| Área | Horas |
|------|------:|
| Investigación de BOMs + comparativa de sensores/fabricantes + búsquedas de precio | 10 |
| Gestión de compras (AliExpress, Amazon, DigiKey, BricoGeek) + seguimiento pedidos | 8 |
| Diseño y documentación pinout (4 perfiles × GPIO mapping completo) | 6 |
| Diseño PCB custom AquaLeak (ESP32-C3 + CH340C + AMS1117 + conectores JST) | 15 |
| Soldadura + ensamblaje prototipo METEO (TTGO + 7 sensores + RS485 + anemómetro/veleta + caja) | 5 |
| Soldadura + ensamblaje prototipo IRRIGATION (placa 4-relay + AHT20 + INA219) | 3 |
| Soldadura + ensamblaje prototipo AquaLeak (PCB SMT + YF-B9 + válvula + sensor presión + caja IP54) | 5 |
| Testing hardware: calibración sensores, verificación I2C, debug GPIO16 RS485, ajuste ADC YL-69 | 12 |
| **Subtotal hardware** | **64 h** |

### 1.5 Infraestructura, documentación y gestión

| Área | Horas |
|------|------:|
| README principal (878 líneas), ARQUITECTURA.md, PINOUT.md, PROGRAM_FLOW.md, BOMs (2) | 20 |
| CHANGELOG coordinado firmware+backend (versionado semántico, compatibilidad cruzada) | 8 |
| Configuración VPS: HestiaCP, Nginx, Let's Encrypt, Docker en producción | 8 |
| Diseño y documentación del sistema de animaciones (design_handoff) | 5 |
| **Subtotal docs/infra** | **41 h** |

---

### TOTAL DE HORAS

| Área | Horas |
|------|------:|
| Firmware ESP32 | 282 |
| Backend Flask + TimescaleDB | 239 |
| Frontend React + ECharts | 214 |
| Hardware + soldadura + prototipos | 64 |
| Documentación + infra | 41 |
| **TOTAL** | **840 h** |

> Rango realista: **750–950 horas** (según velocidad individual y curvas de aprendizaje en componentes nuevos como SIM7000G, XDB401 desde datasheet, y EMA para detección de fugas).

---

## 2. Coste de los materiales hardware (3 prototipos)

### Prototipo 1 — Estación METEO completa

| Componente | Precio estimado |
|-----------|---------------:|
| LilyGo TTGO T-Display (ESP32) | 13 € |
| MCP9808 (módulo Adafruit) | 7 € |
| HTU21D (módulo) | 4 € |
| SparkFun MicroPressure | 12 € |
| TSL2584/APDS-9930 (clon) | 3 € |
| DHT11 (módulo) | 1.5 € |
| Anemómetro analógico | 12 € |
| Veleta analógica | 10 € |
| Sensor suelo Helissense RS485 7-en-1 | 20 € |
| YL-69 humedad suelo (ADC fallback) | 2 € |
| Adaptador RS485 (MAX485) | 3 € |
| XDB401 sensor presión tubería I2C | 15 € |
| YF-B9 caudalímetro G3/4" | 6 € |
| Cables Dupont, PCB proto, resistencias | 5 € |
| Caja estanca IP54 + prensaestopas | 7 € |
| **Subtotal METEO** | **~121 €** |

### Prototipo 2 — Placa de riego IRRIGATION

| Componente | Precio estimado |
|-----------|---------------:|
| ESP32 4-Relay Board (JQC-3FF-S-Z activo-HIGH) | 14 € |
| AHT20 (módulo I2C) | 2 € |
| INA219 (módulo I2C voltaje/corriente) | 3 € |
| BMP280 (módulo I2C fallback) | 2 € |
| Cables + caja | 8 € |
| **Subtotal IRRIGATION** | **~29 €** |

### Prototipo 3 — Sistema AquaLeak

| Componente | Precio estimado |
|-----------|---------------:|
| ESP32-C3-MINI-1-N4 (LCSC) | 3 € |
| YF-B9 caudalímetro G3/4" | 6 € |
| XGZP6847A sensor presión 0–7 bar | 3.5 € |
| Válvula bola motorizada DN20 3/4" 12V | 9 € |
| NTC 10kΩ sonda impermeable IP68 | 2 € |
| CH340C + AMS1117 + pasivos + conectores JST | 5 € |
| PCB custom 2 capas (prototipo unitario JLCPCB) | 25 € |
| Fuente 5V/1A + cable USB-C | 5 € |
| Caja ABS IP54 70×50×25mm | 8 € |
| **Subtotal AquaLeak** | **~66.5 €** |

### Resumen hardware 3 prototipos

| Concepto | Coste |
|----------|------:|
| Componentes 3 prototipos | ~217 € |
| Envíos + IVA (pedidos >150€) + aduanas (+21–25%) | ~54 € |
| Margen reposición (componentes muertos, errores) +15% | ~33 € |
| **Total materiales 3 prototipos** | **~304 €** |

> No incluye: herramientas (soldador, multímetro, osciloscopio, fuente de laboratorio). Si hay que equiparse desde cero: añadir ~150–300€.

---

## 3. Coste de infraestructura

| Concepto | Coste estimado |
|---------|---------------:|
| VPS producción (~6 meses desarrollo activo) | ~75 € |
| Dominio `aquantialab.com` (1 año) | ~12 € |
| Let's Encrypt (gratuito) | 0 € |
| **Total infraestructura** | **~87 €** |

---

## 4. Valoración económica del trabajo de desarrollo

### Tarifa de referencia

El proyecto cruza varias especialidades de alta demanda:
- **Embedded firmware** (ESP32 dual-core, custom drivers desde datasheet, RS485 Modbus)
- **IoT backend** (MQTT/TLS, TimescaleDB, autenticación webhook)
- **Full-stack web** (React 18, ECharts, JWT, tests automatizados)
- **Hardware** (PCB design, BOM engineering, soldering)

Tarifas de mercado europeo 2026 para perfiles similares:

| Perfil | Tarifa/hora |
|--------|----------:|
| Desarrollador embedded ESP32 (senior) | 55–75 €/h |
| Backend IoT Python (mid-senior) | 45–65 €/h |
| Frontend React (mid-senior) | 40–60 €/h |
| Ingeniero hardware + PCB | 50–70 €/h |
| Tarifa media ponderada (todo integrado) | **~55 €/h** |

### Coste total del trabajo

| Escenario | Horas | Tarifa | Coste labor |
|-----------|------:|-------:|------------:|
| Conservador | 750 h | 50 €/h | 37.500 € |
| **Realista** | **840 h** | **55 €/h** | **46.200 €** |
| Ampliado | 950 h | 60 €/h | 57.000 € |

---

## 5. Presupuesto total del proyecto

| Concepto | Coste |
|---------|------:|
| Desarrollo (software + firmware) — 840 h × 55 €/h | 46.200 € |
| Hardware 3 prototipos (componentes + envíos) | 304 € |
| Infraestructura (VPS + dominio, 6 meses) | 87 € |
| Herramientas (soldador, multímetro, accesorios) | 200 € |
| **TOTAL PROYECTO** | **~46.791 €** |

> Redondeado: **entre 42.000 € y 57.000 €** según la tarifa horaria aplicada y la velocidad de ejecución.

---

## 6. Qué justifica esa valoración

El precio no es solo "líneas de código". Las funcionalidades que más elevan el valor son:

1. **Driver XDB401 desde datasheet** — El trigger I2C estaba invertido en la primera versión; reescribir el protocolo correcto (XGZP6847D) requiere conocimiento de bajo nivel difícil de delegar.
2. **LeakDetector con EMA aprendido** — 4 algoritmos de detección, confirmación multi-muestra, warm-up, 4 perfiles de irrigación con baseline independiente. Lógica de control real, no un tutorial.
3. **Driver RS485 Modbus RTU Helissense sin librería** — CRC-16, half-duplex, muestreo adaptativo (3s durante riego / 20s en reposo), bug de GPIO16 con TFT_DC resuelto.
4. **Arquitectura dual-core lock-free** — Queue FreeRTOS para telemetría, mutex solo para config, spinlock para contadores ISR de caudalímetro. Correcto bajo concurrent access; la versión naive con String causaba race conditions.
5. **Autenticación MQTT por bcrypt/MAC** — Cada dispositivo se autentica con su MAC y un token hasheado en fábrica. El broker lo delega a Flask por webhook. Sistema robusto, no solo credenciales hardcodeadas.
6. **Flash Tool GUI completa** — Tkinter con compilación incremental, OTA, mDNS, Factory Provision, QR, borrado de NVS leyendo tabla de particiones real del chip.
7. **426 tests automatizados** (285 backend + 141 frontend) — Cobertura de IDOR, SQL injection, MQTT auth/ACL, algoritmos EWMA fijados como golden regression tests.
8. **Perfil AQUA_SMART_REMOTE SIM7000G** — MQTT sobre GSM/4G con TLS; SNI manual, alternancia de contexto SSL, recuperación PDP, polling AT con cadencia controlada. Difícil de encontrar documentado.

---

*Análisis elaborado a partir de README.md (878 líneas), ARQUITECTURA.md, CHANGELOG.md backend+firmware, BOM_HARDWARE.md, BOM_PRODUCCION.md, PINOUT.md (409 líneas), PROGRAM_FLOW.md (648 líneas), y firmware README (971 líneas). No se accedió al código fuente.*
