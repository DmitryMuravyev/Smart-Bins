#include <algorithm>
// Inventory database tests. They run against the system's SQLite (libsqlite3)
// with a real file in /tmp, so reopening shows what survives a restart.

#include <unity.h>

#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "InventoryDb.h"

using namespace sb::inventory;

static const char *kPath = "/tmp/sb_test_inventory.db";
static InventoryDb db;
static std::string err;

static void removeFiles() {
  std::remove(kPath);
  std::remove((std::string(kPath) + "-journal").c_str());
}

void setUp() {
  db.close();
  removeFiles();
  err.clear();
  TEST_ASSERT_TRUE_MESSAGE(db.open(kPath, err), err.c_str());
  TEST_ASSERT_TRUE(db.syncBins(4, 3, err) == Result::Ok);
}

void tearDown() {
  db.close();
  removeFiles();
}

static PartChanges named(const char *partNumber, int quantity = 0, BinRef bin = BinRef()) {
  PartChanges c;
  c.text[kPartNumber] = std::string(partNumber);
  c.quantity = quantity;
  if (bin.valid()) c.bin = bin;
  return c;
}

static void test_add_and_get() {
  PartChanges c = named("  NE555 ", 25, {1, 2});
  c.text[kManufacturer] = std::string("TI");
  c.text[kNotes] = std::string("timer");
  Part p;
  TEST_ASSERT_TRUE_MESSAGE(db.addPart(c, p, err) == Result::Ok, err.c_str());
  TEST_ASSERT_TRUE(p.id > 0);
  TEST_ASSERT_EQUAL_STRING("NE555", p.text[kPartNumber].c_str());  // trimmed
  TEST_ASSERT_EQUAL_STRING("TI", p.text[kManufacturer].c_str());
  TEST_ASSERT_EQUAL_STRING("", p.text[kCategory].c_str());
  TEST_ASSERT_EQUAL(25, p.quantity);
  TEST_ASSERT_EQUAL(1, p.bin.x);
  TEST_ASSERT_EQUAL(2, p.bin.y);

  Part q;
  TEST_ASSERT_TRUE(db.getPart(p.id, q, err) == Result::Ok);
  TEST_ASSERT_EQUAL_STRING("timer", q.text[kNotes].c_str());
  TEST_ASSERT_TRUE(db.getPart(p.id + 100, q, err) == Result::NotFound);
}

static void test_validation() {
  Part p;
  PartChanges empty;
  TEST_ASSERT_TRUE(db.addPart(empty, p, err) == Result::Invalid);  // no part number or description
  PartChanges desc;
  desc.text[kDescription] = std::string("10k resistors");
  TEST_ASSERT_TRUE(db.addPart(desc, p, err) == Result::Ok);  // a description alone is enough

  PartChanges longText = named("X");
  longText.text[kPartNumber] = std::string(65, 'a');
  TEST_ASSERT_TRUE(db.addPart(longText, p, err) == Result::Invalid);
  TEST_ASSERT_TRUE(db.addPart(named("X", -1), p, err) == Result::Invalid);
  TEST_ASSERT_TRUE(db.addPart(named("X", 0, {9, 9}), p, err) == Result::Invalid);  // no such bin

  // An update may not leave a part without both names.
  Part a;
  TEST_ASSERT_TRUE(db.addPart(named("A"), a, err) == Result::Ok);
  PartChanges clear;
  clear.text[kPartNumber] = std::string("   ");
  TEST_ASSERT_TRUE(db.updatePart(a.id, clear, p, err) == Result::Invalid);
}

static void test_update_and_move() {
  Part p;
  TEST_ASSERT_TRUE(db.addPart(named("BC547", 10, {0, 0}), p, err) == Result::Ok);
  PartChanges c;
  c.text[kPackage] = std::string("TO-92");
  c.bin = BinRef{3, 2};
  Part q;
  TEST_ASSERT_TRUE_MESSAGE(db.updatePart(p.id, c, q, err) == Result::Ok, err.c_str());
  TEST_ASSERT_EQUAL_STRING("BC547", q.text[kPartNumber].c_str());  // untouched fields stay
  TEST_ASSERT_EQUAL_STRING("TO-92", q.text[kPackage].c_str());
  TEST_ASSERT_EQUAL(10, q.quantity);
  TEST_ASSERT_EQUAL(3, q.bin.x);

  PartChanges out;
  out.bin = BinRef();  // take it out of its bin
  TEST_ASSERT_TRUE(db.updatePart(p.id, out, q, err) == Result::Ok);
  TEST_ASSERT_FALSE(q.bin.valid());
  TEST_ASSERT_TRUE(db.updatePart(999, c, q, err) == Result::NotFound);
}

