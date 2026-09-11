bool connectWiFi() {
  oled.clearDisplay();
  oled.setTextColor(SH110X_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print("Connecting to WiFi");
  oled.display();

  // Clear any stuck WiFi credentials before starting
  WiFi.disconnect(true, true);
  delay(100);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  unsigned long startAttempt = millis();
  int dots = 0;
  
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < WIFI_TIMEOUT_MS)
  {
    delay(400);
    oled.setCursor((dots % 20) * 6, 12);
    oled.print(".");
    oled.display(); // Added this so the OLED actually updates the dots
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