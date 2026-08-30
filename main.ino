/*
  ESP32-WROVER-E js-dos Server - Phase 2

  Target hardware:
    Freenove ESP32-WROVER board with microSD slot on back.

  SD interface:
    SD_MMC 1-bit
      CMD = GPIO15
      CLK = GPIO14
      D0  = GPIO2

  Features:
    - Wi-Fi SoftAP
    - HTTP server
    - SD-backed js-dos website
    - .jsdos bundle upload
    - ZIP/js-dos bundle detection
    - Automatic game discovery
    - js-dos filesystem-change save storage
    - Saves stored in /dos/artifacts/js/roms/saves/
    - Boot-only rotating logs
    - Delete bundle
    - Delete save

  Save system:
    js-dos fsChanges sends a .changes bundle to:
      POST /api/save?key=GAME.jsdos.changes

    js-dos loads it from:
      GET /api/save?key=GAME.jsdos.changes

    The original .jsdos game bundle is never modified by saves.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <FS.h>
#include <SD_MMC.h>

#define SD_MMC_CMD 15
#define SD_MMC_CLK 14
#define SD_MMC_D0  2

const char* AP_SSID = "ESP32-jsDOS";
const char* AP_PASSWORD = "jsdos1234";

WebServer server(80);

static const char* WEB_ROOT = "/dos/artifacts/js";
static const char* ROM_ROOT = "/dos/artifacts/js/roms";
static const char* SAVE_ROOT = "/dos/artifacts/js/roms/saves";

static const uint32_t MAX_BUNDLE_BYTES = 256UL * 1024UL * 1024UL;
static const uint32_t MAX_SAVE_BYTES = 32UL * 1024UL * 1024UL;

File uploadFile;

bool uploadAccepted = false;
bool uploadTooLarge = false;

uint32_t uploadBytesReceived = 0;

String uploadPath;
String uploadName;
String uploadError;

bool bootLogging = false;
String bootLogPath;
uint32_t bootNumber = 0;


/* =========================================================
   CONTENT TYPE
   ========================================================= */

String contentType(const String& path) {

  if (path.endsWith(".html") || path.endsWith(".htm"))
    return "text/html";

  if (path.endsWith(".css"))
    return "text/css";

  if (path.endsWith(".js"))
    return "application/javascript";

  if (path.endsWith(".json"))
    return "application/json";

  if (path.endsWith(".png"))
    return "image/png";

  if (path.endsWith(".jpg") || path.endsWith(".jpeg"))
    return "image/jpeg";

  if (path.endsWith(".svg"))
    return "image/svg+xml";

  if (path.endsWith(".ico"))
    return "image/x-icon";

  if (path.endsWith(".wasm"))
    return "application/wasm";

  if (path.endsWith(".jsdos"))
    return "application/octet-stream";

  if (path.endsWith(".changes"))
    return "application/octet-stream";

  return "application/octet-stream";
}


/* =========================================================
   SAFE FILENAMES
   ========================================================= */

bool safeFilename(const String& name) {

  if (name.length() == 0 || name.length() > 96)
    return false;

  if (name.indexOf("..") >= 0)
    return false;

  if (name.indexOf('/') >= 0)
    return false;

  if (name.indexOf('\\') >= 0)
    return false;

  if (name.indexOf(':') >= 0)
    return false;

  for (size_t i = 0; i < name.length(); i++) {

    char c = name[i];

    if (!(isalnum((unsigned char)c) ||
          c == '.' ||
          c == '_' ||
          c == '-' ||
          c == ' ')) {

      return false;
    }
  }

  return true;
}


bool safeSaveKey(String key) {

  if (key.length() == 0 || key.length() > 128)
    return false;

  if (key.indexOf("..") >= 0)
    return false;

  if (key.indexOf('/') >= 0)
    return false;

  if (key.indexOf('\\') >= 0)
    return false;

  if (key.indexOf(':') >= 0)
    return false;

  for (size_t i = 0; i < key.length(); i++) {

    char c = key[i];

    if (!(isalnum((unsigned char)c) ||
          c == '.' ||
          c == '_' ||
          c == '-')) {

      return false;
    }
  }

  return true;
}


/* =========================================================
   BOOT LOGGING
   ========================================================= */

void writeBootLog(const String& line) {

  if (!bootLogging)
    return;

  File f = SD_MMC.open(
    bootLogPath,
    FILE_APPEND
  );

  if (!f)
    return;

  f.println(line);
  f.close();
}


