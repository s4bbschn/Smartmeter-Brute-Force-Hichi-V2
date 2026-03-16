#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>
#include "config.h"

// Hardware Serial für IR Kommunikation
// ESP32-C3 hat nur UART0 (USB) und UART1 (frei)
HardwareSerial customSerial(1);  // UART1 statt UART2!

// WiFi und MQTT Clients
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

// Globale Variablen
int pinDirection = PIN_DIRECTION;
int currentPin = START_PIN - PIN_DIRECTION;
bool pinFound = false;
int referenceMessageLength = 0;
int lastMessageLength = 0;
unsigned long lastMqttUpdate = 0;
bool wifiConnected = false;
bool mqttConnected = false;
bool haDiscoverySent = false;

// Forward Declarations
void mqttCallback(char* topic, byte* payload, unsigned int length);
void publishStatus(const char* message);

#include "readMessage.h"

// Debug-Logger für MQTT
char debugBuffer[256];
void mqttLog(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(debugBuffer, sizeof(debugBuffer), format, args);
    va_end(args);
    
    Serial.println(debugBuffer);  // Auch auf Serial ausgeben
    
    if (mqttConnected && mqttClient.connected()) {
        mqttClient.publish(MQTT_TOPIC_DEBUG, debugBuffer);
    }
}

// WiFi Verbindung herstellen
void connectWiFi() {
    Serial.println("Verbinde mit WiFi...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        Serial.println("\nWiFi verbunden!");
        Serial.print("IP Adresse: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("\nWiFi Verbindung fehlgeschlagen!");
        wifiConnected = false;
    }
}

// MQTT Verbindung herstellen
void connectMQTT() {
    if (!wifiConnected) return;
    
    Serial.println("Verbinde mit MQTT Broker...");
    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    mqttClient.setBufferSize(512);  // Größerer Buffer für Discovery Messages
    mqttClient.setCallback(mqttCallback);  // Callback für eingehende Nachrichten
    
    int attempts = 0;
    while (!mqttClient.connected() && attempts < 5) {
        Serial.print("MQTT Verbindungsversuch...");
        
        bool connected;
        if (strlen(MQTT_USER) > 0) {
            connected = mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD);
        } else {
            connected = mqttClient.connect(MQTT_CLIENT_ID);
        }
        
        if (connected) {
            mqttConnected = true;
            Serial.println("verbunden!");
            
            // Subscribe zu Control Topics
            mqttClient.subscribe(MQTT_TOPIC_SET_PIN);
            mqttClient.subscribe(MQTT_TOPIC_CONTINUE);
            mqttLog("Subscribed zu Control Topics");
            
            return;
        } else {
            Serial.print("fehlgeschlagen, rc=");
            Serial.println(mqttClient.state());
            delay(2000);
            attempts++;
        }
    }
    
    mqttConnected = false;
}

// MQTT Callback für eingehende Nachrichten
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String message = "";
    for (unsigned int i = 0; i < length; i++) {
        message += (char)payload[i];
    }
    
    mqttLog("MQTT RX: %s = %s", topic, message.c_str());
    //mqttLog("MQTT_TOPIC_SET_PIN %s",strcmp(topic, MQTT_TOPIC_SET_PIN) == 0);
    //mqttLog("MQTT_TOPIC_CONTINUE %s",strcmp(topic, MQTT_TOPIC_CONTINUE) == 0);
    
    // Set PIN Command
    if (String(topic).indexOf("set_pin") > 0) {
        int newPin = message.toInt();
        if (newPin >= MIN_PIN && newPin <= MAX_PIN) {
            currentPin = newPin - pinDirection;  // -1 weil nextPin() gleich aufgerufen wird
            mqttLog("PIN manuell gesetzt auf: %04d", newPin);
            publishStatus("PIN manuell gesetzt");
        } else {
            mqttLog("Ungültiger PIN: %d", newPin);
        }
    }
    
    
    mqttLog("Continue Command empfangen, pinFound=%d", pinFound);
        
    if (pinFound) {
        pinFound = false;
            
        // Setze die letzte Nachrichtenlänge als neue Referenz
        // damit der gleiche "Treffer" nicht nochmal erkannt wird
        if (lastMessageLength > referenceMessageLength) {
            int oldRef = referenceMessageLength;
            referenceMessageLength = lastMessageLength;
            mqttLog("Referenz: %d -> %d Bytes", oldRef, referenceMessageLength);
        }
            
        mqttLog("False Positive - Suche fortgesetzt!");
        publishStatus("Suche fortgesetzt (False Positive)");
    } else {
        mqttLog("Kein Treffer aktiv - ignoriere Continue");
    }
}

