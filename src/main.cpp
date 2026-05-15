#include <Arduino.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h>
#include <driver/i2s.h>

#if ENABLE_OLED_DISPLAY
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#endif

#ifndef MIC_SAMPLE_RATE
#define MIC_SAMPLE_RATE 16000
#endif

#ifndef MIC_FRAME_SAMPLES
#define MIC_FRAME_SAMPLES 256
#endif

#ifndef I2S_BCLK_PIN
#define I2S_BCLK_PIN 5
#endif

#ifndef I2S_LRCLK_PIN
#define I2S_LRCLK_PIN 4
#endif

#ifndef I2S_DATA_PIN
#define I2S_DATA_PIN 6
#endif

#ifndef MIC_SAMPLE_SHIFT
#define MIC_SAMPLE_SHIFT 11
#endif

#ifndef ENABLE_ES8311_CODEC
#define ENABLE_ES8311_CODEC 0
#endif

#ifndef ES8311_ADDR
#define ES8311_ADDR 0x18
#endif

#ifndef ES8311_I2C_SDA_PIN
#define ES8311_I2C_SDA_PIN OLED_SDA_PIN
#endif

#ifndef ES8311_I2C_SCL_PIN
#define ES8311_I2C_SCL_PIN OLED_SCL_PIN
#endif

#ifndef I2S_DOUT_PIN
#define I2S_DOUT_PIN SPEAKER_DATA_PIN
#endif

#ifndef I2S_MCLK_PIN
#define I2S_MCLK_PIN I2S_PIN_NO_CHANGE
#endif

#ifndef PA_ENABLE_PIN
#define PA_ENABLE_PIN -1
#endif

#ifndef ENABLE_SPEAKER
#define ENABLE_SPEAKER 0
#endif

#ifndef SPEAKER_BCLK_PIN
#define SPEAKER_BCLK_PIN 7
#endif

#ifndef SPEAKER_LRCLK_PIN
#define SPEAKER_LRCLK_PIN 15
#endif

#ifndef SPEAKER_DATA_PIN
#define SPEAKER_DATA_PIN 16
#endif

#ifndef SPEAKER_GAIN_PERCENT
#define SPEAKER_GAIN_PERCENT 70
#endif

#ifndef ENABLE_OLED_DISPLAY
#define ENABLE_OLED_DISPLAY 0
#endif

#ifndef OLED_SDA_PIN
#define OLED_SDA_PIN 8
#endif

#ifndef OLED_SCL_PIN
#define OLED_SCL_PIN 9
#endif

#ifndef OLED_WIDTH
#define OLED_WIDTH 128
#endif

#ifndef OLED_HEIGHT
#define OLED_HEIGHT 64
#endif

static constexpr i2s_port_t I2S_PORT = I2S_NUM_0;
#if SOC_I2S_NUM > 1
static constexpr i2s_port_t SPEAKER_I2S_PORT = I2S_NUM_1;
#else
static constexpr i2s_port_t SPEAKER_I2S_PORT = I2S_NUM_0;
#endif
static constexpr uint32_t AUDIO_MAGIC = 0x30445541; // "AUD0", little-endian on the wire.
static constexpr uint8_t AUDIO_CODEC_PCM16 = 0;
static constexpr uint8_t AUDIO_CODEC_IMA_ADPCM = 1;
static constexpr uint16_t DEFAULT_SERVER_PORT = 8765;
static constexpr uint32_t WIFI_RETRY_MS = 10000;
static constexpr uint32_t TCP_RETRY_MS = 3000;
static constexpr byte DNS_PORT = 53;
static constexpr const char *AP_SSID = "LeOSListener";
static constexpr const char *AP_PASSWORD = "LeOSListener";
static constexpr size_t I2S_WORDS_PER_FRAME = MIC_FRAME_SAMPLES * 2;
static constexpr size_t DOWNLINK_BUFFER_SIZE = 1600;

struct __attribute__((packed)) AudioFrameHeader {
  uint32_t magic;
  uint16_t samples;
  uint16_t rate;
  uint8_t codec;
  uint8_t stepIndex;
  int16_t predictor;
};

struct RuntimeConfig {
  String ssid;
  String password;
  String serverHost;
  uint16_t serverPort;
  uint8_t sampleShift;
  float inputGain;
  uint16_t noiseGate;
  bool highPass;
  bool agcEnabled;
  uint16_t agcTarget;
  uint8_t channelMode;
};

enum CaptureChannelMode : uint8_t {
  CHANNEL_AUTO = 0,
  CHANNEL_LEFT = 1,
  CHANNEL_RIGHT = 2,
};

static Preferences preferences;
static WebServer webServer(80);
static DNSServer dnsServer;
static WiFiClient audioClient;
static RuntimeConfig config;

static int32_t i2sBuffer[I2S_WORDS_PER_FRAME];
static int16_t codecRxBuffer[MIC_FRAME_SAMPLES];
static int16_t pcmBuffer[MIC_FRAME_SAMPLES];
static int16_t downlinkPcmBuffer[MIC_FRAME_SAMPLES];
static uint8_t adpcmBuffer[(MIC_FRAME_SAMPLES + 1) / 2];
static uint8_t txBuffer[sizeof(AudioFrameHeader) + sizeof(pcmBuffer)];
static uint8_t downlinkBuffer[DOWNLINK_BUFFER_SIZE];

static String apName;
static bool apEnabled = false;
static bool speakerReady = false;
#if ENABLE_OLED_DISPLAY
static Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
static bool displayReady = false;
#endif
static uint32_t lastWifiAttemptMs = 0;
static uint32_t lastTcpAttemptMs = 0;
static uint32_t lastStatusMs = 0;
static uint32_t lastDisplayMs = 0;
static size_t lastSamplesRead = 0;
static size_t lastWordsRead = 0;
static size_t downlinkBytesBuffered = 0;
static uint32_t lastDownlinkMs = 0;
static uint32_t statusFramesSent = 0;
static uint32_t statusPayloadBytesSent = 0;
static uint32_t statusDownlinkFrames = 0;
static uint32_t statusDownlinkBytes = 0;
static float hpPrevInput = 0.0f;
static float hpPrevOutput = 0.0f;
static constexpr float HIGH_PASS_ALPHA = 0.955f;
static float agcEnvelope = 2500.0f;
static float agcCurrentGain = 1.0f;
static float slotEnergyLeft = 1.0f;
static float slotEnergyRight = 1.0f;
static uint8_t activeSlot = 0;
static int16_t adpcmPredictor = 0;
static int8_t adpcmStepIndex = 0;

