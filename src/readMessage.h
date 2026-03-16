#ifndef READ_MESSAGE_H
#define READ_MESSAGE_H

#include <Arduino.h>

extern HardwareSerial customSerial;
extern void mqttLog(const char* format, ...);

int getMaximumMessageLength() {
    int maxLength = 0;
    bool dataReceived = false;
    
    mqttLog("=== Starte Messung ===");
    
    unsigned long totalStart = millis();
    unsigned long maxTotalTime = 50000;  // Absoluter Timeout: 20 Sekunden für alles
    
    // Warte auf Daten und lese mehrere Nachrichten
    for (int attempt = 0; attempt < 5; attempt++) {
        // Prüfe absoluten Timeout
        if (millis() - totalStart > maxTotalTime) {
            mqttLog("Absoluter Timeout erreicht!");
            break;
        }
        
        int currentLength = 0;
        unsigned long startTime = millis();
        unsigned long lastByteTime = millis();
        unsigned long timeout = 12000;  // Max 8 Sekunden pro Versuch
        unsigned long idleTimeout = 500;  // 500ms ohne neue Daten = Nachricht zu Ende
        
        mqttLog("Versuch %d", attempt + 1);
        
        // Warte auf erste Daten (max 3 Sekunden)
        unsigned long waitStart = millis();
        while (!customSerial.available() && (millis() - waitStart < 5000)) {
            if (mqttConnected && mqttClient.connected()) {
                mqttClient.loop();
            }
            delay(10);
        }
        
        if (!customSerial.available()) {
            mqttLog("  Timeout: Keine Daten");
            if (!dataReceived) {
                mqttLog("Breche Messung ab - keine Daten");
                break;
            }
            continue;
        }
        
        lastByteTime = millis();
        
        // Lese alle Daten
        while (millis() - startTime < timeout) {
            if (customSerial.available()) {
                byte data = customSerial.read();
                currentLength++;
                dataReceived = true;
                lastByteTime = millis();
                
                // Debug: Zeige erste paar Bytes
                //if (currentLength <= 5) {
                //    mqttLog("  Byte %d: 0x%02X", currentLength, data);
                //}
            }
            
            // Wenn 500ms keine neuen Daten, Nachricht ist zu Ende
            if (dataReceived && (millis() - lastByteTime > idleTimeout)) {
                mqttLog("  Nachricht komplett");
                break;
            }
            
            // MQTT Loop damit Nachrichten gesendet werden
            if (mqttConnected && mqttClient.connected()) {
                mqttClient.loop();
            }
            
            delay(1);
        }
        
        if (currentLength > maxLength) {
            maxLength = currentLength;
        }
        
        mqttLog("Versuch %d: %d Bytes", attempt + 1, currentLength);
        
        delay(200);
    }
    
    mqttLog("=== Maximum: %d Bytes ===", maxLength);
    
    // Wenn keine Daten kommen, gebe trotzdem einen Wert zurück
    if (maxLength == 0) {
        mqttLog("WARNUNG: Keine UART Daten! Fahre trotzdem fort...");
        return 0;
    }
    
    return maxLength;
}

#endif