// Home Assistant MQTT Discovery konfigurieren
void publishHomeAssistantDiscovery() {
    if (!mqttConnected || !mqttClient.connected() || haDiscoverySent) return;
    
    Serial.println("Sende Home Assistant Discovery Konfiguration...");
    
    char topic[150];
    char payload[512];
    
    // Device Info (wird in allen Sensoren verwendet)
    const char* deviceInfo = "\"device\":{\"identifiers\":[\"" HA_DEVICE_ID "\"],"
                            "\"name\":\"" HA_DEVICE_NAME "\","
                            "\"model\":\"Hichi WiFi v2\","
                            "\"manufacturer\":\"Custom\"}";
    
    // 1. Status Sensor
    snprintf(topic, sizeof(topic), "%s/sensor/%s/status/config", HA_DISCOVERY_PREFIX, HA_DEVICE_ID);
    snprintf(payload, sizeof(payload), 
        "{\"name\":\"Status\","
        "\"state_topic\":\"%s\","
        "\"unique_id\":\"%s_status\","
        "\"icon\":\"mdi:information\","
        "%s}", 
        MQTT_TOPIC_STATUS, HA_DEVICE_ID, deviceInfo);
    mqttClient.publish(topic, payload, true);
    delay(100);
    
    // 2. Current PIN Sensor
    snprintf(topic, sizeof(topic), "%s/sensor/%s/current_pin/config", HA_DISCOVERY_PREFIX, HA_DEVICE_ID);
    snprintf(payload, sizeof(payload), 
        "{\"name\":\"Aktueller PIN\","
        "\"state_topic\":\"%s\","
        "\"unique_id\":\"%s_current_pin\","
        "\"icon\":\"mdi:numeric\","
        "%s}", 
        MQTT_TOPIC_CURRENT_PIN, HA_DEVICE_ID, deviceInfo);
    mqttClient.publish(topic, payload, true);
    delay(100);
    
    // 3. Result Sensor
    snprintf(topic, sizeof(topic), "%s/sensor/%s/result/config", HA_DISCOVERY_PREFIX, HA_DEVICE_ID);
    snprintf(payload, sizeof(payload), 
        "{\"name\":\"Ergebnis\","
        "\"state_topic\":\"%s\","
        "\"unique_id\":\"%s_result\","
        "\"icon\":\"mdi:check-circle\","
        "%s}", 
        MQTT_TOPIC_RESULT, HA_DEVICE_ID, deviceInfo);
    mqttClient.publish(topic, payload, true);
    delay(100);
    
    // 4. Progress Sensor (Prozent)
    snprintf(topic, sizeof(topic), "%s/sensor/%s/progress/config", HA_DISCOVERY_PREFIX, HA_DEVICE_ID);
    snprintf(payload, sizeof(payload), 
        "{\"name\":\"Fortschritt\","
        "\"state_topic\":\"%s\","
        "\"unique_id\":\"%s_progress\","
        "\"unit_of_measurement\":\"%%\","
        "\"icon\":\"mdi:progress-clock\","
        "%s}", 
        MQTT_TOPIC_PROGRESS, HA_DEVICE_ID, deviceInfo);
    mqttClient.publish(topic, payload, true);
    delay(100);
    
    // 5. Message Length Sensor
    snprintf(topic, sizeof(topic), "%s/sensor/%s/message_length/config", HA_DISCOVERY_PREFIX, HA_DEVICE_ID);
    snprintf(payload, sizeof(payload), 
        "{\"name\":\"Nachrichtenlänge\","
        "\"state_topic\":\"%s\","
        "\"unique_id\":\"%s_message_length\","
        "\"unit_of_measurement\":\"Bytes\","
        "\"icon\":\"mdi:file-document\","
        "%s}", 
        MQTT_TOPIC_MESSAGE_LENGTH, HA_DEVICE_ID, deviceInfo);
    mqttClient.publish(topic, payload, true);
    delay(100);
    
    // 6. Debug Log Sensor
    snprintf(topic, sizeof(topic), "%s/sensor/%s/debug/config", HA_DISCOVERY_PREFIX, HA_DEVICE_ID);
    snprintf(payload, sizeof(payload), 
        "{\"name\":\"Debug Log\","
        "\"state_topic\":\"%s\","
        "\"unique_id\":\"%s_debug\","
        "\"icon\":\"mdi:bug\","
        "%s}", 
        MQTT_TOPIC_DEBUG, HA_DEVICE_ID, deviceInfo);
    mqttClient.publish(topic, payload, true);
    delay(100);
    
    // 7. Set PIN Number Input
    snprintf(topic, sizeof(topic), "%s/number/%s/set_pin/config", HA_DISCOVERY_PREFIX, HA_DEVICE_ID);
    snprintf(payload, sizeof(payload), 
        "{\"name\":\"PIN setzen\","
        "\"command_topic\":\"%s\","
        "\"unique_id\":\"%s_set_pin\","
        "\"min\":0,"
        "\"max\":9999,"
        "\"step\":1,"
        "\"mode\":\"box\","
        "\"icon\":\"mdi:numeric\","
        "%s}", 
        MQTT_TOPIC_SET_PIN, HA_DEVICE_ID, deviceInfo);
    mqttClient.publish(topic, payload, true);
    delay(100);
    
    // 8. Continue Button
    snprintf(topic, sizeof(topic), "%s/button/%s/continue/config", HA_DISCOVERY_PREFIX, HA_DEVICE_ID);
    snprintf(payload, sizeof(payload), 
        "{\"name\":\"Weiter suchen\","
        "\"command_topic\":\"%s\","
        "\"unique_id\":\"%s_continue\","
        "\"payload_press\":\"continue\","
        "\"icon\":\"mdi:play-circle\","
        "%s}", 
        MQTT_TOPIC_CONTINUE, HA_DEVICE_ID, deviceInfo);
    mqttClient.publish(topic, payload, true);
    delay(100);
    
    haDiscoverySent = true;
    Serial.println("Home Assistant Discovery abgeschlossen!");
    
    // Initiale Werte senden
    mqttClient.publish(MQTT_TOPIC_STATUS, "Initialisierung...");
    mqttClient.publish(MQTT_TOPIC_CURRENT_PIN, "----");
    mqttClient.publish(MQTT_TOPIC_RESULT, "Warte auf Start");
    mqttClient.publish(MQTT_TOPIC_PROGRESS, "0");
    mqttClient.publish(MQTT_TOPIC_MESSAGE_LENGTH, "0");
}

