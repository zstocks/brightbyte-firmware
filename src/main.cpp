#include <Arduino.h>
#include <FastLED.h>
#include <FastLED_NeoMatrix.h>

#include "icons.h"

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
// Later, the VPS will send exactly this over the WebSocket.
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

// ---------- Temporary clock ----------
// Until Wi-Fi arrives in Phase 3b, the clock counts up from a time
// you set with the "time" command.
int clockBaseMinutes = 12 * 60;
unsigned long clockBaseMillis = 0;

int minutesNow() {
  unsigned long elapsed = (millis() - clockBaseMillis) / 60000UL;
  return (clockBaseMinutes + (int)(elapsed % 1440UL)) % 1440;
}

// ---------- Drawing helpers ----------
int findIcon(const String& name) {
  for (int i = 0; i < ICON_COUNT; i++) {
    if (name == ICONS[i].name) return i;
  }
  return -1;
}

void drawIcon(int index, int x) {
  const Icon& icon = ICONS[index];
  uint16_t color = matrix.Color(icon.r, icon.g, icon.b);
  for (int row = 0; row < 8; row++) {
    for (int col = 0; col < 8; col++) {
      if (icon.rows[row] & (0x80 >> col)) {
        matrix.drawPixel(x + col, row, color);
      }
    }
  }
}

void drawText(const String& text, int x, uint16_t color) {
  for (unsigned int i = 0; i < text.length(); i++) {
    // Passing the same color as foreground and background
    // means "transparent background" in Adafruit GFX.
    matrix.drawChar(x + i * CHAR_WIDTH, 0, text[i], color, color, 1);
  }
}

void drawMessage() {
  uint16_t color = matrix.Color(state.r, state.g, state.b);
  int areaStart = (state.icon >= 0) ? 9 : 0;  // icon takes 8 columns + 1 gap
  int areaWidth = WIDTH - areaStart;
  int textWidth = state.text.length() * CHAR_WIDTH - 1;

  if (textWidth <= areaWidth) {
    // Short text: hold it still, centered in the free area.
    drawText(state.text, areaStart + (areaWidth - textWidth) / 2, color);
  } else {
    // Long text: scroll it right to left.
    drawText(state.text, scrollX, color);
    scrollX--;
    if (scrollX < areaStart - textWidth) scrollX = WIDTH;
  }

  if (state.icon >= 0) {
    matrix.fillRect(0, 0, areaStart, HEIGHT, 0);  // hide text behind the icon
    drawIcon(state.icon, 0);
  }
}

void drawClock() {
  uint16_t color = matrix.Color(state.r, state.g, state.b);
  int minutes = minutesNow();
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
  if (state.mode == MODE_MESSAGE) drawMessage();
  if (state.mode == MODE_CLOCK) drawClock();
  // MODE_DARK draws nothing.
  matrix.show();
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
  Serial.println("  time <HH:MM>   set the temporary clock, 24-hour (time 14:05)");
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
  } else if (command == "time") {
    int colon = argument.indexOf(':');
    int hour = (colon < 0) ? -1 : argument.substring(0, colon).toInt();
    int minute = (colon < 0) ? -1 : argument.substring(colon + 1).toInt();
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
      Serial.println("Use 24-hour HH:MM, for example: time 14:05");
      return;
    }
    clockBaseMinutes = hour * 60 + minute;
    clockBaseMillis = millis();
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

// ---------- Main program ----------
void setup() {
  Serial.begin(115200);
  FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, 3000);
  matrix.begin();
  matrix.setBrightness(20);
  matrix.setTextWrap(false);
  printHelp();
}

void loop() {
  readSerial();

  // Redraw 20 times a second without blocking, so the sign
  // keeps listening for commands between frames.
  if (millis() - lastFrame >= FRAME_MS) {
    lastFrame = millis();
    render();
  }
}