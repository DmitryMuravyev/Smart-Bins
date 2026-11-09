#pragma once

// Loads and saves /config.json on the FAT partition.

#include <cstdint>
#include <mutex>
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

  // For settings changed from a page while it is in use (how a bin lights up,
  // the effects, the grid brightness): validates and takes the new settings
  // at once, and writes the file from loop() once no change has come for a
  // moment (kSaveDelayMs). The answer to the page doesn't wait for the flash
  // write (a few hundred ms, the LED strips pause meanwhile), and a run of
  // changes (presets tapped one after another) costs one write instead of
  // one each. Returns false with a reason when the settings are invalid.
  bool update(const config::AppConfig &cfg, std::string &error);

  // Writes a change update() is holding back once it has rested; call from the main loop.
  void loop();

  // Writes a held-back change now, e.g. before a restart.
  void flush();

 private:
  // The file write itself; the caller holds mutex_.
  bool write(const config::AppConfig &cfg, std::string &error);

  config::AppConfig cfg_;
  // Guards cfg_ against update() (web server task) and flush() (main loop)
  // at the same time, and keeps two writes of the file apart.
  std::mutex mutex_;
  volatile bool pending_ = false;  // update() has a change not yet written
  volatile uint32_t changedAt_ = 0;
};

}  // namespace sb