// MQTT Status senden
void publishStatus(const char* message) {
    if (mqttConnected && mqttClient.connected()) {
        mqttClient.publish(MQTT_TOPIC_STATUS, message);
        Serial.printf("MQTT: %s\n", message);
    }
}

// Aktuellen PIN über MQTT senden
void publishCurrentPin(int pin) {
    if (mqttConnected && mqttClient.connected()) {
        char pinStr[10];
        sprintf(pinStr, "%04d", pin);
        mqttClient.publish(MQTT_TOPIC_CURRENT_PIN, pinStr);
        
        // Fortschritt berechnen und senden
        float progress;
        if (pinDirection > 0) {
            // Aufwärts: von MIN_PIN zu MAX_PIN
            progress = ((float)(pin - MIN_PIN) / (float)(MAX_PIN - MIN_PIN)) * 100.0;
        } else {
            // Abwärts: von MAX_PIN zu MIN_PIN
            progress = ((float)(MAX_PIN - pin) / (float)(MAX_PIN - MIN_PIN)) * 100.0;
        }
        
        // Sicherstellen dass Fortschritt zwischen 0 und 100 liegt
        if (progress < 0) progress = 0;
        if (progress > 100) progress = 100;
        
        char progressStr[10];
        sprintf(progressStr, "%.1f", progress);
        mqttClient.publish(MQTT_TOPIC_PROGRESS, progressStr);
    }
}

