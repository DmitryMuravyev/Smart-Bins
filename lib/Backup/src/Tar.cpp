#include "Tar.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace sb {
namespace backup {

namespace {

// Field offsets and sizes in a ustar header.
constexpr size_t kName = 0, kNameLen = 100;
constexpr size_t kMode = 100;
constexpr size_t kUid = 108, kGid = 116;
constexpr size_t kSize = 124, kSizeLen = 12;
constexpr size_t kMtime = 136;
constexpr size_t kChksum = 148, kChksumLen = 8;
constexpr size_t kType = 156;
constexpr size_t kMagic = 257;
constexpr size_t kPrefix = 345, kPrefixLen = 155;

// Writes `value` as a zero-padded octal number of len-1 digits and a NUL.
void octal(uint8_t *field, size_t len, uint64_t value) {
  for (size_t i = len - 1; i-- > 0;) {
    field[i] = static_cast<uint8_t>('0' + (value & 7));
    value >>= 3;
  }
  field[len - 1] = 0;
}

// Reads an octal field; tar writers end it with NUL or a space, and some pad
// it with leading spaces. GNU tar writes sizes of 8 GB and more in base-256
// (first byte 0x80), which a backup never needs: refused.
bool readOctal(const uint8_t *field, size_t len, uint64_t &value) {
  value = 0;
  size_t i = 0;
  while (i < len && field[i] == ' ') i++;
  bool digits = false;
  for (; i < len && field[i] >= '0' && field[i] <= '7'; i++) {
    value = (value << 3) | (field[i] - '0');
    digits = true;
  }
  return digits && (i == len || field[i] == 0 || field[i] == ' ');
}

uint32_t checksum(const uint8_t *block) {
  // The sum of all header bytes, with the checksum field counted as spaces.
  uint32_t sum = 0;
  for (size_t i = 0; i < kTarBlock; i++) sum += (i >= kChksum && i < kChksum + kChksumLen) ? ' ' : block[i];
  return sum;
}

// A name field is NUL-terminated unless it uses all its bytes.
std::string field(const uint8_t *p, size_t len) {
  const void *nul = memchr(p, 0, len);
  return std::string(reinterpret_cast<const char *>(p), nul ? static_cast<const uint8_t *>(nul) - p : len);
}

}  // namespace

bool tarHeader(uint8_t out[kTarBlock], const std::string &name, uint64_t size, int64_t mtime) {
  if (name.empty() || name.size() >= kNameLen) return false;
  memset(out, 0, kTarBlock);
  memcpy(out + kName, name.data(), name.size());
  octal(out + kMode, 8, 0644);
  octal(out + kUid, 8, 0);
  octal(out + kGid, 8, 0);
  octal(out + kSize, kSizeLen, size);
  octal(out + kMtime, 12, mtime > 0 ? static_cast<uint64_t>(mtime) : 0);
  out[kType] = '0';  // regular file
  memcpy(out + kMagic, "ustar\0" "00", 8);
  // The checksum field: six octal digits, NUL, space (what tar itself writes).
  octal(out + kChksum, 7, checksum(out));
  out[kChksum + 7] = ' ';
  return true;
}

bool TarReader::fail(const std::string &why) {
  if (error_.empty()) error_ = why;
  return false;
}

bool TarReader::complete() const {
  if (failed()) return false;
  // Some writers leave out the two zero blocks at the end; a stream that
  // stops exactly between two files is accepted as well.
  return state_ == State::End || (state_ == State::Header && have_ == 0 && sawEntry_);
}

bool TarReader::parseHeader() {
  // A zero block marks the end of the archive.
  bool zero = true;
  for (size_t i = 0; i < kTarBlock && zero; i++) zero = block_[i] == 0;
  if (zero) {
    state_ = State::End;
    return true;
  }
  uint64_t stored, size;
  if (!readOctal(block_ + kChksum, kChksumLen, stored) || stored != checksum(block_)) {
    return fail("not a tar file, or a damaged one (bad header checksum)");
  }
  if (!readOctal(block_ + kSize, kSizeLen, size)) return fail("damaged tar header (size)");
  sawEntry_ = true;
  std::string name = field(block_ + kName, kNameLen);
  // ustar keeps long paths in two parts: prefix "/" name.
  if (memcmp(block_ + kMagic, "ustar", 5) == 0 && block_[kPrefix]) {
    name = field(block_ + kPrefix, kPrefixLen) + "/" + name;
  }
  // '0' and NUL are regular files; everything else (directories, links, the
  // extended headers some tools add) is stepped over with its data.
  const char type = static_cast<char>(block_[kType]);
  const bool regular = type == '0' || type == 0;
  wanted_ = regular && onFile && onFile(name, size);
  if (failed()) return false;
  remaining_ = size;
  padding_ = tarPadding(size);
  state_ = remaining_ ? State::Data : State::Padding;
  if (state_ == State::Padding) {
    // An empty file ends right away.
    if (wanted_ && onEnd && !onEnd()) return fail("stopped");
    remaining_ = padding_;
    if (!remaining_) state_ = State::Header;
  }
  return true;
}

bool TarReader::feed(const uint8_t *data, size_t len) {
  while (len && !failed()) {
    switch (state_) {
      case State::Header: {
        const size_t n = std::min(len, kTarBlock - have_);
        memcpy(block_ + have_, data, n);
        have_ += n;
        data += n;
        len -= n;
        if (have_ == kTarBlock) {
          have_ = 0;
          if (!parseHeader()) return false;
        }
        break;
      }
      case State::Data: {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(len, remaining_));
        if (wanted_ && onData && !onData(data, n)) return fail("stopped");
        data += n;
        len -= n;
        remaining_ -= n;
        if (!remaining_) {
          if (wanted_ && onEnd && !onEnd()) return fail("stopped");
          remaining_ = padding_;
          state_ = remaining_ ? State::Padding : State::Header;
        }
        break;
      }
      case State::Padding: {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(len, remaining_));
        data += n;
        len -= n;
        remaining_ -= n;
        if (!remaining_) state_ = State::Header;
        break;
      }
      case State::End:
        // The second zero block and anything after the end: ignored.
        return true;
    }
  }
  return !failed();
}

}  // namespace backup
}  // namespace sb
