// Spike: does iOS accept a USB touch screen, pen or absolute mouse? Standalone sketch; hardware/hid_bridge is
// untouched. One image holds all four report descriptors; the variant is chosen at boot (USB descriptors are
// built at runtime), so a whole pass runs from one plug-in. See README.md for how to run it and read results.
//
//   1 single-touch screen   2 multi-touch screen (Windows style)   3 pen   4 absolute mouse (control)
//
// Serial (USB CDC, only useful on the Mac):  status | v <1-4> (restart into a variant, idle) | arm [passes]
//   pure 1|0 (restart without / with the relative mouse: the mouse comes first in the report map, and a host may
//   then treat the whole device as a mouse and ignore the digitizer; pure mode has no control, so run it after a
//   normal pass has shown that USB works)
//
// Build with CDCOnBoot off (build.sh): with it on, the core calls USB.begin() before setup(), HID registration is
// then closed, and addDevice() silently drops everything added in setup(). The device must be added first.
//
// Safety: these reports click at absolute positions, so the sequence never runs on the Mac by accident. `arm`
// waits for the USB cable to be moved: it starts only after the host goes away and a new host appears. Any USB
// disconnect during a run disarms the board, and it disarms itself when the passes are done.

#include <Preferences.h>
#include "USB.h"
#include "USBHID.h"
#include "USBHIDMouse.h"
#include "tusb.h"

const uint8_t REPORT_ID = 10;          // free: USBHID.h takes 1-6
const uint8_t REPORT_ID_MAXCOUNT = 11; // variant 2's Feature report: contact count maximum
const uint16_t FULL = 32767;

const uint32_t START_WAIT_MS = 20000;  // after the host appears: time to unlock and open the test page
const uint32_t HOST_WAIT_MS = 30000;   // after a restart between variants
const uint32_t PASS_PAUSE_MS = 90000;  // between passes: time to switch AssistiveTouch off
const int LOOPS = 2;                   // sequence repeats per variant
const uint32_t POINT_GAP_MS = 2000;
const uint32_t HOLD_MS = 80;
const uint32_t LOOP_GAP_MS = 10000;

#define XY_ABS(max_lo, max_hi)                                          \
  0x05, 0x01,             /* Usage Page (Generic Desktop) */            \
  0x09, 0x30, 0x09, 0x31, /* Usage (X), Usage (Y) */                    \
  0x15, 0x00,             /* Logical Minimum (0) */                     \
  0x26, max_lo, max_hi,   /* Logical Maximum (32767) */                 \
  0x35, 0x00,             /* Physical Minimum (0) */                    \
  0x46, max_lo, max_hi,   /* Physical Maximum (32767) */                \
  0x75, 0x10, 0x95, 0x02, /* Report Size (16), Count (2) */             \
  0x81, 0x02              /* Input (Data, Var, Abs) */

const uint8_t DESC_TOUCH[] = {
  0x05, 0x0D, 0x09, 0x04, 0xA1, 0x01,  // Digitizer, Touch Screen, Application
  0x85, REPORT_ID,
  0x09, 0x22, 0xA1, 0x02,              //   Finger, Logical
  0x09, 0x42, 0x09, 0x32,              //     Tip Switch, In Range
  0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02,
  0x95, 0x06, 0x81, 0x03,              //     padding
  XY_ABS(0xFF, 0x7F),
  0xC0, 0xC0,
};

const uint8_t DESC_MULTITOUCH[] = {
  0x05, 0x0D, 0x09, 0x04, 0xA1, 0x01,  // Digitizer, Touch Screen, Application
  0x85, REPORT_ID,
  0x09, 0x22, 0xA1, 0x02,              //   Finger, Logical
  0x09, 0x42, 0x09, 0x32,              //     Tip Switch, In Range
  0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02,
  0x95, 0x06, 0x81, 0x03,              //     padding
  0x09, 0x51, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,  // Contact Identifier
  XY_ABS(0xFF, 0x7F),
  0xC0,
  0x05, 0x0D, 0x09, 0x54, 0x15, 0x00, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,  // Contact Count
  0x85, REPORT_ID_MAXCOUNT,
  0x09, 0x55, 0x25, 0x0A, 0x75, 0x08, 0x95, 0x01, 0xB1, 0x02,  // Contact Count Maximum (Feature)
  0xC0,
};

