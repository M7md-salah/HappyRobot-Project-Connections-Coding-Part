#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Keypad.h>
#include <Keypad_I2C.h>
#include <WiFi.h>
#include "time.h"
#include <DHT.h>

#include "esp_camera.h"
#include "img_converters.h"
#include "esp_http_server.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// =================================
// DHT11 Temperature/Humidity Sensor
// =================================
// Keep the existing DHT wiring on GPIO 15.
#define DHTPIN 15
#define DHTTYPE DHT11

DHT dht(DHTPIN, DHTTYPE);

// =========================
// WiFi
// =========================

const char* ssid = "Galaxy A55 5G F4F6";
const char* password = "0987654321";

const unsigned long WIFI_TIMEOUT_MS = 15000; // give up after 15s and go offline

// =====================
// Time
// Egypt = UTC+3 currently
// =========================

const char* ntpServer = "pool.ntp.org";

const long gmtOffset_sec = 3 * 3600;
const int daylightOffset_sec = 0;

// =========================
// Shared I2C Pins (OLED + keypad expander)
// =========================
#define I2C_SDA 13
#define I2C_SCL 14

// ---------- Keypad setup (4x4 keypad via PCF8574 I2C expander) ----------
const byte ROWS = 4;
const byte COLS = 4;

char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

// These are PCF8574 port pins (P0-P7), not ESP32 GPIO pins.
byte rowPins[ROWS] = {0, 1, 2, 3};
byte colPins[COLS] = {4, 5, 6, 7};

#define KEYPAD_I2C_ADDRESS 0x20   // PCF8574 default address - adjust if yours differs

// Initialize keypad using the default &Wire instance
Keypad_I2C keypad = Keypad_I2C(makeKeymap(keys), rowPins, colPins, ROWS, COLS, KEYPAD_I2C_ADDRESS, PCF8574, &Wire);

// ---------- Display setup (SH110X OLED, 128x64, I2C) ----------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_I2C_ADDRESS 0x3C
Adafruit_SH1106G oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// =========================
// Onboard flash LED
// =========================
#define FLASH_LED_PIN 4
bool flashState = false;

// =========================
// L298N via 74HC595
// =========================
// The 74HC595 keeps the motor signals off the camera/PSRAM pins.
// Q0-Q3 connect to the L298N IN1-IN4 inputs, respectively.
#define MOTOR_SHIFT_DATA_PIN  2   // 74HC595 pin 14 (SER / DS)
#define MOTOR_SHIFT_CLOCK_PIN 12  // 74HC595 pin 11 (SRCLK / SHCP)
#define MOTOR_SHIFT_LATCH_PIN 1   // 74HC595 pin 12 (RCLK / STCP), labelled U0T on ESP32-CAM

#define MOTOR_BIT_IN1 0
#define MOTOR_BIT_IN2 1
#define MOTOR_BIT_IN3 2
#define MOTOR_BIT_IN4 3

enum MotorDirection : uint8_t { MOTOR_STOP, MOTOR_FORWARD, MOTOR_BACKWARD, MOTOR_LEFT, MOTOR_RIGHT };
volatile MotorDirection motorDirection = MOTOR_STOP;
volatile unsigned long motorLastCommandMs = 0;
const unsigned long MOTOR_COMMAND_TIMEOUT_MS = 700;
SemaphoreHandle_t motorMutex = NULL;

byte motorBitsFor(MotorDirection direction) {
  switch (direction) {
    case MOTOR_FORWARD:
      return bit(MOTOR_BIT_IN1) | bit(MOTOR_BIT_IN3);
    case MOTOR_BACKWARD:
      return bit(MOTOR_BIT_IN2) | bit(MOTOR_BIT_IN4);
    case MOTOR_LEFT:
      return bit(MOTOR_BIT_IN2) | bit(MOTOR_BIT_IN3);
    case MOTOR_RIGHT:
      return bit(MOTOR_BIT_IN1) | bit(MOTOR_BIT_IN4);
    case MOTOR_STOP:
    default:
      return 0;
  }
}

const char *motorDirectionName(MotorDirection direction) {
  switch (direction) {
    case MOTOR_FORWARD: return "forward";
    case MOTOR_BACKWARD: return "backward";
    case MOTOR_LEFT: return "left";
    case MOTOR_RIGHT: return "right";
    case MOTOR_STOP:
    default: return "stopped";
  }
}

void writeMotorShiftRegister(byte value) {
  digitalWrite(MOTOR_SHIFT_LATCH_PIN, LOW);
  shiftOut(MOTOR_SHIFT_DATA_PIN, MOTOR_SHIFT_CLOCK_PIN, MSBFIRST, value);
  digitalWrite(MOTOR_SHIFT_LATCH_PIN, HIGH);
}

