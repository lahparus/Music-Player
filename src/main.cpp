// Music player skeleton: UI + Bluetooth audio together.
// Each fake "track" plays a different steady tone, so you can hear the player
// working. Real music from the SD card replaces the tones later.
// Rename this to main.cpp and put it in the src/ folder of your PlatformIO project.
//
// Controls use the BOOT button on the ESP32 (GPIO0):
//   Track list:   short press = move down,   long press (hold ~0.6 s) = play selected
//   Now playing:  short press = pause/play,  long press = back to track list
//
// Don't hold BOOT while pressing RST or plugging in power.

#include <Arduino.h>
#include <TFT_eSPI.h>
#include "BluetoothA2DPSource.h"

// ---------- settings ----------
// Name (or part of the name) of your Bluetooth speaker. Put it in pairing mode,
// run once, and read its exact name in the Serial Monitor ("Found Bluetooth device:").
const char *TARGET_NAME = "UE BOOM 2";

const int BUTTON_PIN = 0;               // BOOT button
const uint32_t DEBOUNCE_MS = 30;
const uint32_t LONG_PRESS_MS = 600;
const int BT_VOLUME = 127;               // 0 to 127
const int LED_PIN = 2;                         // optional: built-in LED to show Bluetooth status

// Fake tracks. Each plays its own pitch (in Hz). Later these come from the SD card.
const char *titles[] = {"test tone 1", "test tone 2", "test tone 3",
                        "test tone 4", "secret test tone 5 ohhhh"};
const int durations[] = {210, 185, 243, 198, 225};      // seconds
const float tones[] = {262.0, 330.0, 392.0, 440.0, 523.0};
const int NUM_TRACKS = 5;

// Progress bar geometry
const int BAR_X = 20, BAR_Y = 150, BAR_W = 280, BAR_H = 16;

// ---------- objects ----------
TFT_eSPI tft = TFT_eSPI();
BluetoothA2DPSource a2dp_source;

// ---------- player state ----------
enum ScreenId { SCREEN_LIST, SCREEN_NOW };
ScreenId screen = SCREEN_LIST;
int selected = 0;       // highlighted row in the list
int playingTrack = 0;   // track that is loaded
bool playing = false;
int elapsed = 0;        // seconds into the current track
uint32_t lastTick = 0;
bool btConnected = false;

// Shared with the audio callback, which runs in a different task.
// "volatile" tells the compiler these can change at any moment.
volatile bool audioOn = false;
volatile float audioFreq = 440.0;
volatile uint32_t dataCalls = 0;  // counts how often the library asks for audio

// ---------- Bluetooth ----------
bool isTarget(const char *ssid, esp_bd_addr_t address, int rssi) {
  Serial.print("Found Bluetooth device: ");
  Serial.println(ssid);
  return strstr(ssid, TARGET_NAME) != nullptr;
}

// The library calls this whenever it needs more audio (44,100 samples/second,
// left and right). When paused we send silence so the connection stays alive.
int32_t get_data_frames(Frame *frame, int32_t frame_count) {
  static float angle = 0.0;
  float delta = 2.0 * PI * audioFreq / 44100.0;
  bool on = audioOn;

  for (int i = 0; i < frame_count; i++) {
    int16_t s = on ? (int16_t)(3000.0 * sin(angle)) : 0;  // 3000 = quiet (max 32767)
    frame[i].channel1 = s;  // left
    frame[i].channel2 = s;  // right
    angle += delta;
    if (angle > 2.0 * PI) angle -= 2.0 * PI;
  }
  dataCalls++;
  return frame_count;
}

// ---------- button ----------
// Returns 0 = nothing, 1 = short press, 2 = long press (reported on release).
int readButton() {
  static bool lastRaw = HIGH;
  static bool stable = HIGH;
  static uint32_t lastChange = 0;
  static uint32_t pressedAt = 0;

  bool raw = digitalRead(BUTTON_PIN);
  uint32_t now = millis();

  if (raw != lastRaw) {
    lastChange = now;
    lastRaw = raw;
  }
  if (now - lastChange > DEBOUNCE_MS && raw != stable) {
    stable = raw;
    if (stable == LOW) {
      pressedAt = now;
    } else {
      return (now - pressedAt >= LONG_PRESS_MS) ? 2 : 1;
    }
  }
  return 0;
}

// ---------- drawing ----------
void formatTime(int seconds, char *buf) {
  snprintf(buf, 8, "%d:%02d", seconds / 60, seconds % 60);
}

void drawBtStatus() {
  tft.setTextSize(1);
  tft.setTextColor(btConnected ? TFT_GREEN : TFT_YELLOW, TFT_BLACK);
  tft.drawString(btConnected ? "BT: connected" : "BT: searching", 220, 12);
}

void drawList() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("Tracks", 10, 8);

  for (int i = 0; i < NUM_TRACKS; i++) {
    int y = 36 + i * 32;
    bool isSelected = (i == selected);
    uint16_t bg = isSelected ? TFT_NAVY : TFT_BLACK;
    uint16_t fg = isSelected ? TFT_WHITE : TFT_LIGHTGREY;
    tft.fillRect(0, y, 320, 30, bg);
    tft.setTextColor(fg, bg);
    tft.drawString(titles[i], 14, y + 7);
    if (i == playingTrack && playing) {
      tft.setTextColor(TFT_GREEN, bg);
      tft.drawString(">", 290, y + 7);
    }
  }

  tft.setTextSize(1);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("Click: next    Hold: play", 10, 225);
  drawBtStatus();
}

