#include "InventoryDb.h"

#include <sqlite3.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace sb {
namespace inventory {

namespace {

const TextFieldInfo kTextFields[kTextFieldCount] = {
    {"partNumber", "part_number", 64},    {"manufacturer", "manufacturer", 64},
    {"category", "category", 64},         {"description", "description", 256},
    {"package", "package", 32},           {"value", "value", 32},
    {"unit", "unit", 16},                 {"datasheetUrl", "datasheet_url", 256},
    {"notes", "notes", 1024},
};

constexpr size_t kMaxBinLabel = 32;
constexpr int32_t kMaxQuantity = 1000000000;
// Database page size. 2 KB rather than the flash sector's 4 KB: SQLite holds
// several pages at once while it splits or merges B-tree pages during a
// write, and with 4 KB pages that peak (40 KB and more on top of the page
// cache) didn't fit in the heap a board without PSRAM has left. The cost is
// that writing one page makes the FAT layer rewrite a whole 4 KB sector, so
// a commit can erase twice as many sectors; at an inventory's rate of
// changes that is far from mattering for flash wear.
constexpr int kPageSize = 2048;

// Category of the parts benchmark() creates, so they can be told apart (and
// removed) even if the board restarts half way through.
const char *const kBenchCategory = "~benchmark~";

// The columns readPart()/listParts() select, in the order rowToPart() reads
// them. Parts join their bin for its x, y (NULL when the part has no bin).
const char *const kPartSelect =
    "SELECT p.id, p.part_number, p.manufacturer, p.category, p.description, p.package, p.value, p.unit,"
    " p.datasheet_url, p.notes, p.quantity, b.x, b.y, p.photo, p.created_at, p.updated_at"
    " FROM parts p LEFT JOIN bins b ON b.id = p.bin_id";

// Schema steps. Step i takes a database from user_version i to i + 1; each
// runs in its own transaction together with the version bump, so a power cut
// during an upgrade leaves the previous version intact. New versions append a
// step here; existing steps never change, since databases already went
// through them.
const char *const kMigrations[kSchemaVersion] = {
    // 0 -> 1: the first schema.
    // bins: one row per grid position. Parts reference bins by id rather than
    // by x, y so a bin can carry a label and the grid can be resized without
    // touching the parts. in_grid = 0 marks a bin left outside after the grid
    // shrank, kept because parts are still stored in it.
    // parts: text columns are NOT NULL DEFAULT '' so the UI and search never
    // have to deal with NULL. Removing a bin keeps its parts (bin_id = NULL).
    "CREATE TABLE bins ("
    "  id INTEGER PRIMARY KEY,"
    "  x INTEGER NOT NULL, y INTEGER NOT NULL,"
    "  label TEXT,"
    "  in_grid INTEGER NOT NULL DEFAULT 1,"
    "  UNIQUE (x, y));"
    "CREATE TABLE parts ("
    "  id INTEGER PRIMARY KEY,"
    "  part_number TEXT NOT NULL DEFAULT '',"
    "  manufacturer TEXT NOT NULL DEFAULT '',"
    "  category TEXT NOT NULL DEFAULT '',"
    "  description TEXT NOT NULL DEFAULT '',"
    "  package TEXT NOT NULL DEFAULT '',"
    "  value TEXT NOT NULL DEFAULT '',"
    "  unit TEXT NOT NULL DEFAULT '',"
    "  datasheet_url TEXT NOT NULL DEFAULT '',"
    "  notes TEXT NOT NULL DEFAULT '',"
    "  quantity INTEGER NOT NULL DEFAULT 0 CHECK (quantity >= 0),"
    "  bin_id INTEGER REFERENCES bins(id) ON DELETE SET NULL,"
    "  photo TEXT,"
    "  created_at INTEGER,"
    "  updated_at INTEGER);"
    "CREATE INDEX parts_bin ON parts(bin_id);",
};

// Leading and trailing spaces (and tabs/newlines) are dropped from every text
// field, so " 10k" and "10k" don't end up as two values in a filter list.
// ---- Search helpers ----------------------------------------------------
//
// SQLite's own LIKE and NOCASE ignore case only for A-Z, so "резистор" would
// not find "Резистор" and "ñ" would not find "Ñ". The two SQL functions below
// fold case themselves for the alphabets the UI is used in (English, Spanish,
// Russian): sb_fold(text) for sorting and exact filters, sb_match(words,
// field, ...) for the search.

// Lower-cases ASCII, Latin-1 (U+00C0-U+00DE: À..Þ, including Ñ) and Cyrillic
// (U+0400-U+042F: Ѐ..Џ, А..Я) letters in UTF-8 text. Every letter it touches
// keeps its byte length, so the result is the same size as the input. Other
// characters are copied as they are.
void foldCase(const unsigned char *in, size_t n, std::string &out) {
  out.clear();
  out.reserve(n);
  for (size_t i = 0; i < n; i++) {
    const unsigned char c = in[i];
    if (c >= 'A' && c <= 'Z') {
      out += static_cast<char>(c + 32);
      continue;
    }
    // Two-byte letters start with 0xC3 (Latin-1) or 0xD0 (Cyrillic capitals).
    // A continuation byte (0x80-0xBF) is never one of those lead bytes, so
    // the loop can't misread the middle of another character as a letter.
    if (i + 1 < n) {
      const unsigned char d = in[i + 1];
      if (c == 0xC3 && d >= 0x80 && d <= 0x9E && d != 0x97) {  // À..Þ, not × (U+00D7)
        out += static_cast<char>(c);
        out += static_cast<char>(d + 0x20);  // à..þ
        i++;
        continue;
      }
      if (c == 0xD0 && d >= 0x80 && d <= 0xAF) {
        if (d >= 0x90 && d <= 0x9F) {  // А..П -> а..п (U+0430-043F, still 0xD0 0xB0-0xBF)
          out += static_cast<char>(0xD0);
          out += static_cast<char>(d + 0x20);
        } else if (d >= 0xA0) {  // Р..Я -> р..я (U+0440-044F: 0xD1 0x80-0x8F)
          out += static_cast<char>(0xD1);
          out += static_cast<char>(d - 0x20);
        } else {  // Ѐ..Џ, including Ё -> ѐ..џ (U+0450-045F: 0xD1 0x90-0x9F)
          out += static_cast<char>(0xD1);
          out += static_cast<char>(d + 0x10);
        }
        i++;
        continue;
      }
    }
    out += static_cast<char>(c);
  }
}

std::string folded(const std::string &s) {
  std::string out;
  foldCase(reinterpret_cast<const unsigned char *>(s.data()), s.size(), out);
  return out;
}

// sb_fold(text): the text with case folded as above.
void sqlFold(sqlite3_context *ctx, int, sqlite3_value **argv) {
  const unsigned char *t = sqlite3_value_text(argv[0]);
  if (!t) return sqlite3_result_null(ctx);
  std::string out;
  foldCase(t, static_cast<size_t>(sqlite3_value_bytes(argv[0])), out);
  sqlite3_result_text(ctx, out.data(), static_cast<int>(out.size()), SQLITE_TRANSIENT);
}

// At most this many search words count; more would only slow every row down.
constexpr size_t kMaxSearchWords = 8;

// The search words: folded, split at spaces, empty ones dropped.
std::vector<std::string> *searchWords(const unsigned char *text, size_t n) {
  auto *words = new std::vector<std::string>();
  std::string all;
  foldCase(text, n, all);
  size_t pos = 0;
  while (pos < all.size() && words->size() < kMaxSearchWords) {
    const size_t start = all.find_first_not_of(" \t", pos);
    if (start == std::string::npos) break;
    size_t end = all.find_first_of(" \t", start);
    if (end == std::string::npos) end = all.size();
    words->push_back(all.substr(start, end - start));
    pos = end;
  }
  return words;
}

// sb_match(words, field1, field2, ...): 1 when every search word is found in
// at least one of the fields, else 0.
void sqlMatch(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  // The words are the same for every row of a query, so they are split once
  // and kept with the statement as SQLite's "auxiliary data" for argument 0.
  // SQLite may drop it at any time (it then calls the deleter), so it is
  // fetched again after being set rather than trusting the pointer.
  auto *words = static_cast<std::vector<std::string> *>(sqlite3_get_auxdata(ctx, 0));
  std::unique_ptr<std::vector<std::string>> local;
  if (!words) {
    const unsigned char *t = sqlite3_value_text(argv[0]);
    local.reset(searchWords(t ? t : reinterpret_cast<const unsigned char *>(""),
                            static_cast<size_t>(sqlite3_value_bytes(argv[0]))));
    sqlite3_set_auxdata(ctx, 0, new std::vector<std::string>(*local),
                        [](void *p) { delete static_cast<std::vector<std::string> *>(p); });
    words = local.get();
  }
  // All fields folded into one string, each followed by a separator (0x01,
  // which no search word contains), so a word can't match across the end of
  // one field and the start of the next. The buffers are static so a search
  // over thousands of rows doesn't allocate and free memory for every row;
  // that is safe because SQLite here is single-threaded and every query runs
  // under the database mutex.
  static std::string haystack, field;
  haystack.clear();
  for (int i = 1; i < argc; i++) {
    const unsigned char *t = sqlite3_value_text(argv[i]);
    if (t) {
      foldCase(t, static_cast<size_t>(sqlite3_value_bytes(argv[i])), field);
      haystack += field;
    }
    haystack += '\x01';
  }
  for (const std::string &w : *words) {
    if (haystack.find(w) == std::string::npos) return sqlite3_result_int(ctx, 0);
  }
  sqlite3_result_int(ctx, 1);
}

// sb_tally(condition): the condition itself, counting in InventoryDb::tally_
// how many rows it was true for (see findParts()). Not DETERMINISTIC: it must
// run once for every row, never be folded or skipped.
void sqlTally(sqlite3_context *ctx, int, sqlite3_value **argv) {
  const int yes = sqlite3_value_int(argv[0]) != 0;
  if (yes) ++*static_cast<int64_t *>(sqlite3_user_data(ctx));
  sqlite3_result_int(ctx, yes);
}

// Database columns findParts() sorts by, in SortKey order. Text columns sort
// with case folded and empty values last, whichever the direction.
struct SortColumn {
  const char *expr;
  bool text;
};
const SortColumn kSortColumns[] = {
    {"p.id", false},          {"p.part_number", true}, {"p.manufacturer", true}, {"p.category", true},
    {"p.description", true},  {"p.package", true},     {"p.value", true},        {"p.quantity", false},
    {nullptr, false},  // Bin: see findParts()
    {"p.updated_at", false},
};
static_assert(sizeof(kSortColumns) / sizeof(kSortColumns[0]) == static_cast<int>(SortKey::Updated) + 1,
              "one entry per SortKey");

std::string trimmed(const std::string &s) {
  const char *ws = " \t\r\n";
  const size_t a = s.find_first_not_of(ws);
  if (a == std::string::npos) return std::string();
  const size_t b = s.find_last_not_of(ws);
  return s.substr(a, b - a + 1);
}

int64_t elapsedMs(std::chrono::steady_clock::time_point since) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count();
}

// ---------------------------------------------------------------------------
// File layer fix for the ESP32 SQLite library (Sqlite3Esp32).
//
// SQLite hands every PRAGMA to the file layer first (SQLITE_FCNTL_PRAGMA), so
// a file layer can implement pragmas of its own. Answering SQLITE_OK means
// "handled"; SQLITE_NOTFOUND means "not mine, SQLite does it". The library's
// xFileControl answers SQLITE_OK to every request, so SQLite treats every
// PRAGMA as already handled and skips it: "PRAGMA user_version" returns no
// row, and page_size, journal_mode, synchronous etc. silently do nothing.
//
// Instead of patching the library, the database is opened through a thin
// wrapper of its file layer ("sb-fixed"): the same layer in every respect,
// except that opened files get a copy of its method table whose xFileControl
// answers SQLITE_NOTFOUND, which is what SQLite expects for requests a file
// layer doesn't implement.
//
// Only built for the board (or with SB_SQLITE_VFS_FIX, to test it on a PC
// against the library's file layer); the host's own SQLite file layer handles
// file controls correctly and must not be wrapped.
#if defined(ARDUINO) || defined(SB_SQLITE_VFS_FIX)
#define SB_HAS_VFS_FIX 1
const char *const kFixedVfsName = "sb-fixed";
sqlite3_vfs *gBaseVfs = nullptr;                    // the library's file layer
sqlite3_vfs gFixedVfs;                              // the wrapper
const sqlite3_io_methods *gBaseMethods = nullptr;   // the library's file methods
sqlite3_io_methods gFixedMethods;                   // the same with xFileControl fixed

int fixedFileControl(sqlite3_file *, int, void *) { return SQLITE_NOTFOUND; }

int fixedOpen(sqlite3_vfs *, const char *name, sqlite3_file *file, int flags, int *outFlags) {
  const int rc = gBaseVfs->xOpen(gBaseVfs, name, file, flags, outFlags);
  // The library uses one method table for every file. Copy it the first time
  // and point each opened file at the copy. A file with some other table
  // (none today) is left alone rather than given methods that aren't its own.
  // Even after a failed open SQLite calls pMethods->xClose if it is set, and
  // the copy keeps the library's xClose, so swapping is safe in both cases.
  if (file->pMethods) {
    if (!gBaseMethods) {
      gBaseMethods = file->pMethods;
      gFixedMethods = *gBaseMethods;
      gFixedMethods.xFileControl = fixedFileControl;
    }
    if (file->pMethods == gBaseMethods) file->pMethods = &gFixedMethods;
  }
  return rc;
}

// Registers the wrapper once; later calls just return its name.
const char *fixedVfs() {
  if (!gBaseVfs) {
    gBaseVfs = sqlite3_vfs_find(nullptr);  // the default: the library's layer
    if (!gBaseVfs) return nullptr;
    gFixedVfs = *gBaseVfs;  // same sizes, paths, time and randomness functions
    gFixedVfs.pNext = nullptr;
    gFixedVfs.zName = kFixedVfsName;
    gFixedVfs.xOpen = fixedOpen;
    sqlite3_vfs_register(&gFixedVfs, 0);  // 0: not the default, used by name
  }
  return kFixedVfsName;
}
#endif

// The WHERE conditions for a PartQuery's filters (search words, category,
// manufacturer, bin), joined with AND; empty when no filter is set. The
// conditions refer to the parts table as "p". Their parameters are numbered
// (?1, ?2, ...) in the order they are appended to `params`, so a caller binds
// params[i] to i + 1. Shared by findParts() and countByBin(), so both apply
// exactly the same filters.
std::string partFilter(const PartQuery &query, std::vector<std::string> &params) {
  std::string where;
  auto add = [&](const std::string &clause) { where += (where.empty() ? "" : " AND ") + clause; };
  auto param = [&](const std::string &value) {
    params.push_back(value);
    return "?" + std::to_string(params.size());
  };
  if (query.search.find_first_not_of(" \t") != std::string::npos) {
    std::string fields;
    for (const TextFieldInfo &f : kTextFields) fields += std::string(", p.") + f.column;
    add("sb_match(" + param(query.search) + fields + ")");
  }
  if (!query.category.empty()) add("sb_fold(p.category) = " + param(folded(trimmed(query.category))));
  if (!query.manufacturer.empty()) {
    add("sb_fold(p.manufacturer) = " + param(folded(trimmed(query.manufacturer))));
  }
  if (query.bin) {
    if (query.bin->valid()) {
      add("p.bin_id = (SELECT id FROM bins WHERE x = " + param(std::to_string(query.bin->x)) +
          " AND y = " + param(std::to_string(query.bin->y)) + ")");
    } else {
      add("p.bin_id IS NULL");
    }
  }
  return where;
}

}  // namespace