static void test_adjust_quantity_clamps() {
  Part p;
  TEST_ASSERT_TRUE(db.addPart(named("LED", 3), p, err) == Result::Ok);
  TEST_ASSERT_TRUE(db.adjustQuantity(p.id, 2, p, err) == Result::Ok);
  TEST_ASSERT_EQUAL(5, p.quantity);
  TEST_ASSERT_TRUE(db.adjustQuantity(p.id, -10, p, err) == Result::Ok);
  TEST_ASSERT_EQUAL(0, p.quantity);  // never below zero
  TEST_ASSERT_TRUE(db.adjustQuantity(12345, 1, p, err) == Result::NotFound);
}

static void test_delete() {
  Part p;
  TEST_ASSERT_TRUE(db.addPart(named("R1"), p, err) == Result::Ok);
  TEST_ASSERT_TRUE(db.deletePart(p.id, err) == Result::Ok);
  TEST_ASSERT_TRUE(db.getPart(p.id, p, err) == Result::NotFound);
  TEST_ASSERT_TRUE(db.deletePart(p.id, err) == Result::NotFound);
}

static void test_list_pages() {
  Part p;
  for (int i = 0; i < 7; i++) {
    char pn[16];
    snprintf(pn, sizeof(pn), "P%d", i);
    TEST_ASSERT_TRUE(db.addPart(named(pn), p, err) == Result::Ok);
  }
  std::vector<std::string> seen;
  int64_t total = 0;
  TEST_ASSERT_TRUE(db.listParts(5, 5, [&](const Part &x) { seen.push_back(x.text[kPartNumber]); }, total, err) ==
                   Result::Ok);
  TEST_ASSERT_EQUAL(7, total);
  TEST_ASSERT_EQUAL(2, seen.size());
  TEST_ASSERT_EQUAL_STRING("P5", seen[0].c_str());
}

// A small catalogue for the search tests: mixed case, Spanish and Russian
// text, parts in bins and one without.
static void addCatalogue() {
  struct Row {
    const char *pn, *maker, *cat, *desc, *pkg;
    int qty;
    BinRef bin;
  };
  const Row rows[] = {
      {"STM32F103C8T6", "STMicro", "MCU", "ARM Cortex-M3 microcontroller", "LQFP-48", 5, {0, 0}},
      {"ATmega328P", "Microchip", "MCU", "8-bit AVR", "TQFP-32", 12, {1, 0}},
      {"NE555", "TI", "Timer", "Precision timer", "SOIC-8", 40, {1, 0}},
      {"RC0603-10K", "Yageo", "Resistor", "Резистор 10 кОм", "0603", 500, {2, 1}},
      {"GRM188", "Murata", "Capacitor", "Condensador cerámico 100 nF", "0603", 300, BinRef()},
  };
  Part p;
  for (const Row &r : rows) {
    PartChanges c = named(r.pn, r.qty, r.bin);
    c.text[kManufacturer] = std::string(r.maker);
    c.text[kCategory] = std::string(r.cat);
    c.text[kDescription] = std::string(r.desc);
    c.text[kPackage] = std::string(r.pkg);
    TEST_ASSERT_TRUE_MESSAGE(db.addPart(c, p, err) == Result::Ok, err.c_str());
  }
}

// Part numbers findParts() returns for a query, in order; total in *total.
static std::vector<std::string> find(const PartQuery &q, int64_t *total = nullptr) {
  std::vector<std::string> out;
  int64_t t = -1;
  TEST_ASSERT_TRUE_MESSAGE(
      db.findParts(q, [&](const Part &x) { out.push_back(x.text[kPartNumber]); }, t, err) == Result::Ok,
      err.c_str());
  // findPartIds() (what the web API streams from) must name the same parts in
  // the same order, with the same total.
  std::vector<int64_t> ids;
  int64_t idTotal = -1;
  TEST_ASSERT_TRUE_MESSAGE(db.findPartIds(q, ids, idTotal, err) == Result::Ok, err.c_str());
  TEST_ASSERT_EQUAL_INT64(t, idTotal);
  TEST_ASSERT_EQUAL(out.size(), ids.size());
  for (size_t i = 0; i < ids.size(); i++) {
    Part p;
    TEST_ASSERT_TRUE(db.getPart(ids[i], p, err) == Result::Ok);
    TEST_ASSERT_EQUAL_STRING(out[i].c_str(), p.text[kPartNumber].c_str());
  }
  if (total) *total = t;
  return out;
}

static PartQuery search(const char *words) {
  PartQuery q;
  q.search = words;
  return q;
}

