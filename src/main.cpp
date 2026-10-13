#include <Arduino.h>
#include <ArduinoJson.h>
#include <FastLED.h>
#include <FastLED_NeoMatrix.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <esp_ota_ops.h>
#include <time.h>

#include "certs.h"
#include "icons.h"
#include "secrets.h"  // server credentials; not committed to Git

// Change this every time you build firmware to upload from the
// dashboard. The dashboard compares it with what the sign reports.
#define FIRMWARE_VERSION "1.8.1"

// ---------- Hardware ----------
const int DATA_PIN = 13;
const int WIDTH = 32;
const int HEIGHT = 8;
const int NUM_LEDS = WIDTH * HEIGHT;

CRGB leds[NUM_LEDS];
FastLED_NeoMatrix matrix(leds, WIDTH, HEIGHT,
                         NEO_MATRIX_TOP + NEO_MATRIX_LEFT +
                         NEO_MATRIX_COLUMNS + NEO_MATRIX_ZIGZAG);

// ---------- Display state ----------
// Everything the sign needs to know to draw itself.
// The server sends exactly this over the WebSocket.
enum Mode { MODE_MESSAGE, MODE_CLOCK, MODE_DARK };

struct DisplayState {
  Mode mode;
  String text;
  uint8_t r, g, b;  // text color
  int icon;         // index into ICONS, or -1 for none
};

DisplayState state = {MODE_MESSAGE, "Ready", 0, 150, 255, -1};

const int CHAR_WIDTH = 6;  // 5 pixels of glyph + 1 of spacing
const unsigned long FRAME_MS = 50;
unsigned long lastFrame = 0;
int scrollX = WIDTH;

// A notice replaces the normal display while it is set. It is used
// for things the sign needs to tell you itself, such as Wi-Fi setup.
String notice = "";
int noticeScrollX = WIDTH;
bool updating = false;  // true while new firmware is being installed

// ---------- Brightness ----------
// Brightness is set as 1 to 100 from the dashboard. Eyes judge
// brightness on a curve, so the value is squared: this gives fine
// control at the dim end, where an indoor sign is actually used.
// 100 maps to 100 out of FastLED's 255, which keeps the panel well
// inside the power supply's limit.
int brightnessPercent = 30;

void applyBrightness(int percent) {
  if (percent < 1) percent = 1;
  if (percent > 100) percent = 100;
  brightnessPercent = percent;
  int level = percent * percent / 100;
  if (level < 1) level = 1;
  matrix.setBrightness(level);
}

// ---------- Wi-Fi and time ----------
// US Mountain time with daylight saving rules, in POSIX format.
const char* TIMEZONE = "MST7MDT,M3.2.0,M11.1.0";
const char* TIME_SERVER = "pool.ntp.org";

bool wifiReady = false;

// When the sign can't join a saved network, it starts its own Wi-Fi
// network with a setup page where you choose one.
const char* SETUP_NETWORK = "BrightByte-Setup";
const unsigned long SETUP_WINDOW_MS = 3UL * 60 * 1000;
WiFiManager wifiManager;
bool setupMode = false;
unsigned long setupIdleSince = 0;

// Holding the BOOT button for 5 seconds forgets the saved network.
const int BOOT_BUTTON_PIN = 0;
unsigned long buttonDownSince = 0;

// ---------- Firmware updates ----------
// By default a freshly updated sign is marked as good the moment it
// starts. Returning true here hands that decision to this program: the
// new firmware is only kept once it has reached the server. If it
// can't within 5 minutes, the sign goes back to the previous firmware.
extern "C" bool verifyRollbackLater() { return true; }

const unsigned long CONFIRM_WINDOW_MS = 5UL * 60 * 1000;
bool awaitingConfirmation = false;
bool updateRequested = false;
String updateMd5 = "";
String updateError = "";  // why the last update failed, if it did

// ---------- Server connection ----------
WebSocketsClient socket;
bool serverConnected = false;
Preferences prefs;  // small key-value store in flash memory

// Minutes since local midnight, or -1 if the time isn't known yet.
int minutesNow() {
  struct tm now;
  if (!getLocalTime(&now, 0)) return -1;
  return now.tm_hour * 60 + now.tm_min;
}

// ---------- Drawing helpers ----------
int findIcon(const String& name) {
  for (int i = 0; i < ICON_COUNT; i++) {
    if (name == ICONS[i].name) return i;
  }
  return -1;
}

