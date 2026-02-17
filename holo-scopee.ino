/*
 * HoloScopee — POV Holographic Display
 * =====================================
 * Arduino Nano | 9 LEDs (pins 2-10) | Hall sensor (pin 12) | Bluetooth (pins
 * 11,13)
 *
 * LEDs span the full diameter of a spinning motor:
 *   Pin2  Pin3  Pin4  Pin5  [AXLE]  Pin6  Pin7  Pin8  Pin9  Pin10
 *   <--- Side A (4 LEDs) --->       <---- Side B (5 LEDs) ---->
 *
 * At angle θ: Side A renders column θ, Side B renders column θ+180°
 *
 * Bluetooth commands (9600 baud, newline-terminated):
 *   T:Hello     — display text
 *   I:heart     — show icon (heart, smile, star, circle, check)
 *   C           — clear display
 *   B:5         — brightness/on-time (1-10)
 *   D:c1,c2,... — raw 9-bit column data
 */

#include <SoftwareSerial.h>
#include <avr/pgmspace.h>

// ============================================================
//  PIN CONFIGURATION
// ============================================================
#define NUM_LEDS 9
const byte ledPins[NUM_LEDS] = {2, 3, 4, 5, 6, 7, 8, 9, 10};

#define BT_TX_PIN 11 // Arduino TX → BT module RX
#define BT_RX_PIN 13 // Arduino RX ← BT module TX
#define HALL_PIN 12  // Hall sensor (digital, active LOW)

SoftwareSerial btSerial(BT_RX_PIN, BT_TX_PIN); // RX, TX

// ============================================================
//  POV CONFIGURATION
// ============================================================
#define NUM_COLUMNS 200 // Angular resolution (columns per revolution)
#define HALF_COLUMNS (NUM_COLUMNS / 2)
#define SIDE_A_LEDS 4 // LEDs on side A (pins 2-5)
#define SIDE_B_LEDS 5 // LEDs on side B (pins 6-10)

// Display buffer — each entry is a 9-bit mask (bit 0 = pin2/outerA, bit 8 =
// pin10/outerB)
uint16_t displayBuffer[NUM_COLUMNS];

// ============================================================
//  TIMING
// ============================================================
volatile unsigned long lastHallTrigger = 0;
volatile unsigned long revolutionTime = 0;
volatile bool newRevolution = false;

unsigned long columnDelay = 0; // Microseconds per column
byte brightness = 7;           // On-time factor (1-10)

// Debounce: ignore triggers faster than ~3ms (prevents noise)
#define MIN_REVOLUTION_US 3000UL
// Timeout: if no trigger for 500ms, motor is stopped
#define MAX_REVOLUTION_US 500000UL

// ============================================================
//  BLUETOOTH PARSING
// ============================================================
#define CMD_BUF_SIZE 128
char cmdBuffer[CMD_BUF_SIZE];
byte cmdIndex = 0;

// ============================================================
//  DISPLAY STATE
// ============================================================
enum DisplayMode { MODE_CLEAR, MODE_TEXT, MODE_ICON, MODE_CUSTOM, MODE_SCROLL };

DisplayMode currentMode = MODE_CLEAR;
char textBuffer[64];
int scrollOffset = 0;
unsigned long lastScrollTime = 0;
#define SCROLL_INTERVAL_MS 80 // Scroll speed in ms per step

// ============================================================
//  DEBUG: LED feedback for testing without motor
// ============================================================

/*
 * Flash all LEDs quickly to confirm a command was received.
 * Pattern: all ON briefly, then OFF — twice.
 */
void flashConfirm() {
  for (int i = 0; i < NUM_LEDS; i++)
    digitalWrite(ledPins[i], HIGH);
  delay(150);
  for (int i = 0; i < NUM_LEDS; i++)
    digitalWrite(ledPins[i], LOW);
  delay(100);
  for (int i = 0; i < NUM_LEDS; i++)
    digitalWrite(ledPins[i], HIGH);
  delay(150);
  for (int i = 0; i < NUM_LEDS; i++)
    digitalWrite(ledPins[i], LOW);
}

