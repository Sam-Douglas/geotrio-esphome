#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "decoder.h"

using namespace esphome::geotrio;
using Bytes = std::vector<uint8_t>;

static int failures = 0;

#define CHECK(condition)                                                \
  do {                                                                  \
    if (!(condition)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
      failures++;                                                       \
    }                                                                   \
  } while (0)

static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

static void append_little_endian(Bytes &out, uint64_t value, size_t byte_count) {
  for (size_t i = 0; i < byte_count; i++)
    out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}

static void append_record(Bytes &out, uint16_t attribute, uint8_t type, uint64_t value, size_t byte_count) {
  append_little_endian(out, attribute, 2);
  out.push_back(0x00);  // status: success
  out.push_back(type);
  append_little_endian(out, value, byte_count);
}

// Header up to and including the cluster tag and endpoint. The filler bytes stand in for the parts
// of the real header we don't decode.
static Bytes frame_start(uint8_t cluster_low_byte, uint8_t endpoint) {
  return {0x80, 0x02, 0xAA, 0xBB, 0x01, 0x09, 0x07, cluster_low_byte, endpoint};
}

// What actually goes over the wire: start byte, escaped payload, end byte.
static Bytes to_wire(const Bytes &frame) {
  Bytes wire{0x01};
  for (uint8_t byte : frame) {
    if (byte == 0x01 || byte == 0x02 || byte == 0x03) {
      wire.push_back(0x02);
      wire.push_back(byte ^ 0x08);
    } else {
      wire.push_back(byte);
    }
  }
  wire.push_back(0x03);
  return wire;
}

static FrameContents decode_via_reader(const Bytes &frame) {
  FrameReader reader;
  for (uint8_t byte : to_wire(frame)) {
    if (reader.add_byte(byte))
      return decode_frame(reader.frame(), reader.length());
  }
  return {};
}

static void test_frame_reader_unescapes() {
  const Bytes frame = {0x80, 0x02, 0x01, 0x03, 0x02, 0x55};
  FrameReader reader;
  bool complete = false;
  for (uint8_t byte : Bytes{0x55, 0x03})  // noise before the first start byte is ignored
    complete = reader.add_byte(byte);
  CHECK(!complete);
  for (uint8_t byte : to_wire(frame))
    complete = reader.add_byte(byte);
  CHECK(complete);
  CHECK(Bytes(reader.frame(), reader.frame() + reader.length()) == frame);
}

static void test_metering_frame() {
  Bytes frame = frame_start(0x02, 1);
  append_record(frame, 0x0400, 0x2A, 0xFFFFFF - 499, 3);  // instantaneous demand -500 (exporting)
  append_record(frame, 0x0000, 0x25, 14473801, 6);         // meter total
  append_record(frame, 0x0401, 0x25, 5321, 6);             // usage today
  append_record(frame, 0x0440, 0x22, 210000, 3);           // usage this month, as a uint24

  const FrameContents contents = decode_via_reader(frame);
  CHECK(contents.recognised);
  CHECK(contents.fuel == Fuel::ELECTRICITY);
  CHECK(contents.instantaneous_demand == -500);
  CHECK(contents.meter_total == 14473801u);
  CHECK(contents.usage[0] == 5321u);
  CHECK(!contents.usage[1]);
  CHECK(contents.usage[2] == 210000u);
  CHECK(!contents.multiplier && !contents.divisor);  // no unit of measure, so not a formatting frame
}

static void test_gas_metering_frame_has_no_power() {
  Bytes frame = frame_start(0x02, 2);
  append_record(frame, 0x0400, 0x2A, 1234, 3);
  append_record(frame, 0x0C01, 0x25, 12500, 6);  // gas usage today in Wh
  const FrameContents contents = decode_via_reader(frame);
  CHECK(contents.recognised && contents.fuel == Fuel::GAS);
  CHECK(!contents.instantaneous_demand);
  CHECK(contents.usage[0] == 12500u);
}

static void test_scaling_needs_unit_of_measure() {
  Bytes with_unit = frame_start(0x02, 1);
  append_record(with_unit, 0x0300, 0x30, 0, 1);  // unit of measure: kWh
  append_record(with_unit, 0x0301, 0x22, 1, 3);
  append_record(with_unit, 0x0302, 0x22, 10000, 3);
  const FrameContents scaling_frame = decode_via_reader(with_unit);
  CHECK(scaling_frame.multiplier == 1u);
  CHECK(scaling_frame.divisor == 10000u);

  // The same multiplier and divisor records without a unit of measure must be ignored.
  Bytes without_unit = frame_start(0x02, 1);
  append_record(without_unit, 0x0301, 0x22, 1, 3);
  append_record(without_unit, 0x0302, 0x22, 10000, 3);
  const FrameContents other_frame = decode_via_reader(without_unit);
  CHECK(other_frame.recognised);
  CHECK(!other_frame.multiplier);
  CHECK(!other_frame.divisor);
}