const TextFieldInfo &textField(int field) { return kTextFields[field]; }

// A prepared statement, finalized when it goes out of scope. Bind indexes
// start at 1 and column indexes at 0, as in the SQLite API.
class InventoryDb::Stmt {
 public:
  Stmt(sqlite3 *db, const char *sql) { rc_ = sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr); }
  ~Stmt() { sqlite3_finalize(stmt_); }
  Stmt(const Stmt &) = delete;
  Stmt &operator=(const Stmt &) = delete;

  bool ok() const { return rc_ == SQLITE_OK || rc_ == SQLITE_ROW || rc_ == SQLITE_DONE; }

  void bind(int i, int64_t v) { sqlite3_bind_int64(stmt_, i, v); }
  // SQLITE_TRANSIENT: SQLite copies the text, so the string may go away.
  void bind(int i, const std::string &v) { sqlite3_bind_text(stmt_, i, v.data(), (int)v.size(), SQLITE_TRANSIENT); }
  void bindNull(int i) { sqlite3_bind_null(stmt_, i); }
  // 0 means "unknown" for times, stored as NULL.
  void bindTime(int i, int64_t t) { t ? bind(i, t) : bindNull(i); }

  // SQLITE_ROW while rows come, SQLITE_DONE at the end, anything else is an error.
  int step() { return rc_ = sqlite3_step(stmt_); }
  void reset() {
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
  }

  int64_t integer(int col) { return sqlite3_column_int64(stmt_, col); }
  bool isNull(int col) { return sqlite3_column_type(stmt_, col) == SQLITE_NULL; }
  std::string text(int col) {
    const unsigned char *t = sqlite3_column_text(stmt_, col);
    return t ? std::string(reinterpret_cast<const char *>(t), sqlite3_column_bytes(stmt_, col)) : std::string();
  }

 private:
  sqlite3_stmt *stmt_ = nullptr;
  int rc_;
};

