#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

#include "decoder.h"

namespace esphome {
namespace geotrio {

// One entry per sensor the YAML can configure. sensor.py refers to these by name.
enum class SensorId : uint8_t {
  ELECTRICITY_POWER,
  ELECTRICITY_METER,
  ELECTRICITY_USAGE_TODAY,
  ELECTRICITY_USAGE_THIS_WEEK,
  ELECTRICITY_USAGE_THIS_MONTH,
  ELECTRICITY_COST_TODAY,
  ELECTRICITY_COST_THIS_WEEK,
  ELECTRICITY_COST_THIS_MONTH,
  ELECTRICITY_UNIT_RATE,
  ELECTRICITY_STANDING_CHARGE,
  GAS_METER,
  GAS_USAGE_TODAY,
  GAS_USAGE_THIS_WEEK,
  GAS_USAGE_THIS_MONTH,
  GAS_COST_TODAY,
  GAS_COST_THIS_WEEK,
  GAS_COST_THIS_MONTH,
  GAS_UNIT_RATE,
  GAS_STANDING_CHARGE,
  GAS_CALORIFIC_VALUE,
  GAS_VOLUME_CORRECTION,
  GAS_UNIT_RATE_PER_CUBIC_METRE,
  COUNT,  // keep last
};

class GeoTrioComponent : public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_sensor(SensorId id, sensor::Sensor *sensor) { this->sensors_[static_cast<size_t>(id)] = sensor; }
  void set_meter_connection_sensor(binary_sensor::BinarySensor *sensor) { this->meter_connection_sensor_ = sensor; }
  void set_cost_decimal_places(Fuel fuel, uint8_t places) {
    this->fuel(fuel).cost_decimal_places = places;
    this->fuel(fuel).cost_decimal_places_from_config = true;
  }
  void set_log_frames(bool log_frames) { this->log_frames_ = log_frames; }

 protected:
  struct FuelState {
    // Meter values are raw integers x multiplier / divisor. The display announces both when it
    // starts up; until then these defaults (correct for most UK meters) are used.
    uint32_t multiplier{1};
    uint32_t divisor{1000};
    bool scaling_from_meter{false};

    int8_t cost_decimal_places{-1};  // -1 = not worked out yet, so costs aren't published
    bool cost_decimal_places_from_config{false};
    int8_t disagreeing_decimal_places{-1};
    uint32_t disagreeing_since_ms{0};
    std::optional<uint64_t> latest_raw_cost[PERIOD_COUNT];

    float usage_today_kwh{NAN};
    float unit_rate{NAN};
    float standing_charge{NAN};
    MeterTotalFilter meter_total_filter;

    double scale() const { return static_cast<double>(this->multiplier) / this->divisor; }
  };

  void handle_frame(const uint8_t *frame, size_t length);
  void apply_metering(const FrameContents &contents);
  void apply_prepayment(const FrameContents &contents);
  void apply_price(const FrameContents &contents);
  void update_cost_decimal_places(Fuel fuel);
  void publish_costs(Fuel fuel);
  void publish_gas_unit_rate_per_cubic_metre();

  void publish(SensorId id, float value);
  void load_saved_state();
  void save_state();
  void log_frame(const uint8_t *frame, size_t length, bool recognised);
  void report_meter_connection();

  FuelState &fuel(Fuel fuel) { return this->fuels_[static_cast<size_t>(fuel)]; }

  std::array<sensor::Sensor *, static_cast<size_t>(SensorId::COUNT)> sensors_{};
  binary_sensor::BinarySensor *meter_connection_sensor_{nullptr};
  std::array<FuelState, FUEL_COUNT> fuels_{};
  float calorific_value_{NAN};
  float volume_correction_{NAN};

  FrameReader frame_reader_;
  ESPPreferenceObject preference_;
  bool log_frames_{false};

  uint32_t bytes_received_{0};
  uint32_t frames_received_{0};
  uint32_t frames_decoded_{0};
  uint32_t last_decoded_ms_{0};
  uint32_t last_status_log_ms_{0};
  bool meter_connected_{false};
  bool meter_connection_published_{false};
};

}  // namespace geotrio
}  // namespace esphome
