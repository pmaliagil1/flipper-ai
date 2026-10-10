/*
 * Flipper AI - Chatea con Claude (u otra IA) desde tu Flipper Zero.
 *
 * Usa la placa WiFi (ESP32-S2) con el firmware FlipperHTTP de jblanked,
 * asi que no hace falta flashear nada especial en la placa.
 *
 * Protocolo FlipperHTTP (UART pines 13/14, 115200 baudios):
 *   -> [POST/HTTP]{"url":..,"headers":{..},"payload":{..}}
 *   <- [POST/SUCCESS]{"Status-Code":200,...}
 *   <- <cuerpo de la respuesta>
 *   <- [POST/END]
 *   -> [PING]                      <- [PONG]
 *   -> [WIFI/SAVE]{"ssid":..,"password":..}   <- [SUCCESS]/[ERROR]
 *
 * Configuracion en la SD: /ext/apps_data/flipper_ai/config.txt
 */

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <gui/modules/text_box.h>
#include <expansion/expansion.h>
#include <storage/storage.h>
#include <toolbox/stream/file_stream.h>
#include <notification/notification_messages.h>

#define TAG "FlipperAI"
#define BAUDRATE 115200
#define INPUT_SIZE 256
#define RX_LINE_MAX 12000
#define HISTORY_MAX 6 // mensajes recordados (3 preguntas + 3 respuestas)
#define HISTORY_MSG_MAX 600 // caracteres maximos por mensaje guardado
#define TIMEOUT_MS 60000
#define TX_CHUNK 16
#define TX_CHUNK_DELAY_MS 8
#define MAX_RETRIES 2

#define CONFIG_DIR EXT_PATH("apps_data/flipper_ai")
#define CONFIG_PATH CONFIG_DIR "/config.txt"

#define SYSTEM_PROMPT                                                                       \
    "Eres un asistente de IA. El usuario te escribe desde su Flipper Zero y " \
    "lees en su pantalla pequena. Responde en espanol, breve (max. 80 palabras), " \
    "texto plano, sin markdown, emojis ni tablas."

typedef enum {
    ViewSubmenu,
    ViewInput,
    ViewText,
} ViewId;

typedef enum {
    MenuAsk,
    MenuLast,
    MenuReset,
    MenuPing,
    MenuWifi,
    MenuConfig,
    MenuHelp,
} MenuItem;

typedef enum {
    EvtDone = 100,
    EvtTimeout,
} CustomEvt;

typedef enum {
    WorkerFlagStop = (1 << 0),
    WorkerFlagRx = (1 << 1),
} WorkerFlag;

typedef enum {
    ReqNone,
    ReqAsk,
    ReqPing,
    ReqWifi,
} ReqType;

typedef enum {
    InputQuestion,
    InputSsid,
    InputPass,
} InputMode;

typedef struct {
    bool is_user;
    FuriString* text;
} HistMsg;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    TextInput* text_input;
    TextBox* text_box;
    NotificationApp* notifications;

    char input[INPUT_SIZE];
    char ssid[64];
    InputMode input_mode;

    FuriString* display;
    FuriString* last_answer;

    // Config
    bool is_claude;
    FuriString* api_key;
    FuriString* model;
    FuriString* url;
    FuriString* reasoning;
    int max_tokens;

    // Historial de conversacion
    HistMsg history[HISTORY_MAX];
    size_t history_len;
    FuriString* pending_question;
    FuriString* last_cmd;
    uint8_t retries;

    // UART / estado de la peticion (protegido por mutex)
    FuriMutex* mutex;
    ReqType req;
    ReqType done_req;
    bool capturing;
    int status_code;
    bool req_ok;
    FuriString* line;
    FuriString* body;
    FuriString* last_line;
    uint32_t rx_bytes;
    uint32_t rx_lines;

    Expansion* expansion;
    FuriHalSerialHandle* serial;
    FuriStreamBuffer* rx_stream;
    FuriThread* worker;
    FuriTimer* timer;
} App;

/* ================= Utilidades de texto ================= */

static void json_escape_append(FuriString* out, const char* s) {
    for(; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch(c) {
        case '"':
            furi_string_cat(out, "\\\"");
            break;
        case '\\':
            furi_string_cat(out, "\\\\");
            break;
        case '\n':
            furi_string_cat(out, "\\n");
            break;
        case '\r':
            break;
        case '\t':
            furi_string_cat(out, " ");
            break;
        default:
            if(c >= 0x20) furi_string_push_back(out, (char)c);
        }
    }
}

