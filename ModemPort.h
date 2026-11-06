#pragma once
#include <Arduino.h>
#include <HardwareSerial.h>

// One owner of UART1. Cooperative waits keep buttons and the phone portal alive.
// onService must NOT issue AT commands. All modem jobs run from the main loop.
class ModemPort {
  HardwareSerial serial{1};
  String pending;
  bool overflow=false,prompt=false;
  bool quarantine=false;
  uint32_t quarantineAt=0;
  void (*onService)()=nullptr;
  void (*onBoot)()=nullptr;
  void (*onLine)(const String &)=nullptr;
  void consume(const String &line,bool quiet) {
    if(line=="RDY") {
      ++boots; httpResult="";
      if(onBoot) onBoot();
      Serial.println("[MODEM] RDY: modem vua khoi dong lai.");
    }
    if(line.startsWith("+QHTTPPOST:")) httpResult=line;
    if(onLine)onLine(line); // Observers only queue work; never send AT here.
    bool privateLine=line.indexOf("Authorization:")>=0||line.startsWith("AT+QICSGP=");
    if(privateLine)Serial.println("<< [thong tin rieng da an]");
    else if(!quiet || line=="ERROR" || line.startsWith("+CMS ERROR:") ||
       line.startsWith("+CME ERROR:")) {Serial.print("<< ");Serial.println(line);}
  }
public:
  uint32_t boots=0;
  String httpResult;
  bool timedOut=false;
  void begin(void (*service)(),void (*boot)(),void (*observer)(const String &)=nullptr) {
    onService=service;onBoot=boot;onLine=observer;serial.setRxBufferSize(4096);
    serial.begin(115200,SERIAL_8N1,20,21);
  }
  bool line(String &out) {
    while(serial.available()) {
      char c=char(serial.read());
      if(prompt&&c=='>'&&pending.length()==0) {out=">";return true;}
      if(c=='\r') continue;
      if(c=='\n') {
        if(overflow) {pending="";overflow=false;continue;}
        out=pending;pending="";out.trim();if(out.length()) return true;
      } else if(pending.length()<1535) pending+=c;
      else overflow=true;
    }
    return false;
  }
  static bool error(const String &line) {
    return line=="ERROR"||line.startsWith("+CME ERROR:")||line.startsWith("+CMS ERROR:");
  }
  static bool ok(const String &response) {
    return response=="OK\n"||response.endsWith("\nOK\n");
  }
  bool available() {
    if(quarantine && uint32_t(millis()-quarantineAt)>=180000) {
      quarantine=false;pending="";overflow=false;
      Serial.println("[UART] Da het thoi gian cho phuc hoi; se bat tay lai.");
    }
    return !quarantine;
  }
  void isolate() {quarantine=true;quarantineAt=millis();}
  void pump() {String s;while(line(s))consume(s,false);}
  String wait(const String &expected,uint32_t timeout,bool quiet=false,bool prefix=false) {
    String response,s;response.reserve(1024);
    uint32_t start=millis(),boot=boots;timedOut=false;
    prompt=expected==">";
    while(uint32_t(millis()-start)<timeout) {
      while(line(s)) {
        consume(s,quiet);
        bool terminal=boots!=boot || error(s) || (prefix?s.startsWith(expected):s==expected);
        if(response.length()+s.length()<8192 || terminal) response+=s+'\n';
        if(terminal) {
          prompt=false;return response;
        }
      }
      if(onService) onService();
      delay(1);
    }
    prompt=false;timedOut=true;
    Serial.println("[UART TIMEOUT] Cho phuc hoi UART toi da 180s; khong chen lenh moi.");
    isolate();return response;
  }
  String command(const String &cmd,uint32_t timeout=3000,const String &expected="OK",bool quiet=false) {
    if(!available()) return "";
    pump();
    Serial.print(">> ");Serial.println(quiet?"[lenh co thong tin rieng]":cmd);
    serial.print(cmd);serial.print('\r');
    return wait(expected,timeout,quiet);
  }
  bool cmdOK(const String &cmd,uint32_t timeout=3000,bool quiet=false) {
    return ok(command(cmd,timeout,"OK",quiet));
  }
  bool rawOK(const String &bytes,uint32_t timeout=12000) {
    serial.write(reinterpret_cast<const uint8_t*>(bytes.c_str()),bytes.length());
    return ok(wait("OK",timeout,true));
  }
  bool rawOK(const char *bytes,size_t length,uint32_t timeout=12000) {
    serial.write(reinterpret_cast<const uint8_t*>(bytes),length);
    return ok(wait("OK",timeout,true));
  }
  void smsData(const String &text) {serial.print(text);serial.write(0x1A);}
  void cancelSMS() {serial.write(0x1B);}
};
