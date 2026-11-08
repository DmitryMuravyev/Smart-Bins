#include "PhotoStore.h"

#include <FFat.h>
#include <SD.h>
#include <SPI.h>
#include <dirent.h>
#include <esp_log.h>
#include <esp_random.h>

#include <algorithm>
#include <vector>
#include <cstdio>
#include <cstring>

#include "BootReport.h"
#include "led/RmtStrip.h"

namespace sb {

namespace {
const char *TAG = "photos";
const char *kDir = "/photos";
const char *kTempPath = "/photos/upload.tmp";
// Where moveToCard() copies a file before renaming it (not kTempPath: an
// upload to the card may be using that at the same time).
const char *kMovePath = "/photos/move.tmp";
// SPI clock for the card. 20 MHz works with short wires on a breadboard;
// SD.begin() falls back to nothing slower on its own, so a card that fails
// here is treated as absent (photos then go to flash).
constexpr uint32_t kCardHz = 20000000;
// Files open at once on the card: an upload (or a copy to or from the card)
// and a photo being sent. Each slot costs about 4 KB of heap, as on the FAT
// partition (see mountFilesystem() in main.cpp); a third one left boards
// without PSRAM short enough of memory to crash on a page load. So nothing
// else may hold a slot: the backups list is read without opening its files
// (see listCardBackups() in BackupApi.cpp).
constexpr uint8_t kCardMaxFiles = 2;
// Mount attempts at boot, and the pause between them (see begin()).
constexpr int kCardTries = 3;
constexpr uint32_t kCardRetryMs = 25;

// Brings a card back to listening for commands, whatever it was doing when
// the ESP32 restarted without cutting its power (flashing over USB, the
// reset button, a crash). A card that was in the middle of a multi-block
// read keeps sending data for as long as it is clocked and selected: the
// wake-up command (CMD0) then gets data back instead of an answer ("token
// error [0] 0x71", "GO_IDLE_STATE failed"), on every try and after every
// reset, until the power is cut. So:
//   1. end a multi-block write (stop token), then clock out whatever block
//      is in flight (a block is 512 bytes plus its CRC; 600 bytes covers it
//      and the gap before it),
//   2. send STOP_TRANSMISSION (CMD12), which ends a multi-block read,
//   3. wait until the card stops signalling busy.
// A card that was idle ignores all of this (CMD12 then just answers "illegal
// command"), and without a card it is a few dummy bytes. It runs at 400 kHz,
// the speed a card must accept before it is set up: about 15 ms.
void wakeCard(SPIClass &spi, int cs) {
  pinMode(cs, OUTPUT);
  digitalWrite(cs, HIGH);
  spi.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
  for (int i = 0; i < 10; i++) spi.transfer(0xFF);
  digitalWrite(cs, LOW);
  // The stop token ends a multi-block write, should the card be in one. To
  // a card waiting for a command it reads as the start of a garbage one,
  // which the 0xFF bytes below complete and the card rejects.
  spi.transfer(0xFD);
  for (int i = 0; i < 600; i++) spi.transfer(0xFF);
  // CMD12, argument 0, with its CRC (cards check it in SPI mode only on
  // CMD0 and CMD8, but a right one costs nothing).
  static const uint8_t kStop[] = {0x40 | 12, 0, 0, 0, 0, 0x61};
  for (uint8_t b : kStop) spi.transfer(b);
  spi.transfer(0xFF);  // the stuff byte after CMD12
  // The answer: a byte with the top bit clear, within 8 bytes.
  for (int i = 0; i < 8 && (spi.transfer(0xFF) & 0x80); i++) {
  }
  // Busy (0x00) until the card has stopped; 100 ms is far more than a card
  // takes to end a read.
  const uint32_t start = millis();
  while (spi.transfer(0xFF) != 0xFF && millis() - start < 100) {
  }
  digitalWrite(cs, HIGH);
  spi.transfer(0xFF);
  spi.endTransaction();
}
}  // namespace

void PhotoStore::begin(int sck, int miso, int mosi, int cs) {
  if (sck >= 0 && miso >= 0 && mosi >= 0 && cs >= 0) {
    static SPIClass spi(VSPI);
    spi.begin(sck, miso, mosi, cs);
    // A few tries: after a reset that doesn't cut the card's power (flashing
    // over USB, the reset button), the card can still be in the middle of
    // what it was doing and miss the wake-up ("GO_IDLE_STATE failed").
    // wakeCard() stops that before each try. An absent card costs about
    // 100 ms more at boot.
    for (int attempt = 0; attempt < kCardTries && !card_; attempt++) {
      if (attempt) {
        SD.end();
        ESP_LOGW(TAG, "microSD card not ready, trying again");
        delay(kCardRetryMs);
      }
      wakeCard(spi, cs);
      card_ = SD.begin(cs, spi, kCardHz, "/sd", kCardMaxFiles) && SD.cardType() != CARD_NONE;
    }
    if (!card_) {
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

// FFat.usedBytes() counts the free clusters by reading the FAT from flash:
// the strips pause meanwhile (see FlashGuard). Harmless for the card.
uint64_t PhotoStore::usedBytes() const {
  led::FlashGuard guard;
  return card_ ? SD.usedBytes() : FFat.usedBytes();
}

uint64_t PhotoStore::totalBytes() const {
  led::FlashGuard guard;
  return card_ ? SD.totalBytes() : FFat.totalBytes();
}

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

bool PhotoStore::find(const std::string &name, Kind kind, fs::FS *&fs, String &path, size_t &size,
                      bool fallback) const {
  if (!validName(name)) return false;
  // exists() and open() read the FAT's directory from flash, which switches
  // the flash cache off like a write does: no LED frame may be on the wire
  // meanwhile (see FlashGuard). Every thumbnail a list of search results asks
  // for comes through here, so without the guard a long search with photos
  // lit the far end of the strip now and then. Harmless for the card.
  led::FlashGuard guard;
  // The thumbnail first, then the photo itself, which the page then scales.
  const Kind second = fallback ? Kind::Photo : kind;
  for (Kind k : {kind, second}) {
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

bool PhotoStore::parseFileName(const String &file, std::string &name, Kind &kind) {
  String base = file;
  const int slash = base.lastIndexOf('/');
  if (slash >= 0) base = base.substring(slash + 1);
  if (base.endsWith("-t.jpg")) {
    kind = Kind::Thumb;
    base.remove(base.length() - 6);
  } else if (base.endsWith(".jpg")) {
    kind = Kind::Photo;
    base.remove(base.length() - 4);
  } else {
    return false;
  }
  name = base.c_str();
  return validName(name);
}

bool PhotoStore::moveToCard(const std::string &name, uint64_t &moved, std::string &err) {
  if (!card_) return err = "no microSD card", false;
  if (!validName(name)) return err = "bad photo name", false;
  for (Kind kind : {Kind::Photo, Kind::Thumb}) {
    const String path = pathFor(name, kind);
    {
      led::FlashGuard guard;  // reads the FAT (see find())
      if (!FFat.exists(path)) continue;
    }
    size_t size;
    {
      // The guard is held for the whole copy: every read of the flash copy
      // needs it, and so does closing the file at the end of this block.
      led::FlashGuard guard;
      File src = FFat.open(path, FILE_READ);
      if (!src) return err = "can't read " + std::string(path.c_str()) + " in flash", false;
      size = src.size();
      // Already on the card with the same size (an earlier run stopped
      // between the copy and the delete): only the delete is left to do.
      bool copied = false;
      if (SD.exists(path)) {
        File dst = SD.open(path, FILE_READ);
        copied = dst && dst.size() == size;
      }
      if (!copied) {
        // Into a temp file first, so a card pulled half way leaves no broken photo.
        File dst = SD.open(kMovePath, FILE_WRITE);
        if (!dst) return err = "can't write to the card", false;
        uint8_t buf[1024];  // on the task's stack, see the move task in BackupApi.cpp
        size_t left = size;
        while (left) {
          const size_t n = src.read(buf, std::min(left, sizeof(buf)));
          if (!n || dst.write(buf, n) != n) {
            dst.close();
            SD.remove(kMovePath);
            return err = "copying " + std::string(path.c_str()) + " failed (card full?)", false;
          }
          left -= n;
        }
        dst.close();
        if (SD.exists(path)) SD.remove(path);
        if (!SD.rename(kMovePath, path)) return err = "can't finish the copy on the card", false;
        File check = SD.open(path, FILE_READ);
        if (!check || check.size() != size) return err = "the copy on the card has the wrong size", false;
      }
    }
    {
      led::FlashGuard guard;
      if (!FFat.remove(path)) return err = "can't delete " + std::string(path.c_str()) + " from flash", false;
    }
    moved += size;
  }
  return true;
}

// Only the folder's entries are read (readdir), never the files: no file is
// opened, so the walk costs a few directory sectors per hundred entries and
// leaves the file slots alone. It only depends on what is in flash, however
// many photos the card holds.
int PhotoStore::flashPhotoCount() const {
  led::FlashGuard guard;  // reads the FAT directory from flash (see find())
  const String path = String("/ffat") + kDir;  // FFat's mount point
  DIR *dir = opendir(path.c_str());
  if (!dir) return 0;
  int count = 0;
  std::string name;
  Kind kind;
  for (dirent *e = readdir(dir); e; e = readdir(dir)) {
    // Photos only: a thumbnail belongs to the photo next to it.
    if (e->d_type != DT_DIR && parseFileName(e->d_name, name, kind) && kind == Kind::Photo) count++;
  }
  closedir(dir);
  return count;
}

int PhotoStore::removeUnused(const std::function<bool(const std::string &name)> &inUse, uint64_t &freed) {
  int removed = 0;
  for (fs::FS *fs : {card_ ? static_cast<fs::FS *>(&SD) : nullptr, static_cast<fs::FS *>(&FFat)}) {
    if (!fs) continue;
    // Deleting while the directory is open can confuse the walk, so each
    // round reads a few entries, closes the directory, decides which to
    // delete, then deletes them; rounds repeat until the walk reaches the
    // end. Entries kept are skipped next round by remembering how many.
    //
    // The walk reads the FAT from flash, so it holds a FlashGuard (harmless
    // on the card), but inUse() asks the database, which takes its own lock
    // and then the guard: calling it with the guard held could deadlock
    // against a request doing the same in that order. So the entries are
    // read under the guard and judged after it is released.
    struct Entry {
      String path;
      std::string name;  // empty: not a photo (upload.tmp, say), kept
      size_t size;
    };
    size_t skip = 0;
    for (;;) {
      std::vector<Entry> batch;
      {
        led::FlashGuard guard;
        File dir = fs->open(kDir);
        if (!dir || !dir.isDirectory()) break;
        size_t seen = 0;
        std::string name;
        Kind kind;
        for (File f = dir.openNextFile(); f && batch.size() < 16; f = dir.openNextFile()) {
          if (f.isDirectory()) continue;
          if (seen++ < skip) continue;
          const String file = f.name();
          const String base = file.lastIndexOf('/') >= 0 ? file.substring(file.lastIndexOf('/') + 1) : file;
          batch.push_back({String(kDir) + "/" + base, parseFileName(file, name, kind) ? name : std::string(),
                           static_cast<size_t>(f.size())});
        }
      }
      if (batch.empty()) break;
      std::vector<String> doomed;
      for (const Entry &e : batch) {
        // Kept: anything that isn't a photo and photos in use.
        if (e.name.empty() || inUse(e.name)) {
          skip++;
        } else {
          freed += e.size;
          doomed.push_back(e.path);
        }
      }
      if (doomed.empty()) continue;  // all kept: the next round reads further on
      led::FlashGuard guard;  // harmless on the card, needed in flash
      for (const String &path : doomed) {
        if (fs->remove(path)) removed++;
      }
    }
  }
  return removed;
}

}  // namespace sb
