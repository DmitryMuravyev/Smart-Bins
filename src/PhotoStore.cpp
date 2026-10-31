#include "PhotoStore.h"

#include <FFat.h>
#include <SD.h>
#include <SPI.h>
#include <esp_log.h>
#include <esp_random.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "BootReport.h"
#include "led/RmtStrip.h"

namespace sb {

namespace {
const char *TAG = "photos";
const char *kDir = "/photos";
const char *kTempPath = "/photos/upload.tmp";
// SPI clock for the card. 20 MHz works with short wires on a breadboard;
// SD.begin() falls back to nothing slower on its own, so a card that fails
// here is treated as absent (photos then go to flash).
constexpr uint32_t kCardHz = 20000000;
// Files open at once on the card: an upload and a photo being sent. Each slot
// costs heap, as on the FAT partition (see mountFilesystem() in main.cpp).
constexpr uint8_t kCardMaxFiles = 2;
}  // namespace

void PhotoStore::begin(int sck, int miso, int mosi, int cs) {
  if (sck >= 0 && miso >= 0 && mosi >= 0 && cs >= 0) {
    static SPIClass spi(VSPI);
    spi.begin(sck, miso, mosi, cs);
    if (SD.begin(cs, spi, kCardHz, "/sd", kCardMaxFiles) && SD.cardType() != CARD_NONE) {
      card_ = true;
    } else {
      // No card, or no slot at all: give the pins back.
      SD.end();
      spi.end();
    }
  }
  fs::FS &fs = store();
  if (!fs.exists(kDir)) {
    led::FlashGuard guard;  // harmless on a card, needed on flash
    fs.mkdir(kDir);
  }
  // A temp file left by an upload cut short by a restart.
  if (fs.exists(kTempPath)) {
    led::FlashGuard guard;
    fs.remove(kTempPath);
  }
  ESP_LOGI(TAG, "Photos on %s: %s used of %s, %d px", card_ ? "microSD card" : "flash", formatBytes(usedBytes()),
           formatBytes(totalBytes()), maxSide());
}

fs::FS &PhotoStore::store() const {
  if (card_) return SD;
  return FFat;
}

uint64_t PhotoStore::usedBytes() const { return card_ ? SD.usedBytes() : FFat.usedBytes(); }

uint64_t PhotoStore::totalBytes() const { return card_ ? SD.totalBytes() : FFat.totalBytes(); }

uint64_t PhotoStore::availableBytes() const {
  const uint64_t total = totalBytes(), used = usedBytes();
  const uint64_t free = total > used ? total - used : 0;
  if (card_) return free;
  return free > kFlashReserve ? free - kFlashReserve : 0;
}

std::string PhotoStore::newName(int64_t partId) {
  char name[32];
  snprintf(name, sizeof(name), "%lld-%06lx", (long long)partId, (unsigned long)(esp_random() & 0xffffff));
  return name;
}

bool PhotoStore::validName(const std::string &name) {
  // "<digits>-<6 hex>": also what keeps a request from naming any other file.
  const size_t dash = name.find('-');
  if (dash == 0 || dash == std::string::npos || name.size() - dash - 1 != 6 || dash > 19) return false;
  for (size_t i = 0; i < dash; i++) {
    if (name[i] < '0' || name[i] > '9') return false;
  }
  for (size_t i = dash + 1; i < name.size(); i++) {
    const char c = name[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

String PhotoStore::pathFor(const std::string &name, Kind kind) {
  return String(kDir) + "/" + name.c_str() + (kind == Kind::Thumb ? "-t.jpg" : ".jpg");
}

bool PhotoStore::beginUpload(const void *owner, const std::string &name, Kind kind, size_t size, std::string &err) {
  if (owner_ && owner_ != owner) {
    // One upload at a time. A request that stopped sending (phone locked,
    // Wi-Fi lost) without the server noticing frees the slot after a while.
    if (millis() - lastActivity_ < kStaleMs) return err = "another photo is being uploaded, try again", false;
    abortUpload(owner_);
  }
  if (!validName(name)) return err = "bad photo name", false;
  const size_t limit = kind == Kind::Thumb ? kMaxThumbBytes : kMaxPhotoBytes;
  if (size < 4) return err = "empty upload", false;
  if (size > limit) return err = "photo too large (the page resizes it before uploading)", false;
  if (size > availableBytes()) return err = card_ ? "the microSD card is full" : "not enough free flash for photos", false;
  buffer_ = static_cast<uint8_t *>(malloc(kBufferSize));
  if (!buffer_) return err = "out of memory, try again", false;
  {
    led::FlashGuard guard;
    file_ = store().open(kTempPath, FILE_WRITE);
  }
  if (!file_) {
    free(buffer_);
    buffer_ = nullptr;
    return err = "can't create the photo file", false;
  }
  owner_ = owner;
  target_ = pathFor(name, kind);
  expected_ = size;
  received_ = buffered_ = 0;
  lastActivity_ = millis();
  return true;
}

bool PhotoStore::writeUpload(const void *owner, const uint8_t *data, size_t len, std::string &err) {
  if (owner != owner_ || !buffer_) return err = "upload not started", false;
  if (received_ + len > expected_) return err = "more data than announced", false;
  // A JPEG starts with FF D8; anything else is refused before it is stored.
  if (received_ == 0 && (len < 2 || data[0] != 0xFF || data[1] != 0xD8)) return err = "not a JPEG file", false;
  lastActivity_ = millis();
  received_ += len;
  while (len) {
    const size_t n = std::min(len, kBufferSize - buffered_);
    memcpy(buffer_ + buffered_, data, n);
    buffered_ += n;
    data += n;
    len -= n;
    if (buffered_ == kBufferSize && !flushBuffer(err)) return false;
  }
  return true;
}

bool PhotoStore::flushBuffer(std::string &err) {
  if (!buffered_) return true;
  size_t written;
  {
    led::FlashGuard guard;
    written = file_.write(buffer_, buffered_);
  }
  const bool ok = written == buffered_;
  buffered_ = 0;
  if (!ok) err = "writing the photo failed (storage full?)";
  return ok;
}

bool PhotoStore::finishUpload(const void *owner, std::string &err) {
  if (owner != owner_) return err = "upload not started", false;
  if (received_ != expected_) {
    abortUpload(owner);
    return err = "upload incomplete", false;
  }
  if (!flushBuffer(err)) {
    abortUpload(owner);
    return false;
  }
  closeUpload();
  fs::FS &fs = store();
  bool ok;
  {
    // FAT can't rename onto an existing file. Names are new for every photo,
    // so the target exists only after a retry with the same name (the thumbnail).
    led::FlashGuard guard;
    if (fs.exists(target_)) fs.remove(target_);
    ok = fs.rename(kTempPath, target_);
  }
  if (!ok) return err = "saving the photo failed", false;
  return true;
}

void PhotoStore::abortUpload(const void *owner) {
  if (owner != owner_ || !owner_) return;
  closeUpload();
  led::FlashGuard guard;
  store().remove(kTempPath);
}

void PhotoStore::closeUpload() {
  if (file_) {
    led::FlashGuard guard;
    file_.close();
  }
  free(buffer_);
  buffer_ = nullptr;
  buffered_ = 0;
  owner_ = nullptr;
}

void PhotoStore::remove(const std::string &name) {
  if (!validName(name)) return;
  led::FlashGuard guard;
  for (Kind kind : {Kind::Photo, Kind::Thumb}) {
    const String path = pathFor(name, kind);
    if (card_ && SD.exists(path)) SD.remove(path);
    if (FFat.exists(path)) FFat.remove(path);
  }
}

bool PhotoStore::find(const std::string &name, Kind kind, fs::FS *&fs, String &path, size_t &size) const {
  if (!validName(name)) return false;
  // The thumbnail first, then the photo itself, which the page then scales.
  for (Kind k : {kind, Kind::Photo}) {
    path = pathFor(name, k);
    for (fs::FS *candidate : {card_ ? static_cast<fs::FS *>(&SD) : nullptr, static_cast<fs::FS *>(&FFat)}) {
      if (!candidate) continue;
      // exists() first: opening a missing file logs an error on every miss.
      if (!candidate->exists(path)) continue;
      File f = candidate->open(path, FILE_READ);
      if (f && !f.isDirectory()) {
        size = f.size();
        fs = candidate;
        return true;
      }
    }
  }
  return false;
}

}  // namespace sb
