#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Keypad.h>
#include <Keypad_I2C.h>
#include <WiFi.h>
#include "time.h"
#include <DHT.h>

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

// =====================
// Time
// Egypt = UTC+3 currently
// =========================

const char* ntpServer = "pool.ntp.org";

const long gmtOffset_sec = 3 * 3600;
const int daylightOffset_sec = 0;

// =========================
// Shared I2C Pins
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
  int randomNum = random(10);
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
  oled.print("from 0-10");
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
}

// Decorative background for the XO game: just a frame (a fill tint would
// hide the marks on a monochrome screen, so it's left plain).
void drawXOBackground() {
  oled.drawRect(0, 0, 128, 64, SH110X_WHITE);
}

void playXO() {
  int winner;
  oled.clearDisplay();
  drawXOBackground();
  oled.display();
  int arrayX[10] = {};
  int arrayO[10] = {};

  while (!checkWinner(arrayX, arrayO))
  {
    drawXOGrid();
    oled.display();

    int pc_num = random(1, 10);
    while (taken(pc_num, arrayX, arrayO))
    {
      pc_num = random(1, 10);
    }
    drawX(pc_num);
    arrayX[pc_num] = pc_num;
    oled.display();
    winner = checkWinner(arrayX, arrayO);
    if (winner != 0)
      break;

    char your_turn = keypad.waitForKey();
    while (taken(your_turn - '0', arrayX, arrayO))
    {
      your_turn = keypad.waitForKey();
    }
    drawO(your_turn - '0');
    arrayO[your_turn - '0'] = (your_turn - '0');
    oled.display();
    winner = checkWinner(arrayX, arrayO);
  }

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
}

void playReact()
{
  int randTime = random(5000);
  int randX = random(1, 5);
  int randY = random(1, 5);
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
}

// Minigames submenu: pick Number Guess or XO
void showMinigamesMenu() {
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
  oled.setCursor(4, 10);
  oled.print("1-Number Guess");
  oled.setCursor(4, 24);
  oled.print("2-XO");
  oled.setCursor(4, 38);
  oled.print("3-React Fast");
  oled.display();

  char game = keypad.waitForKey();
  if (game == '1')
  {
    playNumberGuess();
  }
  else if (game == '2')
  {
    playXO();
  }
  else if(game == '3')
  {
    playReact();
  }
}

// =====================================================
// Camera feed placeholder
// =====================================================

void playCameraFeed() {
  oled.clearDisplay();
  oled.setCursor(0, 25);
  oled.print("Camera Feed");
  oled.setCursor(0, 35);
  oled.print("Not Available");
  oled.display();
}



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
}

// =====================================================
// Main menu
// =====================================================

void showMainMenu() {

  oled.clearDisplay();

  // Simple robot face (mini)
  oled.drawCircle(64, 16, 12, SH110X_WHITE);
  oled.fillCircle(59, 14, 2, SH110X_WHITE);
  oled.fillCircle(69, 14, 2, SH110X_WHITE);
  oled.drawLine(59, 20, 69, 20, SH110X_WHITE);
  oled.drawLine(64, 4, 64, 0, SH110X_WHITE);
  oled.fillCircle(64, 0, 1, SH110X_WHITE);

  // Text
  oled.setTextColor(SH110X_WHITE);
  oled.setTextSize(1);
  oled.setCursor(40, 32);
  oled.print("WELCOME");

  oled.setCursor(30, 42);
  oled.print("HAPPY ROBOT");
  oled.display();
  delay(2000);
  oled.clearDisplay();

  int w = oled.width();
  int h = oled.height();

  // Frame sized to the actual screen
  oled.drawRect(0, 0, w, h, SH110X_WHITE);

  // Title + underline
  oled.setTextColor(SH110X_WHITE);
  oled.setTextSize(1);
  oled.setCursor(32, 2);
  oled.print("MAIN MENU");
  oled.drawLine(4, 11, w - 4, 11, SH110X_WHITE);


  oled.setTextColor(SH110X_WHITE);
  oled.setCursor(4, 16);
  oled.print("1-Minigames");
  oled.setCursor(4, 28);
  oled.print("2-Camera Feed");
  oled.setCursor(4, 40);
  oled.print("3-Funny Faces");
  oled.display();

  char choice = keypad.waitForKey();
  if (choice == '1')
  {
    showMinigamesMenu();
  }
  else if (choice == '2')
  {
    playCameraFeed();
  }
  else if(choice == '3')
  {
    showFaces();
  }
}

void showLockScreen()
{
  // =========================
  // Connect to WiFi + get time
  // =========================
  oled.clearDisplay();
  oled.setTextColor(SH110X_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print("Connecting to WiFi...");
  oled.display();

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  // =========================
  // Lock screen background
  // =========================
  oled.clearDisplay();
  int w = oled.width();
  int h = oled.height();

  oled.drawRect(0, 0, w, h, SH110X_WHITE);

  oled.setTextColor(SH110X_WHITE);
  oled.setTextSize(1);
  oled.setCursor(w / 2 - 30, 2);
  oled.print("LOCK SCREEN");
  oled.drawLine(4, 11, w - 4, 11, SH110X_WHITE);

  // =========================
  // Live clock + temp/humidity, resized to sit inside the frame together
  // =========================
  char pressedKey = 0;
  while (pressedKey != '#')
  {
    oled.fillRect(1, 12, w - 2, h - 13, SH110X_BLACK);

    struct tm timeinfo;
    if (getLocalTime(&timeinfo))
    {
      char timeString[12];
      strftime(timeString, sizeof(timeString), "%I:%M:%S %p", &timeinfo);

      oled.setCursor(20, 16);
      oled.print(timeString);
    }

    float humid = dht.readHumidity();
    float temp = dht.readTemperature();

    oled.setCursor(4, 30);
    oled.print("Temp: ");
    oled.print(temp);
    oled.print((char)247);
    oled.print("C");

    oled.setCursor(4, 42);
    oled.print("Humidity: ");
    oled.print(humid);
    oled.print("%");

    oled.setCursor(4, 54);
    oled.print("Press # to proceed");

    oled.display();

    pressedKey = keypad.getKey();
    delay(2000);
  }

  showMainMenu();
}

// =====================================================
// Setup / Loop
// =====================================================

void setup()
{
  Serial.begin(115200);

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

}



void loop()
{
  char start = keypad.waitForKey();

  if (start == '#')
  {
      showLockScreen();
  }

}