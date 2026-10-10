# Guía de instalación paso a paso (español)

## Qué necesitas
- Un **Flipper Zero** con firmware reciente (probado en **Momentum MNTM-012**).
- La **placa WiFi Dev Board** (ESP32-S2) con el firmware **FlipperHTTP**
  (https://github.com/jblanked/FlipperHTTP). Si ya usas apps como FlipWiFi,
  FlipSocial o Web Crawler, ya lo tienes.
- Una **API key** de una IA. La forma gratis es **Groq** (console.groq.com).
- **qFlipper** en el PC para copiar archivos a la SD.

## 1. Instalar la app
1. Descarga `flipper_app/dist/flipper_ai.fap` de este repositorio.
2. Abre qFlipper → pestaña de archivos → `SD Card/apps/GPIO/`.
3. Clic derecho → *Upload here* → elige `flipper_ai.fap`.

> El `.fap` está compilado para Momentum MNTM-012. Si usas otro firmware
> (oficial, Unleashed, RogueMaster…) compílalo tú con `ufbt` (ver README).

## 2. Configurar la API key
1. Abre la app una vez en el Flipper (**Apps → GPIO → Flipper AI**). Crea
   el archivo `SD Card/apps_data/flipper_ai/config.txt`.
2. En el PC, copia uno de los ejemplos de la carpeta `examples/`
   (`config_groq.txt` es el gratis), pega tu API key y guárdalo como
   `config.txt`.
3. Súbelo con qFlipper a `SD Card/apps_data/flipper_ai/` sustituyendo el que hay.
4. En la app, **Ver configuración** debe decir `API key: OK`.

## 3. Conectar la placa a tu WiFi
1. Conecta la placa WiFi al Flipper.
2. En la app: **Probar placa WiFi** → debe decir *Placa WiFi OK (FlipperHTTP)*.
3. **Configurar WiFi** → escribe el nombre de tu red y la contraseña.
   Tiene que ser una red de **2,4 GHz** (la ESP32 no ve las de 5 GHz).

## 4. Preguntar
1. **Preguntar** → escribe → pulsa **save**.
2. Espera unos segundos y lee la respuesta con arriba/abajo.
3. La IA recuerda las 3 últimas preguntas. **Nueva conversación** la reinicia.

Trucos del teclado del Flipper:
- La tecla `_` hace de **espacio**.
- La tecla con la flecha cambia mayúsculas/minúsculas.
- No hay tildes: escribe sin ellas, la IA lo entiende igual.

## Problemas frecuentes
| Mensaje | Qué hacer |
|---|---|
| La placa WiFi no responde | Revisa que esté bien conectada y con FlipperHTTP. Pulsa RESET en la placa. |
| Not connected to WiFi | Usa *Configurar WiFi*. Red de 2,4 GHz. |
| Error HTTP 401 | API key incorrecta. Revisa `config.txt`. |
| Error HTTP 404 model does not exist | El proveedor retiró el modelo: cambia la línea `model=`. |
| Error HTTP 429 | Has superado el límite gratis de Groq. Espera un poco. |
| No se pudo abrir la UART | Cierra otras apps GPIO o desactiva el log por UART. |

## Seguridad
Tu API key se guarda en texto plano en la SD. No compartas el archivo
`config.txt` ni subas tu SD a ningún sitio.
