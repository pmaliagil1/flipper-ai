/*
 * Flipper AI Bridge - firmware para la placa WiFi del Flipper Zero (ESP32-S2-WROVER)
 *
 * Recibe preguntas del Flipper por UART, llama a la API de IA por HTTPS
 * y devuelve la respuesta en ASCII terminada en 0x04 (EOT).
 *
 * Requisitos (Arduino IDE):
 *   - Placa: "ESP32S2 Dev Module" (paquete esp32 de Espressif)
 *   - USB CDC On Boot: Enabled  (asi Serial = USB para depurar)
 *   - PSRAM: Enabled
 *   - Libreria: ArduinoJson (v7) de Benoit Blanchon
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "config.h"

#define EOT '\x04'
#define FZ Serial1

struct Msg {
  String role;
  String content;
};

Msg history[MAX_HISTORY];
int historyLen = 0;
String lineBuf;

// ---------- Utilidades ----------

void replyToFlipper(const String& text) {
  FZ.print(text);
  FZ.write(EOT);
  FZ.flush();
}

// La fuente del Flipper solo tiene ASCII: quitar tildes y similares
String toAscii(const String& in) {
  String out;
  out.reserve(in.length());
  const uint8_t* s = (const uint8_t*)in.c_str();
  size_t i = 0, n = in.length();
  while (i < n) {
    uint32_t cp;
    uint8_t c = s[i];
    if (c < 0x80) { cp = c; i += 1; }
    else if ((c & 0xE0) == 0xC0 && i + 1 < n) { cp = ((c & 0x1F) << 6) | (s[i+1] & 0x3F); i += 2; }
    else if ((c & 0xF0) == 0xE0 && i + 2 < n) { cp = ((c & 0x0F) << 12) | ((s[i+1] & 0x3F) << 6) | (s[i+2] & 0x3F); i += 3; }
    else if ((c & 0xF8) == 0xF0 && i + 3 < n) { cp = 0; i += 4; }  // emojis: fuera
    else { i += 1; continue; }

    if (cp == 0) continue;
    if (cp < 0x80) {
      if (cp == '\n' || (cp >= 0x20 && cp < 0x7F)) out += (char)cp;
      continue;
    }
    switch (cp) {
      case 0xE1: case 0xE0: case 0xE4: case 0xE2: out += 'a'; break;
      case 0xE9: case 0xE8: case 0xEB: case 0xEA: out += 'e'; break;
      case 0xED: case 0xEC: case 0xEF: case 0xEE: out += 'i'; break;
      case 0xF3: case 0xF2: case 0xF6: case 0xF4: out += 'o'; break;
      case 0xFA: case 0xF9: case 0xFC: case 0xFB: out += 'u'; break;
      case 0xC1: case 0xC0: case 0xC4: out += 'A'; break;
      case 0xC9: case 0xC8: out += 'E'; break;
      case 0xCD: out += 'I'; break;
      case 0xD3: case 0xD6: out += 'O'; break;
      case 0xDA: case 0xDC: out += 'U'; break;
      case 0xF1: out += 'n'; break;
      case 0xD1: out += 'N'; break;
      case 0xE7: out += 'c'; break;
      case 0xC7: out += 'C'; break;
      case 0xBF: case 0xA1: break;               // ¿ ¡
      case 0x2018: case 0x2019: out += '\''; break;
      case 0x201C: case 0x201D: case 0xAB: case 0xBB: out += '"'; break;
      case 0x2013: case 0x2014: out += '-'; break;
      case 0x2026: out += "..."; break;
      case 0xB0: out += " grados"; break;
      case 0x20AC: out += "EUR"; break;
      default: break;
    }
  }
  return out;
}

bool ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) delay(250);
  return WiFi.status() == WL_CONNECTED;
}

void pushHistory(const String& role, const String& content) {
  if (historyLen == MAX_HISTORY) {
    // Quitar el par mas antiguo (pregunta + respuesta)
    for (int i = 2; i < MAX_HISTORY; i++) history[i - 2] = history[i];
    historyLen -= 2;
  }
  history[historyLen++] = { role, content };
}

// ---------- Llamada a la IA ----------

bool askAI(String& answer) {
  JsonDocument req;
  req["model"] = AI_MODEL;
  req["max_tokens"] = MAX_TOKENS;
  JsonArray msgs = req["messages"].to<JsonArray>();

#if PROVIDER == 1
  req["system"] = SYSTEM_PROMPT;
#else
  JsonObject sys = msgs.add<JsonObject>();
  sys["role"] = "system";
  sys["content"] = SYSTEM_PROMPT;
#endif
  for (int i = 0; i < historyLen; i++) {
    JsonObject m = msgs.add<JsonObject>();
    m["role"] = history[i].role;
    m["content"] = history[i].content;
  }

  String body;
  serializeJson(req, body);

  WiFiClientSecure client;
  client.setInsecure();  // sin verificar certificado (simple). Ver README para hacerlo seguro.
  HTTPClient http;
  http.setTimeout(60000);
  if (!http.begin(client, API_URL)) {
    answer = "Error: URL de API no valida.";
    return false;
  }
  http.addHeader("content-type", "application/json");
#if PROVIDER == 1
  http.addHeader("x-api-key", API_KEY);
  http.addHeader("anthropic-version", "2023-06-01");
#else
  http.addHeader("Authorization", String("Bearer ") + API_KEY);
#endif

  int code = http.POST(body);
  String resp = http.getString();
  http.end();

  if (code <= 0) {
    answer = "Error de conexion: " + http.errorToString(code);
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, resp)) {
    answer = "Error: respuesta no valida (HTTP " + String(code) + ")";
    return false;
  }
  if (code != 200) {
    const char* err = doc["error"]["message"] | "desconocido";
    answer = "Error HTTP " + String(code) + ":\n" + err;
    return false;
  }

#if PROVIDER == 1
  String text;
  for (JsonObject block : doc["content"].as<JsonArray>()) {
    if (String(block["type"] | "") == "text") text += block["text"].as<const char*>();
  }
  answer = text;
#else
  answer = doc["choices"][0]["message"]["content"] | "";
#endif
  return answer.length() > 0;
}

// ---------- Comandos ----------

void handleLine(String line) {
  line.trim();
  if (line.length() == 0) return;

  if (line == "P") {
    String st = "Placa WiFi OK\nModelo: " AI_MODEL "\n";
    if (ensureWifi()) st += "WiFi: " + WiFi.SSID() + "\nIP: " + WiFi.localIP().toString();
    else st += "WiFi: SIN CONEXION\nRevisa config.h";
    replyToFlipper(st);
    return;
  }
  if (line == "R") {
    historyLen = 0;
    replyToFlipper("Conversacion borrada.\nPuedes empezar otra.");
    return;
  }
  if (line.startsWith("Q:")) {
    String question = line.substring(2);
    Serial.println("> " + question);
    if (!ensureWifi()) {
      replyToFlipper("Sin WiFi. Revisa SSID y\ncontrasena en config.h");
      return;
    }
    pushHistory("user", question);
    String answer;
    if (askAI(answer)) {
      pushHistory("assistant", answer);
      Serial.println("< " + answer);
      replyToFlipper(toAscii(answer));
    } else {
      historyLen--;  // quitar la pregunta fallida
      Serial.println("! " + answer);
      replyToFlipper(toAscii(answer));
    }
    return;
  }
  replyToFlipper("Comando desconocido.");
}

void setup() {
  Serial.begin(115200);  // USB, para depurar
  FZ.setRxBufferSize(1024);  // debe ir antes de begin()
  FZ.begin(FLIPPER_BAUD, SERIAL_8N1, FLIPPER_RX_PIN, FLIPPER_TX_PIN);
  WiFi.mode(WIFI_STA);
  ensureWifi();
  Serial.println("Flipper AI Bridge listo");
}

void loop() {
  while (FZ.available()) {
    char c = FZ.read();
    if (c == '\n') {
      handleLine(lineBuf);
      lineBuf = "";
    } else if (c != '\r' && lineBuf.length() < 1024) {
      lineBuf += c;
    }
  }
  delay(2);
}