/*
 * Light each LED one-by-one pin 2→10 then 10→2 to test wiring.
 */
void testLedSequence() {
  for (int i = 0; i < NUM_LEDS; i++) {
    digitalWrite(ledPins[i], HIGH);
    delay(200);
    digitalWrite(ledPins[i], LOW);
  }
  for (int i = NUM_LEDS - 1; i >= 0; i--) {
    digitalWrite(ledPins[i], HIGH);
    delay(200);
    digitalWrite(ledPins[i], LOW);
  }
}

/*
 * Show column 0 of the display buffer on LEDs statically (no spinning).
 * Lets you see what's loaded without the motor.
 */
void showBufferStatic() {
  uint16_t col = displayBuffer[0];
  for (int i = 0; i < NUM_LEDS; i++) {
    digitalWrite(ledPins[i], (col >> i) & 1);
  }
}

// ============================================================
//  FONT DATA — 5 columns × 9 rows
//  Each character is 5 bytes. Each byte represents one column,
//  bit 0 = top row (outer Side A), bit 8 = bottom row (outer Side B)
//  Stored in PROGMEM to save RAM.
// ============================================================

// Font covers ASCII 32 (space) through ASCII 90 ('Z') + punctuation
// Characters: space ! " # $ % & ' ( ) * + , - . / 0-9 : ; < = > ? @ A-Z
#define FONT_START 32
#define FONT_CHARS 59 // ASCII 32..90
#define FONT_WIDTH 5

