#pragma once
// =============================================================================
// LED status no-bloqueante — máquina de estados con patrón por perfil
// Require: ledPin (int), LED_ON, LED_OFF — definidos en el sketch principal.
// =============================================================================
//
//  LED_PROVISIONING   — triple parpadeo lento cada 3 s → portal AP activo
//  LED_WIFI_CONNECTING — parpadeo rápido 100/100 ms    → buscando red WiFi
//  LED_MQTT_CONNECTING — doble parpadeo cada ~2 s      → WiFi OK, MQTT pendiente
//  LED_IDLE            — latido 50 ms / 2950 ms        → conectado, en espera
//  LED_TX_OK           — triple parpadeo (one-shot)    → telemetría enviada OK
//  LED_TX_ERROR        — 1 s ON / 1 s OFF              → error de red persistente
//  LED_RELAY_ON        — encendido fijo                → relay activo (IRRIGATION)

enum LedStateCode : uint8_t {
  LED_PROVISIONING = 0,
  LED_WIFI_CONNECTING,
  LED_MQTT_CONNECTING,
  LED_IDLE,
  LED_TX_OK,      // one-shot: vuelve automáticamente al estado anterior
  LED_TX_ERROR,
  LED_RELAY_ON,
  _LED_STATE_COUNT
};

struct LedStep { uint16_t onMs; uint16_t offMs; };
static const LedStep _ledPat[_LED_STATE_COUNT][4] = {
  /* PROVISIONING    */ {{300, 300}, {300,1800},{300,1800},{0, 0}},  // triple blink lento
  /* WIFI_CONNECTING */ {{100, 100}, {0, 0},    {0, 0},   {0, 0}},
  /* MQTT_CONNECTING */ {{50,  50 }, {50, 1800},{0, 0},   {0, 0}},  // doble blink + pausa
  /* IDLE            */ {{50, 2950}, {0, 0},    {0, 0},   {0, 0}},  // latido lento
  /* TX_OK           */ {{50,  50 }, {50,   50},{50, 800},{0, 0}},  // triple blink
  /* TX_ERROR        */ {{1000,1000},{0, 0},    {0, 0},   {0, 0}},
  /* RELAY_ON        */ {{0,   0  }, {0, 0},    {0, 0},   {0, 0}},  // sin timer (fijo ON)
};
static const uint8_t _ledPatLen[]  = { 3, 1, 2, 1, 3, 1, 0 };
static const bool    _ledOneShot[] = { false, false, false, false, true, false, false };

static volatile LedStateCode _ledState     = LED_PROVISIONING;
static LedStateCode          _ledPrevState = LED_IDLE;
static uint8_t               _ledStep      = 0;
static bool                  _ledPhaseOn   = true;
static volatile bool         _ledNeedsInit = true;
static unsigned long         _ledPhaseMs   = 0;

// Cambiar estado del LED (seguro desde cualquier tarea — solo escribe variables).
void setLedState(LedStateCode s) {
  if ((LedStateCode)_ledState == s) return;
  if (_ledOneShot[s]) _ledPrevState = (LedStateCode)_ledState;
  _ledState     = s;
  _ledStep      = 0;
  _ledPhaseOn   = true;
  _ledNeedsInit = true;  // ledTick() aplicará ON en el próximo ciclo
}

// Llamar únicamente desde loop() (Core 1). Es la única función que escribe el GPIO.
void ledTick() {
  if (ledPin < 0) return;  // placa sin LED onboard (p.ej. LilyGo T-Display)
  LedStateCode state = (LedStateCode)_ledState;
  if (state == LED_RELAY_ON) { digitalWrite(ledPin, LED_ON); return; }

  if (_ledNeedsInit) {
    _ledNeedsInit = false;
    _ledStep    = 0;
    _ledPhaseOn = true;
    _ledPhaseMs = millis();
    digitalWrite(ledPin, LED_ON);
    return;
  }

  const LedStep* pat = _ledPat[state];
  uint8_t len = _ledPatLen[state];
  unsigned long now = millis();
  uint16_t dur = _ledPhaseOn ? pat[_ledStep].onMs : pat[_ledStep].offMs;
  if (now - _ledPhaseMs < dur) return;
  _ledPhaseMs = now;
  if (_ledPhaseOn) {
    digitalWrite(ledPin, LED_OFF);
    _ledPhaseOn = false;
  } else {
    _ledStep++;
    if (_ledStep >= len) {
      if (_ledOneShot[state]) {
        _ledState     = _ledPrevState;
        _ledNeedsInit = true;
        return;
      }
      _ledStep = 0;
    }
    _ledPhaseOn = true;
    digitalWrite(ledPin, LED_ON);
  }
}
