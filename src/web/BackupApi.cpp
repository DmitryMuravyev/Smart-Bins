#include "BackupApi.h"

#include <ESPAsyncWebServer.h>
#include <FFat.h>
#include <SD.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ApiUtil.h"
#include "BootReport.h"
#include "ConfigStore.h"
#include "Csv.h"
#include "InventoryDb.h"
#include "PhotoStore.h"
#include "Security.h"
#include "Tar.h"
#include "led/RmtStrip.h"

#ifndef SB_FW_VERSION
#define SB_FW_VERSION "dev"
#endif

namespace sb {

using backup::CsvReader;
using backup::kTarBlock;
using backup::TarReader;
using inventory::ImportOutcome;
using inventory::ImportRow;
using inventory::InventoryDb;
using inventory::Part;
using inventory::Result;
using Kind = PhotoStore::Kind;

namespace {

const char *TAG = "backup";

// Files on the FAT partition. FFat takes paths below its mount point, SQLite
// (through the C file API) the full path.
const char *kDbFile = "/inventory.db";
const char *kDbPath = "/ffat/inventory.db";
const char *kIncomingFile = "/restore.db";
const char *kIncomingPath = "/ffat/restore.db";
const char *kPreviousFile = "/inventory-before-restore.db";
const char *kPreviousPath = "/ffat/inventory-before-restore.db";

// Version of the archive layout. A restore refuses archives of a later one.
constexpr int kBackupFormat = 1;
// Flash left free on top of an incoming database, for the journal of the
// first writes after the restore.
constexpr uint64_t kRestoreMargin = 64 * 1024;
// Largest manifest and config.json read from an archive.
constexpr size_t kMaxSmallFile = 8192;
// The restored database is written to flash in steps of this size.
constexpr size_t kWriteBuffer = mem::kRestoreWriteBuffer;  // 4 KB, 16 KB with PSRAM
// Rows of a CSV import written per transaction. Each changed row dirties a
// page or two of the database, and SQLite keeps a transaction's dirty pages
// in RAM until the commit: 20 rows spread over the whole file needed more
// heap than a board without PSRAM has (out of memory on a 112-row import).
// 5 rows without PSRAM, 50 with it (MemoryProfile.h).
constexpr size_t kImportBatch = mem::kImportBatch;
// Errors listed in an import's answer (the counts cover all of them).
constexpr size_t kMaxImportErrors = 20;
// An unfinished restore or import that has received nothing for this long
// gives way to a new one (its connection is gone without the server noticing).
constexpr uint32_t kStaleMs = 30000;

int gridCols = 0, gridRows = 0;

// Current time for file dates, or 0 before the clock is set over the network.
int64_t nowOrZero() {
  const time_t t = time(nullptr);
  return t > 1700000000 ? static_cast<int64_t>(t) : 0;
}

// "smartbins-backup-2026-11-01.tar", or without the date when the clock isn't set.
String downloadName(const char *stem, const char *ext) {
  const time_t t = nowOrZero();
  String name = stem;
  if (t) {
    char date[16];
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(date, sizeof(date), "-%Y-%m-%d", &tm);
    name += date;
  }
  return name + ext;
}

void attachment(AsyncWebServerResponse *res, const String &name) {
  String value = "attachment; filename=\"";
  value += name;
  value += "\"";
  res->addHeader("Content-Disposition", value);
  res->addHeader("Cache-Control", "no-store");
}

// Reads part of a file, opening it for just this call: the FAT partition has
// only four file slots (see main.cpp), and a slow download must not hold one.
size_t readAt(fs::FS &fs, const String &path, uint64_t offset, uint8_t *buf, size_t len) {
  // Reading the FAT partition switches the flash cache off: no LED frame may
  // be on the wire meanwhile (see FlashGuard).
  led::FlashGuard guard;
  File f = fs.open(path, FILE_READ);
  if (!f || !f.seek(offset)) return 0;
  return f.read(buf, len);
}

// Both read the FAT from flash: the strips pause meanwhile (see FlashGuard).
uint64_t fileSize(fs::FS &fs, const char *path) {
  led::FlashGuard guard;
  if (!fs.exists(path)) return 0;
  File f = fs.open(path, FILE_READ);
  return f ? f.size() : 0;
}

uint64_t flashFree() {
  led::FlashGuard guard;
  const uint64_t total = FFat.totalBytes(), used = FFat.usedBytes();
  return total > used ? total - used : 0;
}

// ---- Holding database writes during a copy ----
//
// Two downloads may overlap; writes resume when the last one has its copy.
// A copy to the microSD card holds them too, from its own task, hence the
// mutex around the count.
int writeHolders = 0;
std::mutex holdMutex;

void holdWrites(InventoryDb &db) {
  std::lock_guard<std::mutex> lock(holdMutex);
  if (writeHolders++ == 0) db.holdWrites(true);
}

void releaseWrites(InventoryDb &db) {
  std::lock_guard<std::mutex> lock(holdMutex);
  if (writeHolders > 0 && --writeHolders == 0) db.holdWrites(false);
}

bool writesHeld() {
  std::lock_guard<std::mutex> lock(holdMutex);
  return writeHolders > 0;
}

// True while a background job (photos, or the database to or from the
// card) runs; defined with the jobs below.
bool backgroundBusy();

// ---- The backup archive, produced as it is sent ----
//
// The response asks for bytes chunk by chunk (as fast as the connection takes
// them); fill() hands out the next ones. Nothing but the current tar header
// and the small files is held in RAM: the database and photos are read from
// storage straight into the response's buffer.
class BackupStream {
 public:
  BackupStream(InventoryDb &db, PhotoStore &photos, std::string manifest, std::string config)
      : db_(db), photos_(photos), manifest_(std::move(manifest)), config_(std::move(config)), mtime_(nowOrZero()) {}
  ~BackupStream() {
    // Also when the browser gives up half way: the response is deleted and
    // the hold must not outlive it.
    if (holding_) releaseWrites(db_);
    ESP_LOGI(TAG, "Backup %s: %d photo files, %llu bytes", done_ ? "sent" : "stopped", files_,
             (unsigned long long)sent_);
  }

  size_t fill(uint8_t *buf, size_t maxLen) {
    size_t n = 0;
    while (n < maxLen) {
      if (pendingAt_ < pending_.size()) {
        const size_t k = std::min(maxLen - n, pending_.size() - pendingAt_);
        memcpy(buf + n, pending_.data() + pendingAt_, k);
        pendingAt_ += k;
        n += k;
        continue;
      }
      if (fs_) {
        if (at_ < size_) {
          const size_t want = static_cast<size_t>(std::min<uint64_t>(maxLen - n, size_ - at_));
          size_t got = readAt(*fs_, path_, at_, buf + n, want);
          if (!got) {
            // The file went away or shrank since its header was written. The
            // header promised `size_` bytes, so zeros keep the archive
            // readable (a restore of a broken database is refused by its check).
            ESP_LOGW(TAG, "Backup: %s ended early", path_.c_str());
            memset(buf + n, 0, want);
            got = want;
          }
          n += got;
          at_ += got;
          continue;
        }
        fs_ = nullptr;
        setPending(std::string(backup::tarPadding(size_), '\0'));
        if (holding_) {
          // The database has been copied: changes may go on.
          holding_ = false;
          releaseWrites(db_);
        }
        continue;
      }
      if (!next()) break;
    }
    sent_ += n;
    return n;
  }

 private:
  enum class Step { Manifest, Config, Db, Photos, Trailer, Done };