// Convierte un codigo Unicode a ASCII aproximado (la fuente del Flipper es ASCII)
static void append_codepoint_ascii(FuriString* out, uint32_t cp) {
    if(cp == '\n' || (cp >= 0x20 && cp < 0x7F)) {
        furi_string_push_back(out, (char)cp);
        return;
    }
    const char* r = NULL;
    switch(cp) {
    case 0xE1: case 0xE0: case 0xE4: case 0xE2: case 0xE3: r = "a"; break;
    case 0xE9: case 0xE8: case 0xEB: case 0xEA: r = "e"; break;
    case 0xED: case 0xEC: case 0xEF: case 0xEE: r = "i"; break;
    case 0xF3: case 0xF2: case 0xF6: case 0xF4: case 0xF5: r = "o"; break;
    case 0xFA: case 0xF9: case 0xFC: case 0xFB: r = "u"; break;
    case 0xC1: case 0xC0: case 0xC4: case 0xC2: r = "A"; break;
    case 0xC9: case 0xC8: case 0xCB: case 0xCA: r = "E"; break;
    case 0xCD: case 0xCC: r = "I"; break;
    case 0xD3: case 0xD2: case 0xD6: r = "O"; break;
    case 0xDA: case 0xD9: case 0xDC: r = "U"; break;
    case 0xF1: r = "n"; break;
    case 0xD1: r = "N"; break;
    case 0xE7: r = "c"; break;
    case 0xC7: r = "C"; break;
    case 0x2018: case 0x2019: r = "'"; break;
    case 0x201C: case 0x201D: case 0xAB: case 0xBB: r = "\""; break;
    case 0x2013: case 0x2014: case 0x2022: r = "-"; break;
    case 0x2026: r = "..."; break;
    case 0xB0: r = " grados"; break;
    case 0x20AC: r = "EUR"; break;
    case 0xD7: r = "x"; break;
    case 0x2192: r = "->"; break;
    default: r = NULL; break; // ¿ ¡ emojis, etc.: fuera
    }
    if(r) furi_string_cat(out, r);
}

