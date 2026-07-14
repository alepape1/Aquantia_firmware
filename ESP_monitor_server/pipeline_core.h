// pipeline_core.h — Simulación de tubería y lectura de sensores reales XDB401 + caudalímetro.
// Incluido desde ESP_monitor_server.ino después de declarar las variables globales de pipeline,
// los contadores ISR del caudalímetro y los helpers XDB401 de sensor_recovery.h.
#pragma once

// ── Drift lento para valores simulados ──────────────────────────────────────
static float driftClamp(float v, float mn, float mx, float step) {
  float d = ((float)random(-100, 101) / 100.0f) * step;
  return constrain(v + d, mn, mx);
}

void updateSimulatedValues() {
  sim_tempMCP   = driftClamp(sim_tempMCP,   -10.0f,  45.0f,  0.05f);
  sim_tempDHT   = driftClamp(sim_tempDHT,   -10.0f,  45.0f,  0.05f);
  sim_humidity  = driftClamp(sim_humidity,   20.0f,  95.0f,  0.20f);
  sim_tempDHT11 = driftClamp(sim_tempDHT11, -10.0f,  45.0f,  0.05f);
  sim_humDHT11  = driftClamp(sim_humDHT11,   20.0f,  95.0f,  0.20f);
  sim_pressure  = driftClamp(sim_pressure,   95.0f, 110.0f,  0.02f);
  sim_light     = driftClamp(sim_light,       0.0f, 2000.0f, 5.0f);
  sim_windSpeed = driftClamp(sim_windSpeed,   0.0f,   15.0f, 0.3f);
  sim_windDir        = fmod(sim_windDir + ((float)random(-10, 11) / 10.0f) * 5.0f + 360.0f, 360.0f);
  sim_soilMoisture   = driftClamp(sim_soilMoisture, 0.0f, 100.0f, 0.5f);
}

// ── Ruido determinista para la simulación de tubería ─────────────────────────
// Tres ondas sinusoidales — idéntico al pipeline_sim.py del backend.
static float pipelineNoise(float t_s, int ch) {
  return sinf(t_s *  7.3f + ch * 1.7f) * 0.55f
       + sinf(t_s * 13.1f + ch * 3.2f) * 0.30f
       + sinf(t_s * 31.7f + ch * 5.1f) * 0.15f;
}

void updatePipelineSimValues() {
  const bool valveOpen = anyRelayActive();
  float t       = millis() / 1000.0f;
  float p_noise = pipelineNoise(t, 0) * PIPELINE_NOISE_P;
  float q_noise = pipelineNoise(t, 1) * PIPELINE_NOISE_Q;

  if (strcmp(pipelineScenario, "burst") == 0) {
    sim_pipeline_pressure = max(0.0f, 0.25f + p_noise * 0.4f);
    sim_pipeline_flow     = valveOpen
      ? max(0.0f, PIPELINE_NOMINAL_Q * 0.08f + fabsf(q_noise) * 0.3f)
      : 0.0f;

  } else if (strcmp(pipelineScenario, "obstruction") == 0) {
    if (valveOpen) {
      sim_pipeline_pressure = max(0.0f, PIPELINE_STATIC_P + p_noise * 0.5f);
      sim_pipeline_flow     = max(0.0f, fabsf(q_noise) * 0.04f);
    } else {
      sim_pipeline_pressure = max(0.0f, PIPELINE_STATIC_P + p_noise);
      sim_pipeline_flow     = max(0.0f, fabsf(q_noise) * 0.05f);
    }

  } else if (strcmp(pipelineScenario, "leak") == 0) {
    if (valveOpen) {
      sim_pipeline_pressure = max(0.0f, PIPELINE_DYNAMIC_P - 0.18f + p_noise);
      sim_pipeline_flow     = max(0.0f, PIPELINE_NOMINAL_Q - 0.45f + q_noise);
    } else {
      sim_pipeline_pressure = max(0.0f, PIPELINE_STATIC_P - 0.10f + p_noise);
      sim_pipeline_flow     = max(0.0f, 0.28f + fabsf(q_noise) * 0.35f);
    }

  } else {  // "normal"
    if (valveOpen) {
      sim_pipeline_pressure = max(0.0f, PIPELINE_DYNAMIC_P + p_noise);
      sim_pipeline_flow     = max(0.0f, PIPELINE_NOMINAL_Q + q_noise);
    } else {
      sim_pipeline_pressure = max(0.0f, PIPELINE_STATIC_P + p_noise);
      sim_pipeline_flow     = max(0.0f, fabsf(q_noise) * 0.05f);
    }
  }
}