// A write transaction: BEGIN IMMEDIATE on construction (inside the write
// hooks), ROLLBACK on destruction unless commit() succeeded. Taking the write
// lock up front means a transaction never fails half way for lack of it.
class InventoryDb::WriteTxn {
 public:
  explicit WriteTxn(InventoryDb &db) : db_(db) {
    if (db_.hooks_.begin) db_.hooks_.begin();
    std::string err;
    began_ = db_.exec("BEGIN IMMEDIATE", err);
  }
  ~WriteTxn() {
    if (began_ && !committed_) {
      std::string err;
      db_.exec("ROLLBACK", err);
    }
    if (db_.hooks_.end) db_.hooks_.end();
  }
  bool began() const { return began_; }
  bool commit(std::string &err) { return committed_ = db_.exec("COMMIT", err); }

 private:
  InventoryDb &db_;
  bool began_ = false;
  bool committed_ = false;
};

std::string InventoryDb::lastError() const { return db_ ? sqlite3_errmsg(db_) : "database not open"; }

bool InventoryDb::exec(const char *sql, std::string &err) {
  char *msg = nullptr;
  if (sqlite3_exec(db_, sql, nullptr, nullptr, &msg) == SQLITE_OK) return true;
  err = msg ? msg : lastError();
  sqlite3_free(msg);
  return false;
}

