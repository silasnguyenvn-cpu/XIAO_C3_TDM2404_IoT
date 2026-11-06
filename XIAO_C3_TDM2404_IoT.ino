/* XIAO ESP32-C3 + TDM2404 / Quectel EG800K, IoT v3.1.3.
   Arduino ESP32 core 3.3.12; XIAO_ESP32C3; USB CDC On Boot enabled.
   UART1 RX=GPIO20 (D7), TX=GPIO21 (D6). CONFIG=GPIO3/D1, SOS=GPIO4/D2.
   BAT_ADC=GPIO2/D0, divider 200k/200k. Never drive GPIO15 or XIAO EN.
   Hold CONFIG/SOS for 3 seconds. Configuration is a local Wi-Fi web app.
   No third-party libraries; all cloud traffic uses the modem's 4G HTTPS stack.
*/
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "TrackerCore.h"
#include "GpsParserTests.h"
#include "ModemPort.h"
#include "PortalPage.h"
#include "CloudCA.h"
#include "RemoteSms.h"

#if !defined(CONFIG_IDF_TARGET_ESP32C3)
#error "Select XIAO_ESP32C3."
#endif
#if !ARDUINO_USB_CDC_ON_BOOT
#error "Enable USB CDC On Boot."
#endif

constexpr int CONFIG_PIN=3,SOS_PIN=4,BATTERY_PIN=2;
constexpr uint32_t GPS_EVERY=10000,NETWORK_EVERY=30000,SOS_COOLDOWN=30000;
constexpr uint32_t PORTAL_LIMIT=600000;
constexpr float BAT_CALIBRATION=1.0f;
const char CLOUD_HOST[]="circuitdigest.cloud";
const char CLOUD_PATH[]="/api/v1/geolinker";
const char CLOUD_URL[]="https://circuitdigest.cloud/api/v1/geolinker";
const char CA_FILE[]="UFS:tracker_x1_2026.pem";

tracker::Config cfg,staged;
tracker::Fix gps;
tracker::HoldButton configButton,sosButton;
ModemPort modem;
remote_sms::Scan inboxScan;
bool smsRxReady=false,smsInboxDirty=true,hadRemoteReply=false;
String smsReceiveStore="ME";
bool smsDiagRequested=false,smsDiagHold=false;
uint32_t lastInboxScan=0,lastRxSetup=0,lastRemoteReply=0;
WebServer server(80);
DNSServer dns;
Preferences prefs;
bool prefsOK=false,saved=false,applyPending=false,portalActive=false;
bool portalRequested=false,closeRequested=false,sosPending=false,sosActive=false;
bool testPending=false,uploadPending=false,paused=false,initRequested=false;
bool modemReady=false,gnss=false,simReady=false,registered=false,pdpDirty=true,tlsReady=false;
bool cloudAuthBlocked=false,hadSOS=false;
uint32_t portalOpened=0,closeAt=0,lastSOS=0,lastGPS=0,lastNetwork=0,lastInit=0;
uint32_t cloudAttemptAt=0,cloudWaitMs=0,lastBattery=0;
unsigned cloudFailures=0;
float battery=NAN;
String ssid,csrf,console;
String job="San sang",gpsStatus="Dang cho GPS",cloudStatus="Chua gui",smsStatus="Chua gui";
String lastEvent="Khoi dong thiet bi",lastCloudStamp;

void serviceLocal();
void startPortal();
void modemBoot();
void queueSOS();
void checkGPS();
bool remoteSenderAllowed(const String &sender);

bool remoteSenderAllowed(const String &sender) {
  // Incoming permission is independent of outbound recipients in cfg.phones.
  constexpr char authorizedSender[] = ""; // Enter your authorized SMS sender before flashing.
  return remote_sms::samePhone(sender,authorizedSender);
}

void onModemLine(const String &line) {
  if(line.startsWith("+CMTI:"))smsInboxDirty=true;
  inboxScan.line(line);
}

void event(const String &text) {lastEvent=text;Serial.println(text);}
String resultField(const String &r,const String &label) {
  int start=r.indexOf(label);if(start<0)return "";start+=label.length();
  int end=r.indexOf('\n',start);String value=r.substring(start,end<0?r.length():end);
  value.trim();return value;
}
String maps() {
  return gps.known?"https://www.google.com/maps?q="+String(gps.lat,7)+","+String(gps.lon,7):"";
}
bool cloudConfigured() {return cfg.cloudEnabled&&cfg.key[0]&&cfg.device[0]&&cfg.apn[0];}

void modemBoot() {
  smsRxReady=false;smsInboxDirty=true;inboxScan.end();
  modemReady=false;gnss=false;simReady=false;registered=false;tlsReady=false;pdpDirty=true;
  gps.current=false;gpsStatus="Modem vua khoi dong lai";lastInit=millis();
  cloudStatus="Modem khoi dong lai; cho ket noi";
}

