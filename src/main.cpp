#include <Arduino.h>
#include <FastLED.h>
#include <FastLED_NeoMatrix.h>

const int DATA_PIN = 13;
const int WIDTH = 32;
const int HEIGHT = 8;
const int NUM_LEDS = WIDTH * HEIGHT;

CRGB leds[NUM_LEDS];

// The last argument describes how the LEDs are physically chained.
FastLED_NeoMatrix matrix(leds, WIDTH, HEIGHT,
                         NEO_MATRIX_TOP + NEO_MATRIX_LEFT +
                         NEO_MATRIX_COLUMNS + NEO_MATRIX_ZIGZAG);

const char* message = "Hello, Jackie!";
int x = WIDTH;  // start just off the right edge

void setup() {
  FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, 3000);
  matrix.begin();
  matrix.setBrightness(20);
  matrix.setTextWrap(false);
  matrix.setTextColor(matrix.Color(0, 150, 255));
}

void loop() {
  matrix.fillScreen(0);
  matrix.setCursor(x, 0);
  matrix.print(message);
  matrix.show();

  int textWidth = strlen(message) * 6;  // each character is 6 pixels wide
  x--;
  if (x < -textWidth) x = WIDTH;
  delay(60);
}