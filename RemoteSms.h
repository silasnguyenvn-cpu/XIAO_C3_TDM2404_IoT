#pragma once
#include <Arduino.h>

namespace remote_sms {
inline String canonical(String number) {
  number.trim();
  if(number.startsWith("+"))number=number.substring(1);
  else if(number.startsWith("00"))number=number.substring(2);
  if(number.length()==10&&number[0]=='0')number="84"+number.substring(1);
  if(number.length()<7||number.length()>15)return "";
  for(char c:number)if(c<'0'||c>'9')return "";
  return number;
}
inline bool samePhone(const String &a,const String &b) {
  String normalized=canonical(a);
  return normalized.length()&&normalized==canonical(b);
}
inline int hex(char c) {
  if(c>='0'&&c<='9')return c-'0';
  if(c>='A'&&c<='F')return c-'A'+10;
  if(c>='a'&&c<='f')return c-'a'+10;
  return -1;
}
// Parse SMS-DELIVER PDU, not text-mode lines: a message body can never be
// mistaken for OK/RDY/+CMTI or another AT response. Only a single short SOS
// command is supported. Account for UDH fill bits; never execute one fragment
// of a multipart message as if it were a complete command.
struct DecodeInfo {
  String sender;
  const char *reason="UNKNOWN";
  int toa=-1,pid=-1,dcs=-1;
  bool udh=false;
  int headerBytes=0,parts=1,part=1,reference=-1,ie=-1,bodyChars=-1;
};
inline bool decodeSOS(const String &pdu,String &senderOut,DecodeInfo *details=nullptr) {
  DecodeInfo info;
  auto fail=[&](const char *reason) {info.reason=reason;if(details)*details=info;return false;};
  senderOut="";
  uint8_t bytes[256];size_t n=pdu.length()/2;
  if(!n||pdu.length()%2||n>sizeof(bytes))return fail("PDU_LENGTH");
  for(size_t i=0;i<n;++i) {
    int a=hex(pdu[i*2]),b=hex(pdu[i*2+1]);
    if(a<0||b<0)return fail("NOT_HEX_PDU");
    bytes[i]=uint8_t(a*16+b);
  }
  size_t at=1+bytes[0];
  if(at+3>n)return fail("SMSC_LENGTH");
  uint8_t first=bytes[at++],digits=bytes[at++],type=bytes[at++];
  info.toa=type;info.udh=(first&0x40)!=0;
  if((first&3)!=0)return fail("NOT_DELIVER");
  if(!digits||digits>15)return fail("ADDRESS_LENGTH");
  if(type!=0x81&&type!=0x91&&type!=0xA1)return fail("ADDRESS_TYPE");
  size_t addressBytes=(digits+1)/2;
  if(at+addressBytes+10>n)return fail("TRUNCATED_HEADER");
  String sender=type==0x91?"+":"";
  for(unsigned i=0;i<digits;++i) {
    unsigned digit=(bytes[at+i/2]>>(i%2?4:0))&15;
    if(digit>9)return fail("ADDRESS_DIGIT");
    sender+=char('0'+digit);
  }
  if(digits%2 && (bytes[at+addressBytes-1]>>4)!=15)return fail("ADDRESS_PADDING");
  info.sender=sender;
  at+=addressBytes;
  uint8_t pid=bytes[at++],dcs=bytes[at++];
  info.pid=pid;info.dcs=dcs;
  if(pid!=0)return fail("PID_UNSUPPORTED");
  if((dcs&0xE0)!=0 || ((dcs>>2)&3)==1 || ((dcs>>2)&3)==3)return fail("DCS_UNSUPPORTED");
  at+=7; // SMSC timestamp; position comes from live GNSS, never SMS text.
  unsigned length=bytes[at++];
  bool unicode=((dcs>>2)&3)==2;
  unsigned octets=unicode?length:(length*7+7)/8;
  if(at+octets!=n || !length || octets>140)return fail("USER_DATA_LENGTH");
  unsigned headerBytes=0;
  bool concatSeen=false,lockingSeen=false,singleSeen=false;
  const char *unsupported=nullptr;
  if(info.udh) {
    headerBytes=unsigned(bytes[at])+1;info.headerBytes=headerBytes;
    if(headerBytes>octets)return fail("UDH_LENGTH");
    unsigned pos=1;
    while(pos<headerBytes) {
      if(pos+2>headerBytes)return fail("UDH_IE_LENGTH");
      unsigned ie=bytes[at+pos++],size=bytes[at+pos++];
      if(pos+size>headerBytes)return fail("UDH_IE_LENGTH");
      if(ie==0x00||ie==0x08) {
        if(concatSeen||size!=(ie==0?3u:4u))return fail("UDH_CONCAT_INVALID");
        concatSeen=true;
        unsigned field=pos;
        info.reference=bytes[at+field++];
        if(ie==8)info.reference=(info.reference<<8)|bytes[at+field++];
        info.parts=bytes[at+field++];info.part=bytes[at+field];
        if(!info.parts||!info.part||info.part>info.parts)return fail("UDH_CONCAT_INVALID");
      } else if(ie==0x24||ie==0x25) {
        bool &seen=ie==0x24?singleSeen:lockingSeen;
        if(size!=1||seen)return fail("UDH_LANGUAGE_INVALID");
        seen=true;
        // SOS uses no GSM escape characters. A single-shift table cannot
        // change these characters; a non-default locking table can.
        if(!unicode&&((ie==0x25&&bytes[at+pos]!=0)||(ie==0x24&&bytes[at+pos]>13))) {
          unsupported="UDH_LANGUAGE_UNSUPPORTED";info.ie=ie;
        }
      } else if(ie!=0x26) { // Filler has no effect on user text.
        unsupported="UDH_IE_UNSUPPORTED";info.ie=ie;
      }
      pos+=size;
    }
  }
  unsigned headerSeptets=(headerBytes*8+6)/7;
  if((unicode&&(length<headerBytes||(length-headerBytes)%2))||
     (!unicode&&length<headerSeptets))return fail("USER_DATA_LENGTH");
  unsigned chars=unicode?(length-headerBytes)/2:length-headerSeptets;
  info.bodyChars=chars;
  if(unsupported)return fail(unsupported);
  if(info.parts>1)return fail("MULTIPART_NOT_COMMAND");
  if(!chars||chars>80)return fail("COMMAND_LENGTH");
  String command;
  for(unsigned i=0;i<chars;++i) {
    unsigned c;
    if(unicode)c=(unsigned(bytes[at+headerBytes+2*i])<<8)|bytes[at+headerBytes+2*i+1];
    else {
      unsigned bit=(headerSeptets+i)*7,pos=bit/8,shift=bit%8;
      c=bytes[at+pos]>>shift;
      if(shift>1 && pos+1<octets)c|=unsigned(bytes[at+pos+1])<<(8-shift);
      c&=127;
    }
    // Only the GSM/Unicode code points needed by SOS and surrounding whitespace.
    if(c!='S'&&c!='O'&&c!='s'&&c!='o'&&c!=' '&&c!='\r'&&c!='\n')return fail("NOT_SOS");
    if(c=='s')c='S';
    if(c=='o')c='O';
    command+=char(c);
  }
  command.trim();
  if(command!="SOS")return fail("NOT_SOS");
  info.reason="SOS";if(details)*details=info;
  senderOut=sender;return true;
}

inline bool headerNumber(const String &s,unsigned &at,unsigned &number) {
  while(s[at]==' ')++at;
  unsigned start=at;number=0;
  while(s[at]>='0'&&s[at]<='9') {
    number=number*10+unsigned(s[at++]-'0');if(number>65535)return false;
  }
  if(at==start)return false;
  while(s[at]==' ')++at;
  if(s[at]!=',')return false;
  ++at;return true;
}

// Stream every CMGL record so a large inbox cannot truncate away the command.
// Keep only one candidate; remaining requests stay in SIM storage for next scan.
class Scan {
  int pending=-1;
  bool (*allowed)(const String &)=nullptr;
  bool verbose=false;
public:
  unsigned records=0,received=0,badHeaders=0,decoded=0,denied=0,other=0,invalid=0,missing=0;
  bool active=false;
  int index=-1;
  String pdu;
  void begin(bool (*authorize)(const String &),bool debug=false) {
    allowed=authorize;active=true;pending=-1;index=-1;pdu="";
    verbose=debug;records=received=badHeaders=decoded=denied=other=invalid=missing=0;
  }
  void end() {if(pending>=0)++missing;active=false;pending=-1;}
  String summary() const {
    return "records="+String(records)+" received="+String(received)+" SOS="+String(decoded)+
      " denied="+String(denied)+" other="+String(other)+" invalid="+String(invalid)+
      " bad_header="+String(badHeaders)+" missing="+String(missing);
  }
  void line(const String &s) {
    if(!active)return;
    if(s.startsWith("+CMTI:"))return;
    if(s.startsWith("+CMGL:")) {
      ++records;
      if(pending>=0)++missing;
      unsigned id=0,status=99,at=6;
      pending=-1;
      if(!headerNumber(s,at,id)||!headerNumber(s,at,status)) {
        ++badHeaders;
        if(verbose)Serial.println("[SMS DIAG] HEADER_FORMAT: expected numeric index,status in PDU mode");
      } else if(status<=1) {pending=int(id);++received;}
      return;
    }
    if(pending>=0) {
      String sender;DecodeInfo info;
      bool isSOS=decodeSOS(s,sender,&info);
      bool authorized=isSOS&&allowed&&allowed(sender);
      if(isSOS) {++decoded;if(!authorized)++denied;}
      else if(String(info.reason)=="NOT_SOS")++other;
      else ++invalid;
      if(verbose) {
        String masked=info.sender.length()>4?"***"+info.sender.substring(info.sender.length()-4):"unknown";
        Serial.println("[SMS DIAG] index="+String(pending)+" from="+masked+" reason="+info.reason+
          " allowed="+(authorized?"YES":"NO")+" toa="+String(info.toa)+" pid="+String(info.pid)+" dcs="+String(info.dcs)+
          " sender_ok="+(info.sender.length()&&allowed&&allowed(info.sender)?"YES":"NO")+
          " udh="+String(info.udh?1:0)+" header="+String(info.headerBytes)+
          " part="+String(info.part)+"/"+String(info.parts)+" ref="+String(info.reference)+
          " ie="+String(info.ie)+" chars="+String(info.bodyChars));
      }
      if(authorized&&index<0) {index=pending;pdu=s;}
      pending=-1;
    }
  }
};
} // namespace remote_sms
