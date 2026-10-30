/*
 * Interfaz minimalista para la CYD (320x240, apaisada).
 *
 * Solo depende de TFT_eSPI: no sabe nada de WiFi ni de la API, así que se puede
 * retocar (colores, posiciones) sin tocar el resto del firmware.
 *
 *   ┌──────────────────────────────────────┐
 *   │ hace 2 min                      ◜◝   │  fila superior
 *   │                                      │
 *   │      1 2 8                  ↘        │  número grande + flecha
 *   │                            -12       │  variación (5 min)
 *   │                           mg/dL      │
 *   │ ▰▰▰▰▰▰▰▰▱▱▱▱▱▱▱▱▱▱▱▱▱▱▱▱▱▱▱▱▱▱▱▱▱▱  │  barra de rango
 *   │ ▂ ▃ ▃ ▄ ▅ ▅ ▅ ▆ ▆ ▅ ▅ ▆              │  últimas lecturas
 *   └──────────────────────────────────────┘
 */
#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>
#include "fonts.h"

namespace ui {

// ---------------------------------------------------------------- colores
constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

constexpr uint16_t BG     = rgb(8, 10, 12);       // casi negro
constexpr uint16_t GREEN  = rgb(34, 224, 122);    // en rango
constexpr uint16_t AMBER  = rgb(255, 176, 32);    // alto
constexpr uint16_t RED    = rgb(255, 82, 82);     // bajo
constexpr uint16_t GREY   = rgb(104, 112, 122);   // sin datos
constexpr uint16_t TEXT   = rgb(236, 239, 244);   // texto principal
constexpr uint16_t MUTED  = rgb(128, 136, 146);   // texto secundario
constexpr uint16_t TRACK  = rgb(26, 33, 30);      // fondo de la barra
constexpr uint16_t ICON_OFF = rgb(40, 46, 52);    // wifi apagado

// ---------------------------------------------------------------- geometría
constexpr int SCREEN_W = 320;
constexpr int SCREEN_H = 240;

constexpr int HIST_N = 12;            // barras del histograma
constexpr int GLUCOSE_MIN = 40;       // extremos de la escala (mg/dL)
constexpr int GLUCOSE_MAX = 300;

constexpr int NUM_ZONE_W = 226;       // zona izquierda: número
constexpr int NUM_CENTER_X = 116;
constexpr int NUM_TOP_Y = 40;
constexpr int SIDE_CENTER_X = 274;    // columna derecha: flecha + variación
constexpr int ARROW_Y = 68;
constexpr int DELTA_Y = 112;
constexpr int UNIT_Y = 140;
constexpr int GAUGE_X = 16, GAUGE_Y = 156, GAUGE_W = 288, GAUGE_H = 8;
constexpr int HIST_X = 18, HIST_BASE_Y = 228, HIST_BAR_W = 20, HIST_GAP = 4;
constexpr int HIST_MIN_H = 8, HIST_MAX_H = 52;
constexpr int WIFI_X = 298, WIFI_Y = 34;

// ---------------------------------------------------------------- datos
struct GlucoseView {
  int value = 0;
  int trend = 3;            // 0=sin flecha 1=↓ 2=↘ 3=→ 4=↗ 5=↑
  bool hasDelta = false;
  int delta = 0;
  bool stale = false;       // última lectura obsoleta (fallo de conexión)
  int minutesAgo = 0;
  int rssi = -100;
  int low = 70;
  int high = 180;
  int hist[HIST_N] = {0};   // de más antigua a más reciente
  int histCount = 0;
};

// ---------------------------------------------------------------- utilidades
inline uint16_t scale565(uint16_t c, int pct) {
  int r = ((c >> 11) & 0x1F) * pct / 100;
  int g = ((c >> 5) & 0x3F) * pct / 100;
  int b = (c & 0x1F) * pct / 100;
  return (r << 11) | (g << 5) | b;
}

inline uint16_t stateColor(int v, int low, int high) {
  if (v < low) return RED;
  if (v > high) return AMBER;
  return GREEN;
}

inline int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Carga una fuente solo si no es ya la activa (cargar/descargar tiene coste).
inline void useFont(TFT_eSPI& tft, const uint8_t* font) {
  static const uint8_t* current = nullptr;
  if (current == font) return;
  if (current) tft.unloadFont();
  tft.loadFont(font);
  current = font;
}

inline void text(TFT_eSPI& tft, const uint8_t* font, const String& s, int x, int y,
                 uint8_t datum, uint16_t fg, uint16_t bg = BG) {
  useFont(tft, font);
  tft.setTextColor(fg, bg);
  tft.setTextDatum(datum);
  tft.drawString(s, x, y);
}

// ---------------------------------------------------------------- piezas
// Flecha de tendencia dibujada con líneas anchas (anti-aliasing).
inline void drawTrendArrow(TFT_eSPI& tft, int trend, int cx, int cy, uint16_t col) {
  if (trend <= 0) return;   // sin tendencia fiable: no se dibuja flecha
  float dx = 1.0f, dy = 0.0f;
  switch (trend) {
    case 1: dx = 0.0f;     dy = 1.0f;     break;
    case 2: dx = 0.7071f;  dy = 0.7071f;  break;
    case 4: dx = 0.7071f;  dy = -0.7071f; break;
    case 5: dx = 0.0f;     dy = -1.0f;    break;
    default: break;  // 3 = estable
  }
  const float half = 17.0f, wing = 14.0f, width = 5.0f, ang = 0.75f;
  const float tx = cx - dx * half, ty = cy - dy * half;   // cola
  const float hx = cx + dx * half, hy = cy + dy * half;   // punta
  tft.drawWideLine(tx, ty, hx, hy, width, col, BG);

  const float c = cosf(ang), s = sinf(ang);
  const float bx = -dx, by = -dy;                          // dirección hacia atrás
  tft.drawWideLine(hx, hy, hx + wing * (bx * c - by * s), hy + wing * (bx * s + by * c), width, col, BG);
  tft.drawWideLine(hx, hy, hx + wing * (bx * c + by * s), hy + wing * (-bx * s + by * c), width, col, BG);
}

inline int wifiLevel(int rssi) {
  if (rssi >= -60) return 3;
  if (rssi >= -72) return 2;
  if (rssi >= -82) return 1;
  return 0;
}

inline void drawWifi(TFT_eSPI& tft, int rssi) {
  tft.fillRect(WIFI_X - 26, WIFI_Y - 26, 52, 34, BG);
  const int level = wifiLevel(rssi);
  const int radii[3] = {9, 15, 21};
  for (int i = 0; i < 3; i++) {
    uint16_t col = (i < level) ? MUTED : ICON_OFF;
    tft.drawSmoothArc(WIFI_X, WIFI_Y, radii[i], radii[i] - 3, 142, 218, col, BG, false);
  }
  tft.fillSmoothCircle(WIFI_X, WIFI_Y, 3, level > 0 ? MUTED : ICON_OFF, BG);
}

inline String ageText(bool stale, int minutes) {
  String base;
  if (minutes <= 0) base = "ahora";
  else if (minutes == 1) base = "hace 1 min";
  else base = "hace " + String(minutes) + " min";
  return stale ? ("sin datos · " + base) : base;
}

inline void drawAge(TFT_eSPI& tft, bool stale, int minutes) {
  tft.fillRect(0, 12, 250, 28, BG);
  text(tft, FontSmall, ageText(stale, minutes), 18, 26, ML_DATUM, stale ? AMBER : MUTED);
}

inline void drawNumber(TFT_eSPI& tft, const GlucoseView& v, uint16_t color) {
  tft.fillRect(0, NUM_TOP_Y - 4, NUM_ZONE_W, 96, BG);
  useFont(tft, FontBig);
  String s = String(v.value);
  int w = tft.textWidth(s);
  text(tft, FontBig, s, NUM_CENTER_X - w / 2, NUM_TOP_Y, TL_DATUM, color);
}

inline void drawSide(TFT_eSPI& tft, const GlucoseView& v) {
  tft.fillRect(SIDE_CENTER_X - 46, NUM_TOP_Y + 4, 92, 108, BG);
  drawTrendArrow(tft, v.trend, SIDE_CENTER_X, ARROW_Y, v.stale ? GREY : TEXT);
  if (v.hasDelta) {
    String d = v.delta == 0 ? String("0") : (v.delta > 0 ? "+" + String(v.delta) : String(v.delta));
    text(tft, FontMid, d, SIDE_CENTER_X, DELTA_Y, MC_DATUM, v.stale ? GREY : TEXT);
  }
  text(tft, FontSmall, "mg/dL", SIDE_CENTER_X, UNIT_Y, MC_DATUM, MUTED);
}

inline int scaleX(int value, int x0, int w) {
  return x0 + (clampInt(value, GLUCOSE_MIN, GLUCOSE_MAX) - GLUCOSE_MIN) * w / (GLUCOSE_MAX - GLUCOSE_MIN);
}

// Barra de rango: dónde está la glucosa dentro de 40-300 mg/dL, con muescas
// en los umbrales bajo y alto.
inline void drawGauge(TFT_eSPI& tft, const GlucoseView& v, uint16_t color) {
  tft.fillRect(0, GAUGE_Y - 8, SCREEN_W, GAUGE_H + 16, BG);
  tft.fillSmoothRoundRect(GAUGE_X, GAUGE_Y, GAUGE_W, GAUGE_H, GAUGE_H / 2, TRACK, BG);
  int fillW = scaleX(v.value, GAUGE_X, GAUGE_W) - GAUGE_X;
  if (fillW < GAUGE_H) fillW = GAUGE_H;
  tft.fillSmoothRoundRect(GAUGE_X, GAUGE_Y, fillW, GAUGE_H, GAUGE_H / 2, color, BG);
  tft.fillRect(scaleX(v.low, GAUGE_X, GAUGE_W) - 1, GAUGE_Y, 2, GAUGE_H, BG);
  tft.fillRect(scaleX(v.high, GAUGE_X, GAUGE_W) - 1, GAUGE_Y, 2, GAUGE_H, BG);
}

inline void drawHistory(TFT_eSPI& tft, const GlucoseView& v) {
  tft.fillRect(0, HIST_BASE_Y - HIST_MAX_H - 6, SCREEN_W, HIST_MAX_H + 12, BG);
  if (v.histCount <= 0) return;
  // Las barras se alinean a la derecha: la más reciente siempre en el mismo sitio.
  const int firstSlot = HIST_N - v.histCount;
  for (int i = 0; i < v.histCount; i++) {
    const int val = v.hist[i];
    const int h = HIST_MIN_H + (clampInt(val, GLUCOSE_MIN, GLUCOSE_MAX) - GLUCOSE_MIN) *
                                   (HIST_MAX_H - HIST_MIN_H) / (GLUCOSE_MAX - GLUCOSE_MIN);
    const int x = HIST_X + (firstSlot + i) * (HIST_BAR_W + HIST_GAP);
    uint16_t col = stateColor(val, v.low, v.high);
    if (v.stale) col = GREY;
    else if (i < v.histCount - 1) col = scale565(col, 70);  // la más reciente, a pleno color
    tft.fillSmoothRoundRect(x, HIST_BASE_Y - h, HIST_BAR_W, h, 3, col, BG);
  }
}

// ---------------------------------------------------------------- pantallas
// Pantalla principal. `clearAll` = true al venir de otra pantalla.
inline void drawGlucose(TFT_eSPI& tft, const GlucoseView& v, bool clearAll) {
  if (clearAll) tft.fillScreen(BG);
  const uint16_t color = v.stale ? GREY : stateColor(v.value, v.low, v.high);
  drawAge(tft, v.stale, v.minutesAgo);
  drawWifi(tft, v.rssi);
  drawNumber(tft, v, color);
  drawSide(tft, v);
  drawGauge(tft, v, color);
  drawHistory(tft, v);
}

// Mensaje centrado (arranque, conectando, errores…).
inline void drawMessage(TFT_eSPI& tft, const String& title, const String& detail = "",
                        uint16_t accent = TEXT) {
  tft.fillScreen(BG);
  text(tft, FontMid, title, SCREEN_W / 2, SCREEN_H / 2 - 14, MC_DATUM, accent);
  if (detail.length()) {
    text(tft, FontSmall, detail, SCREEN_W / 2, SCREEN_H / 2 + 22, MC_DATUM, MUTED);
  }
}

// Pantalla del portal de configuración.
inline void drawSetup(TFT_eSPI& tft, const String& ssid) {
  tft.fillScreen(BG);
  text(tft, FontMid, "Configuración", 20, 30, ML_DATUM, TEXT);

  const int rows[2] = {92, 164};
  const char* hints[2] = {"Conéctate a esta WiFi", "Abre en el navegador"};
  String values[2] = {ssid, "192.168.4.1"};
  for (int i = 0; i < 2; i++) {
    const int y = rows[i];
    text(tft, FontMid, String(i + 1), 22, y + 4, ML_DATUM, GREEN);
    text(tft, FontSmall, hints[i], 56, y - 10, ML_DATUM, MUTED);
    text(tft, FontMid, values[i], 56, y + 19, ML_DATUM, TEXT);
  }
  text(tft, FontSmall, "Esperando configuración…", SCREEN_W / 2, SCREEN_H - 18, MC_DATUM, GREY);
}

}  // namespace ui
