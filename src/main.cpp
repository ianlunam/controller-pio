#include <Preferences.h>
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
#include <time.h>

TFT_eSPI tft = TFT_eSPI();
Adafruit_AHTX0 aht;

#define CALIBRATION_FILE "/TouchCalData1"
#define REPEAT_CAL false

ButtonWidget btnR = ButtonWidget(&tft);

#define BUTTON_W 100
#define BUTTON_H 50

ButtonWidget *btn[] = { &btnR };
uint8_t buttonCount = sizeof(btn) / sizeof(btn[0]);

// Time bits
struct tm timeinfo;
const char* tz = "NZST-12NZDT,M9.5.0,M4.1.0/3";
uint32_t targetTime = 0;
byte omm = 99;
bool initial = 1;
int16_t xcolon = 0;
uint8_t hh, mm, ss;    // Get H, M, S from compile time
// Digital time location
int16_t digitalXPos = 200;
int16_t digitalYPos = 100;

String ssid, pwd;

// MQTT Broker
const char *mqtt_broker = "mqtt.local";
const char *toggle_topic = "fairylights/toggle";
const char *state_topic = "homeassistant/switch/sonoff_1001ffea20_1/state";
byte on_state[] = {'o','n'};
const int mqtt_port = 1883;

String screenStateTopic = "";
uint32_t updateTime = 0;
uint32_t sensorTime = 0;
sensors_event_t humidity, temp;
int old_temp = 0;
int old_humid = 0;

byte wifi_mac[6];

WiFiClient espClient;
PubSubClient client(espClient);


void plotLinear(char *label, int x, int y)
{
    int w = 36;
    tft.drawRect(x, y, w, 155, TFT_GREY);
    tft.fillRect(x+2, y + 19, w-3, 155 - 38, TFT_WHITE);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawCentreString(label, x + w / 2, y + 2, 2);

    for (int i = 0; i < 110; i += 10)
    {
        tft.drawFastHLine(x + 20, y + 27 + i, 6, TFT_BLACK);
    }

    for (int i = 0; i < 110; i += 50)
    {
        tft.drawFastHLine(x + 20, y + 27 + i, 9, TFT_BLACK);
    }
    
    tft.fillTriangle(x+3, y + 127, x+3+16, y+127, x + 3, y + 127 - 5, TFT_RED);
    tft.fillTriangle(x+3, y + 127, x+3+16, y+127, x + 3, y + 127 + 5, TFT_RED);
    
    tft.drawCentreString("---", x + w / 2, y + 155 - 18, 2);
}


void plotPointer(int new_value, int &old_value, int pos, int range)
{
    int dy = 187;
    byte pw = 16;

    tft.setTextColor(TFT_GREEN, TFT_BLACK);

    char buf[8]; dtostrf(new_value, 4, 0, buf);
    tft.drawRightString(buf, pos * 40 + 36 - 5, 187 - 27 + 155 - 18, 2);

    int dx = 3 + 40 * pos;
    new_value = new_value * 100 / range;

    while (!(new_value == old_value)) {
        dy = 187 + 100 - old_value;
        Serial.print("DY: ");
        Serial.println(dy);
        
        if (old_value > new_value)
        {
            tft.drawLine(dx, dy - 5, dx + pw, dy, TFT_WHITE);
            old_value--;
            tft.drawLine(dx, dy + 6, dx + pw, dy + 1, TFT_RED);
            delay(10);
        }
        else
        {
            tft.drawLine(dx, dy + 5, dx + pw, dy, TFT_WHITE);
            old_value++;
            tft.drawLine(dx, dy - 6, dx + pw, dy - 1, TFT_RED);
            delay(10);
        }
    }
}

void sendMQTTSensors() {
    DynamicJsonDocument doc(1024);
    char buffer[256];

    doc["temperature"] = temp.temperature;
    doc["humidity"] = humidity.relative_humidity;

    size_t n = serializeJson(doc, buffer);
    bool b = client.publish(screenStateTopic.c_str(), buffer, n);
    Serial.print("Sensor response: ");
    Serial.println(b);
    Serial.println(screenStateTopic);
    Serial.println(buffer);
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
    if (memcmp(payload, on_state, sizeof(on_state)) == 0) {
        Serial.println("New state is on");
        btnR.drawSmoothButton(true, 3, TFT_BLACK, "ON");
    } else {
        Serial.println("New state is off");
        btnR.drawSmoothButton(false, 3, TFT_BLACK, "OFF");
    }
}

void btnR_pressAction(void) {
    if (btnR.justPressed()) {
        Serial.println("Button toggled");
        client.publish(toggle_topic, "Light toggle.");
        btnR.setPressTime(millis());
    }
}

void btnR_releaseAction(void) {
    // Not action
}

