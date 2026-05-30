# AquaLeak — Hardware BOM (Bill of Materials)

Documento para estimación de costes del sistema AquaLeak: detección de fugas,
monitorización de consumo y corte automático para fontanería doméstica y red pública.

Actualizado con precios de fabricante verificados en mayo 2026.

---

## Descripción del proyecto

Sistema IoT de monitorización hídrica basado en ESP32-C3-MINI-1, alimentación 5V USB-C,
comunicación WiFi. Incluye caudalímetro efecto Hall, sensor de presión, válvula de corte
motorizada y detección de anomalías por software (caudal/presión).

---

## Lista de componentes

### Microcontrolador

| # | Componente | Descripción técnica | Cant. |
|---|-----------|---------------------|:-----:|
| 1 | ESP32-C3-MINI-1-N4 (Espressif) | RISC-V 160 MHz, 4 MB Flash, WiFi 802.11 b/g/n, BLE 5. 22 GPIOs, I2C, SPI, ADC, UART. OTA nativo. 13.2×16.6 mm castellated. LCSC C2838502. | 1 |

---

### Sensores

| # | Componente | Descripción técnica | Cant. |
|---|-----------|---------------------|:-----:|
| 2 | YF-B9 caudalímetro efecto Hall G3/4" | Latón, rosca BSP G3/4" hembra, 2–50 L/min, ±5%, 476 pulsos/litro, DC 3.5–24 V, salida NPN open collector. Interrupción GPIO. Fabricante: Zhongjiang / Tonhe (Guangdong). | 1 |
| 3 | XGZP6847A 0–7 bar (⭐ opción económica) | CFSensor. MEMS piezoresistivo. Salida analógica 0.5–4.5 V ratiométrica. Rango 0–700 kPa (7 bar). Rosca G1/8". Requiere divisor resistivo (10 kΩ + 20 kΩ) si alimentado a 5 V. Fabricante directo: CFSensor (disponible en DigiKey ref. XGZP6847A700KPG). | 1 |
| 4 | XDB401 I2C 0–1.6 MPa (opción alternativa) | XIDIBEI/xdbsensor.com. Core cerámica piezoresistiva. I2C (dirección 0x6D — verificar por lotes). Alimentación 3.3–5 V. Rosca G1/4" SS316L. IP65. **Más caro** (~13–16 € a 100u). Útil si se requiere I2C limpio y mayor resolución. ⚠ Verificar dirección I2C con i2cscanner antes de comprometer MOQ: algunos lotes responden a 0x7F (reservado). | 1 |
| 5 | NTC 10 kΩ B3950 sonda impermeable IP68 | Sonda sumergible con cable silicona 1 m. Temperatura del agua en tubería. Conectar a ADC ESP32 con divisor resistivo 10 kΩ. | 1 |

> **Recomendación sensor de presión:** Para la versión de producción, usar **XGZP6847A** (analógico, ~2.20–2.80 €/ud a 200–1000u). Es 4–6× más barato que el XDB401 a igual volumen. Para detección de fugas (umbral, no precisión absoluta) la salida analógica es perfectamente suficiente. El XDB401 I2C queda para versiones premium o si la PCB ya no tiene ADC libre.

---

### Válvula de corte

| # | Componente | Descripción técnica | Cant. |
|---|-----------|---------------------|:-----:|
| 6 | Válvula bola motorizada DN20 3/4" 12 VDC (⭐ recomendada) | Latón + PTFE + motor DC. 9–24 VDC. Control 3 hilos CR01 (OPEN / CLOSE / COM). Apertura/cierre ~5 s. 60–80 mA solo durante movimiento. Mantiene posición al cortar corriente (fail-safe para riego). Sin presión mínima de operación. IP65 estándar / IP67 opcional. 50.000–100.000 ciclos. Fabricantes: Tonhe, CWX, Winner (Zhejiang/Ningbo). Control simplificado: relay SPDT desde GPIO, sin H-bridge. | 1 |
| 7 | Válvula latching/bistable 3/4" 6–12 VDC (alternativa) | Latón + NBR/EPDM. Pulso 100–300 ms para cambio de estado. 0 mA en standby (ideal consumo ultra-bajo). Requiere driver H-bridge (DRV8833 o similar) + condensador de reserva 1000–4700 µF para pico de corriente (300–800 mA durante 100–300 ms). Presión mínima operación: ~0.05 bar. Fabricante recomendado: Hubei Chuangsinuo (CSN). | 1 |
| 8 | Driver válvula (según tipo elegido) | Si válvula bola: relay SPDT 12 V / 5 V (o MOSFET + relay). Si válvula latching: DRV8833 dual H-bridge (TI, LCSC C2679699, ~$0.65–0.82 a 100–500u) + condensador electrolítico 4700 µF/16 V. | 1 |

> **Veredicto válvula:** La **bola motorizada 3-wire** gana para producción: más barata en volumen, control más simple (relay SPDT, sin H-bridge), funciona con presión cero, compatible directo con 12 V LiFePO4. La latching es teóricamente más eficiente en consumo, pero el circuito adicional anula la ventaja de coste y complejidad para este caso de uso.

