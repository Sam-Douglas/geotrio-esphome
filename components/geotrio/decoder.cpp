#include "decoder.h"

#include <cmath>
#include <cstring>

namespace esphome {
namespace geotrio {

namespace {

constexpr uint8_t FRAME_START = 0x01;
constexpr uint8_t FRAME_ESCAPE = 0x02;
constexpr uint8_t FRAME_END = 0x03;
constexpr uint8_t ESCAPE_XOR = 0x08;

constexpr uint8_t PAYLOAD_HEADER[2] = {0x80, 0x02};

// Profile 0x0109 (Smart Energy) followed by the cluster id, both high byte first.
constexpr uint8_t METERING_TAG[4] = {0x01, 0x09, 0x07, 0x02};
constexpr uint8_t PRICE_TAG[4] = {0x01, 0x09, 0x07, 0x00};
constexpr uint8_t PREPAYMENT_TAG[4] = {0x01, 0x09, 0x07, 0x05};
constexpr size_t TAG_LENGTH = 4;

// Zigbee data types
constexpr uint8_t TYPE_BITMAP8 = 0x18;
constexpr uint8_t TYPE_UINT8 = 0x20;
constexpr uint8_t TYPE_UINT64 = 0x27;
constexpr uint8_t TYPE_UINT48 = 0x25;
constexpr uint8_t TYPE_INT24 = 0x2A;
constexpr uint8_t TYPE_ENUM8 = 0x30;

// Metering cluster attributes
constexpr uint16_t ATTR_CURRENT_SUMMATION_DELIVERED = 0x0000;
constexpr uint16_t ATTR_UNIT_OF_MEASURE = 0x0300;
constexpr uint16_t ATTR_MULTIPLIER = 0x0301;
constexpr uint16_t ATTR_DIVISOR = 0x0302;
constexpr uint16_t ATTR_INSTANTANEOUS_DEMAND = 0x0400;
// Electricity uses the "historical consumption" set (meter units); gas uses the "alternative
// historical consumption" set, which the display sends in Wh.
constexpr uint16_t USAGE_ATTRIBUTES[FUEL_COUNT][PERIOD_COUNT] = {
    {0x0401, 0x0430, 0x0440},
    {0x0C01, 0x0C30, 0x0C40},
};
constexpr uint32_t MAX_SCALING_FACTOR = 0xFFFFFF;  // multiplier and divisor are 24-bit

// Prepayment cluster attributes: cost of energy used today / this week / this month.
constexpr uint16_t COST_ATTRIBUTES[PERIOD_COUNT] = {0x051C, 0x0530, 0x0540};

// Price cluster commands
constexpr uint8_t COMMAND_READ_ATTRIBUTES_RESPONSE = 0x01;  // a general (not cluster-specific) command
constexpr uint8_t COMMAND_PUBLISH_PRICE = 0x00;
constexpr uint8_t COMMAND_PUBLISH_CONVERSION_FACTOR = 0x02;
constexpr uint8_t COMMAND_PUBLISH_CALORIFIC_VALUE = 0x03;
constexpr uint8_t COMMAND_PUBLISH_TARIFF_INFORMATION = 0x04;
// Price cluster attributes that can arrive in a read attributes response
constexpr uint16_t ATTR_CONVERSION_FACTOR = 0x0302;
constexpr uint16_t ATTR_CONVERSION_FACTOR_TRAILING_DIGIT = 0x0303;
constexpr uint16_t ATTR_CALORIFIC_VALUE = 0x0304;
constexpr uint16_t ATTR_CALORIFIC_VALUE_TRAILING_DIGIT = 0x0306;
// In price frames the Zigbee message length sits at a fixed distance after the tag, followed by
// the message itself. Found by experiment on real hardware; not part of any published spec.
constexpr size_t PRICE_MESSAGE_LENGTH_OFFSET = 18;
constexpr uint32_t PRICE_NOT_AVAILABLE = 0xFFFFFFFF;

int find_bytes(const uint8_t *data, size_t length, const uint8_t *pattern, size_t pattern_length) {
  for (size_t i = 0; i + pattern_length <= length; i++) {
    if (std::memcmp(data + i, pattern, pattern_length) == 0)
      return static_cast<int>(i);
  }
  return -1;
}

uint64_t read_little_endian(const uint8_t *data, size_t byte_count) {
  uint64_t value = 0;
  for (size_t i = byte_count; i > 0; i--)
    value = (value << 8) | data[i - 1];
  return value;
}

int32_t read_int24(const uint8_t *data) {
  int32_t value = static_cast<int32_t>(read_little_endian(data, 3));
  if (value & 0x800000)
    value -= 0x1000000;
  return value;
}

uint32_t read_uint32(const uint8_t *data) { return static_cast<uint32_t>(read_little_endian(data, 4)); }

double power_of_ten(int exponent) {
  double value = 1.0;
  while (exponent-- > 0)
    value *= 10.0;
  return value;
}

// Price values come with a "trailing digit" byte whose high four bits are the number of decimal
// places.
std::optional<double> price_value(uint32_t raw, uint8_t trailing_digit_byte) {
  if (raw == PRICE_NOT_AVAILABLE)
    return std::nullopt;
  return raw / power_of_ten(trailing_digit_byte >> 4);
}

// Finds an attribute record with the given id and data type and returns where its value starts.
// The search starts at `data`, so callers pass the part of the frame after the cluster tag.
const uint8_t *find_attribute(const uint8_t *data, size_t length, uint16_t attribute_id, uint8_t type,
                              size_t value_length) {
  const uint8_t record_start[4] = {static_cast<uint8_t>(attribute_id & 0xFF), static_cast<uint8_t>(attribute_id >> 8),
                                   0x00, type};
  const int position = find_bytes(data, length, record_start, sizeof(record_start));
  if (position < 0 || position + sizeof(record_start) + value_length > length)
    return nullptr;
  return data + position + sizeof(record_start);
}

// Like find_attribute, but accepts any unsigned integer type (uint8 to uint64), because the
// display doesn't always use the width the spec suggests.
std::optional<uint64_t> read_unsigned_attribute(const uint8_t *data, size_t length, uint16_t attribute_id) {
  for (size_t i = 0; i + 4 <= length; i++) {
    const uint8_t type = data[i + 3];
    const bool is_match = data[i] == (attribute_id & 0xFF) && data[i + 1] == (attribute_id >> 8) &&
                          data[i + 2] == 0x00 && type >= TYPE_UINT8 && type <= TYPE_UINT64;
    if (!is_match)
      continue;
    const size_t value_length = type - TYPE_UINT8 + 1;
    if (i + 4 + value_length > length)
      return std::nullopt;
    return read_little_endian(data + i + 4, value_length);
  }
  return std::nullopt;
}

std::optional<uint8_t> read_bitmap8_attribute(const uint8_t *data, size_t length, uint16_t attribute_id) {
  const uint8_t *value = find_attribute(data, length, attribute_id, TYPE_BITMAP8, 1);
  if (value == nullptr)
    return std::nullopt;
  return *value;
}

// Finds a cluster tag and the fuel from the endpoint byte after it. Returns the tag position, or
// -1 if the tag is missing or the endpoint isn't one we know.
int find_cluster(const uint8_t *frame, size_t length, const uint8_t *tag, bool endpoint_3_is_gas, Fuel &fuel) {
  const int position = find_bytes(frame, length, tag, TAG_LENGTH);
  if (position < 0 || static_cast<size_t>(position) + TAG_LENGTH >= length)
    return -1;
  const uint8_t endpoint = frame[position + TAG_LENGTH];
  if (endpoint == 1) {
    fuel = Fuel::ELECTRICITY;
  } else if (endpoint == 2 || (endpoint == 3 && endpoint_3_is_gas)) {
    fuel = Fuel::GAS;
  } else {
    return -1;
  }
  return position;
}

bool decode_metering(const uint8_t *frame, size_t length, FrameContents &contents) {
  const int tag = find_cluster(frame, length, METERING_TAG, false, contents.fuel);
  if (tag < 0)
    return false;
  const uint8_t *records = frame + tag + TAG_LENGTH + 1;
  const size_t records_length = length - (tag + TAG_LENGTH + 1);

  // Live power is only meaningful for electricity (gas meters are battery powered and don't report it).
  if (contents.fuel == Fuel::ELECTRICITY) {
    const uint8_t *demand = find_attribute(records, records_length, ATTR_INSTANTANEOUS_DEMAND, TYPE_INT24, 3);
    if (demand != nullptr)
      contents.instantaneous_demand = read_int24(demand);
  }

  const uint8_t *total = find_attribute(records, records_length, ATTR_CURRENT_SUMMATION_DELIVERED, TYPE_UINT48, 6);
  if (total != nullptr)
    contents.meter_total = read_little_endian(total, 6);

  for (size_t period = 0; period < PERIOD_COUNT; period++) {
    contents.usage[period] =
        read_unsigned_attribute(records, records_length, USAGE_ATTRIBUTES[static_cast<size_t>(contents.fuel)][period]);
  }

  // The multiplier and divisor only count if the frame also has the unit of measure, i.e. it is the
  // start-up "formatting" burst. This stops a coincidental byte match in another frame being used.
  if (find_attribute(records, records_length, ATTR_UNIT_OF_MEASURE, TYPE_ENUM8, 1) != nullptr) {
    const auto multiplier = read_unsigned_attribute(records, records_length, ATTR_MULTIPLIER);
    if (multiplier && *multiplier > 0 && *multiplier <= MAX_SCALING_FACTOR)
      contents.multiplier = static_cast<uint32_t>(*multiplier);
    const auto divisor = read_unsigned_attribute(records, records_length, ATTR_DIVISOR);
    if (divisor && *divisor > 0 && *divisor <= MAX_SCALING_FACTOR)
      contents.divisor = static_cast<uint32_t>(*divisor);
  }
  return true;
}

bool decode_prepayment(const uint8_t *frame, size_t length, FrameContents &contents) {
  const int tag = find_cluster(frame, length, PREPAYMENT_TAG, false, contents.fuel);
  if (tag < 0)
    return false;
  const uint8_t *records = frame + tag + TAG_LENGTH + 1;
  const size_t records_length = length - (tag + TAG_LENGTH + 1);
  for (size_t period = 0; period < PERIOD_COUNT; period++)
    contents.cost[period] = read_unsigned_attribute(records, records_length, COST_ATTRIBUTES[period]);
  return true;
}

// Payload layouts below are from the Zigbee Smart Energy price cluster specification.
void decode_publish_price(const uint8_t *payload, size_t length, FrameContents &contents) {
  // Provider id (4), rate label (1 length byte + text), then fixed fields up to the price.
  if (length < 5)
    return;
  const size_t after_label = 5 + payload[4];
  const size_t trailing_digit = after_label + 11;
  const size_t price = after_label + 19;
  if (price + 4 <= length)
    contents.unit_rate = price_value(read_uint32(payload + price), payload[trailing_digit]);
}

void decode_publish_tariff_information(const uint8_t *payload, size_t length, FrameContents &contents) {
  // Provider, event and tariff ids, start time (16), tariff type (1), tariff label (1 + text), ...
  if (length < 18)
    return;
  const size_t after_label = 18 + payload[17];
  const size_t trailing_digit = after_label + 5;
  const size_t standing_charge = after_label + 6;
  if (standing_charge + 4 <= length)
    contents.standing_charge = price_value(read_uint32(payload + standing_charge), payload[trailing_digit]);
}

bool decode_price(const uint8_t *frame, size_t length, FrameContents &contents) {
  const int tag = find_cluster(frame, length, PRICE_TAG, true, contents.fuel);
  if (tag < 0 || static_cast<size_t>(tag) + PRICE_MESSAGE_LENGTH_OFFSET + 4 > length)
    return false;

  const size_t message_length = frame[tag + PRICE_MESSAGE_LENGTH_OFFSET];
  const size_t message_start = tag + PRICE_MESSAGE_LENGTH_OFFSET + 1;
  if (message_start + message_length > length)
    return false;

  // Zigbee message: frame control, [2-byte manufacturer code], sequence number, command, payload.
  const uint8_t *message = frame + message_start;
  const uint8_t frame_control = message[0];
  const bool has_manufacturer_code = (frame_control & 0x04) != 0;
  const size_t header_length = has_manufacturer_code ? 5 : 3;
  if (message_length < header_length)
    return false;
  const uint8_t command = message[header_length - 1];
  const uint8_t *payload = message + header_length;
  const size_t payload_length = message_length - header_length;
  const bool is_cluster_specific = (frame_control & 0x03) == 0x01;
  const bool is_gas = contents.fuel == Fuel::GAS;

  if (is_cluster_specific) {
    if (command == COMMAND_PUBLISH_PRICE) {
      decode_publish_price(payload, payload_length, contents);
    } else if (command == COMMAND_PUBLISH_TARIFF_INFORMATION) {
      decode_publish_tariff_information(payload, payload_length, contents);
    } else if (command == COMMAND_PUBLISH_CONVERSION_FACTOR && is_gas && payload_length >= 13) {
      // Event id (4), start time (4), conversion factor (4), trailing digit (1)
      contents.volume_correction = price_value(read_uint32(payload + 8), payload[12]);
    } else if (command == COMMAND_PUBLISH_CALORIFIC_VALUE && is_gas && payload_length >= 14) {
      // Event id (4), start time (4), calorific value (4), unit (1), trailing digit (1)
      contents.calorific_value = price_value(read_uint32(payload + 8), payload[13]);
    }
  } else if (command == COMMAND_READ_ATTRIBUTES_RESPONSE && is_gas) {
    const auto calorific_value = read_unsigned_attribute(payload, payload_length, ATTR_CALORIFIC_VALUE);
    const auto calorific_digits = read_bitmap8_attribute(payload, payload_length, ATTR_CALORIFIC_VALUE_TRAILING_DIGIT);
    if (calorific_value && calorific_digits)
      contents.calorific_value = *calorific_value / power_of_ten(*calorific_digits >> 4);

    const auto conversion_factor = read_unsigned_attribute(payload, payload_length, ATTR_CONVERSION_FACTOR);
    const auto conversion_digits =
        read_bitmap8_attribute(payload, payload_length, ATTR_CONVERSION_FACTOR_TRAILING_DIGIT);
    if (conversion_factor && conversion_digits)
      contents.volume_correction = *conversion_factor / power_of_ten(*conversion_digits >> 4);
  }
  return true;
}

}  // namespace

const char *fuel_name(Fuel fuel) { return fuel == Fuel::ELECTRICITY ? "electricity" : "gas"; }

bool FrameReader::add_byte(uint8_t byte) {
  if (this->escape_next_) {
    this->escape_next_ = false;
    if (this->in_frame_)
      this->append(byte ^ ESCAPE_XOR);
    return false;
  }
  switch (byte) {
    case FRAME_START:
      this->in_frame_ = true;
      this->length_ = 0;
      return false;
    case FRAME_END: {
      const bool complete = this->in_frame_;
      this->in_frame_ = false;
      return complete;
    }
    case FRAME_ESCAPE:
      this->escape_next_ = this->in_frame_;
      return false;
    default:
      if (this->in_frame_)
        this->append(byte);
      return false;
  }
}

void FrameReader::append(uint8_t byte) {
  if (this->length_ < MAX_FRAME_LENGTH) {
    this->buffer_[this->length_++] = byte;
  } else {
    // Too long to be a real frame: probably a lost end byte. Drop it and wait for the next start.
    this->in_frame_ = false;
    this->length_ = 0;
  }
}

FrameContents decode_frame(const uint8_t *frame, size_t length) {
  FrameContents contents;
  if (length < sizeof(PAYLOAD_HEADER) || std::memcmp(frame, PAYLOAD_HEADER, sizeof(PAYLOAD_HEADER)) != 0)
    return contents;
  contents.recognised = decode_metering(frame, length, contents) || decode_price(frame, length, contents) ||
                        decode_prepayment(frame, length, contents);
  return contents;
}

bool is_housekeeping_frame(const uint8_t *frame, size_t length) {
  if (length < 2)
    return false;
  return (frame[0] == 0x7F && frame[1] == 0xFF) || (frame[0] == 0x80 && (frame[1] == 0x08 || frame[1] == 0x09));
}

int find_cost_decimal_places(uint64_t raw_cost_today, double usage_today_kwh, double unit_rate,
                             double standing_charge) {
  // A guess matches if the cost is within this factor of the expected cost. It must stay below
  // the square root of 10 (3.16) so that at most one number of decimal places can match.
  constexpr double MATCH_FACTOR = 2.5;
  constexpr int MAX_DECIMAL_PLACES = 7;

  const double expected_cost = usage_today_kwh * unit_rate + standing_charge;
  if (raw_cost_today == 0 || !std::isfinite(expected_cost) || expected_cost <= 0)
    return -1;
  for (int places = 0; places <= MAX_DECIMAL_PLACES; places++) {
    const double ratio = raw_cost_today / power_of_ten(places) / expected_cost;
    if (ratio >= 1.0 / MATCH_FACTOR && ratio <= MATCH_FACTOR)
      return places;
  }
  return -1;
}

bool MeterTotalFilter::accept(double total) {
  if (!(total >= 0 && total <= MAX_PLAUSIBLE_TOTAL))
    return false;

  const bool nothing_accepted_yet = this->last_accepted_ < 0;
  const bool normal_increase =
      total >= this->last_accepted_ && total - this->last_accepted_ <= MAX_INCREASE_PER_READING;
  if (nothing_accepted_yet || normal_increase) {
    this->last_accepted_ = total;
    this->candidate_count_ = 0;
    return true;
  }

  const bool agrees_with_candidate =
      this->candidate_count_ > 0 && std::fabs(total - this->candidate_) <= MAX_INCREASE_PER_READING;
  this->candidate_count_ = agrees_with_candidate ? this->candidate_count_ + 1 : 1;
  this->candidate_ = total;
  if (this->candidate_count_ < READINGS_NEEDED_TO_CONFIRM)
    return false;

  this->last_accepted_ = total;
  this->candidate_count_ = 0;
  return true;
}

}  // namespace geotrio
}  // namespace esphome