  void setPending(std::string s) {
    pending_ = std::move(s);
    pendingAt_ = 0;
  }

  std::string header(const std::string &name, uint64_t size) {
    uint8_t h[kTarBlock];
    backup::tarHeader(h, name, size, mtime_);
    return std::string(reinterpret_cast<char *>(h), kTarBlock);
  }

  // A small file held in RAM: header, data and padding in one go.
  void smallFile(const char *name, const std::string &data) {
    std::string s = header(name, data.size());
    s += data;
    s.append(backup::tarPadding(data.size()), '\0');
    setPending(std::move(s));
  }

  void storedFile(const std::string &name, fs::FS &fs, const String &path, uint64_t size) {
    setPending(header(name, size));
    fs_ = &fs;
    path_ = path;
    size_ = size;
    at_ = 0;
  }

  // Queues the next entry of the archive; false after the end.
  bool next() {
    switch (step_) {
      case Step::Manifest:
        smallFile("smartbins-backup.json", manifest_);
        step_ = Step::Config;
        return true;
      case Step::Config:
        smallFile("config.json", config_);
        config_.clear();
        config_.shrink_to_fit();
        step_ = Step::Db;
        return true;
      case Step::Db: {
        step_ = Step::Photos;
        // Hold writes first, then measure: the size can't change after that.
        holdWrites(db_);
        holding_ = true;
        const uint64_t size = fileSize(FFat, kDbFile);
        if (!size) {
          ESP_LOGE(TAG, "Backup: no database file");
          holding_ = false;
          releaseWrites(db_);
          return next();
        }
        storedFile("inventory.db", FFat, kDbFile, size);
        return true;
      }
      case Step::Photos:
        // Each photo has two files; kind_ walks them, then the next photo.
        while (true) {
          if (!photo_.empty() && kind_ < 2) {
            const Kind kind = kind_++ == 0 ? Kind::Photo : Kind::Thumb;
            fs::FS *fs = nullptr;
            String path;
            size_t size = 0;
            if (!photos_.find(photo_, kind, fs, path, size, false)) continue;
            files_++;
            storedFile(std::string("photos/") + (path.c_str() + path.lastIndexOf('/') + 1), *fs, path, size);
            return true;
          }
          std::string err;
          const Result r = db_.nextPhoto(photoId_, photoId_, photo_, err);
          if (r != Result::Ok) {
            if (r != Result::NotFound) ESP_LOGE(TAG, "Backup: photo list failed: %s", err.c_str());
            break;
          }
          kind_ = 0;
        }
        step_ = Step::Trailer;
        return next();
      case Step::Trailer:
        setPending(std::string(2 * kTarBlock, '\0'));  // the end-of-archive marker
        step_ = Step::Done;
        return true;
      case Step::Done:
        done_ = true;
        return false;
    }
    return false;
  }

  InventoryDb &db_;
  PhotoStore &photos_;
  std::string manifest_, config_;
  int64_t mtime_;
  Step step_ = Step::Manifest;
  std::string pending_;
  size_t pendingAt_ = 0;
  fs::FS *fs_ = nullptr;  // the stored file being sent, if any
  String path_;
  uint64_t size_ = 0, at_ = 0;
  bool holding_ = false;
  int64_t photoId_ = 0;
  std::string photo_;
  int kind_ = 0;
  int files_ = 0;
  uint64_t sent_ = 0;
  bool done_ = false;
};

// ---- Restore ----
//
// One restore at a time. Its state lives here rather than in the request's
// _tempObject (which the server releases with free(), so it can only hold
// plain data).
struct RestoreJob {
  AsyncWebServerRequest *req = nullptr;
  uint32_t lastActivity = 0;
  bool settingsWanted = false;
  bool sniffed = false;  // the first bytes told a tar from a bare database
  bool tar = false;
  uint8_t head[16];
  size_t headLen = 0;
  TarReader reader;
  std::string error;
  int status = 400;

  enum class Target { None, Manifest, Config, Db, Photo } target = Target::None;
  std::string manifest, config;
  bool manifestSeen = false, configSeen = false;

  File dbFile;
  bool dbSeen = false;
  uint8_t dbHead[100];  // the SQLite header: format string and schema version
  size_t dbHeadLen = 0;
  uint8_t *buffer = nullptr;
  size_t buffered = 0;

  bool photoOpen = false;
  int photosRestored = 0, photosFailed = 0;
  std::string photoError;

