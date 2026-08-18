#include <WiFi.h>
#include <PubSubClient.h>

#include <FS.h>
#include "Free_Fonts.h"

#include <TFT_eSPI.h>
#include <TFT_eWidget.h>

#include <ArduinoOTA.h>
#include <Adafruit_AHTX0.h>

#include <ArduinoJson.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>

TFT_eSPI tft = TFT_eSPI();
Adafruit_AHTX0 aht;

#define CALIBRATION_FILE "/TouchCalData1"
#define REPEAT_CAL false

ButtonWidget fairyButton = ButtonWidget(&tft);
#define BUTTON_W 100
#define BUTTON_H 50
#define BUTTON_X 50
uint16_t BUTTON_Y = (BUTTON_H / 2) + 30;

ButtonWidget *btn[] = { &fairyButton };
uint8_t buttonCount = sizeof(btn) / sizeof(btn[0]);

// Time bits
struct tm timeinfo;
uint32_t targetTime = 0;
byte omm = 99;
int16_t xcolon = 0;
uint8_t hh, mm, ss;    // Get H, M, S from compile time

// Intervals. Every deadline is tested as `millis() - last >= interval`, which
// stays correct across the 49.7 day millis() rollover; the old
// `deadline <= millis()` form fires continuously for one interval at rollover.
#define CLOCK_UPDATE_MS      500
#define SENSOR_READ_MS       1000
#define SENSOR_PUBLISH_MS    10000
#define TOUCH_SCAN_MS        50
#define WIFI_RETRY_MS        10000
#define MQTT_RETRY_MS        5000
#define WIFI_BOOT_TIMEOUT_MS 30000

// getLocalTime() busy-waits for its whole timeout while the clock is unset, so
// the 5000ms default stalls the loop on every call until NTP first syncs.
#define TIME_LOOKUP_MS 10

// The resistive panel drops samples mid-press and every dropout looks like a
// fresh press to justPressed(), so one finger press could toggle several times.
#define BUTTON_DEBOUNCE_MS 400

// Digital time location
#define DIGITAL_X 200
#define DIGITAL_Y 30

// Bins
#define BINS_X 350
#define BINS_Y 280

// MQTT Broker
String toggle_topic = "fairylights/toggle";
String state_topic = "homeassistant/switch/fairy_lights_sonoff_1001ffea20_1/state";
const String humidity_topic = "homeassistant/weather/forecast_home/humidity";
const String temperature_topic = "homeassistant/weather/forecast_home/temperature";

String on_state = "on";

String screenStateTopic = "";
String availabilityTopic = "";
String mqttClientId = "";
uint32_t updateTime = 0;
uint32_t sensorTime = 0;
sensors_event_t humidity, temp;
bool ahtPresent = false;
bool sensorsValid = false;
int old_temp = 0;
int old_humid = 0;

// Outside version
int out_temp = 0;
int out_humid = 0;
int old_out_temp = 0;
int old_out_humid = 0;
bool out_temp_valid = false;
bool out_humid_valid = false;

// Wifi
WiFiClient espClient;
PubSubClient pubSubClient(espClient);

// Bin dats
uint8_t lastDay = 0;
// int gardenBin = -1;
int recycleBin = -1;
int landfillBin = -1;

// Parse a decimal integer, tolerating a trailing fraction ("12.5" -> 12).
// Returns false for the "unknown" / "unavailable" / "" payloads Home Assistant
// publishes when an entity has no value. std::stoi threw on those, and an
// uncaught throw reboots the board.
bool parseInt(const char *s, int &out) {
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s) return false;
    out = (int)v;
    return true;
}

void drawCircle(int16_t x, int16_t y, int16_t r, int16_t colour, bool fill) {
    if (fill) {
        tft.fillCircle(x, y, r-1, colour);
        tft.drawCircle(x, y, r, TFT_WHITE);
    } else {
        tft.drawCircle(x, y, r, colour);
    }
}