static constexpr int IMA_INDEX_TABLE[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8};
static constexpr int IMA_STEP_TABLE[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

static int32_t abs32(int32_t value) {
  return value < 0 ? -value : value;
}

static uint8_t sanitizeSampleShift(uint8_t value) {
  return value >= 8 && value <= 16 ? value : 12;
}

static uint8_t sanitizeChannelMode(uint8_t value) {
  return value <= CHANNEL_RIGHT ? value : CHANNEL_AUTO;
}

static const char *channelModeLabel(uint8_t mode) {
  switch (sanitizeChannelMode(mode)) {
    case CHANNEL_LEFT:
      return "left";
    case CHANNEL_RIGHT:
      return "right";
    default:
      return "auto";
  }
}

static const char *slotLabel(uint8_t slot) {
#if ENABLE_ES8311_CODEC
  (void)slot;
  return "codec";
#else
  return slot == 0 ? "left" : "right";
#endif
}

static bool i2cWriteReg(uint8_t addr, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

static uint8_t i2cReadReg(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return 0xFF;
  }
  if (Wire.requestFrom(static_cast<int>(addr), 1) != 1) {
    return 0xFF;
  }
  return Wire.read();
}

static bool es8311Write(uint8_t reg, uint8_t value) {
  return i2cWriteReg(ES8311_ADDR, reg, value);
}

static uint8_t es8311Read(uint8_t reg) {
  return i2cReadReg(ES8311_ADDR, reg);
}

static bool setupEs8311Codec() {
#if ENABLE_ES8311_CODEC
  Wire.begin(ES8311_I2C_SDA_PIN, ES8311_I2C_SCL_PIN);
  bool ok = true;
  ok &= es8311Write(0x44, 0x08);
  ok &= es8311Write(0x44, 0x08);
  ok &= es8311Write(0x01, 0x30);
  ok &= es8311Write(0x02, 0x00);
  ok &= es8311Write(0x03, 0x10);
  ok &= es8311Write(0x16, 0x24);
  ok &= es8311Write(0x04, 0x10);
  ok &= es8311Write(0x05, 0x00);
  ok &= es8311Write(0x0B, 0x00);
  ok &= es8311Write(0x0C, 0x00);
  ok &= es8311Write(0x10, 0x1F);
  ok &= es8311Write(0x11, 0x7F);
  ok &= es8311Write(0x00, 0x80);
  ok &= es8311Write(0x00, es8311Read(0x00) & 0xBF); // ES8311 slave, ESP32 provides clocks.
  ok &= es8311Write(0x01, 0x3F);
  ok &= es8311Write(0x01, es8311Read(0x01) & 0x7F); // MCLK from MCLK pin.
  ok &= es8311Write(0x01, es8311Read(0x01) & ~0x40); // Normal MCLK polarity.
  ok &= es8311Write(0x06, es8311Read(0x06) & ~0x20); // Normal BCLK polarity.

  // Clock coefficients for MCLK=12.288MHz, sample rate=16kHz.
  ok &= es8311Write(0x02, 0x40);
  ok &= es8311Write(0x05, 0x00);
  ok &= es8311Write(0x03, 0x10);
  ok &= es8311Write(0x04, 0x20);
  ok &= es8311Write(0x07, 0x00);
  ok &= es8311Write(0x08, 0xFF);
  ok &= es8311Write(0x06, 0x03);

  // I2S normal format, 16-bit samples.
  ok &= es8311Write(0x09, (es8311Read(0x09) & 0xE3) | 0x0C);
  ok &= es8311Write(0x0A, (es8311Read(0x0A) & 0xE3) | 0x0C);

  ok &= es8311Write(0x13, 0x10);
  ok &= es8311Write(0x1B, 0x0A);
  ok &= es8311Write(0x1C, 0x6A);
  ok &= es8311Write(0x09, es8311Read(0x09) & ~0x40);
  ok &= es8311Write(0x0A, es8311Read(0x0A) & ~0x40);
  ok &= es8311Write(0x17, 0xBF);
  ok &= es8311Write(0x0E, 0x02);
  ok &= es8311Write(0x12, 0x00);
  ok &= es8311Write(0x14, 0x1A);
  ok &= es8311Write(0x0D, 0x01);
  ok &= es8311Write(0x15, 0x40);
  ok &= es8311Write(0x37, 0x08);
  ok &= es8311Write(0x45, 0x00);
  ok &= es8311Write(0x44, 0x58);
  ok &= es8311Write(0x32, 0xBF); // DAC volume.
  ok &= es8311Write(0x31, es8311Read(0x31) & 0x9F); // DAC unmute.
  Serial.printf("ES8311 addr=0x%02x chip=%02x:%02x:%02x init=%s\n",
                ES8311_ADDR,
                es8311Read(0xFD),
                es8311Read(0xFE),
                es8311Read(0xFF),
                ok ? "ok" : "partial");
  return ok;
#else
  return true;
#endif
}

static void resetVoiceState() {
  hpPrevInput = 0.0f;
  hpPrevOutput = 0.0f;
  agcEnvelope = 2500.0f;
  agcCurrentGain = 1.0f;
  slotEnergyLeft = 1.0f;
  slotEnergyRight = 1.0f;
  activeSlot = 0;
  adpcmPredictor = 0;
  adpcmStepIndex = 0;
}

static String htmlEscape(const String &value) {
  String escaped;
  escaped.reserve(value.length());
  for (size_t i = 0; i < value.length(); ++i) {
    const char ch = value[i];
    if (ch == '&') escaped += F("&amp;");
    else if (ch == '<') escaped += F("&lt;");
    else if (ch == '>') escaped += F("&gt;");
    else if (ch == '"') escaped += F("&quot;");
    else escaped += ch;
  }
  return escaped;
}

static void loadConfig() {
  preferences.begin("micwifi", false);
  config.ssid = preferences.isKey("ssid") ? preferences.getString("ssid", "") : "";
  config.password = preferences.isKey("pass") ? preferences.getString("pass", "") : "";
  config.serverHost = preferences.isKey("host") ? preferences.getString("host", "") : "";
  config.serverPort = preferences.isKey("port") ? preferences.getUShort("port", DEFAULT_SERVER_PORT) : DEFAULT_SERVER_PORT;
  config.sampleShift = sanitizeSampleShift(preferences.isKey("shift") ? preferences.getUChar("shift", 12) : 12);
  config.inputGain = preferences.isKey("gain") ? preferences.getFloat("gain", 1.2f) : 1.2f;
  config.noiseGate = preferences.isKey("gate") ? preferences.getUShort("gate", 260) : 260;
  config.highPass = preferences.isKey("hpass") ? preferences.getBool("hpass", true) : true;
  config.agcEnabled = preferences.isKey("agc") ? preferences.getBool("agc", true) : true;
  config.agcTarget = preferences.isKey("agctgt") ? preferences.getUShort("agctgt", 6200) : 6200;
  config.channelMode = sanitizeChannelMode(preferences.isKey("chmode") ? preferences.getUChar("chmode", CHANNEL_AUTO) : CHANNEL_AUTO);
  preferences.end();
}

static void saveConfig() {
  preferences.begin("micwifi", false);
  preferences.putString("ssid", config.ssid);
  preferences.putString("pass", config.password);
  preferences.putString("host", config.serverHost);
  preferences.putUShort("port", config.serverPort);
  preferences.putUChar("shift", sanitizeSampleShift(config.sampleShift));
  preferences.putFloat("gain", config.inputGain);
  preferences.putUShort("gate", config.noiseGate);
  preferences.putBool("hpass", config.highPass);
  preferences.putBool("agc", config.agcEnabled);
  preferences.putUShort("agctgt", config.agcTarget);
  preferences.putUChar("chmode", sanitizeChannelMode(config.channelMode));
  preferences.end();
}

static String connectionState() {
  if (WiFi.status() == WL_CONNECTED) {
    return "connected: " + WiFi.localIP().toString();
  }
  if (config.ssid.length() == 0) {
    return "not configured";
  }
  return "connecting";
}

static void setConfigApEnabled(bool enabled) {
  if (enabled == apEnabled) {
    return;
  }
  if (enabled) {
    WiFi.softAP(apName.c_str(), AP_PASSWORD);
    dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  } else {
    dnsServer.stop();
    WiFi.softAPdisconnect(true);
  }
  apEnabled = enabled;
}

static String renderNetworkOptions() {
  String options;
  options.reserve(2400);
  const int networkCount = WiFi.scanNetworks(false, true);

  if (config.ssid.length() == 0) {
    options += F("<option value=\"\" selected>Select a network</option>");
  } else {
    options += F("<option value=\"");
    options += htmlEscape(config.ssid);
    options += F("\" selected>");
    options += htmlEscape(config.ssid);
    options += F("  saved</option>");
  }

  for (int i = 0; i < networkCount; ++i) {
    const String ssid = WiFi.SSID(i);
    if (ssid.length() == 0 || ssid == config.ssid) {
      continue;
    }
    options += F("<option value=\"");
    options += htmlEscape(ssid);
    options += F("\"");
    if (ssid == config.ssid) {
      options += F(" selected");
    }
    options += F(">");
    options += htmlEscape(ssid);
    options += F("  ");
    options += String(WiFi.RSSI(i));
    options += F(" dBm");
    if (WiFi.encryptionType(i) != WIFI_AUTH_OPEN) {
      options += F("  lock");
    }
    options += F("</option>");
  }

  if (networkCount <= 0) {
    options += F("<option value=\"\" selected>No networks found</option>");
  }

  options += F("<option value=\"__manual\">Manual SSID...</option>");
  WiFi.scanDelete();
  return options;
}

static String renderConfigPage(const String &message = "") {
  String page;
  page.reserve(9000);
  page += F("<!doctype html><html><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>LeOSListener</title>"
            "<style>"
            ":root{color-scheme:light;--bg:#f4f6f8;--panel:#ffffff;--ink:#1f2937;--muted:#667085;--line:#d7dde5;--accent:#1167d8;--ok:#0f8a5f;--warn:#b54708}"
            "*{box-sizing:border-box}body{margin:0;min-height:100vh;font-family:-apple-system,BlinkMacSystemFont,Segoe UI,sans-serif;background:linear-gradient(180deg,#eef4fb 0,#f7f8fa 42%,#f4f6f8 100%);color:var(--ink)}"
            "main{width:min(680px,100%);margin:0 auto;padding:22px 16px 34px}.top{padding:18px 0 10px}.brand{font-size:28px;font-weight:780;letter-spacing:0}.sub{color:var(--muted);margin-top:4px}"
            ".panel{background:rgba(255,255,255,.94);border:1px solid var(--line);border-radius:8px;box-shadow:0 14px 38px rgba(31,41,55,.08);padding:18px;margin-top:16px}"
            ".grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px}.stat{border:1px solid #e4e8ee;border-radius:8px;padding:12px;background:#fbfcfd}.key{font-size:12px;color:var(--muted);text-transform:uppercase}.val{margin-top:5px;font-weight:700;overflow-wrap:anywhere}"
            "label{display:block;margin:16px 0 7px;font-weight:700}input,select{width:100%;padding:12px 11px;border:1px solid #c9d1dc;border-radius:8px;background:#fff;color:var(--ink);font-size:16px;outline:none}"
            "input:focus,select:focus{border-color:var(--accent);box-shadow:0 0 0 3px rgba(17,103,216,.15)}.row{display:grid;grid-template-columns:1fr 120px;gap:10px}.actions{display:flex;gap:10px;flex-wrap:wrap;margin-top:18px}"
            "button{border:0;border-radius:8px;padding:11px 14px;background:var(--accent);color:white;font-size:15px;font-weight:720}.secondary{background:#eef2f7;color:#1f2937}.danger{background:#697386;color:#fff}"
            ".notice{border-left:4px solid var(--ok);background:#eefaf5;border-radius:8px;padding:12px;margin-top:14px}.muted{color:var(--muted);font-size:14px}.manual{display:none}.manual.show{display:block}@media(max-width:560px){.grid,.row{grid-template-columns:1fr}.brand{font-size:25px}}"
            "</style></head><body><main><section class='top'><div class='brand'>LeOSListener</div><div class='sub'>ESP32-S3 microphone uplink</div></section>");
  if (message.length() > 0) {
    page += F("<div class='notice'>");
    page += htmlEscape(message);
    page += F("</div>");
  }
  page += F("<section class='panel'><div class='grid'><div class='stat'><div class='key'>Setup Wi-Fi</div><div class='val'>");
  page += htmlEscape(apName);
  page += F("</div></div><div class='stat'><div class='key'>Setup URL</div><div class='val'>192.168.4.1</div></div><div class='stat'><div class='key'>Wi-Fi</div><div class='val'>");
  page += htmlEscape(connectionState());
  page += F("</div></div><div class='stat'><div class='key'>Audio TCP</div><div class='val'>");
  page += audioClient.connected() ? F("connected") : F("disconnected");
  page += F("</div></div></div></section><section class='panel'><form method='post' action='/save'>"
            "<label for='ssid'>Wi-Fi network</label><select id='ssid' name='ssid'>");
  page += renderNetworkOptions();
  page += F("</select><div id='manualWrap' class='manual'><label for='ssid_manual'>Manual SSID</label><input id='ssid_manual' name='ssid_manual' value=\"");
  page += htmlEscape(config.ssid);
  page += F("\" autocomplete='off'></div><label for='password'>Wi-Fi password</label>"
            "<input id='password' name='password' type='password' value=\"");
  page += htmlEscape(config.password);
  page += F("\"><label for='host'>Server host / IP</label><input id='host' name='host' value=\"");
  page += htmlEscape(config.serverHost);
  page += F("\" placeholder='example.com or 192.168.1.10'><div class='row'><div><label for='port'>ESP32 upload port</label>"
            "<input id='port' name='port' type='number' min='1' max='65535' value='");
  page += String(config.serverPort);
  page += F("'></div><div><label>&nbsp;</label><button class='secondary' type='button' onclick='location.reload()'>Scan</button></div></div>"
            "<div class='grid' style='margin-top:8px'><div><label for='channel'>Mic channel</label><select id='channel' name='channel'>");
  page += F("<option value='auto'");
  if (config.channelMode == CHANNEL_AUTO) page += F(" selected");
  page += F(">Auto detect</option><option value='left'");
  if (config.channelMode == CHANNEL_LEFT) page += F(" selected");
  page += F(">Left slot</option><option value='right'");
  if (config.channelMode == CHANNEL_RIGHT) page += F(" selected");
  page += F(">Right slot</option></select></div><div><label for='shift'>Digital shift</label><input id='shift' name='shift' type='number' min='8' max='16' step='1' value='");
  page += String(config.sampleShift);
  page += F("'></div></div>"
            "<div class='grid' style='margin-top:8px'><div><label for='gain'>Input gain</label><input id='gain' name='gain' type='number' min='0.5' max='4.0' step='0.1' value='");
  page += String(config.inputGain, 1);
  page += F("'></div><div><label for='gate'>Noise gate</label><input id='gate' name='gate' type='number' min='0' max='1200' step='10' value='");
  page += String(config.noiseGate);
  page += F("'></div><div><label for='agctgt'>AGC target</label><input id='agctgt' name='agctgt' type='number' min='3000' max='16000' step='250' value='");
  page += String(config.agcTarget);
  page += F("'></div></div><label style='display:flex;align-items:center;gap:10px;margin-top:14px'><input name='hpass' type='checkbox' style='width:auto' ");
  if (config.highPass) {
    page += F("checked ");
  }
  page += F(">Enable voice high-pass filter</label><label style='display:flex;align-items:center;gap:10px;margin-top:10px'><input name='agc' type='checkbox' style='width:auto' ");
  if (config.agcEnabled) {
    page += F("checked ");
  }
  page += F(">Enable AGC (auto voice leveling)</label><div class='actions'><button type='submit'>Save and reconnect</button></div></form>"
            "<p class='muted'>Auto detect usually finds the real microphone slot; if audio sounds like static, try forcing Left or Right. Lower digital shift makes the raw signal quieter but cleaner, while higher gain or AGC makes quiet voices louder at the cost of more hiss. High-pass reduces rumble and desk vibration.</p>"
            "<form method='post' action='/reset'><button class='danger' type='submit'>Clear saved config</button></form></section>"
            "<script>const s=document.getElementById('ssid'),m=document.getElementById('manualWrap');function u(){m.classList.toggle('show',s.value==='__manual'||s.value==='')}s.addEventListener('change',u);u();</script>"
            "</main></body></html>");
  return page;
}

static void beginStationConnect() {
  if (config.ssid.length() == 0) {
    return;
  }
  Serial.printf("Connecting Wi-Fi SSID=%s\n", config.ssid.c_str());
  WiFi.begin(config.ssid.c_str(), config.password.c_str());
  lastWifiAttemptMs = millis();
}

static void setupConfigPortal() {
  apName = AP_SSID;

  WiFi.mode(WIFI_AP_STA);
  setConfigApEnabled(true);

  webServer.on("/", HTTP_GET, []() {
    webServer.send(200, "text/html", renderConfigPage());
  });

  webServer.on("/save", HTTP_POST, []() {
    const String selectedSsid = webServer.arg("ssid");
    const String manualSsid = webServer.arg("ssid_manual");
    config.ssid = (selectedSsid == "__manual" || selectedSsid.length() == 0) ? manualSsid : selectedSsid;
    config.password = webServer.arg("password");
    config.serverHost = webServer.arg("host");
    const int port = webServer.arg("port").toInt();
    config.serverPort = port > 0 && port <= 65535 ? static_cast<uint16_t>(port) : DEFAULT_SERVER_PORT;
    const String channel = webServer.arg("channel");
    config.channelMode = channel == "left" ? CHANNEL_LEFT : (channel == "right" ? CHANNEL_RIGHT : CHANNEL_AUTO);
    const int shift = webServer.arg("shift").toInt();
    config.sampleShift = sanitizeSampleShift(static_cast<uint8_t>(shift));
    config.inputGain = max(0.5f, min(4.0f, webServer.arg("gain").toFloat()));
    const int gate = webServer.arg("gate").toInt();
    config.noiseGate = gate >= 0 && gate <= 1200 ? static_cast<uint16_t>(gate) : 260;
    config.highPass = webServer.hasArg("hpass");
    config.agcEnabled = webServer.hasArg("agc");
    const int agcTarget = webServer.arg("agctgt").toInt();
    config.agcTarget = agcTarget >= 3000 && agcTarget <= 16000 ? static_cast<uint16_t>(agcTarget) : 6200;
    resetVoiceState();
    saveConfig();
    audioClient.stop();
    WiFi.disconnect(false, true);
    delay(200);
    beginStationConnect();
    webServer.send(200, "text/html", renderConfigPage("Saved. The ESP32 is reconnecting now."));
  });

  webServer.on("/reset", HTTP_POST, []() {
    preferences.begin("micwifi", false);
    preferences.clear();
    preferences.end();
    config = RuntimeConfig{"", "", "", DEFAULT_SERVER_PORT, 12, 1.2f, 260, true, true, 6200, CHANNEL_AUTO};
    resetVoiceState();
    audioClient.stop();
    WiFi.disconnect(false, true);
    webServer.send(200, "text/html", renderConfigPage("Saved config cleared."));
  });

  webServer.onNotFound([]() {
    webServer.send(200, "text/html", renderConfigPage());
  });

  webServer.begin();
  Serial.printf("Config AP ready: SSID=%s password=%s URL=http://192.168.4.1/\n", apName.c_str(), AP_PASSWORD);
}

static void setupI2SMicrophone() {
  i2s_config_t i2sConfig = {};
#if ENABLE_ES8311_CODEC
  i2sConfig.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_TX);
  i2sConfig.sample_rate = MIC_SAMPLE_RATE;
  i2sConfig.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  i2sConfig.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  i2sConfig.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  i2sConfig.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  i2sConfig.dma_buf_count = 8;
  i2sConfig.dma_buf_len = MIC_FRAME_SAMPLES;
  i2sConfig.use_apll = false;
  i2sConfig.tx_desc_auto_clear = true;
  i2sConfig.fixed_mclk = 12288000;

  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_MCLK_PIN;
  pins.bck_io_num = I2S_BCLK_PIN;
  pins.ws_io_num = I2S_LRCLK_PIN;
  pins.data_out_num = I2S_DOUT_PIN;
  pins.data_in_num = I2S_DATA_PIN;
#else
  i2sConfig.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
  i2sConfig.sample_rate = MIC_SAMPLE_RATE;
  i2sConfig.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  i2sConfig.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  i2sConfig.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  i2sConfig.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  i2sConfig.dma_buf_count = 8;
  // For right/left 32-bit I2S slots, each mono output sample needs two 32-bit words.
  i2sConfig.dma_buf_len = I2S_WORDS_PER_FRAME;
  i2sConfig.use_apll = false;
  i2sConfig.tx_desc_auto_clear = false;
  i2sConfig.fixed_mclk = 0;

  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = I2S_BCLK_PIN;
  pins.ws_io_num = I2S_LRCLK_PIN;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = I2S_DATA_PIN;
#endif

  ESP_ERROR_CHECK(i2s_driver_install(I2S_PORT, &i2sConfig, 0, nullptr));
  ESP_ERROR_CHECK(i2s_set_pin(I2S_PORT, &pins));
  ESP_ERROR_CHECK(i2s_zero_dma_buffer(I2S_PORT));
}