bool InventoryDb::open(const char *path, std::string &err, int cacheKiB) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  close();
#ifdef SB_HAS_VFS_FIX
  const char *vfs = fixedVfs();  // see "File layer fix" at the top
#else
  const char *vfs = nullptr;  // SQLite's default
#endif
  if (sqlite3_open_v2(path, &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, vfs) != SQLITE_OK) {
    err = lastError();
    close();
    return false;
  }
  // The search functions (see "Search helpers"). DETERMINISTIC: same input,
  // same result, so SQLite may evaluate them once where it can.
  const int flags = SQLITE_UTF8 | SQLITE_DETERMINISTIC;
  if (sqlite3_create_function(db_, "sb_fold", 1, flags, nullptr, sqlFold, nullptr, nullptr) != SQLITE_OK ||
      sqlite3_create_function(db_, "sb_match", -1, flags, nullptr, sqlMatch, nullptr, nullptr) != SQLITE_OK ||
      sqlite3_create_function(db_, "sb_tally", 1, SQLITE_UTF8, &tally_, sqlTally, nullptr, nullptr) != SQLITE_OK) {
    err = lastError();
    close();
    return false;
  }
  // Lookaside: small per-connection slots for short-lived allocations. The
  // ESP32 library is built without lookaside (SQLITE_OMIT_LOOKASIDE), so this
  // only matters for the PC build of the tests, where it keeps memory use
  // closer to the board's. Must be set before the connection runs any
  // statement.
  sqlite3_db_config(db_, SQLITE_DBCONFIG_LOOKASIDE, nullptr, 128, 64);

  // page_size: see kPageSize. It only takes effect on a new, empty file; an
  // older file with other pages is converted below.
  // cache_size < 0 is in KiB: the RAM the page cache may use.
  // journal_mode PERSIST keeps the rollback journal file and only clears its
  // header after a commit: fewer directory updates on flash than deleting and
  // re-creating it each time. journal_size_limit stops one big transaction
  // (a restore, the benchmark) from leaving a large journal behind.
  // synchronous FULL (the default): a commit is on flash when it returns.
  char pragmas[256];
  snprintf(pragmas, sizeof(pragmas),
           "PRAGMA page_size = %d; PRAGMA cache_size = -%d; PRAGMA journal_mode = PERSIST;"
           "PRAGMA journal_size_limit = 131072; PRAGMA foreign_keys = ON; PRAGMA temp_store = MEMORY;"
           "PRAGMA synchronous = FULL;",
           kPageSize, cacheKiB);
  if (!exec(pragmas, err) || !pragmasApplied(err) || !migrate(err) || !convertPageSize(err)) {
    close();
    return false;
  }
  return true;
}

// Files made by firmware 0.5.0 before this change use 4 KB pages. VACUUM
// rewrites the file with the page_size set above, but it builds the new copy
// in RAM (temp_store = MEMORY), so it only runs while the file is still
// small: an empty or nearly empty inventory, which is what any board has at
// this stage of the project. A bigger file keeps its pages and works as
// before, only with the higher RAM peaks of 4 KB pages.
bool InventoryDb::convertPageSize(std::string &err) {
  int64_t pageSize = 0, pages = 0;
  {
    Stmt size(db_, "PRAGMA page_size");
    Stmt count(db_, "PRAGMA page_count");
    if (size.step() != SQLITE_ROW || count.step() != SQLITE_ROW) return err = lastError(), false;
    pageSize = size.integer(0);
    pages = count.integer(0);
  }
  if (pageSize == kPageSize) return true;
  constexpr int64_t kMaxConvertBytes = 64 * 1024;
  if (pageSize * pages > kMaxConvertBytes) return true;
  return exec("VACUUM", err);
}

// Reads one of the settings back. A file layer that swallows pragmas (see
// "File layer fix" at the top) would otherwise go unnoticed: SQLite ignores
// an unhandled PRAGMA without an error, and the database would run with the
// wrong page size and journal mode.
bool InventoryDb::pragmasApplied(std::string &err) {
  Stmt q(db_, "PRAGMA journal_mode");
  if (q.step() == SQLITE_ROW && q.text(0) == "persist") return true;
  err = "SQLite ignored the PRAGMA settings (file layer problem)";
  return false;
}

void InventoryDb::close() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (db_) sqlite3_close(db_);
  db_ = nullptr;
}

bool InventoryDb::migrate(std::string &err) {
  Stmt q(db_, "PRAGMA user_version");
  if (q.step() != SQLITE_ROW) {
    err = lastError();
    return false;
  }
  const int version = static_cast<int>(q.integer(0));
  if (version > kSchemaVersion) {
    err = "database is from a newer firmware (schema " + std::to_string(version) + ")";
    return false;
  }
  for (int v = version; v < kSchemaVersion; v++) {
    WriteTxn txn(*this);
    // PRAGMA can't take a bound parameter, hence the formatted statement.
    char bump[40];
    snprintf(bump, sizeof(bump), "PRAGMA user_version = %d", v + 1);
    if (!txn.began() || !exec(kMigrations[v], err) || !exec(bump, err) || !txn.commit(err)) {
      err = "schema upgrade to version " + std::to_string(v + 1) + " failed: " + err;
      return false;
    }
  }
  return true;
}

Result InventoryDb::syncBins(int cols, int rows, std::string &err) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!db_) return err = lastError(), Result::Error;
  WriteTxn txn(*this);
  if (!txn.began()) return err = lastError(), Result::Error;
  // INSERT OR IGNORE: bins that already exist (with their labels) stay as they are.
  Stmt add(db_, "INSERT OR IGNORE INTO bins (x, y) VALUES (?, ?)");
  for (int y = 0; y < rows; y++) {
    for (int x = 0; x < cols; x++) {
      add.bind(1, x);
      add.bind(2, y);
      if (add.step() != SQLITE_DONE) return err = lastError(), Result::Error;
      add.reset();
    }
  }
  // Outside the grid: drop empty bins, flag the ones still holding parts so
  // the UI can show where those parts are.
  Stmt drop(db_,
            "DELETE FROM bins WHERE (x >= ?1 OR y >= ?2)"
            " AND NOT EXISTS (SELECT 1 FROM parts WHERE parts.bin_id = bins.id)");
  Stmt flag(db_, "UPDATE bins SET in_grid = (x < ?1 AND y < ?2)");
  for (Stmt *s : {&drop, &flag}) {
    s->bind(1, cols);
    s->bind(2, rows);
    if (s->step() != SQLITE_DONE) return err = lastError(), Result::Error;
  }
  return txn.commit(err) ? Result::Ok : Result::Error;
}

