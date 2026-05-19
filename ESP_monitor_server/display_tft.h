#pragma once
// =============================================================================
// PANTALLA TFT — LilyGo TTGO T-Display (ST7789, 240×135)
// Solo se incluye cuando HAS_DISPLAY está definido.
// Requiere: tft, spr, lastActivityTime, displayOn, displayView (globals),
//           todas las variables de sensor y pipeline (globals del sketch).
// =============================================================================

// Paleta Aquantia: azul corporativo #5ab4e0 · cabecera navy #0d3a6e
#define C_BG      0x0841   // fondo general    ~#0d0d0d
#define C_CARD    0x1082   // fondo tarjeta    ~#101010
#define C_HDR     0x09CD   // cabecera         ~#0d3a6e (navy Aquantia)
#define C_BORDER  0x2124   // borde tarjeta
#define C_TEXT    0xFFFF   // texto principal  blanco
#define C_LABEL   0x7BEF   // etiqueta gris claro
#define C_SIM     0xFD20   // simulado         naranja
#define C_REAL    0x5DBC   // dato real        #5ab4e0 (azul Aquantia)
#define C_RED     0xF800   // error/alerta     rojo

// Layout 240×135 — 2 filas × 3 cols + fila extra para luz
#define HDR_H    18
#define CARD_W   79
#define CARD_H   57

static int cardX(int col) { return col * (CARD_W + 1); }
static int cardY(int row) { return HDR_H + row * (CARD_H + 2); }

void iconThermometer(int cx, int cy, uint16_t col) {
  spr.fillRoundRect(cx - 2, cy - 7, 5, 10, 2, col);
  spr.fillCircle(cx, cy + 5, 4, col);
  spr.drawFastVLine(cx, cy - 7, 8, C_CARD);
}

void iconDrop(int cx, int cy, uint16_t col) {
  spr.fillCircle(cx, cy + 4, 4, col);
  for (int i = 0; i <= 7; i++) {
    int hw = (7 - i) / 2;
    spr.drawFastHLine(cx - hw, cy - 3 + i, hw * 2 + 1, col);
  }
}

void iconGauge(int cx, int cy, uint16_t col) {
  spr.drawCircle(cx, cy, 7, col);
  spr.drawLine(cx, cy, cx + 4, cy - 4, col);
  spr.fillCircle(cx, cy, 2, col);
  spr.drawFastHLine(cx - 5, cy + 5, 11, col);
}

void iconWind(int cx, int cy, uint16_t col) {
  spr.drawFastHLine(cx - 6, cy - 3, 12, col);
  spr.drawLine(cx + 6, cy - 3, cx + 8, cy - 1, col);
  spr.drawFastHLine(cx - 6, cy,     10, col);
  spr.drawFastHLine(cx - 6, cy + 3, 12, col);
}

void iconCompass(int cx, int cy, uint16_t col) {
  spr.drawCircle(cx, cy, 7, col);
  spr.fillTriangle(cx, cy - 6, cx - 2, cy, cx + 2, cy, col);
  spr.fillTriangle(cx, cy + 6, cx - 2, cy, cx + 2, cy, C_LABEL);
}

void iconSun(int cx, int cy, uint16_t col) {
  spr.fillCircle(cx, cy, 4, col);
  for (int a = 0; a < 360; a += 45) {
    float r = a * PI / 180.0f;
    int x1 = cx + (int)(6 * cos(r));
    int y1 = cy + (int)(6 * sin(r));
    int x2 = cx + (int)(9 * cos(r));
    int y2 = cy + (int)(9 * sin(r));
    spr.drawLine(x1, y1, x2, y2, col);
  }
}

typedef void (*IconFn)(int, int, uint16_t);
IconFn iconFns[6] = {
  iconThermometer,
  iconThermometer,
  iconDrop,
  iconGauge,
  iconWind,
  iconCompass,
};

