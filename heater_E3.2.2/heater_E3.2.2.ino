/* version E3.2.2 */
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <EEPROM.h>
#include <LittleFS.h>
#include <Ticker.h>
#include <DHT.h>
#include <ArduinoJson.h>
#include <ESP8266mDNS.h>

/* ================= CONFIG ================= */
#define LED_PIN D5
#define EEPROM_SIZE 256
#define LOG_SIZE 1024

#define ADDR_FLAG        0
#define ADDR_SSID        4
#define ADDR_PASS        40
#define ADDR_IP4         80
#define ADDR_FW_PENDING  100
#define ADDR_FW_VALID    101
#define ADDR_CRASH_CNT   102
#define ADDR_LAST_ERROR  110

#define MAX_CRASH        3
#define FW_WATCHDOG_MS   20000

#define DHT_PIN D6
#define DHT_TYPE DHT11

#define WIFI_CONNECT_TIMEOUT 15000UL
#define MDNS_RETRY_INTERVAL  5000UL

DHT dht(DHT_PIN, DHT_TYPE);

/* ================= GLOBAL ================= */
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
Ticker hwWdt;

float t = 0;
float h = 0;

unsigned long dhtTimer = 0;
unsigned long fwBootTimer = 0;

bool fwConfirmed = false;
bool fwPending = false;

bool mdnsStarted = false;
unsigned long mdnsRetryTimer = 0;

bool apStarted = false;
bool littleFSReady = false;

/* ================= LED ================= */
enum LedMode {
  LED_AP,
  LED_CONNECTING,
  LED_CONNECTED
};

LedMode ledMode = LED_AP;
unsigned long ledTimer = 0;
bool ledState = false;

/* ================= STATE MACHINE ================= */
enum MyWiFiState {
  MY_WIFI_INIT,
  MY_WIFI_TRY_STA,
  MY_WIFI_STA_CONNECTED,
  MY_WIFI_AP_MODE,
  MY_WIFI_SAFE_MODE
};

MyWiFiState wifiState = MY_WIFI_INIT;
unsigned long wifiTimer = 0;

/* ================= DYNAMIC GPIO & X9C CONFIG ================= */
struct GPIOConfig {
  uint8_t pin;
  String mode;
  String function;
};

#define MAX_GPIO 10
GPIOConfig gpioConfigs[MAX_GPIO];
uint8_t gpioCount = 0;

struct X9CConfig {
  uint8_t min;
  uint8_t max;
  uint8_t current;
};

X9CConfig x9c;

/* ================= LOG INTERNAL ================= */
char logBuffer[LOG_SIZE] = {0};
uint16_t logIndex = 0;

void logPrint(const String &msg) {
  Serial.println(msg);

  for (size_t i = 0; i < msg.length(); i++) {
    logBuffer[logIndex++] = msg[i];
    if (logIndex >= LOG_SIZE) logIndex = 0;
  }

  logBuffer[logIndex++] = '\n';
  if (logIndex >= LOG_SIZE) logIndex = 0;
}

String getLogs() {
  String out;

  for (uint16_t i = 0; i < LOG_SIZE; i++) {
    uint16_t idx = (logIndex + i) % LOG_SIZE;
    char c = logBuffer[idx];
    if (c != 0) out += c;
  }

  return out;
}

/* ================= EEPROM ================= */
uint16_t getStringCapacity(int addr) {
  if (addr == ADDR_SSID) return ADDR_PASS - ADDR_SSID;
  if (addr == ADDR_PASS) return ADDR_IP4 - ADDR_PASS;
  if (addr == ADDR_LAST_ERROR) return EEPROM_SIZE - ADDR_LAST_ERROR;
  return 0;
}

