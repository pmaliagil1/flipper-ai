# Flipper AI 🐬🤖

Chat with an AI (Claude, GPT-OSS via Groq, or any OpenAI-compatible API) **directly from your Flipper Zero**, using the official WiFi Dev Board (ESP32-S2) running [FlipperHTTP](https://github.com/jblanked/FlipperHTTP).

![Flipper AI demo](screenshots/demo.png)

*Versión en español más abajo.*

```
[Flipper Zero] --UART--> [WiFi Dev Board + FlipperHTTP] --HTTPS--> [AI API]
```

## Features
- Type a question on the Flipper keyboard and read the answer on screen.
- Remembers the conversation (last 3 questions) until you start a new one.
- Works with **Groq (free)**, the **Claude API**, OpenAI, OpenRouter… anything OpenAI-compatible.
- No custom ESP32 firmware needed: uses the standard FlipperHTTP firmware.
- Built-in WiFi setup, board test and config viewer.
- Accents and Unicode are converted to ASCII for the Flipper font.

Tested on **Momentum MNTM-012** with the official WiFi Dev Board.

## Install
1. Flash **FlipperHTTP** on your WiFi Dev Board (see the FlipperHTTP repo) if you haven't already.
2. Copy `flipper_app/dist/flipper_ai.fap` to your SD card at `apps/GPIO/` (e.g. with qFlipper).
   The prebuilt `.fap` was compiled against the Momentum MNTM-012 SDK; for other firmwares, build it yourself (see below).
3. Open the app once. It creates `apps_data/flipper_ai/config.txt` on the SD card. Replace it with one of the ready-made files in [`examples/`](examples/) with your API key:

**Groq (free, get a key at console.groq.com):**
```
provider=openai
api_key=gsk_...
model=openai/gpt-oss-120b
reasoning_effort=low
max_tokens=1200
url=https://api.groq.com/openai/v1/chat/completions
```

**Claude (paid API, console.anthropic.com):**
```
provider=claude
api_key=sk-ant-...
model=claude-haiku-5-5
```

4. In the app: **Configurar WiFi** (2.4 GHz network, first time only) → **Preguntar**.
   The `_` key on the Flipper keyboard works as a space.

If you get `model does not exist`, the provider retired that model: just change the `model=` line.

## Build
```
pip install ufbt
ufbt update --url <SDK zip for your firmware>
cd flipper_app && ufbt
```

## Technical notes
- Long UART lines (> ~550 bytes) get corrupted on the ESP32 if sent at full speed, so the app sends requests in 16-byte chunks with short pauses, and automatically retries if the board or the API reports invalid JSON.
- `esp32_firmware/` contains an optional standalone bridge firmware (Arduino) that doesn't need FlipperHTTP.
- ⚠️ Your API key is stored in plain text on the SD card. Don't share your SD/config.

---

## Español

App para **Flipper Zero** que te deja chatear con una IA (Claude, GPT-OSS gratis con Groq, o cualquier API compatible con OpenAI) usando la placa WiFi oficial (ESP32-S2) con el firmware [FlipperHTTP](https://github.com/jblanked/FlipperHTTP).

📖 **Guía paso a paso: [docs/GUIA_INSTALACION.md](docs/GUIA_INSTALACION.md)** · Configs de ejemplo en [`examples/`](examples/).

**Instalación:** copia `flipper_ai.fap` a `apps/GPIO/` de la SD, abre la app una vez y edita `apps_data/flipper_ai/config.txt` con tu API key (ejemplos arriba). Después: *Configurar WiFi* (red de 2,4 GHz) → *Preguntar*. La tecla `_` sirve de espacio.

Recuerda las 3 últimas preguntas hasta que elijas *Nueva conversación*.

## Credits
- Made by Pablo Malia Gil, built with the help of Claude.
- Uses the [FlipperHTTP](https://github.com/jblanked/FlipperHTTP) firmware by JBlanked.

## License
MIT
