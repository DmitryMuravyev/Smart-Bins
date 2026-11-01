#include "Csv.h"

#include <cstring>

namespace sb {
namespace backup {

namespace {
// The first line is collected whole to find the separator (and the byte
// order mark) before parsing starts. A header line is short; this only
// bounds a file with no line breaks at all.
constexpr size_t kMaxFirstLine = 8192;

char detectSeparator(const std::string &line) {
  size_t comma = 0, semicolon = 0, tab = 0;
  bool quoted = false;
  for (char c : line) {
    if (c == '"') quoted = !quoted;
    if (quoted) continue;
    if (c == ',') comma++;
    if (c == ';') semicolon++;
    if (c == '\t') tab++;
  }
  if (semicolon > comma && semicolon >= tab) return ';';
  if (tab > comma && tab > semicolon) return '\t';
  return ',';
}
}  // namespace

void csvField(std::string &out, const std::string &value, char sep) {
  const bool formula = !value.empty() && strchr("=+-@", value[0]) != nullptr;
  bool quote = false;
  for (char c : value) {
    if (c == sep || c == '"' || c == '\n' || c == '\r') {
      quote = true;
      break;
    }
  }
  if (quote) out += '"';
  if (formula) out += '\'';
  for (char c : value) {
    if (c == '"') out += '"';
    out += c;
  }
  if (quote) out += '"';
}

bool CsvReader::fail(const std::string &why) {
  if (error_.empty()) error_ = why;
  return false;
}

bool CsvReader::feed(const uint8_t *data, size_t len) {
  if (failed()) return false;
  if (!started_) {
    // Collect the first line, then decide on the separator and parse it.
    const uint8_t *nl = static_cast<const uint8_t *>(memchr(data, '\n', len));
    const size_t take = nl ? static_cast<size_t>(nl - data) + 1 : len;
    firstLine_.append(reinterpret_cast<const char *>(data), take);
    if (!nl && firstLine_.size() < kMaxFirstLine) return true;
    started_ = true;
    std::string first;
    first.swap(firstLine_);
    // The UTF-8 byte order mark that Excel writes (and our export too).
    size_t skip = first.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    if (!sep_) sep_ = detectSeparator(first.substr(skip));
    if (!parse(reinterpret_cast<const uint8_t *>(first.data()) + skip, first.size() - skip)) return false;
    data += take;
    len -= take;
  }
  return parse(data, len);
}

bool CsvReader::finish() {
  if (failed()) return false;
  if (!started_) {
    // The whole file was one line without a line break.
    static const uint8_t nl = '\n';
    if (firstLine_.empty()) return true;
    return feed(&nl, 1) && finish();
  }
  if (quoted_ && !quoteSeen_) {
    return fail("line " + std::to_string(rowLine_) + ": a quoted field is never closed");
  }
  quoted_ = false;
  quoteSeen_ = false;
  if (fieldStarted_ || !fields_.empty()) return endRow();
  return true;
}

bool CsvReader::endField() {
  // An apostrophe in front of = + - @ is what the export adds so spreadsheets
  // don't run the text as a formula; it is not part of the value.
  if (field_.size() >= 2 && field_[0] == '\'' && strchr("=+-@", field_[1])) field_.erase(0, 1);
  if (fields_.size() >= kMaxFields) {
    return fail("line " + std::to_string(rowLine_) + ": more than " + std::to_string(kMaxFields) + " columns");
  }
  fields_.push_back(std::move(field_));
  field_.clear();
  fieldStarted_ = false;
  return true;
}

bool CsvReader::endRow() {
  if (!endField()) return false;
  const bool blank = fields_.size() == 1 && fields_[0].empty();
  bool ok = true;
  if (!blank && onRow) ok = onRow(fields_, rowLine_);
  fields_.clear();
  rowLine_ = line_;
  if (!ok) return fail("stopped");
  return true;
}

bool CsvReader::parse(const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    const char c = static_cast<char>(data[i]);
    const bool cr = afterCr_;
    afterCr_ = false;
    if (quoted_) {
      if (quoteSeen_) {
        quoteSeen_ = false;
        if (c == '"') {  // "" inside quotes: one quote character
          field_ += '"';
          continue;
        }
        quoted_ = false;  // the quote closed the field; c is handled below
      } else {
        if (c == '"') {
          quoteSeen_ = true;
        } else {
          if (c == '\n') line_++;
          if (field_.size() >= kMaxField) {
            return fail("line " + std::to_string(rowLine_) + ": a field is longer than " +
                        std::to_string(kMaxField) + " bytes (a quote that is never closed?)");
          }
          field_ += c;
        }
        continue;
      }
    }
    if (c == sep_) {
      if (!endField()) return false;
    } else if (c == '\n') {
      if (cr) continue;  // the \n of \r\n: the row (and the line) already ended at the \r
      line_++;
      if (!endRow()) return false;
    } else if (c == '\r') {
      line_++;
      afterCr_ = true;
      if (!endRow()) return false;
    } else if (c == '"' && !fieldStarted_) {
      quoted_ = true;
      fieldStarted_ = true;
    } else {
      if (field_.size() >= kMaxField) {
        return fail("line " + std::to_string(rowLine_) + ": a field is longer than " + std::to_string(kMaxField) +
                    " bytes");
      }
      field_ += c;
      fieldStarted_ = true;
    }
  }
  return true;
}

}  // namespace backup
}  // namespace sb