void saveString(int addr, const String &data) {
  uint16_t capacity = getStringCapacity(addr);
  if (capacity < 2) return;

  uint16_t maxLength = capacity - 1;
  uint16_t len = data.length();
  if (len > maxLength) len = maxLength;

  for (uint16_t i = 0; i < len; i++) {
    EEPROM.write(addr + i, data[i]);
  }

  EEPROM.write(addr + len, 0);
  EEPROM.commit();
}

String readString(int addr) {
  uint16_t capacity = getStringCapacity(addr);
  if (capacity < 2) return "";

  char buf[64];
  uint16_t maxLength = capacity - 1;
  if (maxLength > sizeof(buf) - 1) maxLength = sizeof(buf) - 1;

  uint16_t i = 0;
  while (i < maxLength) {
    char c = EEPROM.read(addr + i);
    if (c == 0 || c == 0xFF) break;
    buf[i++] = c;
  }

  buf[i] = 0;
  return String(buf);
}

void logError(const String &e) {
  saveString(ADDR_LAST_ERROR, e);
  logPrint("[ERROR] " + e);
}

/* ================= WATCHDOG ================= */
void hwWatchdogKick() {
  ESP.wdtFeed();
}

/* ================= mDNS MANAGEMENT ================= */
void stopMDNS() {
  if (mdnsStarted) {
    MDNS.close();
    mdnsStarted = false;
    logPrint("mDNS stopped");
  }

  mdnsRetryTimer = 0;
}

void startMDNS() {
  if (mdnsStarted) return;
  if (WiFi.status() != WL_CONNECTED) return;

  if (mdnsRetryTimer != 0 &&
      millis() - mdnsRetryTimer < MDNS_RETRY_INTERVAL) {
    return;
  }

  mdnsRetryTimer = millis();

  if (MDNS.begin("heater")) {
    MDNS.addService("http", "tcp", 80);
    mdnsStarted = true;
    logPrint("mDNS started: heater.local");
  } else {
    logPrint("mDNS start failed - retrying");
  }
}

/* ================= ACCESS POINT ================= */
void startAP() {
  if (apStarted && WiFi.getMode() == WIFI_AP) return;

  stopMDNS();

  WiFi.disconnect();
  WiFi.mode(WIFI_AP);
  delay(100);

  if (WiFi.softAP("Heater-Setting")) {
    apStarted = true;
    logPrint("SoftAP فعال شد: Heater-Setting");
    logPrint("AP IP: " + WiFi.softAPIP().toString());
  } else {
    apStarted = false;
    logPrint("SoftAP start failed");
  }
}

void stopAP() {
  if (!apStarted) return;

  WiFi.softAPdisconnect(true);
  apStarted = false;
  logPrint("SoftAP stopped");
}

/* ================= WIFI STA ================= */
void startSTA() {
  String ssid = readString(ADDR_SSID);
  String pass = readString(ADDR_PASS);

  if (ssid.length() == 0) {
    wifiState = MY_WIFI_AP_MODE;
    return;
  }

  logPrint("در حال تلاش برای اتصال به WiFi...");

  stopMDNS();
  stopAP();

  WiFi.disconnect();
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid.c_str(), pass.c_str());

  wifiTimer = millis();
  wifiState = MY_WIFI_TRY_STA;
  ledMode = LED_CONNECTING;
}

/* ================= OTA CONFIRM ================= */
void handleFwOk() {
  EEPROM.write(ADDR_FW_PENDING, 0);
  EEPROM.write(ADDR_FW_VALID, 1);
  EEPROM.write(ADDR_CRASH_CNT, 0);
  EEPROM.commit();

  fwConfirmed = true;

  server.send(200, "text/html; charset=utf-8", "فریم‌ویر تأیید شد ✅");
  logPrint("Firmware confirmed");
}