const uint8_t DESC_PEN[] = {
  0x05, 0x0D, 0x09, 0x02, 0xA1, 0x01,  // Digitizer, Pen, Application
  0x85, REPORT_ID,
  0x09, 0x20, 0xA1, 0x00,              //   Stylus, Physical
  0x09, 0x42, 0x09, 0x32,              //     Tip Switch, In Range
  0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02,
  0x95, 0x06, 0x81, 0x03,              //     padding
  XY_ABS(0xFF, 0x7F),
  0xC0, 0xC0,
};

const uint8_t DESC_ABS_MOUSE[] = {
  0x05, 0x01, 0x09, 0x02, 0xA1, 0x01,  // Generic Desktop, Mouse, Application
  0x85, REPORT_ID,
  0x09, 0x01, 0xA1, 0x00,              //   Pointer, Physical
  0x05, 0x09, 0x19, 0x01, 0x29, 0x03,  //     Buttons 1-3
  0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x03, 0x81, 0x02,
  0x95, 0x05, 0x81, 0x03,              //     padding
  XY_ABS(0xFF, 0x7F),
  0xC0, 0xC0,
};

struct Variant { const char *name; const uint8_t *desc; uint16_t len; uint8_t down; };
const Variant VARIANTS[] = {
  {"touch", DESC_TOUCH, sizeof DESC_TOUCH, 0x03},
  {"multitouch", DESC_MULTITOUCH, sizeof DESC_MULTITOUCH, 0x03},
  {"pen", DESC_PEN, sizeof DESC_PEN, 0x03},
  {"abs-mouse", DESC_ABS_MOUSE, sizeof DESC_ABS_MOUSE, 0x01},
};
const int VARIANT_COUNT = 4;

USBHID HID;
USBCDC Console;
USBHIDMouse *mouse;  // the control, unless pure: if it moves and the variant does not, USB works
Preferences prefs;
int variant = 1;    // 1-based

class Digitizer : public USBHIDDevice {
  uint16_t _onGetDescriptor(uint8_t *buffer) override {
    const Variant &v = VARIANTS[variant - 1];
    memcpy(buffer, v.desc, v.len);
    return v.len;
  }
  uint16_t _onGetFeature(uint8_t report_id, uint8_t *buffer, uint16_t len) override {
    if (report_id != REPORT_ID_MAXCOUNT || !len) return 0;
    buffer[0] = 1;  // one contact
    return 1;
  }
};
Digitizer digitizer;

bool hostUp() { return tud_mounted(); }

// One absolute report. Layouts differ only in variant 2, which adds a contact id and a contact count.
bool report(uint8_t flags, uint16_t x, uint16_t y) {
  if (variant == 2) {
    uint8_t r[7] = {flags, 0, (uint8_t)x, (uint8_t)(x >> 8), (uint8_t)y, (uint8_t)(y >> 8), 1};
    return HID.SendReport(REPORT_ID, r, sizeof r);
  }
  uint8_t r[5] = {flags, (uint8_t)x, (uint8_t)(x >> 8), (uint8_t)y, (uint8_t)(y >> 8)};
  return HID.SendReport(REPORT_ID, r, sizeof r);
}

uint16_t scale(float f) { return (uint16_t)(f * FULL); }
uint8_t down() { return VARIANTS[variant - 1].down; }

void tap(float fx, float fy, uint32_t hold = HOLD_MS) {
  report(down(), scale(fx), scale(fy));
  delay(hold);
  report(0, scale(fx), scale(fy));
}

void swipe(float fx, float fy0, float fy1, uint32_t ms, int steps) {
  for (int i = 0; i <= steps; i++) {
    report(down(), scale(fx), scale(fy0 + (fy1 - fy0) * i / steps));
    delay(ms / steps);
  }
  report(0, scale(fx), scale(fy1));
}

// false: the host went away, stop at once.
bool wait(uint32_t ms) {
  for (uint32_t t = 0; t < ms; t += 50) {
    if (!hostUp()) return false;
    delay(50);
  }
  return true;
}