// Whole days from the bin epoch to now. Both ends are normalised to local
// midnight so the count ticks over at midnight rather than at midday, and
// lround absorbs the one hour skew across a daylight saving boundary.
// Returns -1 if the clock has not been set yet.
int daysDiff() {
    struct tm tm1;
    if (!getLocalTime(&tm1, TIME_LOOKUP_MS)) return -1;
    tm1.tm_hour = tm1.tm_min = tm1.tm_sec = 0;
    tm1.tm_isdst = -1;

    struct tm tm2 = { 0 };

    /* date 2: 2024-9-26 - A landfill bin day */
    tm2.tm_year = 2024 - 1900;
    tm2.tm_mon = 9 - 1;
    tm2.tm_mday = 26;
    tm2.tm_hour = tm2.tm_min = tm2.tm_sec = 0;
    tm2.tm_isdst = -1;

    time_t t1 = mktime(&tm1);
    time_t t2 = mktime(&tm2);

    double dt = difftime(t1, t2);
    return (int)lround(dt / 86400.0);
}


// void plotLinear(char *label, int x, int y)
// {
//     int w = 36;
//     tft.drawRect(x, y, w, 155, TFT_GREY);
//     tft.fillRect(x+2, y + 19, w-3, 155 - 38, TFT_WHITE);
//     tft.setTextColor(TFT_CYAN, TFT_BLACK);
//     tft.drawCentreString(label, x + w / 2, y + 2, 2);

//     for (int i = 0; i < 110; i += 10)
//     {
//         tft.drawFastHLine(x + 20, y + 27 + i, 6, TFT_BLACK);
//     }

//     for (int i = 0; i < 110; i += 50)
//     {
//         tft.drawFastHLine(x + 20, y + 27 + i, 9, TFT_BLACK);
//     }
    
//     tft.fillTriangle(x+3, y + 127, x+3+16, y+127, x + 3, y + 127 - 5, TFT_RED);
//     tft.fillTriangle(x+3, y + 127, x+3+16, y+127, x + 3, y + 127 + 5, TFT_RED);
    
//     tft.drawCentreString("---", x + w / 2, y + 155 - 18, 2);
// }


// void plotPointer(int new_value, int &old_value, int pos, int range)
// {
//     int dy = 187;
//     byte pw = 16;

//     tft.setTextColor(TFT_GREEN, TFT_BLACK);

//     char buf[8]; dtostrf(new_value, 4, 0, buf);
//     tft.drawRightString(buf, pos * 40 + 36 - 5, 187 - 27 + 155 - 18, 2);

//     int dx = 3 + 40 * pos;
//     new_value = new_value * 100 / range;

//     while (!(new_value == old_value)) {
//         dy = 187 + 100 - old_value;
//         Serial.print("DY: ");
//         Serial.println(dy);
        
//         if (old_value > new_value)
//         {
//             tft.drawLine(dx, dy - 5, dx + pw, dy, TFT_WHITE);
//             old_value--;
//             tft.drawLine(dx, dy + 6, dx + pw, dy + 1, TFT_RED);
//             delay(10);
//         }
//         else
//         {
//             tft.drawLine(dx, dy + 5, dx + pw, dy, TFT_WHITE);
//             old_value++;
//             tft.drawLine(dx, dy - 6, dx + pw, dy - 1, TFT_RED);
//             delay(10);
//         }
//     }
// }

void sendMQTTSensors() {
    // Don't publish a reading the sensor never actually gave us
    if (!sensorsValid || !pubSubClient.connected()) return;

    DynamicJsonDocument doc(1024);
    char buffer[256];

    doc["temperature"] = std::round(temp.temperature * 10.0) / 10.0;
    doc["humidity"] = std::round(humidity.relative_humidity * 10.0) / 10.0;

    size_t n = serializeJson(doc, buffer);
    if (!pubSubClient.publish(screenStateTopic.c_str(), buffer, n)) {
        Serial.println("Sensor publish failed");
    }
}