/* ================= SAFE MODE ================= */
void enterSafeMode() {
  stopMDNS();

  WiFi.disconnect();
  WiFi.mode(WIFI_AP);
  WiFi.softAP("Heater-SafeMode");

  server.on("/", []() {
    server.send(
      200,
      "text/html; charset=utf-8",
      "<h2>حالت ایمن فعال است</h2><p>فریم‌ویر خراب است.</p>"
    );
  });

  server.begin();
  logPrint("Entering SAFE MODE");

  while (true) {
    server.handleClient();
    delay(100);
  }
}

/* ================= UI ================= */
String commonStyle() {
  if (littleFSReady && LittleFS.exists("/style.css")) {
    File f = LittleFS.open("/style.css", "r");

    if (f) {
      String css = "<style>" + f.readString() + "</style>";
      f.close();
      return css;
    }
  }

  return R"rawliteral(
<style>
body{font-family:tahoma;background:#f2f2f2}
.box{width:360px;max-width:90%;margin:40px auto;padding:25px;
border:3px solid #000;border-radius:12px;background:#fff}
input,button{font-size:22px;padding:10px;width:100%;box-sizing:border-box}
h3{font-size:26px}
.settings{position:fixed;bottom:20px;left:20px;font-size:60px}
</style>
)rawliteral";
}

String pageMain() {
  String page;
  String bg = "";

  if (littleFSReady && LittleFS.exists("/bg.jpg")) {
    bg = "body{background:url(/bg.jpg) no-repeat center/cover fixed;} ";
  }

  page += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  page += commonStyle();

  page += "<style>" + bg + R"rawliteral(
.container{display:flex;justify-content:center;gap:30px;margin-top:80px;flex-wrap:wrap}
.card{width:240px;max-width:75vw;height:200px;border:4px solid #000;border-radius:20px;
background:rgba(255,255,255,.9);text-align:center;font-size:32px;padding:20px;box-sizing:border-box}
.value{font-size:42px}
.version{font-size:14px;text-align:center;margin-top:20px;color:#333}
</style></head><body>
<h2 style='text-align:center'>کنترل هیتر</h2>
)rawliteral";

  page += R"rawliteral(
<div class="container">
<div class="card">🌡️<br>دما<div class="value"><span id="t">--</span>°C</div></div>
<div class="card">💧<br>رطوبت<div class="value"><span id="h">--</span>%</div></div>
</div>
<div class='version'>نسخه برنامه: E3.2.2</div>
<div class="settings"><a href="/settings">⚙️</a></div>
<script>
setInterval(()=>{
  fetch('/data')
    .then(r=>r.json())
    .then(j=>{
      document.getElementById('t').innerHTML=j.t;
      document.getElementById('h').innerHTML=j.h;
    })
    .catch(()=>{});
},2000);
</script>
</body></html>
)rawliteral";

  return page;
}

String pageSettings() {
  return "<html><head><meta charset='utf-8'>" + commonStyle() +
    "</head><body><div class='box'>"
    "<h3>WiFi</h3><form action='/save'>"
    "SSID:<input name='s' maxlength='32'><br>"
    "PASS:<input name='p' type='password' maxlength='39'><br>"
    "<input type='submit' value='ذخیره'></form><hr>"
    "<h3>Firmware</h3><form method='POST' action='/fw' enctype='multipart/form-data'>"
    "<input type='file' name='update'><input type='submit' value='بارگذاری'></form><hr>"
    "<h3>Background</h3><form method='POST' action='/bg' enctype='multipart/form-data'>"
    "<input type='file' name='bg'><input type='submit' value='بارگذاری'></form><hr>"
    "<h3>CSS</h3><form method='POST' action='/css' enctype='multipart/form-data'>"
    "<input type='file' name='css'><input type='submit' value='بارگذاری'></form><hr>"
    "<h3>GPIO & X9C</h3><form method='POST' action='/config' enctype='multipart/form-data'>"
    "<input type='file' name='config'><input type='submit' value='بارگذاری'></form><hr>"
    "<h3>Logs</h3><a href='/log'>نمایش لاگ داخلی</a><hr>"
    "<form action='/reset'><button style='background:red;color:white'>بازنشانی</button></form>"
    "<a href='/'>⬅ بازگشت</a></div></body></html>";
}

