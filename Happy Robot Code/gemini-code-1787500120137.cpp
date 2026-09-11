/* =====================================================================
   ENGMECHAFFINE — ESP32-CAM (AI-Thinker, OV3660) + OLED + I2C + L298N
   =====================================================================
   Board: AI-Thinker ESP32-CAM (select this in Arduino IDE > Tools > Board)
   Enable PSRAM: Tools > PSRAM > Enabled (needed for a decent video size)
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
// WiFi & Time Setup
// =========================
const char* ssid = "Galaxy A55 5G F4F6";
const char* password = "0987654321";
const unsigned long WIFI_TIMEOUT_MS = 15000;

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3 * 3600;
const int daylightOffset_sec = 0;

// =========================
// L298N Motor Driver Pins
// =========================
#define MOTOR_IN1 2
#define MOTOR_IN2 4   // Note: Shares pin with onboard Flash LED
#define MOTOR_IN3 12
#define MOTOR_IN4 16  // WARNING: Shares pin with PSRAM. May cause crashes!

// =========================
// Shared I2C Pins (OLED + keypad)
// =========================
#define I2C_SDA 13
#define I2C_SCL 14

const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {0, 1, 2, 3};
byte colPins[COLS] = {4, 5, 6, 7};

#define KEYPAD_I2C_ADDRESS 0x20
Keypad_I2C keypad = Keypad_I2C(makeKeymap(keys), rowPins, colPins, ROWS, COLS, KEYPAD_I2C_ADDRESS, PCF8574, &Wire);

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_I2C_ADDRESS 0x3C
Adafruit_SH1106G oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// =========================
// Camera Pins 
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
// Motor Control Logic
// =====================================================
void stopMotors() {
  digitalWrite(MOTOR_IN1, LOW);
  digitalWrite(MOTOR_IN2, LOW);
  digitalWrite(MOTOR_IN3, LOW);
  digitalWrite(MOTOR_IN4, LOW);
}

void moveForward() {
  digitalWrite(MOTOR_IN1, HIGH);
  digitalWrite(MOTOR_IN2, LOW);
  digitalWrite(MOTOR_IN3, HIGH);
  digitalWrite(MOTOR_IN4, LOW);
}

void moveBackward() {
  digitalWrite(MOTOR_IN1, LOW);
  digitalWrite(MOTOR_IN2, HIGH);
  digitalWrite(MOTOR_IN3, LOW);
  digitalWrite(MOTOR_IN4, HIGH);
}

void turnLeft() {
  digitalWrite(MOTOR_IN1, LOW);
  digitalWrite(MOTOR_IN2, HIGH);
  digitalWrite(MOTOR_IN3, HIGH);
  digitalWrite(MOTOR_IN4, LOW);
}

void turnRight() {
  digitalWrite(MOTOR_IN1, HIGH);
  digitalWrite(MOTOR_IN2, LOW);
  digitalWrite(MOTOR_IN3, LOW);
  digitalWrite(MOTOR_IN4, HIGH);
}

// =====================================================
// Minigame & OLED Logic (Unchanged)
// =====================================================
const int XO_BOARD_X = 10;
const int XO_BOARD_Y = 12;
const int XO_CELL_W = 36;
const int XO_CELL_H = 17;

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
  oled.drawCircle(x + XO_CELL_W / 2, y + XO_CELL_H / 2, 6, SH110X_WHITE);
}

int checkWinner(int arrayX[], int arrayO[]) {
    int wins[8][3] = { {1,2,3}, {4,5,6}, {7,8,9}, {1,4,7}, {2,5,8}, {3,6,9}, {1,5,9}, {3,5,7} };
    for (int i = 0; i < 8; i++) {
        if (arrayX[wins[i][0]] == wins[i][0] && arrayX[wins[i][1]] == wins[i][1] && arrayX[wins[i][2]] == wins[i][2]) return 1;
        if (arrayO[wins[i][0]] == wins[i][0] && arrayO[wins[i][1]] == wins[i][1] && arrayO[wins[i][2]] == wins[i][2]) return 2;
    }
    bool full = true;
    for (int i = 1; i <= 9; i++) {
        if (arrayX[i] == 0 && arrayO[i] == 0) { full = false; break; }
    }
    if (full) return 3;
    return 0;
}

bool taken(int cell, int arrayX[], int arrayO[]) {
    return arrayX[cell] != 0 || arrayO[cell] != 0;
}

void playNumberGuess() {
  int randomNum = random(10);
  oled.clearDisplay();
  oled.drawRect(0, 0, 128, 64, SH110X_WHITE);
  oled.drawCircle(105, 16, 14, SH110X_WHITE);
  oled.drawCircle(105, 16, 9, SH110X_WHITE);
  oled.fillCircle(105, 16, 3, SH110X_WHITE);
  oled.setTextColor(SH110X_WHITE);
  oled.setCursor(4, 4); oled.print("Guess a number");
  oled.setCursor(4, 14); oled.print("from 0-9");
  oled.display();

  char entered = keypad.waitForKey();
  oled.clearDisplay();
  oled.setCursor(20, 28);
  if ((entered - '0') == randomNum) oled.print("Perfect Guess!!");
  else oled.print("Wrong Guess!!");
  oled.display();
  delay(2000);
}

void drawXOBackground() {
  oled.drawRect(0, 0, 128, 64, SH110X_WHITE);
}

void playXO() {
  int winner = 0;
  int arrayX[10] = {};
  int arrayO[10] = {};

  while (!checkWinner(arrayX, arrayO)) {
    oled.clearDisplay();
    drawXOBackground();
    drawXOGrid();
    for (int c = 1; c <= 9; c++) {
      if (arrayX[c] == c) drawX(c);
      if (arrayO[c] == c) drawO(c);
    }
    oled.display();

    int pc_num = random(1, 10);
    while (taken(pc_num, arrayX, arrayO)) pc_num = random(1, 10);
    arrayX[pc_num] = pc_num;
    winner = checkWinner(arrayX, arrayO);
    if (winner != 0) break;

    char your_turn = keypad.waitForKey();
    while (your_turn - '0' < 1 || your_turn - '0' > 9 || taken(your_turn - '0', arrayX, arrayO)) {
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
  oled.setCursor(40, 28);
  if (winner == 1) oled.print("X Wins!");
  else if (winner == 2) oled.print("O Wins!");
  else if (winner == 3) oled.print("Tied!");
  oled.display();
  delay(2000);
}

void playReact() {
  int randTime = random(5000);
  int randX = random(0, 4); 
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
  if(pressedKey == keys[randX][randY] && reactionTime < 2000) {
    oled.print("Nice Win!!");
  } else if(reactionTime > 2000) {
    oled.print("Slow, Try Again!");
  } else {
    oled.print("Wrong button!");
  }
  oled.setCursor(0, 16); oled.print("Time you took:");
  oled.setCursor(0, 26); oled.print(reactionTime); oled.print(" ms");
  oled.display();
  delay(2000);
}

void showMinigamesMenu() {
  bool inMenu = true;
  while (inMenu) {
    oled.clearDisplay();
    oled.drawRect(0, 0, 128, 64, SH110X_WHITE);
    int dieX = 96, dieY = 6, dieSize = 20;
    oled.drawRect(dieX, dieY, dieSize, dieSize, SH110X_WHITE);
    int cx = dieX + dieSize / 2, cy = dieY + dieSize / 2, gap = 6, face = random(1, 7);
    if (face == 1 || face == 3 || face == 5) oled.fillCircle(cx, cy, 1, SH110X_WHITE);
    if (face >= 2) { oled.fillCircle(cx - gap, cy - gap, 1, SH110X_WHITE); oled.fillCircle(cx + gap, cy + gap, 1, SH110X_WHITE); }
    if (face >= 4) { oled.fillCircle(cx + gap, cy - gap, 1, SH110X_WHITE); oled.fillCircle(cx - gap, cy + gap, 1, SH110X_WHITE); }
    if (face == 6) { oled.fillCircle(cx - gap, cy, 1, SH110X_WHITE); oled.fillCircle(cx + gap, cy, 1, SH110X_WHITE); }
    
    oled.setCursor(4, 8); oled.print("1-Number Guess");
    oled.setCursor(4, 20); oled.print("2-XO");
    oled.setCursor(4, 32); oled.print("3-React Fast");
    oled.setCursor(4, 48); oled.print("*-Back");
    oled.display();

    char game = keypad.waitForKey();
    switch (game) {
      case '1': playNumberGuess(); break;
      case '2': playXO(); break;
      case '3': playReact(); break;
      case '*': inMenu = false; break;
    }
  }
}

// =====================================================
// Live Camera & Web UI (EngMechaffine HUD)
// =====================================================

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static const char INDEX_HTML[] PROGMEM = R"HTMLPAGE(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
<title>EngMechaffine Command</title>
<style>
  :root { --bg: #050608; --panel: #11141c; --accent: #00ffcc; --alert: #ff3366; --text: #d0d4dc; }
  * { box-sizing: border-box; touch-action: manipulation; }
  body { margin: 0; font-family: 'Courier New', Courier, monospace; background: var(--bg); color: var(--text); padding: 15px; }
  .grid-container { max-width: 900px; margin: 0 auto; display: grid; gap: 20px; grid-template-columns: 1fr; }
  @media(min-width: 768px) { .grid-container { grid-template-columns: 2fr 1fr; } }
  
  h1 { text-align: center; color: var(--accent); letter-spacing: 2px; text-transform: uppercase; border-bottom: 2px solid var(--accent); padding-bottom: 10px; margin-top: 5px; }
  .hud-panel { background: var(--panel); border: 1px solid #2a3143; border-radius: 8px; padding: 15px; box-shadow: 0 0 15px rgba(0, 255, 204, 0.05); }
  
  .video-wrap { position: relative; border-radius: 4px; overflow: hidden; border: 2px solid var(--accent); aspect-ratio: 4/3; background: #000; }
  .video-wrap img { width: 100%; height: 100%; object-fit: cover; }
  .badge { position: absolute; top: 10px; left: 10px; background: rgba(0,0,0,0.8); border: 1px solid var(--alert); color: var(--alert); padding: 4px 8px; font-size: 0.8rem; font-weight: bold; }
  
  .joystick-container { display: flex; flex-direction: column; align-items: center; justify-content: center; padding: 20px 0; }
  .dpad { display: grid; grid-template-columns: 60px 60px 60px; grid-template-rows: 60px 60px 60px; gap: 8px; }
  .dpad button { background: #1a1f2e; border: 1px solid var(--accent); color: var(--accent); font-size: 1.5rem; font-weight: bold; border-radius: 8px; cursor: pointer; user-select: none; transition: 0.1s; }
  .dpad button:active { background: var(--accent); color: #000; transform: scale(0.95); }
  
  .btn-up { grid-column: 2; grid-row: 1; }
  .btn-left { grid-column: 1; grid-row: 2; }
  .btn-stop { grid-column: 2; grid-row: 2; background: rgba(255, 51, 102, 0.2) !important; border-color: var(--alert) !important; color: var(--alert) !important; }
  .btn-stop:active { background: var(--alert) !important; color: #000 !important; }
  .btn-right { grid-column: 3; grid-row: 2; }
  .btn-down { grid-column: 2; grid-row: 3; }

  .telemetry { display: flex; justify-content: space-around; margin-top: 15px; font-size: 0.9rem; }
  .telemetry span { color: var(--accent); font-weight: bold; }
</style>
</head>
<body>
<div class="grid-container">
  
  <div class="hud-panel">
    <h1>SYS // ENGMECHAFFINE</h1>
    <div class="video-wrap">
      <span class="badge">REC // LIVE</span>
      <img id="stream" src="" alt="camera stream">
    </div>
    <div class="telemetry">
      <div>TEMP: <span id="temp">--&deg;C</span></div>
      <div>HUM: <span id="hum">--%</span></div>
      <div>SYS TIME: <span id="time">--:--:--</span></div>
    </div>
  </div>

  <div class="hud-panel joystick-container">
    <h3 style="color: var(--text); margin-top:0;">NAV CONTROL</h3>
    <div class="dpad">
      <button class="btn-up" data-dir="F">&#9650;</button>
      <button class="btn-left" data-dir="L">&#9664;</button>
      <button class="btn-stop" data-dir="S">&#9724;</button>
      <button class="btn-right" data-dir="R">&#9654;</button>
      <button class="btn-down" data-dir="B">&#9660;</button>
    </div>
  </div>

</div>

<script>
  document.getElementById('stream').src = 'http://' + window.location.hostname + ':81/stream';

  const buttons = document.querySelectorAll('.dpad button');
  
  function sendCommand(dir) {
    fetch('/move?dir=' + dir).catch(err => console.error(err));
  }

  buttons.forEach(btn => {
    // Pointer events handle both mouse and touch efficiently
    btn.addEventListener('pointerdown', (e) => {
      e.preventDefault();
      sendCommand(btn.getAttribute('data-dir'));
    });
    btn.addEventListener('pointerup', (e) => {
      e.preventDefault();
      sendCommand('S'); // Stop on release
    });
    btn.addEventListener('pointerleave', (e) => {
      sendCommand('S'); // Stop if finger drags off the button
    });
  });

  function refreshStatus() {
    fetch('/status').then(r => r.json()).then(d => {
      document.getElementById('temp').textContent = d.temp.toFixed(1) + '\u00B0C';
      document.getElementById('hum').textContent = d.humidity.toFixed(1) + '%';
      document.getElementById('time').textContent = d.time;
    }).catch(e => {});
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

// New handler to process motor commands
static esp_err_t move_handler(httpd_req_t *req) {
  char query[32];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    if (strstr(query, "dir=F")) moveForward();
    else if (strstr(query, "dir=B")) moveBackward();
    else if (strstr(query, "dir=L")) turnLeft();
    else if (strstr(query, "dir=R")) turnRight();
    else stopMotors();
  }
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, "{\"status\":\"ok\"}", -1);
}

static esp_err_t status_handler(httpd_req_t *req) {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  char timeStr[16] = "--:--:--";
  struct tm timeinfo;
  if (WiFi.status() == WL_CONNECTED && getLocalTime(&timeinfo, 100)) {
    strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
  }
  char buf[128];
  snprintf(buf, sizeof(buf), "{\"temp\":%.1f,\"humidity\":%.1f,\"time\":\"%s\"}", 
           isnan(t) ? 0.0 : t, isnan(h) ? 0.0 : h, timeStr);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, buf, strlen(buf));
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
      res = ESP_FAIL;
    } else {
      if (fb->format != PIXFORMAT_JPEG) {
        bool converted = frame2jpg(fb, 80, &jpgBuf, &jpgLen);
        esp_camera_fb_return(fb);
        fb = NULL;
        if (!converted) res = ESP_FAIL;
      } else {
        jpgLen = fb->len;
        jpgBuf = fb->buf;
      }
    }
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (res == ESP_OK) {
      size_t hlen = snprintf(partBuf, sizeof(partBuf), STREAM_PART, (unsigned)jpgLen);
      res = httpd_resp_send_chunk(req, partBuf, hlen);
    }
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)jpgBuf, jpgLen);
    
    if (fb) { esp_camera_fb_return(fb); fb = NULL; jpgBuf = NULL; } 
    else if (jpgBuf) { free(jpgBuf); jpgBuf = NULL; }
    
    if (res != ESP_OK) break;
  }
  return res;
}

void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;

  httpd_uri_t index_uri  = { "/",       HTTP_GET, index_handler,  NULL };
  httpd_uri_t status_uri = { "/status", HTTP_GET, status_handler, NULL };
  httpd_uri_t move_uri   = { "/move",   HTTP_GET, move_handler,   NULL };

  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &status_uri);
    httpd_register_uri_handler(camera_httpd, &move_uri);
  }

  config.server_port = 81;
  config.ctrl_port = 32769;
  httpd_uri_t stream_uri = { "/stream", HTTP_GET, stream_handler, NULL };
  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
  }
}

bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM; config.pin_d2 = Y4_GPIO_NUM; config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM; config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM; config.pin_pclk = PCLK_GPIO_NUM; config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM; config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM; config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  if (psramFound()) {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
  } else {
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 15;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) return false;

  sensor_t *s = esp_camera_sensor_get();
  if (s != NULL && s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  }
  return true;
}

void playCameraFeed() {
  oled.clearDisplay();
  oled.drawRect(0, 0, 128, 64, SH110X_WHITE);
  oled.setCursor(4, 4); oled.print("LIVE CAMERA FEED");
  oled.drawLine(4, 13, 124, 13, SH110X_WHITE);

  if (!cameraReady) {
    oled.setCursor(4, 24); oled.print("Camera error.");
    oled.setCursor(4, 50); oled.print("Press * for menu");
    oled.display();
    char k; do { k = keypad.waitForKey(); } while (k != '*');
    return;
  }

  oled.setCursor(4, 18); oled.print("Open in browser:");
  oled.setCursor(4, 30);
  if (WiFi.status() == WL_CONNECTED) oled.print(WiFi.localIP());
  else oled.print("WiFi offline");
  
  oled.setCursor(4, 54); oled.print("Press * to go back");
  oled.display();

  char k; do { k = keypad.waitForKey(); } while (k != '*');
}

void showFaces() {
  oled.clearDisplay();
  oled.drawRect(0, 0, 128, 64, SH110X_WHITE);
  int face = random(1, 6);
  int cx = 64, cy = 26, eyeY = cy - 8, eyeOffset = 12;
  oled.drawCircle(cx, cy, 22, SH110X_WHITE);

  switch (face) {
    case 1: 
      oled.fillCircle(cx - eyeOffset, eyeY, 2, SH110X_WHITE); oled.fillCircle(cx + eyeOffset, eyeY, 2, SH110X_WHITE);
      oled.drawLine(cx - 10, cy + 8, cx - 5, cy + 13, SH110X_WHITE); oled.drawLine(cx - 5, cy + 13, cx + 5, cy + 13, SH110X_WHITE); oled.drawLine(cx + 5, cy + 13, cx + 10, cy + 8, SH110X_WHITE);
      oled.setCursor(48, 54); oled.print("Happy!"); break;
    case 2: 
      oled.fillCircle(cx - eyeOffset, eyeY, 2, SH110X_WHITE); oled.fillCircle(cx + eyeOffset, eyeY, 2, SH110X_WHITE);
      oled.drawLine(cx - 10, cy + 13, cx - 5, cy + 8, SH110X_WHITE); oled.drawLine(cx - 5, cy + 8, cx + 5, cy + 8, SH110X_WHITE); oled.drawLine(cx + 5, cy + 8, cx + 10, cy + 13, SH110X_WHITE);
      oled.setCursor(54, 54); oled.print("Sad"); break;
    case 3: 
      oled.drawCircle(cx - eyeOffset, eyeY, 3, SH110X_WHITE); oled.drawCircle(cx + eyeOffset, eyeY, 3, SH110X_WHITE);
      oled.drawCircle(cx, cy + 11, 4, SH110X_WHITE);
      oled.setCursor(32, 54); oled.print("Surprised!"); break;
    case 4: 
      oled.drawLine(cx - eyeOffset - 2, eyeY, cx - eyeOffset + 2, eyeY, SH110X_WHITE); oled.fillCircle(cx + eyeOffset, eyeY, 2, SH110X_WHITE);
      oled.drawLine(cx - 8, cy + 11, cx + 9, cy + 8, SH110X_WHITE);
      oled.setCursor(52, 54); oled.print("Wink!"); break;
    case 5: 
      oled.drawLine(cx - 16, eyeY - 5, cx - 6, eyeY - 2, SH110X_WHITE); oled.drawLine(cx + 16, eyeY - 5, cx + 6, eyeY - 2, SH110X_WHITE);
      oled.fillCircle(cx - eyeOffset, eyeY, 2, SH110X_WHITE); oled.fillCircle(cx + eyeOffset, eyeY, 2, SH110X_WHITE);
      oled.drawLine(cx - 9, cy + 11, cx + 9, cy + 11, SH110X_WHITE);
      oled.setCursor(50, 54); oled.print("Angry!"); break;
  }
  oled.display();
  keypad.waitForKey();
}

void showMainMenu() {
  bool inMenu = true;
  while (inMenu) {
    oled.clearDisplay();
    oled.drawRect(0, 0, 128, 64, SH110X_WHITE);
    oled.setCursor(32, 2); oled.print("MAIN MENU");
    oled.drawLine(4, 11, 124, 11, SH110X_WHITE);

    oled.setCursor(4, 16); oled.print("1-Minigames");
    oled.setCursor(4, 28); oled.print("2-Camera Feed");
    oled.setCursor(4, 40); oled.print("3-Funny Faces");
    oled.setCursor(4, 52); oled.print("*-Lock Screen");
    oled.display();

    char choice = keypad.waitForKey();
    switch (choice) {
      case '1': showMinigamesMenu(); break;
      case '2': playCameraFeed(); break;
      case '3': showFaces(); break;
      case '*': inMenu = false; break;
    }
  }
}

void showLockScreen() {
  oled.clearDisplay();
  oled.drawRect(0, 0, 128, 64, SH110X_WHITE);
  oled.setCursor(34, 2); oled.print("LOCK SCREEN");
  oled.drawLine(4, 11, 124, 11, SH110X_WHITE);

  char pressedKey = 0;
  while (pressedKey != '#') {
    oled.fillRect(1, 12, 126, 51, SH110X_BLACK);

    struct tm timeinfo;
    if (WiFi.status() == WL_CONNECTED && getLocalTime(&timeinfo, 100)) {
      char timeString[12];
      strftime(timeString, sizeof(timeString), "%I:%M:%S %p", &timeinfo);
      oled.setCursor(20, 16); oled.print(timeString);
    } else {
      oled.setCursor(14, 16); oled.print("--:--:-- --");
    }

    float humid = dht.readHumidity();
    float temp = dht.readTemperature();

    oled.setCursor(4, 30); oled.print("Temp: ");
    if (isnan(temp)) oled.print("err"); else { oled.print(temp, 1); oled.print((char)247); oled.print("C"); }

    oled.setCursor(4, 42); oled.print("Humidity: ");
    if (isnan(humid)) oled.print("err"); else { oled.print(humid, 1); oled.print("%"); }

    oled.setCursor(4, 54); oled.print("Press # to proceed");
    oled.display();

    pressedKey = keypad.getKey();
    delay(1000);
  }
  showMainMenu();
}

bool connectWiFi() {
  oled.clearDisplay();
  oled.setCursor(0, 0); oled.print("Connecting to WiFi");
  oled.display();

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  unsigned long startAttempt = millis();
  int dots = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < WIFI_TIMEOUT_MS) {
    delay(400);
    oled.setCursor((dots % 20) * 6, 12); oled.print(".");
    dots++;
  }
  
  oled.clearDisplay(); oled.setCursor(0, 0);
  if (WiFi.status() == WL_CONNECTED) {
    oled.print("WiFi connected!");
    oled.setCursor(0, 12); oled.print(WiFi.localIP());
  } else {
    oled.print("WiFi failed.");
    oled.setCursor(0, 12); oled.print("Running offline.");
  }
  oled.display();
  delay(1500);
  return WiFi.status() == WL_CONNECTED;
}

void showWelcomeScreen() {
  oled.clearDisplay();
  oled.drawCircle(64, 16, 12, SH110X_WHITE);
  oled.fillCircle(59, 14, 2, SH110X_WHITE); oled.fillCircle(69, 14, 2, SH110X_WHITE);
  oled.drawLine(59, 20, 69, 20, SH110X_WHITE); oled.drawLine(64, 4, 64, 0, SH110X_WHITE); oled.fillCircle(64, 0, 1, SH110X_WHITE);

  oled.setCursor(20, 32); oled.print("ENGMECHAFFINE OS");
  if (cameraReady && WiFi.status() == WL_CONNECTED) {
    oled.setCursor(2, 54); oled.print(WiFi.localIP());
  }
  oled.display();
  delay(2000);
}

void setup() {
  Serial.begin(115200);
  delay(200);

  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  // Initialize motor pins
  pinMode(MOTOR_IN1, OUTPUT);
  pinMode(MOTOR_IN2, OUTPUT);
  pinMode(MOTOR_IN3, OUTPUT);
  pinMode(MOTOR_IN4, OUTPUT);
  stopMotors();

  Wire.begin(I2C_SDA, I2C_SCL);
  if (!oled.begin(OLED_I2C_ADDRESS, true)) for (;;);
  oled.clearDisplay(); oled.setTextColor(SH110X_WHITE); oled.setTextSize(1);
  
  dht.begin();
  keypad.begin();

  bool wifiOk = connectWiFi();
  if (wifiOk) configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  cameraReady = initCamera();
  if (cameraReady) startCameraServer();

  showWelcomeScreen();
}

void loop() {
  char start = keypad.waitForKey();
  if (start == '#') showLockScreen();
}