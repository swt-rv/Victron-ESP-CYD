# Camper Power Dashboard

ESP32 "Cheap Yellow Display" (ESP32-2432S028R, 2.8" ILI9341) showing:

- **Victron SmartSolar MPPT**: PV watts, battery volts/amps, charge state, yield today
- **Victron SmartShunt / BMV**: state of charge, volts, amps, watts, Ah used, time left, alarms
- **Up to 3 Ruuvi tags**: temperature and humidity (fridge, battery box, outside...)

Everything is read over Bluetooth by listening to broadcasts (Victron "Instant Readout" and Ruuvi data format 5). Nothing is paired.

## Publish the one-click install page

1. Create a new **public** GitHub repository (e.g. `camper-dash`) and upload everything in this folder, keeping the structure:
   ```
   .github/workflows/build.yml
   docs/index.html
   docs/manifest-cyd.json
   docs/manifest-cyd_inverted.json
   src/main.cpp
   platformio.ini
   .gitignore
   ```
   The `.github` folder is hidden on Mac/Linux, so if drag-and-drop skips it, use **Add file > Create new file** on GitHub, type `.github/workflows/build.yml` as the name, and paste the contents.
2. In the repo go to **Settings > Pages** and set **Source** to **GitHub Actions**.
3. Open the **Actions** tab. The "Build and publish" run starts on its own (or press *Run workflow*). It takes a few minutes the first time.
4. When it finishes, your page is at `https://<your-username>.github.io/<repo-name>/`.

Open that link in **Chrome or Edge** on a computer, plug in the display with a USB data cable, and:

1. Click **Install** and pick the serial port.
2. Under **Enter your device settings**, click **Connect**, then fill in the addresses/keys and press **Save to display**.

## Getting the Victron address and key

VictronConnect > open the device > Settings (gear) > **Product info** > enable **Instant readout via Bluetooth** > **Show** under Encryption data.

## No secrets in the repo

Keys and addresses are typed into the web page and stored on the ESP32 itself (flash). They are never part of the code or the published page. Updating the firmware keeps them.

## Troubleshooting

- **Colours inverted**: use the second install button.
- **Blank screen after install**: some CYD boards use a different display chip; open an issue or adjust the pin flags in `platformio.ini`.
- **Serial port not listed**: install the CH340 / CP210x USB driver, and use a data-capable USB cable.
- **"key mismatch" in the log**: the Victron key doesn't belong to that address.
- **Ruuvi "format not supported"**: update the tag firmware in the Ruuvi Station app (needs data format 5).

## Build locally

```
pip install platformio
pio run -e cyd -t upload
```
