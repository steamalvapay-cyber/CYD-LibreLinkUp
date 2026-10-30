/*
 * CYD (ESP32-2432S028R) · Glucosa en pantalla — v5 (LibreLinkUp, Dexcom y alarma)
 * -----------------------------------------------------------------------------
 * Basado en mulscully/CYD_LibreLinkUp_glucose_ConfigScreen (MIT).
 *
 * Qué hace:
 *   - Se conecta directamente por WiFi a LibreLinkUp (FreeStyle Libre) o a
 *     Dexcom Share (Dexcom G7 / G6 / ONE+), sin hub ni app.
 *   - Muestra el valor actual, la tendencia, la variación de los últimos ~5 min,
 *     la posición dentro del rango y las últimas lecturas.
 *   - Alarma sonora opcional: si la glucosa sale de los umbrales suena 5 s, y vuelve a
 *     sonar cada 30 min mientras siga fuera. Dentro del rango nunca suena.
 *   - No lleva credenciales en el código: en el primer arranque abre un portal
 *     WiFi ("CYD-Glucosa") donde se elige el servicio y se introducen WiFi,
 *     usuario y umbrales. Todo se guarda en la flash.
 *
 * Para reconfigurar: mantén pulsado el botón BOOT ~3 s con el equipo ya funcionando
 * (nunca al enchufarlo: GPIO0 es un pin de arranque del ESP32).
 *
 * AVISO: usa APIs no oficiales. No es un producto sanitario ni sustituye al lector
 * o a la app oficial: no tomes decisiones de tratamiento solo con esta pantalla.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <time.h>
#include "mbedtls/md.h"

#include "parse.h"
#include "beeper.h"
#include "ui.h"

// Cada cuánto se consulta la API (ms).
const unsigned long REFRESH_INTERVAL_MS = 60000;

// Si la lectura del sensor es más antigua que esto, se marca como "sin datos".
const int STALE_AFTER_MIN = 15;

// Tras un fallo de usuario/contraseña se espera esto antes de volver a probar, y tras
// MAX_AUTH_FAILURES fallos seguidos se deja de intentar (Dexcom bloquea la cuenta si
// se insiste con una contraseña incorrecta).
const unsigned long AUTH_RETRY_MS = 5UL * 60UL * 1000UL;
const int MAX_AUTH_FAILURES = 3;

// Cabeceras que exige LibreLinkUp. Si el login empieza a fallar tras una actualización
// de Abbott, lo primero que hay que revisar es esta versión.
const char* LLU_PRODUCT = "llu.android";
const char* LLU_VERSION = "4.16.0";

// Dexcom Share (API no oficial, la misma que usan Home Assistant y pydexcom).
const char* DEX_APP_ID = "d89443d2-327c-4a6f-89e5-496bbb0317db";     // EE. UU. y resto del mundo
const char* DEX_APP_ID_JP = "d8665ade-9673-4e27-9ff6-92db4ce13d13";  // Japón

// Variación mostrada en LibreLinkUp = valor actual - valor de hace DELTA_SAMPLES consultas (≈ minutos).
const int DELTA_SAMPLES = 5;

// El botón BOOT de la CYD está en GPIO0.
const int BOOT_BUTTON_PIN = 0;

// Altavoz. La CYD lleva un amplificador y un conector de 2 pines (JST 1.25 mm) para el
// altavoz, controlado desde GPIO26. Si montas un zumbador por cable en otro pin libre
// (GPIO22 o GPIO27), cambia el número con -D ALARM_PIN=27 en platformio.ini.
#ifndef ALARM_PIN
#define ALARM_PIN 26
#endif
const int ALARM_PWM_CHANNEL = 4;   // canal PWM del ESP32 (el 0 y el 1 los usa WiFi/otros)
const int ALARM_FREQ_HZ = 2000;    // tono del pitido
const int ALARM_DUTY = 128;        // 128 = volumen máximo (50 %); más bajo = más suave

TFT_eSPI tft = TFT_eSPI();
Preferences prefs;

// ---------------- Configuración (flash / portal) ----------------
enum Provider { PROV_LIBRE = 0, PROV_DEXCOM = 1 };
Provider provider = PROV_LIBRE;
String dexRegion = "ous";   // "ous" (Europa y resto), "us" o "jp"
String accountUser;         // email (LibreLinkUp) o usuario/email/teléfono (Dexcom)
String accountPass;
int lowThreshold = 70;
int highThreshold = 180;
bool alarmEnabled = true;   // alarma sonora (altavoz)

// ---------------- Estado de la sesión ----------------
String baseUrl = "https://api.libreview.io";   // LibreLinkUp (cambia según la región)
String authToken = "";
String accountIdHash = "";
String patientId = "";
String selfUserId = "";
String dexSession = "";
bool loggedIn = false;
String lastError = "";          // último error legible, se enseña en pantalla
bool lastFailWasAuth = false;   // el último fallo fue de usuario/contraseña
int authFailures = 0;
bool authBlocked = false;
unsigned long nextAuthAllowed = 0;

// Una lectura completa, venga del servicio que venga.
struct Reading {
  int value = 0;
  int trend = 3;
  int64_t epoch = 0;        // momento de la medida (segundos Unix, 0 = desconocido)
  bool hasDelta = false;    // el servicio ya da la variación (Dexcom)
  int delta = 0;
  bool hasHist = false;     // el servicio ya da el historial (Dexcom)
  int hist[ui::HIST_N] = {0};
  int histCount = 0;
};

bool fetchGlucoseViaGraph(const String& id, Reading& r);
void alarmReset();   // definida más abajo, en la sección ALARMA SONORA

// ============================================================
// PORTAL DE CONFIGURACIÓN
// ============================================================

void configModeCallback(WiFiManager* myWiFiManager) {
  ui::drawSetup(tft, myWiFiManager->getConfigPortalSSID());
}

void loadSavedConfig() {
  prefs.begin("glucose-cfg", /*readOnly=*/true);
  accountUser = prefs.getString("email", "");
  accountPass = prefs.getString("password", "");
  lowThreshold = prefs.getInt("low", 70);
  highThreshold = prefs.getInt("high", 180);
  alarmEnabled = prefs.getInt("alarm", 1) != 0;
  provider = prefs.getString("provider", "libre") == "dexcom" ? PROV_DEXCOM : PROV_LIBRE;
  dexRegion = prefs.getString("region", "ous");
  if (dexRegion != "us" && dexRegion != "jp") dexRegion = "ous";
  prefs.end();
}

