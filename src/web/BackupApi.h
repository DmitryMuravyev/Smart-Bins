#pragma once

// Backup and restore.
//
//   GET  /api/backup/info                  what there is to back up, the database kept from
//                                          the last restore, and the photo job's progress
//   GET  /api/backup                       the full backup: one .tar file with
//                                            smartbins-backup.json  what made it (format, firmware, schema, counts)
//                                            config.json            the settings
//                                            inventory.db           the SQLite database
//                                            photos/NAME.jpg, photos/NAME-t.jpg
//   GET  /api/backup/db                    the database file alone (opens in any SQLite tool)
//   POST /api/backup/restore[?settings=1]  body: a backup .tar or a bare .db file. Replaces the
//                                          inventory, adds the photos; settings=1 also restores
//                                          config.json and restarts. The database it replaces is
//                                          kept (see below), so a restore can be undone.
//   GET  /api/backup/previous              that kept database, as a .db file
//   POST /api/backup/previous/delete       removes it
//   GET  /api/parts.csv[?sep=;]            every part as CSV for a spreadsheet (, or ;)
//   POST /api/parts/import                 body: CSV with a header line. A row whose id
//                                          names an existing part updates the columns given;
//                                          any other row adds a part
//   POST /api/photos/move-to-card          moves photos from flash to the microSD card
//   POST /api/photos/clean                 deletes photo files no part refers to
//                                          (both run in the background; progress in /api/backup/info)
//
// While the database file is being copied into a backup, changes to the
// inventory are refused with 503 (see InventoryDb::holdWrites()): a moment for
// a small inventory, and the page asks to try again.

class AsyncWebServer;

namespace sb {

class ConfigStore;
class PhotoStore;
class Security;
namespace inventory {
class InventoryDb;
}

void registerBackupApi(AsyncWebServer &server, inventory::InventoryDb &db, PhotoStore &photos, ConfigStore &config,
                       Security &security, void (*scheduleRestart)(), int gridCols, int gridRows);

}  // namespace sb
