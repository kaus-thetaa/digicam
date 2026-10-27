#include "digicam/network_manager.h"
#include "digicam/config.h"
#include <SD_MMC.h>
#include <Update.h>
#include <esp_system.h>

namespace dc {
namespace {
String escapeHtml(String s) {
  s.replace("&", "&amp;"); s.replace("<", "&lt;"); s.replace(">", "&gt;");
  s.replace("\"", "&quot;"); s.replace("'", "&#39;");
  return s;
}
const char* modeSsid(NetworkMode mode) {
  if (mode == NetworkMode::FirmwareUpdate) return cfg::UPDATE_SSID;
  if (mode == NetworkMode::TimeSetup) return cfg::TIME_SSID;
  return cfg::TRANSFER_SSID;
}
}  // namespace

bool NetworkManager::authenticated() {
  if (server_.authenticate(cfg::WEB_USER, cfg::WEB_PASSWORD)) return true;
  server_.requestAuthentication(BASIC_AUTH, "DigiCam V12", "Enter camera access credentials");
  return false;
}

void NetworkManager::registerRoutes() {
  if (routesRegistered_) return;
  server_.on("/", HTTP_GET, [this]() { if (!authenticated()) return; sendHome(); });
  server_.on("/file", HTTP_GET, [this]() { if (!authenticated()) return; sendFile(); });
  server_.on("/time-sync", HTTP_POST, [this]() { if (!authenticated()) return; handleTimeSync(); });
  server_.on("/update", HTTP_POST, [this]() {
    if (!authenticated()) return;
    if (mode_ != NetworkMode::FirmwareUpdate || uploadFailed_ || Update.hasError() || !updateComplete_) {
      String e = uploadError_.length() ? uploadError_ : String("Update failed. Existing firmware remains selected.");
      server_.send(500, "text/plain; charset=utf-8", e);
      return;
    }
    server_.send(200, "text/html; charset=utf-8",
      "<!doctype html><meta name='viewport' content='width=device-width'><title>DigiCam</title>"
      "<body style='font:16px Helvetica,Arial,sans-serif;background:#111;color:#eee;padding:24px'>"
      "<h2>Firmware image accepted</h2><p>DigiCam is restarting. Reconnect to Wi-Fi after it boots.</p></body>");
    delay(700);
    esp_restart();
  }, [this]() { if (!authenticated()) return; updateUpload(); });
  server_.onNotFound([this]() { if (!authenticated()) return; server_.send(404, "text/plain", "Not found"); });
  routesRegistered_ = true;
}

bool NetworkManager::begin(NetworkMode mode, bool sdAvailable) {
  if (mode == NetworkMode::Off) return false;
  end();
  mode_ = mode;
  sdAvailable_ = sdAvailable;
  uploadStarted_ = uploadFailed_ = updateComplete_ = false;
  timeSyncPending_ = false;
  uploadError_ = "";
  uploadBytesWritten_ = 0;
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(true);
  if (!WiFi.softAP(modeSsid(mode), cfg::AP_PASSWORD)) {
    WiFi.mode(WIFI_OFF);
    mode_ = NetworkMode::Off;
    return false;
  }
  registerRoutes();
  server_.begin();
  serverStarted_ = true;
  return true;
}

void NetworkManager::loop() {
  if (serverStarted_) server_.handleClient();
}

void NetworkManager::end() {
  if (serverStarted_) { server_.stop(); serverStarted_ = false; }
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  WiFi.disconnect(true, true);
  mode_ = NetworkMode::Off;
}

bool NetworkManager::consumeTimeSync(time_t& epochSeconds, int16_t& utcOffsetMinutes) {
  if (!timeSyncPending_) return false;
  epochSeconds = timeSyncEpoch_;
  utcOffsetMinutes = timeSyncUtcOffsetMinutes_;
  timeSyncPending_ = false;
  return true;
}

void NetworkManager::handleTimeSync() {
  if (mode_ != NetworkMode::TimeSetup) {
    server_.send(403, "text/plain; charset=utf-8", "Time sync is not active in this mode.");
    return;
  }
  const long long epoch = strtoll(server_.arg("epoch").c_str(), nullptr, 10);
  const long browserOffset = strtol(server_.arg("offset").c_str(), nullptr, 10); // JS Date.getTimezoneOffset(): UTC - local
  if (epoch < 1760000000LL || epoch > 4102444799LL || browserOffset < -840 || browserOffset > 840) {
    server_.send(400, "text/plain; charset=utf-8", "Invalid time. Check the phone/laptop clock and try again.");
    return;
  }
  timeSyncEpoch_ = static_cast<time_t>(epoch);
  timeSyncUtcOffsetMinutes_ = static_cast<int16_t>(-browserOffset); // local - UTC
  timeSyncPending_ = true;
  server_.send(200, "application/json; charset=utf-8", "{\"ok\":true,\"message\":\"Time sent to DigiCam\"}");
}

void NetworkManager::sendHome() {
  String html;
  html.reserve(4300);
  html += F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>DigiCam V12</title><style>");
  html += F("*{box-sizing:border-box}body{font:16px Helvetica,Arial,sans-serif;max-width:760px;margin:0 auto;padding:24px 18px;background:#111;color:#eee;line-height:1.5}");
  html += F("header{border-bottom:1px solid #444;padding-bottom:18px;margin-bottom:22px}small,.muted{color:#aaa}h1{font-size:22px;letter-spacing:.02em;margin:0}h2{font-size:18px;font-weight:600}a{color:#c9d7ff;text-decoration:none}a:hover{text-decoration:underline}");
  html += F(".panel{border:1px solid #444;border-radius:12px;padding:16px;margin:14px 0;background:#181818}.button{display:inline-block;background:#eee;color:#111;border:0;border-radius:7px;padding:11px 15px;font:inherit;cursor:pointer}.button:disabled{opacity:.5}input{font:inherit;max-width:100%}li{padding:9px 0;border-bottom:1px solid #333;overflow-wrap:anywhere}.tag{font-size:12px;letter-spacing:.08em;text-transform:uppercase;color:#aaa}");
  html += F("progress{width:100%;height:8px}code{overflow-wrap:anywhere;color:#ddd}</style></head><body>");
  html += F("<header><div class='tag'>Offline-first camera</div><h1>DigiCam <span class='muted'>/ V12</span></h1><small>Local access point · 192.168.4.1</small></header>");

  if (mode_ == NetworkMode::FirmwareUpdate) {
    html += F("<div class='panel'><div class='tag'>Maintenance</div><h2>Firmware update</h2><p class='muted'>The image sensor and flash are off during this operation. Upload a firmware binary built for this exact DigiCam V12 partition layout.</p>");
    html += F("<form id='upload' method='POST' action='/update' enctype='multipart/form-data'><input required type='file' name='firmware' accept='.bin,application/octet-stream'><p><button class='button' type='submit'>Install firmware</button></p></form><progress id='progress' value='0' max='100' hidden></progress><p id='state' class='muted'>Do not disconnect power during installation.</p></div>");
    html += F("<p class='muted'>Build output: <code>.pio/build/esp32cam_v12/firmware.bin</code></p>");
    html += F("<script>document.getElementById('upload').addEventListener('submit',function(){let p=document.getElementById('progress');p.hidden=false;document.getElementById('state').textContent='Upload started. Keep the camera powered.';});</script>");
  } else if (mode_ == NetworkMode::TimeSetup) {
    html += F("<div class='panel'><div class='tag'>Clock setup</div><h2>Set date &amp; time</h2><p>Use this device's current local clock and timezone. DigiCam does not contact an internet time server.</p>");
    html += F("<p id='now' class='muted'>Reading this device's clock…</p><button class='button' id='sync'>Sync time to camera</button><p id='state' class='muted'>After syncing, exit this mode on the camera to save power.</p></div>");
    html += F("<script>const n=document.getElementById('now'),s=document.getElementById('state'),b=document.getElementById('sync');function refresh(){n.textContent=new Date().toLocaleString(undefined,{dateStyle:'full',timeStyle:'medium'})+' · '+Intl.DateTimeFormat().resolvedOptions().timeZone;}refresh();b.onclick=async()=>{b.disabled=true;s.textContent='Sending time…';try{let d=new Date(),e=Math.floor(d.getTime()/1000),o=d.getTimezoneOffset();let r=await fetch('/time-sync?epoch='+e+'&offset='+o,{method:'POST',credentials:'same-origin'});let j=await r.json();if(!r.ok||!j.ok)throw new Error(j.message||'Sync failed');s.textContent='Time synchronized. Return to DigiCam and hold the button to exit.';b.textContent='Time synchronized';}catch(err){s.textContent='Could not sync: '+err.message;b.disabled=false;}};</script>");
  } else {
    html += F("<div class='panel'><div class='tag'>Storage</div><h2>Photo transfer</h2><p class='muted'>Read-only access. Camera, flash and image processing are shut down.</p><ul>");
    if (!sdAvailable_) html += F("<li>SD card unavailable. Insert or check the card after leaving this mode.</li>");
    else {
      File dir = SD_MMC.open("/DCIM");
      if (dir && dir.isDirectory()) {
        File f = dir.openNextFile();
        uint16_t count = 0;
        while (f && count < cfg::MAX_PHOTOS_PER_LIST) {
          if (!f.isDirectory()) {
            String name = String(f.name());
            int slash = name.lastIndexOf('/');
            if (slash >= 0) name = name.substring(slash + 1);
            if (name.startsWith("IMG_") && name.endsWith(".jpg") && !name.endsWith(".tmp")) {
              html += F("<li><a href='/file?n="); html += name; html += F("'>");
              html += escapeHtml(name); html += F("</a><br><small>");
              html += String(static_cast<unsigned long>(f.size())); html += F(" bytes</small></li>");
              ++count;
            }
          }
          f.close();
          f = dir.openNextFile();
        }
        if (f) html += F("<li>List capped at 150 photos. Use an SD reader for older files.</li>");
        if (f) f.close();
        dir.close();
        if (count == 0) html += F("<li>No JPEG photos found.</li>");
      } else html += F("<li>DCIM folder unavailable.</li>");
    }
    html += F("</ul></div><p class='muted'>This mode only downloads existing photos. It never captures new images.</p>");
  }
  html += F("<hr style='border:0;border-top:1px solid #333;margin-top:28px'><small>Keep power connected while updating. To exit, use the camera button.</small></body></html>");
  server_.send(200, "text/html; charset=utf-8", html);
}

void NetworkManager::sendFile() {
  if (mode_ != NetworkMode::PhotoTransfer || !sdAvailable_) {
    server_.send(403, "text/plain", "Photo download is not active"); return;
  }
  String n = server_.arg("n");
  if (!n.startsWith("IMG_") || !n.endsWith(".jpg") || n.indexOf("..") >= 0 || n.indexOf('/') >= 0 || n.indexOf('\\') >= 0) {
    server_.send(400, "text/plain", "Invalid filename"); return;
  }
  for (size_t i = 0; i < n.length(); ++i) {
    char c = n[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) {
      server_.send(400, "text/plain", "Invalid filename"); return;
    }
  }
  File f = SD_MMC.open(String("/DCIM/") + n, FILE_READ);
  if (!f) { server_.send(404, "text/plain", "Photo not found"); return; }
  server_.sendHeader("Content-Disposition", String("attachment; filename=\"") + n + "\"");
  server_.streamFile(f, "image/jpeg");
  f.close();
}

void NetworkManager::updateUpload() {
  HTTPUpload& up = server_.upload();
  if (up.status == UPLOAD_FILE_START) {
    uploadStarted_ = true; uploadFailed_ = false; updateComplete_ = false;
    uploadError_ = ""; uploadBytesWritten_ = 0;
    if (mode_ != NetworkMode::FirmwareUpdate) { uploadFailed_ = true; uploadError_ = "Firmware upload is unavailable in photo-transfer mode"; return; }
    if (Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) return;
    uploadFailed_ = true; uploadError_ = String("Update.begin failed: ") + Update.errorString();
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!uploadStarted_ || uploadFailed_) return;
    const size_t wrote = Update.write(up.buf, up.currentSize);
    if (wrote != up.currentSize) {
      uploadFailed_ = true; uploadError_ = String("Flash write failed: ") + Update.errorString(); Update.abort(); return;
    }
    uploadBytesWritten_ += wrote;
    if (up.totalSize && progressCallback_) {
      int percent = static_cast<int>((100ULL * uploadBytesWritten_) / up.totalSize);
      if (percent > 100) percent = 100;
      progressCallback_(percent);
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (!uploadStarted_ || uploadFailed_) return;
    if (Update.end() && Update.isFinished()) {
      updateComplete_ = true;
      if (progressCallback_) progressCallback_(100);
    } else {
      uploadFailed_ = true; uploadError_ = String("Firmware verification failed: ") + Update.errorString(); Update.abort();
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    uploadFailed_ = true; updateComplete_ = false; uploadError_ = "Upload cancelled by client";
    if (uploadStarted_) Update.abort();
  }
}
}  // namespace dc
