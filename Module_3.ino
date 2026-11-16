/*
  ROW THE BOAT
  Sanjidah and Nicole

  Player 1 (host): PLAYER_ID 0
  Player 2:        PLAYER_ID 1
*/

#include <WiFi.h>
#include <esp_now.h>
#include <TFT_eSPI.h>
#include <math.h>

// GAME SETTINGS
#define TEAM_ID             7
#define PLAYER_ID           0  // Player 1 = 0, Player 2 = 1
#define MAX_PLAYERS         2

#define FINISH_DISTANCE     50
#define MAX_DRIFT           2
#define GAME_TIME           45000
#define RESTART_DELAY       3000

#define BUTTON_A            0
#define BUTTON_B            35

#define HEARTBEAT_MS        250
#define DEBOUNCE_MS         20

// ESP-NOW
uint8_t broadcastAddress[] = {
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite waterSprite = TFT_eSprite(&tft);

bool waterSpriteReady = false;

int lastDisplayedSecond = -1;
int lastRestartSecond = -1;

// COLORS
const uint16_t WATER = 0x65FF;
const uint16_t LIGHT_WATER = 0x8EFF;
const uint16_t RIPPLE = 0xFFFF;
const uint16_t DEEP_WATER = 0x4D9F;

const uint16_t BOAT_COLOR = 0xA145;
const uint16_t WOOD_COLOR = 0xD3A4;
const uint16_t SEAT_COLOR = 0xF6C85F;
const uint16_t DARK_COLOR = 0x18C3;

// GAME STATUS
#define WAITING   0
#define PLAYING   1
#define WON       2
#define CAPSIZED  3
#define TIME_UP   4

// MESSAGE TYPES
#define PRESS_MESSAGE  1
#define STATE_MESSAGE  2

// SHARED GAME VARIABLES
uint16_t progress = 0;
uint8_t drift = 0;
int lastPlayer = -1;

uint8_t gameStatus = WAITING;

bool gameStarted = false;
bool gameOver = false;

uint32_t gameStartTime = 0;
uint32_t gameOverAt = 0;
uint32_t lastHeartbeat = 0;
uint32_t lastPressTime = 0;

uint32_t restartAt = 0;

bool lastA = HIGH;
bool lastB = HIGH;

volatile bool needsRedraw = true;

// RECEIVED MESSAGES
typedef struct {
  uint8_t teamId;
  uint8_t playerId;
  uint8_t messageType;

  uint8_t lastPlayer;
  uint8_t drift;
  uint8_t gameStatus;

  uint16_t progress;

  uint32_t timeLeft;
  uint16_t restartIn;
} Message;

volatile uint8_t remotePressCount = 0;
volatile bool newStateReceived = false;

Message receivedState;

// --------------------------------------------------
// SEND A BUTTON PRESS FROM PLAYER 2 TO PLAYER 1
// --------------------------------------------------

void sendPress() {
  Message msg = {};

  msg.teamId = TEAM_ID;
  msg.playerId = PLAYER_ID;
  msg.messageType = PRESS_MESSAGE;

  esp_now_send(
    broadcastAddress,
    (uint8_t *)&msg,
    sizeof(msg)
  );
}

// --------------------------------------------------
// SEND THE SHARED GAME STATE FROM PLAYER 1
// --------------------------------------------------

void sendState() {
  if (PLAYER_ID != 0) return;

  Message msg = {};

  msg.teamId = TEAM_ID;
  msg.playerId = PLAYER_ID;
  msg.messageType = STATE_MESSAGE;

  msg.lastPlayer = (lastPlayer < 0) ? 255 : lastPlayer;
  msg.drift = drift;
  msg.gameStatus = gameStatus;
  msg.progress = progress;

  if (gameStarted) {
    uint32_t elapsed = millis() - gameStartTime;

    msg.timeLeft = (elapsed >= GAME_TIME)
      ? 0
      : GAME_TIME - elapsed;
  } else {
    msg.timeLeft = gameOver ? 0 : GAME_TIME;
  }

  if (gameOver) {
    uint32_t elapsed = millis() - gameOverAt;

    msg.restartIn = (elapsed >= RESTART_DELAY)
      ? 0
      : RESTART_DELAY - elapsed;
  } else {
    msg.restartIn = 0;
  }

  esp_now_send(
    broadcastAddress,
    (uint8_t *)&msg,
    sizeof(msg)
  );
}

// --------------------------------------------------
// RECEIVE ESP-NOW MESSAGES
// --------------------------------------------------

void onReceive(
  const esp_now_recv_info_t *info,
  const uint8_t *data,
  int len
) {
  if (len != sizeof(Message)) return;

  Message msg;
  memcpy(&msg, data, sizeof(msg));

  if (msg.teamId != TEAM_ID) return;
  if (msg.playerId >= MAX_PLAYERS) return;

  // Player 1 receives button presses from Player 2.
  if (PLAYER_ID == 0 &&
      msg.messageType == PRESS_MESSAGE &&
      msg.playerId == 1) {

    if (remotePressCount < 20) {
      remotePressCount++;
    }
  }

  // Player 2 receives the shared state from Player 1.
  if (PLAYER_ID == 1 &&
      msg.messageType == STATE_MESSAGE &&
      msg.playerId == 0) {

    memcpy(&receivedState, &msg, sizeof(Message));
    newStateReceived = true;
  }
}

// --------------------------------------------------
// SET UP ESP-NOW
// --------------------------------------------------

void setupEspNow() {
  WiFi.mode(WIFI_STA);

  if (esp_now_init() != ESP_OK) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_RED);

    tft.drawCentreString(
      "ESP-NOW FAILED",
      120,
      60,
      2
    );

    while (true) {
      delay(1000);
    }
  }

  esp_now_register_recv_cb(onReceive);

  esp_now_peer_info_t peer = {};

  memcpy(
    peer.peer_addr,
    broadcastAddress,
    6
  );

  peer.channel = 0;
  peer.encrypt = false;

  if (!esp_now_is_peer_exist(broadcastAddress)) {
    esp_now_add_peer(&peer);
  }
}