void drawBitmap(const uint8_t rows[8], int x, uint16_t color) {
  for (int row = 0; row < 8; row++) {
    for (int col = 0; col < 8; col++) {
      if (rows[row] & (0x80 >> col)) {
        matrix.drawPixel(x + col, row, color);
      }
    }
  }
}

void drawIcon(int index, int x) {
  const Icon& icon = ICONS[index];
  drawBitmap(icon.rows, x, matrix.Color(icon.r, icon.g, icon.b));
}

void drawText(const String& text, int x, uint16_t color) {
  for (unsigned int i = 0; i < text.length(); i++) {
    // Passing the same color as foreground and background
    // means "transparent background" in Adafruit GFX.
    matrix.drawChar(x + i * CHAR_WIDTH, 0, text[i], color, color, 1);
  }
}

// Draws text in the columns from areaStart to the right edge: centered
// if it fits, scrolling if it doesn't. scroll remembers the position
// between frames.
void drawLine(const String& text, int areaStart, uint16_t color, int& scroll) {
  int areaWidth = WIDTH - areaStart;
  int textWidth = text.length() * CHAR_WIDTH - 1;

  if (textWidth <= areaWidth) {
    drawText(text, areaStart + (areaWidth - textWidth) / 2, color);
  } else {
    drawText(text, scroll, color);
    scroll--;
    if (scroll < areaStart - textWidth) scroll = WIDTH;
  }
  if (areaStart > 0) {
    matrix.fillRect(0, 0, areaStart, HEIGHT, 0);  // keep text out of the icon's space
  }
}

void drawMessage() {
  uint16_t color = matrix.Color(state.r, state.g, state.b);
  int areaStart = (state.icon >= 0) ? 9 : 0;  // icon takes 8 columns + 1 gap
  drawLine(state.text, areaStart, color, scrollX);
  if (state.icon >= 0) drawIcon(state.icon, 0);
}

// Shown while new firmware downloads: a down arrow and "Wait".
const uint8_t DOWN_ARROW[8] = {0b00011000, 0b00011000, 0b00011000, 0b00011000,
                               0b11111111, 0b01111110, 0b00111100, 0b00011000};

void drawUpdating() {
  uint16_t amber = matrix.Color(255, 170, 0);
  int unused = WIDTH;
  drawLine("Wait", 9, amber, unused);
  drawBitmap(DOWN_ARROW, 0, amber);
}

void drawClock() {
  uint16_t color = matrix.Color(state.r, state.g, state.b);
  int minutes = minutesNow();
  if (minutes < 0) {
    drawText("--:--", 1, color);  // time not synced yet
    return;
  }
  int hour24 = minutes / 60;
  int minute = minutes % 60;
  int hour12 = hour24 % 12;
  if (hour12 == 0) hour12 = 12;

  // The time gets columns 0-22 and the icon gets columns 24-31.
  // A leading "1" is drawn narrow so that 12:34 fits.
  int timeWidth = (hour12 >= 10) ? 23 : 19;
  int x = (23 - timeWidth) / 2;

  if (hour12 >= 10) {
    matrix.drawChar(x - 1, 0, '1', color, color, 1);
    x += 4;
  }
  matrix.drawChar(x, 0, '0' + hour12 % 10, color, color, 1);
  x += 6;
  matrix.drawPixel(x, 2, color);  // colon
  matrix.drawPixel(x, 4, color);
  x += 2;
  matrix.drawChar(x, 0, '0' + minute / 10, color, color, 1);
  x += 6;
  matrix.drawChar(x, 0, '0' + minute % 10, color, color, 1);

  bool daytime = (hour24 >= 6 && hour24 < 18);
  drawIcon(findIcon(daytime ? "sun" : "moon"), 24);
}

void render() {
  matrix.fillScreen(0);
  if (updating) {
    drawUpdating();
  } else if (notice.length() > 0) {
    drawLine(notice, 0, matrix.Color(255, 170, 0), noticeScrollX);
  } else if (state.mode == MODE_MESSAGE) {
    drawMessage();
  } else if (state.mode == MODE_CLOCK) {
    drawClock();
  }
  // MODE_DARK draws nothing.
  matrix.show();
}

// ---------- Wi-Fi setup ----------
void forgetWifi() {
  Serial.println("Forgetting the saved Wi-Fi network and restarting.");
  wifiManager.resetSettings();
  delay(500);
  ESP.restart();
}