static void test_search_any_field_any_case() {
  addCatalogue();
  // Part number, description and package, whatever the case.
  TEST_ASSERT_EQUAL(1, find(search("stm32")).size());
  TEST_ASSERT_EQUAL(1, find(search("CORTEX")).size());
  TEST_ASSERT_EQUAL(2, find(search("0603")).size());
  // Every word must match, each in any field.
  auto both = find(search("mcu lqfp"));
  TEST_ASSERT_EQUAL(1, both.size());
  TEST_ASSERT_EQUAL_STRING("STM32F103C8T6", both[0].c_str());
  TEST_ASSERT_EQUAL(0, find(search("mcu soic")).size());
  // Russian and Spanish capitals fold too.
  TEST_ASSERT_EQUAL(1, find(search("РЕЗИСТОР")).size());
  TEST_ASSERT_EQUAL(1, find(search("кОМ")).size());
  TEST_ASSERT_EQUAL(1, find(search("CERÁMICO")).size());
  // Characters LIKE treats specially are plain text here.
  TEST_ASSERT_EQUAL(0, find(search("%")).size());
  TEST_ASSERT_EQUAL(1, find(search("m3")).size());
  // Spaces alone are no search at all.
  TEST_ASSERT_EQUAL(5, find(search("  ")).size());
}

static void test_filters_and_bins() {
  addCatalogue();
  PartQuery q;
  q.category = "mcu";  // any case
  TEST_ASSERT_EQUAL(2, find(q).size());
  q.manufacturer = "microchip";
  TEST_ASSERT_EQUAL(1, find(q).size());

  PartQuery inBin;
  inBin.bin = BinRef{1, 0};
  TEST_ASSERT_EQUAL(2, find(inBin).size());
  inBin.bin = BinRef();  // no bin
  auto loose = find(inBin);
  TEST_ASSERT_EQUAL(1, loose.size());
  TEST_ASSERT_EQUAL_STRING("GRM188", loose[0].c_str());
}

// countByBin(): where a query's parts are, one entry per bin that has any.
static void test_count_by_bin() {
  addCatalogue();
  auto counts = [](const PartQuery &q) {
    std::map<std::pair<int, int>, int64_t> out;
    TEST_ASSERT_TRUE_MESSAGE(db.countByBin(q, [&](const BinCount &c) { out[{c.bin.x, c.bin.y}] = c.parts; }, err) ==
                                 Result::Ok,
                             err.c_str());
    return out;
  };
  // No filter: every part, and the loose one under x = -1.
  auto all = counts(PartQuery());
  TEST_ASSERT_EQUAL(4, all.size());
  TEST_ASSERT_EQUAL(1, (all[{0, 0}]));
  TEST_ASSERT_EQUAL(2, (all[{1, 0}]));
  TEST_ASSERT_EQUAL(1, (all[{-1, -1}]));
  // A search: only the bins that hold matches.
  PartQuery q;
  q.search = "mcu";  // category of the STM32 and the ATmega
  q.sort = SortKey::PartNumber;
  q.limit = 1;  // ignored here
  auto mcu = counts(q);
  TEST_ASSERT_EQUAL(2, mcu.size());
  TEST_ASSERT_EQUAL(1, (mcu[{0, 0}]));
  TEST_ASSERT_EQUAL(1, (mcu[{1, 0}]));
  q = PartQuery();
  q.search = "nothing-like-this";
  TEST_ASSERT_EQUAL(0, counts(q).size());
}

// setPhoto(): stores the name, hands back the old one, counts in stats.
static void test_set_photo() {
  Part p;
  TEST_ASSERT_TRUE(db.addPart(named("LM317", 3), p, err) == Result::Ok);
  TEST_ASSERT_EQUAL_STRING("", p.photo.c_str());
  std::string previous = "x";
  TEST_ASSERT_TRUE_MESSAGE(db.setPhoto(p.id, "1-a1b2c3", previous, p, err) == Result::Ok, err.c_str());
  TEST_ASSERT_EQUAL_STRING("", previous.c_str());
  TEST_ASSERT_EQUAL_STRING("1-a1b2c3", p.photo.c_str());
  Stats s;
  TEST_ASSERT_TRUE(db.stats(s, err) == Result::Ok);
  TEST_ASSERT_EQUAL(1, s.photos);
  TEST_ASSERT_TRUE(db.setPhoto(p.id, "1-d4e5f6", previous, p, err) == Result::Ok);
  TEST_ASSERT_EQUAL_STRING("1-a1b2c3", previous.c_str());
  TEST_ASSERT_TRUE(db.setPhoto(p.id, "", previous, p, err) == Result::Ok);
  TEST_ASSERT_EQUAL_STRING("1-d4e5f6", previous.c_str());
  TEST_ASSERT_EQUAL_STRING("", p.photo.c_str());
  TEST_ASSERT_TRUE(db.stats(s, err) == Result::Ok);
  TEST_ASSERT_EQUAL(0, s.photos);
  TEST_ASSERT_TRUE(db.setPhoto(9999, "x", previous, p, err) == Result::NotFound);
}

