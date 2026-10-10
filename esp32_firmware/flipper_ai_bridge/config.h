#pragma once

// ===== WiFi =====
#define WIFI_SSID "TU_WIFI"
#define WIFI_PASS "TU_CONTRASENA"

// ===== Proveedor de IA =====
// 1 = Claude (API de Anthropic)
// 2 = Cualquier API compatible con OpenAI (OpenAI, Groq, OpenRouter, Ollama en tu PC...)
#define PROVIDER 1

#if PROVIDER == 1
  #define API_URL   "https://api.anthropic.com/v1/messages"
  #define API_KEY   "sk-ant-..."            // https://console.anthropic.com
  #define AI_MODEL  "claude-haiku-5-5"      // rapido y barato; o "claude-sonnet-5-5"
#else
  #define API_URL   "https://api.openai.com/v1/chat/completions"
  #define API_KEY   "sk-..."
  #define AI_MODEL  "gpt-4o-mini"
#endif

// Respuestas cortas: la pantalla del Flipper es de 128x64
#define MAX_TOKENS 300
#define SYSTEM_PROMPT \
  "Eres un asistente que responde en la pantalla de un Flipper Zero (128x64 px). " \
  "Responde en espanol, breve (maximo unas 80 palabras), en texto plano, " \
  "sin markdown, sin emojis y sin tablas."

// Mensajes de la conversacion que se recuerdan (pregunta + respuesta = 2)
#define MAX_HISTORY 8

// ===== UART hacia el Flipper =====
// Placa WiFi oficial (ESP32-S2-WROVER): Flipper TX(pin 13) -> GPIO44, Flipper RX(pin 14) <- GPIO43
#define FLIPPER_RX_PIN 44
#define FLIPPER_TX_PIN 43
#define FLIPPER_BAUD   115200
