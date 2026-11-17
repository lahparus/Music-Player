// Display test: cycles the screen through red, green and blue with text.
// Rename this to main.cpp and put it in the src/ folder of your PlatformIO project.
//
// Wiring (shield pin -> ESP32 pin):
//   LCD_D0 -> GPIO22    LCD_D4 -> GPIO17 (TX2)
//   LCD_D1 -> GPIO13    LCD_D5 -> GPIO16 (RX2)
//   LCD_D2 -> GPIO26    LCD_D6 -> GPIO27
//   LCD_D3 -> GPIO25    LCD_D7 -> GPIO14
//   LCD_CS -> GPIO33    LCD_RS -> GPIO21
//   LCD_WR -> GPIO4     LCD_RST -> GPIO32
//   LCD_RD -> 3V3       5V -> VIN       GND -> GND
// The pin numbers themselves are set in platformio.ini (build_flags).

#include <Arduino.h>
#include <TFT_eSPI.h>

TFT_eSPI tft = TFT_eSPI();

void showColor(uint16_t color, const char *name) {
  tft.fillScreen(color);
  tft.setTextColor(TFT_WHITE, color);
  tft.setTextSize(3);
  tft.setCursor(20, 100);
  tft.print(name);
  Serial.println(name);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("Display test starting...");

  tft.init();
  tft.setRotation(2);  // 1 = landscape (320 x 240)

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(20, 10);
  tft.println("Text 1");
  tft.setCursor(30, 200);
  tft.println("Text 2");
  delay(6000);
}

void loop() {
  showColor(TFT_RED, "Red");
  delay(1000);
  showColor(TFT_GREEN, "Green");
  delay(1000);
  showColor(TFT_BLUE, "Blue");
  delay(1000);
}