/* LORA Receiver for ESP32*/

#include <Arduino.h>
#include <SPI.h>
#include <RH_RF95.h>
#include <RHReliableDatagram.h>

#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <Update.h>

// pinout and configuration
#include <pinout.h>
#include <config.h>

typedef struct payload
{
    uint8_t id;
    float temperature;
    float humidity;
    float lux;
    uint32_t tips;
} Payload;

typedef struct gpsPayload
{
    uint8_t id;
    double latitude;
    double longitude;
} GpsPayload;

RH_RF95 rf95(RFM95_CS, RFM95_INT);
RHReliableDatagram manager(rf95, GW_ID);
uint8_t buf[RH_RF95_MAX_MESSAGE_LEN];

// payloads
GpsPayload gpsPayload;
Payload payload;

WiFiManager wm;
WiFiClient client;
HTTPClient http;
JsonDocument doc;

uint32_t lastCheckUpdate = 0;

void checkConnection();
void checkUpdate();
bool downloadUpdate(HTTPClient &http, uint32_t size = 0);

void setup()
{
    Serial.begin(9600);
    setCpuFrequencyMhz(80);

    Serial.println(F("[System]: Salak ESPGateway v1"));
    Serial.println("[System]: Firmware version: " + String(CURRENT_VERSION));
    Serial.println("[System]: Gateway ID: " + String(GW_ID));
    Serial.println("[System]: CPU Frequency: " + String(ESP.getCpuFreqMHz()) + " MHz");

    // pin led
    pinMode(LED_BUILTIN, OUTPUT);

    // LoRa setup
    pinMode(RFM95_RST, OUTPUT);
    digitalWrite(RFM95_RST, HIGH);
    delay(10);
    digitalWrite(RFM95_RST, LOW);
    delay(10);
    digitalWrite(RFM95_RST, HIGH);
    delay(10);

    if (!manager.init())
    {
        Serial.println(F("[LoRa]: RFM95 radio init failed"));
        while (1)
            ;
    }
    Serial.println(F("[LoRa]: RFM95 radio init OK!"));
    if (!rf95.setFrequency(RFM95_FREQ))
    {
        Serial.println(F("[LoRa]: setFrequency failed"));
        while (1)
            ;
    }

    rf95.setTxPower(23, false);
    // rf95.setThisAddress(GW_ID);
    // rf95.setModeRx();
    rf95.setModemConfig(RH_RF95::Bw125Cr48Sf4096);
    // manager.setThisAddress(GW_ID);
    rf95.setPayloadCRC(true);
    // rf95.setPromiscuous(true);
    manager.setRetries(0);

    // wifi setup
    WiFi.mode(WIFI_STA); // explicitly set mode, esp defaults to STA+AP

    // set timeout for portal, if no access point is found, device will restart
    wm.setConfigPortalTimeout(180);

    // res = wm.autoConnect(); // auto generated AP name from chipid
    bool res = wm.autoConnect("ESPGateway"); // anonymous ap

    if (!res)
    {
        Serial.println(F("[WiFi]: Failed to connect. Restarting..."));
        ESP.restart();
    }
    else
    {
        // if you get here you have connected to the WiFi
        Serial.println(F("[WiFi]: Connected to WiFi"));
    }
}

