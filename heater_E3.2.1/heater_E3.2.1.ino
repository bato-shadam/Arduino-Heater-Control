#include <ESP8266WiFi.h>
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

DHT dht(DHT_PIN, DHT_TYPE);

/* ================= GLOBAL ================= */
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
Ticker hwWdt;

float t = 0, h = 0;
unsigned long dhtTimer = 0;
unsigned long fwBootTimer = 0;

bool fwConfirmed = false;
bool fwPending = false;
bool mdnsStarted = false;

enum LedMode { LED_AP, LED_CONNECTING, LED_CONNECTED };
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
char logBuffer[LOG_SIZE];
uint16_t logIndex = 0;

void logPrint(const String &msg) {
  Serial.println(msg);
  for (size_t i=0; i<msg.length(); i++) {
    logBuffer[logIndex++] = msg[i];
    if(logIndex>=LOG_SIZE) logIndex=0;
  }
  logBuffer[logIndex++] = '\n';
  if(logIndex>=LOG_SIZE) logIndex=0;
}

String getLogs() {
  String out;
  for(uint16_t i=0;i<LOG_SIZE;i++){
    uint16_t idx = (logIndex+i)%LOG_SIZE;
    char c = logBuffer[idx];
    if(c!=0) out+=c;
  }
  return out;
}

/* ================= EEPROM ================= */
void saveString(int addr, const String& data) {
  for (int i = 0; i < data.length(); i++)
    EEPROM.write(addr + i, data[i]);
  EEPROM.write(addr + data.length(), 0);
  EEPROM.commit();
}

String readString(int addr) {
  char buf[64];
  int i = 0;
  char c;
  do {
    c = EEPROM.read(addr + i);
    buf[i++] = c;
  } while (c && i < 63);
  buf[i] = 0;
  return String(buf);
}

void logError(const String& e) {
  saveString(ADDR_LAST_ERROR, e);
  logPrint("[ERROR] "+e);
}

/* ================= WATCHDOG ================= */
void hwWatchdogKick() { ESP.wdtFeed(); }

/* ================= OTA CONFIRM ================= */
void handleFwOk() {
  EEPROM.write(ADDR_FW_PENDING, 0);
  EEPROM.write(ADDR_FW_VALID, 1);
  EEPROM.write(ADDR_CRASH_CNT, 0);
  EEPROM.commit();
  fwConfirmed = true;
  server.send(200, "text/html", "فریم‌ویر تأیید شد ✅");
  logPrint("Firmware confirmed ✅");
}

/* ================= SAFE MODE ================= */
void enterSafeMode() {
  WiFi.softAP("Heater-SafeMode");
  server.on("/", [](){ server.send(200, "text/html", "<h2>حالت ایمن فعال است</h2><p>فریم‌ویر خراب است.</p>"); });
  server.begin();
  logPrint("Entering SAFE MODE");
  while(true){
    server.handleClient();
    delay(100);
  }
}

/* ================= UI ================= */
String commonStyle() {
  if (LittleFS.exists("/style.css")) {
    File f = LittleFS.open("/style.css", "r");
    String css = "<style>" + f.readString() + "</style>";
    f.close();
    return css;
  }
  return R"rawliteral(
<style>
body{font-family:tahoma;background:#f2f2f2}
.box{width:360px;margin:40px auto;padding:25px;
border:3px solid #000;border-radius:12px;background:#fff}
input,button{font-size:22px;padding:10px;width:100%}
h3{font-size:26px}
.settings{position:fixed;bottom:20px;left:20px;font-size:120px}
</style>
)rawliteral";
}

String pageMain() {
  String page;
  String bg = LittleFS.exists("/bg.jpg")
    ? "body{background:url(/bg.jpg) no-repeat center/cover fixed;} "
    : "";
  page += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  page += commonStyle();
  page += "<style>" + bg + R"rawliteral(
.container{display:flex;justify-content:center;gap:30px;margin-top:80px}
.card{width:240px;height:200px;border:4px solid #000;border-radius:20px;
background:rgba(255,255,255,.9);text-align:center;font-size:32px;padding:20px}
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
<div class='version'>نسخه برنامه: E3.2.1</div>
<div class="settings"><a href="/settings">⚙️</a></div>
<script>
setInterval(()=>{ fetch('/data').then(r=>r.json()).then(j=>{ t.innerHTML=j.t; h.innerHTML=j.h; }); },2000);
</script>
</body></html>
)rawliteral";
  return page;
}