bool InventoryDb::validate(const PartChanges &fields, bool creating, std::string &err) const {
  for (int f = 0; f < kTextFieldCount; f++) {
    if (fields.text[f] && fields.text[f]->size() > kTextFields[f].maxLen) {
      err = std::string(kTextFields[f].key) + " is longer than " + std::to_string(kTextFields[f].maxLen) + " bytes";
      return false;
    }
  }
  if (fields.quantity && (*fields.quantity < 0 || *fields.quantity > kMaxQuantity)) {
    err = "quantity must be 0 to " + std::to_string(kMaxQuantity);
    return false;
  }
  // A part with neither a part number nor a description can't be found again.
  if (creating) {
    const bool named = (fields.text[kPartNumber] && !fields.text[kPartNumber]->empty()) ||
                       (fields.text[kDescription] && !fields.text[kDescription]->empty());
    if (!named) {
      err = "a part needs a part number or a description";
      return false;
    }
  }
  return true;
}

Result InventoryDb::binId(BinRef bin, int64_t &id, std::string &err) {
  Stmt q(db_, "SELECT id FROM bins WHERE x = ? AND y = ?");
  q.bind(1, bin.x);
  q.bind(2, bin.y);
  const int rc = q.step();
  if (rc == SQLITE_ROW) {
    id = q.integer(0);
    return Result::Ok;
  }
  if (rc == SQLITE_DONE) {
    err = "there is no bin at " + std::to_string(bin.x) + "," + std::to_string(bin.y);
    return Result::Invalid;
  }
  err = lastError();
  return Result::Error;
}

// Fills a Part from the current row of a kPartSelect query.
void InventoryDb::rowToPart(Stmt &q, Part &out) {
  out = Part();
  out.id = q.integer(0);
  for (int f = 0; f < kTextFieldCount; f++) out.text[f] = q.text(1 + f);
  out.quantity = static_cast<int32_t>(q.integer(10));
  if (!q.isNull(11)) out.bin = {static_cast<int>(q.integer(11)), static_cast<int>(q.integer(12))};
  out.photo = q.text(13);
  out.createdAt = q.integer(14);
  out.updatedAt = q.integer(15);
}

Result InventoryDb::readPart(int64_t id, Part &out, std::string &err) {
  std::string sql = std::string(kPartSelect) + " WHERE p.id = ?";
  Stmt q(db_, sql.c_str());
  q.bind(1, id);
  const int rc = q.step();
  if (rc == SQLITE_DONE) {
    err = "no part with id " + std::to_string(id);
    return Result::NotFound;
  }
  if (rc != SQLITE_ROW) return err = lastError(), Result::Error;
  rowToPart(q, out);
  return Result::Ok;
}

Result InventoryDb::addPart(const PartChanges &in, Part &out, std::string &err) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!db_) return err = lastError(), Result::Error;
  PartChanges fields = in;
  for (auto &t : fields.text) {
    if (t) *t = trimmed(*t);
  }
  if (!validate(fields, true, err)) return Result::Invalid;
  int64_t bin = 0;
  if (fields.bin && fields.bin->valid()) {
    const Result r = binId(*fields.bin, bin, err);
    if (r != Result::Ok) return r;
  }

  WriteTxn txn(*this);
  if (!txn.began()) return err = lastError(), Result::Error;
  // Columns: the text fields in TextField order, then quantity, bin_id and times.
  std::string sql = "INSERT INTO parts (";
  for (const TextFieldInfo &f : kTextFields) sql += std::string(f.column) + ", ";
  sql += "quantity, bin_id, created_at, updated_at) VALUES (";
  for (int i = 0; i < kTextFieldCount + 4; i++) sql += i ? ", ?" : "?";
  sql += ")";
  Stmt ins(db_, sql.c_str());
  for (int f = 0; f < kTextFieldCount; f++) ins.bind(1 + f, fields.text[f].value_or(std::string()));
  ins.bind(kTextFieldCount + 1, fields.quantity.value_or(0));
  if (bin) {
    ins.bind(kTextFieldCount + 2, bin);
  } else {
    ins.bindNull(kTextFieldCount + 2);
  }
  const int64_t t = now();
  ins.bindTime(kTextFieldCount + 3, t);
  ins.bindTime(kTextFieldCount + 4, t);
  if (ins.step() != SQLITE_DONE) return err = lastError(), Result::Error;
  const int64_t id = sqlite3_last_insert_rowid(db_);
  if (!txn.commit(err)) return Result::Error;
  return readPart(id, out, err);
}

Result InventoryDb::getPart(int64_t id, Part &out, std::string &err) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!db_) return err = lastError(), Result::Error;
  return readPart(id, out, err);
}

Result InventoryDb::updatePart(int64_t id, const PartChanges &changes, Part &out, std::string &err) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!db_) return err = lastError(), Result::Error;
  // Read, merge, validate the merged part, write it back whole: the same
  // checks then apply as for a new part (a part number or description must
  // remain), and one UPDATE statement serves every combination of fields.
  Part cur;
  Result r = readPart(id, cur, err);
  if (r != Result::Ok) return r;
  PartChanges merged;
  for (int f = 0; f < kTextFieldCount; f++) {
    merged.text[f] = changes.text[f] ? trimmed(*changes.text[f]) : cur.text[f];
  }
  merged.quantity = changes.quantity.value_or(cur.quantity);
  merged.bin = changes.bin.value_or(cur.bin);
  if (!validate(merged, true, err)) return Result::Invalid;
  int64_t bin = 0;
  if (merged.bin->valid()) {
    r = binId(*merged.bin, bin, err);
    if (r != Result::Ok) return r;
  }

  WriteTxn txn(*this);
  if (!txn.began()) return err = lastError(), Result::Error;
  std::string sql = "UPDATE parts SET ";
  for (const TextFieldInfo &f : kTextFields) sql += std::string(f.column) + " = ?, ";
  sql += "quantity = ?, bin_id = ?, updated_at = ? WHERE id = ?";
  Stmt upd(db_, sql.c_str());
  for (int f = 0; f < kTextFieldCount; f++) upd.bind(1 + f, *merged.text[f]);
  upd.bind(kTextFieldCount + 1, *merged.quantity);
  if (bin) {
    upd.bind(kTextFieldCount + 2, bin);
  } else {
    upd.bindNull(kTextFieldCount + 2);
  }
  upd.bindTime(kTextFieldCount + 3, now());
  upd.bind(kTextFieldCount + 4, id);
  if (upd.step() != SQLITE_DONE) return err = lastError(), Result::Error;
  if (!txn.commit(err)) return Result::Error;
  return readPart(id, out, err);
}