void updateBattery() {
  uint32_t sum=0;for(int i=0;i<8;++i)sum+=analogReadMilliVolts(BATTERY_PIN);
  battery=(sum/8.0f)*2*BAT_CALIBRATION/1000.0f;lastBattery=millis();
}

void queueSOS() {
  if(sosPending||sosActive) {event("[SOS] Dang xu ly yeu cau truoc.");return;}
  if(hadSOS&&uint32_t(millis()-lastSOS)<SOS_COOLDOWN) {
    event("[SOS] Cho du 30 giay giua hai lan kich hoat.");return;
  }
  hadSOS=true;lastSOS=millis();sosPending=true;
  event("[SOS] Da nhan yeu cau; se gui khi UART modem san sang.");
}

void serviceLocal() {
  uint32_t now=millis();
  if(configButton.update(digitalRead(CONFIG_PIN)==LOW,now)) {
    if(portalActive)portalOpened=now;else portalRequested=true;
  }
  if(sosButton.update(digitalRead(SOS_PIN)==LOW,now)) queueSOS();
  if(portalRequested) {portalRequested=false;startPortal();}
  if(portalActive) {
    dns.processNextRequest();server.handleClient();
    uint32_t checkedAt=millis(); // Wi-Fi startup/HTTP handlers may advance time.
    if(uint32_t(checkedAt-portalOpened)>=PORTAL_LIMIT ||
       (closeRequested&&uint32_t(checkedAt-closeAt)>=1500)) {
      server.stop();dns.stop();WiFi.softAPdisconnect(true);WiFi.mode(WIFI_OFF);
      portalActive=false;closeRequested=false;
      event("[CONFIG] Da dong Wi-Fi. Giu CONFIG 3 giay de mo lai.");
    }
  }
  if(uint32_t(now-lastBattery)>=5000)updateBattery();
}

