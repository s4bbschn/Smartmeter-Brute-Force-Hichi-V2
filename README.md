# Hichi WiFi v2 - EMH Smartmeter PIN Brute Force

Dieses Projekt ermöglicht das Brute-Forcing des PINs eines EMH Smartmeters mit einem Hichi WiFi v2 Lesekopf. Die Ergebnisse werden über MQTT veröffentlicht.

## Hardware

- **Hichi WiFi v2** (ESP32-basierter IR-Lesekopf)
- **EMH Smartmeter** mit PIN-geschütztem Zugang

## Features

- ✅ MQTT-Integration für Status-Updates
- ✅ Echtzeit-Überwachung des aktuellen PINs
- ✅ Automatische Erkennung des korrekten PINs
- ✅ Optimiert für EMH Smartmeter
- ✅ WiFi-Verbindung mit Fallback
- ✅ Serielle Debug-Ausgabe

## MQTT Topics

Das Programm veröffentlicht auf folgenden Topics:

- `smartmeter/brute-force/status` - Allgemeine Status-Updates
- `smartmeter/brute-force/current_pin` - Aktuell getesteter PIN (Format: "0000" bis "9999")
- `smartmeter/brute-force/result` - Endergebnis (gefundener PIN)
- `smartmeter/brute-force/progress` - Fortschritt in Prozent (0-100%)
- `smartmeter/brute-force/message_length` - Aktuelle Nachrichtenlänge in Bytes

## Home Assistant Integration

Das Projekt unterstützt **MQTT Discovery** für Home Assistant. Nach dem Start werden automatisch folgende Sensoren erstellt:

- **Status** - Zeigt den aktuellen Status des Brute-Force-Prozesses
- **Aktueller PIN** - Der gerade getestete PIN
- **Ergebnis** - Das finale Ergebnis (gefundener PIN)
- **Fortschritt** - Fortschrittsbalken in Prozent
- **Nachrichtenlänge** - Aktuelle Nachrichtenlänge vom Smartmeter

Die Sensoren erscheinen automatisch unter dem Gerät "Smartmeter Brute Force" in Home Assistant.

### Voraussetzungen für Home Assistant

Stelle sicher, dass in deiner `configuration.yaml` MQTT Discovery aktiviert ist:

```yaml
mqtt:
  discovery: true
  discovery_prefix: homeassistant
```

Nach dem Neustart der Firmware erscheinen die Sensoren automatisch!

## Installation

### 1. Konfiguration anpassen

Bearbeite `src/config.h` und passe folgende Werte an:

```cpp
// WiFi Zugangsdaten
#define WIFI_SSID "DEIN_WIFI_SSID"
#define WIFI_PASSWORD "DEIN_WIFI_PASSWORT"

// MQTT Broker Einstellungen
#define MQTT_SERVER "192.168.1.100"  // IP deines MQTT Brokers
#define MQTT_PORT 1883
#define MQTT_USER ""  // Optional
#define MQTT_PASSWORD ""  // Optional

// PIN-Bereich (optional anpassen)
#define START_PIN 0
#define MAX_PIN 9999
#define MIN_PIN 0
```

### 2. Firmware flashen

```bash
# PlatformIO installieren (falls noch nicht vorhanden)
pip install platformio

# Projekt kompilieren und hochladen
pio run --target upload

# Serielle Ausgabe überwachen
pio device monitor
```

### 3. Nach erfolgreichem PIN-Fund

Sobald der PIN gefunden wurde:
1. Notiere dir den PIN aus der MQTT-Nachricht oder der seriellen Ausgabe
2. Flashe deine normale ESPHome-Firmware wieder auf den Hichi WiFi v2
3. Nutze den gefundenen PIN für den regulären Betrieb

## Funktionsweise

1. **Initialisierung**: Das Programm misst zunächst die Referenz-Nachrichtenlänge des Smartmeters ohne PIN
2. **Brute-Force**: Es testet systematisch alle PINs von 0000 bis 9999
3. **PIN-Eingabe**: Jeder PIN wird durch IR-Pulse an den Smartmeter übertragen:
   - 1 Puls = Ziffer 1
   - 2 Pulse = Ziffer 2
   - usw.