int countBootLogs() {

  int count = 0;

  File dir = SD_MMC.open("/dos/logs");

  if (!dir || !dir.isDirectory()) {

    if (dir)
      dir.close();

    return 0;
  }

  File entry = dir.openNextFile();

  while (entry) {

    if (!entry.isDirectory()) {

      String name = entry.name();

      if (name.startsWith("/dos/logs/boot-") &&
          name.endsWith(".log")) {

        count++;
      }
    }

    entry.close();

    entry = dir.openNextFile();
  }

  dir.close();

  return count;
}


uint32_t findHighestBootNumber() {

  uint32_t highest = 0;

  File dir = SD_MMC.open("/dos/logs");

  if (!dir || !dir.isDirectory()) {

    if (dir)
      dir.close();

    return 0;
  }

  File entry = dir.openNextFile();

  while (entry) {

    if (!entry.isDirectory()) {

      String name = entry.name();

      if (name.startsWith("/dos/logs/boot-") &&
          name.endsWith(".log")) {

        String n =
          name.substring(
            15,
            name.length() - 4
          );

        bool numeric = n.length() > 0;

        for (size_t i = 0; i < n.length(); i++) {

          if (!isDigit(n[i]))
            numeric = false;
        }

        if (numeric) {

          uint32_t v =
            (uint32_t)n.toInt();

          if (v > highest)
            highest = v;
        }
      }
    }

    entry.close();

    entry = dir.openNextFile();
  }

  dir.close();

  return highest;
}


void deleteAllBootLogs() {

  File dir = SD_MMC.open("/dos/logs");

  if (!dir || !dir.isDirectory()) {

    if (dir)
      dir.close();

    return;
  }

  String names[32];

  int count = 0;

  File entry = dir.openNextFile();

  while (entry && count < 32) {

    if (!entry.isDirectory()) {

      String name = entry.name();

      if (name.startsWith("/dos/logs/boot-") &&
          name.endsWith(".log")) {

        names[count++] = name;
      }
    }

    entry.close();

    entry = dir.openNextFile();
  }

  dir.close();

  for (int i = 0; i < count; i++)
    SD_MMC.remove(names[i]);
}


void prepareBootLog() {

  if (!SD_MMC.exists("/dos/logs"))
    SD_MMC.mkdir("/dos/logs");

  /*
    User requirement:

    If there are MORE THAN 11 boot logs,
    delete ALL of them before making
    the log for the current boot.
  */

  if (countBootLogs() > 11)
    deleteAllBootLogs();

  bootNumber =
    findHighestBootNumber() + 1;

  if (bootNumber == 0) {

    deleteAllBootLogs();

    bootNumber = 1;
  }

  char path[64];

  snprintf(
    path,
    sizeof(path),
    "/dos/logs/boot-%04lu.log",
    (unsigned long)bootNumber
  );

  bootLogPath = String(path);

  bootLogging = true;

  File f =
    SD_MMC.open(
      bootLogPath,
      FILE_WRITE
    );

  if (f) {

    f.println("ESP32 js-dos Boot Log");
    f.println("=====================");

    f.close();
  }
}


/* =========================================================
   DIRECTORIES
   ========================================================= */

void ensureDirectories() {

  if (!SD_MMC.exists("/dos"))
    SD_MMC.mkdir("/dos");

  if (!SD_MMC.exists("/dos/logs"))
    SD_MMC.mkdir("/dos/logs");

  if (!SD_MMC.exists("/dos/artifacts"))
    SD_MMC.mkdir("/dos/artifacts");

  if (!SD_MMC.exists(WEB_ROOT))
    SD_MMC.mkdir(WEB_ROOT);

  if (!SD_MMC.exists(
        String(WEB_ROOT) + "/css"))
    SD_MMC.mkdir(
      String(WEB_ROOT) + "/css"
    );

  if (!SD_MMC.exists(
        String(WEB_ROOT) + "/js-dos"))
    SD_MMC.mkdir(
      String(WEB_ROOT) + "/js-dos"
    );

  if (!SD_MMC.exists(ROM_ROOT))
    SD_MMC.mkdir(ROM_ROOT);

  if (!SD_MMC.exists(SAVE_ROOT))
    SD_MMC.mkdir(SAVE_ROOT);
}


/* =========================================================
   SEND FILE
   ========================================================= */

void sendFile(const String& path) {

  File f =
    SD_MMC.open(
      path,
      FILE_READ
    );

  if (!f || f.isDirectory()) {

    if (f)
      f.close();

    server.send(
      404,
      "text/plain",
      "File not found"
    );

    return;
  }

  server.streamFile(
    f,
    contentType(path)
  );

  f.close();
}


/* =========================================================
   STATIC WEBSITE
   ========================================================= */