// Nachrichtenlänge über MQTT senden
void publishMessageLength(int length) {
    if (mqttConnected && mqttClient.connected()) {
        char lengthStr[10];
        sprintf(lengthStr, "%d", length);
        mqttClient.publish(MQTT_TOPIC_MESSAGE_LENGTH, lengthStr);
    }
}

// Ergebnis über MQTT senden
void publishResult(int pin, bool success) {
    if (mqttConnected && mqttClient.connected()) {
        char message[100];
        if (success) {
            sprintf(message, "PIN gefunden: %04d", pin);
        } else {
            sprintf(message, "PIN nicht gefunden");
        }
        mqttClient.publish(MQTT_TOPIC_RESULT, message);
        Serial.printf("MQTT Result: %s\n", message);
    }
}

void setup() {
    // Serial.begin(115200);
    // delay(1000);
    //Serial.println("\n\n=================================");
    //Serial.println("Hichi WiFi v2 - Smartmeter PIN Brute Force");
    //Serial.println("=================================\n");
    
    // WiFi und MQTT verbinden
    connectWiFi();
    if (wifiConnected) {
        connectMQTT();
        if (mqttConnected) {
            publishHomeAssistantDiscovery();
            mqttLog("System gestartet");
        }
        
        // OTA Setup
        ArduinoOTA.setHostname("hichi-brute-force");
        ArduinoOTA.setPassword("brute123");  // Ändere das Passwort wenn du willst
        
        ArduinoOTA.onStart([]() {
            mqttLog("OTA Update startet...");
        });
        
        ArduinoOTA.onEnd([]() {
            mqttLog("OTA Update abgeschlossen");
        });
        
        ArduinoOTA.onError([](ota_error_t error) {
            mqttLog("OTA Error: %u", error);
        });
        
        ArduinoOTA.begin();
        mqttLog("OTA aktiviert (Hostname: hichi-brute-force)");
    }
    
    // IR TX Pin konfigurieren (invertierte Logik - HIGH = LED aus)
    pinMode(IR_TX, OUTPUT);
    digitalWrite(IR_TX, HIGH);  // LED aus
    
    // Serielle Kommunikation für IR Lesekopf
    customSerial.begin(SERIAL_BAUD, SERIAL_CONFIG, IR_RX, 20);
    mqttLog("UART init: RX=GPIO%d TX=GPIO%d Baud=%d", IR_RX, 20, SERIAL_BAUD);
    
    // Kurz warten und prüfen ob Daten kommen
    delay(2000);
    int available = customSerial.available();
    mqttLog("Buffer: %d Bytes verfuegbar", available);
    
    publishStatus("Messe Referenz-Nachrichtenlänge...");
    
    // Referenz-Nachrichtenlänge ermitteln
    mqttLog("Ermittle Referenz-Nachrichtenlaenge...");
    
    unsigned long measureStart = millis();
    referenceMessageLength = getMaximumMessageLength();
    unsigned long measureTime = millis() - measureStart;
    
    lastMessageLength = referenceMessageLength;
    
    char statusMsg[100];
    sprintf(statusMsg, "Referenzlänge: %d Bytes", referenceMessageLength);
    publishStatus(statusMsg);
    mqttLog("Referenzlaenge: %d Bytes (Dauer: %lums)", referenceMessageLength, measureTime);
    
    if (referenceMessageLength == 0) {
        mqttLog("HINWEIS: Keine UART-Daten - PIN-Erkennung funktioniert nicht!");
        mqttLog("Teste trotzdem PINs (nur LED-Pulse)");
    }
    
    mqttLog("Starte in 3 Sekunden...");
    delay(3000);
    
    publishStatus("Brute-Force startet...");
    mqttLog("=== Brute-Force gestartet ===");

        // Test: LED 5x blinken lassen
    /*mqttLog("Test: Blinke LED 5x");
    for (int i = 0; i < 5; i++) {
        digitalWrite(IR_TX, LOW);  // LED an
        delay(500);
        digitalWrite(IR_TX, HIGH); // LED aus
        delay(500);
        mqttLog("  Blink %d/5", i + 1);
    }*/
}

void nextPin() {
    currentPin += pinDirection;
    
    if (currentPin > MAX_PIN) {
        currentPin = MAX_PIN;
        pinDirection = -1;
    } else if (currentPin < MIN_PIN) {
        currentPin = MIN_PIN;
        pinDirection = 1;
    }
}

