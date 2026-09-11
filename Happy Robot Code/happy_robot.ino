/* =====================================================================
   HAPPY ROBOT — ESP32-CAM (AI-Thinker, OV3660) + OLED + I2C Keypad
                 + L298N motor driver (WiFi joystick control)
   =====================================================================
   Board: AI-Thinker ESP32-CAM (select this in Arduino IDE > Tools > Board)
   Enable PSRAM: Tools > PSRAM > Enabled (needed for a decent video size)

   WHAT'S NEW IN THIS VERSION
   -----------------------------------
   - L298N motor driver support: IN1=GPIO2, IN2=GPIO4, IN3=GPIO12,
     IN4=GPIO16, ENA/ENB wired straight to 5V (always-on, no PWM speed
     control — direction only).
   - The web dashboard now has an on-screen joystick that drives the
     robot in 8 directions + stop, right next to the live video.
   - A physical "Drive Robot" option was added to the OLED main menu
     (keys 8/2/4/6 = forward/back/left/right, 5 = stop, * = exit) as an
     offline fallback that uses the same motor code as the web joystick.
   - A watchdog task auto-stops the motors if no /move command has been
     received for 800ms — protects against the browser tab closing,
     WiFi dropping, or a stuck joystick leaving the robot driving blind.

   *** IMPORTANT — READ BEFORE WIRING ***
   The AI-Thinker ESP32-CAM only exposes 10 GPIOs on its header, and the
   camera + OLED/keypad + DHT11 in this sketch already claim most of the
   safe ones. That leaves only the "risky" pins for the motor driver,
   so please note:

   - GPIO4 (IN2) is HARD-WIRED on the board itself to the onboard white
     flash LED. There is no way to separate them in software — driving
     the left motor will also flicker the flash LED, and vice versa.
     Because of this, independent flash-LED web control has been
     removed (a "Flash On" button here would have silently driven a
     motor, which is not something to ship). The LED will just flicker
     as a harmless side effect of motor movement.
   - GPIO12 (IN3) is an ESP32 boot-strapping pin (MTDI). It must read
     LOW at power-on/reset or the chip can try to boot expecting the
     wrong flash voltage and fail to start. If the robot won't boot
     once the L298N is connected, this pin is the first suspect — try
     adding a ~10kΩ pull-down resistor from GPIO12 to GND, or wiring
     IN3 through a small delay/relay so it isn't driven high at
     power-up. This is a hardware-level constraint software can't fully
     work around.
   - GPIO2 and GPIO16 are also somewhat non-standard choices reported as
     occasionally unreliable by other builders, but they're commonly
     used for motor control on this board in practice and should be
     fine for on/off direction signals like this project uses.

   If you run into boot loops or reset issues after wiring the driver,
   it's very likely GPIO12 — that one is worth double-checking first.

   PIN NOTES FOR AI-THINKER ESP32-CAM (unchanged from before)
   -----------------------------------
   The I2C bus (GPIO13/14) and the DHT11 (GPIO15) are free only because
   the onboard microSD card is not being used — those pins are shared
   with the SD_MMC peripheral. Don't add the SD card without moving them.
   ===================================================================== */

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

// IMPORTANT: the ESP32 only has a 2.4GHz radio. If your SSID has "5G" in
// the name (common on phone hotspots, e.g. "Galaxy A55 5G F4F6"), that
// almost always means the hotspot is broadcasting on the 5GHz band and
// this board will NEVER see it, let alone connect - it'll just time out
// every time regardless of password. On the phone, switch the hotspot
// band to 2.4GHz (Samsung: Settings > Connections > Mobile Hotspot and
// Tethering > Configure > Band > 2.4GHz), then update ssid below to match
// whatever name it broadcasts afterward.
const char* ssid = "Galaxy A55 F4F6";  // TODO: confirm this against whatever
                                        // name your phone actually shows once
                                        // the hotspot band is set to 2.4GHz -
                                        // don't just trust this guess
const char* password = "0987654321";

const unsigned long WIFI_TIMEOUT_MS = 15000; // give up after 15s and go offline
const unsigned long WIFI_RETRY_INTERVAL_MS = 30000; // if offline, retry this often
unsigned long lastWifiRetryMillis = 0;

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

// =====================================================
// L298N motor driver
// ENA/ENB are wired directly to 5V, so there is no PWM speed control -
// each motor is simply forward / backward / stopped.
// =====================================================
#define MOTOR_IN1 2    // Left motor, direction pin A
#define MOTOR_IN2 4    // Left motor, direction pin B  (shared w/ onboard flash LED, see notes above)
#define MOTOR_IN3 12   // Right motor, direction pin A (boot-strapping pin, see notes above)
#define MOTOR_IN4 16   // Right motor, direction pin B