String pageSettings() {
  return "<html><head><meta charset='utf-8'>" + commonStyle() +
    "</head><body><div class='box'>"
    "<h3>WiFi</h3><form action='/save'>"
    "SSID:<input name='s'><br>PASS:<input name='p' type='password'><br>"
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
  server.send(200,"application/json",
    "{\"t\":"+String(t,1)+",\"h\":"+String(h,0)+"}");
}

void handleSave() {
  logPrint("=== دریافت اطلاعات شبکه ===");
  String ssid = server.arg("s");
  String pass = server.arg("p");

  EEPROM.write(ADDR_FLAG, 1);
  saveString(ADDR_SSID, ssid);
  saveString(ADDR_PASS, pass);
  EEPROM.commit();

  wifiState = MY_WIFI_INIT;
  server.send(200,"text/plain","تنظیمات ذخیره شد، وضعیت سریال را بررسی کنید");
}

void handleLog() {
  server.send(200,"text/plain",getLogs());
}

/* ================= سایر هندلرها ================= */
void handleBgUpload() { HTTPUpload& u=server.upload(); if(u.status==UPLOAD_FILE_START) LittleFS.remove("/bg.jpg"); if(u.status==UPLOAD_FILE_WRITE){ File f=LittleFS.open("/bg.jpg","a"); f.write(u.buf,u.currentSize); f.close(); } if(u.status==UPLOAD_FILE_END) server.send(200,"text/plain","BG OK"); }
void handleCssUpload() { HTTPUpload& u = server.upload(); if(u.status==UPLOAD_FILE_START) LittleFS.remove("/style.css"); if(u.status==UPLOAD_FILE_WRITE){ File f = LittleFS.open("/style.css","a"); f.write(u.buf,u.currentSize); f.close(); } if(u.status==UPLOAD_FILE_END) server.send(200,"text/plain","CSS OK"); }
void handleConfigUpload() { HTTPUpload& u = server.upload(); if(u.status==UPLOAD_FILE_START) LittleFS.remove("/config.json"); if(u.status==UPLOAD_FILE_WRITE){ File f = LittleFS.open("/config.json","a"); f.write(u.buf,u.currentSize); f.close(); } if(u.status==UPLOAD_FILE_END) { server.send(200,"text/plain","Config OK, راه‌اندازی مجدد توصیه می‌شود"); delay(500); ESP.restart(); } }
void resetAll() { for(int i=0;i<EEPROM_SIZE;i++) EEPROM.write(i,0); EEPROM.commit(); ESP.restart(); }

/* ================= CONFIG LOADER ================= */
void loadConfig() {
  if(!LittleFS.exists("/config.json")) return;

  File f = LittleFS.open("/config.json","r");
  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, f);
  f.close();
  if(error) { 
    logError("خطا در config");
    return;
  }

  gpioCount = 0;
  for(JsonPair kv : doc["gpio"].as<JsonObject>()){
    if(gpioCount>=MAX_GPIO) break;

    const char* keyStr = kv.key().c_str();
    gpioConfigs[gpioCount].pin = keyStr[1]-'0';
    gpioConfigs[gpioCount].mode = kv.value()["mode"].as<String>();
    gpioConfigs[gpioCount].function = kv.value()["function"].as<String>();
    pinMode(gpioConfigs[gpioCount].pin,
    gpioConfigs[gpioCount].mode=="OUTPUT"?OUTPUT:INPUT);
    gpioCount++;
  }

  x9c.min = doc["x9c"]["min"] | 0;
  x9c.max = doc["x9c"]["max"] | 255;
  x9c.current = doc["x9c"]["current"] | ((x9c.max-x9c.min)/2);
}