// Busca "key":"..." en un JSON y devuelve el valor des-escapado y en ASCII.
static bool json_get_string(const char* json, const char* key, FuriString* out) {
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char* p = json;
    while((p = strstr(p, pattern)) != NULL) {
        p += strlen(pattern);
        while(*p == ' ') p++;
        if(*p != ':') continue;
        p++;
        while(*p == ' ') p++;
        if(*p != '"') continue;
        p++;
        break;
    }
    if(!p) return false;

    furi_string_reset(out);
    while(*p && *p != '"') {
        uint32_t cp;
        if(*p == '\\') {
            p++;
            switch(*p) {
            case 'n': cp = '\n'; p++; break;
            case 't': cp = ' '; p++; break;
            case 'r': cp = 0; p++; break;
            case 'b': case 'f': cp = 0; p++; break;
            case 'u': {
                cp = 0;
                p++;
                for(int i = 0; i < 4 && *p; i++, p++) {
                    char h = *p;
                    cp <<= 4;
                    if(h >= '0' && h <= '9') cp |= h - '0';
                    else if(h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                    else if(h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                }
                if(cp >= 0xD800 && cp <= 0xDFFF) cp = 0; // pares sustitutos (emojis)
                break;
            }
            case 0: cp = 0; break;
            default: cp = (unsigned char)*p; p++; break;
            }
        } else {
            unsigned char c = (unsigned char)*p;
            if(c < 0x80) {
                cp = c;
                p++;
            } else if((c & 0xE0) == 0xC0 && p[1]) {
                cp = ((c & 0x1F) << 6) | (p[1] & 0x3F);
                p += 2;
            } else if((c & 0xF0) == 0xE0 && p[1] && p[2]) {
                cp = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
                p += 3;
            } else if((c & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
                cp = 0;
                p += 4;
            } else {
                cp = 0;
                p++;
            }
        }
        if(cp) append_codepoint_ascii(out, cp);
    }
    return true;
}

/* ================= Configuracion ================= */

static void config_write_template(Storage* storage) {
    storage_simply_mkdir(storage, CONFIG_DIR);
    Stream* s = file_stream_alloc(storage);
    if(file_stream_open(s, CONFIG_PATH, FSAM_WRITE, FSOM_CREATE_NEW)) {
        stream_write_cstring(
            s,
            "# Flipper AI - configuracion\n"
            "# provider: claude u openai (cualquier API compatible con OpenAI)\n"
            "provider=claude\n"
            "api_key=PEGA_AQUI_TU_API_KEY\n"
            "model=claude-haiku-5-5\n"
            "# url opcional (por defecto la de cada proveedor), p. ej. Groq:\n"
            "# url=https://api.groq.com/openai/v1/chat/completions\n");
    }
    file_stream_close(s);
    stream_free(s);
}

static void config_load(App* app) {
    app->is_claude = true;
    furi_string_reset(app->api_key);
    furi_string_set(app->model, "claude-haiku-5-5");
    furi_string_reset(app->url);
    furi_string_reset(app->reasoning);
    app->max_tokens = 300;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    if(!storage_file_exists(storage, CONFIG_PATH)) config_write_template(storage);

    Stream* s = file_stream_alloc(storage);
    FuriString* line = furi_string_alloc();
    bool model_set = false;
    if(file_stream_open(s, CONFIG_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        while(stream_read_line(s, line)) {
            furi_string_trim(line);
            if(furi_string_start_with_str(line, "#")) continue;
            size_t eq = furi_string_search_char(line, '=');
            if(eq == FURI_STRING_FAILURE) continue;
            FuriString* k = furi_string_alloc_set(line);
            FuriString* v = furi_string_alloc_set(line);
            furi_string_left(k, eq);
            furi_string_right(v, eq + 1);
            furi_string_trim(k);
            furi_string_trim(v);
            if(furi_string_cmp_str(k, "provider") == 0) {
                app->is_claude = furi_string_cmpi_str(v, "openai") != 0;
            } else if(furi_string_cmp_str(k, "api_key") == 0) {
                furi_string_set(app->api_key, v);
            } else if(furi_string_cmp_str(k, "model") == 0 && !furi_string_empty(v)) {
                furi_string_set(app->model, v);
                model_set = true;
            } else if(furi_string_cmp_str(k, "url") == 0) {
                furi_string_set(app->url, v);
            } else if(furi_string_cmp_str(k, "reasoning_effort") == 0) {
                furi_string_set(app->reasoning, v);
            } else if(furi_string_cmp_str(k, "max_tokens") == 0) {
                int mt = atoi(furi_string_get_cstr(v));
                if(mt >= 50 && mt <= 8000) app->max_tokens = mt;
            }
            furi_string_free(k);
            furi_string_free(v);
        }
    }
    furi_string_free(line);
    file_stream_close(s);
    stream_free(s);
    furi_record_close(RECORD_STORAGE);

    if(!app->is_claude && !model_set) furi_string_set(app->model, "gpt-4o-mini");
    if(furi_string_empty(app->url)) {
        furi_string_set(
            app->url,
            app->is_claude ? "https://api.anthropic.com/v1/messages" :
                             "https://api.openai.com/v1/chat/completions");
    }
}

static bool config_has_key(App* app) {
    return furi_string_size(app->api_key) > 10 &&
           !furi_string_start_with_str(app->api_key, "PEGA_AQUI");
}

/* ================= Historial ================= */

static void history_clear(App* app) {
    for(size_t i = 0; i < app->history_len; i++) furi_string_free(app->history[i].text);
    app->history_len = 0;
}

static void history_push(App* app, bool is_user, const char* text) {
    if(app->history_len == HISTORY_MAX) {
        // quitar la pareja mas antigua
        furi_string_free(app->history[0].text);
        furi_string_free(app->history[1].text);
        for(size_t i = 2; i < HISTORY_MAX; i++) app->history[i - 2] = app->history[i];
        app->history_len -= 2;
    }
    FuriString* t = furi_string_alloc_set(text);
    if(furi_string_size(t) > HISTORY_MSG_MAX) furi_string_left(t, HISTORY_MSG_MAX);
    app->history[app->history_len].is_user = is_user;
    app->history[app->history_len].text = t;
    app->history_len++;
}

/* ================= UART ================= */

static void uart_rx_cb(FuriHalSerialHandle* handle, FuriHalSerialRxEvent event, void* context) {
    App* app = context;
    if(event == FuriHalSerialRxEventData) {
        uint8_t byte = furi_hal_serial_async_rx(handle);
        furi_stream_buffer_send(app->rx_stream, &byte, 1, 0);
        furi_thread_flags_set(furi_thread_get_id(app->worker), WorkerFlagRx);
    }
}

// Procesa una linea completa recibida de la placa. Se llama con el mutex cogido.
// Devuelve true si la peticion actual ha terminado.
static bool process_line(App* app, FuriString* line) {
    const char* l = furi_string_get_cstr(line);
    switch(app->req) {
    case ReqAsk:
        if(furi_string_start_with_str(line, "[POST/SUCCESS]")) {
            app->capturing = true;
            const char* sc = strstr(l, "\"Status-Code\":");
            app->status_code = sc ? atoi(sc + 14) : 0;
            furi_string_reset(app->body);
            return false;
        }
        if(furi_string_start_with_str(line, "[POST/END]")) {
            app->req_ok = true;
            return true;
        }
        if(furi_string_start_with_str(line, "[ERROR]")) {
            furi_string_set(app->body, line);
            app->req_ok = false;
            return true;
        }
        if(app->capturing && furi_string_size(app->body) < RX_LINE_MAX) {
            furi_string_cat(app->body, line);
        }
        return false;
    case ReqPing:
        if(furi_string_search_str(line, "[PONG]") != FURI_STRING_FAILURE) {
            app->req_ok = true;
            return true;
        }
        return false;
    case ReqWifi:
        if(furi_string_start_with_str(line, "[SUCCESS]") ||
           furi_string_start_with_str(line, "[INFO]")) {
            furi_string_set(app->body, line);
            app->req_ok = true;
            return true;
        }
        if(furi_string_start_with_str(line, "[ERROR]")) {
            furi_string_set(app->body, line);
            app->req_ok = false;
            return true;
        }
        return false;
    default:
        return false;
    }
}

static int32_t uart_worker(void* context) {
    App* app = context;
    uint8_t buf[64];

    while(true) {
        uint32_t flags =
            furi_thread_flags_wait(WorkerFlagStop | WorkerFlagRx, FuriFlagWaitAny, FuriWaitForever);
        if(flags & FuriFlagError) continue;
        if(flags & WorkerFlagStop) break;

        size_t len;
        while((len = furi_stream_buffer_receive(app->rx_stream, buf, sizeof(buf), 0)) > 0) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            for(size_t i = 0; i < len; i++) {
                char c = (char)buf[i];
                app->rx_bytes++;
                if(c == '\n') {
                    app->rx_lines++;
                    if(furi_string_size(app->line) > 1) {
                        furi_string_set(app->last_line, app->line);
                        if(furi_string_size(app->last_line) > 160)
                            furi_string_left(app->last_line, 160);
                    }
                    furi_string_trim(app->line, "\r");
                    if(app->req != ReqNone && process_line(app, app->line)) {
                        app->done_req = app->req;
                        app->req = ReqNone;
                        view_dispatcher_send_custom_event(app->view_dispatcher, EvtDone);
                    }
                    furi_string_reset(app->line);
                } else if(furi_string_size(app->line) < RX_LINE_MAX) {
                    furi_string_push_back(app->line, c);
                    // "[ERROR] Failed to parse JSON." llega sin salto de linea
                    if(c == '.' && app->req != ReqNone &&
                       furi_string_start_with_str(app->line, "[ERROR]")) {
                        if(process_line(app, app->line)) {
                            app->done_req = app->req;
                            app->req = ReqNone;
                            view_dispatcher_send_custom_event(app->view_dispatcher, EvtDone);
                        }
                        furi_string_reset(app->line);
                    }
                }
            }
            furi_mutex_release(app->mutex);
        }
    }
    return 0;
}

static void uart_start_request(App* app, ReqType type, const char* cmd) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->req = type;
    app->capturing = false;
    app->req_ok = false;
    app->status_code = 0;
    furi_string_reset(app->body);
    furi_string_reset(app->line);
    furi_string_reset(app->last_line);
    app->rx_bytes = 0;
    app->rx_lines = 0;
    furi_mutex_release(app->mutex);

    // Enviar a trozos y despacio: el buffer de recepcion de la ESP32 es pequeno
    // y las lineas largas (>~550 bytes) se corrompen si se mandan de golpe.
    size_t len = strlen(cmd);
    for(size_t off = 0; off < len; off += TX_CHUNK) {
        size_t n = len - off < TX_CHUNK ? len - off : TX_CHUNK;
        furi_hal_serial_tx(app->serial, (const uint8_t*)cmd + off, n);
        furi_hal_serial_tx_wait_complete(app->serial);
        furi_delay_ms(TX_CHUNK_DELAY_MS);
    }
    furi_hal_serial_tx(app->serial, (const uint8_t*)"\n", 1);
    furi_hal_serial_tx_wait_complete(app->serial);

    furi_timer_start(app->timer, furi_ms_to_ticks(type == ReqPing ? 5000 : TIMEOUT_MS));
}

static void timer_cb(void* context) {
    App* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, EvtTimeout);
}

/* ================= UI ================= */

static void show_text(App* app, const char* text) {
    furi_string_set(app->display, text);
    text_box_reset(app->text_box);
    text_box_set_font(app->text_box, TextBoxFontText);
    text_box_set_text(app->text_box, furi_string_get_cstr(app->display));
    text_box_set_focus(app->text_box, TextBoxFocusStart);
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewText);
}