void handleStatic() {

  String uri = server.uri();

  if (uri == "/") {

    sendFile(
      String(WEB_ROOT) +
      "/index.html"
    );

    return;
  }

  if (!uri.startsWith(WEB_ROOT) ||
      uri.indexOf("..") >= 0) {

    server.send(
      403,
      "text/plain",
      "Forbidden"
    );

    return;
  }

  sendFile(uri);
}


/* =========================================================
   ROM LIST
   ========================================================= */

void handleRoms() {

  File dir =
    SD_MMC.open(ROM_ROOT);

  if (!dir || !dir.isDirectory()) {

    if (dir)
      dir.close();

    server.send(
      200,
      "application/json",
      "[]"
    );

    return;
  }

  String json = "[";

  bool first = true;

  File entry =
    dir.openNextFile();

  while (entry) {

    if (!entry.isDirectory()) {

      String name =
        entry.name();

      if (name.endsWith(".jsdos")) {

        if (!first)
          json += ",";

        json += "\"";

        for (size_t i = 0;
             i < name.length();
             i++) {

          char c = name[i];

          if (c == '"' ||
              c == '\\') {

            json += '\\';
          }

          json += c;
        }

        json += "\"";

        first = false;
      }
    }

    entry.close();

    entry = dir.openNextFile();
  }

  dir.close();

  json += "]";

  server.send(
    200,
    "application/json",
    json
  );
}


/* =========================================================
   JS-DOS BUNDLE VALIDATION
   ========================================================= */

bool validateBundle(const String& path) {

  /*
    js-dos bundles are ZIP archives.

    We intentionally do NOT require:

      .jsdos/dosbox.conf

    because different valid js-dos bundles can have
    different internal layouts.

    We only check for a valid ZIP signature.
  */

  File f =
    SD_MMC.open(
      path,
      FILE_READ
    );

  if (!f)
    return false;

  if (f.size() < 4) {

    f.close();

    return false;
  }

  uint8_t sig[4];

  size_t n =
    f.read(
      sig,
      sizeof(sig)
    );

  f.close();

  if (n != 4)
    return false;

  if (sig[0] != 'P' ||
      sig[1] != 'K') {

    return false;
  }

  /*
    ZIP signatures:

    PK 03 04
      local file header

    PK 05 06
      empty archive / end central directory

    PK 07 08
      data descriptor / streaming ZIP
  */

  bool validZip =
    (sig[2] == 0x03 &&
     sig[3] == 0x04) ||

    (sig[2] == 0x05 &&
     sig[3] == 0x06) ||

    (sig[2] == 0x07 &&
     sig[3] == 0x08);

  return validZip;
}


/* =========================================================
   START BUNDLE UPLOAD
   ========================================================= */

void startBundleUpload(
  const String& filename,
  uint32_t maxBytes
) {

  uploadAccepted = false;
  uploadTooLarge = false;
  uploadBytesReceived = 0;

  uploadPath = "";
  uploadName = filename;
  uploadError = "";

  if (!safeFilename(filename) ||
      !filename.endsWith(".jsdos")) {

    uploadError =
      "Only safe .jsdos filenames are accepted.";

    return;
  }

  if (!SD_MMC.exists(ROM_ROOT))
    SD_MMC.mkdir(ROM_ROOT);

  uploadPath =
    String(ROM_ROOT) +
    "/" +
    filename;

  if (SD_MMC.exists(uploadPath))
    SD_MMC.remove(uploadPath);

  uploadFile =
    SD_MMC.open(
      uploadPath,
      FILE_WRITE
    );

  if (!uploadFile) {

    uploadError =
      "Could not create destination file on SD card.";

    return;
  }

  uploadAccepted = true;
}


/* =========================================================
   START SAVE UPLOAD
   ========================================================= */

void startSaveUpload(
  const String& key
) {

  /*
    js-dos fsChanges sends its .changes bundle here.

    Example:

      key = doom.jsdos.changes

    Stored as:

      /dos/artifacts/js/roms/saves/doom.jsdos.changes

    The original .jsdos file is never touched.
  */

  uploadAccepted = false;
  uploadTooLarge = false;
  uploadBytesReceived = 0;

  uploadPath = "";
  uploadName = key;
  uploadError = "";

  if (!safeSaveKey(key)) {

    uploadError =
      "Invalid save key.";

    return;
  }

  if (!SD_MMC.exists(SAVE_ROOT))
    SD_MMC.mkdir(SAVE_ROOT);

  uploadPath =
    String(SAVE_ROOT) +
    "/" +
    key;

  if (SD_MMC.exists(uploadPath))
    SD_MMC.remove(uploadPath);

  uploadFile =
    SD_MMC.open(
      uploadPath,
      FILE_WRITE
    );

  if (!uploadFile) {

    uploadError =
      "Could not create save file on SD card.";

    return;
  }

  uploadAccepted = true;
}


