#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <EEPROM.h>
#include <LittleFS.h>
#include <Ticker.h>
#include <DHT.h>
#include <ArduinoJson.h>

/* ================= CONFIG ================= */
#define LED_PIN D5
#define EEPROM_SIZE 256

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

/* ================= GLOBAL ================= */
DHT dht(DHT_PIN, DHT_TYPE);
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
Ticker hwWdt;

float t = 0, h = 0;
unsigned long dhtTimer = 0;
unsigned long fwBootTimer = 0;

bool fwConfirmed = false;
bool fwPending = false;

enum LedMode { LED_AP, LED_CONNECTING, LED_CONNECTED };
LedMode ledMode = LED_AP;
unsigned long ledTimer = 0;
bool ledState = false;

/* ================= DYNAMIC GPIO & X9C CONFIG ================= */
struct GPIOConfig { uint8_t pin; String mode; String function; };
#define MAX_GPIO 10
GPIOConfig gpioConfigs[MAX_GPIO];
uint8_t gpioCount = 0;

struct X9CConfig { uint8_t min; uint8_t max; uint8_t current; };
X9CConfig x9c;

/* ================= EEPROM ================= */
void saveString(int addr, const String &data) {
  for (int i = 0; i < data.length(); i++) EEPROM.write(addr + i, data[i]);
  EEPROM.write(addr + data.length(), 0);
  EEPROM.commit();
}

String readString(int addr) {
  char buf[64];
  int i = 0;
  char c;
  do { c = EEPROM.read(addr + i); buf[i++] = c; } while (c && i < 63);
  buf[i] = 0;
  return String(buf);
}

void logError(const String &e) {
  saveString(ADDR_LAST_ERROR, e);
}

/* ================= WATCHDOG ================= */
void hwWatchdogKick() {
  ESP.wdtFeed();
}

/* ================= OTA CONFIRM ================= */
void handleFwOk() {
  EEPROM.write(ADDR_FW_PENDING, 0);
  EEPROM.write(ADDR_FW_VALID, 1);
  EEPROM.write(ADDR_CRASH_CNT, 0);
  EEPROM.commit();
  fwConfirmed = true;
  server.send(200, "text/html", "فریم‌ویر تأیید شد ✅");
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
  String bg = LittleFS.exists("/bg.jpg") ? "body{background:url(/bg.jpg) no-repeat center/cover fixed;}" : "";

  page += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  page += commonStyle();
  page += "<style>" + bg + R"rawliteral(
.container{display:flex;justify-content:center;gap:30px;margin-top:80px}
.card{width:240px;height:200px;border:4px solid #000;border-radius:20px;
background:rgba(255,255,255,.9);text-align:center;font-size:32px;padding:20px}
.value{font-size:42px}
</style></head><body>")rawliteral";

  if (fwPending) {
    page += R"rawliteral(
<div style="margin:30px auto;width:340px;border:4px dashed red;
border-radius:16px;background:#fff;padding:20px;text-align:center;font-size:22px">
⚠ <b>فریم‌ویر جدید</b><br><br>
تأیید تا <span id="sec">30</span> ثانیه<br><br>
<a href="/fw_ok" style="display:inline-block;padding:14px 20px;
background:green;color:white;border-radius:10px;font-size:24px;text-decoration:none">
فریم‌ویر جدید تأیید می‌شود
</a>
</div>
<script>
let s=30;
setInterval(()=>{if(s>0){s--;sec.innerText=s}},1000);
</script>
)rawliteral";
  }

  page += R"rawliteral(
<h2 style='text-align:center'>کنترل هیتر</h2>
<div class="container">
<div class="card">🌡️<br>دما<div class="value"><span id="t">--</span>°C</div></div>
<div class="card">💧<br>رطوبت<div class="value"><span id="h">--</span>%</div></div>
</div>
<div style='text-align:center;margin-top:20px'>Version: E3.1.2</div>
<div class="settings"><a href="/settings">⚙️</a></div>
<script>
setInterval(()=>{
fetch('/data').then(r=>r.json()).then(j=>{t.innerHTML=j.t; h.innerHTML=j.h;});
},2000);
</script>
</body></html>
)rawliteral";

  return page;
}

String pageSettings() {
  return "<html><head><meta charset='utf-8'>" + commonStyle() + "</head><body><div class='box'>"
         "<h3>WiFi</h3><form action='/save'>"
         "SSID:<input name='s'><br>PASS:<input name='p' type='password'><br>IP:<input name='ip'><br><input type='submit' value='Save'></form><hr>"
         "<h3>Firmware</h3><form method='POST' action='/fw' enctype='multipart/form-data'>"
         "<input type='file' name='update'><input type='submit' value='Upload'></form><hr>"
         "<h3>Background</h3><form method='POST' action='/bg' enctype='multipart/form-data'>"
         "<input type='file' name='bg'><input type='submit' value='Upload'></form><hr>"
         "<h3>CSS</h3><form method='POST' action='/css' enctype='multipart/form-data'>"
         "<input type='file' name='css'><input type='submit' value='Upload'></form><hr>"
         "<h3>GPIO & X9C</h3><form method='POST' action='/config' enctype='multipart/form-data'>"
         "<input type='file' name='config'><input type='submit' value='Upload'></form><hr>"
         "<form action='/reset'><button style='background:red;color:white'>Reset</button></form>"
         "<a href='/'>⬅ Back</a></div></body></html>";
}

/* ================= HANDLERS ================= */
void handleData() {
  server.send(200,"application/json", "{" + String("\"t\":") + String(t,1) + ",\"h\":" + String(h,0) + "}");
}

