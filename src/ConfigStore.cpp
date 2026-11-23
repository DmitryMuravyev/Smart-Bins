#include "ConfigStore.h"

#include <Arduino.h>
#include <FFat.h>

#include "led/StripOutput.h"

namespace sb {

namespace {
const char *TAG = "config";
const char *kPath = "/config.json";
// Used while saving, see save().
const char *kTempPath = "/config.json.tmp";
const char *kBackupPath = "/config.json.bak";
const char *kBadPath = "/config.bad.json";

// Reads a whole file. Small chunks into a string sized up front: one
// allocation, and no large stack buffer on the loop task.
bool readFile(const char *path, std::string &out) {
  File f = FFat.exists(path) ? FFat.open(path, "r") : File();
  if (!f) return false;
  out.clear();
  out.reserve(f.size());
  while (f.available()) {
    char buf[256];
    size_t n = f.readBytes(buf, sizeof(buf));
    out.append(buf, n);
  }
  f.close();
  return true;
}

// remove() of a file that isn't there logs an error in the FS layer; most of
// the removals here are clean-ups of files that usually don't exist.
void removeIfThere(const char *path) {
  if (FFat.exists(path)) FFat.remove(path);
}
}  // namespace

void ConfigStore::begin() {
  cfg_ = config::AppConfig();
  std::string json, err;
  config::AppConfig loaded;

  // A power cut during save() can leave the new file only as config.json.tmp
  // (complete, it was closed before any rename) and the old one as
  // config.json.bak. Prefer the new one if it parses, then the old one.
  if (!FFat.exists(kPath)) {
    for (const char *spare : {kTempPath, kBackupPath}) {
      if (readFile(spare, json) && config::parse(json.c_str(), loaded, err)) {
        ESP_LOGW(TAG, "%s missing, recovered it from %s", kPath, spare);
        FFat.rename(spare, kPath);
        break;
      }
    }
  }
  removeIfThere(kTempPath);
  removeIfThere(kBackupPath);

  if (!readFile(kPath, json)) {
    ESP_LOGI(TAG, "No %s, writing defaults", kPath);
    save(cfg_, err);
    return;
  }
  if (config::parse(json.c_str(), loaded, err)) {
    cfg_ = loaded;
    ESP_LOGI(TAG, "Loaded %s", kPath);
    if (loaded.pinsReset) ESP_LOGW(TAG, "%s is from another board: pin map reset to this board's defaults", kPath);
    return;
  }
  // Keep the broken file for inspection (download it over the web UI later)
  // rather than deleting the user's settings, and boot with defaults.
  ESP_LOGE(TAG, "%s is invalid (%s); kept as %s, using defaults", kPath, err.c_str(), kBadPath);
  removeIfThere(kBadPath);
  FFat.rename(kPath, kBadPath);
  save(cfg_, err);
}

// FAT can't rename over an existing file, so the replacement takes three
// steps: write config.json.tmp, move config.json aside to config.json.bak,
// move the new file in, then drop the backup. Wherever a power cut lands,
// either config.json is complete (old or new), or it is missing and begin()
// restores it from the .tmp or .bak file. Never a half-written config.
bool ConfigStore::save(const config::AppConfig &cfg, std::string &error) {
  if (!config::validate(cfg, error)) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!write(cfg, error)) return false;
  cfg_ = cfg;
  // `cfg` was built from get(), so it carries any change update() held back.
  pending_ = false;
  return true;
}

bool ConfigStore::update(const config::AppConfig &cfg, std::string &error) {
  if (!config::validate(cfg, error)) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  cfg_ = cfg;
  changedAt_ = millis();
  pending_ = true;
  return true;
}

void ConfigStore::loop() {
  // 1.5s: long enough for a few quick taps or slider moves to end up in one
  // write, short enough that a power cut right after a change rarely loses it.
  constexpr uint32_t kSaveDelayMs = 1500;
  if (pending_ && millis() - changedAt_ >= kSaveDelayMs) flush();
}

void ConfigStore::flush() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!pending_) return;
  pending_ = false;
  std::string err;
  // Not retried: a full or failing flash would fail again. The settings stay
  // in use until the next restart, and the next change tries again.
  if (!write(cfg_, err)) ESP_LOGE(TAG, "Saving %s failed: %s", kPath, err.c_str());
}

bool ConfigStore::write(const config::AppConfig &cfg, std::string &error) {
  const std::string json = config::serialize(cfg);
  // LED strips pause while the file is written (see led::FlashGuard).
  led::FlashGuard guard;
  File f = FFat.open(kTempPath, "w");
  if (!f) {
    error = "cannot open config for writing";
    return false;
  }
  size_t written = f.write(reinterpret_cast<const uint8_t *>(json.data()), json.size());
  f.close();
  if (written != json.size()) {
    removeIfThere(kTempPath);
    error = "storage full";
    return false;
  }
  removeIfThere(kBackupPath);
  if (FFat.exists(kPath) && !FFat.rename(kPath, kBackupPath)) {
    removeIfThere(kTempPath);
    error = "cannot replace config file";
    return false;
  }
  if (!FFat.rename(kTempPath, kPath)) {
    FFat.rename(kBackupPath, kPath);  // put the old one back
    error = "cannot replace config file";
    return false;
  }
  removeIfThere(kBackupPath);
  ESP_LOGI(TAG, "Saved %s (%u bytes)", kPath, (unsigned)json.size());
  return true;
}

void ConfigStore::erase() {
  std::lock_guard<std::mutex> lock(mutex_);
  pending_ = false;  // a held-back change must not bring the file back
  led::FlashGuard guard;
  removeIfThere(kPath);
  ESP_LOGW(TAG, "Config erased; defaults apply after restart");
}

}  // namespace sb
