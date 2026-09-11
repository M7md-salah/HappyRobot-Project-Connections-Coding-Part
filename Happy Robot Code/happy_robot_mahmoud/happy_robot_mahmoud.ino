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

// =================================
// DHT11 Temperature/Humidity Sensor
// =================================
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

// Shared board geometry, used by the grid and marks so everything stays
// aligned. The compact board leaves a deliberate status bar at the top.
const int XO_BOARD_X = 19;
const int XO_BOARD_Y = 15;
const int XO_CELL_W = 30;
const int XO_CELL_H = 15;

// Small OLED design system. Keeping these primitives in one place makes the
// monochrome UI feel intentional without changing any device behaviour.
void drawScreenFrame() {
  oled.drawRoundRect(0, 0, 128, 64, 3, SH110X_WHITE);
}

void drawHeader(const char *title, const char *tag = NULL) {
  oled.setTextColor(SH110X_WHITE);
  oled.setTextSize(1);
  oled.setCursor(5, 4);
  oled.print(title);
  if (tag != NULL) {
    int tagWidth = strlen(tag) * 6 + 5;
    int tagX = 123 - tagWidth;
    oled.drawRoundRect(tagX, 2, tagWidth, 10, 3, SH110X_WHITE);
    oled.setCursor(tagX + 3, 4);
    oled.print(tag);
  }
  oled.drawFastHLine(5, 13, 118, SH110X_WHITE);
}

void drawFooterHint(const char *hint) {
  oled.drawFastHLine(5, 52, 118, SH110X_WHITE);
  oled.setTextSize(1);
  oled.setCursor(7, 55);
  oled.print(hint);
}

void drawMenuItem(int y, char key, const char *label) {
  oled.drawRoundRect(5, y, 118, 10, 2, SH110X_WHITE);
  oled.fillRoundRect(7, y + 1, 12, 8, 2, SH110X_WHITE);
  oled.setTextColor(SH110X_BLACK);
  oled.setCursor(11, y + 2);
  oled.print(key);
  oled.setTextColor(SH110X_WHITE);
  oled.setCursor(24, y + 2);
  oled.print(label);
}

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
  oled.setCursor(5, 4);
  oled.print("TIC-TAC-TOE");
  oled.drawRoundRect(95, 2, 27, 10, 3, SH110X_WHITE);
  oled.setCursor(99, 4);
  oled.print("YOU O");

  oled.drawRoundRect(XO_BOARD_X, XO_BOARD_Y, XO_CELL_W * 3, XO_CELL_H * 3, 2, SH110X_WHITE);
  // Grid lines drawn from the same cell geometry as the board layout.
  oled.drawFastVLine(XO_BOARD_X + XO_CELL_W, XO_BOARD_Y, XO_CELL_H * 3, SH110X_WHITE);
  oled.drawFastVLine(XO_BOARD_X + XO_CELL_W * 2, XO_BOARD_Y, XO_CELL_H * 3, SH110X_WHITE);
  oled.drawFastHLine(XO_BOARD_X, XO_BOARD_Y + XO_CELL_H, XO_CELL_W * 3, SH110X_WHITE);
  oled.drawFastHLine(XO_BOARD_X, XO_BOARD_Y + XO_CELL_H * 2, XO_CELL_W * 3, SH110X_WHITE);
}

void drawX(int cell) {
  int x, y;
  xoCellOrigin(cell, x, y);

  oled.drawLine(x + 7, y + 3, x + XO_CELL_W - 7, y + XO_CELL_H - 3, SH110X_WHITE);
  oled.drawLine(x + XO_CELL_W - 7, y + 3, x + 7, y + XO_CELL_H - 3, SH110X_WHITE);
}