void drawCard(int col, int row,
              const char* label,
              float value, const char* unit,
              bool simulated,
              bool showCompass = false) {
  int x = cardX(col);
  int y = cardY(row);

  spr.fillRoundRect(x + 1, y + 1, CARD_W - 2, CARD_H - 2, 4, C_CARD);
  spr.drawRoundRect(x, y, CARD_W, CARD_H, 4, C_BORDER);

  uint16_t accentCol = simulated ? C_SIM : C_REAL;
  spr.fillRoundRect(x + 1, y + 1, CARD_W - 2, 3, 2, accentCol);

  int idx = row * 3 + col;
  iconFns[idx](x + 10, y + 13, accentCol);

  spr.setTextColor(C_LABEL, C_CARD);
  spr.drawString(label, x + 22, y + 5, 1);

  spr.setTextColor(C_TEXT, C_CARD);
  char buf[12];
  snprintf(buf, sizeof(buf), showCompass ? "%.0f" : "%.1f", value);
  spr.drawString(buf, x + 4, y + 18, 4);

  spr.setTextColor(C_LABEL, C_CARD);
  spr.drawString(unit, x + 4, y + CARD_H - 13, 1);

  if (showCompass) {
    spr.setTextColor(accentCol, C_CARD);
    spr.drawString(degToCompass(value), x + 42, y + CARD_H - 13, 2);
  }

  const char* badge = simulated ? "[SIM]" : "[OK]";
  spr.setTextColor(accentCol, C_CARD);
  spr.drawRightString(badge, x + CARD_W - 2, y + CARD_H - 13, 1);
}

void drawScreen() {
  spr.fillSprite(C_BG);

  spr.fillRect(0, 0, 240, HDR_H, C_HDR);
  spr.setTextColor(C_TEXT, C_HDR);
  spr.drawString("METEOSTATION", 6, 4, 2);

  spr.fillCircle(110, 9, 3, C_TEXT);
  spr.fillCircle(119, 9, 3, C_LABEL);
  spr.fillCircle(128, 9, 3, C_LABEL);
  spr.fillCircle(137, 9, 3, C_LABEL);

  uint16_t luxCol = tsl_ok ? C_REAL : C_SIM;
  iconSun(118, 9, luxCol);
  char luxBuf[10];
  snprintf(luxBuf, sizeof(luxBuf), "%.0flx", lightLevel);
  spr.setTextColor(luxCol, C_HDR);
  spr.drawString(luxBuf, 130, 5, 1);

  if (WiFi.status() == WL_CONNECTED) {
    spr.setTextColor(C_REAL, C_HDR);
    spr.drawString("WiFi", 168, 5, 1);
  } else {
    spr.setTextColor(C_RED, C_HDR);
    spr.drawString("NoWiFi", 157, 5, 1);
  }

  uint16_t srvCol = lastServerOK ? C_REAL : C_RED;
  spr.fillCircle(230, 9, 5, srvCol);
  spr.drawCircle(230, 9, 5, C_TEXT);

  drawCard(0, 0, "T.EXT",   temperatureMCP,     "C",    !temp_ok);
  drawCard(1, 0, "T.INT",   temperatureDHT,     "C",    !htu_ok);
  drawCard(2, 0, "HUMEDAD", humidity,           "%",    !htu_ok);
  drawCard(0, 1, "PRESION", (float)pressure,    "KPa",  !bar_ok);
  drawCard(1, 1, "VIENTO",  windSpeedFiltered,  "m/s",  false);
  drawCard(2, 1, "DIRECC.", currentWindDirDeg,  "deg",  false, true);

  spr.pushSprite(0, 0);
}