static void test_sorting_and_pages() {
  addCatalogue();
  PartQuery q;
  q.sort = SortKey::PartNumber;
  auto asc = find(q);
  TEST_ASSERT_EQUAL_STRING("ATmega328P", asc[0].c_str());
  TEST_ASSERT_EQUAL_STRING("STM32F103C8T6", asc[4].c_str());
  q.descending = true;
  TEST_ASSERT_EQUAL_STRING("STM32F103C8T6", find(q)[0].c_str());

  q = PartQuery();
  q.sort = SortKey::Quantity;
  q.descending = true;
  TEST_ASSERT_EQUAL_STRING("RC0603-10K", find(q)[0].c_str());

  // By bin: row by row, then column; the part in no bin comes last.
  q = PartQuery();
  q.sort = SortKey::Bin;
  auto byBin = find(q);
  TEST_ASSERT_EQUAL_STRING("STM32F103C8T6", byBin[0].c_str());
  TEST_ASSERT_EQUAL_STRING("RC0603-10K", byBin[3].c_str());
  TEST_ASSERT_EQUAL_STRING("GRM188", byBin[4].c_str());

  // Pages: the total counts every match, not just the page.
  q = PartQuery();
  q.sort = SortKey::PartNumber;
  q.limit = 2;
  int64_t total = 0;
  auto first = find(q, &total);
  TEST_ASSERT_EQUAL(2, first.size());
  TEST_ASSERT_EQUAL(5, total);
  q.offset = 4;
  auto last = find(q, &total);
  TEST_ASSERT_EQUAL(1, last.size());
  TEST_ASSERT_EQUAL(5, total);
  TEST_ASSERT_EQUAL_STRING("STM32F103C8T6", last[0].c_str());
  // With filters the total is counted in the same scan as the page, also in
  // id order, where SQLite could otherwise stop reading after one page.
  q = search("0603");
  q.limit = 1;
  TEST_ASSERT_EQUAL(1, find(q, &total).size());
  TEST_ASSERT_EQUAL(2, total);
  q.offset = 1;
  TEST_ASSERT_EQUAL(1, find(q, &total).size());
  TEST_ASSERT_EQUAL(2, total);
}

static void test_empty_values_sort_last() {
  addCatalogue();
  Part p;
  TEST_ASSERT_TRUE(db.addPart(named("NO-MAKER"), p, err) == Result::Ok);
  PartQuery q;
  q.sort = SortKey::Manufacturer;
  TEST_ASSERT_EQUAL_STRING("NO-MAKER", find(q).back().c_str());
  q.descending = true;
  TEST_ASSERT_EQUAL_STRING("NO-MAKER", find(q).back().c_str());
}

static void test_list_values() {
  addCatalogue();
  std::vector<FieldValue> seen;
  auto collect = [&](const FieldValue &v) { seen.push_back(v); };
  TEST_ASSERT_TRUE(db.listValues(kCategory, "", 50, collect, err) == Result::Ok);
  TEST_ASSERT_EQUAL(4, seen.size());
  TEST_ASSERT_EQUAL_STRING("MCU", seen[0].value.c_str());  // most used first
  TEST_ASSERT_EQUAL(2, seen[0].parts);
  seen.clear();
  TEST_ASSERT_TRUE(db.listValues(kManufacturer, "MICRO", 50, collect, err) == Result::Ok);
  TEST_ASSERT_EQUAL(2, seen.size());  // STMicro, Microchip
  seen.clear();
  TEST_ASSERT_TRUE(db.listValues(kCategory, "", 1, collect, err) == Result::Ok);
  TEST_ASSERT_EQUAL(1, seen.size());
}