void loop()
{

    // read lora incoming packets
    if (manager.available())
    {
        uint8_t len = sizeof(buf);
        if (manager.recvfrom(buf, &len))
        {
            // turn on led
            digitalWrite(LED_BUILTIN, HIGH);

            Serial.print(F("[LoRa]: Received packet from node"));

            // check if incoming struct is same as formatted
            if (len == sizeof(GpsPayload))
            {
                Serial.println(", received GPS distress packet");

                // gpsPayload = (GpsPayload *)buf;
                memcpy(&gpsPayload, buf, sizeof(GpsPayload));

                // send post request to server
                http.begin(client, URL_DISTRESS);
                http.addHeader("Content-Type", "application/json");

                doc.clear();

                doc["node_id"] = gpsPayload.id;
                doc["latitude"] = gpsPayload.latitude;
                doc["longitude"] = gpsPayload.longitude;
                doc["snr"] = rf95.lastSNR();
                doc["rssi"] = rf95.lastRssi();

                String json = doc.as<String>();

                // Serial.println(json);

                int code = http.POST(json);

                Serial.println("Distress HTTP code: " + String(code));
                // Serial.println("Distress HTTP response: " + http.getString());

                if (code == HTTP_CODE_OK || code == HTTP_CODE_CREATED)
                {
                    Serial.println(F("[HTTP]: POST request success"));
                }
                else
                {
                    Serial.println(F("[HTTP]: POST request failed..."));
                }

                http.end();
            }
            else
            {
                Serial.println(", received payload packet");

                // payload = (Payload *)buf;
                memcpy(&payload, buf, sizeof(Payload));

                // send post request to server
                http.begin(client, URL);
                http.addHeader("Content-Type", "application/json");
                // int code = http.POST("{\"node_id\": " + String(payload.id) + ", \"temperature\": " + String(payload.temperature) + ", \"humidity\": " + String(payload.humidity) + ", \"light\": " + String(payload.lux) + ", \"tip\": " + String(payload.tips) + "}");

                doc.clear();

                doc["node_id"] = payload.id;
                doc["temperature"] = payload.temperature;
                doc["humidity"] = payload.humidity;
                doc["light"] = payload.lux;
                doc["tip"] = payload.tips;
                doc["snr"] = rf95.lastSNR();
                doc["rssi"] = rf95.lastRssi();

                String json = doc.as<String>();

                // Serial.println(json);

                int code = http.POST(json);

                Serial.println("Payload HTTP response code: " + String(code));
                // Serial.println("Payload HTTP response: " + http.getString());

                if (code == HTTP_CODE_OK || code == HTTP_CODE_CREATED)
                {
                    Serial.println(F("[HTTP]: POST request success"));
                }
                else
                {
                    Serial.println(F("[HTTP]: POST request failed"));
                }

                http.end();
            }

            // reset buffer
            memset(buf, 0, sizeof(buf));

            // turn off led
            digitalWrite(LED_BUILTIN, LOW);
        }
        else
        {
            Serial.println(F("[LoRa]: Receive failed"));
        }
    }

    // check for update every n minutes
    if (millis() - lastCheckUpdate > OTA_CHECK_INTERVAL)
    {

        // check wifi connection first
        checkConnection();

        Serial.println(F("[System]: Checking for update"));
        checkUpdate();
        lastCheckUpdate = millis();
    }
}

void checkConnection()
{
    // check wifi try to reconnect
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println(F("[WiFi]: WiFi disconnected, trying to reconnect"));
        bool res = wm.autoConnect("ESPGateway");

        if (!res)
        {
            Serial.println(F("[WiFi]: Failed to connect. Restarting..."));
            ESP.restart();
        }
        else
        {
            // if you get here you have connected to the WiFi
            Serial.println(F("[WiFi]: Connected to WiFi"));
        }
    }
}

void checkUpdate()
{
    String latestUpdateUrl = String(URL_OTA) + String("/latest/") + String(GW_ID);
    http.begin(client, latestUpdateUrl);

    int httpCode = http.GET();
    Serial.println("Check update: " + String(httpCode));

    if (httpCode == 200)
    {
        String payload = http.getString();

        doc.clear();
        deserializeJson(doc, payload);

        int version = doc["version"];

        if (version <= CURRENT_VERSION)
        {
            Serial.println("No update available");
            return;
        }

        // URL_OTA + /firmware/ + NODE_ID + / + VERSION + /firmware.bin
        String url = String(URL_OTA) + String("/firmware/") + String(GW_ID) + String("/") + String(version) + String("/firmware.bin");
        Serial.println("Update available");
        Serial.println("Downloading update");

        http.end();

        http.begin(client, url);

        int httpCode = http.GET();

        bool result = downloadUpdate(http, 0);

        if (result)
        {
            Serial.println("Update successful");
            // send report to server that the device is successfully updated

            doc.clear();

            doc["gatewayId"] = GW_ID;
            doc["version"] = version;

            String reportUrl = String(URL_OTA) + String("/report");

            http.begin(client, reportUrl);
            http.addHeader("Content-Type", "application/json");

            int code = http.POST(doc.as<String>());

            if (code == 200)
            {
                Serial.println("Report sent");
            }
            else
            {
                Serial.println("Failed to send report");
            }

            http.end();

            ESP.restart();
        }
        else
        {
            Serial.println("Update failed");
        }
    }
    else
    {
        Serial.println("Failed to check update");
    }

    http.end();
}

bool downloadUpdate(HTTPClient &http, uint32_t size)
{
    size = (size == 0 ? http.getSize() : size);
    if (size == 0)
    {
        return false;
    }
    WiFiClient *client = http.getStreamPtr();

    if (!Update.begin(size, U_FLASH))
    {
        Serial.printf("Update.begin failed! (%s)\n", Update.errorString());
        return false;
    }

    if (Update.writeStream(*client) != size)
    {
        Serial.printf("Update.writeStream failed! (%s)\n", Update.errorString());
        return false;
    }

    if (!Update.end())
    {
        Serial.printf("Update.end failed! (%s)\n", Update.errorString());
        return false;
    }
    return true;
}