// ── Vista 2: Pipeline (caudal + presión) ─────────────────────────────────────
void drawPipelineScreen() {
  spr.fillSprite(C_BG);

  spr.fillRect(0, 0, 240, HDR_H, C_HDR);
  spr.setTextColor(C_TEXT, C_HDR);
  spr.drawString("PIPELINE", 6, 4, 2);

  spr.fillCircle(110, 9, 3, C_LABEL);
  spr.fillCircle(119, 9, 3, C_TEXT);
  spr.fillCircle(128, 9, 3, C_LABEL);
  spr.fillCircle(137, 9, 3, C_LABEL);

  if (WiFi.status() == WL_CONNECTED) {
    spr.setTextColor(C_REAL, C_HDR);
    spr.drawString("WiFi", 168, 5, 1);
  } else {
    spr.setTextColor(C_RED, C_HDR);
    spr.drawString("NoWiFi", 157, 5, 1);
  }
  uint16_t srvCol = lastServerOK ? C_REAL : C_RED;
  spr.fillCircle(230, 9, 5, srvCol);
  spr.drawCircle(230, 9, 5, C_TEXT);

  bool pipeSim = (strcmp(pipelineMode, "sim") == 0);
  uint16_t pCol = (pipeSim || !pipelinePressureOk) ? C_SIM : C_REAL;
  uint16_t fCol = (pipeSim || !pipelineFlowOk)     ? C_SIM : C_REAL;

  int cY = HDR_H + 2;
  int cH = 88;

  // ── Tarjeta PRESION ────────────────────────────────────────────────────────
  spr.fillRoundRect(  1, cY, 115, cH, 4, C_CARD);
  spr.drawRoundRect(  0, cY, 117, cH, 4, C_BORDER);
  spr.fillRoundRect(  1, cY, 115,  3, 2, pCol);

  iconGauge(14, cY + 14, pCol);
  spr.setTextColor(C_LABEL, C_CARD);
  spr.drawString("PRESION", 28, cY + 6, 1);

  spr.setTextColor(C_TEXT, C_CARD);
  char buf[16];
  snprintf(buf, sizeof(buf), "%.2f", sim_pipeline_pressure);
  spr.drawString(buf, 6, cY + 22, 4);

  spr.setTextColor(C_LABEL, C_CARD);
  spr.drawString("bar", 6, cY + cH - 14, 1);
  spr.setTextColor(pCol, C_CARD);
  spr.drawRightString(pipeSim || !pipelinePressureOk ? "[SIM]" : "[OK]",
                      113, cY + cH - 14, 1);

  // ── Tarjeta CAUDAL ─────────────────────────────────────────────────────────
  spr.fillRoundRect(122, cY, 116, cH, 4, C_CARD);
  spr.drawRoundRect(121, cY, 118, cH, 4, C_BORDER);
  spr.fillRoundRect(122, cY, 116,  3, 2, fCol);

  iconDrop(134, cY + 14, fCol);
  spr.setTextColor(C_LABEL, C_CARD);
  spr.drawString("CAUDAL", 148, cY + 6, 1);

  spr.setTextColor(C_TEXT, C_CARD);
  snprintf(buf, sizeof(buf), "%.2f", sim_pipeline_flow);
  spr.drawString(buf, 126, cY + 22, 4);

  spr.setTextColor(C_LABEL, C_CARD);
  spr.drawString("L/min", 126, cY + cH - 14, 1);
  spr.setTextColor(fCol, C_CARD);
  spr.drawRightString(pipeSim || !pipelineFlowOk ? "[SIM]" : "[OK]",
                      237, cY + cH - 14, 1);

  // ── Franja inferior: modo + escenario + temperatura agua ──────────────────
  int bY = cY + cH + 3;
  int bH = 135 - bY;
  spr.fillRoundRect(0, bY, 240, bH, 3, C_CARD);

  uint16_t modeCol = pipeSim ? C_SIM : C_REAL;
  spr.setTextColor(modeCol, C_CARD);
  spr.drawCentreString(
    pipeSim ? "MODO: SIMULADO" : "MODO: REAL",
    120, bY + 1, 1);

  spr.setTextColor(C_LABEL, C_CARD);
  spr.drawString("Escenario:", 6, bY + 8, 1);
  spr.setTextColor(C_TEXT, C_CARD);
  spr.drawString(pipelineScenario, 68, bY + 8, 1);

  if (xdb401_ok && !isnan(xdb401Temperature)) {
    char tbuf[10];
    snprintf(tbuf, sizeof(tbuf), "%.1f", xdb401Temperature);
    spr.setTextColor(C_LABEL, C_CARD);
    spr.drawString("Taq:", 148, bY + 8, 1);
    spr.setTextColor(C_REAL, C_CARD);
    spr.drawString(tbuf, 170, bY + 8, 1);
    spr.setTextColor(C_LABEL, C_CARD);
    spr.drawString("\xB0C", 198, bY + 8, 1);
  }

  // ── Litros del ciclo actual ────────────────────────────────────────────────
  spr.setTextColor(C_LABEL, C_CARD);
  spr.drawString("Ciclo:", 6, bY + 16, 1);
#if defined(FLOW_PIN)
  char lbuf[12];
  snprintf(lbuf, sizeof(lbuf), "%.2f L", g_flowSessionL);
  spr.setTextColor(!pipeSim ? C_REAL : C_SIM, C_CARD);
  spr.drawString(lbuf, 44, bY + 16, 1);
#else
  spr.setTextColor(C_SIM, C_CARD);
  spr.drawString("--", 44, bY + 16, 1);
#endif

  spr.pushSprite(0, 0);
}

