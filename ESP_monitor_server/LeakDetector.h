/**
 * @file LeakDetector.h
 * @brief Detector automático de fugas / roturas / obstrucciones en tubería de riego.
 *
 * Recibe muestras de presión y caudal cada ~500 ms y determina el estado de la tubería:
 *   "normal" | "leak" | "burst" | "obstruction"
 *
 * Aprende el baseline de la instalación mediante EMA durante los primeros WARMUP_SAMPLES
 * ciclos con la válvula abierta. Solo entonces activa la detección.
 *
 * Perfiles de riego predefinidos:
 *   IRRIG_SPRINKLER   — Aspersión       (default, valores actuales del firmware)
 *   IRRIG_DRIP        — Goteo           (0.5-2 bar, 0.5-5 L/min)
 *   IRRIG_DRIP_TAPE   — Cinta de goteo  (muy baja presión)
 *   IRRIG_MICRO_SPRK  — Microaspersión  (intermedio)
 */

#pragma once
#include <stdint.h>

// ── Tipos de riego ─────────────────────────────────────────────────────────────

enum IrrigationType : uint8_t {
    IRRIG_SPRINKLER   = 0,
    IRRIG_DRIP        = 1,
    IRRIG_DRIP_TAPE   = 2,
    IRRIG_MICRO_SPRK  = 3,
    IRRIG_TYPE_COUNT  = 4,
};

// ── Perfil de instalación ──────────────────────────────────────────────────────

struct IrrigationProfile {
    IrrigationType type;
    float nominal_pressure_bar;     ///< Presión dinámica esperada (válvula abierta)
    float static_pressure_bar;      ///< Presión estática (válvula cerrada)
    float nominal_flow_lpm;         ///< Caudal nominal ON (L/min)
    float leak_idle_threshold_lpm;  ///< Caudal OFF que indica fuga (L/min)
    float leak_on_deviation_pct;    ///< % de exceso sobre baseline → fuga
    float burst_pressure_drop_pct;  ///< % de caída de presión → rotura
    float obstruction_flow_drop_pct;///< % de caída de caudal → obstrucción
    float zero_flow_tolerance_lpm;  ///< Ruido mínimo del caudalímetro (L/min)
};

// ── Perfiles predefinidos ──────────────────────────────────────────────────────
//
// Columnas: type | nominal_p | static_p | nominal_q | leak_idle | leak_on% | burst_p% | obs_q% | zero_tol

static const IrrigationProfile IRRIG_PROFILES[IRRIG_TYPE_COUNT] = {
    { IRRIG_SPRINKLER,  2.80f, 3.50f, 5.00f, 0.50f, 20.0f, 40.0f, 30.0f, 0.25f },
    { IRRIG_DRIP,       1.50f, 2.00f, 2.00f, 0.25f, 15.0f, 30.0f, 25.0f, 0.25f },
    { IRRIG_DRIP_TAPE,  0.80f, 1.20f, 0.80f, 0.25f, 20.0f, 25.0f, 30.0f, 0.25f },
    { IRRIG_MICRO_SPRK, 2.20f, 3.00f, 3.50f, 0.40f, 18.0f, 35.0f, 28.0f, 0.25f },
};

inline const char* irrigTypeToStr(IrrigationType t) {
    switch (t) {
        case IRRIG_SPRINKLER:  return "sprinkler";
        case IRRIG_DRIP:       return "drip";
        case IRRIG_DRIP_TAPE:  return "drip_tape";
        case IRRIG_MICRO_SPRK: return "micro_sprinkler";
        default:               return "sprinkler";
    }
}

inline IrrigationType irrigStrToType(const char* s) {
    if (strcmp(s, "drip")            == 0) return IRRIG_DRIP;
    if (strcmp(s, "drip_tape")       == 0) return IRRIG_DRIP_TAPE;
    if (strcmp(s, "micro_sprinkler") == 0) return IRRIG_MICRO_SPRK;
    return IRRIG_SPRINKLER;
}

// ── Clase principal ────────────────────────────────────────────────────────────

class LeakDetector {
public:
    static constexpr float    EMA_ALPHA       = 0.05f;  ///< 1/EMA_ALPHA ≈ muestras para converger
    static constexpr uint16_t WARMUP_SAMPLES  = 20;     ///< Muestras con válvula ON antes de activar detección
    static constexpr uint8_t  IDLE_CONFIRM    = 3;      ///< Muestras consecutivas con caudal OFF para confirmar fuga
    static constexpr uint8_t  BURST_CONFIRM   = 2;      ///< Muestras consecutivas para confirmar burst (evita falso positivo)
    static constexpr uint8_t  OBSTR_CONFIRM   = 2;      ///< Muestras consecutivas para confirmar obstrucción

    // ── API pública ──────────────────────────────────────────────────────────

    /** Configura el detector con el perfil indicado. Llama reset() internamente. */
    void begin(IrrigationType type = IRRIG_SPRINKLER) {
        _profile = IRRIG_PROFILES[type < IRRIG_TYPE_COUNT ? type : 0];
        reset();
    }

