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