// Called once at startup. Joins the saved network, or opens the setup
// network if there isn't one or it can't be reached.
void startWifi() {
  WiFi.mode(WIFI_STA);
  wifiManager.setConfigPortalBlocking(false);  // keep the display running
  wifiManager.setConnectTimeout(20);           // seconds to try the saved network
  Serial.println("Connecting to Wi-Fi...");
  if (wifiManager.autoConnect(SETUP_NETWORK, SETUP_PASSWORD)) return;

  setupMode = true;
  setupIdleSince = millis();
  notice = String("Wi-Fi setup: join ") + SETUP_NETWORK;
  Serial.print("Could not join Wi-Fi. Setup network started: ");
  Serial.println(SETUP_NETWORK);
}

// Called every loop while the setup network is open.
void runSetupMode() {
  wifiManager.process();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Wi-Fi is set up. Restarting.");
    delay(1000);
    ESP.restart();
  }
  // Someone is using the setup page: don't time out on them.
  if (WiFi.softAPgetStationNum() > 0) setupIdleSince = millis();

  // Nobody came. The saved network may just have been down for a
  // while, so restart and try it again.
  if (millis() - setupIdleSince > SETUP_WINDOW_MS) {
    Serial.println("Setup timed out. Restarting to try the saved network again.");
    ESP.restart();
  }
}

void checkButton() {
  if (digitalRead(BOOT_BUTTON_PIN) == HIGH) {  // not pressed
    buttonDownSince = 0;
    return;
  }
  if (buttonDownSince == 0) buttonDownSince = millis();
  if (millis() - buttonDownSince > 5000) forgetWifi();
}

// ---------- Serial commands ----------
struct NamedColor {
  const char* name;
  uint8_t r, g, b;
};

const NamedColor COLORS[] = {
    {"red", 255, 30, 30},    {"green", 0, 220, 60},   {"blue", 0, 150, 255},
    {"white", 220, 220, 220}, {"yellow", 255, 200, 0}, {"orange", 255, 110, 0},
    {"purple", 170, 60, 255}, {"cyan", 0, 220, 220},   {"pink", 255, 80, 160},
};
const int COLOR_COUNT = sizeof(COLORS) / sizeof(COLORS[0]);

void printHelp() {
  Serial.println("Commands:");
  Serial.println("  msg <text>     show a message");
  Serial.println("  color <name>   red green blue white yellow orange purple cyan pink");
  Serial.println("  icon <name>    show an icon beside the message (icon none to remove)");
  Serial.println("  icons          list icon names");
  Serial.println("  clock          show the clock");
  Serial.println("  brightness <n> set brightness, 1 to 100");
  Serial.println("  status         show version, Wi-Fi and server connection status");
  Serial.println("  wifi-reset     forget the saved Wi-Fi network and start setup");
  Serial.println("  dark           turn the display off");
}

void handleCommand(String line) {
  line.trim();
  if (line.length() == 0) return;

  int space = line.indexOf(' ');
  String command = (space < 0) ? line : line.substring(0, space);
  String argument = (space < 0) ? String("") : line.substring(space + 1);
  command.toLowerCase();
  argument.trim();

  if (command == "msg") {
    state.mode = MODE_MESSAGE;
    state.text = argument;
    scrollX = WIDTH;
  } else if (command == "color") {
    argument.toLowerCase();
    bool found = false;
    for (int i = 0; i < COLOR_COUNT; i++) {
      if (argument == COLORS[i].name) {
        state.r = COLORS[i].r;
        state.g = COLORS[i].g;
        state.b = COLORS[i].b;
        found = true;
      }
    }
    if (!found) {
      Serial.println("Unknown color. Type help for the list.");
      return;
    }
  } else if (command == "icon") {
    argument.toLowerCase();
    if (argument == "none") {
      state.icon = -1;
    } else {
      int index = findIcon(argument);
      if (index < 0) {
        Serial.println("Unknown icon. Type icons for the list.");
        return;
      }
      state.icon = index;
    }
    scrollX = WIDTH;
  } else if (command == "icons") {
    for (int i = 0; i < ICON_COUNT; i++) Serial.println(ICONS[i].name);
    return;
  } else if (command == "clock") {
    state.mode = MODE_CLOCK;
  } else if (command == "brightness") {
    int percent = argument.toInt();
    if (percent < 1 || percent > 100) {
      Serial.println("Use a number from 1 to 100.");
      return;
    }
    applyBrightness(percent);
  } else if (command == "wifi-reset") {
    forgetWifi();
    return;
  } else if (command == "status") {
    Serial.print("Firmware: ");
    Serial.println(FIRMWARE_VERSION);
    Serial.println(WiFi.status() == WL_CONNECTED ? "Wi-Fi: connected" : "Wi-Fi: not connected");
    Serial.println(serverConnected ? "Server: connected" : "Server: not connected");
    return;
  } else if (command == "dark") {
    state.mode = MODE_DARK;
  } else {
    printHelp();
    return;
  }
  Serial.println("ok");
}