void initButtons() {
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(5, 30);
    tft.print("Fairy Lights");
    
    // uint16_t x = (tft.width() - BUTTON_W) / 2;
    // uint16_t y = tft.height() / 2 - BUTTON_H - 10;
    uint16_t x = 15;
    uint16_t y = (BUTTON_H / 2) + 20;

    char q[] = "OFF";
    btnR.initButtonUL(x, y, BUTTON_W, BUTTON_H, TFT_WHITE, TFT_BLACK, TFT_GREEN, q, 1);
    btnR.setPressAction(btnR_pressAction);
    //btnR.setReleaseAction(btnR_releaseAction);
    btnR.drawSmoothButton(false, 3, TFT_BLACK); // 3 is outline width, TFT_BLACK is the surrounding background colour for anti-aliasing

}

void printClock() {
    getLocalTime(&timeinfo);
    ss = timeinfo.tm_sec;
    mm = timeinfo.tm_min;
    hh = timeinfo.tm_hour;
    
    if (targetTime < millis()) {
        tft.setFreeFont(FF17);
        tft.setTextSize(2);
        targetTime = millis()+500;

        // Update digital time
        int16_t xpos = digitalXPos;
        int16_t ypos = digitalYPos;

        // if (ss==0 || initial) {
        //     initial = 0;
        //     tft.setTextColor(TFT_GREEN, TFT_BLACK);
        //     tft.setCursor (50, ypos + 60);

        //     char ptr[20];
        //     int rc = strftime(ptr, 20, "%a %e %b", &timeinfo);
        //     tft.print(ptr);
        // }


        if (omm != mm) { // Only redraw every minute to minimise flicker
            // Uncomment ONE of the next 2 lines, using the ghost image demonstrates text overlay as time is drawn over it
            tft.setTextColor(0x39C4, TFT_BLACK);    // Leave a 7 segment ghost image, comment out next line!
            //tft.setTextColor(TFT_BLACK, TFT_BLACK); // Set font colour to black to wipe image
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
            xpos+= tft.drawChar(':',xcolon,ypos,7);
        } else {
            tft.setTextColor(TFT_GREEN, TFT_BLACK);
            tft.drawChar(':',xcolon,ypos,7);
        }
        tft.setTextSize(1);
    } 
}

void setup() {
    Serial.begin(115200);
    Serial.println("Staring");

    // Get WiFi creds from preferences storage
    Preferences wifiCreds;
    wifiCreds.begin("wifiCreds", true);
    ssid = wifiCreds.getString("ssid");
    pwd = wifiCreds.getString("password");
    wifiCreds.end();

    WiFi.begin(ssid.c_str(), pwd.c_str());
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }

    WiFi.macAddress(wifi_mac);
    screenStateTopic = "home/screen/" + String(wifi_mac[5]) + String(wifi_mac[4]) + String(wifi_mac[3]) + String(wifi_mac[2]) + "/state";

    Serial.println("\nWiFi connected.");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    configTzTime(tz, "nz.pool.ntp.org");
    getLocalTime(&timeinfo);
    targetTime = millis() + 500;

    tft.begin();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);
    tft.setFreeFont(FF18);

    // Calibrate the touch screen and retrieve the scaling factors
    touch_calibrate();
    initButtons();

    char q[] = "C";
    plotLinear(q, 0, 160);
    char r[] = "%H";
    plotLinear(r, 40, 160);

    client.setServer(mqtt_broker, mqtt_port);
    client.setCallback(callback);
    while (!client.connected()) {
        String client_id = "esp32-client-";
        client_id += String(WiFi.macAddress());
        Serial.printf("The client %s connects to the public MQTT broker\n", client_id.c_str());
        if (client.connect(client_id.c_str())) { 
            Serial.println("EMQX MQTT broker connected");
        } else {
            Serial.print("failed with state ");
            Serial.print(client.state());
            delay(2000);
        }
    }
    client.subscribe(state_topic);

    if (! aht.begin()) {
        Serial.println("Could not find AHT? Check wiring");
        while (1) delay(10);
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
        Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
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
    static uint32_t scanTime = millis();
    uint16_t t_x = 9999, t_y = 9999; // To store the touch coordinates

    // Scan keys every 50ms at most
    if (millis() - scanTime >= 50) {
        // Pressed will be set true if there is a valid touch on the screen
        bool pressed = tft.getTouch(&t_x, &t_y);
        scanTime = millis();
        for (uint8_t b = 0; b < buttonCount; b++) {
            if (pressed) {
                if (btn[b]->contains(t_x, t_y)) {
                    btn[b]->press(true);
                    btn[b]->pressAction();
                }
            } else {
                btn[b]->press(false);
                btn[b]->releaseAction();
            }
        }
    }

    client.loop();
    printClock();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Reconnecting WiFi");
        WiFi.begin(ssid.c_str(), pwd.c_str());
        delay(500);
    }

    if (updateTime <= millis()) {
        updateTime = millis() + 1000;
        aht.getEvent(&humidity, &temp);// populate temp and humidity objects with fresh data
        plotPointer(int(temp.temperature), old_temp, 0, 40);
        plotPointer(int(humidity.relative_humidity), old_humid, 1, 100);
    }

    if (sensorTime <= millis()) {
        sensorTime = millis() + 10000;
        sendMQTTSensors();
    }
}

