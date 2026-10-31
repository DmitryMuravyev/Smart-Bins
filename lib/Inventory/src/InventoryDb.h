#pragma once

// The parts inventory: a SQLite database of parts and the bins they live in.
//
// Plain C++ on top of the SQLite C API, with no Arduino or ESP-IDF calls, so
// the same code runs on the board and in the native unit tests (which link the
// system's libsqlite3).
//
// Thread safety: SQLite on the ESP32 is built single-threaded, and requests
// arrive on the web server's task while other tasks may read statistics. Every
// public method therefore takes one internal mutex for its whole duration.
//
// Flash writes: on the ESP32 a flash write switches off the flash cache, which
// also stalls the LED strips' refill interrupt (see led::FlashGuard). Every
// write transaction runs between the optional WriteHooks so the firmware can
// pause the strips around it; the native build leaves them empty.

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

struct sqlite3;

namespace sb {
namespace inventory {

// Version of the schema created by this firmware. Stored in the database file
// (PRAGMA user_version); open() upgrades older files step by step and refuses
// newer ones, which a downgraded firmware wouldn't understand.
constexpr int kSchemaVersion = 1;

// Free-text fields of a part, in the order of Part::text. The JSON key is what
// the web API uses; the column is the database name; maxLen (in bytes, UTF-8)
// keeps one record from eating the RAM of a request or the page cache.
enum TextField {
  kPartNumber,
  kManufacturer,
  kCategory,
  kDescription,
  kPackage,
  kValue,
  kUnit,
  kDatasheetUrl,
  kNotes,
  kTextFieldCount
};

struct TextFieldInfo {
  const char *key;     // JSON / form parameter name
  const char *column;  // database column
  uint16_t maxLen;
};
const TextFieldInfo &textField(int field);

// A bin on the grid; x = -1 means "no bin".
struct BinRef {
  int x = -1;
  int y = -1;
  bool valid() const { return x >= 0 && y >= 0; }
};

struct Part {
  int64_t id = 0;
  std::string text[kTextFieldCount];
  int32_t quantity = 0;
  BinRef bin;
  std::string photo;       // file name, set by the photo store (Task 10); empty when none
  int64_t createdAt = 0;   // Unix seconds; 0 when the board's clock wasn't set
  int64_t updatedAt = 0;
};

// A partial update: only the fields that are set change.
struct PartChanges {
  std::optional<std::string> text[kTextFieldCount];
  std::optional<int32_t> quantity;
  std::optional<BinRef> bin;  // a BinRef with x = -1 takes the part out of its bin
};

struct BinInfo {
  int x = 0, y = 0;
  std::string label;  // empty: the UI shows the A1-style name
  int parts = 0;      // number of parts stored in it
  bool inGrid = true; // false: outside the configured grid, kept because it holds parts
};

struct Stats {
  int64_t parts = 0;          // part records
  int64_t totalQuantity = 0;  // sum of all quantities
  int64_t photos = 0;         // parts with a photo
  int64_t categories = 0;     // distinct non-empty categories
  int64_t manufacturers = 0;  // distinct non-empty manufacturers
  int64_t binsTotal = 0;      // bins in the configured grid
  int64_t binsUsed = 0;       // bins (in the grid or not) holding at least one part
  int64_t dbBytes = 0;        // size of the database file (pages x page size)
};

// Timings of benchmark(), in milliseconds.
struct BenchResult {
  int count = 0;
  int64_t insertMs = 0;  // all inserts, one transaction
  int64_t countMs = 0;   // SELECT count(*)
  int64_t searchMs = 0;  // a substring search over every text field (full scan)
  int64_t binMs = 0;     // the parts of one bin (index lookup)
  int64_t getMs = 0;     // one part by id
  int64_t deleteMs = 0;  // removing the test parts again
  int64_t found = 0;     // rows the search matched (a sanity check)
};

// What findParts() returns and in which order. Every filter that is set must
// match; an empty query lists every part.
enum class SortKey { Id, PartNumber, Manufacturer, Category, Description, Package, Value, Quantity, Bin, Updated };

struct PartQuery {
  // Words to look for, separated by spaces. Each word must appear somewhere
  // in the part's text fields (part number, description, notes, ...), as a
  // substring and regardless of case: "stm32 lqfp" finds an STM32F103 whose
  // package is LQFP-48. Case is ignored for Latin, Latin-1 (Spanish) and
  // Cyrillic letters.
  std::string search;
  // Exact value of the field, regardless of case; empty means any.
  std::string category;
  std::string manufacturer;
  // Only the parts in this bin; a BinRef with x = -1 means parts in no bin.
  std::optional<BinRef> bin;
  SortKey sort = SortKey::Id;
  bool descending = false;
  int offset = 0;
  int limit = 50;
};

// One distinct value of a text field and how many parts have it.
struct FieldValue {
  std::string value;
  int64_t parts = 0;
};

// How many of a query's parts are in one bin (see countByBin()).
struct BinCount {
  BinRef bin;  // x = -1: the parts in no bin
  int64_t parts = 0;
};

enum class Result { Ok, NotFound, Invalid, Error };

struct WriteHooks {
  std::function<void()> begin;  // before the first write of a transaction
  std::function<void()> end;    // after its commit or rollback
};

class InventoryDb {
 public:
  InventoryDb() = default;
  ~InventoryDb() { close(); }
  InventoryDb(const InventoryDb &) = delete;
  InventoryDb &operator=(const InventoryDb &) = delete;