void touch_calibrate() {
    uint16_t calData[5];
    uint8_t calDataOK = 0;

    // check file system exists
    if (!LittleFS.begin()) {
        Serial.println("Formating file system");
        LittleFS.format();
        LittleFS.begin();
    }

    // check if calibration file exists and size is correct
    if (LittleFS.exists(CALIBRATION_FILE)) {
        if (REPEAT_CAL) {
            // Delete if we want to re-calibrate
            LittleFS.remove(CALIBRATION_FILE);
        } else {
            File f = LittleFS.open(CALIBRATION_FILE, "r");
            if (f) {
                if (f.readBytes((char *)calData, 14) == 14)
                    calDataOK = 1;
                f.close();
            }
        }
    }

    if (calDataOK && !REPEAT_CAL) {
        // calibration data valid
        tft.setTouch(calData);
    } else {
        // data not valid so recalibrate
        tft.fillScreen(TFT_BLACK);
        tft.setCursor(20, 0);
        tft.setTextSize(1);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);

        tft.println("Touch corners as indicated");

        tft.setTextFont(1);
        tft.println();

        if (REPEAT_CAL) {
            tft.setTextColor(TFT_RED, TFT_BLACK);
            tft.println("Set REPEAT_CAL to false to stop this running again!");
        }

        tft.calibrateTouch(calData, TFT_MAGENTA, TFT_BLACK, 15);

        tft.setTextColor(TFT_GREEN, TFT_BLACK);
        tft.println("Calibration complete!");

        // store data
        File f = LittleFS.open(CALIBRATION_FILE, "w");
        if (f) {
            f.write((const unsigned char *)calData, 14);
            f.close();
        }
    }
}

void callback(char *topic, byte *payload, unsigned int length) {
    tft.setFreeFont(FF18);
    String sTopic(topic);
    String sPayload(payload, length);

    Serial.println(sTopic);
    Serial.println(sPayload);

    if (sTopic == state_topic) {
        if (sPayload == on_state) {
            Serial.println("New state is on");
            fairyButton.drawSmoothButton(true, 3, TFT_BLACK, "ON");
        } else {
            Serial.println("New state is off");
            fairyButton.drawSmoothButton(false, 3, TFT_BLACK, "OFF");
        }
    } else if (sTopic == humidity_topic) {
        if (parseInt(sPayload.c_str(), out_humid)) {
            out_humid_valid = true;
            Serial.printf("New humidity: %d\n", out_humid);
        } else {
            Serial.printf("Ignoring non-numeric humidity: %s\n", sPayload.c_str());
        }
    } else if (sTopic == temperature_topic) {
        if (parseInt(sPayload.c_str(), out_temp)) {
            out_temp_valid = true;
            Serial.printf("New temperature: %d\n", out_temp);
        } else {
            Serial.printf("Ignoring non-numeric temperature: %s\n", sPayload.c_str());
        }
    }
}

// One connection attempt. Never blocks waiting for a retry; the caller decides
// when to try again so the display, clock and OTA keep running meanwhile.
bool mqttConnect() {
    Serial.printf("The client %s connects to the MQTT broker\n", mqttClientId.c_str());

    // Retained last will, so Home Assistant can see when the screen drops off.
    const char *user = (sizeof(MQTT_USER) > 1) ? MQTT_USER : NULL;
    const char *pwd  = (sizeof(MQTT_PWD) > 1) ? MQTT_PWD : NULL;
    bool ok = pubSubClient.connect(mqttClientId.c_str(), user, pwd,
                                   availabilityTopic.c_str(), 0, true, "offline");
    if (!ok) {
        Serial.printf("MQTT connect failed, state %d\n", pubSubClient.state());
        return false;
    }

    Serial.println("MQTT broker connected");
    pubSubClient.publish(availabilityTopic.c_str(), "online", true);
    pubSubClient.subscribe(state_topic.c_str());
    pubSubClient.subscribe(humidity_topic.c_str());
    pubSubClient.subscribe(temperature_topic.c_str());
    return true;
}

void ensureMqtt() {
    static uint32_t lastAttempt = 0;

    if (pubSubClient.connected() || WiFi.status() != WL_CONNECTED) return;
    if (millis() - lastAttempt < MQTT_RETRY_MS) return;

    lastAttempt = millis();
    mqttConnect();
}

void ensureWifi() {
    static uint32_t lastAttempt = 0;
    static bool wasConnected = true;

    if (WiFi.status() == WL_CONNECTED) {
        if (!wasConnected) {
            Serial.print("WiFi reconnected, IP address: ");
            Serial.println(WiFi.localIP());
            wasConnected = true;
        }
        return;
    }

    if (wasConnected) {
        Serial.println("WiFi connection lost");
        wasConnected = false;
        lastAttempt = millis();
    }

    // Association takes several seconds. Calling WiFi.begin() every 500ms, as
    // this used to, restarts the attempt before it can ever complete.
    if (millis() - lastAttempt >= WIFI_RETRY_MS) {
        lastAttempt = millis();
        Serial.println("Reconnecting WiFi");
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PWD);
    }
}