// اصلاح handleSave برای اتصال STA و ذخیره IP به صورت String
void handleSave(){
  EEPROM.write(ADDR_FLAG, 1);
  saveString(ADDR_SSID, server.arg("s"));
  saveString(ADDR_PASS, server.arg("p"));
  saveString(ADDR_IP4, server.arg("ip")); // IP به صورت رشته ذخیره می‌شود
  EEPROM.commit();

  server.send(200,"text/plain","ذخیره شد. در حال اتصال به شبکه...");
  delay(500);

  // خاموش کردن AP و رفتن به حالت STA
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);

  String ssid = readString(ADDR_SSID);
  String pass = readString(ADDR_PASS);
  WiFi.begin(ssid.c_str(), pass.c_str());

  // پیکربندی IP ثابت در صورت وارد شدن
  String ipStr = readString(ADDR_IP4);
  int parts[4];
  if(sscanf(ipStr.c_str(), "%d.%d.%d.%d", &parts[0], &parts[1], &parts[2], &parts[3])==4){
    IPAddress staticIP(parts[0], parts[1], parts[2], parts[3]);
    WiFi.config(staticIP, WiFi.gatewayIP(), WiFi.subnetMask());
  }

  unsigned long start = millis();
  while(WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
  }

  if(WiFi.status() == WL_CONNECTED) {
    Serial.println("STA connected!");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("STA failed, fallback to AP");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Heater-Recovery"); // نام AP اصلاح شد
  }

  ESP.restart();
}

void handleBgUpload(){HTTPUpload &u=server.upload(); if(u.status==UPLOAD_FILE_START) LittleFS.remove("/bg.jpg"); if(u.status==UPLOAD_FILE_WRITE){File f=LittleFS.open("/bg.jpg","a"); f.write(u.buf,u.currentSize); f.close();} if(u.status==UPLOAD_FILE_END) server.send(200,"text/plain","BG OK");}
void handleCssUpload(){HTTPUpload &u=server.upload(); if(u.status==UPLOAD_FILE_START) LittleFS.remove("/style.css"); if(u.status==UPLOAD_FILE_WRITE){File f=LittleFS.open("/style.css","a"); f.write(u.buf,u.currentSize); f.close();} if(u.status==UPLOAD_FILE_END) server.send(200,"text/plain","CSS OK");}
void handleConfigUpload(){HTTPUpload &u=server.upload(); if(u.status==UPLOAD_FILE_START) LittleFS.remove("/config.json"); if(u.status==UPLOAD_FILE_WRITE){File f=LittleFS.open("/config.json","a"); f.write(u.buf,u.currentSize); f.close();} if(u.status==UPLOAD_FILE_END){server.send(200,"text/plain","Config OK, restart recommended"); delay(500); ESP.restart();}}
void resetAll(){for(int i=0;i<EEPROM_SIZE;i++) EEPROM.write(i,0); EEPROM.commit(); ESP.restart();}

/* ================= CONFIG LOADER ================= */
void loadConfig(){
  if(!LittleFS.exists("/config.json")) return;
  File f=LittleFS.open("/config.json","r");
  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc,f);
  f.close();
  if(error){logError("Config parse error"); return;}

  gpioCount=0;
  for(JsonPair kv : doc["gpio"].as<JsonObject>()){
    if(gpioCount>=MAX_GPIO) break;
    String keyStr = String(kv.key().c_str());
    gpioConfigs[gpioCount].pin = keyStr[1]-'0';
    gpioConfigs[gpioCount].mode = kv.value()["mode"].as<String>();
    gpioConfigs[gpioCount].function = kv.value()["function"].as<String>();
    pinMode(gpioConfigs[gpioCount].pin, gpioConfigs[gpioCount].mode=="OUTPUT"?OUTPUT:INPUT);
    gpioCount++;
  }

  x9c.min = doc["x9c"]["min"] | 0;
  x9c.max = doc["x9c"]["max"] | 255;
  x9c.current = doc["x9c"]["current"] | ((x9c.max-x9c.min)/2);
}

/* ================= SETUP ================= */
void setup(){
  Serial.begin(115200);
  pinMode(LED_PIN,OUTPUT);
  EEPROM.begin(EEPROM_SIZE);
  LittleFS.begin();
  dht.begin();

  fwPending = EEPROM.read(ADDR_FW_PENDING)==1 && EEPROM.read(ADDR_FW_VALID)!=1;
  if(fwPending) fwBootTimer = millis();

  ESP.wdtEnable(WDTO_8S);
  hwWdt.attach(2, hwWatchdogKick);

  loadConfig();

  server.on("/",[]{server.send(200,"text/html",pageMain());});
  server.on("/settings",[]{server.send(200,"text/html",pageSettings());});
  server.on("/data",handleData);
  server.on("/save",handleSave);
  server.on("/reset",resetAll);
  server.on("/fw_ok",handleFwOk);
  server.on("/bg",HTTP_POST,[]{},handleBgUpload);
  server.on("/css",HTTP_POST,[]{},handleCssUpload);
  server.on("/config",HTTP_POST,[]{},handleConfigUpload);

  httpUpdater.setup(&server,"/fw");
  server.begin();

  // فعال کردن AP اولیه Safe Mode با نام Heater-Recovery
  WiFi.softAP("Heater-Recovery");
}

/* ================= LOOP ================= */
void loop(){
  server.handleClient();
  if(millis()-dhtTimer>3000){
    dhtTimer=millis();
    float nt=dht.readTemperature();
    float nh=dht.readHumidity();
    if(!isnan(nt)&&!isnan(nh)){ t=nt; h=nh; }
  }

  if(fwPending && !fwConfirmed && millis()-fwBootTimer>FW_WATCHDOG_MS){
    logError("FW not confirmed");
    ESP.restart();
  }
}
