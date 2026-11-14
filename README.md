# geotrio-esphome

An ESPHome external component that reads smart meter data from a GEO Trio II in-home display and exposes it as native Home Assistant entities. An ESP32 listens on the display's internal serial line, without transmitting, and decodes live electricity power, electricity and gas meter readings, usage, costs and tariff information, including values ready for the Home Assistant Energy dashboard.

The display keeps working as normal, including its own WiFi module.

## What you need

- A GEO Trio II in-home display
- A [Seeed Studio XIAO ESP32-C6](https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/) (other ESP32 boards should work with a different `esp32:` and `rx_pin`)
- Three wires, a soldering iron and a multimeter
- Home Assistant with the ESPHome Device Builder add-on

## 1. Wiring

| GEO Trio II | XIAO ESP32-C6 | What it is |
|---|---|---|
| J8 (TX pad) | D10 | Meter data |
| TP6 | GND | Ground |
| TP69 | VBUS | 5 V power for the esp32 |

<img width="600" alt="GEO Trio II board with the J8, TP6 and TP69 pads circled" src="https://github.com/user-attachments/assets/856aef8b-5614-4d17-97a9-9bcc08498fe1" />

<img width="600" alt="GEO Trio II board with the three wires soldered on" src="https://github.com/user-attachments/assets/f8bd6528-9f98-4afd-8aa5-25e0601bae6d" />

<img width="400" alt="XIAO ESP32-C6 with the three wires connected" src="https://github.com/user-attachments/assets/124c465e-7f0e-4574-a4b3-999dccd0ed67" />

## 2. ESPHome

1. In ESPHome Device Builder, click **Create Device**, and if you're using the same board as me search for esp32c6 and choose **Seeed Studio XIAO ESP32C6**. Give it a name and click **Finish setup**.
2. Click **Edit**. Keep everything Device Builder generated and paste the contents of [examples/geo-trio-reader.yaml](examples/geo-trio-reader.yaml) at the end. The antenna `switch:` block and `rx_pin` are specific to the board I am using, so you may need to adjust them.
3. Plug the esp32 into your computer by USB, with the GEO's 5V disconnected. Choose **Install -> Plug into this computer**.
4. Home Assistant finds the device under **Settings -> Devices & services**. When asked for the API encryption key, use the `key:` under `api:` in your device's YAML. If it times out or shows up with an incorrect name, choose **Add integration -> ESPHome** and enter the device's IP address instead (this happened to me as I'd reused the board from a previous project).
5. Unplug USB, connect the wires to the display and power the display. Later updates install over the network.

## 3. Check it's working

Open the device's logs in Device Builder. Within about 20 seconds you should see `Frame decoded` lines, and **Meter connection** turns on in Home Assistant.

- Electricity updates about every 10 seconds.
- Gas updates every 30 minutes or so (it might take quite a long time to start receiving gas data to start with).
- Tariff and costs can take up to an hour to appear the first time, while the component waits for the hourly price messages.

Once everything matches the GEO's screen, set `log_frames: false` in the YAML.

| Log message | Meaning |
|---|---|
| `No data received yet` | Nothing on D10. Check the TX and GND wires. |
| `Receiving data ... nothing decoded yet` | Probably the wrong pad. |
| `Holding back ... costs` | Normal until the tariff arrives. |

## Energy dashboard

In **Settings -> Dashboards -> Energy**:

- **Grid consumption:** choose **Electricity meter**. For the price, choose **Use an entity with current price** -> **Electricity unit rate**.
- **Gas consumption:** choose **Gas meter**. For the price, choose **Use an entity with current price** -> **Gas unit rate per m³**.

The gas meter counts m³ but the tariff is per kWh, so the component converts the price to per m³ using the calorific value and volume correction your meter sends.

## Credits

The meter protocol decoding is based on [geotrio2mqtt](https://github.com/nadnerb33/geotrio2mqtt) by [nadnerb33](https://github.com/nadnerb33), with additions by [mmillmor](https://github.com/mmillmor), used under the MIT licence.
