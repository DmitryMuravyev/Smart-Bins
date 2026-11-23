#pragma once

// Where part photos live: a microSD card when one is found at boot, else the
// FAT partition in flash (the same one as the database and config).
//
// Each photo is two JPEG files in /photos, both made by the browser before
// upload (the ESP32 has no RAM to spare for decoding images):
//
//   /photos/<name>.jpg     the photo, 640 px on the long side on flash, 800 px on a card
//   /photos/<name>-t.jpg   a 128 px thumbnail for the parts list
//
// <name> is "<part id>-<6 hex digits>", new for every upload, and stored in the
// part's photo field. Because a replaced photo gets a new name, browsers may
// cache photo URLs forever, and a half-finished upload never overwrites the
// photo that is shown.
//
// Writes go to /photos/upload.tmp first and are renamed when complete, so a
// power cut or a dropped connection leaves either the old photo or the new
// one, never a broken file. Only one upload runs at a time.
//
// On flash, every write is wrapped in a FlashGuard (it pauses the LED strips,
// see FlashGuard in led/OutputCommon.h), and a reserve is kept free for the database to grow.

#include <Arduino.h>
#include <FS.h>

#include <functional>
#include <string>

namespace sb {

class PhotoStore {
 public:
  enum class Kind { Photo, Thumb };

  // Long side in pixels the browser resizes to, and the size of thumbnails.
  static constexpr int kSideFlash = 640;
  static constexpr int kSideCard = 800;
  static constexpr int kThumbSide = 128;
  // Largest upload accepted. A 800 px JPEG at the browser's quality setting is
  // around 60-120 KB; the limit leaves room for detailed pictures.
  static constexpr size_t kMaxPhotoBytes = 400 * 1024;
  static constexpr size_t kMaxThumbBytes = 48 * 1024;
  // Flash kept free for the database and its journal. Photos stop being
  // accepted below this; a card has no reserve (nothing else lives there).
  static constexpr uint64_t kFlashReserve = 512 * 1024;

  // Mounts the card if one answers on these SPI pins (any pin < 0: no card
  // slot), and makes sure /photos exists. Call after the FAT partition is mounted.
  void begin(int sck, int miso, int mosi, int cs);

  bool onCard() const { return card_; }
  int maxSide() const { return card_ ? kSideCard : kSideFlash; }
  uint64_t usedBytes() const;
  uint64_t totalBytes() const;
  // Bytes a new photo may still use (free space minus the reserve).
  uint64_t availableBytes() const;

  // A fresh name for a part's next photo.
  static std::string newName(int64_t partId);
  // True for names this store makes: digits, '-', 6 lowercase hex digits.
  static bool validName(const std::string &name);

  // Upload, in three steps: start (checks size and space), write the data as
  // it arrives, then finish (the file appears under its name) or abort.
  // `owner` identifies the request doing it; a second upload is refused while
  // one is running (unless the first went silent for kStaleMs).
  bool beginUpload(const void *owner, const std::string &name, Kind kind, size_t size, std::string &err);
  bool writeUpload(const void *owner, const uint8_t *data, size_t len, std::string &err);
  bool finishUpload(const void *owner, std::string &err);
  void abortUpload(const void *owner);

  // Deletes both files of a photo, wherever they are.
  void remove(const std::string &name);

  // Finds a photo's file: on the card first, then in flash (photos taken
  // before a card was added stay visible). A missing thumbnail falls back to
  // the photo. False when there is neither.
  // `fallback` false: only the exact file (a backup copies thumbnails only
  // where they exist).
  bool find(const std::string &name, Kind kind, fs::FS *&fs, String &path, size_t &size,
            bool fallback = true) const;

  // ---- Maintenance ----

  // Copies a photo's files from flash to the card, checks each copy's size,
  // then deletes the flash copy. Adds the bytes freed in flash to `moved`.
  // True when nothing of it is left in flash (also when it never was there).
  bool moveToCard(const std::string &name, uint64_t &moved, std::string &err);
  // How many photos (not counting thumbnails) are in flash: with a card, the
  // ones not moved to it yet. Walks the photo folder in flash.
  int flashPhotoCount() const;
  // Deletes every photo file (on the card and in flash) whose name `inUse`
  // doesn't recognise. Returns the number of files removed; `freed` grows by
  // their size.
  int removeUnused(const std::function<bool(const std::string &name)> &inUse, uint64_t &freed);
  // Splits "NAME.jpg" / "NAME-t.jpg" into the photo name and its kind.
  static bool parseFileName(const String &file, std::string &name, Kind &kind);

 private:
  static String pathFor(const std::string &name, Kind kind);
  fs::FS &store() const;
  bool flushBuffer(std::string &err);
  void closeUpload();

  bool card_ = false;

  // The running upload.
  static constexpr uint32_t kStaleMs = 30000;
  static constexpr size_t kBufferSize = 4096;
  const void *owner_ = nullptr;
  uint32_t lastActivity_ = 0;
  File file_;
  String target_;
  size_t expected_ = 0;
  size_t received_ = 0;
  uint8_t *buffer_ = nullptr;  // collects data so flash is written in 4 KB steps
  size_t buffered_ = 0;
};

}  // namespace sb