static void test_bins_follow_grid() {
  Part p;
  TEST_ASSERT_TRUE(db.addPart(named("kept", 1, {3, 2}), p, err) == Result::Ok);
  TEST_ASSERT_TRUE(db.setBinLabel({1, 0}, " Resistors ", err) == Result::Ok);
  // Shrink to 2 x 2: the empty bins outside go, the one holding a part stays.
  TEST_ASSERT_TRUE(db.syncBins(2, 2, err) == Result::Ok);
  int inGrid = 0, outside = 0;
  std::string label;
  TEST_ASSERT_TRUE(db.listBins(
                       [&](const BinInfo &b) {
                         (b.inGrid ? inGrid : outside)++;
                         if (b.x == 1 && b.y == 0) label = b.label;
                         if (!b.inGrid) TEST_ASSERT_EQUAL(1, b.parts);
                       },
                       err) == Result::Ok);
  TEST_ASSERT_EQUAL(4, inGrid);
  TEST_ASSERT_EQUAL(1, outside);
  TEST_ASSERT_EQUAL_STRING("Resistors", label.c_str());

  Stats s;
  TEST_ASSERT_TRUE(db.stats(s, err) == Result::Ok);
  TEST_ASSERT_EQUAL(4, s.binsTotal);
  TEST_ASSERT_EQUAL(1, s.binsUsed);
  // Growing again brings the bin back into the grid.
  TEST_ASSERT_TRUE(db.syncBins(4, 3, err) == Result::Ok);
  TEST_ASSERT_TRUE(db.stats(s, err) == Result::Ok);
  TEST_ASSERT_EQUAL(12, s.binsTotal);
}

static void test_stats() {
  Part p;
  PartChanges a = named("A", 5, {0, 0});
  a.text[kCategory] = std::string("IC");
  a.text[kManufacturer] = std::string("TI");
  PartChanges b = named("B", 7, {0, 0});
  b.text[kCategory] = std::string("Passive");
  PartChanges c = named("C", 1, {1, 0});
  c.text[kCategory] = std::string("IC");
  for (PartChanges *x : {&a, &b, &c}) TEST_ASSERT_TRUE(db.addPart(*x, p, err) == Result::Ok);
  Stats s;
  TEST_ASSERT_TRUE(db.stats(s, err) == Result::Ok);
  TEST_ASSERT_EQUAL(3, s.parts);
  TEST_ASSERT_EQUAL(13, s.totalQuantity);
  TEST_ASSERT_EQUAL(2, s.categories);
  TEST_ASSERT_EQUAL(1, s.manufacturers);
  TEST_ASSERT_EQUAL(2, s.binsUsed);
  TEST_ASSERT_EQUAL(0, s.photos);
  TEST_ASSERT_TRUE(s.dbBytes > 0);
}

static void test_survives_reopen_and_timestamps() {
  db.setClock([] { return int64_t(1700000000); });
  Part p;
  TEST_ASSERT_TRUE(db.addPart(named("persist", 4, {2, 1}), p, err) == Result::Ok);
  TEST_ASSERT_EQUAL(1700000000, p.createdAt);
  db.close();
  TEST_ASSERT_TRUE_MESSAGE(db.open(kPath, err), err.c_str());
  Part q;
  TEST_ASSERT_TRUE(db.getPart(p.id, q, err) == Result::Ok);
  TEST_ASSERT_EQUAL_STRING("persist", q.text[kPartNumber].c_str());
  TEST_ASSERT_EQUAL(2, q.bin.x);
  db.setClock(nullptr);
}

static void test_write_hooks_wrap_writes() {
  int begins = 0, ends = 0;
  db.setWriteHooks({[&] { begins++; }, [&] { ends++; }});
  Part p;
  TEST_ASSERT_TRUE(db.addPart(named("hooked"), p, err) == Result::Ok);
  TEST_ASSERT_TRUE(db.getPart(p.id, p, err) == Result::Ok);  // a read: no hooks
  TEST_ASSERT_EQUAL(1, begins);
  TEST_ASSERT_EQUAL(1, ends);
  db.setWriteHooks({});
}

// Reads run inside the read hooks (the board pauses the LED strips there),
// balanced, and nested ones (a write inside a read) close in order.
void test_read_hooks_wrap_reads() {
  int depth = 0, maxDepth = 0, reads = 0, writes = 0;
  db.setReadHooks({[&] { reads++; maxDepth = std::max(maxDepth, ++depth); }, [&] { depth--; }});
  db.setWriteHooks({[&] { writes++; TEST_ASSERT_TRUE(depth > 0); }, [] {}});
  Part p;
  TEST_ASSERT_TRUE(db.addPart(named("read hooked"), p, err) == Result::Ok);
  TEST_ASSERT_EQUAL(1, writes);
  TEST_ASSERT_EQUAL(0, depth);
  const int before = reads;
  TEST_ASSERT_TRUE(db.getPart(p.id, p, err) == Result::Ok);
  TEST_ASSERT_EQUAL(before + 1, reads);
  int64_t total = 0;
  TEST_ASSERT_TRUE(db.findParts(search("read hooked"), [](const Part &) {}, total, err) == Result::Ok);
  TEST_ASSERT_EQUAL(1, total);
  std::vector<int64_t> ids{p.id, 999999, p.id};
  int seen = 0;
  TEST_ASSERT_TRUE(db.readParts(ids, 1, 5, [&](const Part &) { seen++; }, err) == Result::Ok);
  TEST_ASSERT_EQUAL(1, seen);  // 999999 doesn't exist; the range is clamped to the list
  TEST_ASSERT_EQUAL(0, depth);
  TEST_ASSERT_EQUAL(2, maxDepth);  // findParts() -> findPartIds()
  TEST_ASSERT_TRUE(db.deletePart(p.id, err) == Result::Ok);
  db.setReadHooks({});
  db.setWriteHooks({});
}