---

### Componentes pasivos y miscelánea

| # | Componente | Descripción técnica | Cant. |
|---|-----------|---------------------|:-----:|
| 9 | Divisor resistivo 10 kΩ + 20 kΩ | Para adaptar señal XGZP6847A (0.5–4.5 V a 5 V) a ADC ESP32 (máx 3.3 V). Obligatorio si sensor alimentado a 5 V. Omitir si se alimenta a 3.3 V. | 1 set |
| 10 | Condensador 100 nF | Anti-rebotes en línea de señal del caudalímetro YF-B9 (entre señal y GND). | 1 |
| 11 | CH340C USB-UART (SOP-16) | WCH. Programación USB-C. Cristal interno integrado. LCSC C84681. Basic library JLCPCB. | 1 |
| 12 | AMS1117-3.3 LDO 3.3 V 1 A | Rail 3.3 V para ESP32-C3 y sensores I2C. SOT-223. | 1 |
| 13 | Conectores JST XH 3-pin | Para caudalímetro, válvula y sonda NTC. Conexión rápida sin soldar en campo. | 4 |
| 14 | PCB 2 capas | Diseño custom. JLCPCB PCBA. ~100×80 mm orientativo. | 1 |
| 15 | Cable USB-C | Alimentación y programación inicial. | 1 |
| 16 | Fuente 5 V / 1 A (USB) | Alimentación del sistema. | 1 |
| 17 | Caja ABS IP54 70×50×25 mm | Con prensaestopas PG7 para cables caudalímetro, válvula y alimentación. | 1 |

---

## Resumen de interfaces

| Bus / Protocolo | Pines ESP32-C3 | Dispositivos |
|-----------------|----------------|--------------|
| ADC 12-bit | GPIO2 | XGZP6847A (presión, analógico via divisor) |
| GPIO interrupt | GPIO3 | YF-B9 (pulsos caudalímetro) |
| ADC 12-bit | GPIO4 | NTC 10 kΩ (temperatura agua) |
| GPIO digital | GPIO5 / GPIO6 | Control relay válvula bola (OPEN / CLOSE) |
| I2C (SDA/SCL) | GPIO8 / GPIO9 | XDB401 (si se usa esta variante) |
| USB (UART) | — | CH340C para programación |

---

## Notas de diseño (validadas en revisión mayo 2026)

- **YF-B9 a 3.3 V:** Si se alimenta el caudalímetro al mismo rail 3.3 V del ESP32, la salida NPN ya es compatible sin divisor. Si se alimenta a 5 V, añadir divisor 10 kΩ + 20 kΩ antes del GPIO o usar buffer 74LVC1G17.
- **Caudalímetro siempre por interrupción** (RISING/FALLING): nunca por polling. Condensador 100 nF anti-rebotes.
- **Válvula bola CR01:** hilo OPEN+COM = abre; CLOSE+COM = cierra; sin corriente = mantiene posición (seguro para riego en corte de tensión).
- **XDB401 dirección I2C:** dirección nominal 0x6D, pero hay lotes que responden a 0x7F (reservado en el estándar). Verificar con i2cscanner en 5 muestras antes de comprometer MOQ.
- **XGZP6847A calibración:** variación ±1.5% FS. Suficiente para detección de fugas por umbral. Calibrar referencia en fábrica si se requiere lectura absoluta.
- **Notas NSF61:** Si caudalímetro y válvula están en contacto con agua potable, solicitar certificación NSF/ANSI 61 al fabricante.

---

## Estimación de costes

### Precios unitarios

| Componente | Retail 1u | Fábrica 100u | Fábrica 500u | Fabricante directo |
|-----------|:---------:|:------------:|:------------:|--------------------|
| ESP32-C3-MINI-1 | ~3 € | ~1.80–2 € | ~1.60–1.80 € | LCSC C2838502 |
| YF-B9 G3/4" | ~6 € | ~3–4 € | ~2–2.50 € | Zhongjiang / Tonhe (Alibaba) |
| XGZP6847A (recomendado) | ~3.50 € | ~2.20–2.80 € | ~2 € | CFSensor / DigiKey XGZP6847A700KPG |
| XDB401 I2C (alternativo) | ~15 € | ~13–15 € | ~10 € | xdbsensor.com / xidibei.com |
| Válvula bola 3/4" 12 V | ~9 € | ~5–7 € | ~3.50–5 € | Tonhe / CWX / Winner (Alibaba) |
| Válvula latching 3/4" | ~15–18 € | ~8–10 € | ~5–7 € | Hubei Chuangsinuo (CSN) |
| PCB + ensamblado SMT | — | ~8–12 €/ud | ~5–8 €/ud | JLCPCB PCBA |
| Pasivos + conectores + driver | — | ~1.50–2 €/ud | ~1–1.50 €/ud | LCSC basic/extended |
| Caja ABS IP54 | — | ~2–3 €/ud | ~1.50–2 €/ud | AliExpress / Gaoke |