    /** Reinicia el baseline (necesario al cambiar perfil o tras reparación). */
    void reset() {
        _ema_pressure   = _profile.nominal_pressure_bar;
        _ema_flow       = _profile.nominal_flow_lpm;
        _trained        = false;
        _warmup         = 0;
        _idle_leak_hits = 0;
        _burst_hits     = 0;
        _obstr_hits     = 0;
        _scenario       = "normal";
    }

    /**
     * @brief Actualiza el detector con una nueva muestra.
     * @param pressure_bar  Presión leída (bar). Si < 0 se ignora la presión.
     * @param flow_lpm      Caudal leído (L/min).
     * @param valve_open    true si algún relay está activo (válvula abierta).
     */
    void update(float pressure_bar, float flow_lpm, bool valve_open) {
        const IrrigationProfile& p = _profile;

        if (!valve_open) {
            // ── Válvula cerrada: cualquier caudal significativo = fuga ───────
            if (flow_lpm > p.leak_idle_threshold_lpm) {
                _idle_leak_hits++;
                if (_idle_leak_hits >= IDLE_CONFIRM) {
                    _scenario = "leak";
                }
            } else {
                _idle_leak_hits = 0;
                if (_scenario == _S_LEAK && _trained) {
                    // Solo limpiamos si la fuga desapareció con válvula cerrada
                    _scenario = "normal";
                }
            }
            // No entrenamos baseline con válvula cerrada
            return;
        }

        // ── Válvula abierta ───────────────────────────────────────────────────
        _idle_leak_hits = 0;

        if (!_trained) {
            // Fase de warm-up: acumular EMA sin disparar alertas
            if (_warmup == 0) {
                // Primer sample: inicializar EMA con valores reales
                if (pressure_bar >= 0.0f) _ema_pressure = pressure_bar;
                _ema_flow = flow_lpm;
            } else {
                if (pressure_bar >= 0.0f)
                    _ema_pressure = EMA_ALPHA * pressure_bar + (1.0f - EMA_ALPHA) * _ema_pressure;
                _ema_flow = EMA_ALPHA * flow_lpm + (1.0f - EMA_ALPHA) * _ema_flow;
            }
            _warmup++;
            if (_warmup >= WARMUP_SAMPLES) {
                _trained  = true;
                _scenario = "normal";
            }
            return;
        }

        // ── Detección (solo con baseline entrenado) ───────────────────────────

        bool pressure_valid = (pressure_bar >= 0.0f);

        // Burst: presión cae bruscamente (si hay sensor de presión)
        if (pressure_valid) {
            float drop_pct = (_ema_pressure > 0.0f)
                ? ((_ema_pressure - pressure_bar) / _ema_pressure) * 100.0f
                : 0.0f;
            if (drop_pct >= p.burst_pressure_drop_pct) {
                if (++_burst_hits >= BURST_CONFIRM) {
                    _scenario = "burst";
                }
                _obstr_hits = 0;
                // No actualizar EMA en burst — el baseline debe preservarse
                return;
            }
        }
        _burst_hits = 0;

        // Leak (válvula abierta): caudal excede baseline significativamente
        if (_ema_flow > p.zero_flow_tolerance_lpm) {
            float excess_pct = ((flow_lpm - _ema_flow) / _ema_flow) * 100.0f;
            if (excess_pct >= p.leak_on_deviation_pct) {
                _obstr_hits = 0;
                _scenario = "leak";
                return;
            }
        }

        // Obstruction: caudal cae significativamente (presión puede subir)
        if (_ema_flow > p.zero_flow_tolerance_lpm) {
            float drop_pct = ((_ema_flow - flow_lpm) / _ema_flow) * 100.0f;
            if (drop_pct >= p.obstruction_flow_drop_pct) {
                if (++_obstr_hits >= OBSTR_CONFIRM) {
                    _scenario = "obstruction";
                }
                return;
            }
        }
        _obstr_hits = 0;

        // Normal: actualizar EMA y resetear escenario
        if (pressure_valid)
            _ema_pressure = EMA_ALPHA * pressure_bar + (1.0f - EMA_ALPHA) * _ema_pressure;
        _ema_flow = EMA_ALPHA * flow_lpm + (1.0f - EMA_ALPHA) * _ema_flow;
        _scenario = "normal";
    }

    /** Resultado del último update(). */
    const char* scenario()   const { return _scenario; }

    /** true cuando el baseline está entrenado y la detección activa. */
    bool hasBaseline()        const { return _trained; }

    /** Progreso del warm-up (0–WARMUP_SAMPLES). */
    uint16_t warmupProgress() const { return _warmup; }

    /** EMA de presión aprendida (bar). */
    float baselinePressure()  const { return _ema_pressure; }

    /** EMA de caudal aprendido (L/min). */
    float baselineFlow()      const { return _ema_flow; }

    /** Perfil activo. */
    const IrrigationProfile& profile() const { return _profile; }

private:
    IrrigationProfile _profile       = IRRIG_PROFILES[IRRIG_SPRINKLER];
    float             _ema_pressure  = 2.80f;
    float             _ema_flow      = 5.00f;
    bool              _trained       = false;
    uint16_t          _warmup        = 0;
    uint8_t           _idle_leak_hits = 0;
    uint8_t           _burst_hits    = 0;
    uint8_t           _obstr_hits    = 0;
    const char*       _scenario      = "normal";

    static constexpr const char* _S_LEAK = "leak";
};