static void test_unknown_endpoint_is_ignored() {
  Bytes frame = frame_start(0x02, 9);
  append_record(frame, 0x0000, 0x25, 1, 6);
  CHECK(!decode_via_reader(frame).recognised);
}

static void test_prepayment_frame() {
  Bytes frame = frame_start(0x05, 2);
  append_record(frame, 0x051C, 0x25, 123456, 6);
  append_record(frame, 0x0540, 0x25, 9876543, 6);
  const FrameContents contents = decode_via_reader(frame);
  CHECK(contents.recognised && contents.fuel == Fuel::GAS);
  CHECK(contents.cost[0] == 123456u);
  CHECK(!contents.cost[1]);
  CHECK(contents.cost[2] == 9876543u);
}

// Price frames: the Zigbee message length is 18 bytes after the start of the tag.
static Bytes price_frame(uint8_t endpoint, uint8_t frame_control, uint8_t command, const Bytes &payload) {
  Bytes frame = frame_start(0x00, endpoint);
  frame.resize(4 + 18, 0xEE);  // filler up to the length byte (the tag starts at index 4)
  frame.push_back(static_cast<uint8_t>(3 + payload.size()));
  frame.push_back(frame_control);
  frame.push_back(0x42);  // sequence number
  frame.push_back(command);
  frame.insert(frame.end(), payload.begin(), payload.end());
  return frame;
}

static void test_publish_price() {
  Bytes payload;
  append_little_endian(payload, 0x11223344, 4);  // provider id
  payload.push_back(3);                          // rate label length
  payload.insert(payload.end(), {'D', 'a', 'y'});
  append_little_endian(payload, 7, 4);      // issuer event id
  append_little_endian(payload, 0, 4);      // current time
  payload.push_back(0x00);                  // unit of measure
  append_little_endian(payload, 826, 2);    // currency: GBP
  payload.push_back(0x51);                  // 5 decimal places, tier 1
  payload.push_back(0x11);                  // tiers
  append_little_endian(payload, 0, 4);      // start time
  append_little_endian(payload, 0xFFFF, 2); // duration
  append_little_endian(payload, 24567, 4);  // price
  const FrameContents contents = decode_via_reader(price_frame(1, 0x09, 0x00, payload));
  CHECK(contents.recognised && contents.fuel == Fuel::ELECTRICITY);
  CHECK(contents.unit_rate && near(*contents.unit_rate, 0.24567));
}

static void test_publish_tariff_information() {
  Bytes payload(16, 0x00);  // provider, event and tariff ids, start time
  payload.push_back(0x00);  // tariff type
  payload.push_back(2);     // label length
  payload.insert(payload.end(), {'E', '7'});
  payload.insert(payload.end(), {0x02, 0x00, 0x00});  // tiers, block thresholds, unit of measure
  append_little_endian(payload, 826, 2);
  payload.push_back(0x40);  // 4 decimal places
  append_little_endian(payload, 5384, 4);
  const FrameContents contents = decode_via_reader(price_frame(3, 0x09, 0x04, payload));
  CHECK(contents.recognised && contents.fuel == Fuel::GAS);
  CHECK(contents.standing_charge && near(*contents.standing_charge, 0.5384));
}

static void test_calorific_value_and_conversion_factor() {
  Bytes calorific(8, 0x00);
  append_little_endian(calorific, 395, 4);
  calorific.push_back(0x01);  // MJ/m³
  calorific.push_back(0x10);  // 1 decimal place
  CHECK(near(*decode_via_reader(price_frame(3, 0x09, 0x03, calorific)).calorific_value, 39.5));

  Bytes conversion(8, 0x00);
  append_little_endian(conversion, 102264, 4);
  conversion.push_back(0x50);
  CHECK(near(*decode_via_reader(price_frame(3, 0x09, 0x02, conversion)).volume_correction, 1.02264));

  // The same two values arriving as a read attributes response.
  Bytes attributes;
  append_record(attributes, 0x0304, 0x23, 395, 4);
  append_record(attributes, 0x0306, 0x18, 0x10, 1);
  append_record(attributes, 0x0302, 0x23, 102264, 4);
  append_record(attributes, 0x0303, 0x18, 0x50, 1);
  const FrameContents contents = decode_via_reader(price_frame(3, 0x08, 0x01, attributes));
  CHECK(contents.calorific_value && near(*contents.calorific_value, 39.5));
  CHECK(contents.volume_correction && near(*contents.volume_correction, 1.02264));
}