void saveConfig() {
  prefs.begin("glucose-cfg", /*readOnly=*/false);
  prefs.putString("email", accountUser);
  prefs.putString("password", accountPass);
  prefs.putInt("low", lowThreshold);
  prefs.putInt("high", highThreshold);
  prefs.putInt("alarm", alarmEnabled ? 1 : 0);
  prefs.putString("provider", provider == PROV_DEXCOM ? "dexcom" : "libre");
  prefs.putString("region", dexRegion);
  prefs.end();
}

// Olvida cualquier sesión abierta (tras reconfigurar, o al cambiar de servicio).
void resetSession() {
  baseUrl = "https://api.libreview.io";
  authToken = "";
  accountIdHash = "";
  patientId = "";
  selfUserId = "";
  dexSession = "";
  loggedIn = false;
  lastError = "";
  lastFailWasAuth = false;
  authFailures = 0;
  authBlocked = false;
  nextAuthAllowed = 0;
}

// Estilo oscuro y minimalista para la página del portal (se ve en el móvil).
const char* PORTAL_HEAD =
  "<style>"
  "body{background:#0a0c0e;color:#ecf0f4;font-family:-apple-system,Segoe UI,Roboto,sans-serif}"
  "h1,h3{color:#22e07a;font-weight:600}"
  "button,input[type=submit]{background:#22e07a!important;color:#0a0c0e!important;border:0;"
  "border-radius:12px!important;font-weight:600}"
  "input,select{background:#14181c!important;color:#ecf0f4!important;border:1px solid #262c33!important;"
  "border-radius:10px!important;width:100%;padding:8px;box-sizing:border-box}"
  "a{color:#22e07a}"
  "</style>";

