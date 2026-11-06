# Absolute touch spike

Does iOS accept a USB touch screen, pen or absolute mouse from the board? If it does, tapping no longer needs the
AssistiveTouch pointer, calibration or re-homing. Standalone sketch; `hardware/hid_bridge` is not touched.

One image holds all four report descriptors, chosen at boot. A relative mouse stays in every variant as the control:
if the pointer moves and the variant does nothing, USB works and iOS rejects the descriptor.

| # | Variant | HID usage | Report |
|---|---|---|---|
| 1 | Single-touch screen | Digitizer / Touch Screen | tip, in range; X, Y 0-32767 |
| 2 | Multi-touch screen | Digitizer / Touch Screen | tip, in range, contact id, X, Y, contact count; Feature: max contacts |
| 3 | Pen | Digitizer / Pen | tip, in range; X, Y |
| 4 | Absolute mouse (control) | Generic Desktop / Mouse | left button; X, Y |

## Run

1. Back up the board's flash, then build and flash `build/abs_touch.ino.merged.bin` at 0.
2. On the Mac: `python3 probe_server.py`. On the phone, open `http://<Mac LAN IP>:8000/` in Safari.
3. Over USB serial on the Mac, send `arm` (or `arm 2` for a second pass, 90 s later, to run with AssistiveTouch off).
4. Move the USB cable from the Mac to the phone and keep the phone unlocked on the test page.

Each variant runs the sequence twice, then the board restarts into the next one: about 5 minutes per pass. Per loop:
a relative wiggle, N quick taps at the top (N = variant number, to tell rounds apart), five points, one swipe.

**Safety:** the reports click at absolute positions, so `arm` starts only after the host goes away and a new one
appears. Any USB disconnect during a run disarms the board, and it disarms when the passes are done. Keep the test
page in front: if a variant works, its taps go to whatever is on screen.

## Results

`events.jsonl` holds one line per event: `pointerType` says whether iOS treats the input as a finger, a pen or a mouse.
Positions are fractions of the page, which is smaller than the screen in Safari, so check relative positions only.