// --------------------------------------------------
// APPLY RECEIVED STATE ON PLAYER 2
// --------------------------------------------------

void applyReceivedState() {
  if (PLAYER_ID != 1 || !newStateReceived) return;

  newStateReceived = false;

  Message msg;
  memcpy(&msg, &receivedState, sizeof(Message));

  bool changed =
    progress != msg.progress ||
    drift != msg.drift ||
    gameStatus != msg.gameStatus ||
    lastPlayer != (msg.lastPlayer == 255 ? -1 : msg.lastPlayer);

  progress = msg.progress;
  drift = msg.drift;
  gameStatus = msg.gameStatus;

  lastPlayer = (msg.lastPlayer == 255)
    ? -1
    : msg.lastPlayer;

  gameStarted = (gameStatus != WAITING);
  gameOver = (gameStatus >= WON);

  if (gameStatus == PLAYING) {
    gameStartTime = millis() - (GAME_TIME - msg.timeLeft);
  }

  if (gameOver) {
    restartAt = millis() + msg.restartIn;
  }

  if (changed) {
    needsRedraw = true;
  }
}

// --------------------------------------------------
// BLEND COLORS FOR OMBRE WATER
// --------------------------------------------------

uint16_t mixColor(
  uint16_t first,
  uint16_t second,
  uint8_t amount
) {
  int r1 = (first >> 11) & 0x1F;
  int g1 = (first >> 5) & 0x3F;
  int b1 = first & 0x1F;

  int r2 = (second >> 11) & 0x1F;
  int g2 = (second >> 5) & 0x3F;
  int b2 = second & 0x1F;

  int r = r1 + ((r2 - r1) * amount) / 255;
  int g = g1 + ((g2 - g1) * amount) / 255;
  int b = b1 + ((b2 - b1) * amount) / 255;

  return (r << 11) | (g << 5) | b;
}

// --------------------------------------------------
// DRAW THE WATER
// --------------------------------------------------