static void setupSpeaker() {
#if ENABLE_SPEAKER
#if ENABLE_ES8311_CODEC
  if (PA_ENABLE_PIN >= 0) {
    pinMode(PA_ENABLE_PIN, OUTPUT);
    digitalWrite(PA_ENABLE_PIN, LOW);
  }
  speakerReady = setupEs8311Codec();
  if (PA_ENABLE_PIN >= 0) {
    digitalWrite(PA_ENABLE_PIN, speakerReady ? HIGH : LOW);
  }
  Serial.printf("ES8311 speaker %s pa=%d dout=%d mclk=%d bclk=%d ws=%d din=%d gain=%d%%\n",
                speakerReady ? "ready" : "failed",
                PA_ENABLE_PIN,
                I2S_DOUT_PIN,
                I2S_MCLK_PIN,
                I2S_BCLK_PIN,
                I2S_LRCLK_PIN,
                I2S_DATA_PIN,
                SPEAKER_GAIN_PERCENT);
#else
  i2s_config_t i2sConfig = {};
  i2sConfig.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
  i2sConfig.sample_rate = MIC_SAMPLE_RATE;
  i2sConfig.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  i2sConfig.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  i2sConfig.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  i2sConfig.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  i2sConfig.dma_buf_count = 8;
  i2sConfig.dma_buf_len = MIC_FRAME_SAMPLES;
  i2sConfig.use_apll = false;
  i2sConfig.tx_desc_auto_clear = true;
  i2sConfig.fixed_mclk = 0;

  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = SPEAKER_BCLK_PIN;
  pins.ws_io_num = SPEAKER_LRCLK_PIN;
  pins.data_out_num = SPEAKER_DATA_PIN;
  pins.data_in_num = I2S_PIN_NO_CHANGE;

  esp_err_t result = i2s_driver_install(SPEAKER_I2S_PORT, &i2sConfig, 0, nullptr);
  if (result == ESP_OK) {
    result = i2s_set_pin(SPEAKER_I2S_PORT, &pins);
  }
  if (result == ESP_OK) {
    i2s_zero_dma_buffer(SPEAKER_I2S_PORT);
    speakerReady = true;
    Serial.printf("Speaker ready bclk=%d ws=%d data=%d gain=%d%%\n",
                  SPEAKER_BCLK_PIN,
                  SPEAKER_LRCLK_PIN,
                  SPEAKER_DATA_PIN,
                  SPEAKER_GAIN_PERCENT);
  } else {
    speakerReady = false;
    Serial.printf("Speaker init failed err=%d\n", static_cast<int>(result));
  }
#endif
#else
  speakerReady = false;
#endif
}