4. **Erkennung**: Wenn die Nachrichtenlänge nach PIN-Eingabe größer ist als die Referenz, wurde der korrekte PIN gefunden
5. **Benachrichtigung**: Der gefundene PIN wird über MQTT und seriell ausgegeben

## Timing-Parameter

Die Timing-Parameter in `config.h` sind für EMH Smartmeter optimiert:

```cpp
#define PULSE_HIGH_MS 250    // LED an Zeit
#define PULSE_LOW_MS 250     // LED aus Zeit  
#define DIGIT_DELAY_MS 3100  // Wartezeit zwischen Ziffern
#define INIT_DELAY_MS 4000   // Wartezeit nach Init-Pulse
```

Falls dein Smartmeter andere Timings benötigt, kannst du diese Werte anpassen.

## GPIO-Pins (Hichi WiFi v2)

```cpp
#define IR_RX 16  // RX Pin für IR Lesekopf
#define IR_TX 17  // TX Pin für IR Lesekopf
```

Diese Pins sind für den Hichi WiFi v2 korrekt konfiguriert.

## Troubleshooting

### WiFi verbindet nicht
- Überprüfe SSID und Passwort in `config.h`
- Stelle sicher, dass der Hichi in Reichweite des WLANs ist

### MQTT verbindet nicht
- Überprüfe die IP-Adresse des MQTT Brokers
- Teste die Verbindung mit einem MQTT-Client (z.B. MQTT Explorer)
- Falls Authentifizierung nötig ist, setze `MQTT_USER` und `MQTT_PASSWORD`

### Keine Reaktion vom Smartmeter
- Überprüfe, ob der IR-Lesekopf korrekt am Smartmeter angebracht ist
- Kontrolliere die serielle Ausgabe auf Fehlermeldungen
- Passe ggf. die Timing-Parameter an

### PIN wird nicht gefunden
- Manche Smartmeter sperren nach mehreren Fehlversuchen
- Warte einige Minuten und starte neu
- Überprüfe, ob der Smartmeter überhaupt PIN-geschützt ist

## Home Assistant Integration

Beispiel für `configuration.yaml`:

```yaml
mqtt:
  sensor:
    - name: "Smartmeter Brute Force Status"
      state_topic: "smartmeter/brute-force/status"
      
    - name: "Smartmeter Current PIN"
      state_topic: "smartmeter/brute-force/current_pin"
      
    - name: "Smartmeter PIN Result"
      state_topic: "smartmeter/brute-force/result"
```

**Hinweis:** Diese manuelle Konfiguration ist nicht mehr nötig, da das Projekt MQTT Discovery unterstützt. Die Sensoren werden automatisch erstellt!

## Home Assistant Dashboard Beispiel

Du kannst eine einfache Lovelace-Karte erstellen, um den Fortschritt zu überwachen:

```yaml
type: entities
title: Smartmeter PIN Brute Force
entities:
  - entity: sensor.smartmeter_brute_force_status
    name: Status
  - entity: sensor.smartmeter_brute_force_aktueller_pin
    name: Aktueller PIN
  - entity: sensor.smartmeter_brute_force_fortschritt
    name: Fortschritt
  - entity: sensor.smartmeter_brute_force_nachrichtenlange
    name: Nachrichtenlänge
  - entity: sensor.smartmeter_brute_force_ergebnis
    name: Ergebnis
```

Oder eine Gauge-Karte für den Fortschritt:

```yaml
type: gauge
entity: sensor.smartmeter_brute_force_fortschritt
name: Brute Force Fortschritt
min: 0
max: 100
severity:
  green: 90
  yellow: 50
  red: 0
```

## Rechtliche Hinweise

⚠️ **Wichtig**: Dieses Tool darf nur an eigenen Smartmetern verwendet werden! Das unbefugte Zugreifen auf fremde Smartmeter ist illegal.

## Credits

Basierend auf dem Original-Projekt von [philipparndt](https://github.com/philipparndt/smartmeter-pin-brute-force)

Angepasst für Hichi WiFi v2 mit MQTT-Integration.