volatile unsigned long lastMoveCommandMillis = 0;
const unsigned long MOVE_WATCHDOG_MS = 800; // auto-stop if no /move command arrives within this window
String currentDirection = "stop";

void motorsLeftForward()   { digitalWrite(MOTOR_IN1, HIGH); digitalWrite(MOTOR_IN2, LOW); }
void motorsLeftBackward()  { digitalWrite(MOTOR_IN1, LOW);  digitalWrite(MOTOR_IN2, HIGH); }
void motorsLeftStop()      { digitalWrite(MOTOR_IN1, LOW);  digitalWrite(MOTOR_IN2, LOW); }

void motorsRightForward()  { digitalWrite(MOTOR_IN3, HIGH); digitalWrite(MOTOR_IN4, LOW); }
void motorsRightBackward() { digitalWrite(MOTOR_IN3, LOW);  digitalWrite(MOTOR_IN4, HIGH); }
void motorsRightStop()     { digitalWrite(MOTOR_IN3, LOW);  digitalWrite(MOTOR_IN4, LOW); }

void motorsStop() {
  motorsLeftStop();
  motorsRightStop();
  currentDirection = "stop";
}

// Bang-bang (no PWM) 8-direction differential drive, driven by either the
// web joystick (/move?dir=...) or the physical keypad drive mode.
void driveDirection(const String &dir) {
  lastMoveCommandMillis = millis();
  currentDirection = dir;

  if (dir == "forward")            { motorsLeftForward();  motorsRightForward();  }
  else if (dir == "backward")      { motorsLeftBackward();  motorsRightBackward(); }
  else if (dir == "left")          { motorsLeftBackward();  motorsRightForward();  } // pivot left
  else if (dir == "right")         { motorsLeftForward();   motorsRightBackward(); } // pivot right
  else if (dir == "forward_left")  { motorsLeftStop();      motorsRightForward();  } // arc left
  else if (dir == "forward_right") { motorsLeftForward();   motorsRightStop();     } // arc right
  else if (dir == "backward_left") { motorsLeftStop();      motorsRightBackward(); }
  else if (dir == "backward_right"){ motorsLeftBackward();  motorsRightStop();     }
  else                              { motorsStop(); }
}