static void setupDisplay() {
#if ENABLE_OLED_DISPLAY
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  displayReady = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (!displayReady) {
    Serial.println("OLED display init failed");
    return;
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("AirMic-Relay");
  display.println("Booting...");
  display.display();
  Serial.printf("OLED ready %dx%d sda=%d scl=%d\n", OLED_WIDTH, OLED_HEIGHT, OLED_SDA_PIN, OLED_SCL_PIN);
#endif
}

static void updateDisplay() {
#if ENABLE_OLED_DISPLAY
  if (!displayReady || millis() - lastDisplayMs < 1000) {
    return;
  }
  lastDisplayMs = millis();
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("AirMic-Relay");
  display.print("WiFi: ");
  display.println(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : (apEnabled ? "setup AP" : "connecting"));
  display.print("TCP: ");
  display.println(audioClient.connected() ? "connected" : "offline");
  display.print("Mic: ");
  display.print(MIC_SAMPLE_RATE);
  display.print("Hz ");
  display.println(slotLabel(activeSlot));
  display.print("Up: ");
  display.print(statusFramesSent);
  display.print(" Down: ");
  display.println(statusDownlinkFrames);
  display.print("Talk: ");
  display.println(millis() - lastDownlinkMs < 1500 ? "receiving" : "idle");
  display.print("Speaker: ");
  display.println(speakerReady ? "ready" : "off");
  display.display();
#endif
}

static void maintainWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (apEnabled) {
      setConfigApEnabled(false);
      Serial.println("Config AP paused while station link is active");
    }
    return;
  }

  if (!apEnabled) {
    setConfigApEnabled(true);
    Serial.println("Config AP resumed because station link is down");
  }

  if (config.ssid.length() == 0 || WiFi.status() == WL_CONNECTED) {
    return;
  }
  if (millis() - lastWifiAttemptMs >= WIFI_RETRY_MS) {
    beginStationConnect();
  }
}

