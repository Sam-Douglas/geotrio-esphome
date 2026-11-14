#pragma once

// Everything in this file is plain C++ with no ESPHome dependencies, so it can be compiled and
// tested on a PC (see tests/decoder/test_decoder.cpp). The ESPHome side lives in geotrio.h/.cpp.
//
// Protocol notes:
// - The display's Zigbee module talks to its main processor over a 115200 baud UART.
// - Frames start with 0x01 and end with 0x03. A data byte that would clash with 0x01, 0x02 or 0x03
//   is sent as 0x02 followed by the byte XORed with 0x08.
// - Frames we decode start 0x80 0x02. Somewhere after that is the 4-byte tag 01 09 07 xx: 0x0109 is
//   the Zigbee Smart Energy profile and 0x07xx is the cluster (0x0702 metering, 0x0700 price,
//   0x0705 prepayment). The byte after the tag is the endpoint, which says which fuel it is about.
// - Metering and prepayment values are standard Zigbee attribute records:
//   [attribute id, low byte first][status 0x00][data type][value, low byte first].

#include <cstddef>
#include <cstdint>
#include <optional>

namespace esphome {
namespace geotrio {

enum class Fuel : uint8_t { ELECTRICITY = 0, GAS = 1 };
constexpr size_t FUEL_COUNT = 2;

enum class Period : uint8_t { TODAY = 0, THIS_WEEK = 1, THIS_MONTH = 2 };
constexpr size_t PERIOD_COUNT = 3;

const char *fuel_name(Fuel fuel);

// Reassembles frames from the UART byte stream and removes the escaping.
class FrameReader {
 public:
  static constexpr size_t MAX_FRAME_LENGTH = 256;

  // Returns true when this byte completes a frame. The frame stays in frame()/length() until the
  // next call.
  bool add_byte(uint8_t byte);
  const uint8_t *frame() const { return this->buffer_; }
  size_t length() const { return this->length_; }

 private:
  void append(uint8_t byte);

  uint8_t buffer_[MAX_FRAME_LENGTH];
  size_t length_{0};
  bool in_frame_{false};
  bool escape_next_{false};
};

// Everything one frame told us. Fields the frame didn't carry are left empty.
struct FrameContents {
  bool recognised{false};
  Fuel fuel{Fuel::ELECTRICITY};

  // Metering cluster. Raw meter integers: multiply by multiplier / divisor to get kW, kWh or m³.
  std::optional<int32_t> instantaneous_demand;
  std::optional<uint64_t> meter_total;
  // Usage today / this week / this month. Electricity is in raw meter units; gas is in Wh.
  std::optional<uint64_t> usage[PERIOD_COUNT];
  // Only present in the burst the display sends when it starts up.
  std::optional<uint32_t> multiplier;
  std::optional<uint32_t> divisor;

  // Prepayment cluster. Cost today / this week / this month including standing charge, as an
  // integer with an installation-specific number of decimal places (see find_cost_decimal_places).
  std::optional<uint64_t> cost[PERIOD_COUNT];

  // Price cluster. Each value carries its own decimal places, so these are already real numbers.
  std::optional<double> unit_rate;        // GBP per kWh
  std::optional<double> standing_charge;  // GBP per day
  std::optional<double> calorific_value;  // MJ per m³ (gas only)
  std::optional<double> volume_correction;  // dimensionless, typically 1.02264 (gas only)
};

FrameContents decode_frame(const uint8_t *frame, size_t length);

// Frames the display sends constantly that carry nothing we use. Only used to keep logs readable.
bool is_housekeeping_frame(const uint8_t *frame, size_t length);

// The display reports "cost today" as an integer whose number of decimal places varies between
// installations. This finds the number of places (0-7) that makes it roughly equal to
// usage today x unit rate + standing charge. Returns -1 if no number of places is a clear match.
int find_cost_decimal_places(uint64_t raw_cost_today, double usage_today_kwh, double unit_rate,
                             double standing_charge);

// Protects Home Assistant's long-term statistics from the occasional corrupt meter total. A meter
// total should only ever go up, and only by a modest amount between readings. A reading that
// breaks that rule is held back, and only accepted if the next few readings agree with it (which
// happens after a meter exchange, or if the first reading after a restart was itself the bad one).
class MeterTotalFilter {
 public:
  static constexpr double MAX_PLAUSIBLE_TOTAL = 1000000.0;
  static constexpr double MAX_INCREASE_PER_READING = 100.0;
  static constexpr uint8_t READINGS_NEEDED_TO_CONFIRM = 3;

  // Returns true if the reading should be published.
  bool accept(double total);

 private:
  double last_accepted_{-1.0};  // negative means nothing accepted yet
  double candidate_{0.0};
  uint8_t candidate_count_{0};
};

}  // namespace geotrio
}  // namespace esphome