void runWiFiManagerSetup(bool forcePortal) {
  loadSavedConfig();

  static char emailBuf[64];
  static char passwordBuf[64];
  static char lowBuf[6];
  static char highBuf[6];
  accountUser.toCharArray(emailBuf, sizeof(emailBuf));
  accountPass.toCharArray(passwordBuf, sizeof(passwordBuf));
  snprintf(lowBuf, sizeof(lowBuf), "%d", lowThreshold);
  snprintf(highBuf, sizeof(highBuf), "%d", highThreshold);

  // Los desplegables son HTML a medida: WiFiManager guarda el puntero, así que
  // los textos tienen que vivir en memoria estática.
  static char providerHtml[420];
  static char regionHtml[420];
  static char alarmHtml[360];
  snprintf(providerHtml, sizeof(providerHtml),
           "<label for='provider'>Servicio</label>"
           "<select name='provider' id='provider'>"
           "<option value='libre'%s>LibreLinkUp (FreeStyle Libre)</option>"
           "<option value='dexcom'%s>Dexcom (G7, G6, ONE+)</option>"
           "</select><br/>",
           provider == PROV_LIBRE ? " selected" : "", provider == PROV_DEXCOM ? " selected" : "");
  snprintf(regionHtml, sizeof(regionHtml),
           "<br/><label for='region'>Región de Dexcom (solo si usas Dexcom)</label>"
           "<select name='region' id='region'>"
           "<option value='ous'%s>Europa y resto del mundo</option>"
           "<option value='us'%s>EE. UU.</option>"
           "<option value='jp'%s>Japón</option>"
           "</select><br/>",
           dexRegion == "ous" ? " selected" : "", dexRegion == "us" ? " selected" : "", dexRegion == "jp" ? " selected" : "");

  snprintf(alarmHtml, sizeof(alarmHtml),
           "<br/><label for='alarm'>Alarma sonora (altavoz)</label>"
           "<select name='alarm' id='alarm'>"
           "<option value='1'%s>Activada: pita al salir de los umbrales</option>"
           "<option value='0'%s>Desactivada</option>"
           "</select><br/>",
           alarmEnabled ? " selected" : "", alarmEnabled ? "" : " selected");

  WiFiManagerParameter custom_hint(
    "<p style='font-size:.85em;color:#8a94a0'>"
    "<b>LibreLinkUp:</b> email y contraseña de la cuenta de LibreLinkUp (seguidor).<br/>"
    "<b>Dexcom:</b> usuario y contraseña de la cuenta de <u>quien lleva el sensor</u> "
    "(no la de Dexcom Follow), con Share activado.</p>");
  WiFiManagerParameter custom_provider(providerHtml);
  WiFiManagerParameter custom_email("email", "Email o usuario de la cuenta", emailBuf, 63);
  WiFiManagerParameter custom_password("password", "Contraseña", passwordBuf, 63, " type='password'");
  WiFiManagerParameter custom_region(regionHtml);
  WiFiManagerParameter custom_low("low", "Umbral bajo (mg/dL)", lowBuf, 5);
  WiFiManagerParameter custom_high("high", "Umbral alto (mg/dL)", highBuf, 5);
  WiFiManagerParameter custom_alarm(alarmHtml);

  WiFiManager wm;
  wm.setTitle("Glucosa CYD");
  wm.setCustomHeadElement(PORTAL_HEAD);
  wm.setAPCallback(configModeCallback);
  wm.addParameter(&custom_hint);
  wm.addParameter(&custom_provider);
  wm.addParameter(&custom_email);
  wm.addParameter(&custom_password);
  wm.addParameter(&custom_region);
  wm.addParameter(&custom_low);
  wm.addParameter(&custom_high);
  wm.addParameter(&custom_alarm);

  // Los desplegables no los gestiona WiFiManager: se leen del formulario al guardar.
  wm.setSaveParamsCallback([&]() {
    if (wm.server->hasArg("provider")) {
      provider = wm.server->arg("provider") == "dexcom" ? PROV_DEXCOM : PROV_LIBRE;
    }
    if (wm.server->hasArg("alarm")) {
      alarmEnabled = wm.server->arg("alarm") != "0";
    }
    if (wm.server->hasArg("region")) {
      String r = wm.server->arg("region");
      dexRegion = (r == "us" || r == "jp") ? r : String("ous");
    }
  });

  // Si nadie configura en 5 min, se reinicia (evita quedarse en modo AP para siempre).
  wm.setConfigPortalTimeout(300);

  bool connected;
  if (forcePortal) {
    connected = wm.startConfigPortal("CYD-Glucosa");
  } else {
    connected = wm.autoConnect("CYD-Glucosa");
  }

  if (!connected) {
    Serial.println("WiFi setup timed out or failed — restarting to try again.");
    ESP.restart();
  }

  accountUser = String(custom_email.getValue());
  accountPass = String(custom_password.getValue());
  accountUser.trim();
  lowThreshold = atoi(custom_low.getValue());
  highThreshold = atoi(custom_high.getValue());
  if (lowThreshold <= 0) lowThreshold = 70;
  if (highThreshold <= 0) highThreshold = 180;
  if (highThreshold <= lowThreshold) highThreshold = lowThreshold + 1;

  saveConfig();
  resetSession();

  Serial.println("WiFi connected: " + WiFi.localIP().toString());
}