Result InventoryDb::deletePart(int64_t id, std::string &err) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!db_) return err = lastError(), Result::Error;
  WriteTxn txn(*this);
  if (!txn.began()) return err = lastError(), Result::Error;
  Stmt del(db_, "DELETE FROM parts WHERE id = ?");
  del.bind(1, id);
  if (del.step() != SQLITE_DONE) return err = lastError(), Result::Error;
  if (sqlite3_changes(db_) == 0) {
    err = "no part with id " + std::to_string(id);
    return Result::NotFound;  // the destructor rolls the empty transaction back
  }
  return txn.commit(err) ? Result::Ok : Result::Error;
}

Result InventoryDb::adjustQuantity(int64_t id, int32_t delta, Part &out, std::string &err) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!db_) return err = lastError(), Result::Error;
  {
    WriteTxn txn(*this);
    if (!txn.began()) return err = lastError(), Result::Error;
    // max(0, min(limit, ...)): clamped in SQL, so the read and the write are
    // one step and the stored value always passes the CHECK constraint.
    Stmt upd(db_, "UPDATE parts SET quantity = max(0, min(?1, quantity + ?2)), updated_at = ?3 WHERE id = ?4");
    upd.bind(1, kMaxQuantity);
    upd.bind(2, delta);
    upd.bindTime(3, now());
    upd.bind(4, id);
    if (upd.step() != SQLITE_DONE) return err = lastError(), Result::Error;
    if (sqlite3_changes(db_) == 0) {
      err = "no part with id " + std::to_string(id);
      return Result::NotFound;
    }
    if (!txn.commit(err)) return Result::Error;
  }
  return readPart(id, out, err);
}

Result InventoryDb::listParts(int offset, int limit, const std::function<void(const Part &)> &each, int64_t &total,
                              std::string &err) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!db_) return err = lastError(), Result::Error;
  Stmt count(db_, "SELECT count(*) FROM parts");
  if (count.step() != SQLITE_ROW) return err = lastError(), Result::Error;
  total = count.integer(0);

  std::string sql = std::string(kPartSelect) + " ORDER BY p.id LIMIT ? OFFSET ?";
  Stmt q(db_, sql.c_str());
  q.bind(1, limit);
  q.bind(2, offset);
  Part part;
  int rc;
  while ((rc = q.step()) == SQLITE_ROW) {
    rowToPart(q, part);
    each(part);
  }
  if (rc != SQLITE_DONE) return err = lastError(), Result::Error;
  return Result::Ok;
}

Result InventoryDb::findParts(const PartQuery &query, const std::function<void(const Part &)> &each,
                              int64_t &total, std::string &err) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!db_) return err = lastError(), Result::Error;

  // The WHERE clause and its parameters, from the filters that are set.
  std::vector<std::string> params;
  const std::string where = partFilter(query, params);
  auto bindAll = [&](Stmt &q) {
    for (size_t i = 0; i < params.size(); i++) q.bind(static_cast<int>(i + 1), params[i]);
  };

  // ORDER BY, always ending in the id so equal keys come in a stable order
  // and pages don't overlap or skip parts.
  const char *dir = query.descending ? " DESC" : " ASC";
  std::string order;
  const int key = static_cast<int>(query.sort);
  if (key < 0 || key > static_cast<int>(SortKey::Updated)) return err = "unknown sort order", Result::Invalid;
  if (query.sort == SortKey::Bin) {
    // Row by row, then column, like the grid; parts in no bin last.
    order = std::string("(b.id IS NULL), b.y") + dir + ", b.x" + dir;
  } else if (kSortColumns[key].text) {
    const std::string col = kSortColumns[key].expr;
    order = "(" + col + " = ''), sb_fold(" + col + ")" + dir;
  } else if (query.sort != SortKey::Id) {
    order = std::string(kSortColumns[key].expr) + dir;
  }
  // The unary + in "+p.id" stops SQLite from reading the table in id order
  // and stopping after one page: with filters, every row has to pass through
  // sb_tally() below so the total comes out of the same scan.
  order += (order.empty() ? "+p.id" : ", p.id") + std::string(dir);

  // Step 1: the ids of the requested page, and the number of matches.
  //
  // Only the sort key and the id go through SQLite's sorter (it keeps at most
  // offset + limit of them), so a search over thousands of parts doesn't hold
  // whole records in RAM.
  //
  // The total comes from the same pass: the conditions are wrapped in
  // sb_tally(), which counts the rows they accept. The search reads every
  // row and runs the case-folding match on it, which takes a noticeable part
  // of a second on the board with a few thousand parts; a separate
  // count(*) query would do all of that a second time.
  std::vector<int64_t> ids;
  {
    tally_ = 0;
    const std::string sql = "SELECT p.id FROM parts p LEFT JOIN bins b ON b.id = p.bin_id" +
                            (where.empty() ? std::string() : " WHERE sb_tally(" + where + ")") + " ORDER BY " +
                            order + " LIMIT " + std::to_string(query.limit) + " OFFSET " +
                            std::to_string(query.offset);
    Stmt q(db_, sql.c_str());
    if (!q.ok()) return err = lastError(), Result::Error;
    bindAll(q);
    int rc;
    while ((rc = q.step()) == SQLITE_ROW) ids.push_back(q.integer(0));
    if (rc != SQLITE_DONE) return err = lastError(), Result::Error;
  }
  if (!where.empty()) {
    total = tally_;
  } else {
    // No filters: every part matches, and SQLite counts the table quickly.
    Stmt q(db_, "SELECT count(*) FROM parts");
    if (q.step() != SQLITE_ROW) return err = lastError(), Result::Error;
    total = q.integer(0);
  }

  // Step 2: the parts themselves, one at a time.
  const std::string sql = std::string(kPartSelect) + " WHERE p.id = ?";
  Stmt q(db_, sql.c_str());
  Part part;
  for (int64_t id : ids) {
    q.bind(1, id);
    if (q.step() == SQLITE_ROW) {
      rowToPart(q, part);
      each(part);
    }
    q.reset();
  }
  return Result::Ok;
}

