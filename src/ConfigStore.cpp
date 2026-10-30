#include "ConfigStore.h"

#include <Arduino.h>
#include <LittleFS.h>

namespace sb {

namespace {
const char *TAG = "config";
const char *kPath = "/config.json";
const char *kTempPath = "/config.json.tmp";
const char *kBadPath = "/config.bad.json";
}  // namespace

void ConfigStore::begin() {
  cfg_ = config::AppConfig();
  File f = LittleFS.exists(kPath) ? LittleFS.open(kPath, "r") : File();
  if (!f) {
    ESP_LOGI(TAG, "No %s, writing defaults", kPath);
    std::string err;
    save(cfg_, err);
    return;
  }
  // Read in small chunks into a string sized up front: one allocation, and no
  // large stack buffer on the loop task.
  std::string json;
  json.reserve(f.size());
  while (f.available()) {
    char buf[256];
    size_t n = f.readBytes(buf, sizeof(buf));
    json.append(buf, n);
  }
  f.close();

  std::string err;
  config::AppConfig loaded;
  if (config::parse(json.c_str(), loaded, err)) {
    cfg_ = loaded;
    ESP_LOGI(TAG, "Loaded %s", kPath);
    return;
  }
  // Keep the broken file for inspection (download it over the web UI later)
  // rather than deleting the user's settings, and boot with defaults.
  ESP_LOGE(TAG, "%s is invalid (%s); kept as %s, using defaults", kPath, err.c_str(), kBadPath);
  if (LittleFS.exists(kBadPath)) LittleFS.remove(kBadPath);
  LittleFS.rename(kPath, kBadPath);
  save(cfg_, err);
}

// Writes the new file next to the old one and then renames it over the old
// one. A power cut at any point leaves either the complete old file or the
// complete new one, never a half-written config.
bool ConfigStore::save(const config::AppConfig &cfg, std::string &error) {
  if (!config::validate(cfg, error)) return false;
  const std::string json = config::serialize(cfg);
  File f = LittleFS.open(kTempPath, "w");
  if (!f) {
    error = "cannot open config for writing";
    return false;
  }
  size_t written = f.write(reinterpret_cast<const uint8_t *>(json.data()), json.size());
  f.close();
  if (written != json.size()) {
    LittleFS.remove(kTempPath);
    error = "filesystem full";
    return false;
  }
  // LittleFS replaces an existing target atomically on rename. The remove-and-
  // retry fallback only runs if a filesystem layer refuses to overwrite; it has
  // a short window with no config file, which then means defaults on next boot.
  if (!LittleFS.rename(kTempPath, kPath)) {
    if (LittleFS.exists(kPath)) LittleFS.remove(kPath);
    if (!LittleFS.rename(kTempPath, kPath)) {
      error = "cannot replace config file";
      return false;
    }
  }
  cfg_ = cfg;
  ESP_LOGI(TAG, "Saved %s (%u bytes)", kPath, (unsigned)json.size());
  return true;
}

void ConfigStore::erase() {
  if (LittleFS.exists(kPath)) LittleFS.remove(kPath);
  ESP_LOGW(TAG, "Config erased; defaults apply after restart");
}

}  // namespace sb