void drawWater() {
  if (!waterSpriteReady) {
    tft.fillRect(0, 18, 240, 77, WATER);
    return;
  }

  waterSprite.fillScreen(WATER);

  // Blue-to-turquoise ombre gradient.
  for (int y = 0; y < 77; y++) {
    uint8_t amount = (y * 255) / 76;

    uint16_t rowColor = mixColor(
      0x35B8,
      0x8F5F,
      amount
    );

    waterSprite.drawFastHLine(
      0,
      y,
      240,
      rowColor
    );
  }

  // Ripples shift as the boat moves.
  int waterMovement = progress * 13;

  for (int i = 0; i < 13; i++) {
    int x = ((i * 47) + waterMovement) % 260 - 10;

    int y = 4 + ((i * 13 + progress * 2) % 68);

    uint16_t outer = (i % 2 == 0)
      ? 0x5E7D
      : 0x7F1F;

    uint16_t middle = (i % 2 == 0)
      ? 0x8F7F
      : 0xA73F;

    uint16_t inner = (i % 3 == 0)
      ? 0xDFFF
      : 0xBFFF;

    int width = 9 + (i % 4) * 3;
    int height = 3 + (i % 3);

    // Layered oval ripples.
    waterSprite.drawEllipse(
      x,
      y,
      width,
      height,
      outer
    );

    waterSprite.drawEllipse(
      x,
      y,
      width - 3,
      height - 1,
      middle
    );

    waterSprite.drawEllipse(
      x,
      y,
      width - 6,
      1,
      inner
    );

    if (i % 3 == 0) {
      waterSprite.drawPixel(
        x + width + 4,
        y + 3,
        0xEFFF
      );
    }
  }

  waterSprite.pushSprite(0, 18);
}

// --------------------------------------------------
// BOAT ROTATION HELPERS
// --------------------------------------------------

void rotatePoint(
  int x,
  int y,
  int centerX,
  int centerY,
  int angle,
  int &outX,
  int &outY
) {
  float radians = angle * 0.0174533;

  float cosine = cos(radians);
  float sine = sin(radians);

  outX = centerX + (int)(x * cosine - y * sine);
  outY = centerY + (int)(x * sine + y * cosine);
}

void fillRotatedTriangle(
  int centerX,
  int centerY,
  int angle,
  int x1,
  int y1,
  int x2,
  int y2,
  int x3,
  int y3,
  uint16_t color
) {
  int ax, ay, bx, by, cx, cy;

  rotatePoint(x1, y1, centerX, centerY, angle, ax, ay);
  rotatePoint(x2, y2, centerX, centerY, angle, bx, by);
  rotatePoint(x3, y3, centerX, centerY, angle, cx, cy);

  tft.fillTriangle(
    ax, ay,
    bx, by,
    cx, cy,
    color
  );
}

void drawRotatedLine(
  int centerX,
  int centerY,
  int angle,
  int x1,
  int y1,
  int x2,
  int y2,
  uint16_t color
) {
  int ax, ay, bx, by;

  rotatePoint(x1, y1, centerX, centerY, angle, ax, ay);
  rotatePoint(x2, y2, centerX, centerY, angle, bx, by);

  tft.drawLine(ax, ay, bx, by, color);
}

// --------------------------------------------------
// DRAW THE WOODEN BOAT FROM ABOVE
// --------------------------------------------------

void drawBoat(
  int centerX,
  int centerY,
  bool flipped
) {
  int angle = 0;

  // Make drifting obvious.
  if (flipped) {
    angle = 180;
  } else if (drift == 1) {
    angle = (lastPlayer == 0) ? 32 : -32;
  }

  uint16_t hullColor = flipped
    ? DARK_COLOR
    : BOAT_COLOR;

  uint16_t insideColor = flipped
    ? BOAT_COLOR
    : WOOD_COLOR;

  // Outer pointed hull.
  fillRotatedTriangle(
    centerX, centerY, angle,
    0, -23,
    -15, -10,
    15, -10,
    hullColor
  );

  fillRotatedTriangle(
    centerX, centerY, angle,
    -15, -10,
    15, -10,
    15, 11,
    hullColor
  );

  fillRotatedTriangle(
    centerX, centerY, angle,
    -15, -10,
    15, 11,
    -15, 11,
    hullColor
  );

  fillRotatedTriangle(
    centerX, centerY, angle,
    -15, 11,
    15, 11,
    0, 23,
    hullColor
  );

  // Inner wooden area.
  if (!flipped) {
    fillRotatedTriangle(
      centerX, centerY, angle,
      0, -17,
      -10, -8,
      10, -8,
      insideColor
    );

    fillRotatedTriangle(
      centerX, centerY, angle,
      -10, -8,
      10, -8,
      10, 9,
      insideColor
    );

    fillRotatedTriangle(
      centerX, centerY, angle,
      -10, -8,
      10, 9,
      -10, 9,
      insideColor
    );

    fillRotatedTriangle(
      centerX, centerY, angle,
      -10, 9,
      10, 9,
      0, 17,
      insideColor
    );

    // Two seats across the boat.
    drawRotatedLine(
      centerX, centerY, angle,
      -9, -5,
      9, -5,
      SEAT_COLOR
    );

    drawRotatedLine(
      centerX, centerY, angle,
      -9, 6,
      9, 6,
      SEAT_COLOR
    );

    drawRotatedLine(
      centerX, centerY, angle,
      -8, -3,
      8, -3,
      DARK_COLOR
    );

    drawRotatedLine(
      centerX, centerY, angle,
      -8, 8,
      8, 8,
      DARK_COLOR
    );
  } else {
    // Planks on the underside when flipped.
    drawRotatedLine(
      centerX, centerY, angle,
      -10, -9,
      10, -9,
      TFT_WHITE
    );

    drawRotatedLine(
      centerX, centerY, angle,
      -12, 0,
      12, 0,
      TFT_WHITE
    );

    drawRotatedLine(
      centerX, centerY, angle,
      -10, 9,
      10, 9,
      TFT_WHITE
    );
  }

  // Oars and wide blades.
  drawRotatedLine(
    centerX, centerY, angle,
    -12, -5,
    -29, 10,
    DARK_COLOR
  );

  drawRotatedLine(
    centerX, centerY, angle,
    12, -5,
    29, 10,
    DARK_COLOR
  );

  drawRotatedLine(
    centerX, centerY, angle,
    -32, 8,
    -27, 13,
    WOOD_COLOR
  );

  drawRotatedLine(
    centerX, centerY, angle,
    27, 13,
    32, 8,
    WOOD_COLOR
  );

  if (flipped) {
    // Bubbles make capsizing more noticeable.
    tft.drawCircle(
      centerX - 34,
      centerY - 13,
      3,
      TFT_WHITE
    );

    tft.drawCircle(
      centerX + 34,
      centerY - 4,
      2,
      TFT_WHITE
    );

    tft.drawCircle(
      centerX - 27,
      centerY + 18,
      2,
      TFT_WHITE
    );
  }
}