// ── Vista 3: Info del dispositivo ────────────────────────────────────────────
void drawInfoScreen() {
  spr.fillSprite(C_BG);

  spr.fillRect(0, 0, 240, HDR_H, C_HDR);
  spr.setTextColor(C_TEXT, C_HDR);
  spr.drawString("DISPOSITIVO", 6, 4, 2);

  spr.fillCircle(110, 9, 3, C_LABEL);
  spr.fillCircle(119, 9, 3, C_LABEL);
  spr.fillCircle(128, 9, 3, C_TEXT);
  spr.fillCircle(137, 9, 3, C_LABEL);

  if (WiFi.status() == WL_CONNECTED) {
    spr.setTextColor(C_REAL, C_HDR);
    spr.drawString("WiFi", 168, 5, 1);
  } else {
    spr.setTextColor(C_RED, C_HDR);
    spr.drawString("NoWiFi", 157, 5, 1);
  }
  uint16_t srvCol = lastServerOK ? C_REAL : C_RED;
  spr.fillCircle(230, 9, 5, srvCol);
  spr.drawCircle(230, 9, 5, C_TEXT);

  int y = HDR_H + 6;
  const int lh = 16;

  spr.setTextColor(C_LABEL, C_BG);
  spr.drawString("Reinicio:", 4, y, 1);
  spr.setTextColor(C_TEXT, C_BG);
  spr.drawString(g_rebootReason, 70, y, 1);
  y += lh;

  spr.setTextColor(C_LABEL, C_BG);
  spr.drawString("Conexion:", 4, y, 1);
  spr.setTextColor(C_REAL, C_BG);
  spr.drawString(g_lastConnectStr, 70, y, 1);
  y += lh;

  spr.setTextColor(C_LABEL, C_BG);
  spr.drawString("IP:", 4, y, 1);
  spr.setTextColor(C_TEXT, C_BG);
  spr.drawString(WiFi.localIP().toString().c_str(), 70, y, 1);
  y += lh;

  int rssi = WiFi.RSSI();
  spr.setTextColor(C_LABEL, C_BG);
  spr.drawString("RSSI:", 4, y, 1);
  char rssiBuf[12];
  snprintf(rssiBuf, sizeof(rssiBuf), "%d dBm", rssi);
  spr.setTextColor(rssi > -70 ? C_REAL : C_RED, C_BG);
  spr.drawString(rssiBuf, 70, y, 1);
  y += lh;

  unsigned long up = millis() / 1000;
  char uptBuf[20];
  snprintf(uptBuf, sizeof(uptBuf), "%luh %02lum %02lus",
           up / 3600, (up % 3600) / 60, up % 60);
  spr.setTextColor(C_LABEL, C_BG);
  spr.drawString("Uptime:", 4, y, 1);
  spr.setTextColor(C_TEXT, C_BG);
  spr.drawString(uptBuf, 70, y, 1);
  y += lh;

  spr.setTextColor(C_LABEL, C_BG);
  spr.drawString("MAC:", 4, y, 1);
  spr.setTextColor(C_LABEL, C_BG);
  spr.drawString(WiFi.macAddress().c_str(), 36, y, 1);
  y += lh;

  spr.setTextColor(C_LABEL, C_BG);
  spr.drawString("FW:", 4, y, 1);
  spr.setTextColor(C_TEXT, C_BG);
  spr.drawString(FIRMWARE_VERSION, 36, y, 1);

  spr.pushSprite(0, 0);
}