/* ================= SETUP ================= */
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN,OUTPUT);
  EEPROM.begin(EEPROM_SIZE);
  LittleFS.begin();
  dht.begin();

  uint8_t crash = EEPROM.read(ADDR_CRASH_CNT);
  fwPending = EEPROM.read(ADDR_FW_PENDING)==1 && EEPROM.read(ADDR_FW_VALID)!=1;
  if (fwPending && crash >= MAX_CRASH) {
    logError("FW rollback / حالت ایمن");
    EEPROM.write(ADDR_FW_PENDING, 0);
    EEPROM.write(ADDR_FW_VALID, 0);
    EEPROM.write(ADDR_CRASH_CNT, 0);
    EEPROM.commit();
    enterSafeMode();
  }
  EEPROM.write(ADDR_CRASH_CNT, crash + 1);
  EEPROM.commit();
  if(fwPending) fwBootTimer = millis();

  ESP.wdtEnable(WDTO_8S);
  hwWdt.attach(2, hwWatchdogKick);

  loadConfig();

  server.on("/",[]{server.send(200,"text/html",pageMain());});
  server.on("/settings",[]{server.send(200,"text/html",pageSettings());});
  server.on("/data",handleData);
  server.on("/log", handleLog); // هندلر لاگ داخلی
  server.on("/save",handleSave);
  server.on("/reset",resetAll);
  server.on("/fw_ok",handleFwOk);
  server.on("/bg",HTTP_POST,[]{},handleBgUpload);
  server.on("/css", HTTP_POST, [](){}, handleCssUpload);
  server.on("/config", HTTP_POST, [](){}, handleConfigUpload);

  httpUpdater.setup(&server,"/fw");
  server.begin();

  WiFi.softAP("Heater-Setting");
  logPrint("SoftAP فعال شد: Heater-Setting");
}

/* ================= LOOP ================= */
void loop() {
  server.handleClient();

  if (mdnsStarted) {
    MDNS.update();
  }
  
  if(millis()-dhtTimer>3000){
    dhtTimer=millis();
    float nt=dht.readTemperature();
    float nh=dht.readHumidity();
    if(!isnan(nt)&&!isnan(nh)){t=nt;h=nh;}
    logPrint("Temp: "+String(t,1)+"°C, Hum: "+String(h,0)+"%");
  }

  if(fwPending && !fwConfirmed && millis()-fwBootTimer>FW_WATCHDOG_MS){
    logError("FW not confirmed");
    logPrint("FW pending تایید نشده، راه‌اندازی مجدد");
    ESP.restart();
  }

  switch (wifiState) {
    case MY_WIFI_INIT:
      if (EEPROM.read(ADDR_FLAG) == 1) startSTA();
      else wifiState = MY_WIFI_AP_MODE;
      break;

    case MY_WIFI_TRY_STA:
      if (WiFi.status() == WL_CONNECTED) {
        logPrint("✅ WiFi متصل شد");
        logPrint("IP: "+WiFi.localIP().toString());
        wifiState = MY_WIFI_STA_CONNECTED;
      }
      else if (millis() - wifiTimer > 15000) {
        logPrint("❌ اتصال ناموفق → AP");
        wifiState = MY_WIFI_AP_MODE;
      }
      break;
    
case MY_WIFI_STA_CONNECTED:

    if (!mdnsStarted) {
        if (MDNS.begin("heater")) {
            mdnsStarted = true;
            MDNS.addService("http", "tcp", 80);
            logPrint("✅ mDNS started: heater.local");
        }
        else {
            logPrint("❌ mDNS responder failed");
        }
    }

    break;
    
  }
}

void startSTA() {
  String ssid = readString(ADDR_SSID);
  String pass = readString(ADDR_PASS);
  if(ssid.length()==0) { wifiState=MY_WIFI_AP_MODE; return; }

  logPrint("⏳ تلاش برای اتصال به WiFi...");
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  wifiTimer = millis();
  wifiState = MY_WIFI_TRY_STA;
}
