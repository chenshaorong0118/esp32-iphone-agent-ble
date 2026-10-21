// hid_ble has no Wi-Fi. The app still reads the encrypted `net` characteristic once to trigger iOS
// pairing, so it answers {"mode":"none"} and ignores writes.
#pragma once
#include <Arduino.h>

inline String net_json() { return "{\"mode\":\"none\"}"; }
inline void net_request(const String &) {}