const uint16_t font5x9[][FONT_WIDTH] PROGMEM = {
    // ASCII 32: Space
    {0x000, 0x000, 0x000, 0x000, 0x000},
    // ASCII 33: !
    {0x000, 0x000, 0x17E, 0x000, 0x000},
    // ASCII 34: "
    {0x000, 0x00E, 0x000, 0x00E, 0x000},
    // ASCII 35: #
    {0x048, 0x1FE, 0x048, 0x1FE, 0x048},
    // ASCII 36: $
    {0x08C, 0x112, 0x1FF, 0x122, 0x0C4},
    // ASCII 37: %
    {0x186, 0x066, 0x018, 0x0CC, 0x0C6},
    // ASCII 38: &
    {0x0EC, 0x112, 0x12A, 0x0C4, 0x1A0},
    // ASCII 39: '
    {0x000, 0x000, 0x00E, 0x000, 0x000},
    // ASCII 40: (
    {0x000, 0x000, 0x07C, 0x082, 0x000},
    // ASCII 41: )
    {0x000, 0x082, 0x07C, 0x000, 0x000},
    // ASCII 42: *
    {0x028, 0x010, 0x07C, 0x010, 0x028},
    // ASCII 43: +
    {0x010, 0x010, 0x07C, 0x010, 0x010},
    // ASCII 44: ,
    {0x000, 0x100, 0x0C0, 0x000, 0x000},
    // ASCII 45: -
    {0x010, 0x010, 0x010, 0x010, 0x010},
    // ASCII 46: .
    {0x000, 0x000, 0x100, 0x000, 0x000},
    // ASCII 47: /
    {0x180, 0x060, 0x018, 0x006, 0x000},
    // ASCII 48: 0
    {0x0FE, 0x162, 0x11A, 0x10E, 0x0FE},
    // ASCII 49: 1
    {0x000, 0x104, 0x1FE, 0x100, 0x000},
    // ASCII 50: 2
    {0x184, 0x142, 0x122, 0x112, 0x10C},
    // ASCII 51: 3
    {0x082, 0x102, 0x112, 0x112, 0x0EC},
    // ASCII 52: 4
    {0x030, 0x028, 0x024, 0x1FE, 0x020},
    // ASCII 53: 5
    {0x09E, 0x112, 0x112, 0x112, 0x0E2},
    // ASCII 54: 6
    {0x0FC, 0x112, 0x112, 0x112, 0x0E0},
    // ASCII 55: 7
    {0x002, 0x1C2, 0x032, 0x00A, 0x006},
    // ASCII 56: 8
    {0x0EC, 0x112, 0x112, 0x112, 0x0EC},
    // ASCII 57: 9
    {0x00C, 0x112, 0x112, 0x112, 0x07E},
    // ASCII 58: :
    {0x000, 0x000, 0x044, 0x000, 0x000},
    // ASCII 59: ;
    {0x000, 0x100, 0x0C4, 0x000, 0x000},
    // ASCII 60: <
    {0x010, 0x028, 0x044, 0x082, 0x000},
    // ASCII 61: =
    {0x028, 0x028, 0x028, 0x028, 0x028},
    // ASCII 62: >
    {0x000, 0x082, 0x044, 0x028, 0x010},
    // ASCII 63: ?
    {0x004, 0x002, 0x162, 0x012, 0x00C},
    // ASCII 64: @
    {0x0FC, 0x102, 0x13A, 0x12A, 0x07C},
    // ASCII 65: A
    {0x1FC, 0x022, 0x022, 0x022, 0x1FC},
    // ASCII 66: B
    {0x1FE, 0x112, 0x112, 0x112, 0x0EC},
    // ASCII 67: C
    {0x0FC, 0x102, 0x102, 0x102, 0x084},
    // ASCII 68: D
    {0x1FE, 0x102, 0x102, 0x102, 0x0FC},
    // ASCII 69: E
    {0x1FE, 0x112, 0x112, 0x112, 0x102},
    // ASCII 70: F
    {0x1FE, 0x012, 0x012, 0x012, 0x002},
    // ASCII 71: G
    {0x0FC, 0x102, 0x102, 0x122, 0x0E4},
    // ASCII 72: H
    {0x1FE, 0x010, 0x010, 0x010, 0x1FE},
    // ASCII 73: I
    {0x000, 0x102, 0x1FE, 0x102, 0x000},
    // ASCII 74: J
    {0x0C0, 0x100, 0x100, 0x100, 0x0FE},
    // ASCII 75: K
    {0x1FE, 0x010, 0x028, 0x044, 0x182},
    // ASCII 76: L
    {0x1FE, 0x100, 0x100, 0x100, 0x100},
    // ASCII 77: M
    {0x1FE, 0x004, 0x008, 0x004, 0x1FE},
    // ASCII 78: N
    {0x1FE, 0x004, 0x018, 0x060, 0x1FE},
    // ASCII 79: O
    {0x0FC, 0x102, 0x102, 0x102, 0x0FC},
    // ASCII 80: P
    {0x1FE, 0x012, 0x012, 0x012, 0x00C},
    // ASCII 81: Q
    {0x0FC, 0x102, 0x142, 0x102, 0x0FC},
    // ASCII 82: R
    {0x1FE, 0x012, 0x012, 0x032, 0x1CC},
    // ASCII 83: S
    {0x08C, 0x112, 0x112, 0x112, 0x062},
    // ASCII 84: T
    {0x002, 0x002, 0x1FE, 0x002, 0x002},
    // ASCII 85: U
    {0x0FE, 0x100, 0x100, 0x100, 0x0FE},
    // ASCII 86: V
    {0x03E, 0x040, 0x180, 0x040, 0x03E},
    // ASCII 87: W
    {0x0FE, 0x100, 0x0FE, 0x100, 0x0FE},
    // ASCII 88: X
    {0x186, 0x048, 0x030, 0x048, 0x186},
    // ASCII 89: Y
    {0x006, 0x008, 0x1F0, 0x008, 0x006},
    // ASCII 90: Z
    {0x182, 0x142, 0x132, 0x10A, 0x106},
};

// ============================================================
//  BUILT-IN ICONS (full circular patterns, stored in PROGMEM)
//  Each icon is NUM_COLUMNS entries of 9-bit column data.
//  To save PROGMEM, icons are stored as compact row-based bitmaps
//  and expanded at runtime.
// ============================================================