  ~RestoreJob() { free(buffer); }
};

RestoreJob *restoreJob = nullptr;

bool restoreFail(RestoreJob &job, int status, const std::string &why) {
  if (job.error.empty()) {
    job.error = why;
    job.status = status;
  }
  return false;
}

bool flushDb(RestoreJob &job) {
  if (!job.buffered) return true;
  size_t written;
  {
    led::FlashGuard guard;
    written = job.dbFile.write(job.buffer, job.buffered);
  }
  const bool ok = written == job.buffered;
  job.buffered = 0;
  return ok || restoreFail(job, 507, "writing the database failed (flash full?)");
}

bool startDb(RestoreJob &job, uint64_t size) {
  if (job.dbSeen) return restoreFail(job, 400, "the archive has two databases");
  job.dbSeen = true;
  // The incoming file sits next to the current one until the swap.
  const uint64_t free = flashFree() + fileSize(FFat, kIncomingFile) + fileSize(FFat, kPreviousFile);
  if (size + kRestoreMargin > free) {
    char msg[96];
    snprintf(msg, sizeof(msg), "not enough free flash for the database (%s needed)", formatBytes(size + kRestoreMargin));
    return restoreFail(job, 507, msg);
  }
  {
    led::FlashGuard guard;
    // The database kept from the last restore makes room for this one: the
    // swap replaces it anyway.
    if (FFat.exists(kPreviousFile)) FFat.remove(kPreviousFile);  // exists(): remove() logs an error when it is missing
    job.dbFile = FFat.open(kIncomingFile, FILE_WRITE);
  }
  if (!job.dbFile) return restoreFail(job, 500, "can't create the database file");
  job.buffer = static_cast<uint8_t *>(malloc(kWriteBuffer));
  if (!job.buffer) return restoreFail(job, 503, "out of memory, try again");
  job.target = RestoreJob::Target::Db;
  return true;
}

bool writeDb(RestoreJob &job, const uint8_t *data, size_t len) {
  if (job.dbHeadLen < sizeof(job.dbHead)) {
    const size_t n = std::min(len, sizeof(job.dbHead) - job.dbHeadLen);
    memcpy(job.dbHead + job.dbHeadLen, data, n);
    job.dbHeadLen += n;
  }
  while (len) {
    const size_t n = std::min(len, kWriteBuffer - job.buffered);
    memcpy(job.buffer + job.buffered, data, n);
    job.buffered += n;
    data += n;
    len -= n;
    if (job.buffered == kWriteBuffer && !flushDb(job)) return false;
  }
  return true;
}

// The archive's entries, as the tar reader finds them.
void wireReader(RestoreJob &job, PhotoStore &photos) {
  RestoreJob *j = &job;
  job.reader.onFile = [j, &photos](const std::string &name, uint64_t size) {
    j->target = RestoreJob::Target::None;
    if (name == "smartbins-backup.json" || name == "config.json") {
      if (size > kMaxSmallFile) {
        j->reader.stop(name + " is too large");
        return false;
      }
      const bool manifest = name[0] == 's';
      if (!manifest && !j->settingsWanted) return false;  // settings not asked for: skipped
      j->target = manifest ? RestoreJob::Target::Manifest : RestoreJob::Target::Config;
      (manifest ? j->manifestSeen : j->configSeen) = true;
      return true;
    }
    if (name == "inventory.db") {
      if (!startDb(*j, size)) {
        j->reader.stop(j->error);
        return false;
      }
      return true;
    }
    std::string photo;
    Kind kind;
    if (name.compare(0, 7, "photos/") == 0 && PhotoStore::parseFileName(name.c_str(), photo, kind)) {
      std::string err;
      if (photos.beginUpload(j, photo, kind, size, err)) {
        j->target = RestoreJob::Target::Photo;
        j->photoOpen = true;
        return true;
      }
      j->photosFailed++;
      if (j->photoError.empty()) j->photoError = err;
    }
    return false;  // anything else is skipped
  };
  job.reader.onData = [j, &photos](const uint8_t *data, size_t len) {
    switch (j->target) {
      case RestoreJob::Target::Manifest: j->manifest.append(reinterpret_cast<const char *>(data), len); break;
      case RestoreJob::Target::Config: j->config.append(reinterpret_cast<const char *>(data), len); break;
      case RestoreJob::Target::Db:
        if (!writeDb(*j, data, len)) {
          j->reader.stop(j->error);
          return false;
        }
        break;
      case RestoreJob::Target::Photo:
        if (j->photoOpen) {
          std::string err;
          if (!photos.writeUpload(j, data, len, err)) {
            // One bad photo doesn't stop the restore: counted, the rest of
            // its data skipped.
            photos.abortUpload(j);
            j->photoOpen = false;
            j->photosFailed++;
            if (j->photoError.empty()) j->photoError = err;
          }
        }
        break;
      case RestoreJob::Target::None: break;
    }
    return true;
  };
  job.reader.onEnd = [j, &photos]() {
    if (j->target == RestoreJob::Target::Db) {
      if (!flushDb(*j)) {
        j->reader.stop(j->error);
        return false;
      }
      led::FlashGuard guard;
      j->dbFile.close();
    } else if (j->target == RestoreJob::Target::Photo && j->photoOpen) {
      std::string err;
      j->photoOpen = false;
      if (photos.finishUpload(j, err)) {
        j->photosRestored++;
      } else {
        j->photosFailed++;
        if (j->photoError.empty()) j->photoError = err;
      }
    }
    j->target = RestoreJob::Target::None;
    return true;
  };
}

// Drops a restore: its files and its slot.
void endRestore(PhotoStore &photos, bool removeIncoming) {
  RestoreJob *job = restoreJob;
  if (!job) return;
  restoreJob = nullptr;
  photos.abortUpload(job);
  if (job->dbFile) {
    led::FlashGuard guard;
    job->dbFile.close();
  }
  if (removeIncoming) {
    led::FlashGuard guard;  // exists() reads the FAT too
    if (FFat.exists(kIncomingFile)) FFat.remove(kIncomingFile);
  }
  delete job;
}

// Feeds a piece of the body to the tar reader, or straight into the
// database file for a bare .db upload.
bool restoreData(RestoreJob &job, const uint8_t *data, size_t len) {
  if (!len) return true;
  if (job.tar) {
    if (!job.reader.feed(data, len)) return restoreFail(job, job.status, job.reader.error());
    return true;
  }
  return writeDb(job, data, len);
}

void onRestoreBody(AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total,
                   PhotoStore &photos, Security &security) {
  if (index == 0) {
    if (restoreJob && millis() - restoreJob->lastActivity < kStaleMs) return;  // busy: answered below
    endRestore(photos, true);
    restoreJob = new (std::nothrow) RestoreJob();
    if (!restoreJob) return;
    restoreJob->req = req;
    restoreJob->settingsWanted = hasParam(req, "settings") && param(req, "settings")->value() == "1";
    // The password middleware only runs once the whole body is in.
    if (!security.allows(req)) restoreFail(*restoreJob, 401, "password required");
    // A copy to or from the microSD card (or a photo job) is using the files.
    else if (backgroundBusy()) restoreFail(*restoreJob, 409, "another job is running, try again when it ends");
    wireReader(*restoreJob, photos);
    req->onDisconnect([req, &photos]() {
      if (restoreJob && restoreJob->req == req) endRestore(photos, true);
    });
  }
  RestoreJob *job = restoreJob;
  if (!job || job->req != req || !job->error.empty()) return;
  job->lastActivity = millis();
  if (!job->sniffed) {
    // A bare database starts with "SQLite format 3\0"; anything else is
    // taken for a tar archive (the reader checks its headers).
    const size_t n = std::min(len, sizeof(job->head) - job->headLen);
    memcpy(job->head + job->headLen, data, n);
    job->headLen += n;
    data += n;
    len -= n;
    if (job->headLen < sizeof(job->head) && index + n < total) return;
    job->sniffed = true;
    job->tar = job->headLen < 16 || memcmp(job->head, "SQLite format 3", 16) != 0;
    if (!job->tar && !startDb(*job, total)) return;
    if (!restoreData(*job, job->head, job->headLen)) return;
  }
  restoreData(*job, data, len);
}

// Big-endian 32-bit number in the SQLite header.
uint32_t be32(const uint8_t *p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

void onRestoreDone(AsyncWebServerRequest *req, InventoryDb &db, PhotoStore &photos, ConfigStore &config,
                   void (*scheduleRestart)()) {
  RestoreJob *job = restoreJob;
  if (!job || job->req != req) {
    if (restoreJob) return sendError(req, 409, "another restore is running");
    if (!req->contentLength()) return sendError(req, 400, "no backup file in the request");
    return sendError(req, 503, "out of memory, try again");
  }
  // From here on every way out ends the job.
  auto fail = [&](int status, const std::string &why) {
    ESP_LOGW(TAG, "Restore refused: %s", why.c_str());
    endRestore(photos, true);
    sendError(req, status, why.c_str());
  };
  if (!job->error.empty()) return fail(job->status, job->error);
  if (job->tar) {
    if (!job->reader.complete()) return fail(400, "the backup file is incomplete (cut off during the download?)");
    if (!job->dbSeen) return fail(400, "this is not a Smart Bins backup (it has no inventory.db)");
    if (job->manifestSeen) {
      JsonDocument m;
      if (deserializeJson(m, job->manifest) || (m["format"] | 0) < 1) {
        return fail(400, "the backup's description (smartbins-backup.json) is damaged");
      }
      if ((m["format"] | 0) > kBackupFormat) return fail(400, "the backup is from a newer firmware, update first");
    }
  } else {
    if (!flushDb(*job)) return fail(job->status, job->error);
    led::FlashGuard guard;
    job->dbFile.close();
  }
  // The SQLite header: magic string, then the schema version (user_version)
  // at offset 60.
  if (job->dbHeadLen < sizeof(job->dbHead) || memcmp(job->dbHead, "SQLite format 3", 16) != 0) {
    return fail(400, "the backup's database is not a SQLite file");
  }
  if (be32(job->dbHead + 60) > static_cast<uint32_t>(inventory::kSchemaVersion)) {
    return fail(400, "the backup's database is from a newer firmware, update first");
  }
  if (job->dbFile) {
    led::FlashGuard guard;
    job->dbFile.close();
  }

  // The swap. The database it replaces stays as kPreviousFile.
  // (No FlashGuard here: replaceFile() takes the database's write hooks,
  // which are the same guard, and it isn't recursive.)
  std::string err;
  if (!db.replaceFile(kDbPath, kIncomingPath, kPreviousPath, err)) return fail(400, err);
  db.syncBins(gridCols, gridRows, err);

  JsonDocument doc;
  doc["ok"] = true;
  inventory::Stats s;
  if (db.stats(s, err) == Result::Ok) doc["parts"] = s.parts;
  doc["photos"] = job->photosRestored;
  doc["photosFailed"] = job->photosFailed;
  if (!job->photoError.empty()) doc["photoError"] = job->photoError;
  bool restarting = false;
  // A bare .db (or an archive without config.json) has no settings: the page
  // says so rather than leaving the tick unanswered.
  if (job->settingsWanted && !job->configSeen) doc["settingsMissing"] = true;
  if (job->settingsWanted && job->configSeen) {
    config::AppConfig cfg;
    if (config::parse(job->config.c_str(), cfg, err) && config.save(cfg, err)) {
      doc["settings"] = true;
      restarting = true;
    } else {
      doc["settings"] = false;
      doc["settingsError"] = err;
    }
  }
  doc["restarting"] = restarting;
  ESP_LOGI(TAG, "Restored: %lld parts, %d photo files (%d failed)%s", (long long)s.parts, job->photosRestored,
           job->photosFailed, restarting ? ", settings, restarting" : "");
  endRestore(photos, false);
  sendJson(req, 200, doc);
  if (restarting) scheduleRestart();
}

// ---- CSV export ----

// Columns of the export, and the names an import recognises. Text fields use
// their database column names; an import also accepts the API's JSON keys
// ("partNumber") and ignores case, spaces and underscores.
enum ColumnKind { kColText, kColId, kColQuantity, kColBin, kColIgnore };
struct Column {
  ColumnKind kind;
  int field;  // TextField for kColText
};
const Column kExportColumns[] = {
    {kColId, 0},
    {kColText, inventory::kPartNumber},
    {kColText, inventory::kManufacturer},
    {kColText, inventory::kCategory},
    {kColText, inventory::kDescription},
    {kColText, inventory::kPackage},
    {kColText, inventory::kValue},
    {kColText, inventory::kUnit},
    {kColQuantity, 0},
    {kColBin, 0},
    {kColText, inventory::kDatasheetUrl},
    {kColText, inventory::kNotes},
};

const char *columnName(const Column &c) {
  switch (c.kind) {
    case kColId: return "id";
    case kColQuantity: return "quantity";
    case kColBin: return "bin";
    case kColText: return inventory::textField(c.field).column;
    default: return "";
  }
}

class CsvExport {
 public:
  CsvExport(InventoryDb &db, char sep) : db_(db), sep_(sep) {}

  size_t fill(uint8_t *buf, size_t maxLen) {
    size_t n = 0;
    while (n < maxLen) {
      if (at_ < pending_.size()) {
        const size_t k = std::min(maxLen - n, pending_.size() - at_);
        memcpy(buf + n, pending_.data() + at_, k);
        at_ += k;
        n += k;
        continue;
      }
      if (done_) break;
      refill();
    }
    return n;
  }

 private:
  void refill() {
    pending_.clear();
    at_ = 0;
    if (!headerDone_) {
      headerDone_ = true;
      pending_ = "\xEF\xBB\xBF";  // UTF-8 byte order mark, for Excel
      for (size_t i = 0; i < sizeof(kExportColumns) / sizeof(kExportColumns[0]); i++) {
        if (i) pending_ += sep_;
        pending_ += columnName(kExportColumns[i]);
      }
      pending_ += "\r\n";
      return;
    }
    // A page of parts at a time keeps RAM flat whatever the inventory's size.
    constexpr int kPage = 20;
    int64_t total = 0;
    std::string err;
    int got = 0;
    const Result r = db_.listParts(offset_, kPage, [&](const Part &p) {
      got++;
      for (size_t i = 0; i < sizeof(kExportColumns) / sizeof(kExportColumns[0]); i++) {
        if (i) pending_ += sep_;
        const Column &c = kExportColumns[i];
        switch (c.kind) {
          case kColId: pending_ += std::to_string(p.id); break;
          case kColQuantity: pending_ += std::to_string(p.quantity); break;
          case kColBin: backup::csvField(pending_, inventory::binName(p.bin), sep_); break;
          case kColText: backup::csvField(pending_, p.text[c.field], sep_); break;
          default: break;
        }
      }
      pending_ += "\r\n";
    }, total, err);
    offset_ += kPage;
    if (r != Result::Ok) ESP_LOGE(TAG, "CSV export: %s", err.c_str());
    if (r != Result::Ok || got < kPage || offset_ >= total) done_ = true;
  }

  InventoryDb &db_;
  char sep_;
  std::string pending_;
  size_t at_ = 0;
  bool headerDone_ = false;
  bool done_ = false;
  int offset_ = 0;
};

// ---- CSV import ----

struct ImportJob {
  AsyncWebServerRequest *req = nullptr;
  uint32_t lastActivity = 0;
  CsvReader reader;
  std::vector<Column> columns;  // from the header line
  bool headerDone = false;
  std::vector<ImportRow> rows;  // waiting for the next transaction
  std::vector<int> lines;       // their line numbers, for error messages
  int added = 0, updated = 0, unchanged = 0, failed = 0;
  std::vector<std::pair<int, std::string>> errors;
  std::string error;
  int status = 400;
};

ImportJob *importJob = nullptr;

// "Part Number", "part_number" and "partNumber" all become "partnumber".
std::string normalized(const std::string &s) {
  std::string out;
  for (char c : s) {
    if (c == ' ' || c == '_' || c == '-') continue;
    out += (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
  }
  return out;
}

Column columnFor(const std::string &title) {
  const std::string n = normalized(title);
  if (n == "id") return {kColId, 0};
  if (n == "quantity" || n == "qty") return {kColQuantity, 0};
  if (n == "bin") return {kColBin, 0};
  for (int f = 0; f < inventory::kTextFieldCount; f++) {
    if (n == normalized(inventory::textField(f).column) || n == normalized(inventory::textField(f).key)) {
      return {kColText, f};
    }
  }
  return {kColIgnore, 0};
}

void importError(ImportJob &job, int line, const std::string &why) {
  job.failed++;
  if (job.errors.size() < kMaxImportErrors) job.errors.emplace_back(line, why);
}

bool flushImport(ImportJob &job, InventoryDb &db) {
  if (job.rows.empty()) return true;
  std::string err;
  auto count = [&](size_t line, ImportOutcome o, const std::string &why) {
    if (o == ImportOutcome::Added) job.added++;
    if (o == ImportOutcome::Updated) job.updated++;
    if (o == ImportOutcome::Unchanged) job.unchanged++;
    if (o == ImportOutcome::Failed) importError(job, static_cast<int>(line), why);
  };
  Result r = db.importParts(job.rows, [&](size_t i, ImportOutcome o, const std::string &why) {
    count(job.lines[i], o, why);
  }, err);
  if (r == Result::Error && job.rows.size() > 1) {
    // The batch was rolled back as a whole (out of memory, most likely, with
    // the heap fragmented): one row per transaction needs the least RAM.
    ESP_LOGW(TAG, "CSV import: batch failed (%s), heap %u free; retrying row by row", err.c_str(),
             (unsigned)heapFree());
    r = Result::Ok;
    for (size_t i = 0; i < job.rows.size() && r == Result::Ok; i++) {
      std::vector<ImportRow> one;
      one.push_back(std::move(job.rows[i]));
      r = db.importParts(one, [&](size_t, ImportOutcome o, const std::string &why) { count(job.lines[i], o, why); },
                         err);
    }
  }
  job.rows.clear();
  job.lines.clear();
  if (r == Result::Ok) return true;
  job.error = r == Result::Busy ? err : "the database failed: " + err;
  job.status = r == Result::Busy ? 503 : 500;
  return false;
}

// Digits only (no sign, no decimals), within `max`.
bool parseCount(const std::string &s, int64_t max, int64_t &out) {
  if (s.empty() || s.size() > 12) return false;
  out = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    out = out * 10 + (c - '0');
  }
  return out <= max;
}

bool importRow(ImportJob &job, InventoryDb &db, const std::vector<std::string> &fields, int line) {
  if (!job.headerDone) {
    job.headerDone = true;
    bool known = false;
    for (const std::string &title : fields) {
      job.columns.push_back(columnFor(title));
      known = known || job.columns.back().kind != kColIgnore;
    }
    if (!known) {
      job.error = "the first line must name the columns (part_number, description, quantity, ...)";
      return false;
    }
    return true;
  }
  ImportRow row;
  bool any = false;
  for (size_t i = 0; i < fields.size() && i < job.columns.size(); i++) {
    std::string v = fields[i];
    // Spreadsheets pad cells; the database trims text fields itself.
    while (!v.empty() && v.back() == ' ') v.pop_back();
    while (!v.empty() && v.front() == ' ') v.erase(0, 1);
    any = any || !v.empty();
    const Column &c = job.columns[i];
    int64_t n;
    switch (c.kind) {
      case kColText: row.fields.text[c.field] = v; break;
      case kColId:
        if (v.empty()) break;
        if (!parseCount(v, INT64_MAX / 16, n)) return importError(job, line, "id must be a number"), true;
        row.id = n;
        break;
      case kColQuantity:
        if (v.empty()) break;  // an update keeps the quantity, a new part gets 0
        if (!parseCount(v, 1000000000, n)) {
          return importError(job, line, "quantity must be a whole number from 0 to 1000000000"), true;
        }
        row.fields.quantity = static_cast<int32_t>(n);
        break;
      case kColBin: {
        inventory::BinRef bin;
        if (!inventory::parseBinName(v, bin)) return importError(job, line, "bin \"" + v + "\" is not like A1"), true;
        row.fields.bin = bin;
        break;
      }
      default: break;
    }
  }
  if (!any) return true;  // a row of empty cells
  job.rows.push_back(std::move(row));
  job.lines.push_back(line);
  return job.rows.size() < kImportBatch || flushImport(job, db);
}

void onImportBody(AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, InventoryDb &db,
                  Security &security) {
  if (index == 0) {
    if (importJob && millis() - importJob->lastActivity < kStaleMs) return;  // busy: answered below
    delete importJob;
    importJob = new (std::nothrow) ImportJob();
    if (!importJob) return;
    importJob->req = req;
    ImportJob *job = importJob;
    job->reader.onRow = [job, &db](const std::vector<std::string> &fields, int line) {
      return importRow(*job, db, fields, line);
    };
    if (!security.allows(req)) {
      job->error = "password required";
      job->status = 401;
    }
    req->onDisconnect([req]() {
      if (importJob && importJob->req == req) {
        delete importJob;
        importJob = nullptr;
      }
    });
  }
  ImportJob *job = importJob;
  if (!job || job->req != req || !job->error.empty()) return;
  job->lastActivity = millis();
  if (!job->reader.feed(data, len) && job->error.empty()) job->error = job->reader.error();
  // Rows complete in this piece are written now rather than waiting for a
  // full batch, so the request never holds more than one piece's worth.
  if (job->error.empty()) flushImport(*job, db);
}

void onImportDone(AsyncWebServerRequest *req, InventoryDb &db) {
  ImportJob *job = importJob;
  if (!job || job->req != req) {
    if (importJob) return sendError(req, 409, "another import is running");
    if (!req->contentLength()) return sendError(req, 400, "no CSV file in the request");
    return sendError(req, 503, "out of memory, try again");
  }
  importJob = nullptr;
  std::unique_ptr<ImportJob> owned(job);
  if (job->error.empty() && !job->reader.finish() && job->error.empty()) job->error = job->reader.error();
  if (job->error.empty()) flushImport(*job, db);
  if (job->error.empty() && !job->headerDone) job->error = "the file is empty";
  JsonDocument doc;
  doc["ok"] = job->error.empty();
  if (!job->error.empty()) doc["error"] = job->error;
  // Batches written before an error stay written: the counts say how far it got.
  doc["added"] = job->added;
  doc["updated"] = job->updated;
  doc["unchanged"] = job->unchanged;
  doc["failed"] = job->failed;
  JsonArray errors = doc["errors"].to<JsonArray>();
  for (const auto &e : job->errors) {
    JsonObject o = errors.add<JsonObject>();
    o["line"] = e.first;
    o["error"] = e.second;
  }
  ESP_LOGI(TAG, "CSV import: %d added, %d updated, %d unchanged, %d failed%s%s (heap %u free, lowest %u)", job->added,
           job->updated, job->unchanged, job->failed, job->error.empty() ? "" : ": ", job->error.c_str(),
           (unsigned)heapFree(), (unsigned)heapMin());
  sendJson(req, job->error.empty() ? 200 : job->status, doc);
}

// ---- Jobs in the background: photos, and the database on the card ----
//
// Moving photos to the card copies megabytes, and the database copies to and
// from the card take a few seconds each; they run in their own task so the
// web server stays responsive, and the page polls /api/backup/info. One job
// at a time: they share the card and the flash.
enum class JobKind : uint8_t {
  Move,         // photos from flash to the card
  Clean,        // photo files no part uses
  CardSave,     // the database to /backups on the card
  CardRestore,  // a database from /backups on the card in place of this one
};

struct PhotoJob {
  volatile bool running = false;
  JobKind kind = JobKind::Move;
  volatile int done = 0;
  int total = 0;
  volatile int failed = 0;
  volatile uint64_t bytes = 0;
  char error[96] = "";
  // The card backup written (CardSave) or read (CardRestore), without the folder.
  char file[48] = "";
  int64_t parts = -1;  // parts in the database after a CardRestore
  // CardRestore with the settings asked for: restored (the board restarts),
  // missing (the copy has none) or failed with settingsError.
  bool settingsWanted = false;
  bool settingsRestored = false;
  bool settingsMissing = false;
  char settingsError[80] = "";
  InventoryDb *db = nullptr;
  PhotoStore *photos = nullptr;
} photoJob;

bool backgroundBusy() { return photoJob.running; }

// Set by registerBackupApi(): the settings a card copy saves and restores,
// and the restart that applies restored settings.
ConfigStore *jobConfig = nullptr;
void (*jobRestart)() = nullptr;

// The settings saved beside a card copy: inventory-....db -> inventory-....json.
String settingsPathFor(const String &dbName) {
  return String(kCardDir) + "/" + dbName.substring(0, dbName.length() - 3) + ".json";
}

// Database backups on the card: /backups/inventory-2026-11-08-1530.db.
const char *kCardDir = "/backups";
const char *kCardTemp = "/backups/save.tmp";
// The copy runs in steps of this size, on the task's stack (see startJob()).
constexpr size_t kCopyStep = 2048;
// Backups listed on the System page, newest first.
constexpr int kCardListMax = 20;

// A file name the page may ask to restore: letters, digits, '-', '_' and
// '.', ending in ".db". Keeps a request from naming a file outside /backups.
bool validBackupName(const String &name) {
  if (name.length() < 4 || name.length() >= sizeof(PhotoJob::file) || !name.endsWith(".db")) return false;
  for (size_t i = 0; i < name.length(); i++) {
    const char c = name[i];
    if (!isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.') return false;
  }
  return name[0] != '.';
}

// The card's database backups, newest first. Their names hold the date and
// time, so the newest sorts last by name.
void listCardBackups(JsonArray out) {
  struct Entry {
    String name;
    uint64_t size;
  };
  std::vector<Entry> found;
  File dir = SD.open(kCardDir);
  if (!dir || !dir.isDirectory()) return;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    String name = f.name();
    const int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    if (f.isDirectory() || !validBackupName(name)) continue;
    found.push_back({name, f.size()});
  }
  std::sort(found.begin(), found.end(), [](const Entry &a, const Entry &b) { return a.name > b.name; });
  for (size_t i = 0; i < found.size() && i < static_cast<size_t>(kCardListMax); i++) {
    JsonObject o = out.add<JsonObject>();
    o["name"] = found[i].name;
    o["size"] = found[i].size;
    // Copies saved by 0.16.0 and later carry the settings beside them.
    o["settings"] = SD.exists(settingsPathFor(found[i].name));
  }
}

void jobError(PhotoJob &job, const std::string &why) {
  job.failed = 1;
  strncpy(job.error, why.c_str(), sizeof(job.error) - 1);
  job.error[sizeof(job.error) - 1] = 0;
}

// The database to the card. Writes are held meanwhile, as for a download,
// so the copy is of one consistent state.
void cardSave(PhotoJob &job) {
  // Name from the clock: inventory-2026-11-08-153012.db. Before network
  // time arrives, the time since start keeps names apart (they then sort
  // before every dated one).
  const time_t t = nowOrZero();
  if (t) {
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(job.file, sizeof(job.file), "inventory-%Y-%m-%d-%H%M%S.db", &tm);
  } else {
    snprintf(job.file, sizeof(job.file), "inventory-0-%lu.db", (unsigned long)millis());
  }
  const String target = String(kCardDir) + "/" + job.file;
  if (!SD.exists(kCardDir)) SD.mkdir(kCardDir);
  if (SD.exists(kCardTemp)) SD.remove(kCardTemp);
  File dst = SD.open(kCardTemp, FILE_WRITE);
  if (!dst) return jobError(job, "can't write to the card");
  holdWrites(*job.db);
  uint8_t buf[kCopyStep];
  bool ok = true;
  uint64_t size = 0;
  {
    File src;
    {
      led::FlashGuard guard;  // reading the FAT partition, see FlashGuard
      src = FFat.open(kDbFile, FILE_READ);
      if (src) size = src.size();
    }
    if (!src) {
      ok = false;
      jobError(job, "can't read the database");
    }
    job.total = static_cast<int>(size);
    while (ok && job.bytes < size) {
      size_t n;
      {
        led::FlashGuard guard;
        n = src.read(buf, sizeof(buf));
      }
      if (!n || dst.write(buf, n) != n) {
        ok = false;
        jobError(job, "copying to the card failed (card full?)");
        break;
      }
      job.bytes = job.bytes + n;
    }
    if (src) {
      led::FlashGuard guard;
      src.close();
    }
  }
  releaseWrites(*job.db);
  dst.close();
  if (ok) {
    File check = SD.open(kCardTemp, FILE_READ);
    ok = check && check.size() == size;
    if (check) check.close();
    if (!ok) jobError(job, "the copy on the card has the wrong size");
  }
  if (ok && !SD.rename(kCardTemp, target)) {
    ok = false;
    jobError(job, "can't finish the copy on the card");
  }
  if (!ok) SD.remove(kCardTemp);
  // The settings beside it (config.json as a full backup holds it), so a
  // restore from the card can bring them back too, like one from a file.
  if (ok && jobConfig) {
    const std::string json = config::serialize(jobConfig->get());
    File f = SD.open(settingsPathFor(job.file), FILE_WRITE);
    if (!f || f.write(reinterpret_cast<const uint8_t *>(json.data()), json.size()) != json.size()) {
      jobError(job, "the database was saved, but the settings could not be written next to it");
    }
  }
  if (ok) ESP_LOGI(TAG, "Database saved to the card: %s, %s", target.c_str(), formatBytes(size));
}

// A database from the card in place of the current one, which is kept as
// the previous database (as by a restore from a file).
void cardRestore(PhotoJob &job) {
  const String source = String(kCardDir) + "/" + job.file;
  File src = SD.open(source, FILE_READ);
  if (!src) return jobError(job, "the backup is not on the card");
  const uint64_t size = src.size();
  job.total = static_cast<int>(size);
  // The same checks as a restore from a file (see onRestoreDone()): a
  // SQLite file, from this firmware's schema or an older one.
  uint8_t head[100];
  if (src.read(head, sizeof(head)) != sizeof(head) || memcmp(head, "SQLite format 3", 16) != 0) {
    return jobError(job, "the backup is not a SQLite database");
  }
  if (be32(head + 60) > static_cast<uint32_t>(inventory::kSchemaVersion)) {
    return jobError(job, "the backup is from a newer firmware, update first");
  }
  src.seek(0);
  // The incoming copy sits next to the current database until the swap; the
  // one kept from the last restore or delete makes room (the swap replaces it).
  const uint64_t free = flashFree() + fileSize(FFat, kIncomingFile) + fileSize(FFat, kPreviousFile);
  if (size + kRestoreMargin > free) {
    char msg[96];
    snprintf(msg, sizeof(msg), "not enough free flash for the database (%s needed)", formatBytes(size + kRestoreMargin));
    return jobError(job, msg);
  }
  File dst;
  {
    led::FlashGuard guard;
    if (FFat.exists(kPreviousFile)) FFat.remove(kPreviousFile);
    if (FFat.exists(kIncomingFile)) FFat.remove(kIncomingFile);
    dst = FFat.open(kIncomingFile, FILE_WRITE);
  }
  if (!dst) return jobError(job, "can't create the database file");
  uint8_t buf[kCopyStep];
  bool ok = true;
  while (job.bytes < size) {
    const size_t n = src.read(buf, sizeof(buf));
    size_t written = 0;
    if (n) {
      led::FlashGuard guard;  // a flash write, see FlashGuard
      written = dst.write(buf, n);
    }
    if (!n || written != n) {
      ok = false;
      jobError(job, n ? "writing the database failed (flash full?)" : "reading the card failed");
      break;
    }
    job.bytes = job.bytes + n;
  }
  {
    led::FlashGuard guard;
    dst.close();
    if (!ok) FFat.remove(kIncomingFile);
  }
  if (!ok) return;
  // The swap (replaceFile() takes the write hooks itself, see onRestoreDone()).
  std::string err;
  if (!job.db->replaceFile(kDbPath, kIncomingPath, kPreviousPath, err)) {
    led::FlashGuard guard;
    if (FFat.exists(kIncomingFile)) FFat.remove(kIncomingFile);
    return jobError(job, err);
  }
  job.db->syncBins(gridCols, gridRows, err);
  inventory::Stats st;
  if (job.db->stats(st, err) == Result::Ok) job.parts = st.parts;
  ESP_LOGI(TAG, "Database restored from the card: %s, %lld parts", source.c_str(), (long long)job.parts);
  if (!job.settingsWanted || !jobConfig) return;
  // The settings saved beside the copy, as a restore from a file does with
  // the archive's config.json: checked, saved, then the board restarts.
  File f = SD.open(settingsPathFor(job.file), FILE_READ);
  if (!f) {
    job.settingsMissing = true;
    return;
  }
  std::string json;
  if (f.size() > kMaxSmallFile) {
    strncpy(job.settingsError, "the saved settings are too large", sizeof(job.settingsError) - 1);
    return;
  }
  json.resize(f.size());
  f.read(reinterpret_cast<uint8_t *>(&json[0]), json.size());
  f.close();
  config::AppConfig cfg;
  if (config::parse(json.c_str(), cfg, err) && jobConfig->save(cfg, err)) {
    job.settingsRestored = true;
    ESP_LOGI(TAG, "Settings restored from the card, restarting");
    if (jobRestart) jobRestart();
  } else {
    strncpy(job.settingsError, err.c_str(), sizeof(job.settingsError) - 1);
  }
}

void photoTask(void *) {
  PhotoJob &job = photoJob;
  switch (job.kind) {
    case JobKind::Move: {
      int64_t id = 0;
      std::string name, err;
      while (job.db->nextPhoto(id, id, name, err) == Result::Ok) {
        uint64_t moved = 0;
        if (!job.photos->moveToCard(name, moved, err)) {
          job.failed = job.failed + 1;
          if (!job.error[0]) strncpy(job.error, err.c_str(), sizeof(job.error) - 1);
        }
        job.bytes = job.bytes + moved;
        job.done = job.done + 1;
      }
      break;
    }
    case JobKind::Clean: {
      // In use: the part named by the photo's id prefix still has this photo.
      uint64_t freed = 0;
      const int removed = job.photos->removeUnused([](const std::string &name) {
        const int64_t id = atoll(name.c_str());
        Part p;
        std::string err;
        const Result r = photoJob.db->getPart(id, p, err);
        // Unsure (database error): keep it.
        return r == Result::Error || (r == Result::Ok && p.photo == name);
      }, freed);
      job.done = removed;
      job.bytes = freed;
      break;
    }
    case JobKind::CardSave: cardSave(job); break;
    case JobKind::CardRestore: cardRestore(job); break;
  }
  ESP_LOGI(TAG, "Job %d finished: %d done, %d failed, %llu bytes%s%s", static_cast<int>(job.kind), job.done,
           job.failed, (unsigned long long)job.bytes, job.error[0] ? ": " : "", job.error);
  job.running = false;
  vTaskDelete(nullptr);
}

// `file`: the card backup to restore (CardRestore only).
bool startJob(JobKind kind, InventoryDb &db, PhotoStore &photos, const char *file, std::string &err,
              bool settings = false) {
  if (photoJob.running) return err = "another job is running, try again when it ends", false;
  photoJob.settingsWanted = settings;
  photoJob.settingsRestored = false;
  photoJob.settingsMissing = false;
  photoJob.settingsError[0] = 0;
  photoJob.kind = kind;
  photoJob.done = 0;
  photoJob.failed = 0;
  photoJob.bytes = 0;
  photoJob.error[0] = 0;
  photoJob.file[0] = 0;
  if (file) {
    strncpy(photoJob.file, file, sizeof(photoJob.file) - 1);
    photoJob.file[sizeof(photoJob.file) - 1] = 0;
  }
  photoJob.parts = -1;
  photoJob.db = &db;
  photoJob.photos = &photos;
  photoJob.total = 0;
  if (kind == JobKind::Move || kind == JobKind::Clean) {
    inventory::Stats s;
    photoJob.total = db.stats(s, err) == Result::Ok ? static_cast<int>(s.photos) : 0;
  }
  photoJob.running = true;
  // 10 KB: SQLite's statements need several KB of stack (as the benchmark),
  // and the card copies keep their 2 KB step buffer there too. Core 0, away
  // from the LED tasks.
  if (xTaskCreatePinnedToCore(photoTask, "photos", 10240, nullptr, 1, nullptr, 0) != pdPASS) {
    photoJob.running = false;
    return err = "not enough memory to start, try again", false;
  }
  return true;
}

void writePhotoJob(JsonObject out) {
  static const char *const kKinds[] = {"move", "clean", "cardSave", "cardRestore"};
  // Plain copies: ArduinoJson takes no volatile values.
  out["running"] = static_cast<bool>(photoJob.running);
  out["kind"] = kKinds[static_cast<int>(photoJob.kind)];
  out["done"] = static_cast<int>(photoJob.done);
  out["total"] = photoJob.total;
  out["failed"] = static_cast<int>(photoJob.failed);
  out["bytes"] = static_cast<uint64_t>(photoJob.bytes);
  if (photoJob.error[0]) out["error"] = photoJob.error;
  if (photoJob.file[0]) out["file"] = photoJob.file;
  if (photoJob.parts >= 0) out["parts"] = photoJob.parts;
  if (photoJob.settingsRestored) out["settings"] = true;
  if (photoJob.settingsMissing) out["settingsMissing"] = true;
  if (photoJob.settingsError[0]) out["settingsError"] = photoJob.settingsError;
}

}  // namespace

void registerBackupApi(AsyncWebServer &server, InventoryDb &db, PhotoStore &photos, ConfigStore &config,
                       Security &security, void (*scheduleRestart)(), int cols, int rows) {
  gridCols = cols;
  gridRows = rows;
  jobConfig = &config;
  jobRestart = scheduleRestart;
  // An incoming database left by a restart in the middle of a restore.
  {
    led::FlashGuard guard;  // exists() reads the FAT too
    if (FFat.exists(kIncomingFile)) FFat.remove(kIncomingFile);
  }

  server.on(AsyncURIMatcher::exact("/api/backup/info"), HTTP_GET, [&db, &photos](AsyncWebServerRequest *req) {
    JsonDocument doc;
    inventory::Stats s;
    std::string err;
    // First: the database takes its own flash guard, after its lock.
    if (db.stats(s, err) == Result::Ok) {
      doc["parts"] = s.parts;
      doc["photos"] = s.photos;
    }
    {
      // File sizes, free space and the photo folder come from the FAT in
      // flash: the strips pause meanwhile (see FlashGuard).
      led::FlashGuard guard;
      doc["dbBytes"] = fileSize(FFat, kDbFile);
      doc["flashFree"] = flashFree();
      doc["card"] = photos.onCard();
      // Only worth the directory walk when there is a card to move them to.
      doc["flashPhotos"] = photos.flashPhotoCount() > 0;
      if (FFat.exists(kPreviousFile)) doc["previousBytes"] = fileSize(FFat, kPreviousFile);
    }
    if (photos.onCard()) listCardBackups(doc["cardBackups"].to<JsonArray>());
    writePhotoJob(doc["job"].to<JsonObject>());
    sendJson(req, 200, doc);
  });

  // exact(): /api/backup/db and the others are separate handlers.
  server.on(AsyncURIMatcher::exact("/api/backup"), HTTP_GET, [&db, &photos, &config](AsyncWebServerRequest *req) {
    if (!db.isOpen()) return sendError(req, 503, "inventory database unavailable");
    JsonDocument m;
    m["format"] = kBackupFormat;
    m["app"] = "smart-bins";
    m["firmware"] = SB_FW_VERSION;
    m["schema"] = inventory::kSchemaVersion;
    m["created"] = nowOrZero();
    m["device"] = String(config.get().deviceName);
    inventory::Stats s;
    std::string err;
    if (db.stats(s, err) == Result::Ok) {
      m["parts"] = s.parts;
      m["photos"] = s.photos;
    }
    std::string manifest;
    serializeJsonPretty(m, manifest);
    auto stream = std::make_shared<BackupStream>(db, photos, std::move(manifest), config::serialize(config.get()));
    AsyncWebServerResponse *res = req->beginChunkedResponse(
        "application/x-tar", [stream](uint8_t *buf, size_t maxLen, size_t) { return stream->fill(buf, maxLen); });
    attachment(res, downloadName("smartbins-backup", ".tar"));
    req->send(res);
  });

  // A database file (the current one, or the one kept from the last restore)
  // with its size known up front.
  auto sendDbFile = [&db](AsyncWebServerRequest *req, const char *file, const char *stem, bool hold) {
    // fileSize() reads the FAT under a FlashGuard; 0 means no such file.
    const uint64_t size = fileSize(FFat, file);
    if (!size) return sendError(req, 404, "no such database file");
    // Lives as long as the response: releases the hold when the response
    // is done or dropped.
    struct Source {
      InventoryDb *db = nullptr;
      bool hold = false;
      String path;
      ~Source() {
        if (hold) releaseWrites(*db);
      }
    };
    auto src = std::make_shared<Source>();
    src->db = &db;
    src->path = file;
    if (hold) {
      holdWrites(db);
      src->hold = true;
    }
    AsyncWebServerResponse *res = req->beginResponse(
        "application/vnd.sqlite3", size, [src](uint8_t *buf, size_t maxLen, size_t index) -> size_t {
          return readAt(FFat, src->path, index, buf, maxLen);
        });
    attachment(res, downloadName(stem, ".db"));
    req->send(res);
  };
  server.on(AsyncURIMatcher::exact("/api/backup/db"), HTTP_GET, [sendDbFile](AsyncWebServerRequest *req) {
    sendDbFile(req, kDbFile, "smartbins-inventory", true);
  });
  server.on(AsyncURIMatcher::exact("/api/backup/previous"), HTTP_GET, [sendDbFile](AsyncWebServerRequest *req) {
    sendDbFile(req, kPreviousFile, "smartbins-before-restore", false);
  });
  server.on(AsyncURIMatcher::exact("/api/backup/previous/delete"), HTTP_POST, [](AsyncWebServerRequest *req) {
    {
      led::FlashGuard guard;
      if (FFat.exists(kPreviousFile)) FFat.remove(kPreviousFile);
    }
    JsonDocument doc;
    doc["ok"] = true;
    sendJson(req, 200, doc);
  });

  server.on(
      AsyncURIMatcher::exact("/api/backup/restore"), HTTP_POST,
      [&db, &photos, &config, scheduleRestart](AsyncWebServerRequest *req) {
        onRestoreDone(req, db, photos, config, scheduleRestart);
      },
      nullptr,
      [&photos, &security](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
        onRestoreBody(req, data, len, index, total, photos, security);
      });

  server.on(AsyncURIMatcher::exact("/api/parts.csv"), HTTP_GET, [&db](AsyncWebServerRequest *req) {
    if (!db.isOpen()) return sendError(req, 503, "inventory database unavailable");
    const AsyncWebParameter *s = param(req, "sep");
    const char sep = s && (s->value() == ";" || s->value() == "semicolon") ? ';' : ',';
    auto csv = std::make_shared<CsvExport>(db, sep);
    AsyncWebServerResponse *res = req->beginChunkedResponse(
        "text/csv; charset=utf-8", [csv](uint8_t *buf, size_t maxLen, size_t) { return csv->fill(buf, maxLen); });
    attachment(res, downloadName("smartbins-parts", ".csv"));
    req->send(res);
  });

  server.on(
      AsyncURIMatcher::exact("/api/parts/import"), HTTP_POST,
      [&db](AsyncWebServerRequest *req) { onImportDone(req, db); }, nullptr,
      [&db, &security](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t) {
        onImportBody(req, data, len, index, db, security);
      });

  server.on(AsyncURIMatcher::exact("/api/photos/move-to-card"), HTTP_POST, [&db, &photos](AsyncWebServerRequest *req) {
    if (!photos.onCard()) return sendError(req, 409, "no microSD card");
    std::string err;
    if (!startJob(JobKind::Move, db, photos, nullptr, err)) return sendError(req, 409, err.c_str());
    JsonDocument doc;
    writePhotoJob(doc.to<JsonObject>());
    sendJson(req, 200, doc);
  });
  server.on(AsyncURIMatcher::exact("/api/photos/clean"), HTTP_POST, [&db, &photos](AsyncWebServerRequest *req) {
    if (!db.isOpen()) return sendError(req, 503, "inventory database unavailable");
    std::string err;
    if (!startJob(JobKind::Clean, db, photos, nullptr, err)) return sendError(req, 409, err.c_str());
    JsonDocument doc;
    writePhotoJob(doc.to<JsonObject>());
    sendJson(req, 200, doc);
  });

  // The database to and from the microSD card (System page). Both copy in
  // the background; the page follows the job through /api/backup/info.
  server.on(AsyncURIMatcher::exact("/api/backup/card/save"), HTTP_POST, [&db, &photos](AsyncWebServerRequest *req) {
    if (!photos.onCard()) return sendError(req, 409, "no microSD card");
    if (!db.isOpen()) return sendError(req, 503, "inventory database unavailable");
    std::string err;
    if (!startJob(JobKind::CardSave, db, photos, nullptr, err)) return sendError(req, 409, err.c_str());
    JsonDocument doc;
    writePhotoJob(doc.to<JsonObject>());
    sendJson(req, 200, doc);
  });
  server.on(AsyncURIMatcher::exact("/api/backup/card/restore"), HTTP_POST, [&db, &photos](AsyncWebServerRequest *req) {
    if (!photos.onCard()) return sendError(req, 409, "no microSD card");
    const AsyncWebParameter *name = param(req, "name");
    if (!name || !validBackupName(name->value())) return sendError(req, 400, "bad backup name");
    if (restoreJob) return sendError(req, 409, "another restore is running");
    // A download reads the database file while the swap would replace it.
    if (writesHeld()) return sendError(req, 409, "the database is being downloaded, try again when it is done");
    std::string err;
    // settings=1: also the settings saved beside the copy (the board restarts).
    const bool settings = hasParam(req, "settings") && param(req, "settings")->value() == "1";
    if (!startJob(JobKind::CardRestore, db, photos, name->value().c_str(), err, settings)) {
      return sendError(req, 409, err.c_str());
    }
    JsonDocument doc;
    writePhotoJob(doc.to<JsonObject>());
    sendJson(req, 200, doc);
  });

  // Delete DB (System page): an empty database takes the current one's
  // place. The current one is kept as the "previous" database, like before
  // a restore, so it can still be downloaded until the next restore or
  // delete. Photos stay; "Remove unused photos" clears them.
  server.on(AsyncURIMatcher::exact("/api/backup/delete-db"), HTTP_POST, [&db](AsyncWebServerRequest *req) {
    if (restoreJob) return sendError(req, 409, "a restore is running");
    if (backgroundBusy()) return sendError(req, 409, "another job is running, try again when it ends");
    // A download of the database holds writes while it reads the file.
    if (writesHeld()) return sendError(req, 409, "the database is being downloaded, try again when it is done");
    std::string err;
    // No FlashGuard here: replaceFile() takes the database's write hooks
    // (see onRestoreDone()).
    if (!db.replaceFile(kDbPath, nullptr, kPreviousPath, err)) return sendError(req, 500, err.c_str());
    db.syncBins(gridCols, gridRows, err);
    ESP_LOGI(TAG, "Database deleted (kept aside as %s)", kPreviousFile);
    JsonDocument doc;
    doc["ok"] = true;
    sendJson(req, 200, doc);
  });
}

}  // namespace sb
