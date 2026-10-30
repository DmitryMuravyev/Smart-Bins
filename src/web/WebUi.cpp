#include "WebUi.h"

#include <ESPAsyncWebServer.h>

#include "WifiLink.h"
#include "generated/WebAssets.h"

namespace sb {

namespace {

// Sends one bundled file, or 304 Not Modified when the browser already has it.
//
// Caching: "no-cache" means "keep a copy but ask before using it". The browser
// then sends the ETag it got (a hash of the file's content) in If-None-Match,
// and an unchanged file costs a 304 with no body. After a firmware update the
// hash differs, so the new UI is picked up at once without a forced reload.
void sendAsset(AsyncWebServerRequest *req, const web::WebAsset &a) {
  const AsyncWebHeader *match = req->getHeader("If-None-Match");
  if (match && match->value() == a.etag) {
    AsyncWebServerResponse *res = req->beginResponse(304);
    res->addHeader("ETag", a.etag);
    req->send(res);
    return;
  }
  AsyncWebServerResponse *res = req->beginResponse(200, a.mime, a.gz, a.size);
  res->addHeader("Content-Encoding", "gzip");
  res->addHeader("Cache-Control", "no-cache");
  res->addHeader("ETag", a.etag);
  req->send(res);
}

const web::WebAsset *findAsset(const char *path) {
  for (size_t i = 0; i < web::kWebAssetCount; i++) {
    if (strcmp(web::kWebAssets[i].path, path) == 0) return &web::kWebAssets[i];
  }
  return nullptr;
}

}  // namespace

void registerWebUi(AsyncWebServer &server, WifiLink &network) {
  // Exact routes: the default matcher would also send /app.js for /app.js/x.
  // The table lives in flash for the life of the program, so the lambdas can
  // keep a pointer to their entry.
  for (size_t i = 0; i < web::kWebAssetCount; i++) {
    const web::WebAsset *a = &web::kWebAssets[i];
    server.on(AsyncURIMatcher::exact(a->path), HTTP_GET, [a](AsyncWebServerRequest *req) { sendAsset(req, *a); });
  }

  const web::WebAsset *index = findAsset("/index.html");
  server.on(AsyncURIMatcher::exact("/"), HTTP_GET, [index, &network](AsyncWebServerRequest *req) {
    // A board that isn't on a network yet is only useful for joining one, so
    // the setup portal opens straight on the Wi-Fi page.
    if (network.state() != WifiLink::State::Connected && network.portalActive()) {
      req->redirect("/wifi");
      return;
    }
    if (index) sendAsset(req, *index);
    else req->send(500, "text/plain", "web UI missing from the build");
  });

  // The plain config editor used to live here; it is now under Settings.
  server.on(AsyncURIMatcher::exact("/config"), HTTP_GET,
            [](AsyncWebServerRequest *req) { req->redirect("/#settings"); });
}

}  // namespace sb
