# geotrio-esphome

An ESPHome external component that reads smart meter data from a GEO Trio II in-home display and exposes it as native Home Assistant entities. An ESP32 listens on the display's internal serial line, without transmitting, and decodes live electricity power, electricity and gas meter readings, usage, costs and tariff information, including values ready for the Home Assistant Energy dashboard.

An example device configuration for a Seeed Studio XIAO ESP32-C6 is in [examples/geo-trio-reader.yaml](examples/geo-trio-reader.yaml).

## Credits

The meter protocol decoding is based on [geotrio2mqtt](https://github.com/nadnerb33/geotrio2mqtt) by [nadnerb33](https://github.com/nadnerb33), with additions by [mmillmor](https://github.com/mmillmor), used under the MIT licence.
