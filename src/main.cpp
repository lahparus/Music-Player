// Music player: plays real WAV and MP3 files from the SD card over Bluetooth.
// Rename this to main.cpp and put it in the src/ folder of your PlatformIO project.
//
// HOW AUDIO FLOWS:
//   SD card --(reader task)--> ring buffer --(Bluetooth callback)--> speaker
// The reader task reads the file in the background and keeps the ring buffer
// topped up. The Bluetooth library takes audio out of it whenever it needs more.
// Because the two sides are separate, drawing on the screen never interrupts the sound.
//
// SD CARD (shield pin -> ESP32 pin): SD_SCK->18, SD_DI->23, SD_DO->19, SD_SS->5
//
// BUTTONS (each wired between its pin and GND):
//   PLAY  (BOOT button, GPIO0)  short = select / play-pause     hold = back to list
//                               (hold in the track list = forget last Bluetooth device, rescan)
//   PREV  (GPIO12)              short = up / previous track     hold = volume down
//   NEXT  (GPIO15)              short = down / next track       hold = volume up
//
// LED: GPIO2 (the blue LED on most ESP32 boards) lights up while Bluetooth is connected.
//
// Don't hold BOOT while pressing RST or plugging in power.

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <TFT_eSPI.h>
#include "BluetoothA2DPSource.h"
#include "MP3DecoderHelix.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

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
const int SD_CS_PIN = 5;    // SD card chip select

const uint32_t DEBOUNCE_MS = 30;
const uint32_t LONG_PRESS_MS = 600;   // hold this long = "hold"
const uint32_t REPEAT_MS = 150;       // while still holding, repeat this often

const int START_VOLUME = 30;          // 0 to 127. Real music is louder than the old test tones!
const int MAX_VOLUME = 100;           // volume limit (keeps things safe)
const int VOLUME_STEP = 6;

// MP3 playback uses about 40 KB of the ESP32's memory, which Bluetooth also needs.
// If playback is unstable, set this to false: WAV files then get all that memory back.
const bool ENABLE_MP3 = true;

// true:  on startup, call the last Bluetooth device directly (it doesn't need to be
//        in pairing mode). Hold Play in the track list to forget it and scan instead.
// false: always scan for any device in TARGET_NAMES (it must be in pairing mode).
const bool AUTO_RECONNECT = true;

const int MAX_TRACKS = 40;            // how many files we keep in the list
const int VISIBLE_ROWS = 5;           // rows shown on the list screen

// Progress bar geometry
const int BAR_X = 20, BAR_Y = 150, BAR_W = 280, BAR_H = 16;

// ---------- objects ----------
TFT_eSPI tft = TFT_eSPI();
BluetoothA2DPSource a2dp_source;

// ---------- track list (filled from the SD card) ----------
String trackPath[MAX_TRACKS];   // e.g. "/My Song.wav"
String trackTitle[MAX_TRACKS];  // e.g. "My Song"
int numTracks = 0;
bool sdOk = false;

// ---------- player state (used by the main loop) ----------
enum ScreenId { SCREEN_LIST, SCREEN_NOW };
ScreenId screen = SCREEN_LIST;
int selected = 0;       // highlighted row in the list
int scrollTop = 0;      // first track shown on the list screen
int playingTrack = 0;   // track that is loaded
bool playing = false;   // what the user wants: play or pause
int elapsed = 0;        // seconds played so far
bool errorShown = false;
int shownDuration = -1; // track length currently drawn on screen
bool btConnected = false;
int volume = START_VOLUME;

// ---------- audio pipeline (shared between tasks) ----------
// One audio "frame" = one left sample + one right sample (16-bit each).
struct Stereo { int16_t l; int16_t r; };

const int RING_FRAMES = 3072;    // about 70 ms of audio (12 KB of memory)
const int CHUNK_FRAMES = 128;    // the reader works in chunks this big

Stereo ring[RING_FRAMES];        // the ring buffer
int ringHead = 0;                // where the reader writes next
int ringTail = 0;                // where Bluetooth reads next
int ringCount = 0;               // how many frames are waiting
SemaphoreHandle_t ringMutex;     // a lock: only one task touches the ring at a time

// Everything below is protected by the lock when it matters.
volatile uint32_t trackGeneration = 0;  // goes up every time a new track is requested
int requestedTrack = -1;                // which track the reader should open
volatile bool sourceEnded = false;      // reader has pushed the last frames of the file
volatile bool trackFinished = false;    // ring is empty and file has ended
volatile bool loadFailed = false;       // the file couldn't be played
char loadErrorText[32] = "";            // why
volatile uint32_t playedFrames = 0;     // frames actually sent to the speaker
volatile int trackDuration = 0;         // length in seconds (0 = unknown)

