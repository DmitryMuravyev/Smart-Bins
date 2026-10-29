#pragma once

// Loads and saves /config.json on LittleFS.

#include <string>

#include "AppConfig.h"

namespace sb {

class ConfigStore {
 public:
  // Loads the config, or writes defaults when the file is missing. An invalid
  // file is kept as /config.bad.json for inspection and replaced by defaults.
  void begin();

  const config::AppConfig &get() const { return cfg_; }

  // Validates and writes atomically (temp file + rename). Returns false with a reason.
  bool save(const config::AppConfig &cfg, std::string &error);

  // Deletes the config file; defaults apply after the next boot.
  void erase();

 private:
  config::AppConfig cfg_;
};

}  // namespace sb