// Icon rendering helper — we define icons as 9-row circular bitmaps
// Each row is a set of angular ranges where the LED is ON

// Simple procedural icon generator
void generateIcon(const char *name) {
  clearBuffer();

  if (strcmp(name, "heart") == 0) {
    generateHeart();
  } else if (strcmp(name, "smile") == 0) {
    generateSmile();
  } else if (strcmp(name, "star") == 0) {
    generateStar();
  } else if (strcmp(name, "circle") == 0) {
    generateCircle();
  } else if (strcmp(name, "check") == 0) {
    generateCheck();
  } else {
    // Unknown icon — flash all LEDs briefly as feedback
    for (int c = 0; c < NUM_COLUMNS; c++) {
      displayBuffer[c] = 0x1FF;
    }
  }
}

// Heart shape — symmetric, wider at top, pointed at bottom
void generateHeart() {
  // We'll draw the heart in the top half relative to center
  // Heart is drawn using angular columns, using the full display
  for (int c = 0; c < NUM_COLUMNS; c++) {
    // Normalize angle to 0..199
    float angle = (float)c / NUM_COLUMNS * 360.0;

    // Heart in polar coordinates (simplified)
    // Use a cardioid-like shape
    uint16_t col = 0;

    // Convert to radians
    float rad = angle * PI / 180.0;
    // Heart polar equation: r = 1 - sin(θ)
    float r = (1.0 - sin(rad)) * 4.0; // Scale to ~4 LED radii

    // Light up LEDs from center outward up to radius r
    // Side A: rows 3(inner) to 0(outer) = radii 1,2,3,4 from center
    // Side B: rows 4(inner) to 8(outer) = radii 1,2,3,4,5 from center
    int rInt = (int)(r + 0.5);
    if (rInt >= 1) {
      // Side A (bits 0-3, where bit 3 = innermost, bit 0 = outermost)
      for (int i = 0; i < SIDE_A_LEDS && i < rInt; i++) {
        col |= (1 << (SIDE_A_LEDS - 1 - i)); // inner to outer
      }
      // Side B (bits 4-8, where bit 4 = innermost, bit 8 = outermost)
      for (int i = 0; i < SIDE_B_LEDS && i < rInt; i++) {
        col |= (1 << (SIDE_A_LEDS + i)); // inner to outer
      }
    }
    displayBuffer[c] = col;
  }
}

// Smiley face — circle outline with eyes and mouth
void generateSmile() {
  for (int c = 0; c < NUM_COLUMNS; c++) {
    uint16_t col = 0;

    // Outer ring — light outermost LEDs all around
    col |= (1 << 0); // Side A outermost
    col |= (1 << 8); // Side B outermost

    // Eyes at roughly 60° and 300° (or mirrored)
    float angle = (float)c / NUM_COLUMNS * 360.0;
    if ((angle > 55 && angle < 75) || (angle > 285 && angle < 305)) {
      // Eyes — light middle LEDs
      col |= (1 << 1); // Side A, second from outside
      col |= (1 << 7); // Side B, second from outside
    }

    // Mouth — arc at bottom (around 150°-210°)
    if (angle > 140 && angle < 220) {
      col |= (1 << 2); // Side A, third from outside
      col |= (1 << 6); // Side B, third from outside
    }

    displayBuffer[c] = col;
  }
}

// Star — 5-pointed star shape
void generateStar() {
  for (int c = 0; c < NUM_COLUMNS; c++) {
    float angle = (float)c / NUM_COLUMNS * 360.0;

    // 5 points at 72° intervals, starting at top (270°)
    float starAngle = fmod(angle + 90.0, 360.0); // Rotate so top is 0
    float segAngle = fmod(starAngle, 72.0);      // Angle within segment

    // Radius varies: peaks at 0° of each segment, valleys at 36°
    float t = segAngle / 72.0; // 0..1 within segment
    float r;
    if (t < 0.5) {
      r = 4.0 - t * 5.0; // 4 down to 1.5
    } else {
      r = 1.5 + (t - 0.5) * 5.0; // 1.5 back up to 4
    }

    int rInt = (int)(r + 0.5);
    uint16_t col = 0;

    for (int i = 0; i < SIDE_A_LEDS && i < rInt; i++) {
      col |= (1 << (SIDE_A_LEDS - 1 - i));
    }
    for (int i = 0; i < SIDE_B_LEDS && i < rInt; i++) {
      col |= (1 << (SIDE_A_LEDS + i));
    }

    displayBuffer[c] = col;
  }
}

