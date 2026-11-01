#pragma once

// Memory profile: every setting that was sized for a board without PSRAM,
// in one place, with the larger values a board with PSRAM can afford.
//
// SB_PSRAM (set by the PSRAM builds in platformio.ini, e.g. esp32-16mb-psram
// for the ESP32-WROOM-32E/UE-N16R2) switches all of them at once. Without
// it (the default, and the host tests) the values are the ones the firmware
// was tuned with on a WROOM-32: about 300 KB of internal RAM, of which some
// 45 KB stays free while the web server runs.
//
// How PSRAM helps: the ESP-IDF heap puts every allocation larger than 4 KB
// (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL) in PSRAM and keeps smaller ones in
// internal RAM. The big, bursty users (SQLite's page cache and B-tree work,
// photo and backup buffers, queued WebSocket messages) move out, which
// leaves internal RAM for task stacks, Wi-Fi and the many small blocks.
//
// A few settings are build flags rather than constants here, because a
// library reads them as macros: WS_MAX_QUEUED_MESSAGES,
// CONFIG_ASYNC_TCP_STACK_SIZE and SQLITE_ESP32VFS_BUFFERSZ (platformio.ini).

#include <cstddef>
#include <cstdint>

#ifndef SB_PSRAM
#define SB_PSRAM 0
#endif

namespace sb {
namespace mem {

constexpr bool kPsram = SB_PSRAM != 0;

// ---- Inventory database (SQLite) ----

// Page size of a new database file. 4 KB matches a flash sector (one page
// write = one sector rewrite), but SQLite holds several pages at once while
// it splits or merges B-tree pages, and that peak (40 KB and more) didn't fit
// without PSRAM; 2 KB pages halve it. An existing file keeps its page size
// unless it is small enough to convert (kDbMaxConvertBytes).
constexpr int kDbPageSize = kPsram ? 4096 : 2048;
// Largest database file converted to kDbPageSize when it opens. The
// conversion (VACUUM) builds the new copy in RAM.
constexpr int64_t kDbMaxConvertBytes = kPsram ? 1024 * 1024 : 64 * 1024;
// RAM the page cache may use, in KiB: the more of the file it holds, the
// fewer flash reads a search does (and the fewer pauses of the LED strips,
// see FlashGuard).
constexpr int kDbCacheKiB = kPsram ? 512 : 16;

// ---- Web server ----

// File and listing responses sent at the same time (SendGate); each one
// holds network buffers while it is sent. More run in parallel with PSRAM.
constexpr int kSendsActive = kPsram ? 6 : 2;
// Requests waiting for a send slot before new ones are answered 503.
constexpr size_t kSendsWaiting = kPsram ? 64 : 24;
// Parts read from the database per chunk of a parts listing (about 300
// bytes of JSON each).
constexpr size_t kPartsPerRead = kPsram ? 25 : 10;
// Rows of a CSV import written per transaction: SQLite keeps a
// transaction's changed pages in RAM until the commit.
constexpr size_t kImportBatch = kPsram ? 50 : 5;
// Restored database written to flash in steps of this size.
constexpr size_t kRestoreWriteBuffer = kPsram ? 16384 : 4096;
// Files the FAT partition may have open at once (each holds a sector buffer).
constexpr int kMaxOpenFiles = kPsram ? 8 : 4;

// ---- Tasks (stacks are always in internal RAM) ----

// The microphone task: 3 KB leaves about 1 KB unused.
constexpr uint32_t kMicStack = kPsram ? 4096 : 3072;

}  // namespace mem
}  // namespace sb
