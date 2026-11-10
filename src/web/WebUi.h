#pragma once

// Serves the web UI that tools/embed_web.py bundled into the firmware.
//
// The files in web/ are stored gzipped in flash (src/generated/WebAssets.h)
// and sent as they are with "Content-Encoding: gzip": every browser unpacks
// them, and the board never has to compress or read a filesystem.

class AsyncWebServer;

namespace sb {

class WifiLink;

// Registers one route per bundled file, plus "/" (index.html) and "/config"
// (a redirect to the settings page, for old bookmarks).
void registerWebUi(AsyncWebServer &server, WifiLink &network);

}  // namespace sb
