# 🚌 DVB Abfahrtsmonitor

Ein kompakter ESP32-Abfahrtsmonitor für **Dresden und Umgebung (VVO / DVB)**, der die nächsten Linien und Abfahrten einer Haltestelle auf einem **128×64 Pixel ST7567-Display** anzeigt.





---

## 🧰 Benötigte Hardware

| Komponente | Beschreibung / Beispiel |
|-------------|-------------------------|
| **ESP32 Devkit** | z. B. ESP32-WROOM-32D oder DEVKIT V1 |
| **Display** | 128 × 64 Pixel **ST7567 / JLX12864** mit **I²C-Schnittstelle** |
| **Verkabelung** | SDA → GPIO 21, SCL → GPIO 22  |
| **Versorgung** | 5 V USB oder 3.3 V Pin |
| **Optionale Taste** | BOOT-Taste (GPIO 0) für Setup-Start beim Einschalten |

---

## 📦 Benötigte Bibliotheken

Diese Bibliotheken sind alle direkt über den Arduino-Bibliotheksverwalter verfügbar:

- [`U8g2`](https://github.com/olikraus/u8g2) — Displayansteuerung  
- [`ArduinoJson`](https://arduinojson.org/) — JSON-Parser für DVB-API  
- `WiFi`, `HTTPClient`, `WebServer`, `Preferences` — sind im **ESP32-Core** enthalten  



---

## ⚙️ Setup & Erststart

1. **Sketch flashen** auf den ESP32  
   - Board: `ESP32 Dev Module`  
   - Upload-Speed: 921600 Baud empfohlen  

2. **Erster Start (oder keine WLAN-Verbindung):**  
   - Der ESP öffnet ein eigenes WLAN:
     ```
     SSID: Wifi@DVB-XXXX
     Passwort: collaborative
     ```
   - Im Display steht eine **Schritt-für-Schritt-Anleitung** (mehrseitig, automatisch weiterblätternd).

3. **Mit dem Smartphone oder Laptop verbinden**
   - **Mobile Daten ausschalten!**
   - Browser öffnen → `http://192.168.4.1`

4. **Einstellungen eintragen**
   - WLAN-SSID & Passwort  
   - Haltestellenname (`hst`)  
   - Ort (`ort`)  
   - Minuten-Offset (`vz`)  
   - Anzahl der angezeigten Einträge (max 5)  

   → Klick auf **„Speichern & Neustarten“**

5. Der ESP32 verbindet sich mit deinem WLAN und zeigt live die Abfahrten.

---

## 🔁 Setup erneut öffnen

Wenn du das Gerät später neu konfigurieren willst:

- **Beim Einschalten die BOOT-Taste gedrückt halten**  
  → nach ca. 2 Sekunden startet der Setup-Hotspot wieder.  
- Alternativ: WLAN-Verbindung scheitert → Setup startet automatisch.

---