void drawO(int cell) {
  int x, y;
  xoCellOrigin(cell, x, y);

  int cx = x + XO_CELL_W / 2;
  int cy = y + XO_CELL_H / 2;
  oled.drawCircle(cx, cy, 5, SH110X_WHITE);
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
  drawScreenFrame();
  drawHeader("NUMBER GUESS", "GAME");

  // A target-style prompt turns the simple single-key game into a clear game
  // screen while retaining its exact rules.
  oled.drawCircle(96, 33, 15, SH110X_WHITE);
  oled.drawCircle(96, 33, 10, SH110X_WHITE);
  oled.fillCircle(96, 33, 3, SH110X_WHITE);
  oled.setTextColor(SH110X_WHITE);
  oled.setTextSize(1);
  oled.setCursor(10, 23);
  oled.print("Pick a digit");
  oled.setCursor(18, 35);
  oled.print("from 0 to 9");
  oled.drawRoundRect(10, 41, 55, 8, 3, SH110X_WHITE);
  oled.setCursor(17, 42);
  oled.print("KEYPAD READY");
  oled.display();

  char entered = keypad.waitForKey();
  oled.clearDisplay();
  drawScreenFrame();
  drawHeader("NUMBER GUESS", "RESULT");
  oled.setTextSize(1);
  if ((entered - '0') == randomNum)
  {
    oled.fillRoundRect(20, 22, 88, 20, 5, SH110X_WHITE);
    oled.setTextColor(SH110X_BLACK);
    oled.setCursor(35, 29);
    oled.print("PERFECT!");
    oled.setTextColor(SH110X_WHITE);
    oled.setCursor(24, 47);
    oled.print("You hit the target");
  }
  else
  {
    oled.setCursor(28, 25);
    oled.print("NOT THIS TIME");
    oled.setCursor(25, 38);
    oled.print("Target was: ");
    oled.setTextSize(2);
    oled.print(randomNum);
  }
  oled.display();
  delay(2000);
}