// The board's numbers come from the same code: /api/inventory/benchmark.
static void test_benchmark_2000() {
  BenchResult r;
  TEST_ASSERT_TRUE_MESSAGE(db.benchmark(2000, 4, 3, r, err) == Result::Ok, err.c_str());
  TEST_ASSERT_EQUAL(2000 / 8, r.found);  // one maker in eight is Murata
  Stats s;
  TEST_ASSERT_TRUE(db.stats(s, err) == Result::Ok);
  TEST_ASSERT_EQUAL(0, s.parts);  // removed again
  printf("benchmark (host): insert %lld ms, count %lld ms, search %lld ms, bin %lld ms, get %lld ms, delete %lld ms\n",
         (long long)r.insertMs, (long long)r.countMs, (long long)r.searchMs, (long long)r.binMs, (long long)r.getMs,
         (long long)r.deleteMs);
}

static void test_refuses_newer_schema() {
  db.close();
  // Pretend a newer firmware wrote the file: bump user_version past what this
  // firmware knows, through a plain SQLite connection.
  sqlite3 *raw = nullptr;
  TEST_ASSERT_EQUAL(SQLITE_OK, sqlite3_open(kPath, &raw));
  TEST_ASSERT_EQUAL(SQLITE_OK, sqlite3_exec(raw, "PRAGMA user_version = 99", nullptr, nullptr, nullptr));
  sqlite3_close(raw);
  TEST_ASSERT_FALSE(db.open(kPath, err));
  TEST_ASSERT_TRUE(err.find("newer") != std::string::npos);
}

static int rawPageSize() {
  sqlite3 *raw = nullptr;
  sqlite3_open(kPath, &raw);
  sqlite3_stmt *q = nullptr;
  sqlite3_prepare_v2(raw, "PRAGMA page_size", -1, &q, nullptr);
  const int size = sqlite3_step(q) == SQLITE_ROW ? sqlite3_column_int(q, 0) : 0;
  sqlite3_finalize(q);
  sqlite3_close(raw);
  return size;
}

// A file from the earlier 0.5.0 builds (4 KB pages) is rewritten with 2 KB
// pages on open, keeping its contents.
static void test_converts_old_page_size() {
  Part p;
  TEST_ASSERT_TRUE(db.addPart(named("KEEP-ME"), p, err) == Result::Ok);
  db.close();
  sqlite3 *raw = nullptr;
  TEST_ASSERT_EQUAL(SQLITE_OK, sqlite3_open(kPath, &raw));
  TEST_ASSERT_EQUAL(SQLITE_OK, sqlite3_exec(raw, "PRAGMA page_size = 4096; VACUUM", nullptr, nullptr, nullptr));
  sqlite3_close(raw);
  TEST_ASSERT_EQUAL(4096, rawPageSize());

  TEST_ASSERT_TRUE_MESSAGE(db.open(kPath, err), err.c_str());
  Stats s;
  TEST_ASSERT_TRUE(db.stats(s, err) == Result::Ok);
  TEST_ASSERT_EQUAL(1, s.parts);
  db.close();
  TEST_ASSERT_EQUAL(2048, rawPageSize());
}

static void test_hold_writes() {
  Part p;
  TEST_ASSERT_TRUE(db.addPart(named("BC547", 5), p, err) == Result::Ok);
  db.holdWrites(true);
  Part q;
  TEST_ASSERT_TRUE(db.adjustQuantity(p.id, 1, q, err) == Result::Busy);
  TEST_ASSERT_TRUE(db.addPart(named("BC557"), q, err) == Result::Busy);
  TEST_ASSERT_TRUE(db.getPart(p.id, q, err) == Result::Ok);  // reads go on
  TEST_ASSERT_EQUAL(5, q.quantity);
  db.holdWrites(false);
  TEST_ASSERT_TRUE(db.adjustQuantity(p.id, 1, q, err) == Result::Ok);
  TEST_ASSERT_EQUAL(6, q.quantity);
}

