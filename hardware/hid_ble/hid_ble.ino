// HID over Bluetooth: an ESP32-S3 (M5Stack StickS3, battery powered) that is the phone's Bluetooth
// mouse + keyboard. No USB cable, no AirPlay: on iOS 27 the app captures the screen itself
// (ScreenCaptureKit). The app's command service and the system HID share one BLE link to the phone;
// a Mac can connect as a second central to drive the same commands wirelessly.
//
// Same `<id> <command>` protocol as hid_bridge (docs/ble-protocol.md), answered "ok" or "err <why>":
//   ping | home | move <dx> <dy> | press | release | click
//   scroll <v> <h> | type <ascii text> | key <combo, e.g. cmd+h, enter> | homescreen | button <1-3>
// homescreen clicks the middle button: map it to Home once, in Settings > Accessibility > Touch > AssistiveTouch >
// Devices > (this board) > Customize Additional Buttons (iOS ignores cmd+h and the AC Home key from it). It then sends
// AC Home too, for hosts that honour it. `button N` clicks button N (2 = right, 3 = middle), e.g. to register it there.
// Board commands: app/log (relay between centrals), unpair. stream/snap answer an error: no video here.

#include <esp_task_wdt.h>
#include "ble_link.h"
#include "ble_hid.h"

const int STEP = 10;      // max pointer delta per report while moving
const int STEP_MS = 8;    // report interval; constant pace keeps iOS acceleration predictable
const int HOME_REPORTS = 50;
const int KEY_MS = 12;    // gap between key down and up, and between characters
#ifndef DEBUG_SERIAL
#define DEBUG_SERIAL 0
#endif

const int WATCHDOG_MS = 8000;
const uint16_t AC_HOME = 0x0223;  // HID consumer page: AC Home

// HID keyboard modifier bits and usages (HID Usage Tables, page 0x07).
enum : uint8_t { MOD_CTRL = 0x01, MOD_SHIFT = 0x02, MOD_ALT = 0x04, MOD_GUI = 0x08 };

static uint8_t buttons = 0;

void mouseReport(int8_t dx, int8_t dy, int8_t wheel = 0, int8_t pan = 0) { ble_hid_mouse(buttons, dx, dy, wheel, pan); }

void movePaced(long dx, long dy) {
  while (dx || dy) {
    int sx = constrain(dx, -STEP, STEP), sy = constrain(dy, -STEP, STEP);
    mouseReport(sx, sy);
    dx -= sx;
    dy -= sy;
    delay(STEP_MS);
  }
}

// US layout: usage for an ASCII character, and whether it needs shift. 0 = not typeable.
uint8_t asciiUsage(char c, bool &shift) {
  static const char *shifted = ")!@#$%^&*(";  // shift + 0..9
  static const char plain[] = "-=[]\\;'`,./";
  static const char upper[] = "_+{}|:\"~<>?";
  static const uint8_t symUsage[] = {0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38};
  shift = false;
  if (c >= 'a' && c <= 'z') return 0x04 + (c - 'a');
  if (c >= 'A' && c <= 'Z') { shift = true; return 0x04 + (c - 'A'); }
  if (c >= '1' && c <= '9') return 0x1E + (c - '1');
  if (c == '0') return 0x27;
  if (c == ' ') return 0x2C;
  if (c == '\n') return 0x28;
  if (c == '\t') return 0x2B;
  for (int i = 0; i < 10; i++)
    if (c == shifted[i]) { shift = true; return i == 0 ? 0x27 : 0x1E + i - 1; }
  for (int i = 0; plain[i]; i++) {
    if (c == plain[i]) return symUsage[i];
    if (c == upper[i]) { shift = true; return symUsage[i]; }
  }
  return 0;
}

void tap(uint8_t mods, uint8_t usage) {
  uint8_t keys[6] = {usage, 0, 0, 0, 0, 0};
  ble_hid_keys(mods, keys);
  delay(KEY_MS);
  const uint8_t none[6] = {0};
  ble_hid_keys(0, none);
  delay(KEY_MS);
}

