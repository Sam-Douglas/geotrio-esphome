#include "geotrio.h"

#include <cstring>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace geotrio {

static const char *const TAG = "geotrio";

namespace {

constexpr SensorId METER_SENSOR[FUEL_COUNT] = {SensorId::ELECTRICITY_METER, SensorId::GAS_METER};
constexpr SensorId UNIT_RATE_SENSOR[FUEL_COUNT] = {SensorId::ELECTRICITY_UNIT_RATE, SensorId::GAS_UNIT_RATE};
constexpr SensorId STANDING_CHARGE_SENSOR[FUEL_COUNT] = {SensorId::ELECTRICITY_STANDING_CHARGE,
                                                         SensorId::GAS_STANDING_CHARGE};
constexpr SensorId USAGE_SENSOR[FUEL_COUNT][PERIOD_COUNT] = {
    {SensorId::ELECTRICITY_USAGE_TODAY, SensorId::ELECTRICITY_USAGE_THIS_WEEK, SensorId::ELECTRICITY_USAGE_THIS_MONTH},
    {SensorId::GAS_USAGE_TODAY, SensorId::GAS_USAGE_THIS_WEEK, SensorId::GAS_USAGE_THIS_MONTH},
};
constexpr SensorId COST_SENSOR[FUEL_COUNT][PERIOD_COUNT] = {
    {SensorId::ELECTRICITY_COST_TODAY, SensorId::ELECTRICITY_COST_THIS_WEEK, SensorId::ELECTRICITY_COST_THIS_MONTH},
    {SensorId::GAS_COST_TODAY, SensorId::GAS_COST_THIS_WEEK, SensorId::GAS_COST_THIS_MONTH},
};

constexpr double WH_PER_KWH = 1000.0;
constexpr double MEGAJOULES_PER_KWH = 3.6;

// Once the cost decimal places are known, a different answer must persist this long before it is
// believed, so usage and cost resetting at slightly different moments around midnight can't flip it.
constexpr uint32_t COST_DECIMAL_PLACES_CONFIRM_MS = 30 * 60 * 1000;
// Electricity frames arrive about every 10 seconds and gas every 2 minutes.
constexpr uint32_t METER_TIMEOUT_MS = 5 * 60 * 1000;
constexpr uint32_t STATUS_LOG_INTERVAL_MS = 60 * 1000;

// What survives a restart, so values don't have to be relearned. Bump the version if this changes.
constexpr uint32_t SAVED_STATE_VERSION = 1;
struct SavedState {
  uint32_t version;
  uint32_t multiplier[FUEL_COUNT];
  uint32_t divisor[FUEL_COUNT];
  uint8_t scaling_from_meter[FUEL_COUNT];
  int8_t cost_decimal_places[FUEL_COUNT];
  float unit_rate[FUEL_COUNT];
  float standing_charge[FUEL_COUNT];
  float calorific_value;
  float volume_correction;
};

size_t index(Fuel fuel) { return static_cast<size_t>(fuel); }

}  // namespace

void GeoTrioComponent::setup() {
  this->preference_ = global_preferences->make_preference<SavedState>(fnv1_hash("geotrio_saved_state"));
  this->load_saved_state();

  // Tariff values arrive only about once an hour, so publish the saved ones straight away.
  for (size_t i = 0; i < FUEL_COUNT; i++) {
    const FuelState &state = this->fuels_[i];
    if (!std::isnan(state.unit_rate))
      this->publish(UNIT_RATE_SENSOR[i], state.unit_rate);
    if (!std::isnan(state.standing_charge))
      this->publish(STANDING_CHARGE_SENSOR[i], state.standing_charge);
  }
  if (!std::isnan(this->calorific_value_))
    this->publish(SensorId::GAS_CALORIFIC_VALUE, this->calorific_value_);
  if (!std::isnan(this->volume_correction_))
    this->publish(SensorId::GAS_VOLUME_CORRECTION, this->volume_correction_);
  this->publish_gas_unit_rate_per_cubic_metre();
}

