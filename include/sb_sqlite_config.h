#pragma once

// Build settings for the ESP32 SQLite library (Sqlite3Esp32), read by its
// sqlite3.c through SQLITE_CUSTOM_INCLUDE (set in platformio.ini). SQLite
// includes this file after the library's own config_ext.h, so a setting here
// overrides the library's choice. Only sqlite3.c reads it.

// The library sets YYSTACKDEPTH to 20: the SQL parser's stack holds 20
// entries, and a statement that nests deeper fails to prepare with "parser
// stack overflow". That already happens for ordinary statements such as
//   DELETE FROM bins WHERE (x >= ?1 OR y >= ?2)
//     AND NOT EXISTS (SELECT 1 FROM parts WHERE parts.bin_id = bins.id)
// 0 makes the stack grow on the heap as a statement needs it (SQLite's own
// option for small task stacks): no fixed limit, and nothing extra on the
// stack of the task that prepares the statement.
#undef YYSTACKDEPTH
#define YYSTACKDEPTH 0

// The library preallocates the page cache for 8 pages when a database opens
// (SQLITE_DEFAULT_PCACHE_INITSZ). With 4 KB pages that is ~33 KB of RAM taken
// up front, before a single page is read. 0: pages are allocated as they are
// used, still limited by PRAGMA cache_size.
#undef SQLITE_DEFAULT_PCACHE_INITSZ
#define SQLITE_DEFAULT_PCACHE_INITSZ 0

// Lookaside memory (small per-connection blocks for short-lived allocations):
// the library's default is 512 slots of 64 bytes, 32 KB per connection. 64
// slots of 128 bytes (8 KB) keep most of the speed-up. InventoryDb::open()
// sets the same size at run time; setting it here too means the 32 KB block
// is never allocated in the first place (and can't fragment the heap when it
// is replaced).
#undef SQLITE_DEFAULT_LOOKASIDE
#define SQLITE_DEFAULT_LOOKASIDE 128, 64