static void test_next_photo() {
  Part a, b, c;
  std::string prev;
  db.addPart(named("A"), a, err);
  db.addPart(named("B"), b, err);
  db.addPart(named("C"), c, err);
  db.setPhoto(a.id, "1-aaaaaa", prev, a, err);
  db.setPhoto(c.id, "3-cccccc", prev, c, err);
  int64_t id = 0;
  std::string photo;
  TEST_ASSERT_TRUE(db.nextPhoto(0, id, photo, err) == Result::Ok);
  TEST_ASSERT_EQUAL(a.id, id);
  TEST_ASSERT_EQUAL_STRING("1-aaaaaa", photo.c_str());
  TEST_ASSERT_TRUE(db.nextPhoto(id, id, photo, err) == Result::Ok);
  TEST_ASSERT_EQUAL(c.id, id);
  TEST_ASSERT_TRUE(db.nextPhoto(id, id, photo, err) == Result::NotFound);
}

static void test_import_parts() {
  Part existing;
  TEST_ASSERT_TRUE(db.addPart(named("OLD", 1, {0, 0}), existing, err) == Result::Ok);
  std::vector<ImportRow> rows(5);
  rows[0].id = existing.id;  // update: only the given columns change
  rows[0].fields.quantity = 42;
  rows[1].fields = named("NEW1", 3, {1, 1});
  rows[2].id = 999;  // no such part: added as new
  rows[2].fields = named("NEW2");
  rows[3].fields.quantity = 1;  // no part number or description: refused
  rows[4].id = existing.id;     // the same values again: not written
  rows[4].fields.text[kPartNumber] = std::string("OLD");
  std::vector<ImportOutcome> outcomes;
  std::vector<std::string> reasons;
  TEST_ASSERT_TRUE_MESSAGE(db.importParts(rows, [&](size_t, ImportOutcome o, const std::string &why) {
    outcomes.push_back(o);
    reasons.push_back(why);
  }, err) == Result::Ok, err.c_str());
  TEST_ASSERT_EQUAL(5, (int)outcomes.size());
  TEST_ASSERT_TRUE(outcomes[0] == ImportOutcome::Updated);
  TEST_ASSERT_TRUE(outcomes[1] == ImportOutcome::Added);
  TEST_ASSERT_TRUE(outcomes[2] == ImportOutcome::Added);
  TEST_ASSERT_TRUE(outcomes[3] == ImportOutcome::Failed);
  TEST_ASSERT_TRUE(reasons[3].find("part number") != std::string::npos);
  TEST_ASSERT_TRUE(outcomes[4] == ImportOutcome::Unchanged);
  Part q;
  TEST_ASSERT_TRUE(db.getPart(existing.id, q, err) == Result::Ok);
  TEST_ASSERT_EQUAL(42, q.quantity);
  TEST_ASSERT_EQUAL_STRING("OLD", q.text[kPartNumber].c_str());
  TEST_ASSERT_EQUAL(0, q.bin.x);
  Stats s;
  db.stats(s, err);
  TEST_ASSERT_EQUAL(3, s.parts);
  // A row whose id is free is added under that id, so importing it again
  // finds it rather than adding a duplicate.
  TEST_ASSERT_TRUE(db.getPart(999, q, err) == Result::Ok);
  TEST_ASSERT_EQUAL_STRING("NEW2", q.text[kPartNumber].c_str());
  std::vector<ImportRow> again(1);
  again[0].id = 999;
  again[0].fields = named("NEW2");
  outcomes.clear();
  db.importParts(again, [&](size_t, ImportOutcome o, const std::string &) { outcomes.push_back(o); }, err);
  TEST_ASSERT_TRUE(outcomes[0] == ImportOutcome::Unchanged || outcomes[0] == ImportOutcome::Updated);
  db.stats(s, err);
  TEST_ASSERT_EQUAL(3, s.parts);
}

static void test_replace_file() {
  static const char *kOther = "/tmp/sb_test_other.db";
  static const char *kPrev = "/tmp/sb_test_prev.db";
  std::remove(kOther);
  std::remove(kPrev);
  Part p;
  db.addPart(named("KEEP"), p, err);
  // Another database with one different part: what a restore brings in.
  {
    InventoryDb other;
    TEST_ASSERT_TRUE(other.open(kOther, err));
    other.syncBins(4, 3, err);
    other.addPart(named("RESTORED", 7), p, err);
    other.addPart(named("RESTORED2", 8), p, err);
  }
  TEST_ASSERT_TRUE_MESSAGE(db.replaceFile(kPath, kOther, kPrev, err), err.c_str());
  Stats s;
  db.stats(s, err);
  TEST_ASSERT_EQUAL(2, s.parts);
  // The old database was kept aside.
  {
    InventoryDb prev;
    TEST_ASSERT_TRUE(prev.open(kPrev, err));
    prev.stats(s, err);
    TEST_ASSERT_EQUAL(1, s.parts);
  }
  // A damaged file is refused and the current database stays.
  FILE *f = std::fopen(kOther, "wb");
  std::fputs("SQLite format 3", f);
  for (int i = 0; i < 5000; i++) std::fputc(i & 0xff, f);
  std::fclose(f);
  TEST_ASSERT_FALSE(db.replaceFile(kPath, kOther, kPrev, err));
  TEST_ASSERT_TRUE(db.isOpen());
  db.stats(s, err);
  TEST_ASSERT_EQUAL(2, s.parts);
  std::remove(kOther);
  std::remove(kPrev);
}

