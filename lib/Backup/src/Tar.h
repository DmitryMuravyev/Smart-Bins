#pragma once

// The backup archive: a plain (uncompressed) POSIX tar file.
//
// Why tar: it can be written and read as a stream, one 512-byte block at a
// time, so the board never needs to hold more than a block of it in RAM, and
// every computer can open it (Windows 10+ with `tar -xf`, macOS and Linux out
// of the box, 7-Zip). JPEG photos and the database don't shrink much with
// compression, so there is little to gain from gzip.
//
// Layout of a tar file: for each file a 512-byte header (name, size, ...),
// then its data padded with zeros to a multiple of 512 bytes; at the end two
// blocks of zeros.
//
// Plain C++ with no Arduino calls, so the native unit tests cover it.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace sb {
namespace backup {

constexpr size_t kTarBlock = 512;

// Fills `out` with the header of a regular file. Names longer than 99 bytes
// are refused (the backup's names are short: "photos/12-a1b2c3-t.jpg").
// `mtime` is Unix seconds (0 when the board's clock isn't set).
bool tarHeader(uint8_t out[kTarBlock], const std::string &name, uint64_t size, int64_t mtime);

// Zero bytes that follow `size` bytes of data to reach the next block.
inline size_t tarPadding(uint64_t size) { return (kTarBlock - size % kTarBlock) % kTarBlock; }

// Reads a tar stream given in pieces of any size (as they arrive over the
// network). For each regular file it calls:
//   onFile(name, size)    when its header has been read; return false to
//                         skip the file's data (onData/onEnd are not called)
//   onData(data, len)     with the file's data, in pieces; return false to stop
//   onEnd()               after the last piece; return false to stop
// Directories and other entry types are skipped. Stopping, or a damaged
// header, puts the reader into the failed state with a reason in error().
class TarReader {
 public:
  std::function<bool(const std::string &name, uint64_t size)> onFile;
  std::function<bool(const uint8_t *data, size_t len)> onData;
  std::function<bool()> onEnd;

  // Feeds the next piece of the stream. False once the reader has failed.
  bool feed(const uint8_t *data, size_t len);
  // True when the end marker (or the end of the last file) has been reached:
  // call it once the whole stream was fed to tell a complete archive from a
  // cut-off one.
  bool complete() const;
  bool failed() const { return !error_.empty(); }
  // For the callbacks: the reason to report when they return false.
  void stop(const std::string &why) { fail(why); }
  const std::string &error() const { return error_; }

 private:
  bool parseHeader();
  bool fail(const std::string &why);

  enum class State { Header, Data, Padding, End };
  State state_ = State::Header;
  uint8_t block_[kTarBlock];
  size_t have_ = 0;        // bytes collected in block_ (Header state)
  uint64_t remaining_ = 0; // data or padding bytes still to come
  size_t padding_ = 0;     // padding after the current file's data
  bool wanted_ = false;    // the caller wants the current file's data
  bool sawEntry_ = false;  // at least one header was read
  std::string error_;
};

}  // namespace backup
}  // namespace sb
