// Music player skeleton: display + Bluetooth audio + 3 buttons + status LED.
// Each fake "track" plays a different steady tone. Real music from the SD card
// replaces the tones later.
// Rename this to main.cpp and put it in the src/ folder of your PlatformIO project.
//
// BUTTONS (each wired between its pin and GND):
//   PLAY  (BOOT button, GPIO0)  short = select / play-pause     hold = back to list
//   PREV  (GPIO12)              short = up / previous track     hold = volume down
//   NEXT  (GPIO15)              short = down / next track       hold = volume up
//
// LED: GPIO2 (the blue LED on most ESP32 boards) lights up while Bluetooth is connected.
//
// Don't hold BOOT while pressing RST or plugging in power.

#include <Arduino.h>
#include <TFT_eSPI.h>
#include "BluetoothA2DPSource.h"

// ---------- settings ----------
// Put the names (or part of the names) of your Bluetooth speakers/headphones here.
// The ESP32 scans, and connects to the FIRST device it finds whose name contains
// any of these. The order of this list does not set priority.
const char *TARGET_NAMES[] = {"UE BOOM 2", "AirFall"};
const int NUM_TARGETS = sizeof(TARGET_NAMES) / sizeof(TARGET_NAMES[0]);

const int PIN_PLAY = 0;     // BOOT button
const int PIN_PREV = 12;
const int PIN_NEXT = 15;
const int PIN_LED = 2;      // Bluetooth-connected LED

const uint32_t DEBOUNCE_MS = 30;
const uint32_t LONG_PRESS_MS = 300;   // hold this long = "hold"
const uint32_t REPEAT_MS = 150;       // while still holding, repeat this often

const int START_VOLUME = 0;          // 0 to 127
const int MAX_VOLUME = 100;           // volume limit (keeps things safe)
const int VOLUME_STEP = 6;

// Fake tracks. Each plays its own pitch (in Hz). Later these come from the SD card.
const char *titles[] = {"Midnight Drive", "Paper Planes", "Slow Burn",
                        "City Lights", "Static Dreams"};
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
int volume = START_VOLUME;

// Shared with the audio callback, which runs in a different task.
// "volatile" tells the compiler these can change at any moment.
volatile bool audioOn = false;
volatile float audioFreq = 440.0;
volatile uint32_t dataCalls = 0;  // counts how often the library asks for audio

// ---------- Bluetooth ----------
// Called once for every Bluetooth device found while scanning.
// Return true to connect to it, false to keep looking.
bool isTarget(const char *ssid, esp_bd_addr_t address, int rssi) {
  Serial.print("Found Bluetooth device: ");
  Serial.println(ssid);
  for (int i = 0; i < NUM_TARGETS; i++) {
    if (strstr(ssid, TARGET_NAMES[i]) != nullptr) {
      Serial.print("  -> match, connecting to: ");
      Serial.println(ssid);
      return true;
    }
  }
  return false;
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

// ---------- buttons ----------
struct Button {
  uint8_t pin;
  bool lastRaw;        // last reading of the pin
  bool stable;         // debounced state (HIGH = released, LOW = pressed)
  uint32_t lastChange; // when the raw reading last changed
  uint32_t pressedAt;  // when the button went down
  uint32_t lastRepeat; // when the last repeat event fired
  bool holdFired;      // true once a "hold" has been reported for this press
};

Button btnPlay = {PIN_PLAY, HIGH, HIGH, 0, 0, 0, false};
Button btnPrev = {PIN_PREV, HIGH, HIGH, 0, 0, 0, false};
Button btnNext = {PIN_NEXT, HIGH, HIGH, 0, 0, 0, false};

enum ButtonEvent {
  EV_NONE,
  EV_SHORT,   // quick press, reported when released
  EV_HOLD,    // held down for LONG_PRESS_MS (reported once)
  EV_REPEAT   // still held: fires every REPEAT_MS
};

ButtonEvent readButton(Button &b) {
  bool raw = digitalRead(b.pin);
  uint32_t now = millis();

  if (raw != b.lastRaw) {  // pin changed: restart the debounce timer
    b.lastChange = now;
    b.lastRaw = raw;
  }
  if (now - b.lastChange > DEBOUNCE_MS && raw != b.stable) {
    b.stable = raw;
    if (b.stable == LOW) {  // button went down
      b.pressedAt = now;
      b.holdFired = false;
    } else {                // button came back up
      return b.holdFired ? EV_NONE : EV_SHORT;
    }
  }
  if (b.stable == LOW) {    // still being held
    if (!b.holdFired && now - b.pressedAt >= LONG_PRESS_MS) {
      b.holdFired = true;
      b.lastRepeat = now;
      return EV_HOLD;
    }
    if (b.holdFired && now - b.lastRepeat >= REPEAT_MS) {
      b.lastRepeat = now;
      return EV_REPEAT;
    }
  }
  return EV_NONE;
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

void drawVolume() {
  char buf[16];
  snprintf(buf, sizeof(buf), "Vol: %3d%%   ", volume * 100 / 127);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(buf, 220, 26);
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
      tft.drawString(">", 290, y + 7);  // marks the track that is playing
    }
  }

  tft.setTextSize(1);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("Prev/Next: move   Play: select   Hold Prev/Next: volume", 10, 225);
  drawBtStatus();
  drawVolume();
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
  tft.drawString("Play: pause   Prev/Next: track   Hold Play: list", 10, 225);
  drawBtStatus();
  drawVolume();
}