### Coste total por unidad (configuración XGZP6847A + válvula bola)

| Escala | Coste componentes | PCB+SMT | Total/ud | Total lote |
|--------|:-----------------:|:-------:|:--------:|:----------:|
| 1u (prototipo) | ~24 € | — | ~24 € | ~24 € |
| 100u | ~20–22 € | ~10 €/ud | ~30–32 € | ~3.000–3.200 € |
| 500u | ~14–16 € | ~6–8 €/ud | ~20–24 € | ~10.000–12.000 € |

> Añadir ~15–20% para flete + aduanas España (IVA 21% en pedidos >150 €, arancel 0–3.7%).

---

## Fabricantes para contactar directamente

### Caudalímetro YF-B9
**Foshan Shunde Zhongjiang Energy Saving Electronics Co., Ltd.**
- Alibaba: buscar "Zhongjiang YF-B9" — marca SEA / DIJIANG / ZHONGJIANG
- 7 años en plataforma, OEM/ODM disponible
- Precio publicado retail: ~$6.61/ud. Contactar para 100u+.

### Sensor de presión analógico XGZP6847A
**CFSensor (China)**
- DigiKey: ref. `XGZP6847A700KPG` (~$3.50 retail)
- Alibaba/directamente: precio ~$2.20–2.80 a 200–1000u
- También disponible en versiones 0–0.5 bar, 0–1 bar, 0–4 bar, 0–7 bar

### Sensor de presión I2C XDB401 (alternativa premium)
**XIDIBEI Sensor & Control**
- Web: [xdbsensor.com](https://www.xdbsensor.com) / [xidibei.com](https://www.xidibei.com)
- Precio publicado GlobalSources: $21.98 (10–99u) → $16.98 (100–499u) → $14.98 (≥1000u)
- Contactar directamente por web (sin comisión Alibaba)
- OEM disponible ≥100u, logo personalizable ≥100u

### Válvula bola motorizada 3/4" 12 VDC
**Tonhe / CWX / Winner** — fabricantes principales en Zhejiang/Ningbo
- Alibaba: buscar "Tonhe motorized ball valve DN20 12V 3-wire" o "CWX-15Q"
- MISOL (Amazon) es reseller de Tonhe/CWX — contactar fábrica directa para mejor precio
- Precio referencia 100u: ~$5–7/ud FOB Ningbo

### Válvula latching/bistable 3/4" (alternativa)
**Hubei Chuangsinuo Electrical Technology Corp. (CSN)**
- Made-in-China: [transldq.en.made-in-china.com](https://transldq.en.made-in-china.com)
- Dirección: No. 162, Jingui Avenue, Enshi, Hubei, China
- CE, ISO, RoHS. Lead time: 5–7 días muestra / 15–25 días producción
- MOQ negociable desde 50u

---

## Comparativa sensores de presión

| Criterio | XGZP6847A (analógico) | XDB401 (I2C) |
|----------|:---------------------:|:------------:|
| Precio 1u | ~3.50 € | ~15 € |
| Precio 100u | ~2.50 € | ~14–15 € |
| Precio 500u | ~2.00 € | ~10 € |
| Interfaz | Analógico 0.5–4.5 V → ADC | I2C (0x6D) |
| Circuito extra | Divisor resistivo (si 5 V) | Solo cable I2C |
| Exactitud | ±1.5% FS | ±1.0% FS |
| Rosca | G1/8" | G1/4" |
| IP | IP65 | IP65 |
| Fabricante | CFSensor (en DigiKey) | XIDIBEI |
| ¿Válido para fugas? | ✅ Sí | ✅ Sí |
| **Recomendación** | **⭐ Producción** | Versión premium |

---

## Comparativa válvulas de corte 3/4"

| Criterio | Bola motorizada (3-wire) | Latching/bistable |
|----------|:------------------------:|:-----------------:|
| Precio 1u | ~9 € | ~15–18 € |
| Precio 100u | ~5–7 € | ~8–10 € |
| Consumo standby | 0 mA | 0 mA |
| Consumo activo | 60–80 mA (~5 s) | 300–800 mA (~200 ms) |
| Circuito control | Relay SPDT simple | H-bridge (DRV8833) + condensador |
| Presión mínima | Sin mínimo | ~0.05 bar |
| Fail-safe (sin corriente) | Mantiene posición | Mantiene posición |
| Velocidad | ~5 s | <0.5 s |
| Ciclos | 50.000–100.000 | >500.000 |
| Compatibilidad 12 V LiFePO4 | ✅ Directa | ✅ Con circuito |
| **Recomendación** | **⭐ Producción** | Aplicaciones ultra-low-power |

---

*Precios verificados mayo 2026. Tipo de cambio orientativo USD/EUR 0.92. FOB Shenzhen salvo indicación.*
*Para pedidos a fábrica: usar Alibaba Trade Assurance en el primer pedido. Solicitar siempre muestra (5 uds) antes de comprometer MOQ.*