static void maintainAudioClient() {
  if (config.serverHost.length() == 0 || WiFi.status() != WL_CONNECTED || audioClient.connected()) {
    return;
  }
  if (millis() - lastTcpAttemptMs < TCP_RETRY_MS) {
    return;
  }

  lastTcpAttemptMs = millis();
  Serial.printf("Connecting audio server %s:%u\n", config.serverHost.c_str(), config.serverPort);
  audioClient.stop();
  audioClient.setNoDelay(true);
  if (audioClient.connect(config.serverHost.c_str(), config.serverPort)) {
    Serial.println("Audio server connected");
  } else {
    Serial.println("Audio server connection failed");
    audioClient.stop();
  }
}

static void reportStatus() {
  if (millis() - lastStatusMs < 5000) {
    return;
  }
  const uint32_t now = millis();
  const uint32_t elapsedMs = lastStatusMs == 0 ? 5000 : max<uint32_t>(1, now - lastStatusMs);
  const float uplinkKbps = (statusPayloadBytesSent * 8.0f) / elapsedMs;
  const float downlinkKbps = (statusDownlinkBytes * 8.0f) / elapsedMs;
  lastStatusMs = now;
  Serial.printf(
      "status ap=%s setup=http://192.168.4.1 wifi=%s server=%s:%u tcp=%s codec=adpcm rate=%d shift=%u mode=%s slot=%s frame=%u words=%u fps=%u up=%.1fkbps down_fps=%u down=%.1fkbps talk=%s speaker=%d gain=%.1f gate=%u hpf=%d agc=%d target=%u bclk=%d ws=%d data=%d\n",
      apName.c_str(),
      connectionState().c_str(),
      config.serverHost.length() ? config.serverHost.c_str() : "(unset)",
      config.serverPort,
      audioClient.connected() ? "connected" : "disconnected",
      MIC_SAMPLE_RATE,
      static_cast<unsigned>(config.sampleShift),
      channelModeLabel(config.channelMode),
      slotLabel(activeSlot),
      static_cast<unsigned>(lastSamplesRead),
      static_cast<unsigned>(lastWordsRead),
      static_cast<unsigned>(statusFramesSent / max<uint32_t>(1, elapsedMs / 1000)),
      uplinkKbps,
      static_cast<unsigned>(statusDownlinkFrames / max<uint32_t>(1, elapsedMs / 1000)),
      downlinkKbps,
      millis() - lastDownlinkMs < 1500 ? "receiving" : "idle",
      speakerReady ? 1 : 0,
      config.inputGain,
      config.noiseGate,
      config.highPass ? 1 : 0,
      config.agcEnabled ? 1 : 0,
      config.agcTarget,
      I2S_BCLK_PIN,
      I2S_LRCLK_PIN,
      I2S_DATA_PIN);
  statusFramesSent = 0;
  statusPayloadBytesSent = 0;
  statusDownlinkFrames = 0;
  statusDownlinkBytes = 0;
}

