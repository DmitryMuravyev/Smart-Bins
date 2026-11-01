#pragma once

// CSV (comma-separated values) for exporting the parts to a spreadsheet and
// importing them back.
//
// Spreadsheets disagree on the separator: Excel with English settings writes
// commas, with Russian or Spanish settings semicolons (the comma is the
// decimal mark there). The writer takes the separator as a parameter; the
// reader detects it from the first line. Text is UTF-8; the export starts with
// a byte order mark so Excel doesn't show Cyrillic as mojibake, and the reader
// skips one if present.
//
// Plain C++ with no Arduino calls, so the native unit tests cover it.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sb {
namespace backup {

// Appends one field to `out`, quoted when it contains the separator, a quote
// or a line break (quotes inside are doubled), as RFC 4180 describes.
// A field that starts with = + - or @ gets a leading apostrophe, so a
// spreadsheet shows it as text rather than running it as a formula.
void csvField(std::string &out, const std::string &value, char sep);

// Reads CSV text given in pieces of any size. Calls onRow once per record
// with its fields and the line number it started on (1 = the first line);
// return false from onRow to stop. Quoted fields may span lines. Empty lines
// are skipped.
class CsvReader {
 public:
  // Limits that keep a malformed file (a stray quote swallowing the rest of
  // the file into one field) from taking all the RAM.
  static constexpr size_t kMaxField = 4096;
  static constexpr size_t kMaxFields = 64;

  // `sep` 0: detected from the first line (the most frequent of , ; and tab
  // outside quotes; comma when there is none).
  explicit CsvReader(char sep = 0) : sep_(sep) {}

  std::function<bool(const std::vector<std::string> &fields, int line)> onRow;

  bool feed(const uint8_t *data, size_t len);
  // Call after the last piece: delivers a last record without a line break.
  bool finish();
  bool failed() const { return !error_.empty(); }
  const std::string &error() const { return error_; }
  char separator() const { return sep_; }

 private:
  bool parse(const uint8_t *data, size_t len);
  bool endField();
  bool endRow();
  bool fail(const std::string &why);

  char sep_;
  bool started_ = false;    // the byte order mark check is done
  std::string firstLine_;   // collected until the separator is known
  std::vector<std::string> fields_;
  std::string field_;
  bool quoted_ = false;     // inside a quoted field
  bool quoteSeen_ = false;  // a quote just closed (or is the first of a "" pair)
  bool fieldStarted_ = false;
  bool afterCr_ = false;    // the previous byte was \r (for \r\n line ends)
  int line_ = 1;            // current line
  int rowLine_ = 1;         // line the current record started on
  std::string error_;
};

}  // namespace backup
}  // namespace sb