void fairyButton_pressAction(void) {
    if (!fairyButton.justPressed()) return;

    // Guard against a flickering touch reading re-triggering justPressed()
    if (millis() - fairyButton.getPressTime() < BUTTON_DEBOUNCE_MS) return;
    fairyButton.setPressTime(millis());

    Serial.println("Button toggled");
    if (!pubSubClient.publish(toggle_topic.c_str(), "Light toggle.")) {
        Serial.println("Toggle publish failed");
    }
}

void initButtons() {
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(BUTTON_X - 10, BUTTON_Y - 10);
    tft.print("Fairy Lights");

    String q("OFF");
    fairyButton.initButtonUL(BUTTON_X, BUTTON_Y, BUTTON_W, BUTTON_H, TFT_WHITE, TFT_BLACK, TFT_GREEN, (char *)q.c_str(), 1);
    fairyButton.setPressAction(fairyButton_pressAction);
    fairyButton.drawSmoothButton(false, 3, TFT_BLACK); // 3 is outline width, TFT_BLACK is the surrounding background colour for anti-aliasing
}

void printClock() {
    if (millis() - targetTime < CLOCK_UPDATE_MS) return;
    targetTime = millis();

    // Short timeout, and skip the draw entirely until NTP has actually synced
    // rather than briefly rendering a 1970 date.
    if (!getLocalTime(&timeinfo, TIME_LOOKUP_MS)) return;

    ss = timeinfo.tm_sec;
    mm = timeinfo.tm_min;
    hh = timeinfo.tm_hour;

    tft.setFreeFont(FF17);
    tft.setTextSize(2);

    // Update digital time
    int16_t xpos = DIGITAL_X;
    int16_t ypos = DIGITAL_Y;

    if (omm != mm) { // Only redraw every minute to minimise flicker
        // Uncomment ONE of the next 2 lines, using the ghost image demonstrates text overlay as time is drawn over it
        // tft.setTextColor(0x39C4, TFT_BLACK);    // Leave a 7 segment ghost image, comment out next line!
        tft.setTextColor(TFT_BLACK, TFT_BLACK); // Set font colour to black to wipe image
        // Font 7 is to show a pseudo 7 segment display.
        // Font 7 only contains characters [space] 0 1 2 3 4 5 6 7 8 9 0 : .
        tft.drawString("88:88",xpos,ypos,7); // Overwrite the text to clear it
        tft.setTextColor(TFT_GREEN); // Orange
        omm = mm;

        if (hh<10) xpos += tft.drawChar('0',xpos,ypos,7);
        xpos += tft.drawNumber(hh,xpos,ypos,7);
        xcolon = xpos;
        xpos += tft.drawChar(':',xpos,ypos,7);
        if (mm<10) xpos += tft.drawChar('0',xpos,ypos,7);
        tft.drawNumber(mm,xpos,ypos,7);
    }

    if (ss%2) { // Flash the colon
        tft.setTextColor(0x39C4, TFT_BLACK);
        tft.drawChar(':',xcolon,ypos,7);
    } else {
        tft.setTextColor(TFT_GREEN, TFT_BLACK);
        tft.drawChar(':',xcolon,ypos,7);
    }

    if (lastDay != timeinfo.tm_mday) {
        lastDay = timeinfo.tm_mday;

        // Day

        tft.setFreeFont(FF24);
        tft.setTextSize(2);
        tft.fillRect(DIGITAL_X, DIGITAL_Y + 100, 280, 100, TFT_BLACK);
        tft.setTextColor(TFT_GREEN, TFT_BLACK);
        tft.setCursor (DIGITAL_X + 80, DIGITAL_Y + 180);
        char ptr[5];
        int rc = strftime(ptr, 5, "%a", &timeinfo);
        tft.print(ptr);

        int timelapse = daysDiff();

        // Landfill is fortnightly from start date
        landfillBin = 14 - (timelapse%14);
        // Recycle is fortnightly from week after start date
        recycleBin = 14 - ((timelapse + 7)%14);
        // Garden bin is 4 weekly from start date
        // gardenBin = 28 - (timelapse%28);

        tft.setFreeFont(FF19);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setTextSize(1);
        tft.fillRect(BINS_X - 110, BINS_Y -20, 260, 100, TFT_BLACK);
        tft.drawString("Bins:", BINS_X - 55, BINS_Y -16);

        if (landfillBin < 7) {
            drawCircle(BINS_X + 50, BINS_Y, 20, TFT_RED, 1);
        } else {
            drawCircle(BINS_X + 50, BINS_Y, 20, TFT_YELLOW, 1);
        }




        // if (gardenBin<7) {
        //     drawCircle(BINS_X + 100, BINS_Y, 20, TFT_GREEN, true);
        // } else {
        //     tft.setTextColor(TFT_GREEN, TFT_BLACK);
        //     tft.drawNumber((gardenBin - (gardenBin % 7)) / 7, BINS_X + 90, BINS_Y - 15);
        //     drawCircle(BINS_X + 100, BINS_Y, 20, TFT_GREEN, false);
        // }
    }
    tft.setTextSize(1);
}

