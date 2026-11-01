#pragma once

// Health: why the board last restarted, and a watchdog for the main loop.
//
// Restart reason. ESP-IDF knows why the chip last reset (power on, a
// software restart, a crash, a hardware watchdog, a brown-out...). The boot
// log prints it and the System page shows it, so a board that restarted by
// itself in the night says so, and why. A brown-out (the 3.3 V supply sagged)
// usually means the 5 V feed or its wiring is too weak for the Wi-Fi peaks.
//
// Main loop watchdog. The ESP-IDF task watchdog already watches the web
// server's task and the idle task on core 0, and resets the board 5 s after
// one of them stops. The Arduino main loop (Wi-Fi supervision, the BOOT
// button, the status LED, live updates, queued file sends) is not watched by
// it, and 5 s would be too tight there: it can wait for the flash gate while
// a long database search or a restore holds it. So a timer checks every
// second that loop() still runs, and restarts the board when it hasn't for
// 60 s: a hang there would otherwise leave the board on but deaf to Wi-Fi
// changes and the button. The next boot reports it as "hang". Uploads over
// ArduinoOTA run inside loop() and take longer; the watch pauses for them.

#include <cstdint>

namespace sb {
namespace health {

// Reads the reset reason (call once, early in setup()) and starts the loop
// watch.
void begin();

// Called on every pass of loop().
void loopAlive();

// Pauses the loop watch (true) and resumes it (false), for work that keeps
// loop() busy on purpose (an ArduinoOTA upload).
void pauseLoopWatch(bool pause);

// The last restart's reason as a short key for the UI and the API: power-on,
// restart, crash, watchdog, hang, brownout, reset-pin, other.
const char *lastRestart();

// True for the reasons that point at a problem (crash, watchdog, hang,
// brown-out): the System page highlights them.
bool lastRestartWasFault();

}  // namespace health
}  // namespace sb
