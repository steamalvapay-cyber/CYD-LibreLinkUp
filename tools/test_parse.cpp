// Pruebas de src/parse.h en el ordenador (no hace falta la placa).
//
//   git clone --depth 1 --branch v7.4.2 https://github.com/bblanchon/ArduinoJson.git /tmp/ArduinoJson
//   g++ -std=c++17 -Wall -Wextra -I/tmp/ArduinoJson/src -Isrc tools/test_parse.cpp -o /tmp/test_parse && /tmp/test_parse
#include <cstdio>
#include "parse.h"

static int failures = 0;
#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) { printf("FALLA l.%d: %s\n", __LINE__, #cond); failures++; } \
  } while (0)

int main() {
  // --- fechas ---
  CHECK(parse::daysFromCivil(1970, 1, 1) == 0);
  CHECK(parse::daysFromCivil(2000, 3, 1) == 11017);
  CHECK(parse::libreEpoch("1/1/2024 12:00:00 AM") == 1704067200);   // medianoche UTC
  CHECK(parse::libreEpoch("1/1/2024 12:00:00 PM") == 1704110400);   // mediodía
  CHECK(parse::libreEpoch("10/30/2026 2:45:12 AM") == 1793328312);
  CHECK(parse::libreEpoch("10/30/2026 11:05:00 PM") == 1793401500);
  CHECK(parse::libreEpoch("2/29/2024 13:30:00") == 1709213400);     // formato 24 h
  CHECK(parse::libreEpoch("basura") == 0);
  CHECK(parse::libreEpoch(nullptr) == 0);
  CHECK(parse::dexcomEpoch("Date(1691455258000)") == 1691455258);
  CHECK(parse::dexcomEpoch("Date(1691455258000-0400)") == 1691455258);
  CHECK(parse::dexcomEpoch("nada") == 0);
  CHECK(parse::dexcomEpoch(nullptr) == 0);

  // --- lista de lecturas Dexcom (tendencia como texto y como número) ---
  const char* list =
      "[{\"WT\":\"Date(1793328600000)\",\"ST\":\"Date(1793328600000)\",\"DT\":\"Date(1793328600000+0100)\",\"Value\":128,\"Trend\":\"Flat\"},"
      " {\"WT\":\"Date(1793328300000)\",\"ST\":\"Date(1793328300000)\",\"DT\":\"Date(1793328300000+0100)\",\"Value\":125,\"Trend\":\"FortyFiveUp\"},"
      " {\"ST\":\"Date(1793328000000)\",\"Value\":0,\"Trend\":\"Flat\"},"
      " {\"DT\":\"Date(1793327700000+0100)\",\"Value\":118,\"Trend\":6}]";
  parse::Sample s[8];
  int n = parse::dexcomParseReadings(list, s, 8);
  CHECK(n == 3);                         // se descarta la de valor 0
  CHECK(s[0].value == 128 && s[0].trend == 3 && s[0].epoch == 1793328600);
  CHECK(s[1].value == 125 && s[1].trend == 4 && s[1].epoch == 1793328300);
  CHECK(s[2].value == 118 && s[2].trend == 1 && s[2].epoch == 1793327700);  // usa DT si falta ST
  CHECK(parse::dexcomParseReadings(list, s, 2) == 2);                       // respeta el máximo
  CHECK(parse::dexcomParseReadings("[]", s, 8) == 0);
  CHECK(parse::dexcomParseReadings("{\"Code\":\"x\"}", s, 8) == -1);
  CHECK(parse::dexcomParseReadings("no es json", s, 8) == -1);

  // --- tendencias ---
  const char* names[] = {"DoubleUp", "SingleUp", "FortyFiveUp", "Flat", "FortyFiveDown", "SingleDown", "DoubleDown", "None", "NotComputable", "RateOutOfRange"};
  const int expect[] = {5, 5, 4, 3, 2, 1, 1, 0, 0, 0};
  for (int i = 0; i < 10; i++) {
    char buf[96];
    snprintf(buf, sizeof buf, "[{\"Value\":100,\"Trend\":\"%s\"}]", names[i]);
    parse::Sample t[1];
    CHECK(parse::dexcomParseReadings(buf, t, 1) == 1 && t[0].trend == expect[i]);
  }

  // --- UUID entre comillas ---
  char id[40];
  CHECK(parse::unquoteUuid("\"12345678-90ab-cdef-1234-567890abcdef\"", id, sizeof id) && !strcmp(id, "12345678-90ab-cdef-1234-567890abcdef"));
  CHECK(!parse::unquoteUuid("\"00000000-0000-0000-0000-000000000000\"", id, sizeof id));  // UUID vacío = error
  CHECK(!parse::unquoteUuid("\"corto\"", id, sizeof id));
  CHECK(!parse::unquoteUuid("{\"Code\":\"AccountPasswordInvalid\"}", id, sizeof id));
  CHECK(!parse::unquoteUuid("\"12345678-90ab-cdef-1234-567890abcdeg\"", id, sizeof id));
  CHECK(!parse::unquoteUuid("", id, sizeof id));

  // --- clasificación de errores ---
  CHECK(parse::dexcomClassifyError("{\"Code\":\"AccountPasswordInvalid\",\"Message\":\"x\"}") == parse::DEXERR_AUTH);
  CHECK(parse::dexcomClassifyError("{\"Code\":\"SSO_InternalError\",\"Message\":\"Cannot Authenticate by AccountName\"}") == parse::DEXERR_AUTH);
  CHECK(parse::dexcomClassifyError("{\"Code\":\"SSO_InternalError\",\"Message\":\"otra cosa\"}") == parse::DEXERR_OTHER);
  CHECK(parse::dexcomClassifyError("{\"Code\":\"SSO_AuthenticateMaxAttemptsExceeded\"}") == parse::DEXERR_LOCKED);
  CHECK(parse::dexcomClassifyError("{\"Code\":\"SessionNotValid\"}") == parse::DEXERR_SESSION);
  CHECK(parse::dexcomClassifyError("{\"Code\":\"SessionIdNotFound\"}") == parse::DEXERR_SESSION);
  CHECK(parse::dexcomClassifyError("<html>502</html>") == parse::DEXERR_OTHER);
  CHECK(parse::dexcomClassifyError("") == parse::DEXERR_OTHER);

  printf(failures ? "\n%d comprobaciones fallidas\n" : "Todas las pruebas correctas\n", failures);
  return failures ? 1 : 0;
}