/* ================= HANDLERS ================= */
void handleData() {
  server.send(
    200,
    "application/json",
    "{\"t\":" + String(t, 1) + ",\"h\":" + String(h, 0) + "}"
  );
}

void handleSave() {
  logPrint("=== دریافت اطلاعات شبکه ===");

  String ssid = server.arg("s");
  String pass = server.arg("p");

  if (ssid.length() == 0 || ssid.length() > 32) {
    server.send(400, "text/plain; charset=utf-8", "SSID نامعتبر است");
    return;
  }

  if (pass.length() > 39) {
    server.send(400, "text/plain; charset=utf-8", "رمز عبور بیش از ظرفیت حافظه است");
    return;
  }

  EEPROM.write(ADDR_FLAG, 1);
  saveString(ADDR_SSID, ssid);
  saveString(ADDR_PASS, pass);
  EEPROM.commit();

  stopMDNS();

  server.send(
    200,
    "text/plain; charset=utf-8",
    "تنظیمات ذخیره شد؛ اتصال مجدد آغاز می‌شود."
  );

  delay(300);
  wifiState = MY_WIFI_INIT;
  logPrint("WiFi settings saved");
}

void handleLog() {
  server.send(200, "text/plain; charset=utf-8", getLogs());
}

/* ================= OTHER UPLOAD HANDLERS ================= */
void handleBgUpload() {
  HTTPUpload &u = server.upload();

  if (u.status == UPLOAD_FILE_START) {
    if (littleFSReady) LittleFS.remove("/bg.jpg");
  }

  if (u.status == UPLOAD_FILE_WRITE) {
    if (littleFSReady) {
      File f = LittleFS.open("/bg.jpg", "a");
      if (f) {
        f.write(u.buf, u.currentSize);
        f.close();
      }
    }
  }

  if (u.status == UPLOAD_FILE_END) {
    server.send(200, "text/plain", "BG OK");
  }
}

void handleCssUpload() {
  HTTPUpload &u = server.upload();

  if (u.status == UPLOAD_FILE_START) {
    if (littleFSReady) LittleFS.remove("/style.css");
  }

  if (u.status == UPLOAD_FILE_WRITE) {
    if (littleFSReady) {
      File f = LittleFS.open("/style.css", "a");
      if (f) {
        f.write(u.buf, u.currentSize);
        f.close();
      }
    }
  }

  if (u.status == UPLOAD_FILE_END) {
    server.send(200, "text/plain", "CSS OK");
  }
}

void handleConfigUpload() {
  HTTPUpload &u = server.upload();

  if (u.status == UPLOAD_FILE_START) {
    if (littleFSReady) LittleFS.remove("/config.json");
  }

  if (u.status == UPLOAD_FILE_WRITE) {
    if (littleFSReady) {
      File f = LittleFS.open("/config.json", "a");
      if (f) {
        f.write(u.buf, u.currentSize);
        f.close();
      }
    }
  }

  if (u.status == UPLOAD_FILE_END) {
    server.send(
      200,
      "text/plain; charset=utf-8",
      "Config OK, راه‌اندازی مجدد توصیه می‌شود"
    );

    delay(500);
    ESP.restart();
  }
}

void resetAll() {
  stopMDNS();

  for (int i = 0; i < EEPROM_SIZE; i++) {
    EEPROM.write(i, 0);
  }

  EEPROM.commit();
  ESP.restart();
}

