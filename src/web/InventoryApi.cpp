#include "InventoryApi.h"

#include <ESPAsyncWebServer.h>

#include <string>

#include "ApiUtil.h"
#include "BootReport.h"
#include "InventoryDb.h"

namespace sb {

using inventory::BinRef;
using inventory::InventoryDb;
using inventory::Part;
using inventory::PartChanges;
using inventory::PartQuery;
using inventory::SortKey;
using inventory::Result;

namespace {

const char *TAG = "inventory";

// Page size limits for /api/parts. The answer is built in RAM before it is
// sent (AsyncResponseStream), so a page is capped: 100 parts with every field
// full would still be about 200 KB, but typical parts are a few hundred bytes.
constexpr long kDefaultLimit = 50;
constexpr long kMaxLimit = 100;

int httpStatus(Result r) {
  switch (r) {
    case Result::Ok: return 200;
    case Result::NotFound: return 404;
    case Result::Invalid: return 400;
    default: return 500;
  }
}

void writePart(JsonObject out, const Part &p) {
  out["id"] = p.id;
  for (int f = 0; f < inventory::kTextFieldCount; f++) {
    // c_str() is copied by ArduinoJson (it only keeps pointers to literals and
    // char arrays), so the Part may go away before serialisation.
    out[inventory::textField(f).key] = p.text[f].c_str();
  }
  out["quantity"] = p.quantity;
  if (p.bin.valid()) {
    JsonObject bin = out["bin"].to<JsonObject>();
    bin["x"] = p.bin.x;
    bin["y"] = p.bin.y;
  } else {
    out["bin"] = nullptr;
  }
  out["photo"] = p.photo.c_str();
  out["created"] = p.createdAt;
  out["updated"] = p.updatedAt;
}

void sendPart(AsyncWebServerRequest *req, const Part &p) {
  JsonDocument doc;
  writePart(doc.to<JsonObject>(), p);
  sendJson(req, 200, doc);
}

void sendResult(AsyncWebServerRequest *req, Result r, const std::string &err) {
  sendError(req, httpStatus(r), err.c_str());
}

// Reads the part fields present in the request. Fields not given stay unset,
// so the same parser serves "add" (missing = empty) and "update" (missing =
// unchanged). False with `err` set on a malformed number or bin.
bool readChanges(AsyncWebServerRequest *req, PartChanges &c, std::string &err) {
  for (int f = 0; f < inventory::kTextFieldCount; f++) {
    const char *key = inventory::textField(f).key;
    if (const AsyncWebParameter *p = param(req, key)) c.text[f] = std::string(p->value().c_str());
  }
  if (hasParam(req, "quantity")) {
    long q;
    if (!intParam(req, "quantity", 0, 1000000000, q)) {
      err = "quantity must be a whole number from 0";
      return false;
    }
    c.quantity = static_cast<int32_t>(q);
  }
  if (hasParam(req, "x")) {
    long x, y;
    if (intParam(req, "x", -1, -1, x)) {
      c.bin = BinRef();  // x=-1: no bin
    } else if (intParam(req, "x", 0, 254, x) && intParam(req, "y", 0, 254, y)) {
      c.bin = BinRef{static_cast<int>(x), static_cast<int>(y)};
    } else {
      err = "x and y must give a bin (x=-1 for none)";
      return false;
    }
  }
  return true;
}

// The sort orders /api/parts accepts, by their API names (the JSON keys of
// the fields, plus bin and updated).
struct SortName {
  const char *name;
  SortKey key;
};
const SortName kSortNames[] = {
    {"id", SortKey::Id},
    {"partNumber", SortKey::PartNumber},
    {"manufacturer", SortKey::Manufacturer},
    {"category", SortKey::Category},
    {"description", SortKey::Description},
    {"package", SortKey::Package},
    {"value", SortKey::Value},
    {"quantity", SortKey::Quantity},
    {"bin", SortKey::Bin},
    {"updated", SortKey::Updated},
};

// Reads the search, filters, sort and page of /api/parts. False with `err`
// set when a parameter is malformed; missing ones keep their defaults.
bool readQuery(AsyncWebServerRequest *req, PartQuery &q, std::string &err) {
  if (const AsyncWebParameter *p = param(req, "q")) q.search = p->value().c_str();
  if (const AsyncWebParameter *p = param(req, "category")) q.category = p->value().c_str();
  if (const AsyncWebParameter *p = param(req, "manufacturer")) q.manufacturer = p->value().c_str();
  if (hasParam(req, "x")) {
    long x, y;
    if (intParam(req, "x", -1, -1, x)) {
      q.bin = BinRef();  // x=-1: parts in no bin
    } else if (intParam(req, "x", 0, 254, x) && intParam(req, "y", 0, 254, y)) {
      q.bin = BinRef{static_cast<int>(x), static_cast<int>(y)};
    } else {
      err = "x and y must give a bin (x=-1 for parts in no bin)";
      return false;
    }
  }
  if (const AsyncWebParameter *p = param(req, "sort")) {
    bool known = false;
    for (const SortName &s : kSortNames) {
      if (p->value() == s.name) {
        q.sort = s.key;
        known = true;
      }
    }
    if (!known) {
      err = "sort must be id, partNumber, manufacturer, category, description, package, value, quantity, bin or updated";
      return false;
    }
  }
  if (const AsyncWebParameter *p = param(req, "order")) {
    if (p->value() != "asc" && p->value() != "desc") {
      err = "order must be asc or desc";
      return false;
    }
    q.descending = p->value() == "desc";
  }
  long offset = 0, limit = kDefaultLimit;
  if (hasParam(req, "offset") && !intParam(req, "offset", 0, 0x7FFFFFFF, offset)) {
    err = "offset must be a whole number from 0";
    return false;
  }
  if (hasParam(req, "limit") && !intParam(req, "limit", 1, kMaxLimit, limit)) {
    err = "limit must be 1 to 100";
    return false;
  }
  q.offset = static_cast<int>(offset);
  q.limit = static_cast<int>(limit);
  return true;
}

bool readId(AsyncWebServerRequest *req, int64_t &id) {
  long v;
  if (!intParam(req, "id", 1, 0x7FFFFFFF, v)) return false;
  id = v;
  return true;
}

// The benchmark runs in its own task: 2,000 inserts plus the queries take
// seconds on flash, far longer than a web request may block the network task
// (its watchdog would reset the board). The page starts it, then polls.
struct Benchmark {
  volatile bool running = false;
  bool done = false;
  inventory::BenchResult result;
  std::string error;
  uint32_t heapBefore = 0, largestBefore = 0, heapAfter = 0, heapMin = 0;
  InventoryDb *db = nullptr;
  int count = 0, cols = 0, rows = 0;
} bench;

void benchTask(void *) {
  bench.heapBefore = heapFree();
  bench.largestBefore = heapLargest();
  std::string err;
  const Result r = bench.db->benchmark(bench.count, bench.cols, bench.rows, bench.result, err);
  bench.error = r == Result::Ok ? std::string() : err;
  bench.heapAfter = heapFree();
  bench.heapMin = heapMin();
  const auto &b = bench.result;
  ESP_LOGI(TAG, "Benchmark %d parts: insert %lld ms, count %lld ms, search %lld ms, bin %lld ms, get %lld ms, delete %lld ms",
           b.count, (long long)b.insertMs, (long long)b.countMs, (long long)b.searchMs, (long long)b.binMs,
           (long long)b.getMs, (long long)b.deleteMs);
  bench.done = true;
  bench.running = false;
  vTaskDelete(nullptr);
}

void sendBenchmark(AsyncWebServerRequest *req) {
  JsonDocument doc;
  doc["running"] = static_cast<bool>(bench.running);
  doc["done"] = bench.done;
  if (bench.done) {
    if (!bench.error.empty()) doc["error"] = bench.error.c_str();
    const auto &b = bench.result;
    doc["count"] = b.count;
    doc["insertMs"] = b.insertMs;
    doc["countMs"] = b.countMs;
    doc["searchMs"] = b.searchMs;
    doc["searchFound"] = b.found;
    doc["binMs"] = b.binMs;
    doc["getMs"] = b.getMs;
    doc["deleteMs"] = b.deleteMs;
    doc["heapBefore"] = bench.heapBefore;
    doc["largestBlockBefore"] = bench.largestBefore;
    doc["heapAfter"] = bench.heapAfter;
    doc["heapMinEver"] = bench.heapMin;  // lowest since boot: includes the benchmark
  }
  sendJson(req, 200, doc);
}

}  // namespace

void writeInventoryStats(JsonObject out, InventoryDb &db) {
  inventory::Stats s;
  std::string err;
  if (!db.isOpen() || db.stats(s, err) != Result::Ok) return;
  out["parts"] = s.parts;
  out["quantity"] = s.totalQuantity;
  out["photos"] = s.photos;
  out["categories"] = s.categories;
  out["manufacturers"] = s.manufacturers;
  out["binsUsed"] = s.binsUsed;
  out["binsTotal"] = s.binsTotal;
  out["dbBytes"] = s.dbBytes;
}

void registerInventoryApi(AsyncWebServer &server, InventoryDb &db, int gridCols, int gridRows) {
  // Every handler starts with this: with no database (the partition failed to
  // mount, or the file is from a newer firmware) the API says so plainly.
  auto ready = [&db](AsyncWebServerRequest *req) {
    if (db.isOpen()) return true;
    sendError(req, 503, "inventory database unavailable, see the serial log");
    return false;
  };

  // Search, filters, sort and pages (see README). Without parameters: every
  // part in id order, 50 at a time.
  server.on(AsyncURIMatcher::exact("/api/parts"), HTTP_GET, [&db, ready](AsyncWebServerRequest *req) {
    if (!ready(req)) return;
    PartQuery query;
    std::string err;
    if (!readQuery(req, query, err)) return sendError(req, 400, err.c_str());
    // Written part by part into the response, so only one part's JSON
    // document exists at a time (the database hands them over one by one).
    AsyncResponseStream *res = req->beginResponseStream("application/json");
    res->print("{\"parts\":[");
    bool first = true;
    int64_t total = 0;
    const Result r = db.findParts(
        query,
        [&](const Part &p) {
          JsonDocument doc;
          writePart(doc.to<JsonObject>(), p);
          if (!first) res->print(',');
          first = false;
          serializeJson(doc, *res);
        },
        total, err);
    if (r != Result::Ok) {
      delete res;  // not sent yet: answer with the error instead
      return sendResult(req, r, err);
    }
    res->printf("],\"total\":%lld,\"offset\":%d}", (long long)total, query.offset);
    req->send(res);
  });

  // Distinct values of one text field with their part counts, most used
  // first: the choices for the category and manufacturer filters, and
  // autocomplete in the part form (Task 9).
  server.on(AsyncURIMatcher::exact("/api/parts/values"), HTTP_GET, [&db, ready](AsyncWebServerRequest *req) {
    if (!ready(req)) return;
    const AsyncWebParameter *fieldName = param(req, "field");
    int field = -1;
    for (int f = 0; fieldName && f < inventory::kTextFieldCount; f++) {
      if (fieldName->value() == inventory::textField(f).key) field = f;
    }
    if (field < 0) return sendError(req, 400, "field must be a part field such as category or manufacturer");
    long limit = 50;
    if (hasParam(req, "limit") && !intParam(req, "limit", 1, 500, limit)) {
      return sendError(req, 400, "limit must be 1 to 500");
    }
    const AsyncWebParameter *contains = param(req, "q");
    AsyncResponseStream *res = req->beginResponseStream("application/json");
    res->print("{\"values\":[");
    bool first = true;
    std::string err;
    const Result r = db.listValues(
        static_cast<inventory::TextField>(field), contains ? std::string(contains->value().c_str()) : std::string(),
        static_cast<int>(limit),
        [&](const inventory::FieldValue &v) {
          JsonDocument doc;
          doc["value"] = v.value.c_str();
          doc["parts"] = v.parts;
          if (!first) res->print(',');
          first = false;
          serializeJson(doc, *res);
        },
        err);
    if (r != Result::Ok) {
      delete res;
      return sendResult(req, r, err);
    }
    res->print("]}");
    req->send(res);
  });

  server.on(AsyncURIMatcher::exact("/api/part"), HTTP_GET, [&db, ready](AsyncWebServerRequest *req) {
    if (!ready(req)) return;
    int64_t id;
    if (!readId(req, id)) return sendError(req, 400, "id must be a part id");
    Part p;
    std::string err;
    const Result r = db.getPart(id, p, err);
    r == Result::Ok ? sendPart(req, p) : sendResult(req, r, err);
  });

  server.on("/api/part/add", HTTP_GET | HTTP_POST, [&db, ready](AsyncWebServerRequest *req) {
    if (!ready(req)) return;
    PartChanges c;
    std::string err;
    if (!readChanges(req, c, err)) return sendError(req, 400, err.c_str());
    Part p;
    const Result r = db.addPart(c, p, err);
    r == Result::Ok ? sendPart(req, p) : sendResult(req, r, err);
  });

  server.on("/api/part/update", HTTP_GET | HTTP_POST, [&db, ready](AsyncWebServerRequest *req) {
    if (!ready(req)) return;
    int64_t id;
    if (!readId(req, id)) return sendError(req, 400, "id must be a part id");
    PartChanges c;
    std::string err;
    if (!readChanges(req, c, err)) return sendError(req, 400, err.c_str());
    Part p;
    const Result r = db.updatePart(id, c, p, err);
    r == Result::Ok ? sendPart(req, p) : sendResult(req, r, err);
  });

  server.on("/api/part/delete", HTTP_GET | HTTP_POST, [&db, ready](AsyncWebServerRequest *req) {
    if (!ready(req)) return;
    int64_t id;
    if (!readId(req, id)) return sendError(req, 400, "id must be a part id");
    std::string err;
    const Result r = db.deletePart(id, err);
    if (r != Result::Ok) return sendResult(req, r, err);
    JsonDocument doc;
    doc["ok"] = true;
    sendJson(req, 200, doc);
  });

  server.on("/api/part/adjust", HTTP_GET | HTTP_POST, [&db, ready](AsyncWebServerRequest *req) {
    if (!ready(req)) return;
    int64_t id;
    long delta;
    if (!readId(req, id)) return sendError(req, 400, "id must be a part id");
    if (!intParam(req, "delta", -1000000000, 1000000000, delta)) return sendError(req, 400, "delta must be a whole number");
    Part p;
    std::string err;
    const Result r = db.adjustQuantity(id, static_cast<int32_t>(delta), p, err);
    r == Result::Ok ? sendPart(req, p) : sendResult(req, r, err);
  });

  server.on(AsyncURIMatcher::exact("/api/bins"), HTTP_GET, [&db, ready](AsyncWebServerRequest *req) {
    if (!ready(req)) return;
    // At most 255 x 255 bins in theory, a few hundred in practice: streamed
    // like the parts list so a large grid doesn't need one big document.
    AsyncResponseStream *res = req->beginResponseStream("application/json");
    res->print("{\"bins\":[");
    bool first = true;
    std::string err;
    const Result r = db.listBins(
        [&](const inventory::BinInfo &b) {
          JsonDocument doc;
          doc["x"] = b.x;
          doc["y"] = b.y;
          doc["label"] = b.label.c_str();
          doc["parts"] = b.parts;
          if (!b.inGrid) doc["outside"] = true;  // left over after the grid shrank
          if (!first) res->print(',');
          first = false;
          serializeJson(doc, *res);
        },
        err);
    if (r != Result::Ok) {
      delete res;
      return sendResult(req, r, err);
    }
    res->print("]}");
    req->send(res);
  });

  server.on("/api/bin/label", HTTP_GET | HTTP_POST, [&db, ready](AsyncWebServerRequest *req) {
    if (!ready(req)) return;
    long x, y;
    if (!intParam(req, "x", 0, 254, x) || !intParam(req, "y", 0, 254, y)) {
      return sendError(req, 400, "x and y must give a bin");
    }
    const AsyncWebParameter *label = param(req, "label");
    std::string err;
    const Result r = db.setBinLabel({static_cast<int>(x), static_cast<int>(y)},
                                    label ? std::string(label->value().c_str()) : std::string(), err);
    if (r != Result::Ok) return sendResult(req, r, err);
    JsonDocument doc;
    doc["ok"] = true;
    sendJson(req, 200, doc);
  });

  server.on(AsyncURIMatcher::exact("/api/inventory/stats"), HTTP_GET, [&db, ready](AsyncWebServerRequest *req) {
    if (!ready(req)) return;
    JsonDocument doc;
    writeInventoryStats(doc.to<JsonObject>(), db);
    sendJson(req, 200, doc);
  });

  // The Task 7 check on the real board: inserts n generated parts (default
  // 2,000), times typical queries and removes the parts again. LED strips
  // pause while the inserts are written (one long write transaction).
  bench.db = &db;
  bench.cols = gridCols;
  bench.rows = gridRows;
  server.on(AsyncURIMatcher::exact("/api/inventory/benchmark"), HTTP_GET | HTTP_POST, [ready](AsyncWebServerRequest *req) {
    if (!ready(req)) return;
    if (req->method() == HTTP_GET) return sendBenchmark(req);
    if (bench.running) return sendError(req, 409, "the benchmark is already running");
    long n = 2000;
    if (hasParam(req, "n") && !intParam(req, "n", 10, 10000, n)) return sendError(req, 400, "n must be 10 to 10000");
    bench.count = static_cast<int>(n);
    bench.done = false;
    bench.running = true;
    // 10 KB stack: SQLite's parser and pager need several KB. Core 0, away
    // from the LED render tasks on core 1.
    if (xTaskCreatePinnedToCore(benchTask, "benchmark", 10240, nullptr, 1, nullptr, 0) != pdPASS) {
      bench.running = false;
      // Almost always a lack of RAM for the task's stack, so say how much
      // there is: the free total and the largest block (the stack must fit in
      // one piece).
      char msg[96];
      snprintf(msg, sizeof(msg), "cannot start the benchmark task (free RAM %u bytes, largest block %u)",
               (unsigned)heapFree(), (unsigned)heapLargest());
      ESP_LOGE(TAG, "%s", msg);
      return sendError(req, 500, msg);
    }
    sendBenchmark(req);
  });
}

}  // namespace sb