// --------------------------------------------------
// DRAW TIMER
// --------------------------------------------------

void drawTimer() {
  uint32_t timeLeft = GAME_TIME;

  if (gameStarted) {
    uint32_t elapsed = millis() - gameStartTime;

    timeLeft = (elapsed >= GAME_TIME)
      ? 0
      : GAME_TIME - elapsed;
  }

  int secondsLeft = (timeLeft + 999) / 1000;

  tft.fillRect(
    183,
    0,
    57,
    18,
    DARK_COLOR
  );

  tft.setTextColor(
    TFT_WHITE,
    DARK_COLOR
  );

  tft.drawRightString(
    String(secondsLeft) + "s",
    235,
    2,
    2
  );
}

// --------------------------------------------------
// DRAW PROGRESS BAR AND DRIFT INDICATORS
// --------------------------------------------------

void drawGameInfo() {
  // Keep the dark panel behind progress and drift.
  // The water ends at y=95, so it cannot cover this panel.
  tft.fillRect(0, 95, 240, 40, DARK_COLOR);

  tft.setTextColor(TFT_WHITE, DARK_COLOR);

  tft.drawString(
    "PROGRESS",
    5,
    98,
    1
  );

  tft.drawRightString(
    String(progress) + "/50",
    235,
    98,
    1
  );

  tft.drawRoundRect(
    5,
    111,
    230,
    9,
    3,
    TFT_WHITE
  );

  int progressWidth =
    (progress * 226) / FINISH_DISTANCE;

  if (progressWidth > 0) {
    tft.fillRoundRect(
      7,
      113,
      progressWidth,
      5,
      2,
      TFT_GREEN
    );
  }

  tft.drawString(
    "DRIFT",
    5,
    123,
    1
  );

  // Two drift indicators because MAX_DRIFT is 2.
  for (int i = 0; i < MAX_DRIFT; i++) {
    uint16_t color = (i < drift)
      ? TFT_RED
      : TFT_WHITE;

    tft.fillCircle(
      49 + i * 19,
      128,
      5,
      color
    );
  }

  if (drift == 1 && gameStatus == PLAYING) {
    tft.setTextColor(TFT_RED, DARK_COLOR);

    tft.drawRightString(
      "SWITCH PLAYERS!",
      235,
      123,
      1
    );
  }
}

// --------------------------------------------------
// DRAW GAME MESSAGES
// --------------------------------------------------