static float applyHighPass(float sample) {
  if (!config.highPass) {
    return sample;
  }
  const float output = HIGH_PASS_ALPHA * (hpPrevOutput + sample - hpPrevInput);
  hpPrevInput = sample;
  hpPrevOutput = output;
  return output;
}

static float applyNoiseGate(float sample) {
  const float level = fabsf(sample);
  const float gate = static_cast<float>(config.noiseGate);
  if (gate <= 0.0f) {
    return sample;
  }
  if (level < gate) {
    const float t = level / gate;
    return sample * t * t * 0.35f;
  }
  if (level < gate * 2.0f) {
    const float t = (level - gate) / gate;
    return sample * (0.35f + 0.65f * t);
  }
  return sample;
}

static float applyAgc(float sample) {
  if (!config.agcEnabled) {
    return sample;
  }
  const float absSample = fabsf(sample);
  const float gate = static_cast<float>(config.noiseGate);
  if (absSample < gate * 0.85f) {
    agcEnvelope = agcEnvelope * 0.998f + absSample * 0.002f;
    agcCurrentGain += (1.0f - agcCurrentGain) * 0.015f;
    return sample * agcCurrentGain;
  }
  agcEnvelope = agcEnvelope * 0.992f + absSample * 0.008f;
  const float floorLevel = max(agcEnvelope, 1200.0f);
  const float desiredGain = min(2.2f, max(0.95f, static_cast<float>(config.agcTarget) / floorLevel));
  const float step = desiredGain > agcCurrentGain ? 0.010f : 0.040f;
  agcCurrentGain += (desiredGain - agcCurrentGain) * step;
  return sample * agcCurrentGain;
}

static int16_t shapeVoiceSample(int32_t rawSample) {
  float sample = static_cast<float>(rawSample) * config.inputGain;
  sample = applyHighPass(sample);
  sample = applyNoiseGate(sample);
  sample = applyAgc(sample);

  const float absSample = fabsf(sample);
  if (absSample > 26000.0f) {
    const float clipped = 26000.0f + (absSample - 26000.0f) * 0.20f;
    sample = sample < 0 ? -clipped : clipped;
  }

  if (sample > 32767.0f) sample = 32767.0f;
  if (sample < -32768.0f) sample = -32768.0f;
  return static_cast<int16_t>(sample);
}

static uint8_t encodeAdpcmNibble(int16_t sample) {
  int predictor = adpcmPredictor;
  int stepIndex = adpcmStepIndex;
  const int step = IMA_STEP_TABLE[stepIndex];
  int diff = static_cast<int>(sample) - predictor;
  uint8_t code = 0;
  if (diff < 0) {
    code = 8;
    diff = -diff;
  }

  int delta = step >> 3;
  if (diff >= step) {
    code |= 4;
    diff -= step;
    delta += step;
  }
  if (diff >= (step >> 1)) {
    code |= 2;
    diff -= step >> 1;
    delta += step >> 1;
  }
  if (diff >= (step >> 2)) {
    code |= 1;
    delta += step >> 2;
  }

  predictor += (code & 8) ? -delta : delta;
  if (predictor > 32767) predictor = 32767;
  if (predictor < -32768) predictor = -32768;

  stepIndex += IMA_INDEX_TABLE[code & 0x0F];
  if (stepIndex < 0) stepIndex = 0;
  if (stepIndex > 88) stepIndex = 88;

  adpcmPredictor = static_cast<int16_t>(predictor);
  adpcmStepIndex = static_cast<int8_t>(stepIndex);
  return code & 0x0F;
}

