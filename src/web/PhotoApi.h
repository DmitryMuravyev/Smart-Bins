#pragma once

// Part photos (see PhotoStore.h for where and how they are stored).
//
//   GET  /api/photos                       where photos go and how large: {storage: "card"|"flash",
//                                          maxSide, thumbSide, maxBytes, maxThumbBytes, used, total, available}
//   POST /api/part/photo?id=N              body: the photo as image/jpeg, already resized by the
//                                          page; answers the part with its new photo name
//   POST /api/part/thumb?id=N&name=NAME    body: its thumbnail (image/jpeg), sent after the photo
//   POST /api/part/photo/delete?id=N       removes the part's photo; answers the part
//   GET  /photos/NAME.jpg, /photos/NAME-t.jpg   the photo and its thumbnail
//
// Photo URLs change with every new photo, so they are sent with a long cache
// lifetime and browsers fetch each one only once.

#include <ArduinoJson.h>

class AsyncWebServer;

namespace sb {

class PhotoStore;
class Security;
namespace inventory {
class InventoryDb;
}

void registerPhotoApi(AsyncWebServer &server, PhotoStore &photos, inventory::InventoryDb &db, Security &security);

// Where photos are stored, for the System page: {storage, used, total, available}.
void writePhotoStats(JsonObject out, PhotoStore &photos);

}  // namespace sb
