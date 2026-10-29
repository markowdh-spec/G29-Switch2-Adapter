# G29-Switch2-Adapter

**Logitech G29 Lenkrad an der Nintendo Switch 2** – ein DIY-Adapter aus einem ca. 13 € Board und einer Firmware-Datei. Ohne Löten.

*English version below.*

☕ **Hat dir das Projekt geholfen?** Ich freue mich über einen Kaffee: [ko-fi.com/g29adapter](https://ko-fi.com/g29adapter)

> Privates Bastelprojekt. Kein offizielles Produkt von Nintendo oder Logitech. Nutzung auf eigene Gefahr – siehe [Haftungsausschluss](#haftungsausschluss).

---

## Was geht

- Lenkrad → linker Stick (Lenkbereich umschaltbar: 90° / 120° / 180° / 270°)
- Gas → A, Bremse → B, Kupplung → R
- Alle Tasten, Schaltwippen, Steuerkreuz, HOME
- Vibration aus dem Spiel wird im Lenkrad spürbar (leicht / mittel / stark / aus)
- Rückstellkraft (wie schwer sich das Lenkrad dreht) am roten Rad einstellbar
- H-Schaltknüppel wird in Schaltwippen-Drücke übersetzt (in Erprobung)
- **Alle Einstellungen werden automatisch gespeichert**

Getestet mit: **Logitech G29** (Schalter PC / PS4 / PS5) an der **Switch 2 im Dock**.

## Was du brauchst

| Teil | Hinweis |
|---|---|
| **Waveshare RP2350-USB-A** | Board mit großer USB-A-Buchse + USB-C (ca. 13 €) |
| USB-C-**Daten**kabel | kein reines Ladekabel |
| Logitech G29 mit Netzteil | Schiebeschalter am Lenkrad auf **PC** |
| Firmware | [`G29-Adapter-Switch2-v10.uf2`](firmware/G29-Adapter-Switch2-v10.uf2) im Ordner `firmware` (dort auf „Download raw file“ tippen) |

## Einrichtung

**1. Firmware aufspielen (einmalig)**
1. `BOOT`-Taste am Board gedrückt halten und per USB-C an den PC stecken.
2. Es erscheint ein Laufwerk „RP2350“. Die `.uf2`-Datei draufziehen – das Laufwerk verschwindet von selbst.

**2. An die Switch 2**
1. Switch 2: *Einstellungen → Controller und Zubehör → „Kabelgebundene Kommunikation für Pro Controller“* einschalten.
2. Schalter am Lenkrad auf **PC** (auch für die Switch!), Netzteil einstecken.
3. Lenkrad-USB in die USB-A-Buchse des Boards, Board per USB-C ans Dock.
4. Lenkrad kalibriert sich, ca. 6 Sekunden warten. LED wird **blau** = fertig.

## Bedienung am Lenkrad

| Bedienelement | Wirkung |
|---|---|
| `+` / `−` | Lenkmitte reagiert früher / ruhiger (Anti-Totzone, 0–50 %) |
| Rotes Rad rechts / links | Lenkrad schwerer / leichter (0–8) |
| Enter kurz | Vibration leicht → mittel → stark → aus |
| Enter halten + `+` / `−` | Lenkbereich 90° / 120° / 180° / 270° |
| Enter 5 Sek. halten | Alles zurück auf Startwerte |

**LED:** blau = läuft · kurz grün = gespeichert · weiß blinkt 1–4× = Lenkbereich · lila 1–3× / rot 1× = Vibrationsstufe / aus · 3× rot = Startwerte.

## Sicherheit

Das Lenkrad dreht sich beim Einstecken (Kalibrierung) und im Standby der Switch, wenn das Dock die USB-Anschlüsse kurz aus- und einschaltet. Hände weg, solange es sich dreht. Nach dem Spielen Netzteil des Lenkrads ziehen. Kleine Kinder nicht unbeaufsichtigt spielen lassen.

## Selbst bauen

```bash
cd Firmware/RP2040
cmake -S . -B build-switch -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DOGXM_BOARD=RP2350_USB_A -DMAX_GAMEPADS=1 -DOGXM_FIXED_DRIVER=SWITCH \
  -DPICO_SDK_PATH=../external/pico-sdk
cmake --build build-switch
```
Benötigt: ARM GCC (arm-none-eabi), CMake, Ninja, Pico SDK 2.1.0 und die Git-Submodule.

Der G29-Treiber liegt in [`Firmware/RP2040/src/USBHost/HostDriver/LogitechG29/`](Firmware/RP2040/src/USBHost/HostDriver/LogitechG29/).

## Danksagung & Quellen

Dieses Projekt ist ein Fork und baut auf der Arbeit anderer auf – vielen Dank!

| Projekt | Autor | Lizenz |
|---|---|---|
| [OGX-Mini-2026](https://github.com/MegaCadeDev/OGX-Mini-2026) (Basis dieses Forks) | MegaCadeDev | MIT |
| [OGX-Mini](https://github.com/wiredopposite/OGX-Mini) (Original) | wiredopposite | MIT |
| [TinyUSB](https://github.com/hathach/tinyusb) | Ha Thach | MIT |
| [Pico-PIO-USB](https://github.com/sekigon-gonnoc/Pico-PIO-USB) | sekigon-gonnoc | MIT |
| [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) | Raspberry Pi Ltd | BSD-3-Clause |
| [libfixmath](https://github.com/PetteriAimonen/libfixmath) | Flatmush, Petteri Aimonen u. a. | MIT |
| [libxsm3](https://github.com/InvoxiPlayGames/libxsm3) | InvoxiPlayGames | LGPL-2.1 |
| Logitech-Lenkrad-Befehle | angelehnt an den Linux-Treiber `hid-lg4ff` | Inspiration, kein übernommener Code |

Die Lizenztexte liegen in [`LICENSE`](LICENSE) und in den Ordnern unter `Firmware/external/`. Der vollständige Quellcode ist in diesem Repository enthalten. Die ursprüngliche OGX-Mini-2026-Beschreibung: [README-OGX-Mini-2026.md](README-OGX-Mini-2026.md).

G29-Treiber und Anpassungen: © 2026 markowdh-spec, MIT-Lizenz.

## Haftungsausschluss

Privates Bastelprojekt. Nachbau und Nutzung erfolgen auf eigene Gefahr. Soweit gesetzlich zulässig, wird keine Haftung übernommen für Schäden an Board, Lenkrad, Pedalen, Konsole, Dock, Kabeln oder anderen Geräten, für Datenverlust oder Folgeschäden. Die Firmware wird ohne Gewährleistung bereitgestellt („as is“). Eine Funktion mit anderen Lenkrädern, Spielen oder künftigen Systemupdates der Switch 2 ist nicht zugesichert. Kein offizielles Produkt von Nintendo oder Logitech und nicht von diesen unterstützt; alle Marken gehören ihren jeweiligen Inhabern.

---

## English

**Logitech G29 racing wheel on the Nintendo Switch 2** with a ~13 € Waveshare RP2350-USB-A board. Fork of [OGX-Mini-2026](https://github.com/MegaCadeDev/OGX-Mini-2026) with a dedicated Logitech G29 host driver.

- Wheel → left stick (range 90/120/180/270°), gas → A, brake → B, clutch → R, all buttons
- Game rumble → force pulses in the wheel, adjustable centring spring, H-shifter → paddle presses
- On-wheel settings (+/−, red dial, Enter), saved to flash automatically

**Setup:** hold BOOT, plug into a PC, drop [`firmware/G29-Adapter-Switch2-v10.uf2`](firmware/G29-Adapter-Switch2-v10.uf2) onto the drive. On the Switch 2 enable *Pro Controller Wired Communication*. Wheel mode switch on **PC**. Wheel → board USB-A, board USB-C → dock.

Personal hobby project, provided as-is without warranty, use at your own risk. Not affiliated with or endorsed by Nintendo or Logitech. Credits and licenses: see the table above.