volatile bool audioOn = false;          // true while the user is playing
volatile uint32_t dataCalls = 0;        // counts how often Bluetooth asks for audio

// Called from the main loop whenever a new track (or a restart) is wanted.
void requestTrack(int idx) {
  xSemaphoreTake(ringMutex, portMAX_DELAY);
  requestedTrack = idx;
  trackGeneration = trackGeneration + 1;
  ringHead = 0;
  ringTail = 0;
  ringCount = 0;                 // throw away any old audio
  sourceEnded = false;
  trackFinished = false;
  loadFailed = false;
  playedFrames = 0;
  trackDuration = 0;
  xSemaphoreGive(ringMutex);
}

// Print how much memory is left. "largest block" is the biggest single piece
// that can still be allocated, which is what matters for big buffers.
void printHeap(const char *label) {
  Serial.printf("[heap] %-24s free=%u  largest block=%u\n", label,
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
}

// ---------- SD card ----------
bool isMusicFile(const String &name) {
  String n = name;
  n.toLowerCase();
  return n.endsWith(".mp3") || n.endsWith(".wav");
}

// Read the root folder of the SD card and build the track list.
void scanSD() {
  numTracks = 0;
  sdOk = SD.begin(SD_CS_PIN);
  if (!sdOk) {
    Serial.println("SD init failed");
    return;
  }

  File root = SD.open("/");
  File entry = root.openNextFile();
  while (entry && numTracks < MAX_TRACKS) {
    String name = entry.name();
    int slash = name.lastIndexOf('/');
    String base = name.substring(slash + 1);  // file name without any folder part

    // Skip folders and hidden files (macOS makes junk files starting with ".")
    if (!entry.isDirectory() && !base.startsWith(".") && isMusicFile(base)) {
      trackPath[numTracks] = "/" + base;
      trackTitle[numTracks] = base.substring(0, base.lastIndexOf('.'));  // drop ".wav"
      numTracks++;
    }
    entry = root.openNextFile();
  }
  root.close();

  // Sort A-Z (ignoring upper/lower case) with a simple insertion sort.
  for (int i = 1; i < numTracks; i++) {
    String p = trackPath[i];
    String t = trackTitle[i];
    String key = t;
    key.toLowerCase();
    int j = i - 1;
    while (j >= 0) {
      String other = trackTitle[j];
      other.toLowerCase();
      if (other.compareTo(key) <= 0) break;
      trackPath[j + 1] = trackPath[j];
      trackTitle[j + 1] = trackTitle[j];
      j--;
    }
    trackPath[j + 1] = p;
    trackTitle[j + 1] = t;
  }

  Serial.printf("Found %d music files:\n", numTracks);
  for (int i = 0; i < numTracks; i++) Serial.println("  " + trackPath[i]);
}

// Cut long text so it fits on the screen.
String shortText(const String &s, int maxChars) {
  if ((int)s.length() <= maxChars) return s;
  return s.substring(0, maxChars - 2) + "..";
}

// ---------- reader task: SD card -> ring buffer ----------
// Only the reader task touches the SD card and these variables.
File wavFile;
uint32_t readerGen = 0;     // which request the reader is working on
volatile uint32_t readerStackFree = 0;  // lowest amount of unused stack the reader has seen (bytes)
uint32_t bytesLeft = 0;     // audio bytes still unread in the file
uint16_t wavChannels = 2;
uint32_t wavRate = 44100;
uint8_t srcBuf[512];        // chunk of raw file data
int srcLen = 0;
int srcPos = 0;

// Resampler state (only used if the file isn't 44,100 Hz)
Stereo resPrev, resNext;
float resFrac = 0.0f;
float resRatio = 1.0f;

struct WavInfo {
  uint16_t channels;
  uint32_t sampleRate;
  uint16_t bits;
  uint32_t dataBytes;
};

static uint32_t le32(const uint8_t *p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t le16(const uint8_t *p) {
  return p[0] | (p[1] << 8);
}

// Record why a file can't be played (unless a newer track was already requested).
void failLoad(const char *text) {
  Serial.print("Can't play: ");
  Serial.println(text);
  xSemaphoreTake(ringMutex, portMAX_DELAY);
  if (readerGen == trackGeneration) {
    strncpy(loadErrorText, text, sizeof(loadErrorText) - 1);
    loadErrorText[sizeof(loadErrorText) - 1] = 0;
    loadFailed = true;
  }
  xSemaphoreGive(ringMutex);
}

// Read the WAV header. On success the file is positioned at the first audio byte.
bool parseWav(File &f, WavInfo &info) {
  uint8_t hdr[12];
  if (f.read(hdr, 12) != 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
    failLoad("Not a valid WAV file");
    return false;
  }

  bool gotFmt = false;
  uint16_t fmtTag = 0;
  info.channels = 0;
  info.sampleRate = 0;
  info.bits = 0;
  info.dataBytes = 0;

  // A WAV file is a series of "chunks". We want the "fmt " chunk (the format)
  // and the "data" chunk (the audio). We skip any others.
  while (f.available()) {
    uint8_t ch[8];
    if (f.read(ch, 8) != 8) break;
    uint32_t size = le32(ch + 4);

    if (memcmp(ch, "fmt ", 4) == 0) {
      uint8_t fmt[16];
      if (size < 16 || f.read(fmt, 16) != 16) {
        failLoad("Bad WAV header");
        return false;
      }
      fmtTag = le16(fmt);
      info.channels = le16(fmt + 2);
      info.sampleRate = le32(fmt + 4);
      info.bits = le16(fmt + 14);
      uint32_t consumed = 16;
      if (fmtTag == 0xFFFE && size >= 26) {  // "extensible" WAV: real format is further in
        uint8_t ext[10];
        if (f.read(ext, 10) != 10) {
          failLoad("Bad WAV header");
          return false;
        }
        fmtTag = le16(ext + 8);
        consumed = 26;
      }
      uint32_t rest = size - consumed + (size & 1);
      if (rest > 0) f.seek(f.position() + rest);
      gotFmt = true;

    } else if (memcmp(ch, "data", 4) == 0) {
      if (!gotFmt) {
        failLoad("Bad WAV header");
        return false;
      }
      uint32_t left = f.size() - f.position();
      info.dataBytes = (size == 0 || size > left) ? left : size;

      if (fmtTag != 1 || info.bits != 16 || (info.channels != 1 && info.channels != 2) ||
          info.sampleRate < 8000 || info.sampleRate > 96000) {
        Serial.printf("WAV format: tag=%u, %u-bit, %u channels, %u Hz\n",
                      (unsigned)fmtTag, (unsigned)info.bits, (unsigned)info.channels,
                      (unsigned)info.sampleRate);
        failLoad("Unsupported WAV format");
        return false;
      }
      return true;

    } else {
      f.seek(f.position() + size + (size & 1));  // skip a chunk we don't need
    }
  }
  failLoad("No audio found in WAV");
  return false;
}

// Get the next frame straight from a WAV file (converts mono to stereo).
bool nextWavFrame(Stereo &out) {
  int bpf = wavChannels * 2;  // bytes per frame
  if (srcPos + bpf > srcLen) {  // buffer used up: read more from the card
    if (bytesLeft < (uint32_t)bpf) return false;
    uint32_t want = (bytesLeft < sizeof(srcBuf)) ? bytesLeft : sizeof(srcBuf);
    want -= want % bpf;
    int got = wavFile.read(srcBuf, want);
    if (got < bpf) return false;
    got -= got % bpf;
    srcLen = got;
    srcPos = 0;
    bytesLeft -= got;
  }
  int16_t a = (int16_t)(srcBuf[srcPos] | (srcBuf[srcPos + 1] << 8));
  int16_t b = a;
  if (wavChannels == 2) b = (int16_t)(srcBuf[srcPos + 2] | (srcBuf[srcPos + 3] << 8));
  srcPos += bpf;
  out.l = a;
  out.r = b;
  return true;
}

// ---- MP3 decoding ----
// The decoder is fed small pieces of the file. Each time it finishes decoding a
// frame it calls mp3Callback with the raw audio, which we collect in a small queue.
using namespace libhelix;

enum SourceType { SRC_WAV, SRC_MP3 };
SourceType srcType = SRC_WAV;

const int MP3_FIFO_FRAMES = 1536;   // room for one decoded MP3 frame (1152) plus a margin
Stereo mp3Fifo[MP3_FIFO_FRAMES];
int mp3FifoHead = 0;
int mp3FifoTail = 0;
int mp3FifoCount = 0;
bool mp3Eof = false;       // the whole file has been fed to the decoder
bool mp3Ready = false;     // decoder memory was allocated at startup (and stays allocated)
int mp3Rate = 0;           // sample rate reported by the decoder
int mp3Bitrate = 0;        // bits per second reported by the decoder
int mp3Chans = 0;          // channels reported by the decoder (1 = mono, 2 = stereo)

uint8_t mp3In[512];        // a piece of the file waiting to be fed to the decoder
int mp3InLen = 0;
int mp3InPos = 0;

// Called by the decoder with each decoded chunk. "len" counts samples across
// all channels, so a stereo chunk has two samples (left, right) per frame.
void mp3Callback(MP3FrameInfo &info, int16_t *pcm, size_t len, void *ref) {
  int ch = info.nChans;
  if (ch < 1) return;
  mp3Rate = info.samprate;
  mp3Bitrate = info.bitrate;
  mp3Chans = ch;
  for (size_t i = 0; i + ch <= len; i += ch) {
    if (mp3FifoCount >= MP3_FIFO_FRAMES) break;  // queue full (shouldn't happen)
    Stereo s;
    s.l = pcm[i];
    s.r = (ch >= 2) ? pcm[i + 1] : pcm[i];       // mono: use the same sound on both sides
    mp3Fifo[mp3FifoHead] = s;
    mp3FifoHead++;
    if (mp3FifoHead >= MP3_FIFO_FRAMES) mp3FifoHead = 0;
    mp3FifoCount++;
  }
}

MP3DecoderHelix mp3(mp3Callback);

// Feed the decoder one small piece of the file.
void feedMp3() {
  if (mp3InPos >= mp3InLen) {  // need more data from the card
    int got = wavFile.read(mp3In, sizeof(mp3In));
    if (got <= 0) {            // end of file: let the decoder finish what it has
      mp3.flush();
      mp3Eof = true;
      return;
    }
    mp3InLen = got;
    mp3InPos = 0;
  }
  int n = mp3InLen - mp3InPos;
  if (n > 64) n = 64;  // small pieces keep the decoded output per call small
  mp3.write(mp3In + mp3InPos, n);
  mp3InPos += n;
}

bool nextMp3Frame(Stereo &out) {
  int feeds = 0;
  while (mp3FifoCount == 0) {
    if (mp3Eof) return false;
    // A real frame needs only about 20 small pieces. If a lot more go in and nothing
    // comes out, the decoder is stuck: give up on this file instead of freezing.
    if (++feeds > 400) {
      mp3Eof = true;
      return false;
    }
    feedMp3();
  }
  out = mp3Fifo[mp3FifoTail];
  mp3FifoTail++;
  if (mp3FifoTail >= MP3_FIFO_FRAMES) mp3FifoTail = 0;
  mp3FifoCount--;
  return true;
}

// Close the open file. (The MP3 decoder stays allocated, so we never have to
// find a big block of memory for it again.)
void closeSource() {
  if (wavFile) wavFile.close();
}

// Get the next frame from whichever kind of file is open.
bool nextSourceFrame(Stereo &out) {
  return (srcType == SRC_MP3) ? nextMp3Frame(out) : nextWavFrame(out);
}

// Get the next frame at 44,100 Hz. Files at other rates are resampled
// with simple linear interpolation (blending between neighboring frames).
bool produceFrame(Stereo &out) {
  if (wavRate == 44100) return nextSourceFrame(out);

  while (resFrac >= 1.0f) {
    resPrev = resNext;
    if (!nextSourceFrame(resNext)) return false;
    resFrac -= 1.0f;
  }
  out.l = resPrev.l + (int16_t)((resNext.l - resPrev.l) * resFrac);
  out.r = resPrev.r + (int16_t)((resNext.r - resPrev.r) * resFrac);
  resFrac += resRatio;
  return true;
}

static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// Work out an MP3's length in seconds from the header of its first frame.
// Variable-bit-rate files carry a "Xing" or "VBRI" tag with the exact frame count.
// Without one we assume constant bit rate and use the file size. Returns 0 if
// the length can't be worked out. (Kept as its own function so its buffer is not
// on the stack while the decoder runs.)
int estimateMp3Seconds(File &f, uint32_t audioStart, uint32_t audioBytes) {
  static const uint16_t bitrateMpeg1[16] = {0, 32, 40, 48, 56, 64, 80, 96,
                                            112, 128, 160, 192, 224, 256, 320, 0};
  static const uint16_t bitrateMpeg2[16] = {0, 8, 16, 24, 32, 40, 48, 56,
                                            64, 80, 96, 112, 128, 144, 160, 0};
  static const uint32_t rateMpeg1[4] = {44100, 48000, 32000, 0};

  uint8_t buf[1024];
  f.seek(audioStart);
  int got = (int)f.read(buf, sizeof(buf));

  for (int i = 0; i + 4 <= got; i++) {  // find the first frame header
    if (buf[i] != 0xFF || (buf[i + 1] & 0xE0) != 0xE0) continue;
    int version = (buf[i + 1] >> 3) & 3;    // 3 = MPEG-1, 2 = MPEG-2, 0 = MPEG-2.5
    int layer = (buf[i + 1] >> 1) & 3;      // 1 = Layer III (MP3)
    int bitrateIndex = buf[i + 2] >> 4;
    int rateIndex = (buf[i + 2] >> 2) & 3;
    if (version == 1 || layer != 1 || bitrateIndex == 0 || bitrateIndex == 15 || rateIndex == 3) continue;

    bool mpeg1 = (version == 3);
    uint32_t rate = rateMpeg1[rateIndex];
    if (version == 2) rate /= 2;
    if (version == 0) rate /= 4;
    bool mono = ((buf[i + 3] >> 6) & 3) == 3;
    bool hasCrc = !(buf[i + 1] & 1);
    uint32_t bitrate = (uint32_t)(mpeg1 ? bitrateMpeg1[bitrateIndex] : bitrateMpeg2[bitrateIndex]) * 1000;
    uint32_t samplesPerFrame = mpeg1 ? 1152 : 576;

    // A Xing/Info tag sits right after the frame's side information.
    int sideInfo = mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
    int xingPos = i + 4 + (hasCrc ? 2 : 0) + sideInfo;
    if (xingPos + 12 <= got &&
        (memcmp(buf + xingPos, "Xing", 4) == 0 || memcmp(buf + xingPos, "Info", 4) == 0)) {
      uint32_t flags = be32(buf + xingPos + 4);
      uint32_t frames = (flags & 1) ? be32(buf + xingPos + 8) : 0;
      if (frames > 0) return (int)((uint64_t)frames * samplesPerFrame / rate);
    }
    // The other kind of tag: "VBRI" always sits 32 bytes after the header.
    int vbriPos = i + 4 + 32;
    if (vbriPos + 18 <= got && memcmp(buf + vbriPos, "VBRI", 4) == 0) {
      uint32_t frames = be32(buf + vbriPos + 14);
      if (frames > 0) return (int)((uint64_t)frames * samplesPerFrame / rate);
    }

    // No tag: assume constant bit rate.
    return (int)((uint64_t)audioBytes * 8 / bitrate);
  }
  return 0;
}

bool openMp3(int idx) {
  printHeap("opening MP3");
  srcType = SRC_MP3;
  wavFile = SD.open(trackPath[idx].c_str(), FILE_READ);
  if (!wavFile) {
    failLoad("Can't open file");
    return false;
  }

  // Skip the ID3 tag (song title, artist...) at the start of the file, if any.
  uint32_t audioStart = 0;
  uint8_t id3[10];
  if (wavFile.read(id3, 10) == 10 && memcmp(id3, "ID3", 3) == 0) {
    uint32_t tagSize = ((uint32_t)(id3[6] & 0x7F) << 21) | ((uint32_t)(id3[7] & 0x7F) << 14) |
                       ((uint32_t)(id3[8] & 0x7F) << 7) | (uint32_t)(id3[9] & 0x7F);
    audioStart = 10 + tagSize;
    if (id3[5] & 0x10) audioStart += 10;  // the tag has a footer
  }
  uint32_t audioBytes = (wavFile.size() > audioStart) ? (wavFile.size() - audioStart) : 0;
  int estimatedSeconds = estimateMp3Seconds(wavFile, audioStart, audioBytes);
  wavFile.seek(audioStart);  // back to the start of the audio, ready for decoding

  if (!mp3Ready) {
    failLoad(ENABLE_MP3 ? "MP3 decoder not available" : "MP3 support is turned off");
    wavFile.close();
    return false;
  }

  mp3.flush();  // finish off anything left over from the previous song...
  mp3FifoHead = 0;  // ...and throw away whatever that produced
  mp3FifoTail = 0;
  mp3FifoCount = 0;
  mp3InLen = 0;
  mp3InPos = 0;
  mp3Eof = false;
  mp3Rate = 0;
  mp3Bitrate = 0;

  // Decode until the first piece of audio comes out. That tells us the sample rate.
  // The limit and the short pauses keep this from hogging the processor if the file is bad.
  int tries = 0;
  while (mp3FifoCount == 0 && !mp3Eof && tries < 400) {
    feedMp3();
    tries++;
    if ((tries & 15) == 0) vTaskDelay(1);
  }
  if (mp3FifoCount == 0 || mp3Rate < 8000 || mp3Rate > 48000) {
    failLoad("Can't decode this MP3");
    closeSource();
    return false;
  }

  wavRate = mp3Rate;
  if (wavRate != 44100) {  // set up the resampler
    resRatio = wavRate / 44100.0f;
    resFrac = 0.0f;
    if (!nextSourceFrame(resPrev) || !nextSourceFrame(resNext)) {
      failLoad("MP3 has no audio");
      closeSource();
      return false;
    }
  }

  // Length: taken from the file's header (exact when it has a Xing/VBRI tag,
  // otherwise assumes constant bit rate). Fall back to the decoder's bit rate.
  trackDuration = estimatedSeconds;
  if (trackDuration == 0 && mp3Bitrate > 0) {
    trackDuration = (int)((uint64_t)audioBytes * 8 / (uint32_t)mp3Bitrate);
  }
  Serial.printf("Playing %s: MP3, %d Hz, %d channel(s), %d kbps, length ~%d s\n",
                trackPath[idx].c_str(), mp3Rate, mp3Chans, mp3Bitrate / 1000,
                (int)trackDuration);
  if (mp3Rate != 44100) {
    Serial.printf("  resampling %d Hz -> 44100 Hz (ratio %.4f)\n", mp3Rate, resRatio);
  }
  return true;
}

bool openWav(int idx) {
  srcType = SRC_WAV;
  wavFile = SD.open(trackPath[idx].c_str(), FILE_READ);
  if (!wavFile) {
    failLoad("Can't open file");
    return false;
  }

  WavInfo info;
  if (!parseWav(wavFile, info)) {
    wavFile.close();
    return false;
  }

  wavChannels = info.channels;
  wavRate = info.sampleRate;
  bytesLeft = info.dataBytes;
  srcLen = 0;
  srcPos = 0;

  if (wavRate != 44100) {  // set up the resampler
    resRatio = wavRate / 44100.0f;
    resFrac = 0.0f;
    if (!nextSourceFrame(resPrev) || !nextSourceFrame(resNext)) {
      failLoad("WAV has no audio");
      wavFile.close();
      return false;
    }
  }

  trackDuration = (int)(info.dataBytes / (info.channels * 2) / info.sampleRate);
  Serial.printf("Playing %s: %u Hz, %u channel(s), %d s\n", trackPath[idx].c_str(),
                (unsigned)wavRate, (unsigned)wavChannels, (int)trackDuration);
  return true;
}

// Open a track, choosing the right method for its file type.
bool openTrack(int idx) {
  String lower = trackPath[idx];
  lower.toLowerCase();
  if (lower.endsWith(".mp3")) return openMp3(idx);
  return openWav(idx);
}

int ringFreeFrames() {
  xSemaphoreTake(ringMutex, portMAX_DELAY);
  int f = RING_FRAMES - ringCount;
  xSemaphoreGive(ringMutex);
  return f;
}

// Add frames to the ring, unless a newer track was requested in the meantime.
void ringPush(uint32_t gen, const Stereo *src, int n, bool ended) {
  xSemaphoreTake(ringMutex, portMAX_DELAY);
  if (gen == trackGeneration) {
    for (int i = 0; i < n; i++) {
      ring[ringHead] = src[i];
      ringHead++;
      if (ringHead >= RING_FRAMES) ringHead = 0;
    }
    ringCount += n;
    if (ended) sourceEnded = true;
  }
  xSemaphoreGive(ringMutex);
}

void readerTask(void *param) {
  static Stereo chunk[CHUNK_FRAMES];
  uint32_t myGen = 0;
  bool fileOpen = false;

  for (;;) {
    // Has the main loop asked for a different track?
    xSemaphoreTake(ringMutex, portMAX_DELAY);
    uint32_t gen = trackGeneration;
    int idx = requestedTrack;
    xSemaphoreGive(ringMutex);

    if (gen != myGen) {
      myGen = gen;
      readerGen = gen;
      closeSource();
      fileOpen = false;
      if (idx >= 0 && idx < numTracks) fileOpen = openTrack(idx);
      continue;
    }

    if (!fileOpen || ringFreeFrames() < CHUNK_FRAMES) {
      vTaskDelay(pdMS_TO_TICKS(5));  // nothing to do right now
      continue;
    }

    int n = 0;
    while (n < CHUNK_FRAMES && produceFrame(chunk[n])) n++;
    bool ended = (n < CHUNK_FRAMES);
    ringPush(myGen, chunk, n, ended);
    readerStackFree = uxTaskGetStackHighWaterMark(nullptr);
    if (ended) {
      closeSource();
      fileOpen = false;
    }
  }
}

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

// The library calls this whenever it needs more audio (44,100 frames/second).
// We take frames out of the ring buffer. If the ring runs dry, or we're paused,
// the rest is filled with silence so the connection stays alive.
int32_t get_data_frames(Frame *frame, int32_t frame_count) {
  bool on = audioOn;
  int n = 0;

  xSemaphoreTake(ringMutex, portMAX_DELAY);
  if (on) {
    n = (ringCount < frame_count) ? ringCount : frame_count;
    for (int i = 0; i < n; i++) {
      frame[i].channel1 = ring[ringTail].l;  // left
      frame[i].channel2 = ring[ringTail].r;  // right
      ringTail++;
      if (ringTail >= RING_FRAMES) ringTail = 0;
    }
    ringCount -= n;
    playedFrames = playedFrames + n;
    if (sourceEnded && ringCount == 0) trackFinished = true;
  }
  xSemaphoreGive(ringMutex);

  for (int i = n; i < frame_count; i++) {
    frame[i].channel1 = 0;
    frame[i].channel2 = 0;
  }
  dataCalls = dataCalls + 1;
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

  if (numTracks == 0) {
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString(sdOk ? "No .mp3 or .wav files" : "SD card not found", 14, 60);
    tft.setTextSize(1);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString(sdOk ? "Put music files in the main folder of the card."
                        : "Check the SD wires and that the card is FAT32.", 14, 90);
    drawBtStatus();
    drawVolume();
    return;
  }

  // Position counter, e.g. "3/12"
  char pos[12];
  snprintf(pos, sizeof(pos), "%d/%d", selected + 1, numTracks);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString(pos, 110, 8);

  // Scroll the window so the highlighted row is always visible.
  if (selected < scrollTop) scrollTop = selected;
  if (selected >= scrollTop + VISIBLE_ROWS) scrollTop = selected - VISIBLE_ROWS + 1;

  for (int i = 0; i < VISIBLE_ROWS; i++) {
    int idx = scrollTop + i;
    if (idx >= numTracks) break;
    int y = 36 + i * 32;
    bool isSelected = (idx == selected);
    uint16_t bg = isSelected ? TFT_NAVY : TFT_BLACK;
    uint16_t fg = isSelected ? TFT_WHITE : TFT_LIGHTGREY;
    tft.fillRect(0, y, 320, 30, bg);
    tft.setTextColor(fg, bg);
    tft.drawString(shortText(trackTitle[idx], 22), 14, y + 7);
    if (idx == playingTrack && playing) {
      tft.setTextColor(TFT_GREEN, bg);
      tft.drawString(">", 296, y + 7);  // marks the track that is playing
    }
  }

  tft.setTextSize(1);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("Hold Play: forget last BT device, rescan", 10, 205);
  tft.drawString("Prev/Next: move   Play: select   Hold Prev/Next: volume", 10, 225);
  drawBtStatus();
  drawVolume();
}

void drawStatus() {
  tft.fillRect(20, 200, 300, 16, TFT_BLACK);
  tft.setTextSize(2);
  if (loadFailed) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString(loadErrorText, 20, 200);
  } else {
    tft.setTextColor(playing ? TFT_GREEN : TFT_YELLOW, TFT_BLACK);
    tft.drawString(playing ? "PLAYING" : "PAUSED", 20, 200);
  }
}

void drawProgress() {
  int duration = trackDuration;
  shownDuration = duration;

  int inner = BAR_W - 4;
  int fill = 0;
  if (duration > 0) {
    fill = (long)elapsed * inner / duration;
    if (fill > inner) fill = inner;
  }
  tft.fillRect(BAR_X + 2, BAR_Y + 2, fill, BAR_H - 4, TFT_GREEN);
  tft.fillRect(BAR_X + 2 + fill, BAR_Y + 2, inner - fill, BAR_H - 4, TFT_BLACK);

  char buf[8];
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  tft.fillRect(BAR_X, BAR_Y + 24, 60, 16, TFT_BLACK);
  formatTime(elapsed, buf);
  tft.drawString(buf, BAR_X, BAR_Y + 24);

  tft.fillRect(BAR_X + BAR_W - 60, BAR_Y + 24, 60, 16, TFT_BLACK);
  if (duration > 0) {
    formatTime(duration, buf);
    tft.drawString(buf, BAR_X + BAR_W - 60, BAR_Y + 24);
  } else {
    tft.drawString("--:--", BAR_X + BAR_W - 60, BAR_Y + 24);
  }
}

void drawNowPlaying() {
  tft.fillScreen(TFT_BLACK);

  tft.setTextSize(1);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  char line[24];
  snprintf(line, sizeof(line), "Track %d of %d", playingTrack + 1, numTracks);
  tft.drawString(line, 20, 12);

  // Short titles in big text, long titles in smaller text.
  const String &title = trackTitle[playingTrack];
  bool big = (title.length() <= 15);
  tft.setTextSize(big ? 3 : 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(shortText(title, big ? 15 : 25), 20, 50);

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
  errorShown = false;
  playing = true;
  requestTrack(index);  // tells the reader task to open this file
}

void changeVolume(int delta) {
  volume = constrain(volume + delta, 0, MAX_VOLUME);
  a2dp_source.set_volume(volume);
  drawVolume();
}

// Forget the last Bluetooth device and restart, so the player scans for any device
// in TARGET_NAMES instead of calling the last one. (A restart is the safest way to
// start a fresh scan when memory is this tight.)
void forgetBluetoothAndRestart() {
  Serial.println("Forgetting the last Bluetooth device and restarting...");
  a2dp_source.clean_last_connection();  // deletes the saved address

  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("Forgetting last device", 20, 80);
  tft.drawString("Release the button...", 20, 110);

  // Important: the BOOT button is also the ESP32's "upload mode" pin. If it is
  // still held down when the chip restarts, it would not run the player at all.
  while (digitalRead(PIN_PLAY) == LOW) delay(20);
  delay(300);
  ESP.restart();
}

void handleButtons() {
  ButtonEvent ePlay = readButton(btnPlay);
  ButtonEvent ePrev = readButton(btnPrev);
  ButtonEvent eNext = readButton(btnNext);

  // PLAY button
  if (ePlay == EV_SHORT) {
    if (screen == SCREEN_LIST) {
      if (numTracks > 0) {
        startTrack(selected);
        screen = SCREEN_NOW;
        drawNowPlaying();
      }
    } else {
      playing = !playing;
      drawStatus();
    }
  } else if (ePlay == EV_HOLD && screen == SCREEN_NOW) {
    selected = playingTrack;
    screen = SCREEN_LIST;
    drawList();
  } else if (ePlay == EV_HOLD && screen == SCREEN_LIST) {
    forgetBluetoothAndRestart();  // hold Play in the track list = search for a new device
  }

  // PREV button
  if (ePrev == EV_SHORT) {
    if (numTracks > 0) {
      if (screen == SCREEN_LIST) {
        selected = (selected + numTracks - 1) % numTracks;
        drawList();
      } else if (elapsed > 3) {  // more than 3 s in: restart this track
        startTrack(playingTrack);
        drawNowPlaying();
      } else {
        startTrack((playingTrack + numTracks - 1) % numTracks);
        selected = playingTrack;
        drawNowPlaying();
      }
    }
  } else if (ePrev == EV_HOLD || ePrev == EV_REPEAT) {
    changeVolume(-VOLUME_STEP);
  }

  // NEXT button
  if (eNext == EV_SHORT) {
    if (numTracks > 0) {
      if (screen == SCREEN_LIST) {
        selected = (selected + 1) % numTracks;
        drawList();
      } else {
        startTrack((playingTrack + 1) % numTracks);
        selected = playingTrack;
        drawNowPlaying();
      }
    }
  } else if (eNext == EV_HOLD || eNext == EV_REPEAT) {
    changeVolume(VOLUME_STEP);
  }
}

// Keep the screen in step with what the audio side is doing.
void updatePlayback() {
  if (numTracks == 0) return;

  // Elapsed time = how much audio has really been played.
  int e = (int)(playedFrames / 44100);
  if (e != elapsed) {
    elapsed = e;
    if (screen == SCREEN_NOW) drawProgress();
  }

  // The track length arrives shortly after a track starts.
  if (screen == SCREEN_NOW && trackDuration != shownDuration) drawProgress();

  // The file couldn't be played: stop and show why.
  if (loadFailed && !errorShown) {
    errorShown = true;
    playing = false;
    if (screen == SCREEN_NOW) drawStatus();
    else drawList();
  }

  // The song ended: go to the next one.
  if (trackFinished) {
    Serial.printf("Finished track %d: played %d s (expected ~%d s)\n", playingTrack + 1,
                  elapsed, (int)trackDuration);
    startTrack((playingTrack + 1) % numTracks);
    if (screen == SCREEN_NOW) drawNowPlaying();
    else drawList();
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
    Serial.printf("is_connected=%d  audio_flowing=%d  heap_free=%u  largest_block=%u  reader_stack_free=%u\n",
                  a2dp_source.is_connected() ? 1 : 0, streamAlive ? 1 : 0,
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
                  (unsigned)readerStackFree);
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
  delay(300);
  printHeap("boot");

  pinMode(PIN_PLAY, INPUT_PULLUP);  // buttons: pressed = LOW
  pinMode(PIN_PREV, INPUT_PULLUP);
  pinMode(PIN_NEXT, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  // SD chip select must be HIGH (deselected) until we talk to the card.
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Reading SD card...", 20, 100);

  scanSD();
  drawList();
  printHeap("after SD + screen");

  // The lock must exist before anything that uses the ring buffer starts.
  ringMutex = xSemaphoreCreateMutex();

  // Start the reader task on core 1 (the same core as loop(), but with higher priority).
  xTaskCreatePinnedToCore(readerTask, "reader", 8192, nullptr, 2, nullptr, 1);

  // Allocate the MP3 decoder now, while there is plenty of free memory and it
  // isn't fragmented yet. It stays allocated for as long as the player runs.
  if (ENABLE_MP3) {
    mp3Ready = mp3.begin();
    printHeap(mp3Ready ? "after MP3 decoder" : "MP3 decoder FAILED");
  } else {
    printHeap("MP3 turned off");
  }

  // Start Bluetooth last. It scans in the background while the screen works.
  a2dp_source.set_ssid_callback(isTarget);
  a2dp_source.set_data_callback_in_frames(get_data_frames);
  a2dp_source.set_auto_reconnect(AUTO_RECONNECT);  // call the last device directly if it's known
  a2dp_source.set_volume(volume);
  a2dp_source.start();
  printHeap("after Bluetooth start");
}

void loop() {
  handleButtons();
  updatePlayback();

  audioOn = playing;  // tell the audio side whether to play or stay silent

  checkBluetooth();
}