// Delete DB: no incoming file, so a new empty database takes the old one's
// place, and the old one is kept aside like before a restore.
static void test_replace_with_empty() {
  static const char *kPrev = "/tmp/sb_test_prev.db";
  std::remove(kPrev);
  Part p;
  db.addPart(named("GONE"), p, err);
  Stats s;
  db.stats(s, err);
  const int64_t before = s.parts;
  TEST_ASSERT_TRUE(before > 0);
  TEST_ASSERT_TRUE_MESSAGE(db.replaceFile(kPath, nullptr, kPrev, err), err.c_str());
  TEST_ASSERT_TRUE(db.isOpen());
  db.syncBins(4, 3, err);
  db.stats(s, err);
  TEST_ASSERT_EQUAL(0, s.parts);
  TEST_ASSERT_EQUAL(12, s.binsTotal);
  // New parts can be added to the empty database.
  TEST_ASSERT_EQUAL(static_cast<int>(Result::Ok), static_cast<int>(db.addPart(named("NEW"), p, err)));
  {
    InventoryDb prev;
    TEST_ASSERT_TRUE(prev.open(kPrev, err));
    prev.stats(s, err);
    TEST_ASSERT_EQUAL(before, s.parts);
  }
  std::remove(kPrev);
}

static void test_bin_names() {
  TEST_ASSERT_EQUAL_STRING("A1", binName({0, 0}).c_str());
  TEST_ASSERT_EQUAL_STRING("Z12", binName({25, 11}).c_str());
  TEST_ASSERT_EQUAL_STRING("AA3", binName({26, 2}).c_str());
  TEST_ASSERT_EQUAL_STRING("", binName(BinRef()).c_str());
  BinRef b;
  TEST_ASSERT_TRUE(parseBinName(" b12 ", b));
  TEST_ASSERT_EQUAL(1, b.x);
  TEST_ASSERT_EQUAL(11, b.y);
  TEST_ASSERT_TRUE(parseBinName("AA3", b));
  TEST_ASSERT_EQUAL(26, b.x);
  TEST_ASSERT_TRUE(parseBinName("", b));
  TEST_ASSERT_FALSE(b.valid());
  TEST_ASSERT_FALSE(parseBinName("12", b));
  TEST_ASSERT_FALSE(parseBinName("A0", b));
  TEST_ASSERT_FALSE(parseBinName("A1x", b));
  for (int x = 0; x < 800; x += 7) {
    TEST_ASSERT_TRUE(parseBinName(binName({x, 4}), b));
    TEST_ASSERT_EQUAL(x, b.x);
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_add_and_get);
  RUN_TEST(test_validation);
  RUN_TEST(test_update_and_move);
  RUN_TEST(test_adjust_quantity_clamps);
  RUN_TEST(test_delete);
  RUN_TEST(test_list_pages);
  RUN_TEST(test_search_any_field_any_case);
  RUN_TEST(test_filters_and_bins);
  RUN_TEST(test_count_by_bin);
  RUN_TEST(test_set_photo);
  RUN_TEST(test_sorting_and_pages);
  RUN_TEST(test_empty_values_sort_last);
  RUN_TEST(test_list_values);
  RUN_TEST(test_bins_follow_grid);
  RUN_TEST(test_stats);
  RUN_TEST(test_survives_reopen_and_timestamps);
  RUN_TEST(test_write_hooks_wrap_writes);
  RUN_TEST(test_read_hooks_wrap_reads);
  RUN_TEST(test_benchmark_2000);
  RUN_TEST(test_refuses_newer_schema);
  RUN_TEST(test_converts_old_page_size);
  RUN_TEST(test_hold_writes);
  RUN_TEST(test_next_photo);
  RUN_TEST(test_import_parts);
  RUN_TEST(test_replace_file);
  RUN_TEST(test_replace_with_empty);
  RUN_TEST(test_bin_names);
  return UNITY_END();
}
