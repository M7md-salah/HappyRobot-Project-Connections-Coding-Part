/* =====================================================================
   ENGMECHAFFINE — ESP32-CAM (AI-Thinker) + OLED + I2C + L298N
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
#define MOTOR_IN2 4   
#define MOTOR_IN3 12
#define MOTOR_IN4 16  // WARNING: Shares pin with PSRAM! 

// =========================
// Shared I2C Pins
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

unsigned long lastClockUpdate = 0;

// =====================================================
// Motor Control Logic
// =====================================================
void stopMotors() {
  digitalWrite(MOTOR_IN1, LOW); digitalWrite(MOTOR_IN2, LOW); digitalWrite(MOTOR_IN3, LOW); digitalWrite(MOTOR_IN4, LOW);
}
void moveForward() {
  digitalWrite(MOTOR_IN1, HIGH); digitalWrite(MOTOR_IN2, LOW); digitalWrite(MOTOR_IN3, HIGH); digitalWrite(MOTOR_IN4, LOW);
}
void moveBackward() {
  digitalWrite(MOTOR_IN1, LOW); digitalWrite(MOTOR_IN2, HIGH); digitalWrite(MOTOR_IN3, LOW); digitalWrite(MOTOR_IN4, HIGH);
}
void turnLeft() {
  digitalWrite(MOTOR_IN1, LOW); digitalWrite(MOTOR_IN2, HIGH); digitalWrite(MOTOR_IN3, HIGH); digitalWrite(MOTOR_IN4, LOW);
}
void turnRight() {
  digitalWrite(MOTOR_IN1, HIGH); digitalWrite(MOTOR_IN2, LOW); digitalWrite(MOTOR_IN3, LOW); digitalWrite(MOTOR_IN4, HIGH);
}

// =====================================================
// Aesthetically Remodeled Minigames 
// =====================================================
const int XO_BOARD_X = 35;
const int XO_BOARD_Y = 5;
const int XO_CELL_W = 18;
const int XO_CELL_H = 18;

void xoCellOrigin(int cell, int &x, int &y) {
  int col = (cell - 1) % 3;
  int row = (cell - 1) / 3;
  x = XO_BOARD_X + col * XO_CELL_W;
  y = XO_BOARD_Y + row * XO_CELL_H;
}

void drawXOGrid() {
  // Thicker, centralized mecha grid
  oled.drawRect(XO_BOARD_X, XO_BOARD_Y, XO_CELL_W * 3, XO_CELL_H * 3, SH110X_WHITE);
  oled.drawFastVLine(XO_BOARD_X + XO_CELL_W, XO_BOARD_Y, XO_CELL_H * 3, SH110X_WHITE);
  oled.drawFastVLine(XO_BOARD_X + XO_CELL_W * 2, XO_BOARD_Y, XO_CELL_H * 3, SH110X_WHITE);
  oled.drawFastHLine(XO_BOARD_X, XO_BOARD_Y + XO_CELL_H, XO_CELL_W * 3, SH110X_WHITE);
  oled.drawFastHLine(XO_BOARD_X, XO_BOARD_Y + XO_CELL_H * 2, XO_CELL_W * 3, SH110X_WHITE);
  oled.setTextSize(1);
  oled.setCursor(2, 6); oled.print("T-T-T");
}

void drawX(int cell) {
  int x, y; xoCellOrigin(cell, x, y);
  oled.drawLine(x + 3, y + 3, x + XO_CELL_W - 3, y + XO_CELL_H - 3, SH110X_WHITE);
  oled.drawLine(x + XO_CELL_W - 3, y + 3, x + 3, y + XO_CELL_H - 3, SH110X_WHITE);
}

void drawO(int cell) {
  int x, y; xoCellOrigin(cell, x, y);
  oled.drawCircle(x + XO_CELL_W / 2, y + XO_CELL_H / 2, 6, SH110X_WHITE);
}

int checkWinner(int arrayX[], int arrayO[]) {
    int wins[8][3] = { {1,2,3}, {4,5,6}, {7,8,9}, {1,4,7}, {2,5,8}, {3,6,9}, {1,5,9}, {3,5,7} };
    for (int i = 0; i < 8; i++) {
        if (arrayX[wins[i][0]] == wins[i][0] && arrayX[wins[i][1]] == wins[i][1] && arrayX[wins[i][2]] == wins[i][2]) return 1;
        if (arrayO[wins[i][0]] == wins[i][0] && arrayO[wins[i][1]] == wins[i][1] && arrayO[wins[i][2]] == wins[i][2]) return 2;
    }
    bool full = true;
    for (int i = 1; i <= 9; i++) if (arrayX[i] == 0 && arrayO[i] == 0) { full = false; break; }
    if (full) return 3;
    return 0;
}
bool taken(int cell, int arrayX[], int arrayO[]) { return arrayX[cell] != 0 || arrayO[cell] != 0; }

void playXO() {
  int winner = 0;
  int arrayX[10] = {}; int arrayO[10] = {};

  while (!checkWinner(arrayX, arrayO)) {
    oled.clearDisplay(); oled.drawRect(0, 0, 128, 64, SH110X_WHITE); drawXOGrid();
    for (int c = 1; c <= 9; c++) { if (arrayX[c] == c) drawX(c); if (arrayO[c] == c) drawO(c); }
    oled.display();

    int pc_num = random(1, 10);
    while (taken(pc_num, arrayX, arrayO)) pc_num = random(1, 10);
    arrayX[pc_num] = pc_num;
    winner = checkWinner(arrayX, arrayO);
    if (winner != 0) break;

    char your_turn = keypad.waitForKey();
    while (your_turn - '0' < 1 || your_turn - '0' > 9 || taken(your_turn - '0', arrayX, arrayO)) your_turn = keypad.waitForKey();
    arrayO[your_turn - '0'] = (your_turn - '0');
    winner = checkWinner(arrayX, arrayO);
  }
  oled.clearDisplay(); oled.drawRect(0, 0, 128, 64, SH110X_WHITE); drawXOGrid();
  for (int c = 1; c <= 9; c++) { if (arrayX[c] == c) drawX(c); if (arrayO[c] == c) drawO(c); }
  oled.display(); delay(600);

  oled.clearDisplay(); oled.drawRect(0, 0, 128, 64, SH110X_WHITE);
  oled.setTextSize(2); oled.setCursor(20, 24);
  if (winner == 1) oled.print("X WINS"); else if (winner == 2) oled.print("O WINS"); else if (winner == 3) oled.print("TIED");
  oled.display(); delay(2000);
}

void playNumberGuess() {
  int randomNum = random(10);
  oled.clearDisplay();
  oled.drawRect(0, 0, 128, 64, SH110X_WHITE);
  oled.drawCircle(64, 28, 20, SH110X_WHITE);
  oled.drawCircle(64, 28, 14, SH110X_WHITE);
  oled.drawLine(64, 0, 64, 8, SH110X_WHITE);
  oled.drawLine(64, 48, 64, 60, SH110X_WHITE);
  oled.drawLine(36, 28, 44, 28, SH110X_WHITE);
  oled.drawLine(84, 28, 92, 28, SH110X_WHITE);
  
  oled.setTextColor(SH110X_WHITE);
  oled.setCursor(22, 54); oled.print("RADAR: GUESS 0-9");
  oled.display();

  char entered = keypad.waitForKey();
  oled.clearDisplay(); oled.drawRect(0, 0, 128, 64, SH110X_WHITE);
  oled.setCursor(18, 24);
  oled.setTextSize(2);
  if ((entered - '0') == randomNum) oled.print("MATCH!");
  else oled.print("FAILED");
  oled.setTextSize(1);
  oled.setCursor(28, 48); oled.print("Target: "); oled.print(randomNum);
  oled.display();
  delay(2000);
}

void playReact() {
  int randTime = random(2000, 5000);
  int randX = random(0, 4); 
  int randY = random(0, 4);
  
  oled.clearDisplay(); oled.drawRect(0, 0, 128, 64, SH110X_WHITE);
  oled.setTextSize(2); oled.setCursor(10, 24); oled.print("STAND BY.."); oled.display();
  delay(randTime);
  
  oled.clearDisplay(); oled.fillRect(0, 0, 128, 64, SH110X_WHITE);
  long startTime = millis();
  oled.setTextColor(SH110X_BLACK);
  oled.setCursor(20, 16); oled.print("PRESS:");
  oled.setTextSize(3); oled.setCursor(54, 34); oled.print(keys[randX][randY]);
  oled.display();
  
  char pressedKey = keypad.waitForKey();
  int reactionTime = millis() - startTime;
  oled.setTextColor(SH110X_WHITE);

  oled.clearDisplay(); oled.drawRect(0, 0, 128, 64, SH110X_WHITE);
  oled.setTextSize(2); oled.setCursor(16, 12);
  if(pressedKey == keys[randX][randY] && reactionTime < 2000) oled.print("SUCCESS!");
  else if(reactionTime > 2000) oled.print("TOO SLOW");
  else oled.print("MISFIRE!");
  
  oled.setTextSize(1); oled.setCursor(28, 40); oled.print(reactionTime); oled.print(" ms");
  oled.display(); delay(2000);
}

// =====================================================
// Live Camera & Aesthetically Remodeled Web UI 
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
<title>EngMechaffine Command HUD</title>
<style>
  @import url('https://fonts.googleapis.com/css2?family=Share+Tech+Mono&display=swap');
  :root { --bg: #090a0f; --panel: #11141c; --accent: #00e5ff; --accent-glow: rgba(0, 229, 255, 0.4); --alert: #ff2a55; --caffeine: #f6ad55; --text: #d0d4dc; }
  * { box-sizing: border-box; touch-action: manipulation; }
  body { margin: 0; font-family: 'Share Tech Mono', monospace; background: var(--bg); color: var(--text); padding: 20px; overflow-x: hidden; }
  
  /* Scanline Overlay for Mecha Vibe */
  body::after { content:""; position:fixed; top:0; left:0; width:100vw; height:100vh; background:linear-gradient(rgba(18,16,16,0) 50%, rgba(0,0,0,0.25) 50%), linear-gradient(90deg, rgba(255,0,0,0.06), rgba(0,255,0,0.02), rgba(0,0,255,0.06)); background-size:100% 4px, 3px 100%; z-index:999; pointer-events:none; }

  .grid-container { max-width: 1000px; margin: 0 auto; display: grid; gap: 25px; grid-template-columns: 1fr; position: relative; z-index: 10; }
  @media(min-width: 768px) { .grid-container { grid-template-columns: 2fr 1fr; } }
  
  h1 { text-align: center; color: var(--accent); letter-spacing: 4px; text-transform: uppercase; border-bottom: 1px solid var(--accent); padding-bottom: 10px; margin-top: 5px; text-shadow: 0 0 10px var(--accent-glow); }
  h1 span { color: var(--caffeine); text-shadow: 0 0 10px rgba(246, 173, 85, 0.4); }
  
  .hud-panel { background: linear-gradient(135deg, var(--panel) 0%, #0c0e14 100%); border: 1px solid #2a3143; border-top: 2px solid var(--accent); border-radius: 4px; padding: 20px; box-shadow: 0 8px 30px rgba(0,0,0,0.5); position: relative; }
  .hud-panel::before { content:''; position:absolute; top:-2px; left:-2px; width:20px; height:20px; border-top:2px solid var(--accent); border-left:2px solid var(--accent); }
  
  .video-wrap { position: relative; border-radius: 2px; overflow: hidden; border: 1px solid #334; aspect-ratio: 4/3; background: #000; box-shadow: inset 0 0 20px #000; }
  .video-wrap img { width: 100%; height: 100%; object-fit: cover; filter: contrast(1.1) saturate(1.1); }
  .badge { position: absolute; top: 12px; left: 12px; background: rgba(0,0,0,0.8); border-left: 3px solid var(--alert); color: var(--alert); padding: 4px 10px; font-size: 0.85rem; letter-spacing: 1px; animation: blink 2s infinite; }
  @keyframes blink { 0%, 100% { opacity: 1; } 50% { opacity: 0.5; } }
  
  /* Orbital Joystick Design */
  .joystick-container { display: flex; flex-direction: column; align-items: center; justify-content: center; padding: 30px 0; }
  .orbital-ring { display: flex; align-items: center; justify-content: center; padding: 20px; border-radius: 50%; border: 1px dashed #3a4556; box-shadow: 0 0 40px rgba(0, 229, 255, 0.05); }
  
  .dpad { display: grid; grid-template-columns: 65px 65px 65px; grid-template-rows: 65px 65px 65px; gap: 10px; }
  .dpad button { background: #171b26; border: 1px solid #2a3143; color: var(--accent); font-size: 1.5rem; font-weight: bold; border-radius: 50%; cursor: pointer; user-select: none; transition: 0.15s cubic-bezier(0.4, 0, 0.2, 1); box-shadow: inset 0 0 10px rgba(0,0,0,0.5); }
  .dpad button:active { background: var(--accent); color: #000; transform: scale(0.9); box-shadow: 0 0 20px var(--accent-glow); border-color: var(--accent); }
  
  .btn-up { grid-column: 2; grid-row: 1; }
  .btn-left { grid-column: 1; grid-row: 2; }
  .btn-stop { grid-column: 2; grid-row: 2; background: #1a0f14 !important; border-color: #4a1520 !important; color: var(--alert) !important; border-radius: 8px !important; }
  .btn-stop:active { background: var(--alert) !important; color: #000 !important; box-shadow: 0 0 20px rgba(255, 42, 85, 0.5) !important; }
  .btn-right { grid-column: 3; grid-row: 2; }
  .btn-down { grid-column: 2; grid-row: 3; }

  .telemetry { display: grid; grid-template-columns: repeat(3, 1fr); gap: 10px; margin-top: 20px; text-align: center; }
  .t-box { background: rgba(0,0,0,0.3); border: 1px solid #232836; padding: 10px; border-radius: 4px; }
  .t-val { font-size: 1.3rem; color: var(--accent); font-weight: bold; margin-bottom: 4px; text-shadow: 0 0 5px var(--accent-glow); }
  .t-lbl { font-size: 0.7rem; color: #69748c; letter-spacing: 1px; }
</style>
</head>
<body>
<div class="grid-container">
  
  <div class="hud-panel">
    <h1>SYS // ENG<span>MECHA</span>FFINE</h1>
    <div class="video-wrap">
      <span class="badge">REC // ONLINE</span>
      <img id="stream" src="" alt="camera stream">
    </div>
    <div class="telemetry">
      <div class="t-box"><div class="t-val" id="temp">--&deg;</div><div class="t-lbl">CORE TEMP</div></div>
      <div class="t-box"><div class="t-val" id="hum">--%</div><div class="t-lbl">HUMIDITY</div></div>
      <div class="t-box"><div class="t-val" id="time">--:--</div><div class="t-lbl">SYS CLOCK</div></div>
    </div>
  </div>

  <div class="hud-panel joystick-container">
    <h3 style="color: var(--text); margin-top:0; letter-spacing: 2px;">ORBITAL NAV</h3>
    <div class="orbital-ring">
      <div class="dpad">
        <button class="btn-up" data-dir="F">&#9650;</button>
        <button class="btn-left" data-dir="L">&#9664;</button>
        <button class="btn-stop" data-dir="S">&#9724;</button>
        <button class="btn-right" data-dir="R">&#9654;</button>
        <button class="btn-down" data-dir="B">&#9660;</button>
      </div>
    </div>
  </div>

</div>

<script>
  document.getElementById('stream').src = 'http://' + window.location.hostname + ':81/stream';

  const buttons = document.querySelectorAll('.dpad button');
  function sendCommand(dir) { fetch('/move?dir=' + dir).catch(err => console.error(err)); }

  buttons.forEach(btn => {
    btn.addEventListener('pointerdown', (e) => { e.preventDefault(); sendCommand(btn.getAttribute('data-dir')); });
    btn.addEventListener('pointerup', (e) => { e.preventDefault(); sendCommand('S'); });
    btn.addEventListener('pointerleave', (e) => { sendCommand('S'); });
  });

  function refreshStatus() {
    fetch('/status').then(r => r.json()).then(d => {
      document.getElementById('temp').textContent = d.temp.toFixed(1) + '\u00B0';
      document.getElementById('hum').textContent = d.humidity.toFixed(0) + '%';
      document.getElementById('time').textContent = d.time;
    }).catch(e => {});
  }
  setInterval(refreshStatus, 2000); refreshStatus();
</script>
</body>
</html>
)HTMLPAGE";

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, (const char *)INDEX_HTML, strlen(INDEX_HTML));
}
static esp_err_t move_handler(httpd_req_t *req) {
  char query[32];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    if (strstr(query, "dir=F")) moveForward();
    else if (strstr(query, "dir=B")) moveBackward();
    else if (strstr(query, "dir=L")) turnLeft();
    else if (strstr(query, "dir=R")) turnRight();
    else stopMotors();
  }
  httpd_resp_set_type(req, "application/json"); return httpd_resp_send(req, "{\"status\":\"ok\"}", -1);
}
static esp_err_t status_handler(httpd_req_t *req) {
  float t = dht.readTemperature(); float h = dht.readHumidity();
  char timeStr[16] = "--:--";
  struct tm timeinfo;
  if (WiFi.status() == WL_CONNECTED && getLocalTime(&timeinfo, 100)) strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
  char buf[128]; snprintf(buf, sizeof(buf), "{\"temp\":%.1f,\"humidity\":%.1f,\"time\":\"%s\"}", isnan(t) ? 0.0 : t, isnan(h) ? 0.0 : h, timeStr);
  httpd_resp_set_type(req, "application/json"); return httpd_resp_send(req, buf, strlen(buf));
}

static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL; esp_err_t res = ESP_OK; size_t jpgLen = 0; uint8_t *jpgBuf = NULL; char partBuf[64];
  res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) { res = ESP_FAIL; } 
    else {
      if (fb->format != PIXFORMAT_JPEG) {
        bool converted = frame2jpg(fb, 80, &jpgBuf, &jpgLen);
        esp_camera_fb_return(fb); fb = NULL;
        if (!converted) res = ESP_FAIL;
      } else { jpgLen = fb->len; jpgBuf = fb->buf; }
    }
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (res == ESP_OK) { size_t hlen = snprintf(partBuf, sizeof(partBuf), STREAM_PART, (unsigned)jpgLen); res = httpd_resp_send_chunk(req, partBuf, hlen); }
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)jpgBuf, jpgLen);
    
    if (fb) { esp_camera_fb_return(fb); fb = NULL; jpgBuf = NULL; } else if (jpgBuf) { free(jpgBuf); jpgBuf = NULL; }
    if (res != ESP_OK) break;
  }
  return res;
}

void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG(); config.server_port = 80; config.ctrl_port = 32768;
  httpd_uri_t index_uri = { "/", HTTP_GET, index_handler, NULL };
  httpd_uri_t status_uri = { "/status", HTTP_GET, status_handler, NULL };
  httpd_uri_t move_uri = { "/move", HTTP_GET, move_handler, NULL };
  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri); httpd_register_uri_handler(camera_httpd, &status_uri); httpd_register_uri_handler(camera_httpd, &move_uri);
  }
  config.server_port = 81; config.ctrl_port = 32769;
  httpd_uri_t stream_uri = { "/stream", HTTP_GET, stream_handler, NULL };
  if (httpd_start(&stream_httpd, &config) == ESP_OK) httpd_register_uri_handler(stream_httpd, &stream_uri);
}