// Decorative background for the XO game: just a frame (a fill tint would
// hide the marks on a monochrome screen, so it's left plain).
void drawXOBackground() {
  drawScreenFrame();
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
  drawScreenFrame();
  drawHeader("TIC-TAC-TOE", "RESULT");
  oled.setTextSize(1);
  if (winner == 1)
  {
    oled.setCursor(39, 25);
    oled.print("ROBOT WINS");
    oled.setCursor(36, 39);
    oled.print("Try another round");
  }
  else if (winner == 2)
  {
    oled.fillRoundRect(27, 22, 74, 18, 4, SH110X_WHITE);
    oled.setTextColor(SH110X_BLACK);
    oled.setCursor(42, 28);
    oled.print("YOU WIN!");
  }
  else if (winner == 3)
  {
    oled.setCursor(45, 25);
    oled.print("DRAW GAME");
    oled.setCursor(34, 39);
    oled.print("Evenly matched!");
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
  drawScreenFrame();
  drawHeader("REFLEX TEST", "GAME");
  oled.setTextSize(2);
  oled.setCursor(34, 25);
  oled.print("READY");
  oled.setTextSize(1);
  oled.setCursor(25, 44);
  oled.print("Wait for the prompt");
  oled.display();
  delay(randTime);
  oled.clearDisplay();
  long startTime = millis();
  drawScreenFrame();
  drawHeader("REFLEX TEST", "GO!");
  oled.drawRoundRect(47, 21, 34, 24, 5, SH110X_WHITE);
  oled.setTextSize(3);
  oled.setCursor(56, 24);
  oled.print(keys[randX][randY]);
  oled.setTextSize(1);
  oled.setCursor(23, 49);
  oled.print("Press the highlighted key");
  oled.display();
  char pressedKey = keypad.waitForKey();
  int reactionTime = millis() - startTime;

  oled.clearDisplay();
  drawScreenFrame();
  drawHeader("REFLEX TEST", "RESULT");
  oled.setTextSize(1);
  if(pressedKey == keys[randX][randY] && reactionTime < 2000)
  {
    oled.fillRoundRect(29, 19, 70, 15, 4, SH110X_WHITE);
    oled.setTextColor(SH110X_BLACK);
    oled.setCursor(43, 23);
    oled.print("NICE ONE!");
    oled.setTextColor(SH110X_WHITE);
    oled.setCursor(27, 43);
    oled.print(reactionTime);
    oled.print(" ms reaction");
  }
  else if(reactionTime > 2000)
  {
    oled.setCursor(34, 23);
    oled.print("TOO SLOW!");
    oled.setCursor(27, 43);
    oled.print(reactionTime);
    oled.print(" ms reaction");
  }
  else if(pressedKey != keys[randX][randY])
  {
    oled.setCursor(30, 23);
    oled.print("WRONG BUTTON");
    oled.setCursor(27, 43);
    oled.print(reactionTime);
    oled.print(" ms reaction");
  }
  oled.display();
  delay(2000);
}

// Minigames submenu: pick Number Guess, XO, or React — loops until '*' (back)
void showMinigamesMenu() {
  bool inMenu = true;
  while (inMenu) {
    oled.clearDisplay();
    drawScreenFrame();
    drawHeader("ARCADE");

    // Die badge gives the games hub a little personality without using RAM.
    int dieX = 106, dieY = 4, dieSize = 12;
    oled.drawRoundRect(dieX, dieY, dieSize, dieSize, 2, SH110X_WHITE);
    int cx = dieX + dieSize / 2, cy = dieY + dieSize / 2, gap = 3;
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
    drawMenuItem(17, '1', "NUMBER GUESS");
    drawMenuItem(29, '2', "TIC-TAC-TOE");
    drawMenuItem(41, '3', "REFLEX TEST");
    drawFooterHint("*  BACK TO MENU");
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
  :root{--ink:#edf4ff;--muted:#9eabc2;--panel:rgba(16,25,46,.78);--panel2:rgba(24,36,63,.8);--line:rgba(155,190,255,.18);--cyan:#69e5f7;--violet:#a98bff;--lime:#b5f7a0;--danger:#ff758f;}
  *{box-sizing:border-box;}
  body{margin:0;font-family:Inter,'Segoe UI',Roboto,Arial,sans-serif;background:#08101f;color:var(--ink);min-height:100vh;padding:26px 18px;overflow-x:hidden;}
  body:before,body:after{content:'';position:fixed;width:48vw;height:48vw;border-radius:50%;filter:blur(85px);opacity:.22;pointer-events:none;z-index:-1;}
  body:before{background:var(--violet);top:-22vw;left:-14vw;} body:after{background:var(--cyan);right:-18vw;bottom:-25vw;}
  .wrap{max-width:760px;margin:0 auto;}
  h1{display:flex;align-items:center;justify-content:center;gap:10px;font-size:clamp(1.45rem,4vw,2rem);letter-spacing:-.7px;margin:4px 0 7px;font-weight:750;}
  h1 span{background:linear-gradient(100deg,var(--cyan),#a9baff 52%,var(--violet));-webkit-background-clip:text;background-clip:text;color:transparent;}
  .sub{display:flex;justify-content:center;align-items:center;gap:7px;color:var(--muted);margin-bottom:24px;font-size:.8rem;letter-spacing:.08em;text-transform:uppercase;}
  .sub:before{content:'';width:6px;height:6px;border-radius:50%;background:var(--lime);box-shadow:0 0 12px var(--lime);}
  .card{background:linear-gradient(145deg,rgba(30,45,77,.88),var(--panel));border:1px solid var(--line);border-radius:22px;padding:15px;box-shadow:0 18px 46px rgba(0,0,0,.3),inset 0 1px rgba(255,255,255,.04);margin-bottom:15px;backdrop-filter:blur(12px);}
  .video-wrap{position:relative;border-radius:15px;overflow:hidden;border:1px solid rgba(128,190,255,.28);background:#030713;aspect-ratio:4/3;display:flex;align-items:center;justify-content:center;box-shadow:inset 0 0 40px #000;}
  .video-wrap:after{content:'';position:absolute;inset:0;pointer-events:none;background:linear-gradient(115deg,rgba(105,229,247,.11),transparent 28%,transparent 72%,rgba(169,139,255,.1));}
  .video-wrap img{width:100%;height:100%;object-fit:cover;display:block;}
  .badge{position:absolute;z-index:1;top:11px;left:11px;background:rgba(5,10,24,.75);border:1px solid rgba(255,255,255,.13);padding:5px 10px;border-radius:20px;font-size:.68rem;font-weight:700;letter-spacing:.12em;display:flex;align-items:center;gap:7px;backdrop-filter:blur(6px);}
  .dot{width:7px;height:7px;border-radius:50%;background:var(--danger);box-shadow:0 0 9px var(--danger);animation:pulse 1.4s infinite;}
  @keyframes pulse{0%,100%{opacity:1;transform:scale(1)}50%{opacity:.35;transform:scale(.72)}}
  .stats{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;}
  .stat{position:relative;overflow:hidden;background:var(--panel2);border:1px solid var(--line);border-radius:15px;padding:15px 8px 13px;text-align:center;}
  .stat:before{content:'';position:absolute;top:0;left:22%;right:22%;height:2px;background:var(--cyan);box-shadow:0 0 10px var(--cyan);}
  .stat:nth-child(2):before{background:var(--violet);box-shadow:0 0 10px var(--violet)} .stat:nth-child(3):before{background:var(--lime);box-shadow:0 0 10px var(--lime)}
  .stat .val{font-size:clamp(1rem,4vw,1.45rem);font-weight:750;color:var(--cyan);font-variant-numeric:tabular-nums;white-space:nowrap;}
  .stat:nth-child(2) .val{color:#c4b3ff}.stat:nth-child(3) .val{color:var(--lime);font-size:clamp(.9rem,3.5vw,1.22rem);}
  .stat .lbl{font-size:.62rem;color:var(--muted);text-transform:uppercase;letter-spacing:.11em;margin-top:5px;}
  .controls{display:flex;gap:10px;margin-top:14px;flex-wrap:wrap;}
  button{flex:1;min-width:140px;padding:12px 16px;border:1px solid rgba(105,229,247,.45);border-radius:12px;background:linear-gradient(115deg,var(--cyan),#8cbfff);color:#07101e;font-weight:750;letter-spacing:.02em;font-size:.88rem;cursor:pointer;box-shadow:0 7px 18px rgba(77,192,237,.17);transition:transform .15s,box-shadow .15s,filter .15s;}
  button:hover{filter:brightness(1.08);box-shadow:0 10px 25px rgba(77,192,237,.27)} button:active{transform:scale(.97);}
  button.secondary{border-color:rgba(169,139,255,.55);background:linear-gradient(115deg,#a98bff,#d3a8ff);color:#150e2d;box-shadow:0 7px 18px rgba(169,139,255,.16);}
  button.off{border-color:rgba(255,117,143,.5);background:rgba(255,117,143,.14);color:#ffb1c0;box-shadow:none;}
  footer{text-align:center;color:var(--muted);font-size:.7rem;letter-spacing:.1em;text-transform:uppercase;margin:20px 0 4px;}
  @media(max-width:440px){body{padding:18px 11px}.card{padding:11px;border-radius:18px}.stats{gap:6px}.stat{padding:12px 3px}.stat .lbl{font-size:.52rem}.controls{gap:8px}button{min-width:0;padding:11px 8px}}
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

  function refreshStatus() {
    fetch('/status').then(function (r) { return r.json(); }).then(function (d) {
      document.getElementById('temp').textContent = d.temp.toFixed(1) + '\u00B0C';
      document.getElementById('hum').textContent = d.humidity.toFixed(1) + '%';
      document.getElementById('time').textContent = d.time;
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
  char buf[160];
  snprintf(buf, sizeof(buf),
    "{\"temp\":%.1f,\"humidity\":%.1f,\"time\":\"%s\",\"flash\":%s}",
    isnan(t) ? 0.0 : t, isnan(h) ? 0.0 : h, timeStr, flashState ? "true" : "false");
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

  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &status_uri);
    httpd_register_uri_handler(camera_httpd, &flash_uri);
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
  drawScreenFrame();
  drawHeader("CAMERA", cameraReady ? "LIVE" : "OFF");

  if (!cameraReady) {
    oled.setCursor(26, 25);
    oled.print("Camera not");
    oled.setCursor(26, 35);
    oled.print("initialized.");
    drawFooterHint("*  RETURN TO MENU");
    oled.display();
    char k;
    do { k = keypad.waitForKey(); } while (k != '*');
    return;
  }

  oled.setCursor(12, 20);
  oled.print("OPEN THIS ADDRESS");
  oled.drawRoundRect(8, 26, 112, 14, 3, SH110X_WHITE);
  oled.setCursor(13, 29);
  if (WiFi.status() == WL_CONNECTED) {
    oled.print(WiFi.localIP());
  } else {
    oled.print("WiFi offline");
  }
  oled.setCursor(22, 45);
  oled.print("Live stream ready");
  drawFooterHint("*  RETURN TO MENU");
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
  drawScreenFrame();
  drawHeader("MOOD METER", "FACE");

  int face = random(1, 6); // 1-5

  int cx = 64, cy = 32, eyeY = cy - 7, eyeOffset = 10;

  // Head outline, shared by every expression
  oled.drawCircle(cx, cy, 18, SH110X_WHITE);

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
    drawScreenFrame();
    drawHeader("HAPPY ROBOT", "MENU");
    drawMenuItem(17, '1', "MINIGAMES");
    drawMenuItem(29, '2', "CAMERA FEED");
    drawMenuItem(41, '3', "FUNNY FACES");
    drawFooterHint("*  LOCK SCREEN");
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

  drawScreenFrame();
  oled.setTextColor(SH110X_WHITE);
  oled.setTextSize(1);
  drawHeader("HAPPY ROBOT", "LOCKED");

  char pressedKey = 0;
  while (pressedKey != '#')
  {
    oled.fillRect(3, 14, w - 6, h - 17, SH110X_BLACK);

    struct tm timeinfo;
    if (WiFi.status() == WL_CONNECTED && getLocalTime(&timeinfo, 100))
    {
      char timeString[6];
      strftime(timeString, sizeof(timeString), "%H:%M", &timeinfo);
      oled.setTextSize(2);
      oled.setCursor(25, 17);
      oled.print(timeString);
    }
    else
    {
      oled.setTextSize(2);
      oled.setCursor(25, 17);
      oled.print("--:--:-- --");
    }

    float humid = dht.readHumidity();
    float temp = dht.readTemperature();

    oled.setTextSize(1);
    oled.setCursor(12, 39);
    oled.print("TEMP ");
    if (isnan(temp)) {
      oled.print("sensor err");
    } else {
      oled.print(temp, 1);
      oled.print((char)247);
      oled.print("C");
    }

    oled.setCursor(68, 39);
    oled.print("HUM ");
    if (isnan(humid)) {
      oled.print("err");
    } else {
      oled.print(humid, 1);
      oled.print("%");
    }

    drawFooterHint("#  UNLOCK");

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
  drawScreenFrame();

  // Robot badge
  oled.drawRoundRect(47, 5, 34, 24, 7, SH110X_WHITE);
  oled.fillCircle(57, 15, 2, SH110X_WHITE);
  oled.fillCircle(71, 15, 2, SH110X_WHITE);
  oled.drawLine(57, 22, 71, 22, SH110X_WHITE);
  oled.drawLine(64, 5, 64, 1, SH110X_WHITE);
  oled.fillCircle(64, 1, 1, SH110X_WHITE);

  oled.setTextColor(SH110X_WHITE);
  oled.setTextSize(1);
  oled.setCursor(42, 34);
  oled.print("WELCOME");
  oled.setCursor(31, 44);
  oled.print("HAPPY ROBOT");

  if (cameraReady && WiFi.status() == WL_CONNECTED) {
    oled.setCursor(27, 55);
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