Result InventoryDb::countByBin(const PartQuery &query, const std::function<void(const BinCount &)> &each,
                               std::string &err) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!db_) return err = lastError(), Result::Error;
  // Same filters as findParts(); sort, offset and limit don't apply. One
  // pass over the parts, grouped by bin; parts in no bin form one group whose
  // b.x and b.y are NULL.
  std::vector<std::string> params;
  const std::string where = partFilter(query, params);
  const std::string sql = "SELECT b.x, b.y, count(*) FROM parts p LEFT JOIN bins b ON b.id = p.bin_id" +
                          (where.empty() ? std::string() : " WHERE " + where) + " GROUP BY p.bin_id";
  Stmt q(db_, sql.c_str());
  if (!q.ok()) return err = lastError(), Result::Error;
  for (size_t i = 0; i < params.size(); i++) q.bind(static_cast<int>(i + 1), params[i]);
  int rc;
  while ((rc = q.step()) == SQLITE_ROW) {
    BinCount c;
    if (q.isNull(0)) {
      c.bin = {-1, -1};
    } else {
      c.bin = {static_cast<int>(q.integer(0)), static_cast<int>(q.integer(1))};
    }
    c.parts = q.integer(2);
    each(c);
  }
  if (rc != SQLITE_DONE) return err = lastError(), Result::Error;
  return Result::Ok;
}

Result InventoryDb::listValues(TextField field, const std::string &contains, int limit,
                               const std::function<void(const FieldValue &)> &each, std::string &err) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!db_) return err = lastError(), Result::Error;
  if (field < 0 || field >= kTextFieldCount) return err = "unknown field", Result::Invalid;
  const std::string col = kTextFields[field].column;
  const bool narrow = contains.find_first_not_of(" \t") != std::string::npos;
  // Grouped by the exact value: "Resistor" and "resistor" are listed apart,
  // so the list shows what is really stored.
  const std::string sql = "SELECT " + col + ", count(*) FROM parts WHERE " + col + " <> ''" +
                          (narrow ? " AND sb_match(?1, " + col + ")" : std::string()) + " GROUP BY " + col +
                          " ORDER BY count(*) DESC, sb_fold(" + col + ") LIMIT " + std::to_string(limit);
  Stmt q(db_, sql.c_str());
  if (narrow) q.bind(1, contains);
  FieldValue v;
  int rc;
  while ((rc = q.step()) == SQLITE_ROW) {
    v.value = q.text(0);
    v.parts = q.integer(1);
    each(v);
  }
  if (rc != SQLITE_DONE) return err = lastError(), Result::Error;
  return Result::Ok;
}

Result InventoryDb::listBins(const std::function<void(const BinInfo &)> &each, std::string &err) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!db_) return err = lastError(), Result::Error;
  // Row by row (y, then x), the order a grid is drawn in.
  Stmt q(db_,
         "SELECT b.x, b.y, b.label, count(p.id), b.in_grid FROM bins b"
         " LEFT JOIN parts p ON p.bin_id = b.id GROUP BY b.id ORDER BY b.y, b.x");
  int rc;
  BinInfo bin;
  while ((rc = q.step()) == SQLITE_ROW) {
    bin.x = static_cast<int>(q.integer(0));
    bin.y = static_cast<int>(q.integer(1));
    bin.label = q.text(2);
    bin.parts = static_cast<int>(q.integer(3));
    bin.inGrid = q.integer(4) != 0;
    each(bin);
  }
  if (rc != SQLITE_DONE) return err = lastError(), Result::Error;
  return Result::Ok;
}

Result InventoryDb::setBinLabel(BinRef bin, const std::string &rawLabel, std::string &err) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!db_) return err = lastError(), Result::Error;
  const std::string label = trimmed(rawLabel);
  if (label.size() > kMaxBinLabel) {
    err = "label is longer than " + std::to_string(kMaxBinLabel) + " bytes";
    return Result::Invalid;
  }
  int64_t id;
  const Result r = binId(bin, id, err);
  if (r != Result::Ok) return r;
  WriteTxn txn(*this);
  if (!txn.began()) return err = lastError(), Result::Error;
  Stmt upd(db_, "UPDATE bins SET label = ? WHERE id = ?");
  // An empty label is stored as NULL: "no label", the UI shows the A1 name.
  label.empty() ? upd.bindNull(1) : upd.bind(1, label);
  upd.bind(2, id);
  if (upd.step() != SQLITE_DONE) return err = lastError(), Result::Error;
  return txn.commit(err) ? Result::Ok : Result::Error;
}

Result InventoryDb::stats(Stats &out, std::string &err) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!db_) return err = lastError(), Result::Error;
  // One statement for all the counts: a single pass of each table, and the
  // numbers belong to the same moment.
  Stmt q(db_,
         "SELECT"
         " (SELECT count(*) FROM parts),"
         " (SELECT coalesce(sum(quantity), 0) FROM parts),"
         " (SELECT count(*) FROM parts WHERE photo IS NOT NULL AND photo <> ''),"
         " (SELECT count(DISTINCT category) FROM parts WHERE category <> ''),"
         " (SELECT count(DISTINCT manufacturer) FROM parts WHERE manufacturer <> ''),"
         " (SELECT count(*) FROM bins WHERE in_grid),"
         " (SELECT count(DISTINCT bin_id) FROM parts WHERE bin_id IS NOT NULL)");
  if (q.step() != SQLITE_ROW) return err = lastError(), Result::Error;
  out.parts = q.integer(0);
  out.totalQuantity = q.integer(1);
  out.photos = q.integer(2);
  out.categories = q.integer(3);
  out.manufacturers = q.integer(4);
  out.binsTotal = q.integer(5);
  out.binsUsed = q.integer(6);
  Stmt pages(db_, "PRAGMA page_count");
  Stmt size(db_, "PRAGMA page_size");
  if (pages.step() != SQLITE_ROW || size.step() != SQLITE_ROW) return err = lastError(), Result::Error;
  out.dbBytes = pages.integer(0) * size.integer(0);
  return Result::Ok;
}