// ============================================================
// HORA (para saber qué antigüedad tiene de verdad la lectura)
// ============================================================

void startTimeSync() {
  configTime(0, 0, "pool.ntp.org", "time.google.com");
}

bool timeSynced() {
  return time(nullptr) > 1700000000;
}

// ============================================================
// LIBRELINKUP
// ============================================================

String sha256hex(const String& input) {
  byte shaResult[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
  mbedtls_md_starts(&ctx);
  mbedtls_md_update(&ctx, (const unsigned char*)input.c_str(), input.length());
  mbedtls_md_finish(&ctx, shaResult);
  mbedtls_md_free(&ctx);

  String hash = "";
  for (int i = 0; i < 32; i++) {
    char buf[3];
    sprintf(buf, "%02x", shaResult[i]);
    hash += buf;
  }
  return hash;
}

void addLLUHeaders(HTTPClient& http) {
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  http.addHeader("product", LLU_PRODUCT);
  http.addHeader("version", LLU_VERSION);
  if (authToken.length() > 0) {
    http.addHeader("Authorization", "Bearer " + authToken);
  }
  if (accountIdHash.length() > 0) {
    http.addHeader("Account-Id", accountIdHash);
  }
}

bool libreLogin() {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, baseUrl + "/llu/auth/login");
  addLLUHeaders(http);

  JsonDocument body;
  body["email"] = accountUser;
  body["password"] = accountPass;
  String payload;
  serializeJson(body, payload);

  int code = http.POST(payload);
  if (code != 200) {
    Serial.printf("Login failed, HTTP code: %d\n", code);
    lastError = code > 0 ? "Login: error HTTP " + String(code) : String("Sin conexión con LibreLinkUp");
    http.end();
    return false;
  }

  String response = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, response);
  if (err) {
    Serial.println("Login JSON parse failed");
    lastError = "Login: respuesta no válida";
    return false;
  }

  bool redirect = doc["data"]["redirect"] | false;
  if (redirect) {
    const char* region = doc["data"]["region"];
    if (region) {
      baseUrl = "https://api-" + String(region) + ".libreview.io";
      Serial.println("Redirected to region: " + baseUrl);
      return libreLogin();
    }
  }

  const char* token = doc["data"]["authTicket"]["token"];
  if (!token) {
    Serial.println("No auth token in response");
    lastError = "Revisa email y contraseña";
    lastFailWasAuth = true;
    return false;
  }
  authToken = String(token);

  const char* userId = doc["data"]["user"]["id"];
  if (userId) {
    selfUserId = String(userId);
    accountIdHash = sha256hex(selfUserId);
  } else {
    Serial.println("WARNING: no user id in login response, account-id header will be missing");
  }

  loggedIn = true;
  lastError = "";
  Serial.println("Login successful.");
  return true;
}

bool libreFetch(Reading& r) {
  if (!loggedIn) {
    if (!libreLogin()) return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, baseUrl + "/llu/connections");
  addLLUHeaders(http);

  int code = http.GET();
  if (code == 401) {
    Serial.println("Token expired, re-logging in...");
    loggedIn = false;
    http.end();
    if (!libreLogin()) return false;
    return libreFetch(r);
  }
  if (code != 200) {
    Serial.printf("Connections fetch failed, HTTP code: %d\n", code);
    lastError = code > 0 ? "Lectura: error HTTP " + String(code) : String("Sin conexión con LibreLinkUp");
    http.end();
    return false;
  }

  String response = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, response);
  if (err) {
    Serial.println("Connections JSON parse failed");
    lastError = "Lectura: respuesta no válida";
    return false;
  }

  JsonArray connections = doc["data"].as<JsonArray>();
  if (connections.size() == 0) {
    Serial.println("No connections found — trying self-monitoring (patient role) fallback...");
    patientId = selfUserId;
    return fetchGlucoseViaGraph(selfUserId, r);
  }

  JsonObject chosen = connections[0];
  patientId = chosen["patientId"].as<String>();
  JsonObject gm = chosen["glucoseMeasurement"];
  if (gm.isNull()) {
    Serial.println("No glucoseMeasurement in connection data");
    lastError = "Sin lecturas del sensor";
    return false;
  }

  r.value = gm["ValueInMgPerDl"] | gm["Value"].as<int>();
  r.trend = gm["TrendArrow"] | 3;
  r.epoch = parse::libreEpoch(gm["FactoryTimestamp"].as<const char*>());

  lastError = "";
  return true;
}