void drawGameMessage() {
  if (gameStatus == WAITING) {
    tft.fillRoundRect(
      48, 83, 144, 14, 5, 0xFEA0
    );

    tft.setTextColor(TFT_BLACK, 0xFEA0);

    tft.drawCentreString(
      "PRESS A BUTTON TO START",
      120,
      85,
      1
    );
  }

  else if (gameStatus == PLAYING && drift == 1) {
    tft.fillRoundRect(
      76, 84, 88, 11, 4, TFT_RED
    );

    tft.setTextColor(TFT_WHITE, TFT_RED);

    tft.drawCentreString(
      "DRIFT! SWITCH!",
      120,
      85,
      1
    );
  }

  else if (gameStatus >= WON) {
    tft.fillRoundRect(
      25, 43, 190, 29, 7, DARK_COLOR
    );

    tft.drawRoundRect(
      25, 43, 190, 29, 7, TFT_WHITE
    );

    tft.setTextColor(TFT_WHITE, DARK_COLOR);

    if (gameStatus == WON) {
      tft.drawCentreString("You made it!", 120, 50, 2);
    }
    else if (gameStatus == CAPSIZED) {
      tft.drawCentreString("Uh oh, try again!", 120, 50, 2);
    }
    else if (gameStatus == TIME_UP) {
      tft.drawCentreString("Times up!", 120, 50, 2);
    }
  }
}

// --------------------------------------------------
// DRAW RESTART COUNTDOWN
// --------------------------------------------------

void drawRestartCountdown() {
  if (!gameOver) return;

  uint32_t remaining = 0;

  if (PLAYER_ID == 0) {
    uint32_t elapsed = millis() - gameOverAt;

    remaining = (elapsed >= RESTART_DELAY)
      ? 0
      : RESTART_DELAY - elapsed;
  } else {
    int32_t difference =
      (int32_t)(restartAt - millis());

    remaining = (difference > 0)
      ? difference
      : 0;
  }

  int secondsLeft = (remaining + 999) / 1000;

  tft.fillRoundRect(
    40,
    73,
    160,
    15,
    5,
    0xB7F5
  );

  tft.setTextColor(TFT_BLACK, 0xB7F5);

  tft.drawCentreString(
    "Restarting in " + String(secondsLeft) + "...",
    120,
    75,
    1
  );
}

// --------------------------------------------------
// DRAW THE CHANGING GAME SCENE
// --------------------------------------------------

void drawDynamicScene() {
  // Refresh only the water area between the title and bottom panel.
  drawWater();

  // Boat moves immediately with progress.
  // The rounded calculation makes the first press visible.
  int boatCenterY =
    72 - ((progress * 32 + 25) / FINISH_DISTANCE);

  bool flipped = (gameStatus == CAPSIZED);

  drawBoat(
    120,
    boatCenterY,
    flipped
  );

  drawGameMessage();

  if (gameOver) {
    drawRestartCountdown();
  }
}

// --------------------------------------------------
// DRAW THE COMPLETE GAME SCREEN
// --------------------------------------------------

void drawScreen() {
  // Full-screen drawing happens only at startup.
  tft.fillScreen(DARK_COLOR);

  // Title bar.
  tft.fillRect(
    0,
    0,
    240,
    18,
    DARK_COLOR
  );

  tft.setTextColor(
    TFT_WHITE,
    DARK_COLOR
  );

  tft.drawString(
    "ROW THE BOAT",
    5,
    2,
    2
  );

  // The water area and bottom progress/drift panel are separate.
  drawDynamicScene();
  drawTimer();
  drawGameInfo();
}

// --------------------------------------------------
// RESET GAME
// --------------------------------------------------

void startNewRound() {
  progress = 0;
  drift = 0;
  lastPlayer = -1;

  gameStarted = false;
  gameOver = false;
  gameStatus = WAITING;

  gameStartTime = 0;
  gameOverAt = 0;
  restartAt = 0;

  lastRestartSecond = -1;
  lastDisplayedSecond = -1;

  needsRedraw = true;
}

// --------------------------------------------------
// END GAME
// --------------------------------------------------

void endGame(uint8_t result) {
  gameStatus = result;
  gameOver = true;
  gameOverAt = millis();

  needsRedraw = true;

  sendState();
}

// --------------------------------------------------
// PROCESS A BUTTON PRESS
// ONLY PLAYER 1 RUNS THIS FUNCTION
// --------------------------------------------------