static void test_cost_decimal_places() {
  // 10 kWh at 24.5p plus 53.84p standing charge = £2.9884
  CHECK(find_cost_decimal_places(298840, 10.0, 0.245, 0.5384) == 5);
  CHECK(find_cost_decimal_places(299, 10.0, 0.245, 0.5384) == 2);
  CHECK(find_cost_decimal_places(298840, NAN, 0.245, 0.5384) == -1);  // usage not known yet
  CHECK(find_cost_decimal_places(0, 10.0, 0.245, 0.5384) == -1);
}

static void test_meter_total_filter() {
  MeterTotalFilter filter;
  CHECK(filter.accept(4236.845));
  CHECK(filter.accept(4236.900));
  CHECK(!filter.accept(9.9e12));  // the kind of absurd value the display occasionally sends
  CHECK(!filter.accept(4236.0));  // went backwards
  CHECK(filter.accept(4237.0));
  // A genuine jump (e.g. a replaced meter) is accepted once it has been seen three times running.
  CHECK(!filter.accept(10.0));
  CHECK(!filter.accept(10.1));
  CHECK(filter.accept(10.2));
  CHECK(filter.accept(10.3));
}

static int run_built_in_tests() {
  test_frame_reader_unescapes();
  test_metering_frame();
  test_gas_metering_frame_has_no_power();
  test_scaling_needs_unit_of_measure();
  test_unknown_endpoint_is_ignored();
  test_prepayment_frame();
  test_publish_price();
  test_publish_tariff_information();
  test_calorific_value_and_conversion_factor();
  test_cost_decimal_places();
  test_meter_total_filter();
  std::printf(failures == 0 ? "All decoder tests passed\n" : "%d check(s) failed\n", failures);
  return failures == 0 ? 0 : 1;
}

static void print_contents(const FrameContents &c) {
  if (!c.recognised) {
    std::printf("  not recognised\n");
    return;
  }
  static const char *const PERIODS[PERIOD_COUNT] = {"today", "this week", "this month"};
  std::printf("  %s\n", fuel_name(c.fuel));
  if (c.instantaneous_demand)
    std::printf("    instantaneous demand (raw): %d\n", *c.instantaneous_demand);
  if (c.meter_total)
    std::printf("    meter total (raw): %llu\n", (unsigned long long) *c.meter_total);
  for (size_t p = 0; p < PERIOD_COUNT; p++) {
    if (c.usage[p])
      std::printf("    usage %s (raw): %llu\n", PERIODS[p], (unsigned long long) *c.usage[p]);
    if (c.cost[p])
      std::printf("    cost %s (raw): %llu\n", PERIODS[p], (unsigned long long) *c.cost[p]);
  }
  if (c.multiplier)
    std::printf("    multiplier: %u\n", *c.multiplier);
  if (c.divisor)
    std::printf("    divisor: %u\n", *c.divisor);
  if (c.unit_rate)
    std::printf("    unit rate: %.5f GBP/kWh\n", *c.unit_rate);
  if (c.standing_charge)
    std::printf("    standing charge: %.5f GBP/day\n", *c.standing_charge);
  if (c.calorific_value)
    std::printf("    calorific value: %.2f MJ/m3\n", *c.calorific_value);
  if (c.volume_correction)
    std::printf("    volume correction: %.5f\n", *c.volume_correction);
}

static int decode_log_file(const char *path) {
  std::ifstream file(path);
  if (!file) {
    std::printf("Can't open %s\n", path);
    return 1;
  }
  std::string line;
  while (std::getline(file, line)) {
    // Use the text after "): " if this is a log line, otherwise the whole line.
    const size_t marker = line.find("): ");
    const std::string hex = marker == std::string::npos ? line : line.substr(marker + 3);
    Bytes frame;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
      if (!std::isxdigit(static_cast<unsigned char>(hex[i])) || !std::isxdigit(static_cast<unsigned char>(hex[i + 1])))
        break;
      frame.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    if (frame.size() < 2)
      continue;
    std::printf("%s\n", hex.substr(0, frame.size() * 2).c_str());
    print_contents(decode_frame(frame.data(), frame.size()));
  }
  return 0;
}

int main(int argc, char **argv) { return argc > 1 ? decode_log_file(argv[1]) : run_built_in_tests(); }
