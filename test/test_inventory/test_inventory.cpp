// Inventory database tests. They run against the system's SQLite (libsqlite3)
// with a real file in /tmp, so reopening shows what survives a restart.

#include <unity.h>

#include <chrono>
#include <cstdio>
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

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_add_and_get);
  RUN_TEST(test_validation);
  RUN_TEST(test_update_and_move);
  RUN_TEST(test_adjust_quantity_clamps);
  RUN_TEST(test_delete);
  RUN_TEST(test_list_pages);
  RUN_TEST(test_bins_follow_grid);
  RUN_TEST(test_stats);
  RUN_TEST(test_survives_reopen_and_timestamps);
  RUN_TEST(test_write_hooks_wrap_writes);
  RUN_TEST(test_benchmark_2000);
  RUN_TEST(test_refuses_newer_schema);
  RUN_TEST(test_converts_old_page_size);
  return UNITY_END();
}