// ── Helper XDB401: reintentos + lectura con gestión de fallos ─────────────────
// Centraliza la lógica duplicada de reconexión y lectura del sensor de presión.
// Siempre asigna pressureBar: NAN si falla, valor real si tiene éxito.
static void readXDB401Safe(float& pressureBar) {
  pressureBar = NAN;
  if (!xdb401_ok && millis() >= xdb401_retry_at) {
    if (xdb401_begin()) {
      float _p0, _t0;
      if (xdb401_read(_p0, _t0)) {
        xdb401_ok = true; xdb401_failures = 0; xdb401_recovery_failures = 0;
        pressureBar = _p0; xdb401Temperature = _t0;
        DLOGLN("[XDB401] Reconectado tras fallo");
      }
    }
    if (!xdb401_ok) xdb401_schedule_retry_after_recovery_fail();
  }
  if (xdb401_ok) {
    float p, tc;
    if (xdb401_read(p, tc)) {
      pressureBar = p; xdb401Temperature = tc;
      xdb401_failures = 0; xdb401_recovery_failures = 0;
    } else if (++xdb401_failures >= XDB401_MAX_FAILURES) {
      xdb401_ok       = false;
      xdb401_retry_at = millis() + XDB401_RETRY_INTERVAL;
      DLOGF("[XDB401] %u fallos consecutivos — suspendido %lus\n",
            (unsigned)XDB401_MAX_FAILURES, (unsigned long)XDB401_RETRY_INTERVAL / 1000);
    }
  }
}

// ── Lectura de sensores reales de tubería ─────────────────────────────────────
static bool readRealPipelineSensors(float& pressureBar, float& flowLpm) {
#if defined(FLOW_PIN)
  unsigned long now = micros();
  unsigned long dt  = now - _flowLastCalcUs;

  if (dt < 500000UL) {
    // Intervalo demasiado corto para recalcular caudal — reutilizar último valor.
    // Pero SÍ leemos el XDB401: presión y caudal son independientes.
    //
    // Guarda contra caudal fantasma: si el flujo se detuvo bruscamente, _flowLpm
    // seguiría siendo positivo durante hasta 500 ms. Si no llegó ningún pulso en
    // el intervalo actual Y ese intervalo supera 2 períodos esperados al caudal
    // actual, forzamos cero para reflejar la parada inmediatamente.
    if (_flowLpm > 0.0f) {
      noInterrupts();
      uint32_t _peekCount = _flowPulseCount;
      interrupts();
      float _twoPeriodsUs = 2.0f * 60.0e6f / (_flowLpm * (float)FLOW_K_FACTOR);
      if (_peekCount == 0 && (float)dt >= _twoPeriodsUs) _flowLpm = 0.0f;
    }
    flowLpm = _flowLpm;
    readXDB401Safe(pressureBar);
    return true;
  }

  noInterrupts();
  uint32_t pulses = _flowPulseCount;
  _flowPulseCount = 0;
  interrupts();

  _flowLastCalcUs = now;
  _flowLastDtUs   = dt;
  _flowLastPulses = pulses;

  float dt_s = dt * 1e-6f;
  _flowLpm   = (pulses * 60.0f) / (dt_s * (float)FLOW_K_FACTOR);
  flowLpm    = _flowLpm;

  if (anyRelayActive()) _flowIrrigPulses += pulses;
  else                  _flowLeakPulses  += pulses;

  noInterrupts();
  uint32_t _fTot  = _flowPulseTotal;
  uint32_t _fBase = _flowSessionBase;
  interrupts();
  g_flowSessionL = (_fTot - _fBase) / (float)FLOW_K_FACTOR;

  readXDB401Safe(pressureBar);
  return true;
#else
  // Sin caudalímetro: si hay XDB401 devolvemos presión real con caudal 0
  readXDB401Safe(pressureBar);
  if (!isnan(pressureBar)) {
    flowLpm = 0.0f;
    return true;
  }
  (void)pressureBar;
  (void)flowLpm;
  return false;
#endif
}

// ── Actualización de valores de tubería según modo ───────────────────────────
void updatePipelineValues() {
  if (strcmp(pipelineMode, "real") == 0) {
    float realPressure = 0.0f, realFlow = 0.0f;
    if (readRealPipelineSensors(realPressure, realFlow)) {
      sim_pipeline_flow = max(0.0f, realFlow);
      pipelineFlowOk    = true;

      if (!isnan(realPressure)) {
        sim_pipeline_pressure = realPressure;
        pipelinePressureOk    = true;
        pipelineSource        = "real";
      } else {
        float savedFlow = sim_pipeline_flow;
        updatePipelineSimValues();
        sim_pipeline_flow  = savedFlow;
        pipelinePressureOk = false;
        pipelineSource     = "real_flow";
      }
      leakDetector.update(sim_pipeline_pressure, sim_pipeline_flow, anyRelayActive());
      strlcpy(pipelineScenario, leakDetector.scenario(), sizeof(pipelineScenario));
      return;
    }
    pipelineSource     = "fallback";
    pipelinePressureOk = false;
    pipelineFlowOk     = false;
  } else {
    pipelineSource     = "sim";
    pipelinePressureOk = false;
    pipelineFlowOk     = false;
  }
  updatePipelineSimValues();
}