bool initCamera() {
  camera_config_t config; config.ledc_channel = LEDC_CHANNEL_0; config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM; config.pin_d2 = Y4_GPIO_NUM; config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM; config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM; config.pin_pclk = PCLK_GPIO_NUM; config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM; config.pin_sscb_scl = SIOC_GPIO_NUM; config.pin_pwdn = PWDN_GPIO_NUM; config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000; config.pixel_format = PIXFORMAT_JPEG; config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  if (psramFound()) { config.frame_size = FRAMESIZE_SVGA; config.jpeg_quality = 12; config.fb_count = 2; config.fb_location = CAMERA_FB_IN_PSRAM; } 
  else { config.frame_size = FRAMESIZE_QVGA; config.jpeg_quality = 15; config.fb_count = 1; config.fb_location = CAMERA_FB_IN_DRAM; }
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) return false;
  sensor_t *s = esp_camera_sensor_get();
  if (s != NULL && s->id.PID == OV3660_PID) { s->set_vflip(s, 1); s->set_brightness(s, 1); s->set_saturation(s, -2); }
  return true;
}

// =====================================================
// OLED GUI Logic
// =====================================================
void playCameraFeed() {
  oled.clearDisplay(); oled.drawRect(0, 0, 128, 64, SH110X_WHITE);
  oled.setCursor(4, 4); oled.print("LIVE CAMERA FEED"); oled.drawLine(4, 13, 124, 13, SH110X_WHITE);
  if (!cameraReady) {
    oled.setCursor(4, 24); oled.print("Camera error."); oled.setCursor(4, 50); oled.print("Press * to exit"); oled.display();
    char k; do { k = keypad.waitForKey(); } while (k != '*'); return;
  }
  oled.setCursor(4, 18); oled.print("Browser IP:"); oled.setCursor(4, 30);
  if (WiFi.status() == WL_CONNECTED) oled.print(WiFi.localIP()); else oled.print("WiFi offline");
  oled.setCursor(4, 54); oled.print("Press * to go back"); oled.display();
  char k; do { k = keypad.waitForKey(); } while (k != '*');
}