bool fetchGlucoseViaGraph(const String& id, Reading& r) {
  if (id.length() == 0) {
    Serial.println("No self user id available for graph fallback");
    lastError = "Sin conexiones en LibreLinkUp";
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, baseUrl + "/llu/connections/" + id + "/graph");
  addLLUHeaders(http);

  int code = http.GET();
  if (code != 200) {
    Serial.printf("Graph fallback fetch failed, HTTP code: %d\n", code);
    lastError = code > 0 ? "Lectura: error HTTP " + String(code) : String("Sin conexión con LibreLinkUp");
    http.end();
    return false;
  }

  String response = http.getString();
  http.end();

  // Solo nos interesa la medición actual: el filtro evita cargar todo el gráfico en RAM.
  JsonDocument filter;
  filter["data"]["connection"]["glucoseMeasurement"] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, response, DeserializationOption::Filter(filter));
  if (err) {
    Serial.println("Graph fallback JSON parse failed");
    lastError = "Lectura: respuesta no válida";
    return false;
  }

  JsonObject gm = doc["data"]["connection"]["glucoseMeasurement"];
  if (gm.isNull()) {
    Serial.println("No glucoseMeasurement in graph fallback response");
    lastError = "Sin lecturas del sensor";
    return false;
  }

  r.value = gm["ValueInMgPerDl"] | gm["Value"].as<int>();
  r.trend = gm["TrendArrow"] | 3;
  r.epoch = parse::libreEpoch(gm["FactoryTimestamp"].as<const char*>());

  lastError = "";
  return true;
}

// Historial para las barras de LibreLinkUp: últimas lecturas del gráfico (≈ una cada
// 15 min), de la más antigua a la más reciente. Es opcional: si falla, no se dibujan
// barras. Devuelve cuántos valores ha guardado en `out`.
int libreHistory(const String& id, int* out, int maxCount) {
  if (id.length() == 0 || maxCount <= 0) return 0;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, baseUrl + "/llu/connections/" + id + "/graph");
  addLLUHeaders(http);

  int code = http.GET();
  if (code != 200) {
    Serial.printf("History fetch failed, HTTP code: %d\n", code);
    http.end();
    return 0;
  }

  String response = http.getString();
  http.end();

  JsonDocument filter;
  filter["data"]["graphData"][0]["ValueInMgPerDl"] = true;
  filter["data"]["graphData"][0]["Value"] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, response, DeserializationOption::Filter(filter));
  if (err) {
    Serial.println("History JSON parse failed");
    return 0;
  }

  JsonArray points = doc["data"]["graphData"].as<JsonArray>();
  int total = (int)points.size();
  int count = total < maxCount ? total : maxCount;
  for (int i = 0; i < count; i++) {
    JsonObject p = points[total - count + i];
    out[i] = p["ValueInMgPerDl"] | p["Value"].as<int>();
  }
  return count;
}

// ============================================================
// DEXCOM SHARE
// ============================================================

String dexBaseUrl() {
  if (dexRegion == "us") return "https://share2.dexcom.com/ShareWebServices/Services/";
  if (dexRegion == "jp") return "https://share.dexcom.jp/ShareWebServices/Services/";
  return "https://shareous1.dexcom.com/ShareWebServices/Services/";
}

const char* dexAppId() {
  return dexRegion == "jp" ? DEX_APP_ID_JP : DEX_APP_ID;
}

// POST JSON a Dexcom. Devuelve el código HTTP (<= 0 = fallo de red) y deja el cuerpo en `body`.
int dexPost(const String& endpoint, const String& query, const String& payload, String& body) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(15000);
  http.begin(client, dexBaseUrl() + endpoint + query);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");

  int code = http.POST(payload);
  body = (code > 0) ? http.getString() : String("");
  http.end();
  return code;
}

void dexSetLoginError(int code, const String& body) {
  if (code <= 0) {
    lastError = "Sin conexión con Dexcom";
    return;
  }
  switch (parse::dexcomClassifyError(body.c_str())) {
    case parse::DEXERR_AUTH:
      lastError = "Usuario o contraseña incorrectos";
      lastFailWasAuth = true;
      break;
    case parse::DEXERR_LOCKED:
      lastError = "Cuenta bloqueada: espera un rato";
      lastFailWasAuth = true;
      authFailures = MAX_AUTH_FAILURES - 1;   // este fallo la deja bloqueada
      break;
    default:
      lastError = "Dexcom: error HTTP " + String(code);
      break;
  }
}