// Runs independently of the main loop (which spends most of its time
// blocked inside OLED menus), so the robot can't get "stuck" driving if
// the browser stops sending commands.
void motorWatchdogTask(void *param) {
  for (;;) {
    if (currentDirection != "stop" && millis() - lastMoveCommandMillis > MOVE_WATCHDOG_MS) {
      motorsStop();
      Serial.println("Motor watchdog: no command received in time, stopping.");
    }
    vTaskDelay(pdMS_TO_TICKS(150));
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
// Live camera feed + web GUI (MJPEG stream, joystick, telemetry)
// =====================================================

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// Styled single-page dashboard: live video + joystick side by side, plus
// a small telemetry panel that polls /status every couple of seconds.
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
  .wrap{max-width:720px;margin:0 auto;}
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
  footer{text-align:center;color:var(--muted);font-size:.75rem;margin-top:20px;}

  .joy-row{display:flex;align-items:center;gap:20px;justify-content:center;flex-wrap:wrap;}
  .joy-base{position:relative;width:150px;height:150px;border-radius:50%;background:radial-gradient(circle at 35% 35%,#232833,#12141a);border:2px solid #2b3140;touch-action:none;flex-shrink:0;}
  .joy-knob{position:absolute;top:50%;left:50%;width:56px;height:56px;margin:-28px 0 0 -28px;border-radius:50%;background:linear-gradient(145deg,var(--accent),#2c9d92);box-shadow:0 4px 10px rgba(0,0,0,.5);transition:transform .05s linear;}
  .joy-info{text-align:center;min-width:120px;}
  .joy-dir-label{font-size:.7rem;color:var(--muted);text-transform:uppercase;letter-spacing:.5px;}
  .joy-dir{font-size:1.1rem;font-weight:600;color:var(--accent);margin:4px 0 12px;text-transform:capitalize;}
  .stop-btn{width:100%;padding:10px;border:none;border-radius:10px;background:#e53e3e;color:#fff;font-weight:700;letter-spacing:.5px;cursor:pointer;}
  .stop-btn:active{transform:scale(.95);}
</style>
</head>
<body>
<div class="wrap">
  <h1>&#129302; <span>Happy Robot</span> Control Panel</h1>
  <div class="sub">Live camera + drive controls</div>

  <div class="card">
    <div class="video-wrap">
      <span class="badge"><span class="dot"></span> LIVE</span>
      <img id="stream" src="" alt="camera stream">
    </div>
    <div class="controls">
      <button id="snapBtn">Snapshot</button>
    </div>
  </div>

  <div class="card">
    <div class="joy-row">
      <div class="joy-base" id="joyBase">
        <div class="joy-knob" id="joyKnob"></div>
      </div>
      <div class="joy-info">
        <div class="joy-dir-label">Direction</div>
        <div class="joy-dir" id="dirLabel">stop</div>
        <button id="stopBtn" class="stop-btn">STOP</button>
      </div>
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

  // ---------------- Joystick ----------------
  (function () {
    var base = document.getElementById('joyBase');
    var knob = document.getElementById('joyKnob');
    var dirLabel = document.getElementById('dirLabel');
    var radius = 47; // max knob travel in px
    var dragging = false;
    var sendTimer = null;
    var currentDir = 'stop';

    function setKnob(x, y) {
      knob.style.transform = 'translate(' + x + 'px,' + y + 'px)';
    }

    function computeDir(dx, dy) {
      var nx = dx / radius;
      var ny = -dy / radius;
      var mag = Math.min(1, Math.sqrt(nx * nx + ny * ny));
      if (mag < 0.3) return 'stop';
      var angle = Math.atan2(ny, nx) * 180 / Math.PI;
      if (angle >= -22.5 && angle < 22.5) return 'right';
      if (angle >= 22.5 && angle < 67.5) return 'forward_right';
      if (angle >= 67.5 && angle < 112.5) return 'forward';
      if (angle >= 112.5 && angle < 157.5) return 'forward_left';
      if (angle >= 157.5 || angle < -157.5) return 'left';
      if (angle >= -157.5 && angle < -112.5) return 'backward_left';
      if (angle >= -112.5 && angle < -67.5) return 'backward';
      return 'backward_right';
    }

    function sendDir(dir) {
      fetch('/move?dir=' + dir).catch(function () {});
    }

    function updateFromPoint(clientX, clientY) {
      var r = base.getBoundingClientRect();
      var cx = r.left + r.width / 2;
      var cy = r.top + r.height / 2;
      var dx = clientX - cx;
      var dy = clientY - cy;
      var dist = Math.sqrt(dx * dx + dy * dy);
      if (dist > radius) {
        dx = dx / dist * radius;
        dy = dy / dist * radius;
      }
      setKnob(dx, dy);
      var dir = computeDir(dx, dy);
      if (dir !== currentDir) {
        currentDir = dir;
        dirLabel.textContent = dir.replace(/_/g, ' ');
        sendDir(dir);
      }
    }

    function reset() {
      setKnob(0, 0);
      currentDir = 'stop';
      dirLabel.textContent = 'stop';
      sendDir('stop');
      if (sendTimer) { clearInterval(sendTimer); sendTimer = null; }
    }

    base.addEventListener('pointerdown', function (e) {
      dragging = true;
      base.setPointerCapture(e.pointerId);
      updateFromPoint(e.clientX, e.clientY);
      sendTimer = setInterval(function () { sendDir(currentDir); }, 300);
    });
    base.addEventListener('pointermove', function (e) {
      if (!dragging) return;
      updateFromPoint(e.clientX, e.clientY);
    });
    ['pointerup', 'pointercancel', 'pointerleave'].forEach(function (evt) {
      base.addEventListener(evt, function () {
        if (!dragging) return;
        dragging = false;
        reset();
      });
    });

    document.getElementById('stopBtn').onclick = reset;
    document.addEventListener('visibilitychange', function () {
      if (document.hidden) reset();
    });
    window.addEventListener('beforeunload', reset);
  })();
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
    "{\"temp\":%.1f,\"humidity\":%.1f,\"time\":\"%s\"}",
    isnan(t) ? 0.0 : t, isnan(h) ? 0.0 : h, timeStr);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, buf, strlen(buf));
}

// Drives the robot. Expects ?dir=forward|backward|left|right|forward_left|
// forward_right|backward_left|backward_right|stop
static esp_err_t move_handler(httpd_req_t *req) {
  char query[64];
  char dirVal[24] = "stop";
  if (httpd_req_get_url_query_len(req) > 0 &&
      httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    httpd_query_key_value(query, "dir", dirVal, sizeof(dirVal));
  }
  driveDirection(String(dirVal));

  char resp[48];
  snprintf(resp, sizeof(resp), "{\"dir\":\"%s\"}", dirVal);
  httpd_resp_set_type(req, "application/json");
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
  httpd_uri_t move_uri    = { "/move",    HTTP_GET, move_handler,    NULL };

  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &status_uri);
    httpd_register_uri_handler(camera_httpd, &move_uri);
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
// Manual drive mode (physical keypad fallback for the web joystick)
// =====================================================

void driveModeManual() {
  oled.clearDisplay();
  oled.drawRect(0, 0, 128, 64, SH110X_WHITE);
  oled.setTextColor(SH110X_WHITE);
  oled.setTextSize(1);
  oled.setCursor(4, 4);
  oled.print("MANUAL DRIVE");
  oled.drawLine(4, 13, 124, 13, SH110X_WHITE);
  oled.setCursor(4, 18);
  oled.print("8=Fwd   2=Back");
  oled.setCursor(4, 28);
  oled.print("4=Left  6=Right");
  oled.setCursor(4, 38);
  oled.print("5=Stop  *=Exit");
  oled.display();

  bool driving = true;
  while (driving) {
    oled.fillRect(2, 48, 124, 14, SH110X_BLACK);
    oled.setCursor(4, 50);
    oled.print("Dir: ");
    oled.print(currentDirection);
    oled.display();

    char k = keypad.getKey();
    if (k) {
      switch (k) {
        case '8': driveDirection("forward"); break;
        case '2': driveDirection("backward"); break;
        case '4': driveDirection("left"); break;
        case '6': driveDirection("right"); break;
        case '5': driveDirection("stop"); break;
        case '*': driveDirection("stop"); driving = false; break;
        default: break;
      }
    }
    delay(80);
  }
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

    oled.setCursor(4, 14);
    oled.print("1-Minigames");
    oled.setCursor(4, 24);
    oled.print("2-Camera Feed");
    oled.setCursor(4, 34);
    oled.print("3-Funny Faces");
    oled.setCursor(4, 44);
    oled.print("4-Drive Robot");
    oled.setCursor(4, 54);
    oled.print("*-Lock Screen");
    oled.display();

    char choice = keypad.waitForKey();
    switch (choice) {
      case '1': showMinigamesMenu(); break;
      case '2': playCameraFeed(); break;
      case '3': showFaces(); break;
      case '4': driveModeManual(); break;
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

    // Non-blocking periodic retry: if we're offline, try again every
    // WIFI_RETRY_INTERVAL_MS instead of staying offline until reboot.
    // WiFi.reconnect() doesn't block, so this won't freeze the clock/menu.
    if (WiFi.status() != WL_CONNECTED &&
        millis() - lastWifiRetryMillis > WIFI_RETRY_INTERVAL_MS) {
      Serial.println("WiFi offline - attempting reconnect...");
      WiFi.reconnect();
      lastWifiRetryMillis = millis();
    }

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
  WiFi.disconnect(true);  // clear any stale/cached connection state first
  delay(100);
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
    WiFi.setSleep(false); // keeps the camera stream/joystick responsive
  }
  else
  {
    oled.print("WiFi failed.");
    oled.setCursor(0, 12);
    oled.print("Running offline.");
    Serial.println("WiFi connect timed out - continuing offline.");
    Serial.println("(If your SSID has \"5G\" in it, the ESP32 can't see 5GHz networks - switch the hotspot to 2.4GHz.)");
  }
  oled.display();
  delay(1500);
  lastWifiRetryMillis = millis();
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

  // Disable brownout detector: the camera (and now the motors) can cause
  // brief voltage dips that would otherwise reset an ESP32-CAM board.
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  // Force the motor driver pins to a known, stopped state as early as
  // possible. Note this can't fully protect GPIO12 at power-on itself —
  // that strapping read happens in the boot ROM before this code runs —
  // see the top-of-file notes if you hit boot problems after wiring IN3.
  pinMode(MOTOR_IN1, OUTPUT);
  pinMode(MOTOR_IN2, OUTPUT);
  pinMode(MOTOR_IN3, OUTPUT);
  pinMode(MOTOR_IN4, OUTPUT);
  motorsStop();

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

  // 5. Motor safety watchdog - runs independently of the (often blocked) main loop
  xTaskCreate(motorWatchdogTask, "motorWatchdog", 2048, NULL, 1, NULL);

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