// Pantalla que se muestra mientras el portal SoftAP está activo
void drawAPScreen(const char* ap_ssid, const char* serial) {
  spr.fillSprite(C_BG);
  spr.fillRect(0, 0, 240, HDR_H, C_HDR);
  spr.setTextColor(C_TEXT, C_HDR);
  spr.drawCentreString("AQUANTIA  SETUP", 120, 4, 2);

  spr.setTextColor(C_REAL, C_BG);
  spr.drawCentreString("Configuracion WiFi", 120, 24, 2);

  spr.setTextColor(C_LABEL, C_BG);
  spr.drawString("Conecta tu movil a:", 6, 46, 1);
  spr.setTextColor(C_TEXT, C_BG);
  spr.drawString(ap_ssid, 6, 58, 2);

  spr.setTextColor(C_LABEL, C_BG);
  spr.drawString("Pass: aquantia1", 6, 80, 1);
  spr.drawString("Web:  192.168.4.1", 6, 92, 1);

  spr.setTextColor(C_SIM, C_BG);
  spr.drawString(serial, 6, 112, 1);

  spr.pushSprite(0, 0);
}

// ── Vista 4: Suelo Helissense ─────────────────────────────────────────────────
void iconLeaf(int cx, int cy, uint16_t col) {
  spr.fillEllipse(cx, cy - 2, 5, 7, col);
  spr.drawFastVLine(cx, cy + 3, 5, col);
}

void iconFlask(int cx, int cy, uint16_t col) {
  spr.drawLine(cx - 3, cy - 7, cx - 3, cy - 1, col);
  spr.drawLine(cx + 3, cy - 7, cx + 3, cy - 1, col);
  spr.fillTriangle(cx - 5, cy + 6, cx + 5, cy + 6, cx, cy - 1, col);
}

void iconEC(int cx, int cy, uint16_t col) {
  spr.drawFastHLine(cx - 5, cy - 3, 10, col);
  spr.drawFastHLine(cx - 5, cy,     8,  col);
  spr.drawFastHLine(cx - 5, cy + 3, 10, col);
  spr.drawFastVLine(cx - 5, cy - 3, 7,  col);
}

void drawSoilCard(int col, int row, const char* label, float value, const char* unit,
                  bool ok, void (*icon)(int, int, uint16_t), bool isInt = false) {
  int x = cardX(col);
  int y = cardY(row);
  uint16_t accentCol = ok ? C_REAL : C_SIM;

  spr.fillRoundRect(x + 1, y + 1, CARD_W - 2, CARD_H - 2, 4, C_CARD);
  spr.drawRoundRect(x, y, CARD_W, CARD_H, 4, C_BORDER);
  spr.fillRoundRect(x + 1, y + 1, CARD_W - 2, 3, 2, accentCol);

  icon(x + 10, y + 13, accentCol);

  spr.setTextColor(C_LABEL, C_CARD);
  spr.drawString(label, x + 22, y + 5, 1);

  spr.setTextColor(ok ? C_TEXT : C_LABEL, C_CARD);
  char buf[12];
  if (ok) {
    if (isInt) snprintf(buf, sizeof(buf), "%.0f", value);
    else       snprintf(buf, sizeof(buf), "%.1f", value);
  } else {
    snprintf(buf, sizeof(buf), "--");
  }
  spr.drawString(buf, x + 4, y + 18, 4);

  spr.setTextColor(C_LABEL, C_CARD);
  spr.drawString(unit, x + 4, y + CARD_H - 13, 1);

  spr.setTextColor(accentCol, C_CARD);
  spr.drawRightString(ok ? "[OK]" : "[ERR]", x + CARD_W - 2, y + CARD_H - 13, 1);
}