bool dexLogin() {
  // Paso 1: usuario + contraseña -> id de la cuenta.
  JsonDocument d1;
  d1["accountName"] = accountUser;
  d1["password"] = accountPass;
  d1["applicationId"] = dexAppId();
  String payload, body;
  serializeJson(d1, payload);
  int code = dexPost("General/AuthenticatePublisherAccount", "", payload, body);

  char accountId[40];
  if (code != 200 || !parse::unquoteUuid(body.c_str(), accountId, sizeof(accountId))) {
    if (code == 200) {   // Dexcom contesta 200 con un UUID vacío cuando los datos no valen
      lastError = "Usuario o contraseña incorrectos";
      lastFailWasAuth = true;
    } else {
      dexSetLoginError(code, body);
    }
    Serial.printf("Dexcom authenticate failed, HTTP code: %d\n", code);
    return false;
  }

  // Paso 2: id de la cuenta + contraseña -> sesión.
  JsonDocument d2;
  d2["accountId"] = accountId;
  d2["password"] = accountPass;
  d2["applicationId"] = dexAppId();
  payload = "";
  serializeJson(d2, payload);
  code = dexPost("General/LoginPublisherAccountById", "", payload, body);

  char sessionId[40];
  if (code != 200 || !parse::unquoteUuid(body.c_str(), sessionId, sizeof(sessionId))) {
    if (code == 200) {
      lastError = "Usuario o contraseña incorrectos";
      lastFailWasAuth = true;
    } else {
      dexSetLoginError(code, body);
    }
    Serial.printf("Dexcom login failed, HTTP code: %d\n", code);
    return false;
  }

  dexSession = String(sessionId);
  loggedIn = true;
  lastError = "";
  Serial.println("Dexcom login successful.");
  return true;
}

// Una sola consulta da la lectura actual, la tendencia, la variación y el historial.
bool dexFetch(Reading& r) {
  // Cada 3 lecturas (15 min) una barra: HIST_N-1 barras anteriores + la actual.
  constexpr int STEP = 3;
  constexpr int N = (ui::HIST_N - 1) * STEP + 1;

  for (int attempt = 0; attempt < 2; attempt++) {
    if (!loggedIn && !dexLogin()) return false;

    String body;
    String query = "?sessionId=" + dexSession + "&minutes=1440&maxCount=" + String(N);
    int code = dexPost("Publisher/ReadPublisherLatestGlucoseValues", query, "{}", body);

    if (code != 200) {
      if (code > 0 && parse::dexcomClassifyError(body.c_str()) == parse::DEXERR_SESSION) {
        loggedIn = false;   // la sesión caducó: se vuelve a entrar una vez
        continue;
      }
      Serial.printf("Dexcom readings failed, HTTP code: %d\n", code);
      lastError = code > 0 ? "Dexcom: error HTTP " + String(code) : String("Sin conexión con Dexcom");
      return false;
    }

    parse::Sample s[N];
    int n = parse::dexcomParseReadings(body.c_str(), s, N);
    if (n < 0) {
      lastError = "Dexcom: respuesta no válida";
      return false;
    }
    if (n == 0) {
      lastError = "Sin lecturas (¿Share activado?)";
      return false;
    }

    r.value = s[0].value;
    r.trend = s[0].trend;
    r.epoch = s[0].epoch;

    // Variación respecto a la lectura anterior (5 min), si es realmente la anterior.
    if (n > 1 && (s[0].epoch == 0 || s[1].epoch == 0 || (s[0].epoch - s[1].epoch) <= 600)) {
      r.hasDelta = true;
      r.delta = s[0].value - s[1].value;
    }

    // Historial: de la más antigua a la más reciente.
    int older = (n - 1) / STEP;
    if (older > ui::HIST_N - 1) older = ui::HIST_N - 1;
    for (int j = older; j >= 1; j--) r.hist[older - j] = s[j * STEP].value;
    r.hist[older] = s[0].value;
    r.histCount = older + 1;
    r.hasHist = true;

    lastError = "";
    return true;
  }

  lastError = "Dexcom: sesión no válida";
  return false;
}

// ============================================================
// CONSULTA COMÚN (con protección contra bloqueos de cuenta)
// ============================================================

