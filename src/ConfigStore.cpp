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
  File f = LittleFS.open(kPath, "r");
  if (!f) {
    ESP_LOGI(TAG, "No %s, writing defaults", kPath);
    std::string err;
    save(cfg_, err);
    return;
  }
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
  ESP_LOGE(TAG, "%s is invalid (%s); kept as %s, using defaults", kPath, err.c_str(), kBadPath);
  LittleFS.remove(kBadPath);
  LittleFS.rename(kPath, kBadPath);
  save(cfg_, err);
}

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
  LittleFS.remove(kPath);
  if (!LittleFS.rename(kTempPath, kPath)) {
    error = "cannot replace config file";
    return false;
  }
  cfg_ = cfg;
  ESP_LOGI(TAG, "Saved %s (%u bytes)", kPath, (unsigned)json.size());
  return true;
}

void ConfigStore::erase() {
  LittleFS.remove(kPath);
  ESP_LOGW(TAG, "Config erased; defaults apply after restart");
}

}  // namespace sb
