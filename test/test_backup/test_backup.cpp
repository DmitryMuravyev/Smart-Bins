// Tests for the backup archive (tar) and the CSV reader and writer.

#include <unity.h>

#include <cstring>
#include <string>
#include <vector>

#include "Csv.h"
#include "Tar.h"

using namespace sb::backup;

void setUp() {}
void tearDown() {}

// ---- tar ----

struct Entry {
  std::string name, data;
};

static std::string makeTar(const std::vector<Entry> &entries) {
  std::string out;
  uint8_t header[kTarBlock];
  for (const Entry &e : entries) {
    TEST_ASSERT_TRUE(tarHeader(header, e.name, e.data.size(), 1700000000));
    out.append(reinterpret_cast<char *>(header), kTarBlock);
    out += e.data;
    out.append(tarPadding(e.data.size()), '\0');
  }
  out.append(2 * kTarBlock, '\0');
  return out;
}

// Feeds `tar` in pieces of `step` bytes and collects the files read back.
static std::vector<Entry> readTar(const std::string &tar, size_t step, TarReader &r) {
  std::vector<Entry> got;
  r.onFile = [&](const std::string &name, uint64_t) {
    got.push_back({name, ""});
    return true;
  };
  r.onData = [&](const uint8_t *d, size_t n) {
    got.back().data.append(reinterpret_cast<const char *>(d), n);
    return true;
  };
  r.onEnd = [] { return true; };
  for (size_t i = 0; i < tar.size(); i += step) {
    r.feed(reinterpret_cast<const uint8_t *>(tar.data()) + i, std::min(step, tar.size() - i));
  }
  return got;
}

static void test_tar_round_trip_any_piece_size() {
  std::string big(1500, 'x');
  for (size_t i = 0; i < big.size(); i++) big[i] = static_cast<char>(i * 7);
  const std::vector<Entry> files = {
      {"smartbins-backup.json", "{\"format\":1}"}, {"empty.txt", ""}, {"inventory.db", big}, {"photos/1-abcdef.jpg", std::string(512, 'p')}};
  const std::string tar = makeTar(files);
  TEST_ASSERT_EQUAL(0, (int)(tar.size() % kTarBlock));
  for (size_t step : {1, 7, 511, 512, 513, 1460, 100000}) {
    TarReader r;
    std::vector<Entry> got = readTar(tar, step, r);
    TEST_ASSERT_FALSE_MESSAGE(r.failed(), r.error().c_str());
    TEST_ASSERT_TRUE(r.complete());
    TEST_ASSERT_EQUAL((int)files.size(), (int)got.size());
    for (size_t i = 0; i < files.size(); i++) {
      TEST_ASSERT_EQUAL_STRING(files[i].name.c_str(), got[i].name.c_str());
      TEST_ASSERT_TRUE(files[i].data == got[i].data);
    }
  }
}

static void test_tar_header_fields() {
  uint8_t h[kTarBlock];
  TEST_ASSERT_TRUE(tarHeader(h, "a.txt", 10, 0));
  TEST_ASSERT_EQUAL_STRING("a.txt", reinterpret_cast<char *>(h));
  TEST_ASSERT_EQUAL_STRING("00000000012", reinterpret_cast<char *>(h + 124));  // size in octal
  TEST_ASSERT_EQUAL_MEMORY("ustar\0" "00", h + 257, 8);
  TEST_ASSERT_FALSE(tarHeader(h, std::string(100, 'n'), 1, 0));  // too long
  TEST_ASSERT_FALSE(tarHeader(h, "", 1, 0));
}

static void test_tar_cut_off_and_garbage() {
  const std::string tar = makeTar({{"inventory.db", std::string(3000, 'd')}});
  TarReader cut;
  readTar(tar.substr(0, 2000), 512, cut);
  TEST_ASSERT_FALSE(cut.failed());
  TEST_ASSERT_FALSE(cut.complete());  // stopped in the middle of the data

  TarReader junk;
  std::string notTar(2048, 'z');
  readTar(notTar, 512, junk);
  TEST_ASSERT_TRUE(junk.failed());
  TEST_ASSERT_TRUE(junk.error().find("checksum") != std::string::npos);
}