void setup() {
    Serial.begin(115200);
    Serial.println("Staring");

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);
    WiFi.begin(WIFI_SSID, WIFI_PWD);

    // Bounded wait: the clock and the local sensor work without WiFi, so a
    // router slower to boot than we are must not strand the display forever.
    uint32_t wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < WIFI_BOOT_TIMEOUT_MS) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("WiFi connected. IP address: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("No WiFi at boot, continuing - loop() will keep retrying");
    }

    // Zero padded hex. The old decimal concatenation was ambiguous: MAC bytes
    // 0x01,0x12 and 0x11,0x02 both rendered as "112".
    byte wifi_mac[6];
    WiFi.macAddress(wifi_mac);
    char macHex[9];
    snprintf(macHex, sizeof(macHex), "%02X%02X%02X%02X",
             wifi_mac[5], wifi_mac[4], wifi_mac[3], wifi_mac[2]);
    screenStateTopic = String("home/screen/") + macHex + "/state";
    availabilityTopic = String("home/screen/") + macHex + "/availability";
    mqttClientId = String("esp32-client-") + macHex;
    Serial.printf("State topic:        %s\n", screenStateTopic.c_str());
    Serial.printf("Availability topic: %s\n", availabilityTopic.c_str());

    configTzTime(TIMEZONE, "nz.pool.ntp.org");
    getLocalTime(&timeinfo, TIME_LOOKUP_MS);

    tft.begin();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);
    tft.setFreeFont(FF18);

    // Calibrate the touch screen and retrieve the scaling factors
    touch_calibrate();
    initButtons();

    // char q[] = "C";
    // plotLinear(q, 40, 160);
    // char r[] = "%H";
    // plotLinear(r, 120, 160);

    pubSubClient.setServer(MQTT_BROKER, MQTT_PORT);
    pubSubClient.setCallback(callback);
    mqttConnect();  // one attempt; ensureMqtt() retries if it fails

    // Carry on without the sensor rather than spinning forever: the clock, the
    // bins and the outside readings still work, and OTA still works.
    ahtPresent = aht.begin();
    if (!ahtPresent) {
        Serial.println("Could not find AHT? Check wiring");
    }

    ArduinoOTA
    .onStart([]() {
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH)
            type = "sketch";
        else // U_SPIFFS
            type = "filesystem";

        // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
        Serial.println("Start updating " + type);
    })
    .onEnd([]() {
        Serial.println("\nEnd");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
        if (total) Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    })
    .onError([](ota_error_t error) {
        Serial.printf("Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
        else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
        else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
        else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });

    ArduinoOTA.setHostname("mpi3501");
    ArduinoOTA.begin();

}

