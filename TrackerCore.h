#pragma once
#include <Arduino.h>
#include <math.h>
#include "GpsParser.h"

namespace tracker {
constexpr uint32_t CONFIG_MAGIC = 0x54444D33;
constexpr uint32_t FIX_FRESH_MS = 60000;
struct Config {
  uint32_t magic = CONFIG_MAGIC;
  char key[161] = "";
  char device[65] = "";
  char apn[64] = "v-internet";
  char user[64] = "";
  char pass[64] = "";
  char phones[3][18] = {"", "", ""};
  char wifiPass[64] = "gps123456";
  uint32_t intervalSec = 60;
  bool cloudEnabled = true;
};
inline String field(const String &csv, int index) {
  int begin=0;
  for(int i=0;i<index;++i) {
    int comma=csv.indexOf(',',begin); if(comma<0) return ""; begin=comma+1;
  }
  int end=csv.indexOf(',',begin);
  String out=csv.substring(begin,end<0?csv.length():end); out.trim(); return out;
}
inline bool chars(const String &s, size_t min, size_t max, const char *extras) {
  if(s.length()<min || s.length()>max) return false;
  for(char c:s) if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||
                     (c>='0'&&c<='9')||strchr(extras,c))) return false;
  return true;
}
inline bool phone(const String &s) {
  if(s.length()<7 || s.length()>16) return false;
  unsigned begin=s[0]=='+'?1:0;
  for(unsigned i=begin;i<s.length();++i) if(s[i]<'0'||s[i]>'9') return false;
  return s.length()-begin<=15;
}
inline bool atText(const String &s, size_t max) {
  if(s.length()>max) return false;
  for(char c:s) if(c<32||c>126||c=='"'||c=='\\') return false;
  return true;
}
inline bool validate(const Config &c) {
  // Check termination before creating Strings from a flash record.
  if(c.magic!=CONFIG_MAGIC || !memchr(c.key,0,sizeof(c.key)) ||
     !memchr(c.device,0,sizeof(c.device)) || !memchr(c.apn,0,sizeof(c.apn)) ||
     !memchr(c.user,0,sizeof(c.user)) || !memchr(c.pass,0,sizeof(c.pass)) ||
     !memchr(c.wifiPass,0,sizeof(c.wifiPass))) return false;
  for(auto &p:c.phones) if(!memchr(p,0,sizeof(p))) return false;
  if(!chars(c.key,0,160,"_.-+/=") || !chars(c.device,0,64,"_.-") ||
     !chars(c.apn,1,63,".-") || !atText(c.user,63) || !atText(c.pass,63) ||
     !atText(c.wifiPass,63) || strlen(c.wifiPass)<8 ||
     c.intervalSec<30 || c.intervalSec>3600 || !phone(c.phones[0])) return false;
  for(int i=1;i<3;++i) if(c.phones[i][0] && !phone(c.phones[i])) return false;
  return !c.cloudEnabled || (c.key[0] && c.device[0]);
}
constexpr int digit(char c) { return c>='0'&&c<='9'?c-'0':-1000; }
constexpr int pair(const char *s,int i) { return 10*digit(s[i])+digit(s[i+1]); }
constexpr bool leap(int y) { return y%4==0 && (y%100!=0 || y%400==0); }
constexpr bool dateTime(const char *date,const char *utc) {
  size_t d=0,t=0; while(date[d])++d; while(utc[t])++t;
  if(d!=6 || t<6) return false;
  for(size_t i=0;i<6;++i) if(digit(date[i])<0 || digit(utc[i])<0) return false;
  if(t>6) {
    if(utc[6]!='.'||t==7) return false;
    for(size_t i=7;i<t;++i) if(digit(utc[i])<0) return false;
  }
  int day=pair(date,0),month=pair(date,2),year=2000+pair(date,4);
  if(month<1||month>12 || year<2024 || pair(utc,0)>23 ||
     pair(utc,2)>59 || pair(utc,4)>59) return false;
  int days=month==2?(leap(year)?29:28):
    (month==4||month==6||month==9||month==11?30:31);
  return day>=1&&day<=days;
}
inline bool numeric(const String &s, double &out) {
  if(!s.length()) return false;
  char *end=nullptr; out=strtod(s.c_str(),&end);
  return end!=s.c_str() && *end==0 && isfinite(out);
}
struct Fix {
  bool current=false, known=false, clockValid=false;
  double lat=0,lon=0,altitude=NAN,speed=NAN,hdop=NAN;
  int satellites=0,dimension=0;
  uint32_t acquired=0;
  String utc,date,stamp;
  bool fresh(uint32_t now) const { return current&&known&&uint32_t(now-acquired)<=FIX_FRESH_MS; }
};
inline bool decode(const String &response, Fix &fix, uint32_t now) {
  auto parsed=gps_parser::extract(response.c_str());
  if(!parsed.complete) {fix.current=false;return false;}
  String csv=parsed.data;
  String dimension=field(csv,5); double lat=0,lon=0;
  if((dimension!="2"&&dimension!="3") ||
     !gps_parser::toDegrees(field(csv,1).c_str(),true,lat)||
     !gps_parser::toDegrees(field(csv,2).c_str(),false,lon)) {
    fix.current=false; return false;
  }
  fix.current=fix.known=true; fix.lat=lat;fix.lon=lon; fix.acquired=now;
  fix.dimension=dimension.toInt();
  if(!numeric(field(csv,4),fix.altitude)) fix.altitude=NAN;
  if(!numeric(field(csv,7),fix.speed)||fix.speed<0) fix.speed=NAN;
  if(!numeric(field(csv,3),fix.hdop)||fix.hdop<0) fix.hdop=NAN;
  double sats=0; fix.satellites=numeric(field(csv,10),sats)&&sats>=0&&sats<=255?int(sats):0;
  fix.utc=field(csv,0);fix.date=field(csv,9);
  fix.clockValid=dateTime(fix.date.c_str(),fix.utc.c_str()); fix.stamp="";
  if(fix.clockValid) {
    fix.stamp="20"+fix.date.substring(4,6)+"-"+fix.date.substring(2,4)+"-"+
      fix.date.substring(0,2)+" "+fix.utc.substring(0,2)+":"+
      fix.utc.substring(2,4)+":"+fix.utc.substring(4,6);
  }
  return true;
}
inline String jsonString(const String &s) {
  String out="\"";
  for(unsigned char c:s) {
    if(c=='"'||c=='\\') {out+='\\';out+=char(c);}
    else if(c<32) { char escaped[7];snprintf(escaped,sizeof(escaped),"\\u%04x",c);out+=escaped; }
    else out+=char(c);
  }
  return out+'"';
}
inline String cloudBody(const Config &c,const Fix &f,float battery) {
  String body="{\"device_id\":"+jsonString(c.device)+",\"timestamp\":["+jsonString(f.stamp)+
    "],\"lat\":["+String(f.lat,7)+"],\"long\":["+String(f.lon,7)+"]";
  if(isfinite(f.speed)) body+=",\"speed\":["+String(f.speed,2)+"]";
  if(isfinite(f.altitude)) body+=",\"altitude\":["+String(f.altitude,1)+"]";
  body+=",\"satellites\":["+String(f.satellites)+"]";
  if(isfinite(battery)) body+=",\"battery\":["+String(battery,3)+"]";
  if(isfinite(f.hdop)) body+=",\"payload\":[{\"hdop\":"+String(f.hdop,2)+"}]";
  return body+"}";
}
inline String sosBody(const Fix &f,uint32_t now) {
  if(f.fresh(now)) return "SOS! GPS: https://www.google.com/maps?q="+String(f.lat,7)+","+String(f.lon,7);
  if(f.known) {
    uint32_t age=uint32_t(now-f.acquired)/1000;
    return "SOS! LAST KNOWN ("+String(age)+"s ago): https://www.google.com/maps?q="+
      String(f.lat,7)+","+String(f.lon,7);
  }
  return "SOS! Can giup do. Chua co vi tri GPS.";
}
// One hold -> one event, rearm only after release; debounce included.
class HoldButton {
  bool raw=false,stable=false,fired=false;
  uint32_t edge=0,pressed=0;
public:
  bool update(bool down,uint32_t now) {
    if(down!=raw) {raw=down;edge=now;}
    if(raw!=stable && uint32_t(now-edge)>=35) {
      stable=raw; if(stable) pressed=now; else fired=false;
    }
    if(stable&&!fired&&uint32_t(now-pressed)>=3000) {fired=true;return true;}
    return false;
  }
};
static_assert(dateTime("231026","053144.00"),"GPS timestamp UTC");
static_assert(dateTime("290224","235959.9"),"Leap day");
static_assert(!dateTime("290225","235959.9"),"Invalid leap day");
static_assert(!dateTime("310426","053144"),"Invalid day");
static_assert(!dateTime("231026","246000"),"Invalid time");
static_assert(!dateTime("231026","053144x"),"Invalid clock suffix");
} // namespace tracker
