# geotrio-esphome

An ESPHome external component that reads smart meter data from a GEO Trio II in-home display and exposes it as native Home Assistant entities. An ESP32 listens on the display's internal serial line, without transmitting, and decodes live electricity power, electricity and gas meter readings, usage, costs and tariff information, including values ready for the Home Assistant Energy dashboard.

An example device configuration for a Seeed Studio XIAO ESP32-C6 is in [examples/geo-trio-reader.yaml](examples/geo-trio-reader.yaml).

<img width="4080" height="2805" alt="board" src="https://github.com/user-attachments/assets/856aef8b-5614-4d17-97a9-9bcc08498fe1" />

<img width="2954" height="2825" alt="soldered-board" src="https://github.com/user-attachments/assets/f8bd6528-9f98-4afd-8aa5-25e0601bae6d" />

<img width="2549" height="2563" alt="esp32" src="https://github.com/user-attachments/assets/124c465e-7f0e-4574-a4b3-999dccd0ed67" />

## Credits

The meter protocol decoding is based on [geotrio2mqtt](https://github.com/nadnerb33/geotrio2mqtt) by [nadnerb33](https://github.com/nadnerb33), with additions by [mmillmor](https://github.com/mmillmor), used under the MIT licence.
