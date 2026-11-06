#pragma once
#include "GpsParser.h"

// These fixtures are used only by static_assert. Never injected into runtime
// GPS state, the UART, or SMS. A compiler failure prevents an incorrect build.
namespace gps_parser_tests {
constexpr char CSV[] = "124807.00,1230.1234N,04520.5678E,0.89,12.6,3,,0.007,0.004,040924,35";
constexpr auto INLINE = gps_parser::extract("+QGPSLOC: 124807.00,1230.1234N,04520.5678E,0.89,12.6,3,,0.007,0.004,040924,35\r\nOK\r\n");
constexpr auto NEXT_LINE = gps_parser::extract("AT+QGPSLOC=0\r\n+QGPSLOC:\r\n124807.00,1230.1234N,04520.5678E,0.89,12.6,3,,0.007,0.004,040924,35\r\nOK\r\n");
constexpr auto WRAPPED = gps_parser::extract("+QGPSLOC:\n124807.00,1230.1234N,04520.5678E,0.89,12.6,3,,0.007,0.004,0\n40924,35\nOK\n");
constexpr auto WITH_URC = gps_parser::extract("+QGPSLOC:\n+QIND: SMS DONE\n124807.00,1230.1234N,04520.5678E,0.89,12.6,3,,0.007,0.004,040924,35\nOK\n");
static_assert(INLINE.complete && gps_parser::equals(INLINE.data, CSV), "Inline GPS response");
static_assert(NEXT_LINE.complete && gps_parser::equals(NEXT_LINE.data, CSV), "Next-line GPS response");
static_assert(WRAPPED.complete && gps_parser::equals(WRAPPED.data, CSV), "Wrapped GPS date and empty COG");
static_assert(WITH_URC.complete && gps_parser::equals(WITH_URC.data, CSV), "Interleaved URC");
static_assert(!gps_parser::extract("+CME ERROR: 516\n").complete, "No fix must not pass");
static_assert(!gps_parser::extract("+QGPSLOC: 124807.00,1230.1234N,04520.5678E\nOK\n").complete, "Missing fields");
static_assert(!gps_parser::extract("+QGPSLOC: 124807.00,1230.1234N,04520.5678E,0.89,12.6,3,,0.007,0.004,040924,35\n").complete, "Missing final OK");
static_assert(!gps_parser::extract("+QGPSLOC: 124807.00,1230.1234N,04520.5678E,0.89,12.6,3,,0.007,0.004,040924,35\n+CME ERROR: 505\n").complete, "Final error must not pass");
constexpr double decoded(const char *value, bool latitude) {
  double result = 1000;
  return gps_parser::toDegrees(value, latitude, result) ? result : 1000;
}
constexpr bool near(double a, double b) { return a > b - 0.00000001 && a < b + 0.00000001; }
static_assert(near(decoded("1230.1234N", true), 12.502056666667), "Sample latitude");
static_assert(near(decoded("04520.5678E", false), 45.342796666667), "Sample longitude");
static_assert(near(decoded("1230.1234 S", true), -12.502056666667), "Southern latitude");
static_assert(near(decoded("04520.5678W", false), -45.342796666667), "Western longitude");
static_assert(decoded("04560.5678E", false) == 1000, "Invalid minutes");
static_assert(decoded("9100.0000N", true) == 1000, "Invalid latitude range");
static_assert(decoded("1230.1234E", true) == 1000, "Wrong hemisphere");
static_assert(near(decoded("0000.0000N", true), 0), "Equator is valid");
} // namespace gps_parser_tests