static void send_question(App* app, const char* question) {
    FuriString* cmd = furi_string_alloc();
    furi_string_cat(cmd, "[POST/HTTP]{\"url\":\"");
    json_escape_append(cmd, furi_string_get_cstr(app->url));
    furi_string_cat(cmd, "\",\"headers\":{\"Content-Type\":\"application/json\",");
    if(app->is_claude) {
        furi_string_cat(cmd, "\"x-api-key\":\"");
        json_escape_append(cmd, furi_string_get_cstr(app->api_key));
        furi_string_cat(cmd, "\",\"anthropic-version\":\"2023-06-01\"}");
    } else {
        furi_string_cat(cmd, "\"Authorization\":\"Bearer ");
        json_escape_append(cmd, furi_string_get_cstr(app->api_key));
        furi_string_cat(cmd, "\"}");
    }
    furi_string_cat(cmd, ",\"payload\":{\"model\":\"");
    json_escape_append(cmd, furi_string_get_cstr(app->model));
    furi_string_cat_printf(cmd, "\",\"max_tokens\":%d,", app->max_tokens);
    if(!furi_string_empty(app->reasoning)) {
        furi_string_cat(cmd, "\"reasoning_effort\":\"");
        json_escape_append(cmd, furi_string_get_cstr(app->reasoning));
        furi_string_cat(cmd, "\",");
    }
    if(app->is_claude) {
        furi_string_cat(cmd, "\"system\":\"" SYSTEM_PROMPT "\",\"messages\":[");
    } else {
        furi_string_cat(cmd, "\"messages\":[{\"role\":\"system\",\"content\":\"" SYSTEM_PROMPT "\"},");
    }
    for(size_t i = 0; i < app->history_len; i++) {
        furi_string_cat_printf(
            cmd,
            "{\"role\":\"%s\",\"content\":\"",
            app->history[i].is_user ? "user" : "assistant");
        json_escape_append(cmd, furi_string_get_cstr(app->history[i].text));
        furi_string_cat(cmd, "\"},");
    }
    furi_string_cat(cmd, "{\"role\":\"user\",\"content\":\"");
    json_escape_append(cmd, question);
    furi_string_cat(cmd, "\"}]}}");

    furi_string_set(app->pending_question, question);
    furi_string_set(app->last_cmd, cmd);
    app->retries = 0;
    uart_start_request(app, ReqAsk, furi_string_get_cstr(cmd));
    furi_string_free(cmd);
}