void drawStatus() {
  tft.setTextSize(2);
  tft.setTextColor(playing ? TFT_GREEN : TFT_YELLOW, TFT_BLACK);
  tft.drawString(playing ? "PLAYING" : "PAUSED ", 20, 200);  // trailing space clears old text
}

void drawProgress() {
  int inner = BAR_W - 4;
  int fill = (long)elapsed * inner / durations[playingTrack];
  tft.fillRect(BAR_X + 2, BAR_Y + 2, fill, BAR_H - 4, TFT_GREEN);
  tft.fillRect(BAR_X + 2 + fill, BAR_Y + 2, inner - fill, BAR_H - 4, TFT_BLACK);

  char cur[8], total[8];
  formatTime(elapsed, cur);
  formatTime(durations[playingTrack], total);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(cur, BAR_X, BAR_Y + 24);
  tft.drawString(total, BAR_X + BAR_W - 48, BAR_Y + 24);
}

void drawNowPlaying() {
  tft.fillScreen(TFT_BLACK);

  tft.setTextSize(1);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  char line[24];
  snprintf(line, sizeof(line), "Track %d of %d", playingTrack + 1, NUM_TRACKS);
  tft.drawString(line, 20, 12);

  tft.setTextSize(3);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(titles[playingTrack], 20, 50);

  tft.drawRect(BAR_X, BAR_Y, BAR_W, BAR_H, TFT_WHITE);
  drawProgress();
  drawStatus();

  tft.setTextSize(1);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("Click: pause/play    Hold: back", 10, 225);
  drawBtStatus();
}

// ---------- main program ----------
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  tft.init();
  tft.setRotation(1);
  digitalWrite(LED_PIN, LOW);
  drawList();

  // Start Bluetooth last. It scans in the background while the screen works.
  a2dp_source.set_ssid_callback(isTarget);
  a2dp_source.set_data_callback_in_frames(get_data_frames);
  a2dp_source.set_volume(BT_VOLUME);
  a2dp_source.start();
}

void loop() {
  // 1) Handle button presses
  int press = readButton();
  if (press == 1) {  // short press
    if (screen == SCREEN_LIST) {
      selected = (selected + 1) % NUM_TRACKS;
      drawList();
    } else {
      playing = !playing;
      lastTick = millis();
      drawStatus();
    }
  } else if (press == 2) {  // long press
    if (screen == SCREEN_LIST) {
      playingTrack = selected;
      elapsed = 0;
      playing = true;
      lastTick = millis();
      screen = SCREEN_NOW;
      drawNowPlaying();
    } else {
      selected = playingTrack;
      screen = SCREEN_LIST;
      drawList();
    }
  }

  // 2) Advance the playback clock once per second
  if (playing && millis() - lastTick >= 1000) {
    lastTick += 1000;
    elapsed++;
    if (elapsed >= durations[playingTrack]) {  // track finished: next one
      playingTrack = (playingTrack + 1) % NUM_TRACKS;
      elapsed = 0;
      if (screen == SCREEN_NOW) drawNowPlaying();
    } else if (screen == SCREEN_NOW) {
      drawProgress();
    }
  }

  // 3) Tell the audio side what to play
  audioOn = playing;
  audioFreq = tones[playingTrack];

  // 4) Check the Bluetooth link. is_connected() can keep saying "yes" after the
  //    speaker drops, so we also check that the library is still asking us for
  //    audio. While the link is healthy that happens about 100 times a second.
  static uint32_t lastCheck = 0;
  static uint32_t lastCalls = 0;
  static bool streamAlive = false;
  static bool everConnected = false;
  static uint32_t troubleSince = 0;

  if (millis() - lastCheck >= 1000) {
    if (btConnected) digitalWrite(LED_PIN, streamAlive ? HIGH : LOW);
    else digitalWrite(LED_PIN, LOW);
    lastCheck = millis();
    uint32_t calls = dataCalls;
    streamAlive = (calls != lastCalls);
    lastCalls = calls;
    Serial.printf("is_connected=%d  audio_flowing=%d\n",
                  a2dp_source.is_connected() ? 1 : 0, streamAlive ? 1 : 0);
  }

  bool linkOk = a2dp_source.is_connected() && streamAlive;

  if (linkOk) {
    everConnected = true;
    troubleSince = 0;
  } else if (everConnected) {
    // The link worked before and now it doesn't. Give it 10 seconds to recover,
    // then restart the ESP32 so it scans for the speaker again.
    if (troubleSince == 0) {
      troubleSince = millis();
    } else if (millis() - troubleSince > 10000) {
      Serial.println("Bluetooth link lost - restarting...");
      delay(200);
      ESP.restart();
    }
  }

  if (linkOk != btConnected) {  // update the screen label only when it changes
    btConnected = linkOk;
    digitalWrite(LED_PIN, btConnected ? HIGH : LOW);
    drawBtStatus();
  }
}