// Circle — just the outer ring
void generateCircle() {
  for (int c = 0; c < NUM_COLUMNS; c++) {
    uint16_t col = 0;
    // Light outermost LEDs on both sides
    col |= (1 << 0); // Side A outermost (pin 2)
    col |= (1 << 8); // Side B outermost (pin 10)
    // Also second-to-outer for thickness
    col |= (1 << 1);
    col |= (1 << 7);
    displayBuffer[c] = col;
  }
}

// Checkmark — angled check shape
void generateCheck() {
  clearBuffer();
  // Draw checkmark in angular range ~100° to ~260°
  // Short leg: 100°-150°, going from mid to inner
  // Long leg:  150°-260°, going from inner to outer
  for (int c = 0; c < NUM_COLUMNS; c++) {
    float angle = (float)c / NUM_COLUMNS * 360.0;

    if (angle >= 100 && angle <= 150) {
      float t = (angle - 100.0) / 50.0; // 0..1
      int r = 2 + (int)(t * 2);         // radius 2 to 4
      uint16_t col = 0;
      // Draw at this radius on both sides
      if (r < SIDE_A_LEDS)
        col |= (1 << (SIDE_A_LEDS - 1 - r));
      if (r < SIDE_B_LEDS)
        col |= (1 << (SIDE_A_LEDS + r));
      displayBuffer[c] = col;
    } else if (angle > 150 && angle <= 280) {
      float t = (angle - 150.0) / 130.0; // 0..1
      int r = 4 - (int)(t * 4);          // radius 4 down to 0
      uint16_t col = 0;
      if (r >= 0 && r < SIDE_A_LEDS)
        col |= (1 << (SIDE_A_LEDS - 1 - r));
      if (r >= 0 && r < SIDE_B_LEDS)
        col |= (1 << (SIDE_A_LEDS + r));
      displayBuffer[c] = col;
    }
  }
}

// ============================================================
//  CORE FUNCTIONS
// ============================================================

void clearBuffer() { memset(displayBuffer, 0, sizeof(displayBuffer)); }

/*
 * Render text into the display buffer.
 * Text is mapped around the circumference of the circle.
 * Each character = 5 columns + 1 space column = 6 columns
 * offset = scroll position for scrolling text
 */
void renderText(const char *text, int offset) {
  clearBuffer();

  int textLen = strlen(text);
  if (textLen == 0)
    return;

  // Total columns needed for the text
  int totalTextCols =
      textLen * 6 - 1; // 5 cols + 1 space per char, minus trailing space

  // For each display column, find which text column maps to it
  for (int c = 0; c < NUM_COLUMNS; c++) {
    // Map display column to text column (with offset for scrolling)
    int textCol = (c + offset) % totalTextCols;
    if (textCol < 0)
      textCol += totalTextCols;

    // Find which character and which column within that character
    int charIdx = textCol / 6;
    int colInChar = textCol % 6;

    if (colInChar >= FONT_WIDTH) {
      // This is the spacing column between characters
      displayBuffer[c] = 0;
      continue;
    }

    if (charIdx >= textLen) {
      displayBuffer[c] = 0;
      continue;
    }

    // Get the character
    char ch = text[charIdx];

    // Convert to uppercase if lowercase
    if (ch >= 'a' && ch <= 'z') {
      ch = ch - 'a' + 'A';
    }

    // Check if character is in our font range
    int fontIdx = ch - FONT_START;
    if (fontIdx < 0 || fontIdx >= FONT_CHARS) {
      // Unknown character — show as space
      displayBuffer[c] = 0;
      continue;
    }

    // Read column data from PROGMEM
    uint16_t colData = pgm_read_word(&font5x9[fontIdx][colInChar]);
    displayBuffer[c] = colData;
  }
}