static void input_done_cb(void* context);

static void open_input_with_cb(App* app, InputMode mode) {
    app->input_mode = mode;
    memset(app->input, 0, sizeof(app->input));
    text_input_reset(app->text_input);
    const char* header = mode == InputQuestion ? "Tu pregunta:" :
                         mode == InputSsid     ? "Nombre de la WiFi (SSID):" :
                                                 "Contrasena de la WiFi:";
    text_input_set_header_text(app->text_input, header);
    size_t size = mode == InputQuestion ? INPUT_SIZE : 64;
    text_input_set_result_callback(app->text_input, input_done_cb, app, app->input, size, true);
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewInput);
}

static void input_done_cb(void* context) {
    App* app = context;
    if(strlen(app->input) == 0) {
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewSubmenu);
        return;
    }
    switch(app->input_mode) {
    case InputQuestion:
        // En el teclado del Flipper la tecla de espacio escribe '_'
        for(char* c = app->input; *c; c++)
            if(*c == '_') *c = ' ';
        send_question(app, app->input);
        show_text(app, "Pensando...\n\n(Atras para cancelar)");
        break;
    case InputSsid:
        strlcpy(app->ssid, app->input, sizeof(app->ssid));
        open_input_with_cb(app, InputPass);
        break;
    case InputPass: {
        FuriString* cmd = furi_string_alloc_set("[WIFI/SAVE]{\"ssid\":\"");
        json_escape_append(cmd, app->ssid);
        furi_string_cat(cmd, "\",\"password\":\"");
        json_escape_append(cmd, app->input);
        furi_string_cat(cmd, "\"}");
        uart_start_request(app, ReqWifi, furi_string_get_cstr(cmd));
        furi_string_free(cmd);
        memset(app->input, 0, sizeof(app->input));
        show_text(app, "Conectando a la WiFi...");
        break;
    }
    }
}