void GeoTrioComponent::loop() {
  uint8_t byte;
  while (this->available() > 0 && this->read_byte(&byte)) {
    this->bytes_received_++;
    if (this->frame_reader_.add_byte(byte))
      this->handle_frame(this->frame_reader_.frame(), this->frame_reader_.length());
  }
  this->report_meter_connection();
}

void GeoTrioComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "GEO Trio II meter reader:");
  ESP_LOGCONFIG(TAG, "  Log frames: %s", YESNO(this->log_frames_));
  for (size_t i = 0; i < FUEL_COUNT; i++) {
    const FuelState &state = this->fuels_[i];
    const char *name = fuel_name(static_cast<Fuel>(i));
    ESP_LOGCONFIG(TAG, "  %s scaling: x%u / %u (%s)", name, (unsigned) state.multiplier, (unsigned) state.divisor,
                  state.scaling_from_meter ? "announced by meter" : "default");
    if (state.cost_decimal_places < 0) {
      ESP_LOGCONFIG(TAG, "  %s cost decimal places: not worked out yet (costs held back)", name);
    } else {
      ESP_LOGCONFIG(TAG, "  %s cost decimal places: %d (%s)", name, state.cost_decimal_places,
                    state.cost_decimal_places_from_config ? "set in YAML" : "worked out from tariff");
    }
  }
}

void GeoTrioComponent::handle_frame(const uint8_t *frame, size_t length) {
  this->frames_received_++;
  const FrameContents contents = decode_frame(frame, length);
  if (this->log_frames_ && !is_housekeeping_frame(frame, length))
    this->log_frame(frame, length, contents.recognised);
  if (!contents.recognised)
    return;

  this->frames_decoded_++;
  this->last_decoded_ms_ = millis();
  this->apply_metering(contents);
  this->apply_prepayment(contents);
  this->apply_price(contents);
}

void GeoTrioComponent::apply_metering(const FrameContents &contents) {
  FuelState &state = this->fuel(contents.fuel);
  const char *name = fuel_name(contents.fuel);

  if (contents.multiplier || contents.divisor) {
    const uint32_t multiplier = contents.multiplier.value_or(state.multiplier);
    const uint32_t divisor = contents.divisor.value_or(state.divisor);
    if (multiplier != state.multiplier || divisor != state.divisor || !state.scaling_from_meter) {
      ESP_LOGI(TAG, "%s scaling announced by meter: x%u / %u", name, (unsigned) multiplier, (unsigned) divisor);
      state.multiplier = multiplier;
      state.divisor = divisor;
      state.scaling_from_meter = true;
      this->save_state();
    }
  }

  if (contents.instantaneous_demand) {
    const double kilowatts = *contents.instantaneous_demand * state.scale();
    this->publish(SensorId::ELECTRICITY_POWER, kilowatts * 1000.0);
  }

  if (contents.meter_total) {
    const double total = *contents.meter_total * state.scale();
    if (state.meter_total_filter.accept(total)) {
      this->publish(METER_SENSOR[index(contents.fuel)], total);
    } else {
      ESP_LOGW(TAG, "Ignoring implausible %s meter total %.3f (held back until confirmed)", name, total);
    }
  }

  for (size_t period = 0; period < PERIOD_COUNT; period++) {
    if (!contents.usage[period])
      continue;
    const double kwh = contents.fuel == Fuel::ELECTRICITY ? *contents.usage[period] * state.scale()
                                                          : *contents.usage[period] / WH_PER_KWH;
    if (period == static_cast<size_t>(Period::TODAY))
      state.usage_today_kwh = kwh;
    this->publish(USAGE_SENSOR[index(contents.fuel)][period], kwh);
  }
}

void GeoTrioComponent::apply_prepayment(const FrameContents &contents) {
  FuelState &state = this->fuel(contents.fuel);
  bool has_cost = false;
  for (size_t period = 0; period < PERIOD_COUNT; period++) {
    if (contents.cost[period]) {
      state.latest_raw_cost[period] = contents.cost[period];
      has_cost = true;
    }
  }
  if (!has_cost)
    return;
  this->update_cost_decimal_places(contents.fuel);
  this->publish_costs(contents.fuel);
}