static size_t encodeAdpcmFrame(size_t samplesRead) {
  size_t out = 0;
  for (size_t i = 0; i < samplesRead; i += 2) {
    const uint8_t lo = encodeAdpcmNibble(pcmBuffer[i]);
    const uint8_t hi = (i + 1 < samplesRead) ? encodeAdpcmNibble(pcmBuffer[i + 1]) : 0;
    adpcmBuffer[out++] = static_cast<uint8_t>(lo | (hi << 4));
  }
  return out;
}

static size_t audioPayloadBytes(uint16_t samples, uint8_t codec) {
  if (codec == AUDIO_CODEC_PCM16) {
    return static_cast<size_t>(samples) * sizeof(int16_t);
  }
  if (codec == AUDIO_CODEC_IMA_ADPCM) {
    return (static_cast<size_t>(samples) + 1) / 2;
  }
  return 0;
}

static size_t decodeImaAdpcmFrame(const uint8_t *payload, size_t payloadBytes, uint16_t samples, int16_t predictor, uint8_t stepIndex) {
  int currentPredictor = predictor;
  int currentStepIndex = min<int>(88, stepIndex);
  size_t out = 0;
  for (size_t i = 0; i < payloadBytes && out < samples; ++i) {
    const uint8_t byte = payload[i];
    for (uint8_t half = 0; half < 2 && out < samples; ++half) {
      const uint8_t code = half == 0 ? (byte & 0x0F) : (byte >> 4);
      const int step = IMA_STEP_TABLE[currentStepIndex];
      int delta = step >> 3;
      if (code & 4) delta += step;
      if (code & 2) delta += step >> 1;
      if (code & 1) delta += step >> 2;
      currentPredictor += (code & 8) ? -delta : delta;
      if (currentPredictor > 32767) currentPredictor = 32767;
      if (currentPredictor < -32768) currentPredictor = -32768;
      currentStepIndex += IMA_INDEX_TABLE[code & 0x0F];
      if (currentStepIndex < 0) currentStepIndex = 0;
      if (currentStepIndex > 88) currentStepIndex = 88;
      downlinkPcmBuffer[out++] = static_cast<int16_t>(currentPredictor);
    }
  }
  return out;
}

static size_t decodeDownlinkFrame(const AudioFrameHeader &header, const uint8_t *payload, size_t payloadBytes) {
  if (header.codec == AUDIO_CODEC_PCM16) {
    const size_t samples = min(static_cast<size_t>(header.samples), payloadBytes / sizeof(int16_t));
    memcpy(downlinkPcmBuffer, payload, samples * sizeof(int16_t));
    return samples;
  }
  if (header.codec == AUDIO_CODEC_IMA_ADPCM) {
    return decodeImaAdpcmFrame(payload, payloadBytes, header.samples, header.predictor, header.stepIndex);
  }
  return 0;
}

static void playSpeakerPcm(size_t samples) {
#if ENABLE_SPEAKER
  if (!speakerReady || samples == 0) {
    return;
  }
  const int gain = max(0, min(200, SPEAKER_GAIN_PERCENT));
  if (gain != 100) {
    for (size_t i = 0; i < samples; ++i) {
      int32_t sample = static_cast<int32_t>(downlinkPcmBuffer[i]) * gain / 100;
      if (sample > 32767) sample = 32767;
      if (sample < -32768) sample = -32768;
      downlinkPcmBuffer[i] = static_cast<int16_t>(sample);
    }
  }
  size_t written = 0;
#if ENABLE_ES8311_CODEC
  i2s_write(I2S_PORT, downlinkPcmBuffer, samples * sizeof(int16_t), &written, pdMS_TO_TICKS(30));
#else
  i2s_write(SPEAKER_I2S_PORT, downlinkPcmBuffer, samples * sizeof(int16_t), &written, pdMS_TO_TICKS(30));
#endif
#else
  (void)samples;
#endif
}

static void processDownlinkBuffer() {
  while (downlinkBytesBuffered >= sizeof(AudioFrameHeader)) {
    size_t pos = 0;
    while (pos + sizeof(uint32_t) <= downlinkBytesBuffered) {
      uint32_t magic = 0;
      memcpy(&magic, downlinkBuffer + pos, sizeof(magic));
      if (magic == AUDIO_MAGIC) {
        break;
      }
      ++pos;
    }
    if (pos > 0) {
      memmove(downlinkBuffer, downlinkBuffer + pos, downlinkBytesBuffered - pos);
      downlinkBytesBuffered -= pos;
    }
    if (downlinkBytesBuffered < sizeof(AudioFrameHeader)) {
      return;
    }

    AudioFrameHeader header = {};
    memcpy(&header, downlinkBuffer, sizeof(header));
    if (header.magic != AUDIO_MAGIC) {
      memmove(downlinkBuffer, downlinkBuffer + 1, downlinkBytesBuffered - 1);
      downlinkBytesBuffered -= 1;
      continue;
    }
    const size_t payloadBytes = audioPayloadBytes(header.samples, header.codec);
    if (payloadBytes == 0 || payloadBytes > sizeof(pcmBuffer)) {
      memmove(downlinkBuffer, downlinkBuffer + sizeof(uint32_t), downlinkBytesBuffered - sizeof(uint32_t));
      downlinkBytesBuffered -= sizeof(uint32_t);
      continue;
    }
    const size_t totalBytes = sizeof(AudioFrameHeader) + payloadBytes;
    if (downlinkBytesBuffered < totalBytes) {
      return;
    }

    const size_t samples = decodeDownlinkFrame(header, downlinkBuffer + sizeof(AudioFrameHeader), payloadBytes);
    playSpeakerPcm(samples);
    lastDownlinkMs = millis();
    statusDownlinkFrames++;
    statusDownlinkBytes += totalBytes;
    memmove(downlinkBuffer, downlinkBuffer + totalBytes, downlinkBytesBuffered - totalBytes);
    downlinkBytesBuffered -= totalBytes;
  }
}

static void receiveDownlinkAudio() {
  if (!audioClient.connected()) {
    downlinkBytesBuffered = 0;
    return;
  }
  while (audioClient.available() > 0) {
    if (downlinkBytesBuffered >= sizeof(downlinkBuffer)) {
      memmove(downlinkBuffer, downlinkBuffer + sizeof(uint32_t), downlinkBytesBuffered - sizeof(uint32_t));
      downlinkBytesBuffered -= sizeof(uint32_t);
    }
    const size_t capacity = sizeof(downlinkBuffer) - downlinkBytesBuffered;
    const int readBytes = audioClient.read(downlinkBuffer + downlinkBytesBuffered, capacity);
    if (readBytes <= 0) {
      break;
    }
    downlinkBytesBuffered += static_cast<size_t>(readBytes);
    processDownlinkBuffer();
  }
}