void showMinigamesMenu() {
  bool inMenu = true;
  while (inMenu) {
    oled.clearDisplay(); oled.drawRect(0, 0, 128, 64, SH110X_WHITE);
    oled.drawLine(0, 12, 128, 12, SH110X_WHITE);
    oled.setCursor(30, 2); oled.print("GAMES MENU");
    oled.setCursor(4, 18); oled.print("1-Radar Guess");
    oled.setCursor(4, 30); oled.print("2-Tic-Tac-Toe");
    oled.setCursor(4, 42); oled.print("3-React Fast");
    oled.setCursor(4, 54); oled.print("*-Back");
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

void showAppsMenu() {
  bool inMenu = true;
  while (inMenu) {
    oled.clearDisplay(); oled.drawRect(0, 0, 128, 64, SH110X_WHITE);
    oled.drawLine(0, 12, 128, 12, SH110X_WHITE);
    oled.setCursor(34, 2); oled.print("APPS MENU");
    
    oled.setCursor(4, 18); oled.print("1-Minigames");
    oled.setCursor(4, 30); oled.print("2-Camera Status");
    oled.setCursor(4, 54); oled.print("*-Return to Clock");
    oled.display();

    char choice = keypad.waitForKey();
    switch (choice) {
      case '1': showMinigamesMenu(); break;
      case '2': playCameraFeed(); break;
      case '*': inMenu = false; break;
    }
  }
}

void showClockScreen() {
  oled.clearDisplay();
  
  // Mecha-style border framing
  oled.drawRect(0, 0, 128, 64, SH110X_WHITE);
  oled.drawLine(0, 14, 128, 14, SH110X_WHITE);
  oled.drawLine(0, 48, 128, 48, SH110X_WHITE);
  oled.fillRect(0, 0, 128, 14, SH110X_WHITE);
  
  oled.setTextColor(SH110X_BLACK);
  oled.setCursor(18, 4); oled.print("SYS.IDLE // TIME");
  oled.setTextColor(SH110X_WHITE);

  struct tm timeinfo;
  if (WiFi.status() == WL_CONNECTED && getLocalTime(&timeinfo, 100)) {
    char timeStr[10]; strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo);
    char secStr[4];   strftime(secStr, sizeof(secStr), ":%S", &timeinfo);

    oled.setTextSize(3); oled.setCursor(16, 22); oled.print(timeStr);
    oled.setTextSize(1); oled.setCursor(102, 36); oled.print(secStr);
  } else {
    oled.setTextSize(2); oled.setCursor(32, 24); oled.print("--:--");
    oled.setTextSize(1);
  }

  float temp = dht.readTemperature(); float humid = dht.readHumidity();
  oled.setCursor(4, 52); if (!isnan(temp)) { oled.print(temp, 1); oled.print("C"); } else oled.print("--C");
  oled.setCursor(60, 52); oled.print("[#] MENU");
  oled.setCursor(100, 52); if (!isnan(humid)) { oled.print(humid, 1); oled.print("%"); } else oled.print("--%");
  oled.display();
}

bool connectWiFi() {
  oled.clearDisplay(); oled.drawRect(0,0,128,64,SH110X_WHITE);
  oled.setCursor(10, 24); oled.print("INITIALIZING..."); oled.display();
  WiFi.mode(WIFI_STA); WiFi.begin(ssid, password);
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < WIFI_TIMEOUT_MS) delay(400);
  
  oled.clearDisplay(); oled.drawRect(0,0,128,64,SH110X_WHITE); oled.setCursor(4, 24);
  if (WiFi.status() == WL_CONNECTED) { oled.print("NETWORK ONLINE"); oled.setCursor(4, 38); oled.print(WiFi.localIP()); } 
  else { oled.print("NETWORK OFFLINE"); oled.setCursor(4, 38); oled.print("Local Mode Active"); }
  oled.display(); delay(1500);
  return WiFi.status() == WL_CONNECTED;
}

void setup() {
  Serial.begin(115200);
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  pinMode(MOTOR_IN1, OUTPUT); pinMode(MOTOR_IN2, OUTPUT); pinMode(MOTOR_IN3, OUTPUT); pinMode(MOTOR_IN4, OUTPUT);
  stopMotors();

  Wire.begin(I2C_SDA, I2C_SCL);
  if (!oled.begin(OLED_I2C_ADDRESS, true)) for (;;);
  oled.clearDisplay(); oled.setTextColor(SH110X_WHITE); oled.setTextSize(1);
  
  dht.begin(); keypad.begin();

  if (connectWiFi()) configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  cameraReady = initCamera();
  if (cameraReady) startCameraServer();
  
  // Force immediate clock draw
  lastClockUpdate = 0; 
}

void loop() {
  char key = keypad.getKey();
  
  if (key == '#') {
    showAppsMenu();
    lastClockUpdate = 0; // Force immediate redraw of clock upon return
  }
  
  // Non-blocking clock update (every 1 second)
  if (millis() - lastClockUpdate > 1000) {
    showClockScreen();
    lastClockUpdate = millis();
  }
}