void GeoTrioComponent::update_cost_decimal_places(Fuel fuel) {
  FuelState &state = this->fuel(fuel);
  const auto &raw_cost_today = state.latest_raw_cost[static_cast<size_t>(Period::TODAY)];
  if (state.cost_decimal_places_from_config || !raw_cost_today)
    return;

  const int places =
      find_cost_decimal_places(*raw_cost_today, state.usage_today_kwh, state.unit_rate, state.standing_charge);
  if (places < 0 || places == state.cost_decimal_places) {
    state.disagreeing_decimal_places = -1;
    return;
  }

  // First answer: use it straight away. Changing an existing answer needs it to persist.
  if (state.cost_decimal_places >= 0) {
    const uint32_t now = millis();
    if (state.disagreeing_decimal_places != places) {
      state.disagreeing_decimal_places = places;
      state.disagreeing_since_ms = now;
      return;
    }
    if (now - state.disagreeing_since_ms < COST_DECIMAL_PLACES_CONFIRM_MS)
      return;
  }

  ESP_LOGI(TAG, "%s cost decimal places worked out from tariff: %d", fuel_name(fuel), places);
  state.cost_decimal_places = static_cast<int8_t>(places);
  state.disagreeing_decimal_places = -1;
  this->save_state();
}

void GeoTrioComponent::publish_costs(Fuel fuel) {
  const FuelState &state = this->fuel(fuel);
  if (state.cost_decimal_places < 0) {
    ESP_LOGD(TAG, "Holding back %s costs until the tariff arrives to work out their decimal places", fuel_name(fuel));
    return;
  }
  double divisor = 1.0;
  for (int i = 0; i < state.cost_decimal_places; i++)
    divisor *= 10.0;
  for (size_t period = 0; period < PERIOD_COUNT; period++) {
    if (state.latest_raw_cost[period])
      this->publish(COST_SENSOR[index(fuel)][period], *state.latest_raw_cost[period] / divisor);
  }
}

void GeoTrioComponent::apply_price(const FrameContents &contents) {
  FuelState &state = this->fuel(contents.fuel);
  bool changed = false;
  auto update = [&changed](float &stored, double value) {
    if (stored != static_cast<float>(value)) {
      stored = static_cast<float>(value);
      changed = true;
    }
  };

  if (contents.unit_rate) {
    update(state.unit_rate, *contents.unit_rate);
    this->publish(UNIT_RATE_SENSOR[index(contents.fuel)], state.unit_rate);
  }
  if (contents.standing_charge) {
    update(state.standing_charge, *contents.standing_charge);
    this->publish(STANDING_CHARGE_SENSOR[index(contents.fuel)], state.standing_charge);
  }
  if (contents.calorific_value) {
    update(this->calorific_value_, *contents.calorific_value);
    this->publish(SensorId::GAS_CALORIFIC_VALUE, this->calorific_value_);
  }
  if (contents.volume_correction) {
    update(this->volume_correction_, *contents.volume_correction);
    this->publish(SensorId::GAS_VOLUME_CORRECTION, this->volume_correction_);
  }
  if (!changed)
    return;

  this->save_state();
  this->publish_gas_unit_rate_per_cubic_metre();
  // A new tariff may let the cost decimal places be worked out at last.
  this->update_cost_decimal_places(contents.fuel);
  this->publish_costs(contents.fuel);
}

// The gas unit rate is per kWh but the gas meter counts m³. Home Assistant's Energy dashboard needs
// a price in the same unit as the meter, so convert using the standard UK formula:
// kWh = m³ x volume correction x calorific value (MJ/m³) / 3.6.
void GeoTrioComponent::publish_gas_unit_rate_per_cubic_metre() {
  const FuelState &gas = this->fuel(Fuel::GAS);
  const double per_cubic_metre = gas.unit_rate * this->volume_correction_ * this->calorific_value_ / MEGAJOULES_PER_KWH;
  if (std::isfinite(per_cubic_metre))
    this->publish(SensorId::GAS_UNIT_RATE_PER_CUBIC_METRE, per_cubic_metre);
}