static void updateAutoSlot(size_t samplesRead) {
  if (samplesRead == 0) {
    return;
  }
  float left = 0.0f;
  float right = 0.0f;
  for (size_t i = 0; i < samplesRead; ++i) {
    left += static_cast<float>(abs32(i2sBuffer[i * 2] >> config.sampleShift));
    right += static_cast<float>(abs32(i2sBuffer[i * 2 + 1] >> config.sampleShift));
  }
  slotEnergyLeft = slotEnergyLeft * 0.92f + left * 0.08f;
  slotEnergyRight = slotEnergyRight * 0.92f + right * 0.08f;
  if (slotEnergyRight > slotEnergyLeft * 1.35f) {
    activeSlot = 1;
  } else if (slotEnergyLeft > slotEnergyRight * 1.35f) {
    activeSlot = 0;
  }
}

static size_t readPcmFrame() {
#if ENABLE_ES8311_CODEC
  size_t bytesRead = 0;
  uint8_t *dest = reinterpret_cast<uint8_t *>(codecRxBuffer);
  const uint32_t startedAt = millis();
  while (bytesRead < sizeof(codecRxBuffer)) {
    size_t chunkBytes = 0;
    const esp_err_t result = i2s_read(
        I2S_PORT,
        dest + bytesRead,
        sizeof(codecRxBuffer) - bytesRead,
        &chunkBytes,
        pdMS_TO_TICKS(40));
    if (result != ESP_OK || chunkBytes == 0) {
      break;
    }
    bytesRead += chunkBytes;
    if (millis() - startedAt >= 80) {
      break;
    }
  }
  if (bytesRead == 0) {
    return 0;
  }
  const size_t samplesRead = min(bytesRead / sizeof(codecRxBuffer[0]), static_cast<size_t>(MIC_FRAME_SAMPLES));
  lastWordsRead = samplesRead;
  lastSamplesRead = samplesRead;
  activeSlot = 0;
  for (size_t i = 0; i < samplesRead; ++i) {
    pcmBuffer[i] = shapeVoiceSample(codecRxBuffer[i]);
  }
  return samplesRead;
#else
  size_t bytesRead = 0;
  uint8_t *dest = reinterpret_cast<uint8_t *>(i2sBuffer);
  const uint32_t startedAt = millis();
  while (bytesRead < sizeof(i2sBuffer)) {
    size_t chunkBytes = 0;
    const esp_err_t result = i2s_read(
        I2S_PORT,
        dest + bytesRead,
        sizeof(i2sBuffer) - bytesRead,
        &chunkBytes,
        pdMS_TO_TICKS(40));
    if (result != ESP_OK) {
      break;
    }
    if (chunkBytes == 0) {
      break;
    }
    bytesRead += chunkBytes;
    if (millis() - startedAt >= 80) {
      break;
    }
  }

  if (bytesRead == 0) {
    return 0;
  }

  const size_t wordsRead = bytesRead / sizeof(i2sBuffer[0]);
  const size_t samplesRead = min(wordsRead / 2, static_cast<size_t>(MIC_FRAME_SAMPLES));
  lastWordsRead = wordsRead;
  lastSamplesRead = samplesRead;
  const uint8_t shift = sanitizeSampleShift(config.sampleShift);
  if (config.channelMode == CHANNEL_LEFT) {
    activeSlot = 0;
  } else if (config.channelMode == CHANNEL_RIGHT) {
    activeSlot = 1;
  } else {
    updateAutoSlot(samplesRead);
  }
  for (size_t i = 0; i < samplesRead; ++i) {
    const int32_t sampleWord = i2sBuffer[i * 2 + activeSlot];
    const int32_t sample = sampleWord >> shift;
    pcmBuffer[i] = shapeVoiceSample(sample);
  }
  return samplesRead;
#endif
}

static void sendAudioFrame(size_t samplesRead) {
  if (!audioClient.connected() || samplesRead == 0) {
    return;
  }

  const int16_t framePredictor = adpcmPredictor;
  const uint8_t frameStepIndex = static_cast<uint8_t>(adpcmStepIndex);
  const size_t payloadBytes = encodeAdpcmFrame(samplesRead);
  const AudioFrameHeader header = {
      AUDIO_MAGIC,
      static_cast<uint16_t>(samplesRead),
      static_cast<uint16_t>(MIC_SAMPLE_RATE),
      AUDIO_CODEC_IMA_ADPCM,
      frameStepIndex,
      framePredictor};
  memcpy(txBuffer, &header, sizeof(header));
  memcpy(txBuffer + sizeof(header), adpcmBuffer, payloadBytes);
  const size_t totalBytes = sizeof(header) + payloadBytes;
  const size_t written = audioClient.write(txBuffer, totalBytes);

  if (written != totalBytes) {
    Serial.println("Audio server write failed");
    audioClient.stop();
    return;
  }
  statusFramesSent++;
  statusPayloadBytesSent += totalBytes;
}

void setup() {
  Serial.begin(921600);
  delay(1200);
  loadConfig();
  setupDisplay();
  setupConfigPortal();
  beginStationConnect();
  setupI2SMicrophone();
  setupSpeaker();
  resetVoiceState();
  Serial.printf("I2S ready rate=%d frame=%d words=%u shift=%u mode=%s slot=%s gain=%.1f gate=%u hpf=%d agc=%d target=%u bclk=%d ws=%d data=%d\n",
                MIC_SAMPLE_RATE,
                MIC_FRAME_SAMPLES,
                static_cast<unsigned>(I2S_WORDS_PER_FRAME),
                static_cast<unsigned>(config.sampleShift),
                channelModeLabel(config.channelMode),
                slotLabel(activeSlot),
                config.inputGain,
                config.noiseGate,
                config.highPass ? 1 : 0,
                config.agcEnabled ? 1 : 0,
                config.agcTarget,
                I2S_BCLK_PIN,
                I2S_LRCLK_PIN,
                I2S_DATA_PIN);
}

void loop() {
  if (apEnabled) {
    dnsServer.processNextRequest();
  }
  webServer.handleClient();
  maintainWifi();
  maintainAudioClient();
  receiveDownlinkAudio();
  reportStatus();
  updateDisplay();

  const size_t samplesRead = readPcmFrame();
  sendAudioFrame(samplesRead);
  receiveDownlinkAudio();
}
