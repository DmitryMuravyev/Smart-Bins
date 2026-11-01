# HTTP API

Everything the UI does goes through a small JSON API, handy for scripts and home automation. Endpoints that change something accept POST with a form-encoded body and, for quick tests from the address bar, GET with the same parameters. Errors come back with a 4xx/5xx status and `{"error": "message"}`. With a password set, requests need HTTP digest auth (`curl --digest -u admin:PASSWORD ...`).

| Endpoint | What it does |
|---|---|
| `GET /api/system` | firmware, chip, memory (`heapFree`, `heapMin`; `psramFree`, `psramTotal` on a board with PSRAM), storage, Wi-Fi, password on/off, the last restart's reason (`restart`: `power-on`, `restart`, `crash`, `watchdog`, `hang`, `brownout`, `reset-pin`, `other`; `restartFault` is true for the ones that point at a problem) and whether a firmware upload is possible (`firmwareUpload`) |
| `POST /api/system/restart` | restarts the board |
| `GET /api/config`, `POST /api/config` | reads / replaces `config.json` (JSON body), then restarts |
| `GET /api/pins` | the pin map in use (`pins`), and the GPIOs each kind of function may take (`outputs`, `inputs`) |
| `POST /api/firmware` | installs new firmware: a multipart upload with the file in the field `firmware` (`curl -F firmware=@smartbins-1.0.0-esp32-8mb-update.bin ...`); restarts with it when the image is complete and valid, otherwise keeps the running one and answers the reason |
| `/api/locate?x=3&y=2` | lights one bin; optional `color=FF0000`, `mode=steady\|slow-pulse\|fast-pulse`, `s=10` (0 = until cleared) |
| `/api/locate/clear[?x=3&y=2]` | fades every located bin out, or just the one given |
| `/api/locate/defaults` | without parameters: reads the defaults; with `color`, `mode`, `s`: changes and saves them |
| `/api/led/test?pattern=walk\|chain\|rows\|columns\|colors\|off` | grid test patterns |
| `/api/led/brightness?value=1..255` | grid brightness, applied and saved |
| `/api/effects` | without parameters: the running `effect` (`remainingS` until its timer stops it), `microphone` (whether one was found) and the running effect's settings. `effect=off\|fire\|sparks\|relax\|demo\|spectrum\|beat\|pulse` starts one with its own saved settings (the music effects answer 409 when `microphone` is false); `speed`, `intensity`, `brightness` (1..255), `palette=auto\|heat\|ocean\|forest\|lava\|rainbow\|party` and `timer` (minutes, 0 = keep running) change and save the settings of the effect named with them, or else of the running one (`effects.<name>` in `config.json`) |
| `GET /api/led/status` | estimated grid current, frame rate, current limit active, the running `effect` and `effectRemainingS`, lit bins (`lit: [[x, y], ...]`) and their `version`; `/api/locate` and `/api/locate/clear` answer with the same status |
| `GET /api/lights` | the work lights (`id`, name, type, state) |
| `/api/lights/set?id=0&on=toggle` | changes one light: `on=1\|0\|toggle`, `level=1..255`, `color=FF8800`, `kelvin=2700`, `effect=solid\|breathe\|rainbow` |
| `/api/lights/off` | every light off |
| `POST /api/security` | `password=...` sets the password (4–64 characters), empty removes it |
| `GET /api/parts` | parts, 50 at a time, with the `total` number of matches. All parameters are optional: `q=stm32 lqfp` (every word must appear in some text field, any case, also Cyrillic and accented letters), `category=MCU` and `manufacturer=TI` (exact, any case), `x=3&y=2` (parts in that bin; `x=-1` for parts in no bin), `sort=id\|partNumber\|manufacturer\|category\|description\|package\|value\|quantity\|bin\|updated` with `order=asc\|desc` (empty text values always last), `offset=0&limit=50` (`limit` up to 100) |
| `GET /api/parts/values?field=category` | the distinct values of one text field with their number of `parts`, most used first; `q=res` narrows them like a search word, `limit` up to 500 (default 50). For filter lists and autocomplete |
| `GET /api/parts/bins?q=stm32` | where the parts matching a search are: one entry per bin holding any, `{x, y, parts}`; takes the same `q`, `category` and `manufacturer` filters as `/api/parts`; parts in no bin come as `x: -1`. For the bin grid view |
| `GET /api/part?id=N` | one part: `partNumber`, `manufacturer`, `category`, `description`, `package`, `value`, `unit`, `datasheetUrl`, `notes`, `quantity`, `bin` (`{x, y}` or `null`), `photo`, `created`, `updated` (Unix seconds, 0 before network time) |
| `/api/part/add?partNumber=NE555&quantity=10&x=2&y=1` | adds a part from any of the fields above (a part number or a description is required) |
| `/api/part/update?id=N&...` | changes only the fields given; `x=-1` takes a part out of its bin |
| `/api/part/delete?id=N` | deletes a part and its photo |
| `GET /api/photos` | where photos are stored (`storage: card\|flash`, `used`, `total`, `available` bytes) and the sizes the page resizes to (`maxSide`, `thumbSide`, `maxBytes`, `maxThumbBytes`) |
| `POST /api/part/photo?id=N` | body: the photo as `image/jpeg` (already resized; at most 400 KB); gives the part a new photo and answers the part with its new `photo` name. `curl --data-binary @photo.jpg -H 'Content-Type: image/jpeg' ...` |
| `POST /api/part/thumb?id=N&name=NAME` | body: the photo's thumbnail (`image/jpeg`, at most 48 KB); `NAME` is the part's current `photo` |
| `/api/part/photo/delete?id=N` | removes a part's photo |
| `GET /photos/NAME.jpg`, `GET /photos/NAME-t.jpg` | a photo and its thumbnail (a missing thumbnail gives the photo); cached by browsers, since a new photo gets a new name |
| `GET /api/backup` | the full backup as a `.tar` file: `smartbins-backup.json` (format, firmware, schema), `config.json`, `inventory.db`, `photos/*.jpg`. While the database file is being sent (a moment), changes to parts answer 503 |
| `GET /api/backup/db` | the database file alone (SQLite; opens in DB Browser for SQLite and the like) |
| `POST /api/backup/restore[?settings=1]` | body: a backup `.tar` or a `.db` file. `curl --data-binary @smartbins-backup.tar ...` Answers `{parts, photos, photosFailed, settings, restarting}` |
| `GET /api/backup/previous`, `POST /api/backup/previous/delete` | the database kept from before the last restore |
| `GET /api/backup/info` | database size, the kept database, whether photos are still in flash with a card in, and the photo job's progress |
| `GET /api/parts.csv[?sep=;]` | every part as CSV (UTF-8 with BOM, `,` or `;`) |
| `POST /api/parts/import` | body: CSV with a header line (`id`, `part_number`, ..., `quantity`, `bin`, ...); answers `{added, updated, failed, errors: [{line, error}]}` |
| `POST /api/photos/move-to-card`, `POST /api/photos/clean` | move photos from flash to the microSD card / delete photo files no part uses; run in the background (progress in `/api/backup/info`) |
| `/api/part/adjust?id=N&delta=-1` | quantity up or down, never below 0 |
| `GET /api/bins` | every bin with its `label` and number of `parts` (`outside: true` for a bin left outside after the grid shrank) |
| `/api/bin/label?x=0&y=0&label=Resistors` | names a bin; an empty label goes back to the A1-style name |
| `GET /api/inventory/stats` | the inventory figures shown on the System page |
| `POST /api/inventory/benchmark?n=2000`, then `GET` | inserts n test parts, times typical queries, removes them; the `GET` reports progress and results (the LED strips pause while the parts are written) |
| `ws://smartbins.local/ws` | WebSocket: pushes light changes, grid current and system status as JSON |

An address under `/api/` that doesn't exist answers 404 with `{"error": "no such API endpoint"}`.
