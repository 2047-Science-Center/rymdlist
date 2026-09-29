// ============================================================================
//  Rymdlist — ESP32 + WS2812B, live-styrd från mobilen
// ----------------------------------------------------------------------------
//  Fristående testrigg. ESP32:n serverar själv en webbsida (ingen extern
//  server, ingen MQTT-broker). Ändringar går via WebSocket och speglas till
//  alla anslutna telefoner. Senaste inställning sparas i NVS.
//
//  Arkitektur:
//    * State = det enda kontraktet (samma fält som framtida Blocks/MQTT).
//    * applyState(State) = enda vägen in. Webb-UI, NVS-återläsning och
//      (i framtiden) MQTT går ALLA genom denna funktion.
//    * Effektmotorn läser globala `state` och ritar non-blocking i loop().
// ============================================================================

#include <Arduino.h>
#include <FastLED.h>
#if defined(ESP32)
  #include <WiFi.h>
  #include <ESPmDNS.h>
  #include <Preferences.h>
#elif defined(ESP8266)
  #include <ESP8266WiFi.h>
  #include <ESP8266mDNS.h>
  #include <EEPROM.h>
#else
  #error "Rymdlist kräver ett kort med WiFi: ESP32 eller ESP8266."
#endif
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

#include "config.h"

// ============================================================================
//  STATE-KONTRAKT  — håll fältnamnen stabila (webb, NVS, framtida MQTT/Blocks)
// ============================================================================
struct State {
  String  effect;      // "solid" | "av" | "puls" | "lugn" | "alarm" | "regnbage"
  CRGB    color;       // vald färg (ignoreras av "alarm" och "regnbage")
  uint8_t brightness;  // 0..255, global ljusstyrka (gäller alla effekter)
  uint8_t speed;       // 0..255, takt/hastighet (gäller alla effekter)
};

// Tillåtna effekter — enda källan till sanning för giltiga värden.
static const char* const EFFECTS[] = {"solid", "av", "puls", "lugn", "alarm", "regnbage"};
static const size_t NUM_EFFECTS = sizeof(EFFECTS) / sizeof(EFFECTS[0]);

enum Effect : uint8_t { EFF_SOLID, EFF_OFF, EFF_PULS, EFF_LUGN, EFF_ALARM, EFF_REGNBAGE };

// ============================================================================
//  GLOBALER
// ============================================================================
CRGB    leds[NUM_LEDS];
State   state;                 // aktuellt tillstånd
Effect  currentEffect = EFF_SOLID;

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
#if defined(ESP32)
Preferences   prefs;
#endif

// Debouncad NVS-skrivning (skonar flashminnet vid slider-dragning)
bool     nvsDirty   = false;
uint32_t nvsDirtyAt = 0;
const uint32_t NVS_DEBOUNCE_MS = 1500;

const uint8_t FPS = 60;        // uppdateringsfrekvens för effektmotorn