static void show_config(App* app) {
    config_load(app);
    FuriString* t = furi_string_alloc();
    furi_string_printf(
        t,
        "Proveedor: %s\nModelo: %s\nAPI key: %s\n\nArchivo de config:\nSD/apps_data/flipper_ai/\nconfig.txt\n\n"
        "Editalo con qFlipper\n(Archivos) y vuelve\na abrir este menu.",
        app->is_claude ? "Claude" : "OpenAI/compat.",
        furi_string_get_cstr(app->model),
        config_has_key(app) ? "OK" : "FALTA");
    show_text(app, furi_string_get_cstr(t));
    furi_string_free(t);
}

static void submenu_cb(void* context, uint32_t index) {
    App* app = context;
    switch(index) {
    case MenuAsk:
        config_load(app);
        if(!config_has_key(app)) {
            show_text(
                app,
                "Falta la API key.\n\nPonla en la SD, en\napps_data/flipper_ai/\nconfig.txt\n(linea api_key=...)");
            return;
        }
        open_input_with_cb(app, InputQuestion);
        break;
    case MenuLast:
        show_text(
            app,
            furi_string_empty(app->last_answer) ? "Aun no hay respuestas." :
                                                  furi_string_get_cstr(app->last_answer));
        break;
    case MenuReset:
        history_clear(app);
        furi_string_reset(app->last_answer);
        show_text(app, "Conversacion borrada.\nPuedes empezar otra.");
        break;
    case MenuPing:
        uart_start_request(app, ReqPing, "[PING]");
        show_text(app, "Comprobando placa WiFi...");
        break;
    case MenuWifi:
        open_input_with_cb(app, InputSsid);
        break;
    case MenuConfig:
        show_config(app);
        break;
    case MenuHelp:
        show_text(
            app,
            "Flipper AI\n\n"
            "Necesita la placa WiFi\n"
            "con firmware FlipperHTTP.\n\n"
            "1. Configurar WiFi (solo\n"
            "   la primera vez).\n"
            "2. Pon tu API key en\n"
            "   apps_data/flipper_ai/\n"
            "   config.txt\n"
            "3. Preguntar. Usa\n"
            "   arriba/abajo para leer.\n\n"
            "La IA recuerda la charla\n"
            "hasta que la borres.");
        break;
    }
}

// Los bytes a veces se corrompen por la UART: si la placa o la API dicen que el
// JSON es invalido, reintentamos la misma peticion.
static bool should_retry(App* app) {
    if(app->retries >= MAX_RETRIES) return false;
    const char* body = furi_string_get_cstr(app->body);
    if(!app->req_ok) return strstr(body, "parse JSON") != NULL;
    return app->status_code == 400;
}