/* ================= GPIO CONVERSION ================= */
int gpioFromKey(const String &key) {
  if (key.length() == 0) return -1;

  if (key.startsWith("D") || key.startsWith("d")) {
    int d = key.substring(1).toInt();

    switch (d) {
      case 0: return 16;
      case 1: return 5;
      case 2: return 4;
      case 3: return 0;
      case 4: return 2;
      case 5: return 14;
      case 6: return 12;
      case 7: return 13;
      case 8: return 15;
      default: return -1;
    }
  }

  // Numeric GPIO keys are accepted only as GPIO numbers.
  if (key.length() > 2) return -1;

  for (uint8_t i = 0; i < key.length(); i++) {
    if (!isDigit(key[i])) return -1;
  }

  int pin = key.toInt();
  if (pin >= 0 && pin <= 16) return pin;

  return -1;
}

/* ================= CONFIG LOADER ================= */
void loadConfig() {
  if (!littleFSReady) return;
  if (!LittleFS.exists("/config.json")) return;

  File f = LittleFS.open("/config.json", "r");

  if (!f) {
    logError("باز کردن config ناموفق بود");
    return;
  }

  // ArduinoJson 6 and 7 compatibility.
#if ARDUINOJSON_VERSION_MAJOR >= 7
  JsonDocument doc;
#else
  StaticJsonDocument<1024> doc;
#endif

  DeserializationError error = deserializeJson(doc, f);
  f.close();

  if (error) {
    logError("خطا در config: " + String(error.c_str()));
    return;
  }

  gpioCount = 0;

  JsonObject gpioObject = doc["gpio"].as<JsonObject>();

  for (JsonPair kv : gpioObject) {
    if (gpioCount >= MAX_GPIO) break;

    String key = kv.key().c_str();
    int pin = gpioFromKey(key);

    if (pin < 0 || pin > 16) {
      logPrint("Invalid GPIO: " + key);
      continue;
    }

    String mode = kv.value()["mode"].as<String>();

    gpioConfigs[gpioCount].pin = (uint8_t)pin;
    gpioConfigs[gpioCount].mode = mode;
    gpioConfigs[gpioCount].function =
      kv.value()["function"].as<String>();

    pinMode(
      pin,
      mode == "OUTPUT" ? OUTPUT : INPUT
    );

    gpioCount++;
  }

  int minValue = doc["x9c"]["min"] | 0;
  int maxValue = doc["x9c"]["max"] | 255;
  int currentValue = doc["x9c"]["current"] |
    (minValue + (maxValue - minValue) / 2);

  if (minValue < 0) minValue = 0;
  if (maxValue > 255) maxValue = 255;

  if (minValue > maxValue) {
    minValue = 0;
    maxValue = 255;
  }

  if (currentValue < minValue) currentValue = minValue;
  if (currentValue > maxValue) currentValue = maxValue;

  x9c.min = (uint8_t)minValue;
  x9c.max = (uint8_t)maxValue;
  x9c.current = (uint8_t)currentValue;

  logPrint("GPIO configuration loaded");
  logPrint("X9C configuration loaded");
}

