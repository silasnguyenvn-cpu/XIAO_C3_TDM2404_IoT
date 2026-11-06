#pragma once
#include <stddef.h>

// Pure parser: no UART, GPS state or SMS side effects. Also tested at compile time.
namespace gps_parser {
constexpr size_t MAX_PAYLOAD = 255;
struct Payload {
  bool complete = false;
  char data[MAX_PAYLOAD + 1] = {};
};

constexpr bool space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

constexpr bool starts(const char *text, size_t begin, size_t end, const char *prefix) {
  for (size_t i = 0; prefix[i]; ++i) {
    if (begin + i >= end || text[begin + i] != prefix[i]) return false;
  }
  return true;
}

constexpr bool equals(const char *a, const char *b) {
  size_t i = 0;
  while (a[i] && b[i]) {
    if (a[i] != b[i]) return false;
    ++i;
  }
  return a[i] == b[i];
}

constexpr bool dataChar(char c) {
  return (c >= '0' && c <= '9') || c == '.' || c == ',' || c == '+' ||
         c == '-' || c == 'N' || c == 'S' || c == 'E' || c == 'W' || space(c);
}

// Accept label+CSV, label then CSV, and hard-wrapped CSV, including empty fields.
// A complete response must end in OK and contain the 11 QGPSLOC=0 fields.
constexpr Payload extract(const char *response) {
  Payload result;
  bool foundLabel = false;
  size_t i = 0, used = 0;
  while (response[i]) {
    size_t begin = i;
    while (response[i] && response[i] != '\n') ++i;
    size_t end = i;
    if (response[i] == '\n') ++i;
    while (begin < end && space(response[begin])) ++begin;
    while (end > begin && space(response[end - 1])) --end;
    if (begin == end) continue;
    if (starts(response, begin, end, "ERROR") ||
        starts(response, begin, end, "+CME ERROR:") ||
        starts(response, begin, end, "+CMS ERROR:")) return result;
    if (end - begin == 2 && starts(response, begin, end, "OK")) {
      if (foundLabel && used) {
        size_t commas = 0;
        for (size_t j = 0; j < used; ++j) if (result.data[j] == ',') ++commas;
        result.complete = commas == 10;
      }
      return result;
    }
    if (starts(response, begin, end, "+QGPSLOC:")) {
      if (foundLabel) return result; // Do not merge two different position records.
      foundLabel = true;
      begin += 9;
      while (begin < end && space(response[begin])) ++begin;
    } else if (!foundLabel) continue;
    if (begin == end) continue;
    bool isData = true;
    for (size_t j = begin; j < end; ++j) if (!dataChar(response[j])) isData = false;
    if (!isData) {
      if (response[begin] == '+') continue; // Ignore unrelated one-line URCs.
      return result;
    }
    for (size_t j = begin; j < end; ++j) {
      if (used == MAX_PAYLOAD) return result;
      result.data[used++] = response[j];
    }
    result.data[used] = '\0';
  }
  return result; // No final OK: do not accept a partial response.
}

// ddmm.mmmmN / dddmm.mmmmE to decimal degrees; signs come from N/S/E/W.
constexpr bool toDegrees(const char *value, bool latitude, double &degreesOut) {
  size_t begin = 0, end = 0;
  while (value[end]) ++end;
  while (begin < end && space(value[begin])) ++begin;
  while (end > begin && space(value[end - 1])) --end;
  if (end - begin < 2) return false;
  char hemisphere = value[--end];
  if (latitude ? (hemisphere != 'N' && hemisphere != 'S')
               : (hemisphere != 'E' && hemisphere != 'W')) return false;
  while (end > begin && space(value[end - 1])) --end;
  double raw = 0, fraction = 0.1;
  bool dot = false, haveDigit = false;
  size_t wholeDigits = 0;
  for (size_t j = begin; j < end; ++j) {
    char c = value[j];
    if (c == '.' && !dot) { dot = true; continue; }
    if (c < '0' || c > '9') return false;
    haveDigit = true;
    if (!dot) { raw = raw * 10 + (c - '0'); ++wholeDigits; }
    else { raw += (c - '0') * fraction; fraction *= 0.1; }
  }
  if (!haveDigit || wholeDigits != (latitude ? 4u : 5u) ||
      raw > (latitude ? 9000.0 : 18000.0)) return false;
  int wholeDegrees = int(raw / 100.0);
  double minutes = raw - wholeDegrees * 100.0;
  if (minutes < 0 || minutes >= 60) return false;
  double decimal = wholeDegrees + minutes / 60.0;
  if (decimal > (latitude ? 90.0 : 180.0)) return false;
  degreesOut = (hemisphere == 'S' || hemisphere == 'W') ? -decimal : decimal;
  return true;
}
} // namespace gps_parser