void processPress(int player) {
  if (PLAYER_ID != 0 || gameOver) return;

  // Start timer with the first press.
  if (!gameStarted) {
    gameStarted = true;
    gameStatus = PLAYING;
    gameStartTime = millis();
  }

  // Same player presses twice.
  if (player == lastPlayer) {
    drift++;

    // Capsize after two drift points.
    if (drift >= MAX_DRIFT) {
      lastPlayer = player;

      endGame(CAPSIZED);
      return;
    }
  }

  // Different player presses.
  else {
    // Correcting drift does not add progress.
    if (drift > 0) {
      drift--;
    } else {
      progress++;
    }
  }

  lastPlayer = player;

  // Check for victory.
  if (progress >= FINISH_DISTANCE) {
    progress = FINISH_DISTANCE;

    endGame(WON);
    return;
  }

  needsRedraw = true;

  sendState();
}

// --------------------------------------------------
// BUTTON DETECTION
// --------------------------------------------------

bool buttonPressed(
  uint8_t pin,
  bool &last
) {
  bool now = digitalRead(pin);

  bool pressed =
    (last == HIGH && now == LOW);

  last = now;

  return pressed;
}

// --------------------------------------------------
// SETUP
// --------------------------------------------------

void setup() {
  Serial.begin(115200);

  pinMode(
    BUTTON_A,
    INPUT_PULLUP
  );

  pinMode(
    BUTTON_B,
    INPUT
  );

  lastA = digitalRead(BUTTON_A);
  lastB = digitalRead(BUTTON_B);

  tft.init();
  tft.setRotation(1);

  // Create a separate water drawing area.
  waterSprite.setColorDepth(16);

  waterSpriteReady =
    (waterSprite.createSprite(240, 77) != nullptr);

  tft.fillScreen(DARK_COLOR);

  setupEspNow();

  drawScreen();
}

// --------------------------------------------------
// MAIN LOOP
// --------------------------------------------------

void loop() {

  // PLAYER 2: Apply the latest shared state.
  applyReceivedState();

  // Read physical buttons.
  bool a = buttonPressed(
    BUTTON_A,
    lastA
  );

  bool b = buttonPressed(
    BUTTON_B,
    lastB
  );

  bool pressed =
    (a || b) &&
    (millis() - lastPressTime > DEBOUNCE_MS);

  if (pressed) {
    lastPressTime = millis();

    if (gameOver) {
      // Ignore button presses during restart countdown.
    }

    else if (PLAYER_ID == 0) {
      // Host processes its own button press.
      processPress(0);
    }

    else {
      // Player 2 sends a press event to the host.
      sendPress();
    }
  }

  // PLAYER 1: Process button presses from Player 2.
  if (PLAYER_ID == 0) {

    while (remotePressCount > 0) {
      remotePressCount--;

      if (!gameOver) {
        processPress(1);
      }
    }

    // Check timer.
    if (gameStarted &&
        !gameOver &&
        millis() - gameStartTime >= GAME_TIME) {

      endGame(TIME_UP);
    }

    // Automatically restart after 3 seconds.
    if (gameOver &&
        millis() - gameOverAt >= RESTART_DELAY) {

      startNewRound();

      sendState();
    }

    // Send updates to Player 2.
    if (millis() - lastHeartbeat >= HEARTBEAT_MS) {
      lastHeartbeat = millis();

      sendState();
    }
  }

  // Update only the changing areas.
  // No full-screen redraw when a button is pressed.
  if (needsRedraw) {
    needsRedraw = false;

    drawDynamicScene();
    drawGameInfo();
  }

  // Update timer once per second.
  uint32_t currentTimeLeft = GAME_TIME;

  if (gameStarted) {
    uint32_t elapsed = millis() - gameStartTime;

    currentTimeLeft = (elapsed >= GAME_TIME)
      ? 0
      : GAME_TIME - elapsed;
  }

  int currentSecond =
    (currentTimeLeft + 999) / 1000;

  if (currentSecond != lastDisplayedSecond) {
    lastDisplayedSecond = currentSecond;

    drawTimer();
  }

  // Update restart countdown without flashing.
  if (gameOver) {

    uint32_t remaining = 0;

    if (PLAYER_ID == 0) {
      uint32_t elapsed = millis() - gameOverAt;

      remaining = (elapsed >= RESTART_DELAY)
        ? 0
        : RESTART_DELAY - elapsed;
    } else {
      int32_t difference =
        (int32_t)(restartAt - millis());

      remaining = (difference > 0)
        ? difference
        : 0;
    }

    int secondsLeft =
      (remaining + 999) / 1000;

    if (secondsLeft != lastRestartSecond) {
      lastRestartSecond = secondsLeft;

      drawRestartCountdown();
    }
  } else {
    lastRestartSecond = -1;
  }

  delay(5);
}