bool sequence() {
  if (mouse) mouse->move(50, 0);  // control: the AssistiveTouch pointer wiggles if USB HID works at all
  if (!wait(1500)) return false;
  if (mouse) mouse->move(-50, 0);
  if (!wait(1000)) return false;
  for (int i = 0; i < variant; i++) {  // the variant number as quick taps at the top, to tell rounds apart in the log
    tap(0.5, 0.12, 60);
    if (!wait(250)) return false;
  }
  if (!wait(POINT_GAP_MS)) return false;
  const float pts[][2] = {{0.25, 0.25}, {0.75, 0.25}, {0.5, 0.5}, {0.25, 0.75}, {0.75, 0.75}};
  for (auto &p : pts) {
    tap(p[0], p[1]);
    if (!wait(POINT_GAP_MS)) return false;
  }
  swipe(0.5, 0.7, 0.3, 300, 15);
  return wait(LOOP_GAP_MS);
}

void disarm(const char *why) {
  prefs.putInt("passes", 0);
  prefs.putBool("armed", false);
  Console.printf("disarmed: %s\n", why);
}

void restartInto(int v) {
  prefs.putInt("variant", v);
  Console.flush();
  delay(100);
  ESP.restart();
}

void status() {
  Console.printf("variant %d (%s), pure %d, armed %d, passes %d, usb %s\n", variant, VARIANTS[variant - 1].name,
                mouse == nullptr, prefs.getBool("armed", false), prefs.getInt("passes", 0), hostUp() ? "mounted" : "off");
}

void serialCommands() {
  if (!Console.available()) return;
  String line = Console.readStringUntil('\n');
  line.trim();
  if (line == "status") status();
  else if (line.startsWith("v ")) {
    int v = line.substring(2).toInt();
    if (v >= 1 && v <= VARIANT_COUNT) { disarm("variant change"); restartInto(v); }
  } else if (line.startsWith("pure ")) {
    prefs.putBool("pure", line.substring(5).toInt() != 0);
    disarm("mode change");
    restartInto(variant);
  } else if (line.startsWith("arm")) {
    int passes = line.length() > 4 ? line.substring(4).toInt() : 1;
    prefs.putInt("passes", passes < 1 ? 1 : passes);
    prefs.putInt("variant", 1);
    prefs.putBool("waiting", true);  // start after the cable moves to the phone
    prefs.putBool("armed", true);
    Console.println("armed: unplug and plug into the phone");
    Console.flush();
    delay(100);
    ESP.restart();
  }
}

void setup() {
  prefs.begin("abstouch");
  variant = constrain(prefs.getInt("variant", 1), 1, VARIANT_COUNT);
  Console.begin(115200);
  if (!prefs.getBool("pure", false)) mouse = new USBHIDMouse();
  if (!HID.addDevice(&digitizer, VARIANTS[variant - 1].len)) Console.println("addDevice failed");
  HID.begin();
  USB.productName("Abs Touch Spike");
  USB.begin();
}

void loop() {
  static uint32_t bootMs = millis();
  serialCommands();
  if (!prefs.getBool("armed", false)) { delay(20); return; }

  if (prefs.getBool("waiting", false)) {  // armed on the Mac: wait for its USB to go, then for the phone's to come
    static bool seen = false, left = false;  // seen first: slow enumeration must not read as "unplugged"
    if (hostUp()) seen = true;
    else if (seen) left = true;
    if (!left || !hostUp()) { delay(20); return; }
    prefs.putBool("waiting", false);
  } else if (!hostUp()) {  // restarted between variants: the phone enumerates again
    if (millis() - bootMs > HOST_WAIT_MS) disarm("no host after restart");
    delay(20);
    return;
  }

  if (!wait(START_WAIT_MS)) return disarm("host left");
  for (int i = 0; i < LOOPS; i++)
    if (!sequence()) return disarm("host left");

  if (variant < VARIANT_COUNT) return restartInto(variant + 1);
  int passes = prefs.getInt("passes", 1) - 1;
  prefs.putInt("passes", passes);
  if (passes <= 0) { disarm("passes done"); prefs.putInt("variant", 1); return; }
  if (!wait(PASS_PAUSE_MS)) return disarm("host left");
  restartInto(1);
}