// ============================================================================
//  FÄRG-HJÄLPARE  (hex <-> CRGB, för <input type=color> och JSON)
// ============================================================================
static CRGB hexToCRGB(const char* hex) {
  if (!hex) return CRGB::Black;
  if (*hex == '#') hex++;                 // tolerera ledande '#'
  long v = strtol(hex, nullptr, 16);
  return CRGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

static void crgbToHex(const CRGB& c, char* out /* minst 8 byte */) {
  snprintf(out, 8, "#%02X%02X%02X", c.r, c.g, c.b);
}

static Effect effectToEnum(const String& e) {
  if (e == "av")       return EFF_OFF;
  if (e == "puls")     return EFF_PULS;
  if (e == "lugn")     return EFF_LUGN;
  if (e == "alarm")    return EFF_ALARM;
  if (e == "regnbage") return EFF_REGNBAGE;
  return EFF_SOLID;                        // "solid" + okänt -> solid
}

static bool isValidEffect(const String& e) {
  for (size_t i = 0; i < NUM_EFFECTS; i++)
    if (e == EFFECTS[i]) return true;
  return false;
}

// ============================================================================
//  STATE  <->  JSON
// ============================================================================
static String stateToJson() {
  JsonDocument doc;
  char hex[8];
  crgbToHex(state.color, hex);
  doc["effect"]     = state.effect;
  doc["color"]      = hex;
  doc["brightness"] = state.brightness;
  doc["speed"]      = state.speed;
  String out;
  serializeJson(doc, out);
  return out;
}

// ============================================================================
//  BESTÄNDIG LAGRING  (spara/återläs senaste inställning)
//    ESP32:   NVS via Preferences.
//    ESP8266: liten struct i EEPROM (Preferences finns inte där).
// ============================================================================
static uint8_t effectIndex(const String& e) {
  for (uint8_t i = 0; i < NUM_EFFECTS; i++)
    if (e == EFFECTS[i]) return i;
  return 0;   // "solid"
}

#if defined(ESP8266)
struct Persist {                 // lagras på offset 0 i EEPROM
  uint8_t magic;                 // PERSIST_MAGIC = giltig data
  uint8_t effectIdx;
  uint8_t r, g, b;
  uint8_t brightness;
  uint8_t speed;
};
static const uint8_t PERSIST_MAGIC = 0x5A;
#endif

static void initStorage() {
#if defined(ESP8266)
  EEPROM.begin(sizeof(Persist));
#endif
}

static void saveState() {
#if defined(ESP32)
  prefs.begin("rymdlist", /*readOnly=*/false);
  prefs.putString("effect", state.effect);
  prefs.putUInt("color", ((uint32_t)state.color.r << 16) |
                         ((uint32_t)state.color.g << 8)  |
                          (uint32_t)state.color.b);
  prefs.putUChar("bri", state.brightness);
  prefs.putUChar("spd", state.speed);
  prefs.end();
#elif defined(ESP8266)
  Persist p;
  p.magic      = PERSIST_MAGIC;
  p.effectIdx  = effectIndex(state.effect);
  p.r          = state.color.r;
  p.g          = state.color.g;
  p.b          = state.color.b;
  p.brightness = state.brightness;
  p.speed      = state.speed;
  EEPROM.put(0, p);
  EEPROM.commit();
#endif
}

static void loadState() {
  // Standardvärden om inget finns sparat
  state.effect     = "solid";
  state.color      = CRGB(0, 120, 255);   // rymdblå
  state.brightness = 128;
  state.speed      = 128;

#if defined(ESP32)
  prefs.begin("rymdlist", /*readOnly=*/true);
  state.effect     = prefs.getString("effect", state.effect);
  uint32_t packed  = prefs.getUInt("color",
                       ((uint32_t)state.color.r << 16) |
                       ((uint32_t)state.color.g << 8)  |
                        (uint32_t)state.color.b);
  state.color      = CRGB((packed >> 16) & 0xFF, (packed >> 8) & 0xFF, packed & 0xFF);
  state.brightness = prefs.getUChar("bri", state.brightness);
  state.speed      = prefs.getUChar("spd", state.speed);
  prefs.end();
#elif defined(ESP8266)
  Persist p;
  EEPROM.get(0, p);
  if (p.magic == PERSIST_MAGIC && p.effectIdx < NUM_EFFECTS) {
    state.effect     = EFFECTS[p.effectIdx];
    state.color      = CRGB(p.r, p.g, p.b);
    state.brightness = p.brightness;
    state.speed      = p.speed;
  }
#endif

  if (!isValidEffect(state.effect)) state.effect = "solid";
}

// ============================================================================
//  applyState() — ENDA vägen in. Alla styrytor går genom denna.
// ============================================================================
static void broadcastState();   // fwd

void applyState(const State& s) {
  state              = s;
  if (!isValidEffect(state.effect)) state.effect = "solid";
  currentEffect      = effectToEnum(state.effect);
  FastLED.setBrightness(state.brightness);   // global ljusstyrka -> alla effekter

  nvsDirty   = true;                          // spara (debouncat) i loop()
  nvsDirtyAt = millis();

  broadcastState();                           // spegla till alla telefoner
}

// Partiell uppdatering från JSON: utgå från nuvarande state, skriv bara fält
// som finns med. Samma parser som den framtida MQTT-kroken ska återanvända.
static void applyStateFromJson(const char* json, size_t len) {
  JsonDocument doc;
  if (deserializeJson(doc, json, len)) return;   // ignorera trasig payload

  State s = state;                               // börja från nuvarande
  if (!doc["effect"].isNull()) {
    String e = doc["effect"].as<String>();
    if (isValidEffect(e)) s.effect = e;          // ignorera okända effekter
  }
  if (!doc["color"].isNull())      s.color      = hexToCRGB(doc["color"].as<const char*>());
  if (!doc["brightness"].isNull()) s.brightness = constrain(doc["brightness"].as<int>(), 0, 255);
  if (!doc["speed"].isNull())      s.speed      = constrain(doc["speed"].as<int>(), 0, 255);

  applyState(s);
}

// ============================================================================
//  EFFEKT-MOTOR  (non-blocking, ritas varje frame i loop())
// ============================================================================
static void renderEffect() {
  switch (currentEffect) {

    case EFF_OFF:
      fill_solid(leds, NUM_LEDS, CRGB::Black);
      break;

    case EFF_SOLID:
      fill_solid(leds, NUM_LEDS, state.color);
      break;

    case EFF_PULS: {                              // andning i vald färg
      uint8_t bpm    = map(state.speed, 0, 255, 6, 90);
      uint8_t breath = beatsin8(bpm, 25, 255);    // aldrig helt släckt
      CRGB c = state.color;
      c.nscale8_video(breath);
      fill_solid(leds, NUM_LEDS, c);
      break;
    }

    case EFF_LUGN: {                              // långsam våg/gradient i färgen
      uint8_t bpm = map(state.speed, 0, 255, 3, 26);
      for (uint16_t i = 0; i < NUM_LEDS; i++) {
        uint8_t phase = (uint16_t)i * 255 / (NUM_LEDS > 0 ? NUM_LEDS : 1);
        uint8_t wave  = beatsin8(bpm, 40, 255, 0, phase);
        CRGB c = state.color;
        c.nscale8_video(wave);
        leds[i] = c;
      }
      break;
    }

    case EFF_ALARM: {                             // snabb röd blink (ignorerar color)
      uint8_t bpm = map(state.speed, 0, 255, 40, 260);
      bool on = beat8(bpm) < 128;
      fill_solid(leds, NUM_LEDS, on ? CRGB::Red : CRGB::Black);
      break;
    }

    case EFF_REGNBAGE: {                          // rullande regnbåge
      uint8_t startHue = beat8(map(state.speed, 0, 255, 2, 60));
      fill_rainbow(leds, NUM_LEDS, startHue, 7);
      break;
    }
  }
}

// ============================================================================
//  WEBSOCKET
// ============================================================================
static void broadcastState() {
  if (ws.count() == 0) return;
  String json = stateToJson();
  ws.textAll(json);
}

static void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                      AwsEventType type, void* arg, uint8_t* data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
      // Ny telefon: skicka nuvarande state direkt så UI:t speglar verkligheten.
      client->text(stateToJson());
      break;

    case WS_EVT_DATA: {
      AwsFrameInfo* info = (AwsFrameInfo*)arg;
      // Styrmeddelanden är små -> hantera enkel single-frame text.
      if (info->final && info->index == 0 && info->len == len &&
          info->opcode == WS_TEXT) {
        applyStateFromJson((const char*)data, len);
      }
      break;
    }

    default:
      break;   // CONNECT/DISCONNECT/PONG/ERROR — inget att göra
  }
}

