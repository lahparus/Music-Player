#include <Arduino.h>

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("================================");
    Serial.println("       MUSIC PLAYER");
    Serial.println("================================");
    Serial.println("ESP32 is working!");
}

void loop() {
  Serial.println("Music Player is running...");
  delay(1000);
}