// ---------- player actions ----------
void startTrack(int index) {
  playingTrack = index;
  elapsed = 0;
  playing = true;
  lastTick = millis();
}

void changeVolume(int delta) {
  volume = constrain(volume + delta, 0, MAX_VOLUME);
  a2dp_source.set_volume(volume);
  drawVolume();
}

void handleButtons() {
  ButtonEvent ePlay = readButton(btnPlay);
  ButtonEvent ePrev = readButton(btnPrev);
  ButtonEvent eNext = readButton(btnNext);

  // PLAY button
  if (ePlay == EV_SHORT) {
    if (screen == SCREEN_LIST) {
      startTrack(selected);
      screen = SCREEN_NOW;
      drawNowPlaying();
    } else {
      playing = !playing;
      lastTick = millis();
      drawStatus();
    }
  } else if (ePlay == EV_HOLD && screen == SCREEN_NOW) {
    selected = playingTrack;
    screen = SCREEN_LIST;
    drawList();
  }

  // PREV button
  if (ePrev == EV_SHORT) {
    if (screen == SCREEN_LIST) {
      selected = (selected + NUM_TRACKS - 1) % NUM_TRACKS;
      drawList();
    } else if (elapsed > 3) {  // more than 3 s in: restart this track
      elapsed = 0;
      drawProgress();
    } else {
      startTrack((playingTrack + NUM_TRACKS - 1) % NUM_TRACKS);
      selected = playingTrack;
      drawNowPlaying();
    }
  } else if (ePrev == EV_HOLD || ePrev == EV_REPEAT) {
    changeVolume(-VOLUME_STEP);
  }

  // NEXT button
  if (eNext == EV_SHORT) {
    if (screen == SCREEN_LIST) {
      selected = (selected + 1) % NUM_TRACKS;
      drawList();
    } else {
      startTrack((playingTrack + 1) % NUM_TRACKS);
      selected = playingTrack;
      drawNowPlaying();
    }
  } else if (eNext == EV_HOLD || eNext == EV_REPEAT) {
    changeVolume(VOLUME_STEP);
  }
}

// Advance the playback clock once per second.
void tickClock() {
  if (playing && millis() - lastTick >= 1000) {
    lastTick += 1000;
    elapsed++;
    if (elapsed >= durations[playingTrack]) {  // track finished: next one
      startTrack((playingTrack + 1) % NUM_TRACKS);
      if (screen == SCREEN_NOW) drawNowPlaying();
      else drawList();
    } else if (screen == SCREEN_NOW) {
      drawProgress();
    }
  }
}

// Check the Bluetooth link. is_connected() can keep saying "yes" after the
// speaker drops, so we also check that the library is still asking us for audio.
void checkBluetooth() {
  static uint32_t lastCheck = 0;
  static uint32_t lastCalls = 0;
  static bool streamAlive = false;
  static bool everConnected = false;
  static uint32_t troubleSince = 0;

  if (millis() - lastCheck >= 1000) {
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
    // then restart the ESP32 so it scans again.
    if (troubleSince == 0) {
      troubleSince = millis();
    } else if (millis() - troubleSince > 10000) {
      Serial.println("Bluetooth link lost - restarting...");
      delay(200);
      ESP.restart();
    }
  }

  if (linkOk != btConnected) {  // update screen label and LED only when it changes
    btConnected = linkOk;
    digitalWrite(PIN_LED, btConnected ? HIGH : LOW);
    drawBtStatus();
  }
}

// ---------- main program ----------
void setup() {
  Serial.begin(115200);

  pinMode(PIN_PLAY, INPUT_PULLUP);  // buttons: pressed = LOW
  pinMode(PIN_PREV, INPUT_PULLUP);
  pinMode(PIN_NEXT, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  tft.init();
  tft.setRotation(1);
  drawList();

  // Start Bluetooth last. It scans in the background while the screen works.
  a2dp_source.set_ssid_callback(isTarget);
  a2dp_source.set_data_callback_in_frames(get_data_frames);
  a2dp_source.set_volume(volume);
  a2dp_source.set_auto_reconnect(true);
  a2dp_source.start();
}

void loop() {
  handleButtons();
  tickClock();

  // Tell the audio side what to play
  audioOn = playing;
  audioFreq = tones[playingTrack];

  checkBluetooth();
}