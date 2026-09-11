#include <Wire.h>
#include <Keypad.h>
#include <Keypad_I2C.h>
#include <LiquidCrystal_I2C.h>

// ---- I2C addresses ----
#define I2CADDR   0x20   // PCF8574 address driving the keypad
#define LCD_ADDR  0x27   // I2C LCD backpack address (0x27 or 0x3F)

// ---- Keypad layout (4x4) ----
const byte ROWS = 4;
const byte COLS = 4;

char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

// PCF8574 pin connections
byte rowPins[ROWS] = {0, 1, 2, 3};   // P0 - P3
byte colPins[COLS] = {4, 5, 6, 7};   // P4 - P7

Keypad_I2C kpd(makeKeymap(keys), rowPins, colPins, ROWS, COLS, I2CADDR, PCF8574);
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);

void setup() {
  Serial.begin(9600);
  Wire.begin();

  kpd.begin();

  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("4x4 Keypad Test");
  lcd.setCursor(0, 1);
  lcd.print("Press a key...");

  Serial.println("4x4 Keypad + I2C LCD test ready.");
}

void loop() {
  char key = kpd.getKey();

  if (key) {
    Serial.print("Key pressed: ");
    Serial.println(key);

    lcd.setCursor(0, 1);
    lcd.print("Key: ");
    lcd.print(key);
    lcd.print("        "); // Clear leftover characters
  }
}