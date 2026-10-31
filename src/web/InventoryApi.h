#pragma once

// Inventory endpoints. Like the rest of the API, changes accept GET (handy for
// testing from a browser) and POST with a form-encoded body (used by the UI);
// parameters are the JSON keys of a part (partNumber, manufacturer, category,
// description, package, value, unit, datasheetUrl, notes, quantity) plus
// x and y for its bin (x=-1 takes it out of its bin).
//
//   GET  /api/parts[?offset=0&limit=50]   {total, offset, parts: [...]} in id order
//   GET  /api/part?id=N                   one part
//   POST /api/part/add                    creates a part from the fields given
//   POST /api/part/update?id=N            changes only the fields given
//   POST /api/part/delete?id=N
//   POST /api/part/adjust?id=N&delta=-1   quantity +/- (never below 0)
//   GET  /api/bins                        every bin with its label and part count
//   POST /api/bin/label?x=&y=&label=      names a bin (empty label: back to "A1")
//   GET  /api/inventory/stats             counts and database size
//   POST /api/inventory/benchmark[?n=2000]  starts the speed test (see .cpp)
//   GET  /api/inventory/benchmark         its progress and results
//
// A part is {id, partNumber, ..., notes, quantity, bin: {x, y} or null,
// photo, created, updated} with times in Unix seconds (0 when unknown).

#include <ArduinoJson.h>

class AsyncWebServer;

namespace sb {

namespace inventory {
class InventoryDb;
}

void registerInventoryApi(AsyncWebServer &server, inventory::InventoryDb &db, int gridCols, int gridRows);

// Adds the inventory figures to `out` (System page): parts, quantity, photos,
// categories, manufacturers, binsUsed, binsTotal, dbBytes. Leaves `out` empty
// when the database isn't open.
void writeInventoryStats(JsonObject out, inventory::InventoryDb &db);

}  // namespace sb
