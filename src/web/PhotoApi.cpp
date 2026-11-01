#include "PhotoApi.h"

#include <ESPAsyncWebServer.h>

#include <cstring>
#include <memory>
#include <string>

#include "ApiUtil.h"
#include "InventoryApi.h"
#include "InventoryDb.h"
#include "PhotoStore.h"
#include "Security.h"
#include "SendGate.h"

namespace sb {

using inventory::InventoryDb;
using inventory::Part;
using inventory::Result;
using Kind = PhotoStore::Kind;

namespace {

const char *TAG = "photos";

// What an upload request carries between its body chunks and its end. Kept
// in the request's _tempObject, which the server releases with free() when
// the request ends, so it must be plain data (no std::string).
struct UploadState {
  bool started;     // the store accepted the upload
  int64_t partId;
  char name[24];    // the photo's name (new for a photo, given for a thumbnail)
  char error[96];   // why the upload was refused; empty when it went fine
  int status;       // HTTP status for the error
};

UploadState *stateOf(AsyncWebServerRequest *req) { return static_cast<UploadState *>(req->_tempObject); }

void fail(UploadState *st, int status, const std::string &message) {
  if (st->error[0]) return;  // keep the first reason
  st->status = status;
  strncpy(st->error, message.c_str(), sizeof(st->error) - 1);
}

bool readPartId(AsyncWebServerRequest *req, int64_t &id) {
  long v;
  if (!intParam(req, "id", 1, 2147483647L, v)) return false;
  id = v;
  return true;
}

void sendPartJson(AsyncWebServerRequest *req, const Part &p) {
  JsonDocument doc;
  writePart(doc.to<JsonObject>(), p);
  sendJson(req, 200, doc);
}

// Streams a file without keeping it open between chunks: the FAT partition
// has only a few file slots (see main.cpp), and a slow phone could otherwise
// hold one for seconds. Each chunk opens the file, seeks and reads.
void sendFile(AsyncWebServerRequest *req, fs::FS &fs, const String &path, size_t size) {
  struct Source {
    fs::FS *fs;
    String path;
  };
  auto src = std::make_shared<Source>(Source{&fs, path});
  // Through the send gate (see SendGate.h): a screen of part cards asks for
  // many thumbnails at once.
  sendGate().run(req, [src, size](AsyncWebServerRequest *req, SendGate::Slot slot) {
    AsyncWebServerResponse *res =
        req->beginResponse("image/jpeg", size, [src, slot](uint8_t *buf, size_t maxLen, size_t index) -> size_t {
          File f = src->fs->open(src->path, FILE_READ);
          if (!f || !f.seek(index)) return 0;
          return f.read(buf, maxLen);
        });
    // The name changes whenever the photo does, so this URL never shows anything else.
    res->addHeader("Cache-Control", "public, max-age=31536000, immutable");
    req->send(res);
  });
}

// Body handler shared by the photo and thumbnail uploads. Runs for every
// chunk as it arrives; the first chunk checks everything and starts the file.
void onUploadBody(AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total, Kind kind,
                  PhotoStore &photos, InventoryDb &db, Security &security) {
  if (index == 0) {
    auto *st = static_cast<UploadState *>(calloc(1, sizeof(UploadState)));
    if (!st) return;  // the request handler answers "out of memory"
    req->_tempObject = st;
    // The password middleware only runs once the whole body is in, so check
    // here before anything is written.
    if (!security.allows(req)) return fail(st, 401, "password required");
    if (!db.isOpen()) return fail(st, 503, "inventory database unavailable");
    if (!readPartId(req, st->partId)) return fail(st, 400, "id must be a part id");
    Part p;
    std::string err;
    const Result r = db.getPart(st->partId, p, err);
    if (r != Result::Ok) return fail(st, r == Result::NotFound ? 404 : 500, err);
    std::string name;
    if (kind == Kind::Photo) {
      name = PhotoStore::newName(st->partId);
    } else {
      // A thumbnail belongs to the photo the part has now; a late one for a
      // photo that was replaced meanwhile is refused.
      const AsyncWebParameter *n = param(req, "name");
      name = n ? std::string(n->value().c_str()) : std::string();
      if (name.empty() || name != p.photo) return fail(st, 409, "the part has a different photo now");
    }
    strncpy(st->name, name.c_str(), sizeof(st->name) - 1);
    if (!photos.beginUpload(req, name, kind, total, err)) {
      // 507 "Insufficient Storage" when space ran out, so the page can say so.
      const bool full = photos.availableBytes() < total && total <= PhotoStore::kMaxPhotoBytes;
      return fail(st, full ? 507 : 400, err);
    }
    st->started = true;
    // A dropped connection never reaches the request handler: free the slot.
    req->onDisconnect([&photos, req]() { photos.abortUpload(req); });
  }
  UploadState *st = stateOf(req);
  if (!st || !st->started || st->error[0]) return;
  std::string err;
  if (!photos.writeUpload(req, data, len, err)) {
    photos.abortUpload(req);
    st->started = false;
    fail(st, 400, err);
  }
}

// Request handler shared by both uploads: runs once the body is complete.
void onUploadDone(AsyncWebServerRequest *req, Kind kind, PhotoStore &photos, InventoryDb &db) {
  UploadState *st = stateOf(req);
  if (!st) {
    // No body at all, or no memory for the upload's state on its first chunk.
    if (!req->contentLength()) return sendError(req, 400, "no photo in the request");
    return sendError(req, 503, "out of memory, try again");
  }
  if (st->error[0]) {
    photos.abortUpload(req);
    return sendError(req, st->status, st->error);
  }
  std::string err;
  if (!photos.finishUpload(req, err)) return sendError(req, 500, err.c_str());
  if (kind == Kind::Thumb) {
    JsonDocument doc;
    doc["ok"] = true;
    return sendJson(req, 200, doc);
  }
  // The new photo is on storage: point the part at it, then remove the old one.
  std::string previous;
  Part p;
  const Result r = db.setPhoto(st->partId, st->name, previous, p, err);
  if (r != Result::Ok) {
    photos.remove(st->name);  // nothing refers to it
    return sendError(req, r == Result::NotFound ? 404 : 500, err.c_str());
  }
  if (!previous.empty()) photos.remove(previous);
  ESP_LOGI(TAG, "Part %lld: photo %s", (long long)st->partId, st->name);
  sendPartJson(req, p);
}

}  // namespace

void writePhotoStats(JsonObject out, PhotoStore &photos) {
  out["storage"] = photos.onCard() ? "card" : "flash";
  out["used"] = photos.usedBytes();
  out["total"] = photos.totalBytes();
  out["available"] = photos.availableBytes();
}

void registerPhotoApi(AsyncWebServer &server, PhotoStore &photos, InventoryDb &db, Security &security) {
  server.on(AsyncURIMatcher::exact("/api/photos"), HTTP_GET, [&photos](AsyncWebServerRequest *req) {
    JsonDocument doc;
    writePhotoStats(doc.to<JsonObject>(), photos);
    doc["maxSide"] = photos.maxSide();
    doc["thumbSide"] = PhotoStore::kThumbSide;
    doc["maxBytes"] = PhotoStore::kMaxPhotoBytes;
    doc["maxThumbBytes"] = PhotoStore::kMaxThumbBytes;
    sendJson(req, 200, doc);
  });

  server.on(
      AsyncURIMatcher::exact("/api/part/photo"), HTTP_POST,
      [&photos, &db](AsyncWebServerRequest *req) { onUploadDone(req, Kind::Photo, photos, db); }, nullptr,
      [&photos, &db, &security](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
        onUploadBody(req, data, len, index, total, Kind::Photo, photos, db, security);
      });

  server.on(
      AsyncURIMatcher::exact("/api/part/thumb"), HTTP_POST,
      [&photos, &db](AsyncWebServerRequest *req) { onUploadDone(req, Kind::Thumb, photos, db); }, nullptr,
      [&photos, &db, &security](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
        onUploadBody(req, data, len, index, total, Kind::Thumb, photos, db, security);
      });

  server.on(AsyncURIMatcher::exact("/api/part/photo/delete"), HTTP_GET | HTTP_POST,
            [&photos, &db](AsyncWebServerRequest *req) {
              if (!db.isOpen()) return sendError(req, 503, "inventory database unavailable");
              int64_t id;
              if (!readPartId(req, id)) return sendError(req, 400, "id must be a part id");
              std::string previous, err;
              Part p;
              const Result r = db.setPhoto(id, "", previous, p, err);
              if (r != Result::Ok) return sendError(req, r == Result::NotFound ? 404 : 500, err.c_str());
              if (!previous.empty()) photos.remove(previous);
              sendPartJson(req, p);
            });

  // /photos/NAME.jpg and /photos/NAME-t.jpg. The name is checked by the store
  // (digits, a dash, hex), so no other file can be asked for.
  server.on(AsyncURIMatcher::prefix("/photos/"), HTTP_GET, [&photos](AsyncWebServerRequest *req) {
    String file = req->url().substring(strlen("/photos/"));
    Kind kind = Kind::Photo;
    if (file.endsWith("-t.jpg")) {
      kind = Kind::Thumb;
      file.remove(file.length() - strlen("-t.jpg"));
    } else if (file.endsWith(".jpg")) {
      file.remove(file.length() - strlen(".jpg"));
    } else {
      return sendError(req, 404, "no such photo");
    }
    fs::FS *fs = nullptr;
    String path;
    size_t size = 0;
    if (!photos.find(std::string(file.c_str()), kind, fs, path, size)) return sendError(req, 404, "no such photo");
    sendFile(req, *fs, path, size);
  });
}

}  // namespace sb