/*
 * Parse raw column data from Bluetooth: "D:val1,val2,val3,..."
 * Each value is a 9-bit number (0-511)
 */
void parseCustomBitmap(const char *data) {
  clearBuffer();

  int col = 0;
  const char *p = data;

  while (*p && col < NUM_COLUMNS) {
    // Parse number
    uint16_t val = 0;
    while (*p >= '0' && *p <= '9') {
      val = val * 10 + (*p - '0');
      p++;
    }
    displayBuffer[col] = val & 0x1FF; // Mask to 9 bits
    col++;

    if (*p == ',')
      p++; // Skip comma separator
  }
}

// ============================================================
//  HALL SENSOR (Polling-based)
//  Pin 12 is on Port B which is used by SoftwareSerial's PCINT,
//  so we poll instead of using our own ISR.
// ============================================================

bool lastHallState = HIGH; // Previous reading (INPUT_PULLUP → default HIGH)

void setupHallSensor() { pinMode(HALL_PIN, INPUT_PULLUP); }

/*
 * Poll the hall sensor for a falling edge (magnet near = LOW).
 * Call this frequently in loop().
 */
void pollHallSensor() {
  bool currentState = digitalRead(HALL_PIN);

  // Detect falling edge: was HIGH, now LOW
  if (lastHallState == HIGH && currentState == LOW) {
    unsigned long now = micros();
    unsigned long elapsed = now - lastHallTrigger;

    // Debounce: ignore if too fast
    if (elapsed > MIN_REVOLUTION_US) {
      revolutionTime = elapsed;
      lastHallTrigger = now;
      newRevolution = true;
    }
  }

  lastHallState = currentState;
}

// ============================================================
//  BLUETOOTH COMMAND PROCESSING
// ============================================================

void processCommand(const char *cmd) {
  if (cmd[0] == '\0')
    return;

  // Debug: echo command to USB Serial Monitor
  Serial.print(F("[BT CMD] "));
  Serial.println(cmd);

  if (cmd[0] == 'T' && cmd[1] == ':') {
    // Text command: T:Hello World
    strncpy(textBuffer, cmd + 2, sizeof(textBuffer) - 1);
    textBuffer[sizeof(textBuffer) - 1] = '\0';
    scrollOffset = 0;

    // Decide: static or scrolling
    int textLen = strlen(textBuffer);
    int totalCols = textLen * 6 - 1;
    if (totalCols > NUM_COLUMNS) {
      currentMode = MODE_SCROLL;
    } else {
      currentMode = MODE_TEXT;
    }
    renderText(textBuffer, 0);
    btSerial.print(F("OK:TEXT="));
    btSerial.println(textBuffer);
    Serial.print(F("[OK] Text: "));
    Serial.println(textBuffer);
    flashConfirm();
    showBufferStatic();

  } else if (cmd[0] == 'I' && cmd[1] == ':') {
    // Icon command: I:heart
    generateIcon(cmd + 2);
    currentMode = MODE_ICON;
    btSerial.print(F("OK:ICON="));
    btSerial.println(cmd + 2);
    Serial.print(F("[OK] Icon: "));
    Serial.println(cmd + 2);
    flashConfirm();
    showBufferStatic();

  } else if (cmd[0] == 'C') {
    // Clear: C
    clearBuffer();
    currentMode = MODE_CLEAR;
    allLedsOff();
    btSerial.println(F("OK:CLEAR"));
    Serial.println(F("[OK] Cleared"));
    flashConfirm();

  } else if (cmd[0] == 'B' && cmd[1] == ':') {
    // Brightness: B:5
    int val = atoi(cmd + 2);
    if (val >= 1 && val <= 10) {
      brightness = val;
      btSerial.print(F("OK:BRI="));
      btSerial.println(val);
      Serial.print(F("[OK] Bri="));
      Serial.println(val);
      flashConfirm();
    } else {
      btSerial.println(F("ERR:BRI 1-10"));
      Serial.println(F("[ERR] Bri 1-10"));
    }

  } else if (cmd[0] == 'D' && cmd[1] == ':') {
    // Custom bitmap: D:511,0,255,...
    parseCustomBitmap(cmd + 2);
    currentMode = MODE_CUSTOM;
    btSerial.println(F("OK:CUSTOM"));
    Serial.println(F("[OK] Custom"));
    flashConfirm();
    showBufferStatic();

  } else if (strcmp(cmd, "TEST") == 0) {
    // LED wiring test: light each LED one by one
    btSerial.println(F("OK:TESTING..."));
    Serial.println(F("[TEST] LEDs..."));
    testLedSequence();
    btSerial.println(F("OK:TEST DONE"));
    Serial.println(F("[TEST] Done"));

  } else if (strcmp(cmd, "STATUS") == 0) {
    // Report current status
    btSerial.print(F("M:"));
    btSerial.print(currentMode);
    btSerial.print(F(" B:"));
    btSerial.print(brightness);
    btSerial.print(F(" R:"));
    btSerial.print(revolutionTime);
    btSerial.println(F("us"));
    Serial.print(F("[ST] M="));
    Serial.print(currentMode);
    Serial.print(F(" B="));
    Serial.print(brightness);
    Serial.print(F(" R="));
    Serial.print(revolutionTime);
    Serial.println(F("us"));

  } else {
    btSerial.println(F("ERR:?"));
    Serial.print(F("[ERR] "));
    Serial.println(cmd);
  }
}