static void finish_ask(App* app) {
    if(should_retry(app)) {
        app->retries++;
        FuriString* t = furi_string_alloc_printf("Pensando... (reintento %u)", app->retries);
        show_text(app, furi_string_get_cstr(t));
        furi_string_free(t);
        uart_start_request(app, ReqAsk, furi_string_get_cstr(app->last_cmd));
        return;
    }
    FuriString* text = furi_string_alloc();
    const char* body = furi_string_get_cstr(app->body);

    if(!app->req_ok) {
        furi_string_printf(text, "Error de la placa:\n%s", body);
    } else if(app->status_code == 200) {
        bool ok = json_get_string(body, app->is_claude ? "text" : "content", text);
        if(!ok || furi_string_empty(text)) {
            furi_string_set(text, "(respuesta vacia)");
        } else {
            furi_string_trim(text);
            history_push(app, true, furi_string_get_cstr(app->pending_question));
            history_push(app, false, furi_string_get_cstr(text));
        }
    } else {
        FuriString* msg = furi_string_alloc();
        if(!json_get_string(body, "message", msg)) furi_string_set(msg, body);
        furi_string_printf(text, "Error HTTP %d:\n%s", app->status_code, furi_string_get_cstr(msg));
        if(app->status_code == 401) furi_string_cat(text, "\n\nRevisa la API key.");
        furi_string_free(msg);
    }

    furi_string_set(app->last_answer, text);
    show_text(app, furi_string_get_cstr(text));
    furi_string_free(text);
}

static bool custom_event_cb(void* context, uint32_t event) {
    App* app = context;
    if(event == EvtDone) {
        furi_timer_stop(app->timer);
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        ReqType r = app->done_req;
        app->done_req = ReqNone;
        furi_mutex_release(app->mutex);
        if(r == ReqAsk) {
            finish_ask(app);
            notification_message(app->notifications, &sequence_single_vibro);
        } else if(r == ReqPing) {
            show_text(
                app,
                "Placa WiFi OK (FlipperHTTP)\n\nSi aun no la has conectado\na tu WiFi, usa\n'Configurar WiFi'.");
        } else if(r == ReqWifi) {
            FuriString* t = furi_string_alloc_printf(
                "%s\n\n%s",
                app->req_ok ? "WiFi configurada." : "No se pudo conectar.",
                furi_string_get_cstr(app->body));
            show_text(app, furi_string_get_cstr(t));
            furi_string_free(t);
        }
        return true;
    }
    if(event == EvtTimeout) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        ReqType r = app->req;
        app->req = ReqNone;
        furi_mutex_release(app->mutex);
        if(r == ReqPing) {
            show_text(
                app,
                "La placa WiFi no responde.\n\nComprueba que esta bien\nconectada y que tiene el\nfirmware FlipperHTTP.\nPrueba a pulsar RESET.");
        } else if(
            r == ReqAsk && app->retries < MAX_RETRIES &&
            furi_string_search_str(app->line, "parse JSON") != FURI_STRING_FAILURE) {
            app->retries++;
            show_text(app, "Pensando... (reintento)");
            uart_start_request(app, ReqAsk, furi_string_get_cstr(app->last_cmd));
        } else if(r != ReqNone && furi_string_start_with_str(app->line, "[ERROR]")) {
            FuriString* t = furi_string_alloc_printf(
                "Error de la placa:\n%s", furi_string_get_cstr(app->line));
            show_text(app, furi_string_get_cstr(t));
            furi_string_free(t);
        } else if(r != ReqNone) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            FuriString* t = furi_string_alloc_printf(
                "Tiempo de espera agotado.\n\nDiag: %lu bytes, %lu lineas,\ncapturando=%d, HTTP %d,\ncuerpo=%u, linea=%u\nUltima linea:\n%s",
                (unsigned long)app->rx_bytes,
                (unsigned long)app->rx_lines,
                app->capturing,
                app->status_code,
                (unsigned)furi_string_size(app->body),
                (unsigned)furi_string_size(app->line),
                furi_string_get_cstr(app->last_line));
            furi_mutex_release(app->mutex);
            show_text(app, furi_string_get_cstr(t));
            furi_string_free(t);
        }
        return true;
    }
    return false;
}