// Returns how many characters had no key (non-ASCII: use the phone's input method instead).
int typeText(const String &s) {
  int skipped = 0;
  for (size_t i = 0; i < s.length(); i++) {
    bool shift;
    uint8_t u = asciiUsage(s[i], shift);
    if (!u) { skipped++; continue; }
    tap(shift ? MOD_SHIFT : 0, u);
  }
  return skipped;
}

// One part of a combo: a modifier bit, or a key usage.
bool comboPart(const String &k, uint8_t &mods, uint8_t &usage) {
  if (k == "cmd") { mods |= MOD_GUI; return true; }
  if (k == "ctrl") { mods |= MOD_CTRL; return true; }
  if (k == "alt" || k == "option") { mods |= MOD_ALT; return true; }
  if (k == "shift") { mods |= MOD_SHIFT; return true; }
  static const struct { const char *name; uint8_t usage; } named[] = {
      {"enter", 0x28}, {"esc", 0x29}, {"backspace", 0x2A}, {"tab", 0x2B}, {"space", 0x2C},
      {"delete", 0x4C}, {"right", 0x4F}, {"left", 0x50}, {"down", 0x51}, {"up", 0x52},
      {"pageup", 0x4B}, {"pagedown", 0x4E}, {"home", 0x4A}, {"end", 0x4D},
  };
  for (auto &n : named)
    if (k == n.name) { usage = n.usage; return true; }
  if (k.length() != 1) return false;
  bool shift;
  usage = asciiUsage(k[0], shift);
  if (shift) mods |= MOD_SHIFT;
  return usage != 0;
}

bool pressCombo(String combo) {
  uint8_t mods = 0, usage = 0;
  combo += '+';
  for (int i; (i = combo.indexOf('+')) >= 0; combo = combo.substring(i + 1))
    if (!comboPart(combo.substring(0, i), mods, usage)) return false;
  uint8_t keys[6] = {usage, 0, 0, 0, 0, 0};
  ble_hid_keys(mods, keys);
  delay(30);
  const uint8_t none[6] = {0};
  ble_hid_keys(0, none);
  return true;
}

void releaseAll() {
  buttons = 0;
  if (!ble_hid_connected()) return;
  mouseReport(0, 0);
  const uint8_t none[6] = {0};
  ble_hid_keys(0, none);
  ble_hid_consumer(0);
}

void clickButton(uint8_t bit) {
  buttons |= bit; mouseReport(0, 0); delay(60);
  buttons &= ~bit; mouseReport(0, 0);
}

String run(const String &line) {
  int sp = line.indexOf(' ');
  String cmd = sp < 0 ? line : line.substring(0, sp);
  String arg = sp < 0 ? "" : line.substring(sp + 1);
  long a = 0, b = 0;
  sscanf(arg.c_str(), "%ld %ld", &a, &b);

  if (cmd == "ping") return "ok";
  if (!ble_hid_connected()) return "err phone bluetooth mouse not connected (pair it under AssistiveTouch > Devices)";
  if (cmd == "home") {  // slam the pointer into the top-left corner
    for (int i = 0; i < HOME_REPORTS; i++) { mouseReport(-127, -127); delay(STEP_MS); }
    return "ok";
  }
  if (cmd == "move") { movePaced(a, b); return "ok"; }
  if (cmd == "press") { buttons |= 1; mouseReport(0, 0); return "ok"; }
  if (cmd == "release") { buttons &= ~1; mouseReport(0, 0); return "ok"; }
  if (cmd == "click") { clickButton(1); return "ok"; }
  if (cmd == "scroll") { mouseReport(0, 0, a, b); return "ok"; }
  if (cmd == "type") {
    int skipped = typeText(arg);
    return skipped ? "err skipped " + String(skipped) + " non-ASCII chars" : "ok";
  }
  if (cmd == "key") return pressCombo(arg) ? "ok" : "err unknown key";
  if (cmd == "button") {
    if (a < 1 || a > 3) return "err usage: button 1|2|3";
    clickButton(1 << (a - 1));
    return "ok";
  }
  if (cmd == "homescreen") {
    clickButton(4);
    ble_hid_consumer(AC_HOME); delay(40); ble_hid_consumer(0);
    return "ok";
  }
  return "err unknown command";
}