void makePulse() {
    digitalWrite(IR_TX, LOW);   // LED an (invertierte Logik)
    delay(PULSE_HIGH_MS);
    digitalWrite(IR_TX, HIGH);  // LED aus (invertierte Logik)
    delay(PULSE_LOW_MS);
}

void initPinInput() {
    delay(800);
    
    // LED-Puls um PIN-Eingabe zu starten
    makePulse();
    
    delay(INIT_DELAY_MS);
}

void sendPin(int pin) {
    char statusMsg[100];
    sprintf(statusMsg, "Sende PIN: %04d", pin);
    publishStatus(statusMsg);
    mqttLog("Starte PIN-Eingabe: %04d", pin);
    

    
    initPinInput();
    
    char formattedPin[5];
    sprintf(formattedPin, "%04d", pin);
    
    for (int i = 0; i < 4; i++) {
        int digit = formattedPin[i] - '0';
        
        mqttLog("Sende Ziffer #%d/4: %d", i + 1, digit);
        
        // Sende Pulse für die Ziffer
        for (int j = 0; j < digit; j++) {
            makePulse();
        }
        
        delay(DIGIT_DELAY_MS);
    }
    
    mqttLog("PIN-Eingabe abgeschlossen");
}

void loop() {
    // OTA Handle
    ArduinoOTA.handle();
    
    // MQTT Verbindung aufrechterhalten
    if (mqttConnected) {
        if (!mqttClient.connected()) {
            connectMQTT();
        }
        mqttClient.loop();  // Wichtig: Auch wenn PIN gefunden, damit Commands empfangen werden
    }
    
    if (pinFound) {
        // PIN wurde gefunden - Endlosschleife mit Status-Updates
        char resultMsg[100];
        sprintf(resultMsg, "✓ PIN gefunden: %04d", currentPin);
        
        if (millis() - lastMqttUpdate > 10000) {  // Alle 10 Sekunden
            publishResult(currentPin, true);
            Serial.println(resultMsg);
            lastMqttUpdate = millis();
        }
        
        delay(100);  // Kurzes Delay damit MQTT Loop öfter läuft
        
    }else{
    
        
        mqttLog("--- Test PIN: %04d ---", currentPin);
        publishCurrentPin(currentPin);
        
        char statusMsg[150];
        sprintf(statusMsg, "Teste PIN %04d (Ref: %d -> Aktuell: %d Bytes)", 
            currentPin, referenceMessageLength, lastMessageLength);
            publishStatus(statusMsg);
            
            sendPin(currentPin);
            
            // Warte 5 Sekunden ohne zu blockieren (OTA und MQTT bleiben aktiv)
            mqttLog("Warte 5s auf Zähler-Antwort...");
            unsigned long waitUntil = millis() + 5000;
            while (millis() < waitUntil) {
                ArduinoOTA.handle();
                if (mqttConnected && mqttClient.connected()) mqttClient.loop();
                delay(10);
            }
            
            // Nachrichtenlänge nach PIN-Eingabe messen
            mqttLog("Messe Antwort-Laenge...");
            
            int messageLength = getMaximumMessageLength();
            lastMessageLength = messageLength;
            publishMessageLength(messageLength);
            
            mqttLog("Nachrichtenlaenge: %d Bytes (Ref: %d)", messageLength, referenceMessageLength);
            
            // PIN ist korrekt wenn Nachricht DEUTLICH länger wird (mindestens 50 Bytes mehr)
            int difference = messageLength - referenceMessageLength;
            if (difference > 50) {
                pinFound = true;
                mqttLog("*** PIN GEFUNDEN: %04d ***", currentPin);
                mqttLog("Laenge: %d -> %d Bytes (+%d)", referenceMessageLength, messageLength, difference);
                publishResult(currentPin, true);
                publishStatus("✓ PIN erfolgreich gefunden!");
                
                // Fortschritt auf 100% setzen
                mqttClient.publish(MQTT_TOPIC_PROGRESS, "100.0");
            } else if (difference > 0) {
                mqttLog("Laenge +%d Bytes (zu wenig, brauche >50)", difference);
            }
            nextPin();
    }
    // Kurze Pause zwischen Versuchen
    delay(500);
}