static void test_tar_skip_and_stop() {
  const std::string tar = makeTar({{"skip.bin", std::string(700, 's')}, {"keep.bin", "kk"}});
  TarReader r;
  std::string kept;
  r.onFile = [](const std::string &name, uint64_t) { return name == "keep.bin"; };
  r.onData = [&](const uint8_t *d, size_t n) {
    kept.append(reinterpret_cast<const char *>(d), n);
    return true;
  };
  r.feed(reinterpret_cast<const uint8_t *>(tar.data()), tar.size());
  TEST_ASSERT_TRUE(r.complete());
  TEST_ASSERT_EQUAL_STRING("kk", kept.c_str());

  TarReader s;
  s.onFile = [](const std::string &, uint64_t) { return true; };
  s.onData = [&](const uint8_t *, size_t) {
    s.stop("disk full");
    return false;
  };
  TEST_ASSERT_FALSE(s.feed(reinterpret_cast<const uint8_t *>(tar.data()), tar.size()));
  TEST_ASSERT_EQUAL_STRING("disk full", s.error().c_str());
}

// ---- CSV ----

static std::vector<std::vector<std::string>> readCsv(const std::string &text, size_t step, CsvReader &r,
                                                     std::vector<int> *lines = nullptr) {
  std::vector<std::vector<std::string>> rows;
  r.onRow = [&](const std::vector<std::string> &f, int line) {
    rows.push_back(f);
    if (lines) lines->push_back(line);
    return true;
  };
  for (size_t i = 0; i < text.size(); i += step) {
    r.feed(reinterpret_cast<const uint8_t *>(text.data()) + i, std::min(step, text.size() - i));
  }
  r.finish();
  return rows;
}

static void test_csv_write_quotes() {
  std::string out;
  csvField(out, "plain", ',');
  out += ',';
  csvField(out, "a,b", ',');
  out += ',';
  csvField(out, "say \"hi\"", ',');
  out += ',';
  csvField(out, "two\nlines", ',');
  out += ',';
  csvField(out, "+5V", ',');
  out += ',';
  csvField(out, "a,b", ';');  // a comma needs no quotes with ; as separator
  TEST_ASSERT_EQUAL_STRING("plain,\"a,b\",\"say \"\"hi\"\"\",\"two\nlines\",'+5V,a,b", out.c_str());
}

static void test_csv_round_trip_and_separators() {
  const std::vector<std::string> values = {"id", "Résistor 10k", "x;y", "a,b", "q\"q", "multi\r\nline", "-12V", ""};
  for (char sep : {',', ';', '\t'}) {
    std::string text = "\xEF\xBB\xBF";
    for (int rep = 0; rep < 2; rep++) {
      for (size_t i = 0; i < values.size(); i++) {
        if (i) text += sep;
        csvField(text, values[i], sep);
      }
      text += "\r\n";
    }
    for (size_t step : {1, 3, 64, 100000}) {
      CsvReader r;
      std::vector<int> lines;
      auto rows = readCsv(text, step, r, &lines);
      TEST_ASSERT_FALSE_MESSAGE(r.failed(), r.error().c_str());
      TEST_ASSERT_EQUAL(sep, r.separator());
      TEST_ASSERT_EQUAL(2, (int)rows.size());
      TEST_ASSERT_EQUAL(1, lines[0]);
      TEST_ASSERT_EQUAL(3, lines[1]);  // the quoted line break counts
      for (size_t i = 0; i < values.size(); i++) TEST_ASSERT_EQUAL_STRING(values[i].c_str(), rows[1][i].c_str());
    }
  }
}

static void test_csv_blank_lines_and_no_final_newline() {
  CsvReader r;
  auto rows = readCsv("a;b\n\n1;2\n3;4", 5, r);
  TEST_ASSERT_EQUAL(3, (int)rows.size());
  TEST_ASSERT_EQUAL_STRING("4", rows[2][1].c_str());
  CsvReader one;
  rows = readCsv("only,header", 100, one);
  TEST_ASSERT_EQUAL(1, (int)rows.size());
  TEST_ASSERT_EQUAL(2, (int)rows[0].size());
}

static void test_csv_errors() {
  CsvReader open;
  readCsv("a,b\n\"never closed,1\n", 4, open);
  TEST_ASSERT_TRUE(open.failed());
  TEST_ASSERT_TRUE(open.error().find("line 2") != std::string::npos);

  CsvReader longField;
  std::string text = "a\n\"" + std::string(CsvReader::kMaxField + 10, 'x') + "\"\n";
  readCsv(text, 512, longField);
  TEST_ASSERT_TRUE(longField.failed());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_tar_round_trip_any_piece_size);
  RUN_TEST(test_tar_header_fields);
  RUN_TEST(test_tar_cut_off_and_garbage);
  RUN_TEST(test_tar_skip_and_stop);
  RUN_TEST(test_csv_write_quotes);
  RUN_TEST(test_csv_round_trip_and_separators);
  RUN_TEST(test_csv_blank_lines_and_no_final_newline);
  RUN_TEST(test_csv_errors);
  return UNITY_END();
}