// Only publishes when the value changes, to keep traffic to Home Assistant down.
void GeoTrioComponent::publish(SensorId id, float value) {
  sensor::Sensor *target = this->sensors_[static_cast<size_t>(id)];
  if (target == nullptr || (target->has_state() && target->state == value))
    return;
  target->publish_state(value);
}

void GeoTrioComponent::load_saved_state() {
  SavedState saved{};
  if (!this->preference_.load(&saved) || saved.version != SAVED_STATE_VERSION)
    return;
  for (size_t i = 0; i < FUEL_COUNT; i++) {
    FuelState &state = this->fuels_[i];
    if (saved.multiplier[i] > 0 && saved.divisor[i] > 0) {
      state.multiplier = saved.multiplier[i];
      state.divisor = saved.divisor[i];
      state.scaling_from_meter = saved.scaling_from_meter[i] != 0;
    }
    if (!state.cost_decimal_places_from_config && saved.cost_decimal_places[i] <= 7)
      state.cost_decimal_places = saved.cost_decimal_places[i];
    state.unit_rate = saved.unit_rate[i];
    state.standing_charge = saved.standing_charge[i];
  }
  this->calorific_value_ = saved.calorific_value;
  this->volume_correction_ = saved.volume_correction;
}

void GeoTrioComponent::save_state() {
  SavedState saved{};
  saved.version = SAVED_STATE_VERSION;
  for (size_t i = 0; i < FUEL_COUNT; i++) {
    const FuelState &state = this->fuels_[i];
    saved.multiplier[i] = state.multiplier;
    saved.divisor[i] = state.divisor;
    saved.scaling_from_meter[i] = state.scaling_from_meter;
    // A value from YAML isn't saved, so removing it from YAML goes back to working it out.
    saved.cost_decimal_places[i] = state.cost_decimal_places_from_config ? -1 : state.cost_decimal_places;
    saved.unit_rate[i] = state.unit_rate;
    saved.standing_charge[i] = state.standing_charge;
  }
  saved.calorific_value = this->calorific_value_;
  saved.volume_correction = this->volume_correction_;
  // ESPHome batches writes, so flash is only written occasionally even if this is called often.
  this->preference_.save(&saved);
}

void GeoTrioComponent::log_frame(const uint8_t *frame, size_t length, bool recognised) {
  static constexpr char HEX_DIGITS[] = "0123456789ABCDEF";
  char hex[FrameReader::MAX_FRAME_LENGTH * 2 + 1];
  for (size_t i = 0; i < length; i++) {
    hex[i * 2] = HEX_DIGITS[frame[i] >> 4];
    hex[i * 2 + 1] = HEX_DIGITS[frame[i] & 0x0F];
  }
  hex[length * 2] = '\0';
  ESP_LOGI(TAG, "Frame %s (%u bytes): %s", recognised ? "decoded" : "not decoded", (unsigned) length, hex);
}

void GeoTrioComponent::report_meter_connection() {
  const uint32_t now = millis();
  const bool connected = this->frames_decoded_ > 0 && now - this->last_decoded_ms_ < METER_TIMEOUT_MS;
  if (this->meter_connection_sensor_ != nullptr &&
      (!this->meter_connection_published_ || connected != this->meter_connected_)) {
    this->meter_connection_sensor_->publish_state(connected);
    this->meter_connection_published_ = true;
  }
  this->meter_connected_ = connected;

  if (now - this->last_status_log_ms_ < STATUS_LOG_INTERVAL_MS)
    return;
  this->last_status_log_ms_ = now;
  if (this->bytes_received_ == 0) {
    ESP_LOGW(TAG, "No data received yet. Check the TX and GND wires and the uart rx_pin.");
  } else if (this->frames_decoded_ == 0) {
    ESP_LOGW(TAG, "Receiving data (%u bytes, %u frames) but nothing decoded yet. Check it is the TX pad.",
             (unsigned) this->bytes_received_, (unsigned) this->frames_received_);
  } else {
    ESP_LOGD(TAG, "Received %u bytes, %u frames, %u decoded", (unsigned) this->bytes_received_,
             (unsigned) this->frames_received_, (unsigned) this->frames_decoded_);
  }
}

}  // namespace geotrio
}  // namespace esphome