/* =========================================================
   UPLOAD DATA
   ========================================================= */

void handleUploadData() {

  HTTPUpload& upload =
    server.upload();

  String uri =
    server.uri();


  /* -------------------------
     START
     ------------------------- */

  if (upload.status ==
      UPLOAD_FILE_START) {

    String filename =
      upload.filename;

    int slash =
      filename.lastIndexOf('/');

    if (slash >= 0)
      filename =
        filename.substring(
          slash + 1
        );

    slash =
      filename.lastIndexOf('\\');

    if (slash >= 0)
      filename =
        filename.substring(
          slash + 1
        );


    if (uri == "/api/upload") {

      startBundleUpload(
        filename,
        MAX_BUNDLE_BYTES
      );

    }

    else if (uri == "/api/save") {

      String key =
        server.arg("key");

      if (key.length() == 0)
        key = filename;

      startSaveUpload(key);

    }

    else {

      uploadError =
        "Unknown upload endpoint.";

      uploadAccepted = false;
    }
  }


  /* -------------------------
     DATA
     ------------------------- */

  else if (
    upload.status ==
    UPLOAD_FILE_WRITE
  ) {

    if (upload.currentSize > 0) {

      uint32_t limit =
        (uri == "/api/save")
        ? MAX_SAVE_BYTES
        : MAX_BUNDLE_BYTES;


      if (uploadBytesReceived > limit ||
          upload.currentSize >
          (limit - uploadBytesReceived)) {

        uploadTooLarge = true;
        uploadAccepted = false;

        uploadError =
          "Upload exceeds the size limit.";

      }

      else {

        uploadBytesReceived +=
          upload.currentSize;
      }
    }


    if (uploadAccepted &&
        uploadFile) {

      size_t written =
        uploadFile.write(
          upload.buf,
          upload.currentSize
        );

      if (written != upload.currentSize) {

        uploadAccepted = false;

        uploadError =
          "SD card write failed.";
      }
    }
  }


  /* -------------------------
     END
     ------------------------- */

  else if (
    upload.status ==
    UPLOAD_FILE_END
  ) {

    if (uploadFile)
      uploadFile.close();


    if (uploadTooLarge ||
        !uploadAccepted) {

      if (uploadPath.length() &&
          SD_MMC.exists(uploadPath)) {

        SD_MMC.remove(uploadPath);
      }

      return;
    }


    /*
      Only .jsdos uploads are validated.

      Save .changes files are NOT treated
      as js-dos game bundles.
    */

    if (uri == "/api/upload") {

      if (!validateBundle(uploadPath)) {

        SD_MMC.remove(uploadPath);

        uploadAccepted = false;

        uploadError =
          "Not a valid js-dos bundle: ZIP signature not found.";
      }
    }
  }


  /* -------------------------
     ABORT
     ------------------------- */

  else if (
    upload.status ==
    UPLOAD_FILE_ABORTED
  ) {

    if (uploadFile)
      uploadFile.close();

    if (uploadPath.length() &&
        SD_MMC.exists(uploadPath)) {

      SD_MMC.remove(uploadPath);
    }

    uploadAccepted = false;

    uploadError =
      "Upload aborted.";
  }
}


/* =========================================================
   UPLOAD RESULT
   ========================================================= */

void handleUploadResult() {

  if (uploadAccepted) {

    server.send(
      200,
      "application/json",
      "{\"ok\":true}"
    );

  }

  else {

    String msg =
      uploadError.length()
      ? uploadError
      : "Upload rejected.";

    msg.replace(
      "\\",
      "\\\\"
    );

    msg.replace(
      "\"",
      "\\\""
    );

    server.send(
      400,
      "application/json",
      "{\"ok\":false,\"message\":\"" +
      msg +
      "\"}"
    );
  }
}


/* =========================================================
   GET SAVE
   ========================================================= */

void handleGetSave() {

  String key =
    server.arg("key");

  if (!safeSaveKey(key)) {

    server.send(
      400,
      "text/plain",
      "Invalid save key"
    );

    return;
  }

  String path =
    String(SAVE_ROOT) +
    "/" +
    key;

  if (!SD_MMC.exists(path)) {

    server.send(
      404,
      "text/plain",
      "Save not found"
    );

    return;
  }

  sendFile(path);
}


/* =========================================================
   DELETE SAVE
   ========================================================= */