// Track if any bytes ever arrived (helps diagnose wiring issues)
bool btEverReceived = false;
unsigned long lastByteTime = 0;

void readBluetooth() {
  while (btSerial.available()) {
    char c = btSerial.read();
    lastByteTime = millis();

    // First-ever byte? Print a big notice
    if (!btEverReceived) {
      btEverReceived = true;
      Serial.println(F(">>> BT DATA DETECTED! <<<"));
    }

    // Log every raw byte for debugging
    Serial.print(F("[RAW] 0x"));
    if ((byte)c < 0x10)
      Serial.print('0');
    Serial.print((byte)c, HEX);
    Serial.print(F(" '"));
    if (c >= 32 && c < 127)
      Serial.print(c);
    else
      Serial.print('.');
    Serial.println(F("'"));

    if (c == '\n' || c == '\r') {
      if (cmdIndex > 0) {
        cmdBuffer[cmdIndex] = '\0';
        Serial.print(F("[CMD] Processing: "));
        Serial.println(cmdBuffer);
        processCommand(cmdBuffer);
        cmdIndex = 0;
      }
    } else {
      if (cmdIndex < CMD_BUF_SIZE - 1) {
        cmdBuffer[cmdIndex++] = c;
      }
    }
  }

  // Fallback: if bytes arrived but no newline for 2 seconds, process anyway
  // (handles phone apps that don't send newline)
  if (cmdIndex > 0 && (millis() - lastByteTime > 2000)) {
    cmdBuffer[cmdIndex] = '\0';
    Serial.println(F("[TIMEOUT] No newline, processing anyway:"));
    Serial.println(cmdBuffer);
    processCommand(cmdBuffer);
    cmdIndex = 0;
  }
}

// ============================================================
//  POV RENDERING ENGINE
// ============================================================

/*
 * Write one column of LED data.
 *
 * At the current arm position (angle θ):
 *   - Side A (pins 2-5): bits 0-3 of the CURRENT column
 *     bit 0 = pin 2 (outermost), bit 3 = pin 5 (innermost near axle)
 *   - Side B (pins 6-10): bits 4-8 of the OPPOSITE column (+180°)
 *     bit 4 = pin 6 (innermost near axle), bit 8 = pin 10 (outermost)
 */
