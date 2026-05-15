# AirMic-Relay

AirMic-Relay turns ESP32 audio boards into wireless microphones and talk-back
speakers: ESP32 captures voice, sends compressed audio to a Python relay over
TCP, and browsers or TCP clients listen and talk back through the same relay.

This project streams a common I2S digital microphone from an ESP32-S3 to a
relay server over Wi-Fi/TCP. Listener clients connect to the relay server and
play the live audio.

Default I2S wiring:

| Mic pin | ESP32-S3 GPIO |
| --- | --- |
| BCLK / SCK | GPIO5 |
| LRCLK / WS | GPIO4 |
| DOUT / SD | GPIO6 |
| VCC | 3V3 |
| GND | GND |

After flashing, the ESP32 starts a setup Wi-Fi network:

| Setting | Value |
| --- | --- |
| AP SSID | `LeOSListener` |
| AP password | `LeOSListener` |
| Setup page | `http://192.168.4.1/` |

On the setup page:

- Choose a scanned Wi-Fi network, then enter its password.
- Server host/IP and source port. The default source port is `8765`.
- Tune voice capture with:
  `Input gain` for loudness, `Noise gate` for background hiss, `High-pass` for rumble filtering.

Files needed on the cloud server:

| File | Needed for |
| --- | --- |
| `listen.py` | Relay server, listener client, web player, history API |
| `requirements.txt` | Python packages for listener clients |

Run the relay server on a reachable cloud machine:

```sh
python3 listen.py server --source-port 8765 --client-port 8766 --web-port 8080 --recordings-dir recordings
```

The relay server mode only needs Python standard libraries.

The server also saves one WAV file per day:

```text
recordings/YYYY-MM-DD.wav
```

The ESP32 connects to `source-port`. Phones and browsers open:

```text
http://YOUR_SERVER_HOST_OR_IP:8080/
```

The web page supports:

- live listening
- talk-back to an ESP32 speaker on builds that enable `ENABLE_SPEAKER`
- `Live / Balanced / Stable` playback modes
- daily history playback
- WAV downloads from the server

Browser microphone capture for talk-back requires HTTPS on most phones and
modern browsers. Plain HTTP is usually allowed only on `localhost`.

For the newer ESP32-S3 hardware with an I2S speaker and SSD1306 OLED display,
build the optional environment and adjust the pins in `platformio.ini` first:

```sh
python3 -m platformio run -e esp32s3_talk_display --target upload
```

Default optional hardware pins:

| Function | ESP32-S3 GPIO |
| --- | --- |
| Speaker BCLK | GPIO7 |
| Speaker LRCLK / WS | GPIO15 |
| Speaker DIN | GPIO16 |
| OLED SDA | GPIO8 |
| OLED SCL | GPIO9 |

For the ESP32-C3 ES8311 voice board from the `SCH_3636_v2` schematic, use the
`xmini_c3_es8311` environment. This board has 16MB external NOR flash, so the
environment pins the flash clock to 40MHz DIO for stable boot:

```sh
python3 -m platformio run -e xmini_c3_es8311 --target upload
```

Default ESP32-C3 ES8311 pins:

| Function | ESP32-C3 GPIO |
| --- | --- |
| ES8311 / OLED SDA | GPIO3 |
| ES8311 / OLED SCL | GPIO4 |
| I2S DIN from ES8311 | GPIO5 |
| I2S LRCLK / WS | GPIO6 |
| I2S BCLK | GPIO7 |
| I2S MCLK | GPIO10 |
| I2S DOUT to ES8311 | GPIO1 |
| Speaker PA enable | GPIO0 |

Command-line listener devices connect to `client-port`:

```sh
python3 -m pip install --user -r requirements.txt
python3 listen.py client --host YOUR_SERVER_HOST_OR_IP --port 8766 --gain 8
```

Open these TCP ports on the cloud firewall:

| Port | Purpose |
| --- | --- |
| `8765` | ESP32 audio upload |
| `8766` | command-line listener clients |
| `8080` | phone/browser web listener |

For clearer voice monitoring, this firmware now defaults to 16 kHz mono PCM with
ESP32-side voice shaping:

- `Input gain`: start around `1.5` to `1.9`
- `Noise gate`: start around `140` to `220`
- `High-pass`: keep enabled unless voices sound too thin

If playback is choppy on phones, switch the web page from `Live` to `Balanced`
or `Stable`.

The old USB serial listener is still available for older firmware:

```sh
python3 listen.py serial --port /dev/cu.usbmodem11101 --gain 8
```