// ============================================================================
//  WEBB-UI  (en sida, embeddad, ingen byggkedja, mörkt tema, storfingrat)
//  PROGMEM krävs på ESP8266 (annars äter sidan RAM); på ESP32 är det en no-op.
//  Serveras därför PROGMEM-säkert (send_P på ESP8266) i route-handlern nedan.
// ============================================================================
const char INDEX_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="sv">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no">
<title>Rymdlist</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; -webkit-tap-highlight-color: transparent; }
  body {
    margin: 0; padding: 20px 16px 40px;
    font-family: -apple-system, system-ui, "Segoe UI", Roboto, sans-serif;
    background: #0b0e17; color: #e8ecf5;
    max-width: 520px; margin-inline: auto;
  }
  h1 { font-size: 22px; font-weight: 700; margin: 4px 0 2px; letter-spacing: .5px; }
  .status { font-size: 13px; color: #7c88a5; margin-bottom: 22px; }
  .status b { color: #4ade80; }
  .status.off b { color: #f87171; }
  h2 { font-size: 13px; text-transform: uppercase; letter-spacing: 1.5px;
       color: #7c88a5; margin: 26px 0 10px; font-weight: 600; }
  .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; }
  button.eff {
    appearance: none; border: 2px solid #1e2740; background: #141a2b;
    color: #e8ecf5; font-size: 18px; font-weight: 600; padding: 22px 8px;
    border-radius: 16px; cursor: pointer; transition: all .12s ease;
    touch-action: manipulation;
  }
  button.eff:active { transform: scale(.97); }
  button.eff.active { border-color: #4f8cff; background: #17233f; box-shadow: 0 0 0 3px rgba(79,140,255,.25); }
  .row { margin: 18px 0; }
  .row label { display: flex; justify-content: space-between; font-size: 15px;
               margin-bottom: 10px; color: #c3cbe0; }
  .row label span { color: #7c88a5; font-variant-numeric: tabular-nums; }
  input[type=range] {
    -webkit-appearance: none; appearance: none; width: 100%; height: 14px;
    background: #1e2740; border-radius: 10px; outline: none;
  }
  input[type=range]::-webkit-slider-thumb {
    -webkit-appearance: none; appearance: none; width: 34px; height: 34px;
    border-radius: 50%; background: #4f8cff; cursor: pointer;
    border: 3px solid #0b0e17;
  }
  input[type=range]::-moz-range-thumb {
    width: 34px; height: 34px; border-radius: 50%; background: #4f8cff;
    cursor: pointer; border: 3px solid #0b0e17;
  }
  .colorwrap { display: flex; align-items: center; gap: 16px; }
  input[type=color] {
    -webkit-appearance: none; appearance: none; border: none; background: none;
    width: 84px; height: 84px; border-radius: 20px; cursor: pointer; padding: 0;
  }
  input[type=color]::-webkit-color-swatch-wrapper { padding: 0; }
  input[type=color]::-webkit-color-swatch { border: 2px solid #1e2740; border-radius: 20px; }
  input[type=color]::-moz-color-swatch { border: 2px solid #1e2740; border-radius: 20px; }
  .colorwrap .hint { font-size: 14px; color: #7c88a5; }
  #dot { display:inline-block; width:9px; height:9px; border-radius:50%;
         background:#f87171; margin-right:6px; vertical-align:middle; }
  #dot.on { background:#4ade80; }
</style>
</head>
<body>
  <h1>🚀 Rymdlist</h1>
  <div class="status" id="status"><span id="dot"></span>Ansluter…</div>

  <h2>Effekt</h2>
  <div class="grid" id="effects">
    <button class="eff" data-effect="solid">Fast sken</button>
    <button class="eff" data-effect="av">Av</button>
    <button class="eff" data-effect="puls">Puls</button>
    <button class="eff" data-effect="lugn">Lugn</button>
    <button class="eff" data-effect="alarm">Alarm</button>
    <button class="eff" data-effect="regnbage">Regnbåge</button>
  </div>

  <h2>Färg</h2>
  <div class="colorwrap">
    <input type="color" id="color" value="#0078ff">
    <span class="hint">Ignoreras av Alarm och Regnbåge</span>
  </div>

  <div class="row">
    <label>Ljusstyrka <span id="briVal">128</span></label>
    <input type="range" id="bri" min="0" max="255" value="128">
  </div>

  <div class="row">
    <label>Hastighet <span id="spdVal">128</span></label>
    <input type="range" id="spd" min="0" max="255" value="128">
  </div>

<script>
(function () {
  var ws, connected = false;
  var $ = function (id) { return document.getElementById(id); };

  // --- Throttle: skicka senaste värdet, max ~var 60:e ms per nyckel ---
  var pending = {}, timers = {};
  function send(key, obj) {
    pending[key] = obj;
    if (timers[key]) return;
    flush(key);
    timers[key] = setInterval(function () {
      if (pending[key] === null) { clearInterval(timers[key]); timers[key] = null; return; }
      flush(key);
    }, 60);
  }
  function flush(key) {
    if (!connected || pending[key] === null || pending[key] === undefined) { pending[key] = null; return; }
    ws.send(JSON.stringify(pending[key]));
    pending[key] = null;
  }

  // --- Spegla inkommande state till kontrollerna ---
  function applyToUI(s) {
    if (s.effect) {
      var btns = document.querySelectorAll('.eff');
      for (var i = 0; i < btns.length; i++)
        btns[i].classList.toggle('active', btns[i].dataset.effect === s.effect);
    }
    if (s.color)                         $('color').value  = s.color;
    if (typeof s.brightness === 'number'){ $('bri').value = s.brightness; $('briVal').textContent = s.brightness; }
    if (typeof s.speed === 'number')     { $('spd').value = s.speed;      $('spdVal').textContent = s.speed; }
  }

  function setStatus(txt, ok) {
    $('status').innerHTML = '<span id="dot" class="' + (ok ? 'on' : '') + '"></span>' + txt;
  }

  // --- WebSocket med auto-reconnect ---
  function connect() {
    ws = new WebSocket('ws://' + location.host + '/ws');
    ws.onopen = function () { connected = true; setStatus('Ansluten', true); };
    ws.onclose = function () {
      connected = false; setStatus('Frånkopplad — försöker igen…', false);
      setTimeout(connect, 1000);
    };
    ws.onerror = function () { ws.close(); };
    ws.onmessage = function (e) {
      try { applyToUI(JSON.parse(e.data)); } catch (err) {}
    };
  }
  connect();

  // --- Kontroller ---
  var effBtns = document.querySelectorAll('.eff');
  for (var i = 0; i < effBtns.length; i++) {
    effBtns[i].addEventListener('click', function () {
      var eff = this.dataset.effect;
      applyToUI({ effect: eff });          // omedelbar visuell återkoppling
      send('effect', { effect: eff });
    });
  }
  $('color').addEventListener('input', function () {
    send('color', { color: this.value });
  });
  $('bri').addEventListener('input', function () {
    $('briVal').textContent = this.value;
    send('bri', { brightness: parseInt(this.value, 10) });
  });
  $('spd').addEventListener('input', function () {
    $('spdVal').textContent = this.value;
    send('spd', { speed: parseInt(this.value, 10) });
  });
})();
</script>
</body>
</html>)HTML";

// ============================================================================
//  NÄT  (STA -> mDNS, annars eget AP)
// ============================================================================
static bool connectSTA() {
  if (strlen(WIFI_SSID) == 0) {
    Serial.println("[wifi] WIFI_SSID tom i config.h — hoppar direkt till AP-läge.");
    return false;
  }
  Serial.printf("[wifi] Ansluter till \"%s\"", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

static void startAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID);               // öppet AP (fungerar även om nätet isolerar klienter)
  Serial.println("[wifi] Startade eget AP:");
  Serial.printf ("        SSID: %s (öppet)\n", AP_SSID);
  Serial.printf ("        Öppna: http://%s\n", WiFi.softAPIP().toString().c_str());
}

static void startNetworkServices() {
  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[mdns] Namn aktivt: http://%s.local\n", MDNS_NAME);
  } else {
    Serial.println("[mdns] Kunde inte starta mDNS.");
  }

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
#if defined(ESP8266)
    req->send_P(200, "text/html", INDEX_HTML);   // PROGMEM-säkert på ESP8266
#else
    req->send(200, "text/html", INDEX_HTML);
#endif
  });
  server.onNotFound([](AsyncWebServerRequest* req) {
    req->redirect("/");              // allt annat -> UI:t (bekvämt i AP-läge)
  });
  server.begin();
  Serial.println("[web] Webbserver + WebSocket igång på port 80.");
}

// ============================================================================
//  setup / loop
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Rymdlist ===");

  // FastLED + strömtak (skyddar klen adapter enligt config)
  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS)
         .setCorrection(TypicalLEDStrip);
  FastLED.setMaxPowerInVoltsAndMilliamps(VOLTS, MAX_MA);
  Serial.printf("[led] %d px på GPIO %d, strömtak %d mA @ %dV\n",
                NUM_LEDS, LED_PIN, MAX_MA, VOLTS);

  // Återläs senaste inställning och applicera den genom den enda tratten
  initStorage();
  loadState();
  applyState(state);
  nvsDirty = false;                  // nyss inläst — inget att skriva tillbaka

  // Nät: försök STA, annars eget AP
  if (connectSTA()) {
    Serial.printf("[wifi] Ansluten. IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("[wifi] Öppna: http://%s.local  (eller http://%s)\n",
                  MDNS_NAME, WiFi.localIP().toString().c_str());
  } else {
    startAP();
  }
  startNetworkServices();
}

void loop() {
  // Effektmotorn — non-blocking, fast bildfrekvens
  static uint32_t lastFrame = 0;
  uint32_t now = millis();
  if (now - lastFrame >= (1000 / FPS)) {
    lastFrame = now;
    renderEffect();
    FastLED.show();
  }

  // Debouncad skrivning till beständig lagring
  if (nvsDirty && (now - nvsDirtyAt) > NVS_DEBOUNCE_MS) {
    saveState();
    nvsDirty = false;
  }

#if defined(ESP8266)
  MDNS.update();          // ESP8266:s mDNS måste pumpas i loop()
#endif
  ws.cleanupClients();
}

// ============================================================================
//  === FRAMTIDA MQTT-KROK — bygg vidare här, aktivera INTE nu ===
// ----------------------------------------------------------------------------
//  Sömmen är redan klar: en MQTT-klient behöver bara mata samma JSON till
//  applyStateFromJson() (som i sin tur går genom applyState). Inga andra
//  ändringar krävs — effektmotorn och webb-UI:t speglar automatiskt.
//
//  1) platformio.ini:   lägg till   knolleary/PubSubClient @ ^2.8
//  2) config.h:         #define MQTT_HOST "192.168.x.x"  (broker)
//                       #define MQTT_ZON  "syd"          -> topic rymdbasen/syd/led
//  3) Kod (skiss):
//
//     #include <PubSubClient.h>
//     WiFiClient   mqttNet;
//     PubSubClient mqtt(mqttNet);
//
//     void onMqttMessage(char* topic, byte* payload, unsigned int len) {
//       // payload = {"effect":"puls","color":"#ff0000","brightness":200,"speed":180}
//       applyStateFromJson((const char*)payload, len);   // <-- samma tratt!
//     }
//
//     void mqttSetup() {
//       mqtt.setServer(MQTT_HOST, 1883);
//       mqtt.setCallback(onMqttMessage);
//     }
//     void mqttReconnect() {
//       if (mqtt.connect("rymdlist")) mqtt.subscribe("rymdbasen/" MQTT_ZON "/led");
//     }
//     // I loop():  if (!mqtt.connected()) mqttReconnect();  mqtt.loop();
//
//  Topic-konvention bekräftas mot Rymdbasen/Blocks innan hopkoppling (dagens
//  faktiska mönster är station<N>/<signal>; håll effect/color/brightness/speed
//  stabila oavsett transport).
// ============================================================================