String inputLine;

void readSerial() {
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      handleCommand(inputLine);
      inputLine = "";
    } else {
      inputLine += c;
    }
  }
}

// ---------- Messages from the server ----------
// Applies a state message such as:
//   {"type":"state","state":{"mode":"message","text":"Busy",
//                            "color":"#ff1e1e","icon":"dnd",
//                            "brightness":30}}
// Returns false and changes nothing if the message isn't valid.
bool applyStateJson(const char* json, size_t length) {
  JsonDocument doc;
  if (deserializeJson(doc, json, length)) return false;
  if (strcmp(doc["type"] | "", "state") != 0) return false;

  JsonObject incoming = doc["state"];
  const char* mode = incoming["mode"] | "";
  const char* color = incoming["color"] | "";
  const char* icon = incoming["icon"] | "";  // null arrives as ""

  DisplayState next = state;
  if (strcmp(mode, "message") == 0) {
    next.mode = MODE_MESSAGE;
  } else if (strcmp(mode, "clock") == 0) {
    next.mode = MODE_CLOCK;
  } else if (strcmp(mode, "dark") == 0) {
    next.mode = MODE_DARK;
  } else {
    return false;
  }

  next.text = incoming["text"] | "";
  next.icon = findIcon(icon);  // -1 when there is no icon

  // Color arrives as "#rrggbb".
  if (strlen(color) == 7 && color[0] == '#') {
    long value = strtol(color + 1, nullptr, 16);
    next.r = (value >> 16) & 0xFF;
    next.g = (value >> 8) & 0xFF;
    next.b = value & 0xFF;
  }

  // Restart scrolling only when what's on screen actually changes.
  if (next.mode != state.mode || next.text != state.text || next.icon != state.icon) {
    scrollX = WIDTH;
  }
  state = next;

  int brightness = incoming["brightness"] | 0;  // 0 when not included
  if (brightness >= 1 && brightness <= 100) applyBrightness(brightness);
  return true;
}

void handleServerMessage(const char* json, size_t length) {
  // An update message: {"type":"update","version":"1.8.1","md5":"..."}
  JsonDocument doc;
  if (!deserializeJson(doc, json, length) && strcmp(doc["type"] | "", "update") == 0) {
    updateMd5 = doc["md5"] | "";
    updateRequested = true;  // the main loop does the download
    Serial.println("The server asked for a firmware update.");
    return;
  }

  if (!applyStateJson(json, length)) {
    Serial.println("Ignored a message from the server.");
    return;
  }
  // Remember the state so it survives a power cut. Flash memory wears
  // out with writes, so only save when something changed.
  String latest = String(json);
  if (latest != prefs.getString("state", "")) {
    prefs.putString("state", latest);
  }
}

// Tells the server which firmware this sign runs and, if the last
// update attempt failed, why.
void sendHello() {
  JsonDocument doc;
  doc["type"] = "hello";
  doc["version"] = FIRMWARE_VERSION;
  if (updateError.length() > 0) doc["updateError"] = updateError.c_str();
  char message[256];
  serializeJson(doc, message, sizeof(message));
  socket.sendTXT(message);
}

// Reaching the server proves new firmware works, so keep it.
void confirmFirmware() {
  if (!awaitingConfirmation) return;
  esp_ota_mark_app_valid_cancel_rollback();
  awaitingConfirmation = false;
  Serial.println("New firmware confirmed.");
}

void onSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_CONNECTED) {
    serverConnected = true;
    Serial.println("Server connected.");
    confirmFirmware();
    sendHello();
  } else if (type == WStype_DISCONNECTED) {
    if (serverConnected) Serial.println("Server disconnected. Retrying...");
    serverConnected = false;
  } else if (type == WStype_TEXT) {
    handleServerMessage((const char*)payload, length);
  }
}