void drawSueloScreen(bool haliOk, float soilMoist, float soilTemp, float soilPh,
                     int soilN, int soilP, int soilK) {
  spr.fillSprite(C_BG);

  spr.fillRect(0, 0, 240, HDR_H, C_HDR);
  spr.setTextColor(C_TEXT, C_HDR);
  spr.drawString("SUELO", 6, 4, 2);

  // Indicadores de vista (4 puntos, el 4º activo)
  spr.fillCircle(110, 9, 3, C_LABEL);
  spr.fillCircle(119, 9, 3, C_LABEL);
  spr.fillCircle(128, 9, 3, C_LABEL);
  spr.fillCircle(137, 9, 3, C_TEXT);

  // Estado Helissense — mismo estilo que WiFi en el resto de pantallas
  uint16_t haliCol = haliOk ? C_REAL : C_RED;
  spr.setTextColor(haliCol, C_HDR);
  spr.drawString(haliOk ? "HALI" : "HALI!", 152, 5, 1);

  if (WiFi.status() == WL_CONNECTED) {
    spr.setTextColor(C_REAL, C_HDR);
    spr.drawString("WiFi", 178, 5, 1);
  } else {
    spr.setTextColor(C_RED, C_HDR);
    spr.drawString("NoWiFi", 167, 5, 1);
  }
  uint16_t srvCol2 = lastServerOK ? C_REAL : C_RED;
  spr.fillCircle(230, 9, 5, srvCol2);
  spr.drawCircle(230, 9, 5, C_TEXT);

  // Fila 0: Humedad, Temp suelo, pH
  drawSoilCard(0, 0, "HUMEDAD",  soilMoist, "%",   haliOk, iconDrop);
  drawSoilCard(1, 0, "T.SUELO",  soilTemp,  "C",   haliOk, iconThermometer);
  drawSoilCard(2, 0, "pH",       soilPh,    "",    haliOk, iconFlask);

  // Fila 1: N, P, K
  drawSoilCard(0, 1, "N",  (float)soilN, "mg/kg", haliOk, iconLeaf, true);
  drawSoilCard(1, 1, "P",  (float)soilP, "mg/kg", haliOk, iconEC,   true);
  drawSoilCard(2, 1, "K",  (float)soilK, "mg/kg", haliOk, iconGauge, true);

  spr.pushSprite(0, 0);
}

void drawBootScreen(const char* wifiMsg) {
  spr.fillSprite(C_BG);

  spr.fillRect(0, 0, 240, HDR_H, C_HDR);
  spr.setTextColor(C_TEXT, C_HDR);
  spr.drawCentreString("METEOSTATION  v3", 120, 4, 2);

  struct { const char* lbl; bool ok; } sensors[4] = {
    { "MCP9808  (T.Ext)", mcp_ok },
    { "Barometro       ", bar_ok },
    { "HTU2x    (T+H)  ", htu_ok },
    { "LuzAmb   (0x39) ", tsl_ok },
  };

  for (int i = 0; i < 4; i++) {
    int y = 22 + i * 24;
    spr.setTextColor(C_LABEL, C_BG);
    spr.drawString(sensors[i].lbl, 8, y, 2);

    uint16_t badgeCol    = sensors[i].ok ? C_REAL : C_SIM;
    const char* badgeTxt = sensors[i].ok ? "  REAL  " : "  SIM  ";
    spr.fillRoundRect(163, y, 60, 16, 3, badgeCol);
    spr.setTextColor(TFT_BLACK, badgeCol);
    spr.drawCentreString(badgeTxt, 193, y + 4, 1);
  }

  spr.setTextColor(C_LABEL, C_BG);
  spr.drawCentreString(wifiMsg, 120, 120, 2);

  spr.pushSprite(0, 0);
}