void handleDeleteSave() {

  String key =
    server.arg("key");

  if (!safeSaveKey(key)) {

    server.send(
      400,
      "application/json",
      "{\"ok\":false}"
    );

    return;
  }

  String path =
    String(SAVE_ROOT) +
    "/" +
    key;

  bool ok =
    SD_MMC.exists(path) &&
    SD_MMC.remove(path);

  server.send(
    ok ? 200 : 404,
    "application/json",
    ok
      ? "{\"ok\":true}"
      : "{\"ok\":false}"
  );
}


/* =========================================================
   DELETE BUNDLE
   ========================================================= */

void handleDeleteBundle() {

  String name =
    server.arg("name");

  if (!safeFilename(name) ||
      !name.endsWith(".jsdos")) {

    server.send(
      400,
      "application/json",
      "{\"ok\":false}"
    );

    return;
  }

  String path =
    String(ROM_ROOT) +
    "/" +
    name;

  bool ok =
    SD_MMC.exists(path) &&
    SD_MMC.remove(path);

  server.send(
    ok ? 200 : 404,
    "application/json",
    ok
      ? "{\"ok\":true}"
      : "{\"ok\":false}"
  );
}


/* =========================================================
   SETUP
   ========================================================= */

void setup() {

  Serial.begin(115200);

  delay(500);

  Serial.println();
  Serial.println(
    "ESP32 js-dos server starting..."
  );


  /* -------------------------
     SD CARD
     ------------------------- */

  SD_MMC.setPins(
    SD_MMC_CLK,
    SD_MMC_CMD,
    SD_MMC_D0
  );


  if (!SD_MMC.begin(
        "/sdcard",
        true,
        true,
        SDMMC_FREQ_DEFAULT,
        5
      )) {

    Serial.println(
      "ERROR: SD_MMC mount failed."
    );

    while (true)
      delay(1000);
  }


  ensureDirectories();


  /* -------------------------
     BOOT LOG
     ------------------------- */

  prepareBootLog();

  writeBootLog(
    "Boot: " +
    String(bootNumber)
  );

  writeBootLog(
    "Chip: ESP32-WROVER-E"
  );

  writeBootLog(
    "SD_MMC: mounted"
  );

  writeBootLog(
    "SD_MMC bus: 1-bit"
  );


  /* -------------------------
     AP FIRST
     ------------------------- */

  writeBootLog(
    "AP: starting"
  );

  WiFi.mode(WIFI_AP);

  bool apOK =
    WiFi.softAP(
      AP_SSID,
      AP_PASSWORD
    );


  if (!apOK) {

    writeBootLog(
      "AP: FAILED"
    );

    writeBootLog(
      "System: HALTED"
    );

    bootLogging = false;

    while (true)
      delay(1000);
  }


  IPAddress apIP =
    WiFi.softAPIP();


  writeBootLog(
    "AP: " +
    String(AP_SSID)
  );

  writeBootLog(
    "AP IP: " +
    apIP.toString()
  );


  /* -------------------------
     WEB SERVER
     ------------------------- */

  server.on(
    "/api/roms",
    HTTP_GET,
    handleRoms
  );


  server.on(
    "/api/upload",
    HTTP_POST,
    handleUploadResult,
    handleUploadData
  );


  server.on(
    "/api/save",
    HTTP_POST,
    handleUploadResult,
    handleUploadData
  );


  server.on(
    "/api/save",
    HTTP_GET,
    handleGetSave
  );


  server.on(
    "/api/save",
    HTTP_DELETE,
    handleDeleteSave
  );


  server.on(
    "/api/delete",
    HTTP_POST,
    handleDeleteBundle
  );


  server.onNotFound(
    handleStatic
  );


  server.begin();


  writeBootLog(
    "Web server: started"
  );

  writeBootLog(
    "System: READY"
  );

  writeBootLog(
    "Logging: STOPPED"
  );

  /*
    Boot logging ends here.
    Runtime/save activity is intentionally
    NOT added to the boot log.
  */

  bootLogging = false;


  Serial.println(
    "Wi-Fi AP started"
  );

  Serial.print(
    "SSID: "
  );

  Serial.println(
    AP_SSID
  );

  Serial.print(
    "Password: "
  );

  Serial.println(
    AP_PASSWORD
  );

  Serial.print(
    "Server: http://"
  );

  Serial.println(
    apIP
  );

  Serial.println(
    "HTTP server started."
  );

  Serial.println(
    "Boot logging stopped."
  );
}


/* =========================================================
   LOOP
   ========================================================= */

void loop() {

  server.handleClient();

  delay(2);
}
