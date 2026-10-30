/*
 * Funciones de análisis puras (sin WiFi ni pantalla) para las respuestas de
 * LibreLinkUp y Dexcom Share. Solo dependen de ArduinoJson y de la librería
 * estándar, así que se pueden probar en el ordenador: ver tools/test_parse.cpp.
 */
#pragma once

#include <ArduinoJson.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace parse {

// ---------------------------------------------------------------- fechas
// Días desde 1970-01-01 de una fecha del calendario gregoriano.
inline int64_t daysFromCivil(int y, int m, int d) {
  y -= (m <= 2) ? 1 : 0;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const int yoe = y - era * 400;
  const int mp = (m > 2) ? (m - 3) : (m + 9);
  const int doy = (153 * mp + 2) / 5 + d - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + doe - 719468;
}

// FactoryTimestamp de LibreLinkUp (UTC): "10/30/2026 2:45:12 AM" -> segundos Unix.
// Devuelve 0 si no se puede interpretar.
inline int64_t libreEpoch(const char* s) {
  if (!s) return 0;
  int mo, d, y, h, mi, sec;
  char ap[4] = {0};
  int n = sscanf(s, "%d/%d/%d %d:%d:%d %3s", &mo, &d, &y, &h, &mi, &sec, ap);
  if (n < 6) return 0;
  if (mo < 1 || mo > 12 || d < 1 || d > 31 || y < 2000 || h < 0 || h > 23 ||
      mi < 0 || mi > 59 || sec < 0 || sec > 60) return 0;
  if (n >= 7) {
    if ((ap[0] == 'P' || ap[0] == 'p') && h < 12) h += 12;
    if ((ap[0] == 'A' || ap[0] == 'a') && h == 12) h = 0;
  }
  return daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + sec;
}

// Fecha de Dexcom: "Date(1691455258000)" o "Date(1691455258000-0400)" -> segundos Unix.
// El número es siempre milisegundos UTC; el sufijo es solo la zona horaria de visualización.
inline int64_t dexcomEpoch(const char* s) {
  if (!s) return 0;
  const char* p = strchr(s, '(');
  if (!p) return 0;
  char* end = nullptr;
  long long ms = strtoll(p + 1, &end, 10);
  if (end == p + 1 || ms <= 0) return 0;
  return ms / 1000;
}

// ---------------------------------------------------------------- tendencia
// Escala de la interfaz: 0 = sin flecha, 1 = baja rápido ... 3 = estable ... 5 = sube rápido.
inline int dexcomTrendToUi(JsonVariantConst v) {
  int t = -1;
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    static const char* names[] = {"DoubleUp", "SingleUp", "FortyFiveUp", "Flat",
                                  "FortyFiveDown", "SingleDown", "DoubleDown"};
    for (int i = 0; i < 7; i++) if (s && strcmp(s, names[i]) == 0) t = i + 1;
  } else if (v.is<int>()) {
    t = v.as<int>();
  }
  switch (t) {
    case 1: case 2: return 5;  // sube rápido
    case 3: return 4;          // sube
    case 4: return 3;          // estable
    case 5: return 2;          // baja
    case 6: case 7: return 1;  // baja rápido
    default: return 0;         // sin tendencia fiable
  }
}

// ---------------------------------------------------------------- Dexcom
struct Sample {
  int value;
  int trend;       // escala de la interfaz
  int64_t epoch;   // segundos Unix (0 = desconocido)
};

// Lista que devuelve Publisher/ReadPublisherLatestGlucoseValues (la más reciente primero).
// Devuelve nº de lecturas válidas (0 = lista vacía) o -1 si el JSON no es válido.
inline int dexcomParseReadings(const char* json, Sample* out, int maxCount) {
  JsonDocument filter;
  filter[0]["Value"] = true;
  filter[0]["Trend"] = true;
  filter[0]["ST"] = true;
  filter[0]["DT"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, json, DeserializationOption::Filter(filter))) return -1;
  if (!doc.is<JsonArray>()) return -1;
  int n = 0;
  for (JsonObjectConst o : doc.as<JsonArrayConst>()) {
    if (n >= maxCount) break;
    int value = o["Value"] | 0;
    if (value <= 0) continue;
    int64_t epoch = dexcomEpoch(o["ST"].as<const char*>());
    if (epoch == 0) epoch = dexcomEpoch(o["DT"].as<const char*>());
    out[n].value = value;
    out[n].trend = dexcomTrendToUi(o["Trend"]);
    out[n].epoch = epoch;
    n++;
  }
  return n;
}

// Las respuestas de Authenticate / Login son un UUID entre comillas. Lo copia a `out`.
// Falla si no es un UUID o si es el UUID "vacío" que devuelve Dexcom ante un error.
inline bool unquoteUuid(const char* body, char* out, size_t outSize) {
  if (!body || outSize < 37) return false;
  const char* a = strchr(body, '"');
  if (!a) return false;
  const char* b = strchr(a + 1, '"');
  if (!b || (b - a - 1) != 36) return false;
  memcpy(out, a + 1, 36);
  out[36] = 0;
  bool allZero = true;
  for (int i = 0; i < 36; i++) {
    const char c = out[i];
    const bool dashPos = (i == 8 || i == 13 || i == 18 || i == 23);
    if (dashPos ? c != '-' : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    if (!dashPos && c != '0') allZero = false;
  }
  return !allZero;
}

enum DexcomError { DEXERR_OTHER = 0, DEXERR_AUTH, DEXERR_LOCKED, DEXERR_SESSION };

// Clasifica el cuerpo de error de Dexcom ({"Code":"...","Message":"..."}).
inline DexcomError dexcomClassifyError(const char* json) {
  JsonDocument doc;
  if (!json || deserializeJson(doc, json)) return DEXERR_OTHER;
  const char* code = doc["Code"].as<const char*>();
  const char* msg = doc["Message"].as<const char*>();
  if (!code) return DEXERR_OTHER;
  if (!strcmp(code, "SessionIdNotFound") || !strcmp(code, "SessionNotValid")) return DEXERR_SESSION;
  if (!strcmp(code, "SSO_AuthenticateMaxAttemptsExceeded")) return DEXERR_LOCKED;
  if (!strcmp(code, "AccountPasswordInvalid")) return DEXERR_AUTH;
  if (!strcmp(code, "SSO_InternalError") && msg && strstr(msg, "Cannot Authenticate")) return DEXERR_AUTH;
  return DEXERR_OTHER;
}

}  // namespace parse