const char *resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_TASK_WDT: case ESP_RST_INT_WDT: case ESP_RST_WDT: return "watchdog";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_SW: return "software";
    case ESP_RST_USB: return "usb";
    default: return "other";
  }
}

// The app reads `usb` as "can the hands act": here that is the Bluetooth HID subscription.
const char *hidState() { return ble_hid_connected() ? "ok" : "off"; }

String board(const String &line, const String &id) {
  int sp = line.indexOf(' ');
  String cmd = sp < 0 ? line : line.substring(0, sp);
  String arg = sp < 0 ? "" : line.substring(sp + 1);
  if (cmd == "stream" || cmd == "snap" || cmd == "bench" || cmd == "res" || cmd == "fps")
    return "err no video on this board (the app captures the screen)";
  if (cmd == "app" || cmd == "log") {  // relay between centrals (Mac <-> app); each side ignores lines it doesn't handle
    ble_reply(cmd + " " + arg);
    return "ok";
  }
  if (cmd == "unpair") {
    releaseAll();
    ble_reply(id + " ok");
    ble_unpair();
    return "";
  }
  return run(line);
}

String statusJson() {
  char s[256];
  snprintf(s, sizeof s,
           "{\"up\":%lu,\"rst\":\"%s\",\"heap\":%u,\"usb\":\"%s\",\"hid\":\"ble\",\"net\":\"none\",\"ap\":\"idle\","
           "\"ble\":{\"mtu\":%u,\"phy\":%u,\"n\":%d}}",
           millis(), resetReason(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), hidState(), ble_mtu(),
           ble_phy(), ble_peers());
  return s;
}

void statusTask(void *) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    if (ble_connected()) ble_status(statusJson());
  }
}

void setup() {
  Serial.begin(115200);
  esp_task_wdt_config_t wdt = {.timeout_ms = WATCHDOG_MS, .idle_core_mask = 0, .trigger_panic = true};
  esp_task_wdt_reconfigure(&wdt);
  esp_task_wdt_add(NULL);
  ble_begin("ESP32 Agent " + String((uint32_t)(ESP.getEfuseMac() >> 24) & 0xFFFFFF, HEX));
  xTaskCreatePinnedToCore(statusTask, "status", 4096, NULL, 1, NULL, 1);
  Serial.printf("boot hid_ble: free internal heap %u, reset %s\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), resetReason());
}

void loop() {
  static bool hands = false;
  esp_task_wdt_reset();
  if (hands && !ble_hid_connected()) buttons = 0;  // a gesture cut off mid-way must not stay held
  if (!hands && ble_hid_connected()) Serial.println("hid: phone subscribed");
  hands = ble_hid_connected();

  String line;
  if (Serial.available()) {  // USB serial takes the same `<id> <command>` lines, for debugging from a Mac
    line = Serial.readStringUntil('\n');
    line.trim();
    int sp = line.indexOf(' ');
    String id = sp < 0 ? line : line.substring(0, sp);
    String reply = board(sp < 0 ? "" : line.substring(sp + 1), id);
    if (reply.length()) Serial.println(id + " " + reply);
    return;
  }
  if (!ble_pop_cmd(line, 50)) return;
  int sp = line.indexOf(' ');
  String id = sp < 0 ? line : line.substring(0, sp);
  String rest = sp < 0 ? "" : line.substring(sp + 1);
  rest.trim();
  String reply = id.toInt() || id == "0" ? board(rest, id) : "err missing request id";
  if (reply.length()) ble_reply(id + " " + reply);
#if DEBUG_SERIAL
  Serial.printf("%s -> %s\n", line.c_str(), reply.c_str());
#endif
}