bool fetchNow(Reading& r) {
  if (authBlocked) {
    lastError = "Cuenta rechazada: revisa datos";
    return false;
  }
  if (!loggedIn && nextAuthAllowed != 0 && (long)(millis() - nextAuthAllowed) < 0) {
    return false;   // esperando antes de volver a probar el login
  }

  bool wasLoggedIn = loggedIn;
  lastFailWasAuth = false;
  bool ok = (provider == PROV_DEXCOM) ? dexFetch(r) : libreFetch(r);

  if (ok) {
    authFailures = 0;
    nextAuthAllowed = 0;
  } else if (lastFailWasAuth && !wasLoggedIn) {
    authFailures++;
    nextAuthAllowed = millis() + AUTH_RETRY_MS;
    if (nextAuthAllowed == 0) nextAuthAllowed = 1;
    if (authFailures >= MAX_AUTH_FAILURES) {
      authBlocked = true;
      lastError = "Cuenta rechazada: revisa datos";
    }
  }
  return ok;
}

// ============================================================
// ESTADO + PANTALLA
// ============================================================

unsigned long lastFetch = 0;
unsigned long lastSuccessMillis = 0;
bool haveReading = false;
bool lastFetchFailed = false;
Reading cur;

// Muestras (una por consulta correcta) para la variación de LibreLinkUp.
int sampleBuf[DELTA_SAMPLES + 1];
int sampleCount = 0;

// Historial de LibreLinkUp (se refresca cada 5 min, no en cada consulta).
int libreHist[ui::HIST_N - 1];
int libreHistN = 0;
unsigned long lastHistFetch = 0;

ui::GlucoseView view;
bool needsFullRedraw = true;
bool needsBackground = true;
int drawnMinutes = -1;
int drawnWifiLevel = -1;
bool drawnStale = false;

void pushSample(int v) {
  if (sampleCount == DELTA_SAMPLES + 1) {
    for (int i = 0; i < DELTA_SAMPLES; i++) sampleBuf[i] = sampleBuf[i + 1];
    sampleCount = DELTA_SAMPLES;
  }
  sampleBuf[sampleCount++] = v;
}

void clearReadingState() {
  haveReading = false;
  lastFetchFailed = false;
  sampleCount = 0;
  libreHistN = 0;
  lastHistFetch = 0;
  drawnStale = false;
  needsBackground = true;
  alarmReset();
}

// ============================================================
// ALARMA SONORA
// ============================================================

beeper::Controller alarmCtl;
bool speakerOn = false;

// Enciende o apaga el altavoz (tono cuadrado por PWM a través del amplificador de la CYD).
void speakerSet(bool on) {
  if (on == speakerOn) return;
  speakerOn = on;
  if (on) {
    ledcSetup(ALARM_PWM_CHANNEL, ALARM_FREQ_HZ, 8);
    ledcAttachPin(ALARM_PIN, ALARM_PWM_CHANNEL);
    ledcWrite(ALARM_PWM_CHANNEL, ALARM_DUTY);
  } else {
    ledcWrite(ALARM_PWM_CHANNEL, 0);
    ledcDetachPin(ALARM_PIN);
    pinMode(ALARM_PIN, OUTPUT);
    digitalWrite(ALARM_PIN, LOW);   // en reposo, sin ruido
  }
}

// Corta el pitido a tiempo. Hay que llamarla a menudo (se hace en idleWait()).
void alarmTick() {
  speakerSet(alarmCtl.tick(millis()));
}

void alarmReset() {
  alarmCtl.reset();
  speakerSet(false);
}

// Sustituye a delay(): espera `ms` sin dejar de atender el altavoz, para que un
// pitido dure 5 s aunque el bucle esté "durmiendo".
void idleWait(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    alarmTick();
    delay(25);
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== Booting ===");

  tft.init();
  tft.setRotation(3);
  tft.fillScreen(ui::BG);

  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  pinMode(ALARM_PIN, OUTPUT);
  digitalWrite(ALARM_PIN, LOW);

  ui::drawMessage(tft, "Iniciando…");

  // No se comprueba BOOT aquí: GPIO0 a nivel bajo durante el reset mete al chip
  // en modo descarga. La reconfiguración se hace desde loop().
  runWiFiManagerSetup(false);
  startTimeSync();

  ui::drawMessage(tft, "Conectando", provider == PROV_DEXCOM ? "Dexcom" : "LibreLinkUp");
  lastFetch = 0;
  needsBackground = true;
}

unsigned long bootHeldSince = 0;
const unsigned long BOOT_HOLD_MS = 3000;

bool checkForReconfigureHold() {
  if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
    if (bootHeldSince == 0) {
      bootHeldSince = millis();
    } else if (millis() - bootHeldSince >= BOOT_HOLD_MS) {
      bootHeldSince = 0;
      return true;
    }
  } else {
    bootHeldSince = 0;
  }
  return false;
}

