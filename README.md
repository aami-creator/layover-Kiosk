# ✈️ DXB Layover Kiosk

**An AI-powered airport kiosk that turns your layover into a mini adventure.**
Turn the knob to set your layover time, press the button, and the kiosk plans a fun itinerary for Dubai (DXB) — delivered to your phone as a cute boarding pass with Google Maps links for every stop.

Built on an **ESP32 (MYOSA board)** with an OLED screen, 3 status LEDs, a buzzer and two push buttons.

<!-- 🎥 DEMO VIDEO: on GitHub, click the ✏️ edit button on this README,
     drag-and-drop your demo video right here, then commit. GitHub will
     insert a video link automatically. (Or paste a YouTube link.) -->

> 🎥 **Demo video:** 



https://github.com/user-attachments/assets/b665d434-1c52-4f4b-ba4b-25cafcd65ed4


---

## ✨ Features

- 🎛️ **Pick your layover** (2–12 hours) with a rotary knob
- 🤖 **AI trip planner** — asks Google Gemini for a 3-stop plan; automatically falls back to backup AIs (Groq / OpenRouter) and finally to a built-in plan, so it **always** gives a result
- 🏙️ **Smart planning for DXB**
  - Under 6 h → things to do **inside the airport**
  - 6 h or more → real places in Dubai (Metro Red Line / taxi), with a reminder about UAE visas
  - Always keeps a 2.5 h buffer to get back for the flight
- 🎫 **Cute boarding-pass web page** hosted by the ESP32 itself, opened by scanning a **QR code** on the OLED
- 📍 **Google Maps button** for every stop
- 🛂 **Sponsor ad slot** — the idle screen briefly shows a Visa Guy ad + QR, and the pass has an ad banner
- 🚦 **3 status LEDs** — yellow = thinking, green = pass ready, red = error
- 🔔 **Reset button** — tap to clear the pass for the next traveller (plays an airport “ding-dong” chime with an LED light show), hold 3 s to restart
- 🖨️ **3D-printable enclosure** included

---

## 🧰 Hardware

| Part | Qty |
|---|---|
| ESP32 board (MYOSA / ESP32-WROOM-32E dev board) | 1 |
| 0.96" SSD1306 I2C OLED (128×64) | 1 |
| B10K potentiometer + knob | 1 |
| 6×6 mm push buttons | 2 |
| 5 mm LEDs (yellow, green, red) | 3 |
| 220 Ω resistors (for LEDs) | 3 |
| Buzzer (passive recommended for the chime) | 1 |
| Breadboard + jumper wires | — |

## 🔌 Wiring

| Component | Pin | ESP32 / MYOSA pin |
|---|---|---|
| **OLED** | VCC / GND / SDA / SCL | 3V3 / GND / SDA (GPIO 21) / SCL (GPIO 22) |
| **Potentiometer** | left / middle / right | 3V3 / **D34** / GND |
| **PLAN button** | diagonal legs | **D18** ↔ GND |
| **RESET button** | diagonal legs | **D19** ↔ GND |
| **Yellow LED** (thinking) | + (via 220 Ω) / − | **D33** / GND |
| **Green LED** (ready) | + (via 220 Ω) / − | **D25** / GND |
| **Red LED** (error) | + (via 220 Ω) / − | **D26** / GND |
| **Buzzer** | + / − | **D12** / GND |

> The on-board **BOOT** button also works as a PLAN button.
> If the board ever fails to boot, move the buzzer from D12 to D27 (D12 is a boot-strapping pin) and change `BUZZER_PIN` in the code.

---

## 🚀 Setup

1. **Install Arduino IDE** and the **ESP32 board package** (Boards Manager → “esp32 by Espressif”).
2. **Install libraries** (Library Manager):
   - Adafruit SSD1306
   - Adafruit GFX Library
   - ArduinoJson (v7)
   > ⚠️ Do **not** install the “QRCode” library by Richard Moore — the ESP32 core already has a built-in `qrcode.h`, and the two clash.
3. **Add your secrets:** in `firmware/layover_kiosk/`, copy `secrets_example.h` to **`secrets.h`** and fill in:
   - Wi-Fi name + password (**2.4 GHz** network or phone hotspot)
   - Gemini API key from [Google AI Studio](https://aistudio.google.com/apikey)
   - (optional) Groq / OpenRouter keys as backups
4. Open `firmware/layover_kiosk/layover_kiosk.ino`, select **ESP32 Dev Module** and the right COM port, and **Upload**.
5. Open Serial Monitor at **115200** baud to see the kiosk’s IP address and logs.

## 🕹️ How to use

1. Turn the knob → OLED shows layover hours.
2. Press **PLAN** → yellow LED, the AI plans your trip.
3. Green LED + QR code → scan it with a phone on the **same Wi-Fi** as the kiosk.
4. Enjoy your boarding pass 🎫 — tap 📍 to open each stop in Google Maps.
5. Press **RESET** for the next traveller.

## 🧠 How it works

```
 Knob ─► ESP32 ─► Gemini API (→ Groq → OpenRouter → built-in plan)
                    │
                    ▼  JSON plan (title, 3 stops, tips)
            ESP32 builds HTML boarding pass
                    │
                    ▼
       Web server on ESP32  ◄── phone scans QR on OLED
```

---



---

## 📌 Notes & limitations

- Best placed in the **arrivals / transfer area**, before passengers go airside — that’s when they decide whether to step out.
- Leaving the airport needs a valid UAE visa or entry permission for many nationalities; the kiosk reminds users to check eligibility.
- The phone must be on the same Wi-Fi network as the kiosk to open the pass.

## 👥 Team

_Add your team members’ names here._

## 📄 License

_Add a license if you want (MIT is a common choice for student projects)._