void setMotorDirection(MotorDirection direction) {
  if (motorMutex && xSemaphoreTake(motorMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
  writeMotorShiftRegister(motorBitsFor(direction));
  motorDirection = direction;
  motorLastCommandMs = direction == MOTOR_STOP ? 0 : millis();
  if (motorMutex) xSemaphoreGive(motorMutex);
}

// Runs independently of the OLED/keypad menus, which can block while waiting
// for a key. A browser must refresh its motion command to keep the motors moving.
void motorSafetyTask(void *parameter) {
  for (;;) {
    if (motorDirection != MOTOR_STOP && millis() - motorLastCommandMs > MOTOR_COMMAND_TIMEOUT_MS) {
      setMotorDirection(MOTOR_STOP);
    }
    vTaskDelay(pdMS_TO_TICKS(40));
  }
}

// =========================
// Camera pins (AI-Thinker ESP32-CAM)
// =========================
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

bool cameraReady = false;
httpd_handle_t camera_httpd = NULL;
httpd_handle_t stream_httpd = NULL;

// =====================================================
// XO (Tic-Tac-Toe) drawing helpers
// =====================================================

// Shared board geometry, used by the grid, the marks, and the background
// so everything lines up on the same cells. Sized to fit the 128x64 OLED.
const int XO_BOARD_X = 10;
const int XO_BOARD_Y = 12;
const int XO_CELL_W = 36;
const int XO_CELL_H = 17;

// Top-left pixel of a given cell (1-9)
void xoCellOrigin(int cell, int &x, int &y) {
  int col = (cell - 1) % 3;
  int row = (cell - 1) / 3;
  x = XO_BOARD_X + col * XO_CELL_W;
  y = XO_BOARD_Y + row * XO_CELL_H;
}

void drawXOGrid() {
  oled.setTextColor(SH110X_WHITE);
  oled.setTextSize(1);
  oled.setCursor(28, 2);
  oled.print("TIC-TAC-TOE");

  // Grid lines drawn from the same cell geometry as the board layout
  oled.drawFastVLine(XO_BOARD_X + XO_CELL_W, XO_BOARD_Y, XO_CELL_H * 3, SH110X_WHITE);
  oled.drawFastVLine(XO_BOARD_X + XO_CELL_W * 2, XO_BOARD_Y, XO_CELL_H * 3, SH110X_WHITE);
  oled.drawFastHLine(XO_BOARD_X, XO_BOARD_Y + XO_CELL_H, XO_CELL_W * 3, SH110X_WHITE);
  oled.drawFastHLine(XO_BOARD_X, XO_BOARD_Y + XO_CELL_H * 2, XO_CELL_W * 3, SH110X_WHITE);
}

void drawX(int cell) {
  int x, y;
  xoCellOrigin(cell, x, y);

  oled.drawLine(x + 4, y + 3, x + XO_CELL_W - 4, y + XO_CELL_H - 3, SH110X_WHITE);
  oled.drawLine(x + XO_CELL_W - 4, y + 3, x + 4, y + XO_CELL_H - 3, SH110X_WHITE);
}

void drawO(int cell) {
  int x, y;
  xoCellOrigin(cell, x, y);

  int cx = x + XO_CELL_W / 2;
  int cy = y + XO_CELL_H / 2;
  oled.drawCircle(cx, cy, 6, SH110X_WHITE);
}

// =====================================================
// XO game logic
// =====================================================

int checkWinner(int arrayX[], int arrayO[])
{
    int wins[8][3] = {
        {1, 2, 3},
        {4, 5, 6},
        {7, 8, 9},
        {1, 4, 7},
        {2, 5, 8},
        {3, 6, 9},
        {1, 5, 9},
        {3, 5, 7}
    };

    for (int i = 0; i < 8; i++)
    {
        if (arrayX[wins[i][0]] == wins[i][0] &&
            arrayX[wins[i][1]] == wins[i][1] &&
            arrayX[wins[i][2]] == wins[i][2])
        {
            return 1; // X wins
        }

        if (arrayO[wins[i][0]] == wins[i][0] &&
            arrayO[wins[i][1]] == wins[i][1] &&
            arrayO[wins[i][2]] == wins[i][2])
        {
            return 2; // O wins
        }
    }
    // Check tie
    bool full = true;

    for (int i = 1; i <= 9; i++)
    {
        if (arrayX[i] == 0 && arrayO[i] == 0)
        {
            full = false;
            break;
        }
    }

    if (full)
        return 3;


    return 0; // Nobody won
}

bool taken(int cell, int arrayX[], int arrayO[])
{
    return arrayX[cell] != 0 || arrayO[cell] != 0;
}

// =====================================================
// Minigames (Number Guess + XO + Reaction Timer), grouped under one submenu
// =====================================================

void playNumberGuess() {
  int randomNum = random(10); // 0-9 (matches the single keypress we read below)
  oled.clearDisplay();

  oled.drawRect(0, 0, 128, 64, SH110X_WHITE);

  // Small bullseye motif, top-right
  oled.drawCircle(105, 16, 14, SH110X_WHITE);
  oled.drawCircle(105, 16, 9, SH110X_WHITE);
  oled.fillCircle(105, 16, 3, SH110X_WHITE);

  oled.setTextColor(SH110X_WHITE);
  oled.setTextSize(1);
  oled.setCursor(4, 4);
  oled.print("Guess a number");
  oled.setCursor(4, 14);
  oled.print("from 0-9");
  oled.display();

  char entered = keypad.waitForKey();
  oled.clearDisplay();
  oled.setCursor(20, 28);
  if ((entered - '0') == randomNum)
  {
    oled.print("Perfect Guess!!");
  }
  else
  {
    oled.print("Wrong Guess!!");
  }
  oled.display();
  delay(2000);
}

// Decorative background for the XO game: just a frame (a fill tint would
// hide the marks on a monochrome screen, so it's left plain).
void drawXOBackground() {
  oled.drawRect(0, 0, 128, 64, SH110X_WHITE);
}

void playXO() {
  int winner = 0;
  oled.clearDisplay();
  drawXOBackground();
  oled.display();
  int arrayX[10] = {};
  int arrayO[10] = {};

  while (!checkWinner(arrayX, arrayO))
  {
    oled.clearDisplay();
    drawXOBackground();
    drawXOGrid();
    for (int c = 1; c <= 9; c++) {
      if (arrayX[c] == c) drawX(c);
      if (arrayO[c] == c) drawO(c);
    }
    oled.display();

    int pc_num = random(1, 10);
    while (taken(pc_num, arrayX, arrayO))
    {
      pc_num = random(1, 10);
    }
    arrayX[pc_num] = pc_num;
    winner = checkWinner(arrayX, arrayO);
    if (winner != 0)
      break;

    char your_turn = keypad.waitForKey();
    while (your_turn - '0' < 1 || your_turn - '0' > 9 || taken(your_turn - '0', arrayX, arrayO))
    {
      your_turn = keypad.waitForKey();
    }
    arrayO[your_turn - '0'] = (your_turn - '0');
    winner = checkWinner(arrayX, arrayO);
  }

  oled.clearDisplay();
  drawXOBackground();
  drawXOGrid();
  for (int c = 1; c <= 9; c++) {
    if (arrayX[c] == c) drawX(c);
    if (arrayO[c] == c) drawO(c);
  }
  oled.display();
  delay(600);

  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setCursor(40, 28);
  if (winner == 1)
  {
    oled.print("X Wins!");
  }
  else if (winner == 2)
  {
    oled.print("O Wins!");
  }
  else if (winner == 3)
  {
    oled.print("Tied!");
  }
  oled.display();
  delay(2000);
}

void playReact()
{
  int randTime = random(5000);
  int randX = random(0, 4); // valid keys[][] indices are 0-3
  int randY = random(0, 4);
  oled.clearDisplay();
  oled.setCursor(0, 0);
  oled.print("Ready...");
  oled.display();
  delay(randTime);
  oled.clearDisplay();
  long startTime = millis();
  oled.setCursor(0, 0);
  oled.print("Quickly, Press ");
  oled.print(keys[randX][randY]);
  oled.print("!!");
  oled.display();
  char pressedKey = keypad.waitForKey();
  int reactionTime = millis() - startTime;

  oled.clearDisplay();
  oled.setCursor(0, 0);
  if(pressedKey == keys[randX][randY] && reactionTime < 2000)
  {
    oled.print("Nice Win!!");
    oled.setCursor(0, 16);
    oled.print("Time you took:");
    oled.setCursor(0, 26);
    oled.print(reactionTime);
    oled.print(" ms");
  }
  else if(reactionTime > 2000)
  {
    oled.print("Slow, Try Again!");
    oled.setCursor(0, 16);
    oled.print("Time you took:");
    oled.setCursor(0, 26);
    oled.print(reactionTime);
    oled.print(" ms");
  }
  else if(pressedKey != keys[randX][randY])
  {
    oled.print("Wrong button!");
    oled.setCursor(0, 16);
    oled.print("Time you took:");
    oled.setCursor(0, 26);
    oled.print(reactionTime);
    oled.print(" ms");
  }
  oled.display();
  delay(2000);
}

// Minigames submenu: pick Number Guess, XO, or React — loops until '*' (back)
void showMinigamesMenu() {
  bool inMenu = true;
  while (inMenu) {
    oled.clearDisplay();
    oled.drawRect(0, 0, 128, 64, SH110X_WHITE);

    // Small die icon with a random face, redrawn each time the menu opens
    int dieX = 96, dieY = 6, dieSize = 20;
    oled.drawRect(dieX, dieY, dieSize, dieSize, SH110X_WHITE);

    int cx = dieX + dieSize / 2;
    int cy = dieY + dieSize / 2;
    int gap = 6;
    int face = random(1, 7); // 1-6

    if (face == 1 || face == 3 || face == 5) oled.fillCircle(cx, cy, 1, SH110X_WHITE);
    if (face >= 2) {
      oled.fillCircle(cx - gap, cy - gap, 1, SH110X_WHITE);
      oled.fillCircle(cx + gap, cy + gap, 1, SH110X_WHITE);
    }
    if (face >= 4) {
      oled.fillCircle(cx + gap, cy - gap, 1, SH110X_WHITE);
      oled.fillCircle(cx - gap, cy + gap, 1, SH110X_WHITE);
    }
    if (face == 6) {
      oled.fillCircle(cx - gap, cy, 1, SH110X_WHITE);
      oled.fillCircle(cx + gap, cy, 1, SH110X_WHITE);
    }
    oled.setTextColor(SH110X_WHITE);
    oled.setTextSize(1);
    oled.setCursor(4, 8);
    oled.print("1-Number Guess");
    oled.setCursor(4, 20);
    oled.print("2-XO");
    oled.setCursor(4, 32);
    oled.print("3-React Fast");
    oled.setCursor(4, 48);
    oled.print("*-Back");
    oled.display();

    char game = keypad.waitForKey();
    switch (game) {
      case '1': playNumberGuess(); break;
      case '2': playXO(); break;
      case '3': playReact(); break;
      case '*': inMenu = false; break;
      default: break; // ignore unrecognized key, just redraw
    }
  }
}

// =====================================================
// Live camera feed (real ESP32-CAM MJPEG stream + web GUI)
// =====================================================

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// Styled single-page dashboard: live video, snapshot button, flash toggle,
// and a small telemetry panel that polls /status every couple of seconds.
static const char INDEX_HTML[] PROGMEM = R"HTMLPAGE(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Happy Robot</title>
<style>
  :root{--bg:#0f1115;--card:#171a21;--accent:#4fd1c5;--accent2:#f6ad55;--text:#e8eaf0;--muted:#8a91a3;}
  *{box-sizing:border-box;}
  body{margin:0;font-family:'Segoe UI',Roboto,Arial,sans-serif;background:radial-gradient(circle at top,#1b1f29,#0b0d11);color:var(--text);min-height:100vh;padding:24px;}
  .wrap{max-width:860px;margin:0 auto;}
  h1{text-align:center;font-size:1.6rem;letter-spacing:1px;margin-bottom:4px;}
  h1 span{color:var(--accent);}
  .sub{text-align:center;color:var(--muted);margin-bottom:24px;font-size:.85rem;}
  .card{background:var(--card);border-radius:16px;padding:16px;box-shadow:0 10px 30px rgba(0,0,0,.4);margin-bottom:18px;}
  .video-wrap{position:relative;border-radius:12px;overflow:hidden;border:2px solid #262b36;background:#000;aspect-ratio:4/3;display:flex;align-items:center;justify-content:center;}
  .video-wrap img{width:100%;height:100%;object-fit:cover;display:block;}
  .badge{position:absolute;top:10px;left:10px;background:rgba(0,0,0,.55);padding:4px 10px;border-radius:20px;font-size:.75rem;display:flex;align-items:center;gap:6px;}
  .dot{width:8px;height:8px;border-radius:50%;background:#ff5c5c;animation:pulse 1.4s infinite;}
  @keyframes pulse{0%{opacity:1;}50%{opacity:.3;}100%{opacity:1;}}
  .stats{display:grid;grid-template-columns:repeat(3,1fr);gap:12px;}
  .stat{background:#1d212b;border-radius:12px;padding:14px;text-align:center;}
  .stat .val{font-size:1.3rem;font-weight:600;color:var(--accent);}
  .stat .lbl{font-size:.7rem;color:var(--muted);text-transform:uppercase;letter-spacing:.5px;margin-top:2px;}
  .controls{display:flex;gap:12px;margin-top:16px;flex-wrap:wrap;}
  button{flex:1;min-width:140px;padding:12px 16px;border:none;border-radius:10px;background:var(--accent);color:#0b0d11;font-weight:600;font-size:.9rem;cursor:pointer;transition:transform .15s;}
  button:active{transform:scale(.96);}
  button.secondary{background:var(--accent2);}
  button.off{background:#3a3f4b;color:var(--text);}
  footer{text-align:center;color:var(--muted);font-size:.75rem;margin-top:20px;}
  .drive-card{display:grid;grid-template-columns:1fr 1.15fr;gap:22px;align-items:center;}
  .drive-copy h2{font-size:1.05rem;margin:0 0 6px;letter-spacing:.4px;}
  .drive-copy p{color:var(--muted);font-size:.84rem;line-height:1.5;margin:0 0 14px;}
  .drive-state{display:inline-flex;align-items:center;gap:7px;background:#1d212b;border:1px solid #303746;border-radius:999px;padding:7px 11px;font-size:.75rem;color:var(--muted);}
  .drive-state i{display:block;width:8px;height:8px;border-radius:50%;background:#64748b;transition:.2s;}
  .drive-state.moving i{background:var(--accent);box-shadow:0 0 12px var(--accent);}
  .joystick-shell{width:min(250px,74vw);aspect-ratio:1;margin:auto;border-radius:50%;padding:16px;background:radial-gradient(circle at 36% 30%,#323949,#171b24 65%);box-shadow:inset 0 2px 10px rgba(255,255,255,.07),0 12px 25px rgba(0,0,0,.35);touch-action:none;user-select:none;}
  .joystick{width:100%;height:100%;position:relative;border:1px solid #454e60;border-radius:50%;background:radial-gradient(circle,#262c38 0 26%,transparent 27%),linear-gradient(#ffffff10 1px,transparent 1px),linear-gradient(90deg,#ffffff10 1px,transparent 1px);background-size:auto,25% 25%,25% 25%;}
  .joy-label{position:absolute;left:50%;top:12px;transform:translateX(-50%);color:#8a91a3;font-size:.62rem;letter-spacing:1.7px;font-weight:700;}
  .joy-label.bottom{top:auto;bottom:12px;}
  .joy-label.left{left:12px;top:50%;transform:translateY(-50%) rotate(-90deg);}
  .joy-label.right{left:auto;right:12px;top:50%;transform:translateY(-50%) rotate(90deg);}
  .joy-knob{position:absolute;left:50%;top:50%;width:76px;height:76px;transform:translate(-50%,-50%);border-radius:50%;background:linear-gradient(145deg,#63e6d8,#34a89e);border:5px solid #b4fff7;box-shadow:0 7px 16px #0008,inset 0 1px 3px #fff9;transition:transform .08s ease-out;}
  .hint{text-align:center;color:var(--muted);font-size:.72rem;margin:11px 0 0;}
  @media(max-width:620px){body{padding:14px}.drive-card{grid-template-columns:1fr}.drive-copy{text-align:center}.stats{gap:7px}.stat{padding:11px 5px}.stat .val{font-size:1.08rem;}.controls{gap:8px}}
</style>
</head>
<body>
<div class="wrap">
  <h1>&#129302; <span>Happy Robot</span> Control Panel</h1>
  <div class="sub">Live camera + sensor dashboard</div>

  <div class="card">
    <div class="video-wrap">
      <span class="badge"><span class="dot"></span> LIVE</span>
      <img id="stream" src="" alt="camera stream">
    </div>
    <div class="controls">
      <button id="snapBtn">Snapshot</button>
      <button id="flashBtn" class="secondary">Flash On</button>
    </div>
  </div>

  <div class="card drive-card">
    <div class="drive-copy">
      <h2>Drive control</h2>
      <p>Press and hold the joystick to move. Releasing it, switching tabs, or losing the connection stops the motors.</p>
      <div id="driveState" class="drive-state"><i></i><span>Motors stopped</span></div>
    </div>
    <div>
      <div id="joystickShell" class="joystick-shell" aria-label="Robot direction joystick">
        <div class="joystick">
          <span class="joy-label">FORWARD</span><span class="joy-label bottom">REVERSE</span>
          <span class="joy-label left">LEFT</span><span class="joy-label right">RIGHT</span>
          <div id="joyKnob" class="joy-knob"></div>
        </div>
      </div>
      <p class="hint">Keyboard: arrow keys or W A S D &middot; Space stops</p>
    </div>
  </div>

  <div class="card">
    <div class="stats">
      <div class="stat"><div class="val" id="temp">--&deg;C</div><div class="lbl">Temperature</div></div>
      <div class="stat"><div class="val" id="hum">--%</div><div class="lbl">Humidity</div></div>
      <div class="stat"><div class="val" id="time">--:--:--</div><div class="lbl">Device Time</div></div>
    </div>
  </div>

  <footer>Happy Robot &middot; ESP32-CAM</footer>
</div>

<script>
  var host = window.location.hostname;
  document.getElementById('stream').src = 'http://' + host + ':81/stream';

  var flashOn = false;
  document.getElementById('flashBtn').onclick = function () {
    flashOn = !flashOn;
    fetch('/flash?state=' + (flashOn ? 'on' : 'off')).then(function () {
      var b = document.getElementById('flashBtn');
      b.textContent = flashOn ? 'Flash Off' : 'Flash On';
      b.className = flashOn ? 'off' : 'secondary';
    });
  };

  document.getElementById('snapBtn').onclick = function () {
    window.open('/capture', '_blank');
  };

  var joystickShell = document.getElementById('joystickShell');
  var joyKnob = document.getElementById('joyKnob');
  var driveState = document.getElementById('driveState');
  var held = false, activeDirection = 'stop', driveHeartbeat = null;
  var keyDirections = {ArrowUp:'forward',w:'forward',W:'forward',ArrowDown:'backward',s:'backward',S:'backward',ArrowLeft:'left',a:'left',A:'left',ArrowRight:'right',d:'right',D:'right'};

  function showDriveState(direction) {
    var moving = direction !== 'stop';
    driveState.className = moving ? 'drive-state moving' : 'drive-state';
    driveState.querySelector('span').textContent = moving ? 'Moving ' + direction : 'Motors stopped';
  }
  function sendMotor(direction) {
    activeDirection = direction;
    showDriveState(direction);
    fetch('/motor?dir=' + direction, {cache:'no-store'}).catch(function () {});
  }
  function resetKnob() { joyKnob.style.transform = 'translate(-50%,-50%)'; }
  function directionFromPointer(e) {
    var r = joystickShell.getBoundingClientRect(), cx = r.left + r.width / 2, cy = r.top + r.height / 2;
    var dx = e.clientX - cx, dy = e.clientY - cy, max = r.width * .27;
    var distance = Math.sqrt(dx * dx + dy * dy);
    if (distance > max) { dx *= max / distance; dy *= max / distance; distance = max; }
    joyKnob.style.transform = 'translate(calc(-50% + ' + dx + 'px),calc(-50% + ' + dy + 'px))';
    if (distance < r.width * .10) return 'stop';
    return Math.abs(dx) > Math.abs(dy) ? (dx > 0 ? 'right' : 'left') : (dy > 0 ? 'backward' : 'forward');
  }
  function beginDrive(direction) {
    held = true;
    if (direction !== activeDirection) sendMotor(direction);
    clearInterval(driveHeartbeat);
    driveHeartbeat = setInterval(function () { if (held && activeDirection !== 'stop') sendMotor(activeDirection); }, 180);
  }
  function stopDrive() {
    held = false;
    clearInterval(driveHeartbeat);
    driveHeartbeat = null;
    resetKnob();
    if (activeDirection !== 'stop') sendMotor('stop');
  }
  joystickShell.addEventListener('pointerdown', function (e) {
    e.preventDefault(); joystickShell.setPointerCapture(e.pointerId); beginDrive(directionFromPointer(e));
  });
  joystickShell.addEventListener('pointermove', function (e) {
    if (held) { var direction = directionFromPointer(e); if (direction !== activeDirection) sendMotor(direction); }
  });
  joystickShell.addEventListener('pointerup', stopDrive);
  joystickShell.addEventListener('pointercancel', stopDrive);
  joystickShell.addEventListener('lostpointercapture', stopDrive);
  window.addEventListener('blur', stopDrive);
  document.addEventListener('visibilitychange', function () { if (document.hidden) stopDrive(); });
  window.addEventListener('keydown', function (e) {
    if (e.code === 'Space') { e.preventDefault(); stopDrive(); return; }
    if (keyDirections[e.key] && !e.repeat) { e.preventDefault(); beginDrive(keyDirections[e.key]); }
  });
  window.addEventListener('keyup', function (e) { if (keyDirections[e.key]) stopDrive(); });
  window.addEventListener('pagehide', function () { fetch('/motor?dir=stop', {keepalive:true}); });

  function refreshStatus() {
    fetch('/status').then(function (r) { return r.json(); }).then(function (d) {
      document.getElementById('temp').textContent = d.temp.toFixed(1) + '\u00B0C';
      document.getElementById('hum').textContent = d.humidity.toFixed(1) + '%';
      document.getElementById('time').textContent = d.time;
      if (!held && d.motor) showDriveState(d.motor === 'stopped' ? 'stop' : d.motor);
    }).catch(function () {});
  }
  setInterval(refreshStatus, 2000);
  refreshStatus();
</script>
</body>
</html>
)HTMLPAGE";

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, (const char *)INDEX_HTML, strlen(INDEX_HTML));
}

static esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

static esp_err_t status_handler(httpd_req_t *req) {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  char timeStr[16] = "--:--:--";
  struct tm timeinfo;
  if (WiFi.status() == WL_CONNECTED && getLocalTime(&timeinfo, 100)) {
    strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
  }
  char buf[200];
  snprintf(buf, sizeof(buf),
    "{\"temp\":%.1f,\"humidity\":%.1f,\"time\":\"%s\",\"flash\":%s,\"motor\":\"%s\"}",
    isnan(t) ? 0.0 : t, isnan(h) ? 0.0 : h, timeStr, flashState ? "true" : "false", motorDirectionName(motorDirection));
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, buf, strlen(buf));
}

static esp_err_t flash_handler(httpd_req_t *req) {
  char query[32];
  bool turnOn = false;
  if (httpd_req_get_url_query_len(req) > 0 &&
      httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    if (strstr(query, "state=on") != NULL) turnOn = true;
  }
  flashState = turnOn;
  digitalWrite(FLASH_LED_PIN, flashState ? HIGH : LOW);
  httpd_resp_set_type(req, "application/json");
  const char *resp = flashState ? "{\"flash\":true}" : "{\"flash\":false}";
  return httpd_resp_send(req, resp, strlen(resp));
}

// /motor?dir=forward|backward|left|right|stop
static esp_err_t motor_handler(httpd_req_t *req) {
  char query[64] = {};
  char directionText[16] = {};
  MotorDirection direction = MOTOR_STOP; // Unknown or absent commands always stop.

  if (httpd_req_get_url_query_len(req) > 0 &&
      httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
      httpd_query_key_value(query, "dir", directionText, sizeof(directionText)) == ESP_OK) {
    if (strcmp(directionText, "forward") == 0) direction = MOTOR_FORWARD;
    else if (strcmp(directionText, "backward") == 0) direction = MOTOR_BACKWARD;
    else if (strcmp(directionText, "left") == 0) direction = MOTOR_LEFT;
    else if (strcmp(directionText, "right") == 0) direction = MOTOR_RIGHT;
  }

  setMotorDirection(direction);
  char response[48];
  snprintf(response, sizeof(response), "{\"motor\":\"%s\"}", motorDirectionName(direction));
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, response, strlen(response));
}

static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
  size_t jpgLen = 0;
  uint8_t *jpgBuf = NULL;
  char partBuf[64];

  res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      res = ESP_FAIL;
    } else {
      if (fb->format != PIXFORMAT_JPEG) {
        bool converted = frame2jpg(fb, 80, &jpgBuf, &jpgLen);
        esp_camera_fb_return(fb);
        fb = NULL;
        if (!converted) {
          Serial.println("JPEG compression failed");
          res = ESP_FAIL;
        }
      } else {
        jpgLen = fb->len;
        jpgBuf = fb->buf;
      }
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    }
    if (res == ESP_OK) {
      size_t hlen = snprintf(partBuf, sizeof(partBuf), STREAM_PART, (unsigned)jpgLen);
      res = httpd_resp_send_chunk(req, partBuf, hlen);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char *)jpgBuf, jpgLen);
    }
    if (fb) {
      esp_camera_fb_return(fb);
      fb = NULL;
      jpgBuf = NULL;
    } else if (jpgBuf) {
      free(jpgBuf);
      jpgBuf = NULL;
    }
    if (res != ESP_OK) {
      break;
    }
  }
  return res;
}

void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;

  httpd_uri_t index_uri   = { "/",        HTTP_GET, index_handler,   NULL };
  httpd_uri_t capture_uri = { "/capture", HTTP_GET, capture_handler, NULL };
  httpd_uri_t status_uri  = { "/status",  HTTP_GET, status_handler,  NULL };
  httpd_uri_t flash_uri   = { "/flash",   HTTP_GET, flash_handler,   NULL };
  httpd_uri_t motor_uri   = { "/motor",   HTTP_GET, motor_handler,   NULL };

  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &status_uri);
    httpd_register_uri_handler(camera_httpd, &flash_uri);
    httpd_register_uri_handler(camera_httpd, &motor_uri);
  } else {
    Serial.println("Failed to start main web server");
  }

  config.server_port = 81;
  config.ctrl_port = 32769;
  httpd_uri_t stream_uri = { "/stream", HTTP_GET, stream_handler, NULL };
  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
  } else {
    Serial.println("Failed to start stream server");
  }
}

bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  // NOTE: some newer esp32-camera library versions rename these two fields
  // to pin_sccb_sda / pin_sccb_scl. If this line fails to compile, rename it.
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  if (psramFound()) {
    config.frame_size = FRAMESIZE_SVGA;   // 800x600
    config.jpeg_quality = 12;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
  } else {
    config.frame_size = FRAMESIZE_QVGA;   // 320x240
    config.jpeg_quality = 15;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s != NULL && s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  }

  return true;
}

// OLED screen shown from the main menu: real IP/status instead of a
// "Not Available" placeholder. The stream itself runs continuously in
// the background regardless of which OLED screen is showing.
void playCameraFeed() {
  oled.clearDisplay();
  oled.drawRect(0, 0, 128, 64, SH110X_WHITE);
  oled.setTextColor(SH110X_WHITE);
  oled.setTextSize(1);
  oled.setCursor(4, 4);
  oled.print("LIVE CAMERA FEED");
  oled.drawLine(4, 13, 124, 13, SH110X_WHITE);

  if (!cameraReady) {
    oled.setCursor(4, 24);
    oled.print("Camera not");
    oled.setCursor(4, 34);
    oled.print("initialized.");
    oled.setCursor(4, 50);
    oled.print("Press * for menu");
    oled.display();
    char k;
    do { k = keypad.waitForKey(); } while (k != '*');
    return;
  }

  oled.setCursor(4, 18);
  oled.print("Open in browser:");
  oled.setCursor(4, 30);
  if (WiFi.status() == WL_CONNECTED) {
    oled.print(WiFi.localIP());
  } else {
    oled.print("WiFi offline");
  }
  oled.setCursor(4, 44);
  oled.print("Stream is live.");
  oled.setCursor(4, 54);
  oled.print("Press * to go back");
  oled.display();

  char k;
  do { k = keypad.waitForKey(); } while (k != '*');
}

// =====================================================
// Funny faces
// =====================================================

void showFaces()
{
  oled.clearDisplay();
  oled.drawRect(0, 0, 128, 64, SH110X_WHITE);

  int face = random(1, 6); // 1-5

  int cx = 64, cy = 26, eyeY = cy - 8, eyeOffset = 12;

  // Head outline, shared by every expression
  oled.drawCircle(cx, cy, 22, SH110X_WHITE);

  oled.setTextColor(SH110X_WHITE);
  oled.setTextSize(1);

  switch (face)
  {
    case 1: // Happy
      oled.fillCircle(cx - eyeOffset, eyeY, 2, SH110X_WHITE);
      oled.fillCircle(cx + eyeOffset, eyeY, 2, SH110X_WHITE);
      oled.drawLine(cx - 10, cy + 8, cx - 5, cy + 13, SH110X_WHITE);
      oled.drawLine(cx - 5, cy + 13, cx + 5, cy + 13, SH110X_WHITE);
      oled.drawLine(cx + 5, cy + 13, cx + 10, cy + 8, SH110X_WHITE);
      oled.setCursor(48, 54);
      oled.print("Happy!");
      break;

    case 2: // Sad
      oled.fillCircle(cx - eyeOffset, eyeY, 2, SH110X_WHITE);
      oled.fillCircle(cx + eyeOffset, eyeY, 2, SH110X_WHITE);
      oled.drawLine(cx - 10, cy + 13, cx - 5, cy + 8, SH110X_WHITE);
      oled.drawLine(cx - 5, cy + 8, cx + 5, cy + 8, SH110X_WHITE);
      oled.drawLine(cx + 5, cy + 8, cx + 10, cy + 13, SH110X_WHITE);
      oled.setCursor(54, 54);
      oled.print("Sad");
      break;

    case 3: // Surprised
      oled.drawCircle(cx - eyeOffset, eyeY, 3, SH110X_WHITE);
      oled.drawCircle(cx + eyeOffset, eyeY, 3, SH110X_WHITE);
      oled.drawCircle(cx, cy + 11, 4, SH110X_WHITE);
      oled.setCursor(32, 54);
      oled.print("Surprised!");
      break;

    case 4: // Wink
      oled.drawLine(cx - eyeOffset - 2, eyeY, cx - eyeOffset + 2, eyeY, SH110X_WHITE);
      oled.fillCircle(cx + eyeOffset, eyeY, 2, SH110X_WHITE);
      oled.drawLine(cx - 8, cy + 11, cx + 9, cy + 8, SH110X_WHITE);
      oled.setCursor(52, 54);
      oled.print("Wink!");
      break;

    case 5: // Angry
      oled.drawLine(cx - 16, eyeY - 5, cx - 6, eyeY - 2, SH110X_WHITE);
      oled.drawLine(cx + 16, eyeY - 5, cx + 6, eyeY - 2, SH110X_WHITE);
      oled.fillCircle(cx - eyeOffset, eyeY, 2, SH110X_WHITE);
      oled.fillCircle(cx + eyeOffset, eyeY, 2, SH110X_WHITE);
      oled.drawLine(cx - 9, cy + 11, cx + 9, cy + 11, SH110X_WHITE);
      oled.setCursor(50, 54);
      oled.print("Angry!");
      break;
  }

  oled.display();
  keypad.waitForKey(); // hold the expression on screen until a key is pressed
}

// =====================================================
// Main menu (loops; '*' returns to the lock screen)
// =====================================================

void showMainMenu() {
  bool inMenu = true;
  while (inMenu) {
    oled.clearDisplay();
    int w = oled.width();
    int h = oled.height();

    oled.drawRect(0, 0, w, h, SH110X_WHITE);
    oled.setTextColor(SH110X_WHITE);
    oled.setTextSize(1);
    oled.setCursor(32, 2);
    oled.print("MAIN MENU");
    oled.drawLine(4, 11, w - 4, 11, SH110X_WHITE);

    oled.setCursor(4, 16);
    oled.print("1-Minigames");
    oled.setCursor(4, 28);
    oled.print("2-Camera Feed");
    oled.setCursor(4, 40);
    oled.print("3-Funny Faces");
    oled.setCursor(4, 52);
    oled.print("*-Lock Screen");
    oled.display();

    char choice = keypad.waitForKey();
    switch (choice) {
      case '1': showMinigamesMenu(); break;
      case '2': playCameraFeed(); break;
      case '3': showFaces(); break;
      case '*': inMenu = false; break;
      default: break; // ignore unrecognized key, just redraw
    }
  }
}

// =====================================================
// Lock screen (clock + temp/humidity, waits for '#')
// =====================================================

void showLockScreen()
{
  oled.clearDisplay();
  int w = oled.width();
  int h = oled.height();

  oled.drawRect(0, 0, w, h, SH110X_WHITE);
  oled.setTextColor(SH110X_WHITE);
  oled.setTextSize(1);
  oled.setCursor(w / 2 - 30, 2);
  oled.print("LOCK SCREEN");
  oled.drawLine(4, 11, w - 4, 11, SH110X_WHITE);

  char pressedKey = 0;
  while (pressedKey != '#')
  {
    oled.fillRect(1, 12, w - 2, h - 13, SH110X_BLACK);

    struct tm timeinfo;
    if (WiFi.status() == WL_CONNECTED && getLocalTime(&timeinfo, 100))
    {
      char timeString[12];
      strftime(timeString, sizeof(timeString), "%I:%M:%S %p", &timeinfo);
      oled.setCursor(20, 16);
      oled.print(timeString);
    }
    else
    {
      oled.setCursor(14, 16);
      oled.print("--:--:-- --");
    }

    float humid = dht.readHumidity();
    float temp = dht.readTemperature();

    oled.setCursor(4, 30);
    oled.print("Temp: ");
    if (isnan(temp)) {
      oled.print("sensor err");
    } else {
      oled.print(temp, 1);
      oled.print((char)247);
      oled.print("C");
    }

    oled.setCursor(4, 42);
    oled.print("Humidity: ");
    if (isnan(humid)) {
      oled.print("err");
    } else {
      oled.print(humid, 1);
      oled.print("%");
    }

    oled.setCursor(4, 54);
    oled.print("Press # to proceed");

    oled.display();

    pressedKey = keypad.getKey();
    delay(1000);
  }

  showMainMenu();
}

// =====================================================
// WiFi connect helper (bounded, no more infinite blocking)
// =====================================================

bool connectWiFi() {
  oled.clearDisplay();
  oled.setTextColor(SH110X_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print("Connecting to WiFi");
  oled.display();

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  unsigned long startAttempt = millis();
  int dots = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < WIFI_TIMEOUT_MS)
  {
    delay(400);
    oled.setCursor((dots % 20) * 6, 12);
    oled.print(".");
    dots++;
    Serial.print(".");
  }
  Serial.println();

  oled.clearDisplay();
  oled.setCursor(0, 0);
  if (WiFi.status() == WL_CONNECTED)
  {
    oled.print("WiFi connected!");
    oled.setCursor(0, 12);
    oled.print(WiFi.localIP());
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  }
  else
  {
    oled.print("WiFi failed.");
    oled.setCursor(0, 12);
    oled.print("Running offline.");
    Serial.println("WiFi connect timed out - continuing offline.");
  }
  oled.display();
  delay(1500);
  return WiFi.status() == WL_CONNECTED;
}

void showWelcomeScreen() {
  oled.clearDisplay();

  // Simple robot face (mini)
  oled.drawCircle(64, 16, 12, SH110X_WHITE);
  oled.fillCircle(59, 14, 2, SH110X_WHITE);
  oled.fillCircle(69, 14, 2, SH110X_WHITE);
  oled.drawLine(59, 20, 69, 20, SH110X_WHITE);
  oled.drawLine(64, 4, 64, 0, SH110X_WHITE);
  oled.fillCircle(64, 0, 1, SH110X_WHITE);

  oled.setTextColor(SH110X_WHITE);
  oled.setTextSize(1);
  oled.setCursor(40, 32);
  oled.print("WELCOME");
  oled.setCursor(30, 42);
  oled.print("HAPPY ROBOT");

  if (cameraReady && WiFi.status() == WL_CONNECTED) {
    oled.setCursor(2, 54);
    oled.print(WiFi.localIP());
  }
  oled.display();
  delay(2000);
}

// =====================================================
// Setup / Loop
// =====================================================

void setup()
{
  Serial.begin(115200);
  delay(200);

  // Disable brownout detector: the camera can cause brief voltage dips
  // during init/capture that would otherwise reset an ESP32-CAM board.
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  // Put a known STOP pattern in the shift register before WiFi/camera setup.
  // GPIO 2 and GPIO 12 are boot-strapping pins: the external 10k pulldowns
  // described in the wiring notes keep them LOW while the ESP32 is resetting.
  pinMode(MOTOR_SHIFT_DATA_PIN, OUTPUT);
  pinMode(MOTOR_SHIFT_CLOCK_PIN, OUTPUT);
  pinMode(MOTOR_SHIFT_LATCH_PIN, OUTPUT);
  digitalWrite(MOTOR_SHIFT_DATA_PIN, LOW);
  digitalWrite(MOTOR_SHIFT_CLOCK_PIN, LOW);
  digitalWrite(MOTOR_SHIFT_LATCH_PIN, LOW);
  motorMutex = xSemaphoreCreateMutex();
  setMotorDirection(MOTOR_STOP);
  xTaskCreate(motorSafetyTask, "motorSafety", 2048, NULL, 1, NULL);

  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);

  // 1. Initialize the shared I2C bus on GPIO 13 and 14
  Wire.begin(I2C_SDA, I2C_SCL);

  // 2. Begin OLED (now using the re-mapped shared bus)
  if (!oled.begin(OLED_I2C_ADDRESS, true)) {
    Serial.println("SH110X allocation failed");
    for (;;);
  }
  oled.clearDisplay();
  oled.setTextColor(SH110X_WHITE);
  oled.setTextSize(1);
  oled.display();

  dht.begin();

  // 3. Begin Keypad (also using the shared &Wire bus configured above)
  keypad.begin();

  // 4. WiFi + time + camera are set up once here, not on every menu visit
  bool wifiOk = connectWiFi();
  if (wifiOk) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  }

  cameraReady = initCamera();
  if (cameraReady) {
    startCameraServer();
    Serial.println("Camera server started.");
  } else {
    Serial.println("Camera unavailable - Camera Feed menu will say so.");
  }

  showWelcomeScreen();
}

void loop()
{
  char start = keypad.waitForKey();

  if (start == '#')
  {
      showLockScreen();
  }
}