static uint32_t exit_cb(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

static uint32_t back_to_menu_cb(void* context) {
    UNUSED(context);
    return ViewSubmenu;
}

/* ================= Ciclo de vida ================= */

int32_t flipper_ai_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));

    app->expansion = furi_record_open(RECORD_EXPANSION);
    expansion_disable(app->expansion);

    app->display = furi_string_alloc();
    app->last_answer = furi_string_alloc();
    app->api_key = furi_string_alloc();
    app->model = furi_string_alloc();
    app->url = furi_string_alloc();
    app->reasoning = furi_string_alloc();
    app->pending_question = furi_string_alloc();
    app->last_cmd = furi_string_alloc();
    app->line = furi_string_alloc();
    app->body = furi_string_alloc();
    app->last_line = furi_string_alloc();
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->rx_stream = furi_stream_buffer_alloc(4096, 1);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    config_load(app);

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, custom_event_cb);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->submenu = submenu_alloc();
    submenu_set_header(app->submenu, "Flipper AI");
    submenu_add_item(app->submenu, "Preguntar", MenuAsk, submenu_cb, app);
    submenu_add_item(app->submenu, "Ultima respuesta", MenuLast, submenu_cb, app);
    submenu_add_item(app->submenu, "Nueva conversacion", MenuReset, submenu_cb, app);
    submenu_add_item(app->submenu, "Probar placa WiFi", MenuPing, submenu_cb, app);
    submenu_add_item(app->submenu, "Configurar WiFi", MenuWifi, submenu_cb, app);
    submenu_add_item(app->submenu, "Ver configuracion", MenuConfig, submenu_cb, app);
    submenu_add_item(app->submenu, "Ayuda", MenuHelp, submenu_cb, app);
    view_set_previous_callback(submenu_get_view(app->submenu), exit_cb);
    view_dispatcher_add_view(app->view_dispatcher, ViewSubmenu, submenu_get_view(app->submenu));

    app->text_input = text_input_alloc();
    view_set_previous_callback(text_input_get_view(app->text_input), back_to_menu_cb);
    view_dispatcher_add_view(app->view_dispatcher, ViewInput, text_input_get_view(app->text_input));

    app->text_box = text_box_alloc();
    view_set_previous_callback(text_box_get_view(app->text_box), back_to_menu_cb);
    view_dispatcher_add_view(app->view_dispatcher, ViewText, text_box_get_view(app->text_box));

    app->timer = furi_timer_alloc(timer_cb, FuriTimerTypeOnce, app);

    app->worker = furi_thread_alloc_ex("FlipperAIRx", 2048, uart_worker, app);
    furi_thread_start(app->worker);

    app->serial = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    if(app->serial) {
        furi_hal_serial_init(app->serial, BAUDRATE);
        furi_hal_serial_async_rx_start(app->serial, uart_rx_cb, app, false);
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewSubmenu);
    } else {
        FURI_LOG_E(TAG, "UART ocupada");
        show_text(
            app,
            "No se pudo abrir la UART.\n\nCierra otras apps GPIO o\ndesactiva el log por UART\nen Ajustes > Sistema.");
    }

    view_dispatcher_run(app->view_dispatcher);

    furi_timer_stop(app->timer);
    if(app->serial) {
        furi_hal_serial_async_rx_stop(app->serial);
        furi_hal_serial_deinit(app->serial);
        furi_hal_serial_control_release(app->serial);
    }
    furi_thread_flags_set(furi_thread_get_id(app->worker), WorkerFlagStop);
    furi_thread_join(app->worker);
    furi_thread_free(app->worker);
    furi_timer_free(app->timer);

    view_dispatcher_remove_view(app->view_dispatcher, ViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, ViewInput);
    view_dispatcher_remove_view(app->view_dispatcher, ViewText);
    submenu_free(app->submenu);
    text_input_free(app->text_input);
    text_box_free(app->text_box);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);

    history_clear(app);
    furi_stream_buffer_free(app->rx_stream);
    furi_mutex_free(app->mutex);
    furi_string_free(app->display);
    furi_string_free(app->last_answer);
    furi_string_free(app->api_key);
    furi_string_free(app->model);
    furi_string_free(app->url);
    furi_string_free(app->reasoning);
    furi_string_free(app->pending_question);
    furi_string_free(app->last_cmd);
    furi_string_free(app->line);
    furi_string_free(app->body);
    furi_string_free(app->last_line);

    expansion_enable(app->expansion);
    furi_record_close(RECORD_EXPANSION);

    free(app);
    return 0;
}