// Removes the benchmark's parts, a batch at a time. One DELETE of every
// matching row makes SQLite hold far more RAM while it runs (on a PC about
// 60 KB above the page cache for 2,000 rows, against under 20 KB in batches),
// which a board with little free heap can't spare. All batches share one
// transaction, so the work on flash is the same.
Result InventoryDb::deleteBenchParts(std::string &err) {
  WriteTxn txn(*this);
  if (!txn.began()) return err = lastError(), Result::Error;
  Stmt del(db_, "DELETE FROM parts WHERE id IN (SELECT id FROM parts WHERE category = ? LIMIT 100)");
  // Each pass removes up to 100 rows; a pass that removes none means they're
  // all gone. The category is bound on every pass because reset() also
  // clears the bindings.
  int removed = 0;
  do {
    del.bind(1, std::string(kBenchCategory));
    if (del.step() != SQLITE_DONE) return err = lastError(), Result::Error;
    removed = sqlite3_changes(db_);
    del.reset();
  } while (removed > 0);
  return txn.commit(err) ? Result::Ok : Result::Error;
}

Result InventoryDb::benchmark(int count, int cols, int rows, BenchResult &out, std::string &err) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!db_) return err = lastError(), Result::Error;
  out = BenchResult();
  out.count = count;
  // Every failure below names the step it happened in ("insert: out of
  // memory"), so a report from the board says where to look.
  const char *phase = "cleanup";
  auto fail = [&]() {
    if (err.empty()) err = lastError();
    err = std::string(phase) + ": " + err;
    return Result::Error;
  };
  // Leftovers of an interrupted run would skew the numbers.
  if (deleteBenchParts(err) != Result::Ok) return fail();

  // Realistic-looking rows: a mix of manufacturers, values and packages, and
  // text in most fields, so the file grows as a real inventory would.
  static const char *const kMakers[] = {"Yageo", "Murata", "TI", "STMicro", "Vishay", "Nexperia", "Microchip", "Bourns"};
  static const char *const kKinds[] = {"Resistor", "Capacitor", "MCU", "Op-amp", "Diode", "Transistor", "Regulator"};
  static const char *const kPackages[] = {"0603", "0805", "SOT-23", "SOIC-8", "TO-220", "LQFP-48"};
  phase = "insert";
  auto start = std::chrono::steady_clock::now();
  {
    WriteTxn txn(*this);
    if (!txn.began()) return fail();
    Stmt ins(db_,
             "INSERT INTO parts (part_number, manufacturer, category, description, package, value, unit, notes,"
             " quantity, bin_id) VALUES (?, ?, ?, ?, ?, ?, 'pcs', ?, ?,"
             " (SELECT id FROM bins WHERE x = ? AND y = ?))");
    char buf[96];
    for (int i = 0; i < count; i++) {
      snprintf(buf, sizeof(buf), "BENCH-%05d", i);
      ins.bind(1, std::string(buf));
      ins.bind(2, std::string(kMakers[i % 8]));
      ins.bind(3, std::string(kBenchCategory));
      snprintf(buf, sizeof(buf), "%s number %d for the benchmark, general purpose", kKinds[i % 7], i);
      ins.bind(4, std::string(buf));
      ins.bind(5, std::string(kPackages[i % 6]));
      snprintf(buf, sizeof(buf), "%dk", i % 100);
      ins.bind(6, std::string(buf));
      ins.bind(7, std::string("Generated test data; removed when the benchmark ends."));
      ins.bind(8, i % 500);
      ins.bind(9, cols > 0 ? i % cols : 0);
      ins.bind(10, rows > 0 ? (i / (cols > 0 ? cols : 1)) % rows : 0);
      if (ins.step() != SQLITE_DONE) return fail();
      ins.reset();
    }
    if (!txn.commit(err)) return fail();
  }
  out.insertMs = elapsedMs(start);

  // Each query below sits in its own block so its prepared statement (several
  // KB each) is freed before the next step, instead of all of them staying
  // allocated until the end and adding up.
  phase = "count";
  start = std::chrono::steady_clock::now();
  {
    Stmt cnt(db_, "SELECT count(*) FROM parts");
    if (cnt.step() != SQLITE_ROW) return fail();
  }
  out.countMs = elapsedMs(start);

  // The global search as the web UI runs it (findParts: every word in any
  // text field, any case, first page of 50, sorted by part number). No index
  // helps a substring search, so every row is read.
  phase = "search";
  start = std::chrono::steady_clock::now();
  {
    PartQuery q;
    q.search = "murata";
    q.sort = SortKey::PartNumber;
    std::string e;
    if (findParts(q, [](const Part &) {}, out.found, e) != Result::Ok) return err = e, fail();
  }
  out.searchMs = elapsedMs(start);

  phase = "bin";
  start = std::chrono::steady_clock::now();
  {
    std::string binSql = std::string(kPartSelect) + " WHERE p.bin_id = (SELECT id FROM bins WHERE x = 0 AND y = 0)";
    Stmt inBin(db_, binSql.c_str());
    while (inBin.step() == SQLITE_ROW) {
    }
  }
  out.binMs = elapsedMs(start);

  phase = "get";
  start = std::chrono::steady_clock::now();
  {
    Part p;
    int64_t id = 0;
    {
      Stmt mid(db_, "SELECT id FROM parts WHERE category = ? LIMIT 1 OFFSET ?");
      mid.bind(1, std::string(kBenchCategory));
      mid.bind(2, count / 2);
      if (mid.step() == SQLITE_ROW) id = mid.integer(0);
    }
    if (id && readPart(id, p, err) != Result::Ok) return fail();
  }
  out.getMs = elapsedMs(start);

  phase = "delete";
  start = std::chrono::steady_clock::now();
  if (deleteBenchParts(err) != Result::Ok) return fail();
  out.deleteMs = elapsedMs(start);
  return Result::Ok;
}

}  // namespace inventory
}  // namespace sb