void loop() {
  if (checkForReconfigureHold()) {
    Serial.println("BOOT held for 3s during normal operation — entering config portal.");
    alarmReset();
    runWiFiManagerSetup(true);
    startTimeSync();
    clearReadingState();
    lastFetch = 0;
  }

  if (WiFi.status() != WL_CONNECTED) {
    // La pila WiFi del ESP32 reintenta sola; mientras tanto no consultamos.
    idleWait(1000);
    return;
  }

  unsigned long now = millis();

  if (now - lastFetch >= REFRESH_INTERVAL_MS || lastFetch == 0) {
    Reading r;
    bool ok = fetchNow(r);
    lastFetch = millis();

    if (ok) {
      cur = r;
      haveReading = true;
      lastFetchFailed = false;
      lastSuccessMillis = lastFetch;
      Serial.printf("Glucosa: %d mg/dL, tendencia: %d, epoch: %lld\n", r.value, r.trend, (long long)r.epoch);

      if (provider == PROV_LIBRE) {
        pushSample(r.value);
        // El historial cambia despacio: se pide como mucho cada 5 min.
        if (lastHistFetch == 0 || lastFetch - lastHistFetch >= 300000UL) {
          int n = libreHistory(patientId, libreHist, ui::HIST_N - 1);
          if (n > 0) libreHistN = n;
          lastHistFetch = lastFetch;
        }
      }
      needsFullRedraw = true;
    } else if (!haveReading) {
      ui::drawMessage(tft, "Sin datos", lastError.length() ? lastError : "Consultando…", ui::AMBER);
      needsBackground = true;
      idleWait(1000);
      return;
    } else {
      // Fallo con una lectura previa: se mantiene en pantalla marcada como obsoleta.
      lastFetchFailed = true;
      sampleCount = 0;   // la separación entre muestras ya no es fiable
      needsFullRedraw = true;
    }
  }

  if (haveReading) {
    now = millis();

    // Antigüedad real de la lectura: la hora de la medida del sensor si hay hora
    // sincronizada; si no, el tiempo desde la última consulta correcta.
    const bool reliableAge = cur.epoch > 0 && timeSynced();
    int minutesAgo;
    if (reliableAge) {
      long long diff = (long long)time(nullptr) - (long long)cur.epoch;
      minutesAgo = diff < 0 ? 0 : (int)(diff / 60);
    } else {
      minutesAgo = (int)((now - lastSuccessMillis) / 60000UL);
    }

    int rssi = WiFi.RSSI();
    int wifiLevel = ui::wifiLevel(rssi);

    view.value = cur.value;
    view.trend = cur.trend;
    view.stale = lastFetchFailed || (reliableAge && minutesAgo > STALE_AFTER_MIN);
    view.minutesAgo = minutesAgo;
    view.rssi = rssi;
    view.low = lowThreshold;
    view.high = highThreshold;

    // Alarma: solo con lecturas vigentes (una lectura obsoleta no dispara ni resetea nada).
    if (!alarmEnabled) {
      alarmReset();
    } else if (!view.stale) {
      alarmCtl.onReading(millis(), view.value, lowThreshold, highThreshold);
      alarmTick();
    }

    if (provider == PROV_DEXCOM) {
      view.hasDelta = cur.hasDelta;
      view.delta = cur.delta;
      view.histCount = cur.histCount;
      for (int i = 0; i < cur.histCount; i++) view.hist[i] = cur.hist[i];
    } else {
      view.hasDelta = (sampleCount == DELTA_SAMPLES + 1);
      view.delta = view.hasDelta ? (cur.value - sampleBuf[0]) : 0;
      view.histCount = libreHistN + 1;   // barras anteriores + la actual
      for (int i = 0; i < libreHistN; i++) view.hist[i] = libreHist[i];
      view.hist[libreHistN] = cur.value;
    }

    if (view.stale != drawnStale) needsFullRedraw = true;

    if (needsFullRedraw || needsBackground) {
      ui::drawGlucose(tft, view, needsBackground);
      needsFullRedraw = false;
      needsBackground = false;
      drawnMinutes = minutesAgo;
      drawnWifiLevel = wifiLevel;
      drawnStale = view.stale;
    } else {
      // Actualizaciones pequeñas: texto de "hace X min" e icono WiFi.
      if (minutesAgo != drawnMinutes) {
        ui::drawAge(tft, view.stale, minutesAgo);
        drawnMinutes = minutesAgo;
      }
      if (wifiLevel != drawnWifiLevel) {
        ui::drawWifi(tft, rssi);
        drawnWifiLevel = wifiLevel;
      }
    }
  }

  idleWait(1000);
}