void loop() {
    static uint32_t scanTime = 0;
    uint16_t t_x = 0, t_y = 0; // To store the touch coordinates

    // Without this the OTA server never services a request, so espota uploads
    // silently fail however the rest of the sketch behaves.
    ArduinoOTA.handle();

    // Scan keys every 50ms at most
    if (millis() - scanTime >= TOUCH_SCAN_MS) {
        // Pressed will be set true if there is a valid touch on the screen
        bool pressed = tft.getTouch(&t_x, &t_y);
        if (pressed) Serial.printf("Touch coordinates: %d, %d\n", t_x, t_y);
        scanTime = millis();
        for (uint8_t b = 0; b < buttonCount; b++) {
            // Drive the state on every scan, including when the touch lands
            // outside the button: skipping it leaves the press state stale.
            btn[b]->press(pressed && btn[b]->contains(t_x, t_y));
            btn[b]->pressAction();
            btn[b]->releaseAction();
        }
    }

    ensureWifi();
    ensureMqtt();
    pubSubClient.loop();
    printClock();

    if (ahtPresent && millis() - updateTime >= SENSOR_READ_MS) {
        updateTime = millis();
        // populate temp and humidity objects with fresh data
        if (!aht.getEvent(&humidity, &temp)) {
            if (sensorsValid) Serial.println("AHT read failed");
            sensorsValid = false;
        } else {
            sensorsValid = true;
        }
        // plotPointer(int(temp.temperature), old_temp, 1, 40);
        // plotPointer(int(humidity.relative_humidity), old_humid, 3, 100);

        if (sensorsValid && int(temp.temperature) != old_temp) {
            old_temp = int(temp.temperature);

            tft.setFreeFont(FF3);
            tft.setTextSize(1);

            // Update digital time
            int16_t xpos = 30;
            int16_t ypos = 140;

            tft.setTextColor(TFT_BLACK, TFT_BLACK); // Set font colour to black to wipe image
            // Font 7 is to show a pseudo 7 segment display.
            // Font 7 only contains characters [space] 0 1 2 3 4 5 6 7 8 9 0 : .
            tft.drawString("888",xpos,ypos); // Overwrite the text to clear it
            tft.setTextColor(TFT_DARKCYAN); // Orange
            tft.fillRect(xpos, ypos-20, 80, 80, TFT_BLACK);

            xpos += tft.drawNumber(int(temp.temperature),xpos,ypos,7);
            tft.drawChar('c',xpos,ypos+40);

        }
        if (sensorsValid && int(humidity.relative_humidity) != old_humid) {
            old_humid = int(humidity.relative_humidity);

            tft.setFreeFont(FF3);
            tft.setTextSize(1);

            // Update digital time
            int16_t xpos = 130;
            int16_t ypos = 140;
            tft.fillRect(xpos, ypos-20, 80, 80, TFT_BLACK);

            tft.setTextColor(TFT_BLACK, TFT_BLACK); // Set font colour to black to wipe image
            // Font 7 is to show a pseudo 7 segment display.
            // Font 7 only contains characters [space] 0 1 2 3 4 5 6 7 8 9 0 : .
            tft.drawString("888",xpos,ypos); // Overwrite the text to clear it
            tft.setTextColor(TFT_DARKCYAN); // Orange
            
            xpos += tft.drawNumber(int(humidity.relative_humidity),xpos,ypos,7);
            tft.drawChar('%',xpos,ypos+40);
        }




        if (out_temp_valid && out_temp != old_out_temp) {
            old_out_temp = out_temp;

            tft.setFreeFont(FF3);
            tft.setTextSize(1);

            // Update digital time
            int16_t xpos = 30;
            int16_t ypos = 220;

            tft.setTextColor(TFT_BLACK, TFT_BLACK); // Set font colour to black to wipe image
            // Font 7 is to show a pseudo 7 segment display.
            // Font 7 only contains characters [space] 0 1 2 3 4 5 6 7 8 9 0 : .
            tft.drawString("888",xpos,ypos); // Overwrite the text to clear it
            tft.setTextColor(TFT_DARKCYAN); // Orange
            tft.fillRect(xpos, ypos-20, 80, 80, TFT_BLACK);

            xpos += tft.drawNumber(out_temp,xpos,ypos,7);
            tft.drawChar('c',xpos,ypos+40);

        }
        if (out_humid_valid && out_humid != old_out_humid) {
            old_out_humid = out_humid;

            tft.setFreeFont(FF3);
            tft.setTextSize(1);

            // Update digital time
            int16_t xpos = 130;
            int16_t ypos = 220;
            tft.fillRect(xpos, ypos-20, 80, 80, TFT_BLACK);

            tft.setTextColor(TFT_BLACK, TFT_BLACK); // Set font colour to black to wipe image
            // Font 7 is to show a pseudo 7 segment display.
            // Font 7 only contains characters [space] 0 1 2 3 4 5 6 7 8 9 0 : .
            tft.drawString("888",xpos,ypos); // Overwrite the text to clear it
            tft.setTextColor(TFT_DARKCYAN); // Orange
            
            xpos += tft.drawNumber(out_humid,xpos,ypos,7);
            tft.drawChar('%',xpos,ypos+40);
        }
    }




    if (millis() - sensorTime >= SENSOR_PUBLISH_MS) {
        sensorTime = millis();
        sendMQTTSensors();
    }
}