void noCache() {server.sendHeader("Cache-Control","no-store");server.sendHeader("X-Content-Type-Options","nosniff");}
void jsonReply(const String &body) {noCache();server.send(200,"application/json; charset=utf-8",body);}
bool validToken() {
  if(server.method()!=HTTP_POST || !csrf.length() || server.arg("token")!=csrf) {
    server.send(403,"text/plain; charset=utf-8","Phiên cấu hình không hợp lệ. Tải lại trang.");return false;
  }
  return true;
}
void handleConfig() {
  const tracker::Config &c=applyPending?staged:cfg;
  String body="{\"token\":"+tracker::jsonString(csrf)+",\"device\":"+tracker::jsonString(c.device)+
    ",\"apn\":"+tracker::jsonString(c.apn)+",\"user\":"+tracker::jsonString(c.user)+
    ",\"interval\":"+String(c.intervalSec)+",\"cloud\":"+(c.cloudEnabled?"true":"false")+
    ",\"hasKey\":"+(c.key[0]?"true":"false");
  for(int i=0;i<3;++i)body+=",\"phone"+String(i+1)+"\":"+tracker::jsonString(c.phones[i]);
  jsonReply(body+"}"); // Never return API key, APN password, or Wi-Fi password.
}
void handleStatus() {
  String status=gpsStatus;
  if(gps.known)status+="; vi tri cach "+String(uint32_t(millis()-gps.acquired)/1000)+" giay";
  String body="{\"sim\":"+tracker::jsonString(simReady?"San sang":"Dang cho")+
    ",\"network\":"+tracker::jsonString(registered?"Da dang ky":"Dang cho dang ky")+
    ",\"gps\":"+tracker::jsonString(status)+",\"battery\":"+tracker::jsonString(isfinite(battery)?String(battery,3):"--")+
    ",\"cloud\":"+tracker::jsonString(cloudStatus)+",\"sms\":"+tracker::jsonString(smsStatus)+
    ",\"job\":"+tracker::jsonString(job)+",\"event\":"+tracker::jsonString(lastEvent)+
    ",\"fresh\":"+(gps.fresh(millis())?"true":"false")+",\"map\":"+tracker::jsonString(maps())+"}";
  jsonReply(body);
}
void handleSave() {
  if(!validToken())return;
  tracker::Config next=applyPending?staged:cfg;
  String key=server.arg("key");key.trim();if(key.startsWith("Bearer "))key=key.substring(7);
  if(key.length())strlcpy(next.key,key.c_str(),sizeof(next.key));
  String device=server.arg("device"),apn=server.arg("apn"),user=server.arg("user");
  device.trim();apn.trim();user.trim();
  String password=server.arg("pass"),wifi=server.arg("wifi");
  String interval=server.arg("interval");
  bool lengths=key.length()<=160&&device.length()<=64&&apn.length()<=63&&user.length()<=63&&
    password.length()<=63&&wifi.length()<=63&&interval.length()<=4;
  strlcpy(next.device,device.c_str(),sizeof(next.device));strlcpy(next.apn,apn.c_str(),sizeof(next.apn));
  strlcpy(next.user,user.c_str(),sizeof(next.user));
  if(server.hasArg("clearpass"))next.pass[0]=0;
  else if(password.length())strlcpy(next.pass,password.c_str(),sizeof(next.pass));
  if(wifi.length())strlcpy(next.wifiPass,wifi.c_str(),sizeof(next.wifiPass));
  next.cloudEnabled=server.hasArg("cloud");
  bool digits=interval.length()>0;
  for(char c:interval)if(c<'0'||c>'9')digits=false;
  next.intervalSec=interval.toInt();
  for(int i=0;i<3;++i) {
    String number=server.arg("phone"+String(i+1));number.trim();
    if(number.length()>16)lengths=false;
    strlcpy(next.phones[i],number.c_str(),sizeof(next.phones[i]));
  }
  // Deduplicate recipients to avoid sending the same event twice to one number.
  for(int i=1;i<3;++i)for(int j=0;j<i;++j)
    if(next.phones[i][0] && String(next.phones[i])==next.phones[j])next.phones[i][0]=0;
  if(!lengths||!digits||!tracker::validate(next)) {
    server.send(400,"text/plain; charset=utf-8",
      "Chưa lưu. Kiểm tra key/Device ID khi bật cloud, APN, số SOS 1, chu kỳ 30–3600 giây và mật khẩu Wi-Fi 8–63 ký tự. Device ID chỉ gồm chữ không dấu, số, _ . -.");return;
  }
  if(!prefsOK||prefs.putBytes("config",&next,sizeof(next))!=sizeof(next)) {
    server.send(500,"text/plain; charset=utf-8","Không lưu được bộ nhớ thiết bị. Chưa áp dụng cấu hình mới.");return;
  }
  staged=next;applyPending=true;saved=true;
  server.send(200,"text/plain; charset=utf-8","Đã lưu. Cấu hình sẽ áp dụng khi tác vụ modem hiện tại kết thúc. Bạn có thể bấm Đóng Wi-Fi.");
}
void handleAction() {
  if(!validToken())return;
  String action=server.arg("action");
  if(action=="close") {closeRequested=true;closeAt=millis();}
  else if(action=="test") {
    if(testPending||sosActive||sosPending) {server.send(409,"text/plain; charset=utf-8","Đang có SMS chờ xử lý.");return;}
    testPending=true;
  } else if(action=="upload")uploadPending=true;
  else {server.send(400,"text/plain","Unknown action");return;}
  server.send(202,"text/plain; charset=utf-8",action=="close"?"Đang đóng Wi-Fi. Giữ CONFIG 3 giây để mở lại.":"Đã nhận yêu cầu. Theo dõi kết quả ở trạng thái thiết bị.");
}
void setupRoutes() {
  server.on("/",HTTP_GET,[]{
    noCache();server.sendHeader("Content-Security-Policy","default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'");
    server.send_P(200,"text/html; charset=utf-8",PORTAL_PAGE);
  });
  server.on("/api/config",HTTP_GET,handleConfig);server.on("/api/status",HTTP_GET,handleStatus);
  server.on("/api/save",HTTP_POST,handleSave);server.on("/api/action",HTTP_POST,handleAction);
  server.onNotFound([]{
    if(server.uri().startsWith("/api/"))server.send(404,"text/plain","Not found");
    else {server.sendHeader("Location","http://192.168.4.1/");server.send(302,"text/plain","");}
  });
}
void startPortal() {
  if(portalActive) {portalOpened=millis();return;}
  WiFi.mode(WIFI_AP);WiFi.softAPConfig(IPAddress(192,168,4,1),IPAddress(192,168,4,1),IPAddress(255,255,255,0));
  if(!WiFi.softAP(ssid.c_str(),cfg.wifiPass,1,false,2)) {event("[CONFIG FAIL] Khong mo duoc Wi-Fi.");return;}
  char token[33];snprintf(token,sizeof(token),"%08lx%08lx%08lx%08lx",
    (unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random());csrf=token;
  dns.start(53,"*",WiFi.softAPIP());server.begin();portalActive=true;portalOpened=millis();closeRequested=false;
  event("[CONFIG] Wi-Fi "+ssid+"; mo http://192.168.4.1 (tu dong dong sau 10 phut).");
}

void checkNetwork() {
  job="Kiem tra SIM/mang";
  String r=modem.command("AT+CPIN?");simReady=ModemPort::ok(r)&&resultField(r,"+CPIN:")=="READY";
  if(!modemReady)return;
  r=modem.command("AT+CEREG?");int n=-1,stat=-1;
  registered=ModemPort::ok(r)&&sscanf(resultField(r,"+CEREG:").c_str(),"%d,%d",&n,&stat)==2&&(stat==1||stat==5);
  if(modemReady) {modem.command("AT+CSQ");}
  lastNetwork=millis();job="San sang";
}
void checkGPS() {
  if(!modemReady||!modem.available())return;
  job="Doc GPS";
  if(!gnss) {
    String r=modem.command("AT+QGPS?");
    gnss=ModemPort::ok(r)&&tracker::field(resultField(r,"+QGPS:"),0)=="1";
    if(!gnss&&modemReady)gnss=modem.cmdOK("AT+QGPS=1",10000);
  }
  if(!gnss||!modemReady){gps.current=false;gpsStatus="Chua bat duoc GPS";lastGPS=millis();return;}
  String r=modem.command("AT+QGPSLOC=0",5000);
  if(tracker::decode(r,gps,millis())&&modemReady) {
    gpsStatus="Fix "+String(gps.dimension)+"D; "+String(gps.satellites)+" ve tinh";
    event("[GPS PASS] LAT="+String(gps.lat,7)+" LON="+String(gps.lon,7));
  } else {
    gps.current=false;
    if(r.indexOf("+CME ERROR: 516")>=0)gpsStatus="Dang cho fix GPS";
    else if(r.indexOf("+CME ERROR: 505")>=0||r.indexOf("+CME ERROR: 507")>=0) {gnss=false;gpsStatus="GNSS chua hoat dong";}
    else gpsStatus="Chua co vi tri hop le";
  }
  lastGPS=millis();job="San sang";
}
void initializeModem() {
  job="Khoi tao modem";lastInit=millis();
  modemReady=false;gnss=false;tlsReady=false;smsRxReady=false;
  if(!modem.cmdOK("AT",1500)||!modem.cmdOK("ATE0")||!modem.cmdOK("AT+CMEE=1"))return;
  modemReady=true;
  modem.command("ATI");modem.command("AT+CGMR");
  checkNetwork();if(modemReady)checkGPS();
  job="San sang";event("[INIT] Da bat tay UART; GPS/mang se tiep tuc duoc kiem tra.");
}

bool sendSMS(const String &phone,const String &body) {
  job="Gui SMS";smsStatus="Dang chuan bi";
  if(!tracker::phone(phone)||body.length()<1||body.length()>160||!modemReady)return false;
  if(!modem.cmdOK("ATE0")||!modem.cmdOK("AT+CMEE=1"))return false;
  String r=modem.command("AT+CPIN?");
  if(!ModemPort::ok(r)||resultField(r,"+CPIN:")!="READY") {
    smsStatus="SIM chua san sang";event("[SMS FAIL] SIM chua READY.");return false;
  }
  if(!modem.cmdOK("AT+CMGF=1")||!modem.cmdOK("AT+CSCS=\"GSM\"")||
     !modem.cmdOK("AT+CSMP=17,167,0,0"))return false;
  r=modem.command("AT+CMGS=\""+phone+"\"",15000,">");
  if(!r.endsWith(">\n")) {
    modem.cancelSMS();smsStatus="Khong nhan duoc dau >";event("[SMS FAIL] Chua gui noi dung.");return false;
  }
  modem.smsData(body);smsStatus="Da gui Ctrl+Z; dang cho ket qua";
  event("[SMS] Cho +CMGS va OK; toi da 125 giay.");
  r=modem.wait("OK",125000);
  bool submitted=modemReady&&ModemPort::ok(r)&&r.indexOf("+CMGS:")>=0;
  if(submitted) {smsStatus="Da gui len mang";event("[SMS SUBMITTED] +CMGS va OK; kiem tra dien thoai nhan.");}
  else {
    String err=resultField(r,"+CMS ERROR:");
    smsStatus=err.length()?"Loi CMS "+err:"Khong xac nhan duoc ket qua";
    event("[SMS FAIL/UNKNOWN] "+smsStatus+"; khong tu gui lai.");
  }
  return submitted;
}
void doSOS(bool test,bool remote=false) {
  sosActive=true;
  if(!remote) {sosPending=false;testPending=false;}
  if(!test)checkGPS();
  String body=test?"Test SMS XIAO GPS. "+tracker::sosBody(gps,millis()):tracker::sosBody(gps,millis());
  if(remote)body="Vi tri theo yeu cau. "+
    (gps.known?tracker::sosBody(gps,millis()).substring(5):String("Chua co vi tri GPS."));
  // Freeze recipients: saving the portal cannot alter an event in progress.
  tracker::Config recipients=cfg;
  int total=0,submitted=0;
  for(int i=0;i<(test?1:3);++i) {
    if(!recipients.phones[i][0])continue;
    bool duplicate=false;
    for(int j=0;j<i;++j)if(remote_sms::samePhone(recipients.phones[i],recipients.phones[j]))duplicate=true;
    if(duplicate)continue;
    ++total;
    if(!modemReady||!modem.available()) {event("[SOS FAIL] Modem chua san sang.");continue;}
    if(sendSMS(recipients.phones[i],body))++submitted;
  }
  smsStatus="Da gui len mang "+String(submitted)+"/"+String(total)+" so";
  event(String(remote?"[SMS GPS RESULT] ":"[SOS RESULT] ")+smsStatus+". Khong tu gui lai.");
  sosActive=false;job="San sang";
}

void setupSMSInbox() {
  lastRxSetup=millis();job="Bat nhan SMS";
  // Receive in modem memory (Quectel default); continue checking old SIM SMS.
  smsReceiveStore="ME";
  bool storage=modem.cmdOK("AT+CPMS=\"ME\",\"ME\",\"ME\"",10000);
  if(!storage&&modemReady&&modem.available()) {
    smsReceiveStore="SM";
    storage=modem.cmdOK("AT+CPMS=\"SM\",\"SM\",\"SM\"",10000);
  }
  if(storage&&!modem.cmdOK("AT+QURCCFG=\"urcport\",\"uart1\""))
    event("[SMS RX NOTE] Chua dat duoc URC UART; van quet hop thu dinh ky.");
  smsRxReady=storage&&modem.available()&&modemReady&&
    modem.cmdOK("AT+CNMI=2,1,0,0,0",5000)&&modemReady;
  if(smsRxReady) {
    smsInboxDirty=true;lastInboxScan=millis()-30000;
    event("[SMS RX READY] Nhan vao "+smsReceiveStore+"; quet ca SM va ME.");
  } else event("[SMS RX FAIL] Chua bat duoc hop thu; se thu lai sau 30s.");
  job="San sang";
}

// A received command is removed only after re-reading the same PDU at the same
// slot. Delete acknowledgement precedes sending, so it cannot trigger endlessly
// after resets. Other inbox messages are never deleted by this feature.
void pollSMSInbox(bool diagnostic=false) {
  lastInboxScan=millis();smsInboxDirty=false;job="Doc SMS den";
  if(diagnostic) {
    Serial.println("[SMS DIAG BEGIN] Doc SM + ME; khong xoa/tra loi trong che do nay.");
    modem.command("AT+CPMS?");modem.command("AT+CNMI?");
    modem.command("AT+QURCCFG=\"urcport\"");
  }
  if(!modem.cmdOK("AT+CMGF=0"))return;
  if(diagnostic)modem.command("AT+CMGF?");
  bool valid=false;String handledStore;
  const char *stores[]={"SM","ME"};
  for(const char *store:stores) {
    if(!modemReady||!modem.available())break;
    // Only mem1 changes here. mem2/mem3 retain the receiving configuration.
    String r=modem.command("AT+CPMS=\""+String(store)+"\"",10000);
    if(!ModemPort::ok(r)) {
      Serial.println("[SMS STORE] "+String(store)+" unavailable; khong doc/xoa bang chi so cua bo nho truoc.");
      continue;
    }
    Serial.println("[SMS STORE] "+String(store)+" used,total="+resultField(r,"+CPMS:"));
    inboxScan.begin(remoteSenderAllowed,diagnostic);
    Serial.println("[SMS RX] Doc AT+CMGL=4 tu "+String(store)+"; noi dung SMS duoc an.");
    r=modem.command("AT+CMGL=4",10000,"OK",true);
    inboxScan.end();
    Serial.println("[SMS RX SCAN] store="+String(store)+" result="+(ModemPort::ok(r)?"OK ":"FAIL ")+inboxScan.summary());
    int index=inboxScan.index;String pdu=inboxScan.pdu;
    valid=!diagnostic&&modemReady&&ModemPort::ok(r)&&index>=0;
    if(!valid)continue;
    r=modem.command("AT+CMGR="+String(index),5000,"OK",true);
    int header=r.indexOf("+CMGR:");
    int start=header<0?-1:r.indexOf('\n',header);
    int end=start<0?-1:r.indexOf('\n',start+1);
    String readPdu=start<0||end<0?String(""):r.substring(start+1,end);
    String sender;
    valid=modemReady&&ModemPort::ok(r)&&readPdu==pdu&&
      remote_sms::decodeSOS(readPdu,sender)&&remoteSenderAllowed(sender);
    if(!valid)event("[SMS RX FAIL] Doc lai CMGR khong khop/khong hop le; giu nguyen tin.");
    if(valid) {
      valid=modem.cmdOK("AT+CMGD="+String(index)+",0",5000);
      if(!valid)event("[SMS RX FAIL] Chua xac nhan xoa lenh SOS; chua gui tra loi.");
    }
    handledStore=store;
    break; // At most one received command per cycle, including delete failures.
  }
  if(modem.available()&&modemReady)modem.command("AT+CPMS=\""+smsReceiveStore+"\"",10000);
  // Restore the existing SMS sender's text mode before any other modem job.
  bool restored=modem.available()&&modemReady&&modem.cmdOK("AT+CMGF=1");
  job="San sang";
  if(diagnostic) {
    Serial.println(String("[SMS DIAG END] text_mode=")+(restored?"OK":"FAIL")+". Go :run de tiep tuc tu dong xu ly SMS/cloud.");
    return;
  }
  if(!valid)return;
  hadRemoteReply=true;lastRemoteReply=millis();
  if(!restored) {event("[SMS RX FAIL] Lenh da xu ly nhung modem loi; gui lai SOS de thu moi.");return;}
  event("[SMS RX SOS] store="+handledStore+"; gui vi tri den tat ca so SOS trong cau hinh.");
  doSOS(false,true);
  lastRemoteReply=millis();
}

bool prepareData() {
  job="Ket noi Internet 4G";
  String r=modem.command("AT+QIACT?");
  if(!ModemPort::ok(r))return false;
  bool active=r.indexOf("+QIACT: 1,1,")>=0;
  if(pdpDirty) {
    if(active&&!modem.cmdOK("AT+QIDEACT=1",45000))return false;
    String auth=cfg.user[0]?"1":"0";
    String command="AT+QICSGP=1,1,\""+String(cfg.apn)+"\",\""+String(cfg.user)+"\",\""+String(cfg.pass)+"\","+auth;
    if(!modem.cmdOK(command,10000,true))return false;
    pdpDirty=false;active=false;
  }
  if(sosPending)return false;
  if(!active&&!modem.cmdOK("AT+QIACT=1",150000))return false;
  if(!modemReady||sosPending)return false;
  r=modem.command("AT+QIACT?");
  return ModemPort::ok(r)&&r.indexOf("+QIACT: 1,1,")>=0;
}
bool prepareTLS() {
  job="Chuan bi HTTPS";
  if(!tlsReady) {
    String r=modem.command(String("AT+QFLST=\"")+CA_FILE+"\"");
    bool exists=r.indexOf(String("\"")+CA_FILE+"\","+String(strlen(CLOUD_CA)))>=0;
    if(!exists) {
      // Only our own certificate file is replaced. No user files are erased.
      modem.command(String("AT+QFDEL=\"")+CA_FILE+"\"");
      r=modem.command(String("AT+QFUPL=\"")+CA_FILE+"\","+String(strlen(CLOUD_CA))+",10",5000,"CONNECT");
      if(!r.endsWith("CONNECT\n")||!modem.rawOK(CLOUD_CA,strlen(CLOUD_CA),15000))return false;
    }
    if(!modem.cmdOK("AT+QHTTPCFG=\"contextid\",1")||
       !modem.cmdOK("AT+QHTTPCFG=\"sslctxid\",1")||
       !modem.cmdOK("AT+QHTTPCFG=\"requestheader\",1")||
       !modem.cmdOK("AT+QHTTPCFG=\"responseheader\",0")||
       !modem.cmdOK("AT+QHTTPCFG=\"rspout/auto\",0")||
       !modem.cmdOK("AT+QSSLCFG=\"sslversion\",1,3")||
       !modem.cmdOK("AT+QSSLCFG=\"ciphersuite\",1,0xFFFF")||
       !modem.cmdOK("AT+QSSLCFG=\"sni\",1,1")||
       !modem.cmdOK("AT+QSSLCFG=\"seclevel\",1,1")||
       !modem.cmdOK(String("AT+QSSLCFG=\"cacert\",1,\"")+CA_FILE+"\"")||
       !modem.cmdOK("AT+QSSLCFG=\"ignorelocaltime\",1,0")||
       !modem.cmdOK("AT+QSSLCFG=\"negotiatetime\",1,30"))return false;
    tlsReady=true;
  }
  return modemReady;
}
void cloudFailure(const String &reason) {
  cloudStatus=reason;event("[CLOUD FAIL] "+reason);++cloudFailures;
  uint32_t backoff=60000UL<<min(cloudFailures-1,3u);
  cloudWaitMs=max<uint32_t>(cfg.intervalSec*1000,backoff);cloudAttemptAt=millis();
}
void uploadCloud() {
  uploadPending=false;
  if(!cloudConfigured()) {cloudStatus="Chua bat cloud/nhap key/Device ID";cloudWaitMs=60000;cloudAttemptAt=millis();return;}
  if(cloudAuthBlocked) {cloudStatus="Key bi tu choi; luu lai cau hinh de thu tiep";return;}
  if(!modemReady||!simReady||!registered) {cloudFailure("SIM/mang chua san sang");return;}
  uint32_t boot=modem.boots;
  cloudAttemptAt=millis();cloudStatus="Dang ket noi";
  if(!prepareData()) {cloudFailure(sosPending?"Tam hoan de xu ly SOS":"Chua ket noi duoc PDP; kiem tra APN/data SIM");return;}
  if(sosPending)return;
  if(!prepareTLS()) {cloudFailure("Khong cau hinh duoc HTTPS; xem lenh loi goc");return;}
  if(sosPending)return;
  // PDP activation may take minutes. Always fetch a new real fix afterwards.
  checkGPS();
  if(!gps.fresh(millis())||!gps.clockValid) {cloudFailure("Dang cho GPS va thoi gian UTC hop le");return;}
  String clock="AT+CCLK=\""+gps.date.substring(4,6)+"/"+gps.date.substring(2,4)+"/"+
    gps.date.substring(0,2)+","+gps.utc.substring(0,2)+":"+gps.utc.substring(2,4)+":"+gps.utc.substring(4,6)+"+00\"";
  if(!modem.cmdOK(clock)) {cloudFailure("Khong dat duoc gio UTC cho TLS");return;}
  if(sosPending)return;
  String body=tracker::cloudBody(cfg,gps,battery),stamp=gps.stamp;
  String request="POST "+String(CLOUD_PATH)+" HTTP/1.1\r\nHost: "+CLOUD_HOST+
    "\r\nAuthorization: Bearer "+String(cfg.key)+"\r\nContent-Type: application/json\r\nContent-Length: "+
    String(body.length())+"\r\nConnection: close\r\n\r\n"+body;
  job="Gui GPS len cloud";
  String r=modem.command("AT+QHTTPURL="+String(strlen(CLOUD_URL))+",10",5000,"CONNECT");
  if(!r.endsWith("CONNECT\n")||!modem.rawOK(String(CLOUD_URL))) {cloudFailure("Khong nap duoc URL HTTPS");return;}
  if(sosPending)return;
  modem.httpResult="";
  r=modem.command("AT+QHTTPPOST="+String(request.length())+",10,30",125000,"CONNECT");
  if(!r.endsWith("CONNECT\n")) {cloudFailure("Khong ket noi duoc HTTPS; xem loi goc");return;}
  // Exactly the advertised byte count. No println/newline/Ctrl+Z in HTTP data.
  if(!modem.rawOK(request,15000)) {cloudFailure("Chua xac nhan nhan du du lieu POST");return;}
  if(!modem.httpResult.length())modem.wait("+QHTTPPOST:",35000,false,true);
  r=modem.httpResult;
  int err=-1,http=-1,length=0;
  sscanf(resultField(r,"+QHTTPPOST:").c_str(),"%d,%d,%d",&err,&http,&length);
  if(modem.boots!=boot||!modemReady) {cloudFailure("Modem khoi dong lai giua giao dich");return;}
  if(err==0&&http==200) {
    cloudStatus="HTTP 200 · "+stamp+" UTC";lastCloudStamp=stamp;cloudFailures=0;
    cloudWaitMs=cfg.intervalSec*1000UL;cloudAttemptAt=millis();event("[CLOUD PASS] "+cloudStatus);
  } else {
    if(http==401||http==403)cloudAuthBlocked=true;
    cloudFailure("Quectel="+String(err)+"; HTTP="+String(http));
    if(http==429)cloudWaitMs=max<uint32_t>(cloudWaitMs,300000);
  }
  // Release buffered response/session before the next transaction. The status
  // code above is logged even if reading the response subsequently fails.
  if(modem.available()&&length>0) {
    modem.command("AT+QHTTPREAD=10",15000,"+QHTTPREAD: 0",true);
  }
  job="San sang";
}

void applyConfig() {
  bool dataChanged=String(cfg.apn)!=staged.apn||String(cfg.user)!=staged.user||String(cfg.pass)!=staged.pass;
  cfg=staged;applyPending=false;cloudAuthBlocked=false;cloudFailures=0;cloudWaitMs=0;
  if(dataChanged)pdpDirty=true;
  cloudStatus=cfg.cloudEnabled?"Da luu; cho GPS/mang":"Da tat gui cloud";
  event("[CONFIG] Da ap dung cau hinh; khong in API key/mat khau.");
}
void serialCommand(String command) {
  command.trim();
  if(command==":config")portalRequested=true;
  else if(command==":sos")queueSOS();
  else if(command==":test")testPending=true;
  else if(command==":upload")uploadPending=true;
  else if(command==":pause") {paused=true;event("[AUTO] PAUSE; nut SOS/CONFIG van hoat dong.");}
  else if(command==":run") {paused=false;smsDiagHold=false;event("[AUTO] RUN");}
  else if(command==":smsdiag") {
    smsDiagRequested=true;smsDiagHold=true;paused=true;
    event("[SMS DIAG] Da xep lich doc hop thu. Tam dung tu dong tra loi/cloud; nut SOS van hoat dong.");
  }
  else if(command==":init")initRequested=true;
  else if(command==":status") {
    Serial.println("[FW] 3.1.3-IoT-EG800K");Serial.println("[JOB] "+job);
    Serial.println("[GPS] "+gpsStatus);Serial.println("[MAP] "+maps());
    Serial.println("[CLOUD] "+cloudStatus);Serial.println("[SMS] "+smsStatus);
    Serial.println(String("[SMS RX] ")+(smsRxReady?"San sang; lenh SOS":"Dang cho bat hop thu"));
    Serial.printf("[PCB] Pin ~ %.3f V; RDY=%lu; CONFIG=%d SOS=%d\n",battery,(unsigned long)modem.boots,digitalRead(CONFIG_PIN),digitalRead(SOS_PIN));
  } else event("Lenh: :config :status :sos :test :upload :pause :run :init :smsdiag");
}
void setup() {
  pinMode(CONFIG_PIN,INPUT_PULLUP);pinMode(SOS_PIN,INPUT_PULLUP);pinMode(BATTERY_PIN,INPUT);
  analogReadResolution(12);analogSetPinAttenuation(BATTERY_PIN,ADC_11db);
  Serial.begin(115200);uint32_t start=millis();while(!Serial&&uint32_t(millis()-start)<3000)delay(10);
  Serial.println("\n[FW] 3.1.3-IoT-EG800K");
  prefsOK=prefs.begin("tracker-iot",false);
  if(prefsOK&&prefs.getBytesLength("config")==sizeof(cfg)) {
    tracker::Config loaded;
    if(prefs.getBytes("config",&loaded,sizeof(loaded))==sizeof(loaded)&&tracker::validate(loaded)) {cfg=loaded;saved=true;}
  }
  uint64_t mac=ESP.getEfuseMac();char name[32];snprintf(name,sizeof(name),"XIAO-GPS-%06lX",(unsigned long)(mac&0xFFFFFF));ssid=name;
  setupRoutes();WiFi.mode(WIFI_OFF);updateBattery();
  modem.begin(serviceLocal,modemBoot,onModemLine);
  lastRxSetup=millis()-30000;
  // USB is optional; a missing configuration opens the phone portal immediately.
  if(!saved||digitalRead(CONFIG_PIN)==LOW)startPortal();
  lastInit=millis()-30000;lastNetwork=millis()-NETWORK_EVERY;lastGPS=millis()-GPS_EVERY;
  event("Giu CONFIG 3s: cau hinh Wi-Fi. Giu SOS 3s: gui SMS. Serial :help.");
}
void loop() {
  serviceLocal();modem.pump();
  while(Serial.available()) {
    char c=char(Serial.read());
    if(c=='\r')continue;
    if(c=='\n') {serialCommand(console);console="";}
    else if(console.length()<160)console+=c;
    else console="";
  }
  if(applyPending)applyConfig();
  if(!modem.available()){job="Cho phuc hoi UART";delay(2);return;}
  if(modem.timedOut){modem.timedOut=false;modemReady=false;lastInit=millis()-30000;}
  if(initRequested||(!modemReady&&uint32_t(millis()-lastInit)>=30000)) {
    initRequested=false;initializeModem();delay(1);return;
  }
  if(!modemReady){job="Cho modem san sang";delay(2);return;}
  if(sosPending) {doSOS(false);delay(1);return;}
  if(testPending) {doSOS(true);delay(1);return;}
  if(simReady&&!smsRxReady&&uint32_t(millis()-lastRxSetup)>=30000) {
    setupSMSInbox();delay(1);return;
  }
  if(smsDiagRequested&&smsRxReady) {
    smsDiagRequested=false;pollSMSInbox(true);delay(1);return;
  }
  bool remoteCooldown=!hadRemoteReply||uint32_t(millis()-lastRemoteReply)>=30000;
  uint32_t inboxAge=uint32_t(millis()-lastInboxScan);
  if(!smsDiagHold&&smsRxReady&&remoteCooldown&&(inboxAge>=30000||(smsInboxDirty&&inboxAge>=5000))) {
    pollSMSInbox();delay(1);return;
  }
  if(!paused&&uint32_t(millis()-lastNetwork)>=NETWORK_EVERY) {checkNetwork();delay(1);return;}
  if(!paused&&uint32_t(millis()-lastGPS)>=GPS_EVERY) {checkGPS();delay(1);return;}
  bool due=cloudConfigured()&&!cloudAuthBlocked&&uint32_t(millis()-cloudAttemptAt)>=cloudWaitMs;
  if(uploadPending||(!paused&&due)) {uploadCloud();job="San sang";}
  delay(2);
}