void renderColumn(int colIdx) {
  // Current column for Side A
  uint16_t colA = displayBuffer[colIdx];
  // Opposite column for Side B (180° away)
  int oppositeIdx = (colIdx + HALF_COLUMNS) % NUM_COLUMNS;
  uint16_t colB = displayBuffer[oppositeIdx];

  // Set Side A LEDs (pins 2-5) from bits 0-3 of colA
  for (int i = 0; i < SIDE_A_LEDS; i++) {
    digitalWrite(ledPins[i], (colA >> i) & 1);
  }

  // Set Side B LEDs (pins 6-10) from bits 4-8 of colB
  for (int i = 0; i < SIDE_B_LEDS; i++) {
    digitalWrite(ledPins[SIDE_A_LEDS + i], (colB >> (SIDE_A_LEDS + i)) & 1);
  }
}

void allLedsOff() {
  for (int i = 0; i < NUM_LEDS; i++) {
    digitalWrite(ledPins[i], LOW);
  }
}

/*
 * Main rendering loop for one full revolution.
 * Called when the hall sensor triggers, renders all columns
 * with proper timing based on measured revolution time.
 */
void renderRevolution() {
  if (revolutionTime == 0 || revolutionTime > MAX_REVOLUTION_US) {
    allLedsOff();
    return;
  }

  columnDelay = revolutionTime / NUM_COLUMNS;

  // Calculate on-time based on brightness (fraction of column time)
  unsigned long onTime = (columnDelay * brightness) / 10;
  unsigned long offTime = columnDelay - onTime;

  unsigned long startTime = micros();

  for (int c = 0; c < HALF_COLUMNS; c++) {
    // Render this column (handles both sides simultaneously)
    renderColumn(c);

    // On-time
    if (onTime > 0)
      delayMicroseconds(onTime);

    // Turn off for off-time (creates brightness control)
    if (offTime > 4) {
      allLedsOff();
      delayMicroseconds(offTime);
    }

    // Check if we've exceeded expected revolution time (safety)
    if (micros() - startTime > revolutionTime)
      break;
  }

  allLedsOff();
}

// ============================================================
//  SETUP & LOOP
// ============================================================

void setup() {
  // Configure LED pins as outputs
  for (int i = 0; i < NUM_LEDS; i++) {
    pinMode(ledPins[i], OUTPUT);
    digitalWrite(ledPins[i], LOW);
  }

  // Initialize USB Serial for debug monitoring
  Serial.begin(9600);
  Serial.println(F("=========================="));
  Serial.println(F(" HoloScopee POV Display"));
  Serial.println(F("=========================="));
  Serial.println(F("USB Serial active."));
  Serial.println(F("Waiting for BT cmds..."));
  Serial.println();

  // Initialize Bluetooth serial
  btSerial.begin(9600);
  btSerial.println(F("HoloScopee Ready!"));
  btSerial.println(F("T:text I:icon C B:1-10 TEST STATUS"));

  // Setup hall sensor (polling)
  setupHallSensor();

  // Initialize with clear display
  clearBuffer();
  currentMode = MODE_CLEAR;

  // Startup LED test — quick flash to show it's alive
  Serial.println(F("[BOOT] LED flash..."));
  flashConfirm();
  Serial.println(F("[BOOT] Ready!"));
  Serial.println();
}

void loop() {
  // Poll hall sensor for revolution detection
  pollHallSensor();

  // Read incoming Bluetooth commands
  readBluetooth();

  // Handle scrolling text
  if (currentMode == MODE_SCROLL) {
    unsigned long now = millis();
    if (now - lastScrollTime >= SCROLL_INTERVAL_MS) {
      lastScrollTime = now;
      scrollOffset++;
      int textLen = strlen(textBuffer);
      int totalCols = textLen * 6 - 1;
      if (scrollOffset >= totalCols)
        scrollOffset = 0;
      renderText(textBuffer, scrollOffset);
    }
  }

  // Render the display if motor is spinning
  if (newRevolution) {
    newRevolution = false;
    renderRevolution();
  }
}