// Runs every loop. The first time Wi-Fi comes up, start the clock
// sync and open the connection to the server.
void checkWifi() {
  if (wifiReady || WiFi.status() != WL_CONNECTED) return;
  wifiReady = true;
  Serial.println("Wi-Fi connected.");
  configTzTime(TIMEZONE, TIME_SERVER);

  String path = String("/device?sign=") + SIGN_ID;
  String header = String("Authorization: Bearer ") + DEVICE_TOKEN;
  socket.beginSslWithCA(SERVER_HOST, 443, path.c_str(), ROOT_CA_CERTS);
  socket.setExtraHeaders(header.c_str());
  socket.onEvent(onSocketEvent);
  socket.setReconnectInterval(5000);       // retry every 5 seconds
  socket.enableHeartbeat(15000, 3000, 2);  // notice a dead connection
}

// Downloads new firmware from the server and installs it. The sign has
// two firmware slots: the download goes into the spare one while this
// one keeps running, so a failed download changes nothing.
void performUpdate() {
  updateRequested = false;
  Serial.println("Downloading new firmware...");
  updating = true;
  render();
  socket.disconnect();  // free memory for the download's own connection

  WiFiClientSecure client;
  client.setCACert(ROOT_CA_CERTS);
  HTTPClient http;
  http.setTimeout(15000);
  String url = String("https://") + SERVER_HOST + "/api/device/firmware?sign=" + SIGN_ID;
  String error = "";

  if (!http.begin(client, url)) {
    error = "could not start the download";
  } else {
    http.addHeader("Authorization", String("Bearer ") + DEVICE_TOKEN);
    int status = http.GET();
    int size = http.getSize();
    if (status != 200) {
      error = String("server answered ") + String(status);
    } else if (size <= 0) {
      error = "server did not give a file size";
    } else if (!Update.begin(size)) {
      error = Update.errorString();
    } else {
      // The MD5 is a fingerprint of the file. If the download doesn't
      // match it, Update.end() refuses to install.
      if (updateMd5.length() == 32) Update.setMD5(updateMd5.c_str());
      size_t written = Update.writeStream(*http.getStreamPtr());
      if (written != (size_t)size) {
        error = "download stopped early";
        Update.abort();
      } else if (!Update.end()) {
        error = Update.errorString();
      }
    }
    http.end();
  }

  if (error.length() == 0) {
    Serial.println("Update installed. Restarting.");
    delay(500);
    ESP.restart();
  }

  // Failed: carry on with the current firmware. The connection to the
  // server reopens by itself and reports what went wrong.
  Serial.print("Update failed: ");
  Serial.println(error);
  updateError = error;
  updating = false;
}

// ---------- Main program ----------
void setup() {
  Serial.begin(115200);
  FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, 3000);
  // Dithering flickers LEDs to fake in-between colors. At this
  // sign's refresh rate that shows as shimmer when dim, so turn it off.
  FastLED.setDither(0);
  matrix.begin();
  applyBrightness(brightnessPercent);
  matrix.setTextWrap(false);
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  Serial.print("BrightByte firmware ");
  Serial.println(FIRMWARE_VERSION);
  printHelp();

  // Is this the first start after an update? If so, it still has to
  // prove itself by reaching the server.
  esp_ota_img_states_t imageState;
  if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &imageState) == ESP_OK &&
      imageState == ESP_OTA_IMG_PENDING_VERIFY) {
    awaitingConfirmation = true;
    Serial.println("New firmware: waiting to reach the server before keeping it.");
  }

  // Show whatever the sign was showing before it lost power.
  prefs.begin("sign", false);
  String saved = prefs.getString("state", "");
  if (saved.length() > 0) applyStateJson(saved.c_str(), saved.length());
  render();

  startWifi();
}

void loop() {
  readSerial();
  checkButton();

  if (setupMode) {
    runSetupMode();
  } else {
    checkWifi();
    if (wifiReady) socket.loop();
  }

  if (updateRequested) performUpdate();

  // New firmware that never reached the server: go back to the old one.
  if (awaitingConfirmation && millis() > CONFIRM_WINDOW_MS) {
    Serial.println("New firmware could not reach the server. Going back to the previous version.");
    delay(200);
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }

  // Redraw 20 times a second without blocking, so the sign
  // keeps listening for commands between frames.
  if (millis() - lastFrame >= FRAME_MS) {
    lastFrame = millis();
    render();
  }
}