  // Opens (or creates) the database file and brings its schema up to date.
  // `cacheKiB` is SQLite's page cache size: the main RAM cost of the database.
  // 16 KiB (eight 2 KB pages) holds the top of both tables' B-trees, which
  // is what lookups touch most. A board without PSRAM has only some 40 KB of
  // heap left once Wi-Fi and the web server run, and a large write (the
  // benchmark, a restore) briefly needs tens of KB more on top of the cache.
  bool open(const char *path, std::string &err, int cacheKiB = 16);
  void close();
  bool isOpen() const { return db_ != nullptr; }

  void setWriteHooks(WriteHooks hooks) { hooks_ = std::move(hooks); }
  // Current time in Unix seconds, or 0 when unknown (the board before its
  // clock is set over the network). Used for created/updated times.
  void setClock(std::function<int64_t()> clock) { clock_ = std::move(clock); }

  // Makes the bins table match a grid of cols x rows: missing bins are added,
  // and bins outside the grid are removed unless parts are stored in them.
  Result syncBins(int cols, int rows, std::string &err);

  Result addPart(const PartChanges &fields, Part &out, std::string &err);
  Result getPart(int64_t id, Part &out, std::string &err);
  Result updatePart(int64_t id, const PartChanges &changes, Part &out, std::string &err);
  Result deletePart(int64_t id, std::string &err);
  // Sets the part's photo (the photo store's name for it; empty: none) and
  // its updated time. `previous` receives the name it had, so the caller can
  // delete the old files once the new name is saved.
  Result setPhoto(int64_t id, const std::string &photo, std::string &previous, Part &out, std::string &err);
  // Adds `delta` to the quantity (negative to take parts out); never goes
  // below 0. One statement, so two quick taps on "-" can't lose an update.
  Result adjustQuantity(int64_t id, int32_t delta, Part &out, std::string &err);

  // Parts in id order, one callback per part so a long list never has to be
  // held in memory. `total` receives the number of parts overall.
  Result listParts(int offset, int limit, const std::function<void(const Part &)> &each, int64_t &total,
                   std::string &err);
  // Search, filter, sort and page through the parts. `each` gets the parts of
  // the requested page one at a time; `total` is the number of parts that
  // match, across all pages.
  Result findParts(const PartQuery &query, const std::function<void(const Part &)> &each, int64_t &total,
                   std::string &err);
  // How the parts matching `query` are spread over the bins: one BinCount per
  // bin that holds at least one of them (sort, offset and limit are ignored).
  // Lets the bin grid show where the search results are.
  Result countByBin(const PartQuery &query, const std::function<void(const BinCount &)> &each, std::string &err);
  // The distinct non-empty values of one text field, most used first, for
  // filter lists and autocomplete. `contains` narrows them like a search word
  // (substring, any case); `limit` caps how many come back.
  Result listValues(TextField field, const std::string &contains, int limit,
                    const std::function<void(const FieldValue &)> &each, std::string &err);
  Result listBins(const std::function<void(const BinInfo &)> &each, std::string &err);
  Result setBinLabel(BinRef bin, const std::string &label, std::string &err);

  Result stats(Stats &out, std::string &err);

  // Inserts `count` generated parts, times a few typical queries, and removes
  // the parts again. The parts are spread over a cols x rows grid.
  Result benchmark(int count, int cols, int rows, BenchResult &out, std::string &err);


 private:
  class Stmt;
  class WriteTxn;

  bool exec(const char *sql, std::string &err);
  bool migrate(std::string &err);
  bool pragmasApplied(std::string &err);
  bool convertPageSize(std::string &err);
  // Removes the parts benchmark() added (all in kBenchCategory), in batches.
  Result deleteBenchParts(std::string &err);
  Result readPart(int64_t id, Part &out, std::string &err);
  static void rowToPart(Stmt &q, Part &out);
  Result binId(BinRef bin, int64_t &id, std::string &err);
  bool validate(const PartChanges &fields, bool creating, std::string &err) const;
  int64_t now() const { return clock_ ? clock_() : 0; }
  std::string lastError() const;

  sqlite3 *db_ = nullptr;
  std::recursive_mutex mutex_;
  int64_t tally_ = 0;  // rows sb_tally() accepted in the current findParts() scan
  WriteHooks hooks_;
  std::function<int64_t()> clock_;
};

}  // namespace inventory
}  // namespace sb