/* ================= SETUP ================= */
void setup() {
  Serial.begin(115200);
  delay(100);

  logPrint("Heater Controller E3.2.2");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  EEPROM.begin(EEPROM_SIZE);

  littleFSReady = LittleFS.begin();

  if (!littleFSReady) {
    logPrint("LittleFS mount failed");
  } else {
    logPrint("LittleFS mounted");
  }

  dht.begin();

  /* ---------- Firmware Crash Check ---------- */
  uint8_t crash = EEPROM.read(ADDR_CRASH_CNT);

  fwPending =
    EEPROM.read(ADDR_FW_PENDING) == 1 &&
    EEPROM.read(ADDR_FW_VALID) != 1;

  if (fwPending && crash >= MAX_CRASH) {
    logError("FW rollback / حالت ایمن");

    EEPROM.write(ADDR_FW_PENDING, 0);
    EEPROM.write(ADDR_FW_VALID, 0);
    EEPROM.write(ADDR_CRASH_CNT, 0);
    EEPROM.commit();

    enterSafeMode();
  }

  if (fwPending) {
    if (crash < 254) crash++;
    EEPROM.write(ADDR_CRASH_CNT, crash);
    EEPROM.commit();
    fwBootTimer = millis();
  } else {
    EEPROM.write(ADDR_CRASH_CNT, 0);
    EEPROM.commit();
  }

  /* ---------- Watchdog ---------- */
  ESP.wdtEnable(WDTO_8S);
  hwWdt.attach(2, hwWatchdogKick);

  /* ---------- Load Configuration ---------- */
  loadConfig();

  /* ---------- Web Routes ---------- */
  server.on("/", []() {
    server.send(200, "text/html; charset=utf-8", pageMain());
  });

  server.on("/settings", []() {
    server.send(200, "text/html; charset=utf-8", pageSettings());
  });

  server.on("/data", handleData);
  server.on("/log", handleLog);
  server.on("/save", handleSave);
  server.on("/reset", resetAll);
  server.on("/fw_ok", handleFwOk);

  server.on("/bg", HTTP_POST, []() {}, handleBgUpload);
  server.on("/css", HTTP_POST, []() {}, handleCssUpload);
  server.on("/config", HTTP_POST, []() {}, handleConfigUpload);

  /* ---------- OTA ---------- */
  httpUpdater.setup(&server, "/fw");

  /* ---------- Web Server ---------- */
  server.begin();
  logPrint("Web Server started");

  /* ---------- WiFi ---------- */
  wifiState = MY_WIFI_INIT;
  logPrint("WiFi State Machine started");
}

/* ================= LOOP ================= */
void loop() {
  /* ---------- Web Server ---------- */
  server.handleClient();

  /* ---------- mDNS ---------- */
  if (mdnsStarted) {
    MDNS.update();
  }

  /* ---------- DHT ---------- */
  if (millis() - dhtTimer > 3000UL) {
    dhtTimer = millis();

    float nt = dht.readTemperature();
    float nh = dht.readHumidity();

    if (!isnan(nt) && !isnan(nh)) {
      t = nt;
      h = nh;
    }

    logPrint(
      "Temp: " + String(t, 1) +
      "°C, Hum: " + String(h, 0) + "%"
    );
  }

  /* ---------- Firmware Watchdog ---------- */
  if (
    fwPending &&
    !fwConfirmed &&
    millis() - fwBootTimer > FW_WATCHDOG_MS
  ) {
    logError("FW not confirmed");
    logPrint("FW pending تایید نشده، راه‌اندازی مجدد");
    ESP.restart();
  }

  /* ---------- WiFi State Machine ---------- */
  switch (wifiState) {
    case MY_WIFI_INIT:
      if (EEPROM.read(ADDR_FLAG) == 1) {
        startSTA();
      } else {
        wifiState = MY_WIFI_AP_MODE;
      }
      break;

    case MY_WIFI_TRY_STA:
      if (WiFi.status() == WL_CONNECTED) {
        logPrint("WiFi connected");
        logPrint("IP: " + WiFi.localIP().toString());

        wifiState = MY_WIFI_STA_CONNECTED;
        ledMode = LED_CONNECTED;
        mdnsRetryTimer = 0;
      }
      else if (millis() - wifiTimer > WIFI_CONNECT_TIMEOUT) {
        logPrint("WiFi connection failed -> AP");

        WiFi.disconnect();
        wifiState = MY_WIFI_AP_MODE;
      }
      break;

    case MY_WIFI_STA_CONNECTED:
      if (WiFi.status() != WL_CONNECTED) {
        stopMDNS();

        logPrint("WiFi disconnected");

        WiFi.disconnect();
        wifiState = MY_WIFI_INIT;
        ledMode = LED_CONNECTING;
        break;
      }

      startMDNS();
      break;

    case MY_WIFI_AP_MODE:
      startAP();
      ledMode = LED_AP;
      break;

    case MY_WIFI_SAFE_MODE:
      break;
  }
}

