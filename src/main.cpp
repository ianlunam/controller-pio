#include <WiFi.h>
#include <PubSubClient.h>

#include <FS.h>
#include <LittleFS.h>
#include "Free_Fonts.h"

#include <TFT_eSPI.h>
#include <TFT_eWidget.h>

#include <ArduinoOTA.h>
#include <Adafruit_AHTX0.h>

#include <ArduinoJson.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Build-time configuration checks
//
// platformio.ini pulls these out of ../.secrets. When a key was missing the
// build used to succeed with an empty value, which then looked like a runtime
// WiFi fault. Fail at compile time instead. sizeof("") is 1, so a literal of
// size 1 is an empty string.
// ---------------------------------------------------------------------------
#if !defined(WIFI_SSID) || !defined(WIFI_PWD) || !defined(TIMEZONE) || \
    !defined(MQTT_BROKER) || !defined(MQTT_PORT) || \
    !defined(MQTT_USER) || !defined(MQTT_PWD)
#error "Secret build flags missing - see scripts/secret.sh and ../.secrets"
#endif

static_assert(sizeof(WIFI_SSID) > 1, "WIFI_SSID is empty - check ../.secrets");
static_assert(sizeof(WIFI_PWD) > 1, "WIFI_PWD is empty - check ../.secrets");
static_assert(sizeof(TIMEZONE) > 1, "TIMEZONE is empty - check ../.secrets");
static_assert(sizeof(MQTT_BROKER) > 1, "MQTT_BROKER is empty - check ../.secrets");
static_assert(sizeof(MQTT_USER) > 1, "MQTT_USER is empty - check ../.secrets");
static_assert(sizeof(MQTT_PWD) > 1, "MQTT_PWD is empty - check ../.secrets");
// MQTT_PORT is emitted unquoted by secret.sh --raw, so it is checked as a
// number rather than a string literal.
static_assert(MQTT_PORT > 0 && MQTT_PORT <= 65535, "MQTT_PORT out of range");

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
#define CALIBRATION_FILE "/TouchCalData1"
static constexpr bool REPEAT_CAL = false;

// Fairy light button
static constexpr int16_t BUTTON_W = 100;
static constexpr int16_t BUTTON_H = 50;
static constexpr int16_t BUTTON_X = 50;
static constexpr int16_t BUTTON_Y = (BUTTON_H / 2) + 30;

// Digital clock
static constexpr int16_t DIGITAL_X = 200;
static constexpr int16_t DIGITAL_Y = 30;

// Bins
static constexpr int16_t BINS_X = 350;
static constexpr int16_t BINS_Y = 280;

// Numeric readouts: temperature left, humidity right; inside above, outside below
static constexpr int16_t READOUT_TEMP_X    = 30;
static constexpr int16_t READOUT_HUMID_X   = 130;
static constexpr int16_t READOUT_INSIDE_Y  = 140;
static constexpr int16_t READOUT_OUTSIDE_Y = 220;
static constexpr int16_t READOUT_W = 80;
static constexpr int16_t READOUT_H = 80;

// MQTT topics. Plain literals rather than String globals: these are compared on
// every inbound message, and repeated small heap allocations fragment the heap
// over the months this thing stays powered on.
static constexpr const char *TOGGLE_TOPIC      = "fairylights/toggle";
static constexpr const char *STATE_TOPIC       = "homeassistant/switch/fairy_lights_sonoff_1001ffea20_1/state";
static constexpr const char *HUMIDITY_TOPIC    = "homeassistant/weather/forecast_home/humidity";
static constexpr const char *TEMPERATURE_TOPIC = "homeassistant/weather/forecast_home/temperature";
static constexpr const char *ON_STATE          = "on";
static constexpr const char *PAYLOAD_ONLINE    = "online";
static constexpr const char *PAYLOAD_OFFLINE   = "offline";

// Intervals. Every deadline below is tested as `millis() - last >= interval`,
// which stays correct across the 49.7 day millis() rollover. The old
// `deadline <= millis()` form fires continuously for one interval at rollover.
static constexpr uint32_t TOUCH_SCAN_MS        = 50;
static constexpr uint32_t CLOCK_UPDATE_MS      = 500;
static constexpr uint32_t SENSOR_READ_MS       = 1000;
static constexpr uint32_t SENSOR_PUBLISH_MS    = 10000;
static constexpr uint32_t WIFI_RETRY_MS        = 10000;
static constexpr uint32_t MQTT_RETRY_MS        = 5000;
static constexpr uint32_t WIFI_BOOT_TIMEOUT_MS = 30000;

// getLocalTime() busy-waits for its whole timeout while the clock is unset, so
// the 5000ms default would stall the loop on every call until NTP first syncs.
static constexpr uint32_t TIME_LOOKUP_MS = 10;

// The resistive panel drops samples mid-press, and every dropout looks like a
// fresh press to justPressed(), so one finger press could toggle several times.
static constexpr uint32_t BUTTON_DEBOUNCE_MS = 400;

// Bin schedule anchor: 2024-09-26 was a landfill collection day. Recycle is the
// same fortnightly cycle offset by a week.
enum BinType { BIN_LANDFILL, BIN_RECYCLE };
static constexpr int BIN_EPOCH_YEAR  = 2024;
static constexpr int BIN_EPOCH_MONTH = 9;
static constexpr int BIN_EPOCH_DAY   = 26;

// ---------------------------------------------------------------------------
// Hardware
// ---------------------------------------------------------------------------
static TFT_eSPI tft = TFT_eSPI();
static Adafruit_AHTX0 aht;
static WiFiClient espClient;
static PubSubClient pubSubClient(espClient);

static ButtonWidget fairyButton = ButtonWidget(&tft);
static ButtonWidget *btn[] = { &fairyButton };
static constexpr uint8_t buttonCount = sizeof(btn) / sizeof(btn[0]);

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
// Derived from the MAC at boot and logged, so the topics are discoverable.
static char screenStateTopic[64];
static char availabilityTopic[64];
static char mqttClientId[32];

static struct tm timeinfo;
static uint8_t lastDrawnMinute = 99;  // impossible value, forces the first draw
static uint8_t lastDrawnDay    = 0;
static int16_t xcolon          = 0;

static sensors_event_t humidityEvent, tempEvent;
static bool ahtPresent   = false;
static bool sensorsValid = false;

// INT_MIN means "nothing drawn yet", so a genuine reading of 0 still draws.
static int shownInsideTemp   = INT_MIN;
static int shownInsideHumid  = INT_MIN;
static int shownOutsideTemp  = INT_MIN;
static int shownOutsideHumid = INT_MIN;

static int  outsideTemp       = 0;
static int  outsideHumid      = 0;
static bool outsideTempValid  = false;
static bool outsideHumidValid = false;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Parse a decimal integer, tolerating a trailing fraction ("12.5" -> 12).
// Returns false for the "unknown" / "unavailable" / "" payloads Home Assistant
// publishes when an entity has no value. std::stoi used to be used here, which
// throws on those, and an uncaught throw reboots the board.
static bool parseInt(const char *s, int &out) {
    char *end = nullptr;
    long v = strtol(s, &end, 10);
    if (end == s) return false;
    out = static_cast<int>(v);
    return true;
}

static void drawCircle(int16_t x, int16_t y, int16_t r, uint16_t colour, bool fill) {
    if (fill) {
        tft.fillCircle(x, y, r - 1, colour);
        tft.drawCircle(x, y, r, TFT_WHITE);
    } else {
        tft.drawCircle(x, y, r, colour);
    }
}

// Redraw one numeric readout, and only when the value actually changed: the SPI
// display is slow enough that a needless redraw shows up as a visible flicker.
static void drawValue(int value, int &shown, int16_t x, int16_t y, char unit) {
    if (value == shown) return;
    shown = value;

    tft.setFreeFont(FF3);
    tft.setTextSize(1);
    tft.fillRect(x, y - 20, READOUT_W, READOUT_H, TFT_BLACK);
    tft.setTextColor(TFT_DARKCYAN);

    // Font 7 is a pseudo 7 segment face: [space] 0-9 : . only, hence the
    // separate drawChar for the unit in the current free font.
    int16_t cursor = x + tft.drawNumber(value, x, y, 7);
    tft.drawChar(unit, cursor, y + 40);
}

// Whole days from the bin epoch to `nowLocal`. Both ends are normalised to
// local midnight so the count ticks over at midnight rather than at midday,
// and lround absorbs the one hour skew across a daylight saving boundary.
static int daysSinceBinEpoch(const struct tm &nowLocal) {
    struct tm today = nowLocal;
    today.tm_hour = today.tm_min = today.tm_sec = 0;
    today.tm_isdst = -1;

    struct tm epoch = {};
    epoch.tm_year  = BIN_EPOCH_YEAR - 1900;
    epoch.tm_mon   = BIN_EPOCH_MONTH - 1;
    epoch.tm_mday  = BIN_EPOCH_DAY;
    epoch.tm_isdst = -1;

    time_t t1 = mktime(&today);
    time_t t2 = mktime(&epoch);
    return static_cast<int>(lround(difftime(t1, t2) / 86400.0));
}

// Which bin goes out next, which is what the single coloured circle shows:
// red for landfill, yellow for recycle. The two collections are the same
// fortnightly cycle a week apart. Collection day itself counts as zero days
// away, so the circle holds the colour of the bin due that morning and only
// flips once the day is over.
static BinType nextBinType(int daysSinceEpoch) {
    // Floored modulo, so a clock reading a date before the epoch still lands
    // in [0,14) instead of going negative.
    const int phase      = ((daysSinceEpoch % 14) + 14) % 14;
    const int toLandfill = (14 - phase) % 14;  // 0 on landfill collection day
    const int toRecycle  = (21 - phase) % 14;  // 0 on recycle collection day
    return toLandfill < toRecycle ? BIN_LANDFILL : BIN_RECYCLE;
}

// ---------------------------------------------------------------------------
// Touch calibration
// ---------------------------------------------------------------------------
static void touch_calibrate() {
    uint16_t calData[5];
    uint8_t calDataOK = 0;

    if (!LittleFS.begin()) {
        Serial.println("Formatting file system");
        LittleFS.format();
        LittleFS.begin();
    }

    if (LittleFS.exists(CALIBRATION_FILE)) {
        if (REPEAT_CAL) {
            LittleFS.remove(CALIBRATION_FILE);
        } else {
            File f = LittleFS.open(CALIBRATION_FILE, "r");
            if (f) {
                if (f.readBytes((char *)calData, 14) == 14) calDataOK = 1;
                f.close();
            }
        }
    }

    if (calDataOK && !REPEAT_CAL) {
        tft.setTouch(calData);
        return;
    }

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

    File f = LittleFS.open(CALIBRATION_FILE, "w");
    if (f) {
        f.write((const unsigned char *)calData, 14);
        f.close();
    }
}

// ---------------------------------------------------------------------------
// MQTT
// ---------------------------------------------------------------------------
static void mqttCallback(char *topic, byte *payload, unsigned int length) {
    // PubSubClient hands over a non-terminated pointer into its own buffer.
    // Copy it into a stack buffer rather than building Strings on the heap.
    char value[32];
    size_t n = (length < sizeof(value) - 1) ? length : sizeof(value) - 1;
    memcpy(value, payload, n);
    value[n] = '\0';

    Serial.printf("MQTT %s = %s\n", topic, value);

    if (strcmp(topic, STATE_TOPIC) == 0) {
        bool on = (strcmp(value, ON_STATE) == 0);
        tft.setFreeFont(FF18);
        fairyButton.drawSmoothButton(on, 3, TFT_BLACK, on ? "ON" : "OFF");
    } else if (strcmp(topic, HUMIDITY_TOPIC) == 0) {
        if (parseInt(value, outsideHumid)) outsideHumidValid = true;
        else Serial.printf("Ignoring non-numeric humidity: %s\n", value);
    } else if (strcmp(topic, TEMPERATURE_TOPIC) == 0) {
        if (parseInt(value, outsideTemp)) outsideTempValid = true;
        else Serial.printf("Ignoring non-numeric temperature: %s\n", value);
    }
}

// One connection attempt. Never blocks waiting for a retry; the caller decides
// when to try again so that the display, clock and OTA keep running meanwhile.
static bool mqttConnect() {
    Serial.printf("Connecting to MQTT broker as %s\n", mqttClientId);

    // Retained last will, so Home Assistant can see when the screen drops off.
    bool ok = pubSubClient.connect(mqttClientId, MQTT_USER, MQTT_PWD,
                                   availabilityTopic, 0, true, PAYLOAD_OFFLINE);
    if (!ok) {
        Serial.printf("MQTT connect failed, state %d\n", pubSubClient.state());
        return false;
    }

    Serial.println("MQTT broker connected");
    pubSubClient.publish(availabilityTopic, PAYLOAD_ONLINE, true);
    pubSubClient.subscribe(STATE_TOPIC);
    pubSubClient.subscribe(HUMIDITY_TOPIC);
    pubSubClient.subscribe(TEMPERATURE_TOPIC);
    return true;
}

static void ensureMqtt() {
    static uint32_t lastAttempt = 0;

    if (pubSubClient.connected() || WiFi.status() != WL_CONNECTED) return;
    if (millis() - lastAttempt < MQTT_RETRY_MS) return;

    lastAttempt = millis();
    mqttConnect();
}

static void publishSensors() {
    static uint32_t lastPublish = 0;

    if (!sensorsValid || !pubSubClient.connected()) return;
    if (millis() - lastPublish < SENSOR_PUBLISH_MS) return;
    lastPublish = millis();

    // Two floats need nothing like the 1024 byte heap document this used to
    // allocate every ten seconds.
    StaticJsonDocument<96> doc;
    doc["temperature"] = roundf(tempEvent.temperature * 10.0f) / 10.0f;
    doc["humidity"]    = roundf(humidityEvent.relative_humidity * 10.0f) / 10.0f;

    char buffer[96];
    size_t n = serializeJson(doc, buffer, sizeof(buffer));
    if (!pubSubClient.publish(screenStateTopic, buffer, n)) {
        Serial.println("Sensor publish failed");
    }
}

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------
static void ensureWifi() {
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
        lastAttempt  = millis();
    }

    // Association takes several seconds. Calling WiFi.begin() every 500ms, as
    // this used to, restarts the attempt before it can ever complete.
    if (millis() - lastAttempt >= WIFI_RETRY_MS) {
        lastAttempt = millis();
        Serial.println("Retrying WiFi");
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PWD);
    }
}

// ---------------------------------------------------------------------------
// Buttons and touch
// ---------------------------------------------------------------------------
static void fairyButton_pressAction(void) {
    if (!fairyButton.justPressed()) return;

    // Guard against a flickering touch reading re-triggering justPressed().
    if (millis() - fairyButton.getPressTime() < BUTTON_DEBOUNCE_MS) return;
    fairyButton.setPressTime(millis());

    Serial.println("Fairy light toggle");
    if (!pubSubClient.publish(TOGGLE_TOPIC, "Light toggle.")) {
        Serial.println("Toggle publish failed");
    }
}

static void initButtons() {
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(BUTTON_X - 10, BUTTON_Y - 10);
    tft.print("Fairy Lights");

    char label[] = "OFF";
    fairyButton.initButtonUL(BUTTON_X, BUTTON_Y, BUTTON_W, BUTTON_H,
                             TFT_WHITE, TFT_BLACK, TFT_GREEN, label, 1);
    fairyButton.setPressAction(fairyButton_pressAction);
    // 3 is outline width, TFT_BLACK the surrounding colour for anti-aliasing
    fairyButton.drawSmoothButton(false, 3, TFT_BLACK);
}

static void handleTouch() {
    static uint32_t lastScan = 0;
    if (millis() - lastScan < TOUCH_SCAN_MS) return;
    lastScan = millis();

    uint16_t t_x = 0, t_y = 0;
    bool pressed = tft.getTouch(&t_x, &t_y);
    if (pressed) Serial.printf("Touch at %u, %u\n", t_x, t_y);

    for (uint8_t b = 0; b < buttonCount; b++) {
        // Drive the state on every scan, including when the touch lands outside
        // the button: skipping it leaves the widget's press state stale.
        btn[b]->press(pressed && btn[b]->contains(t_x, t_y));
        btn[b]->pressAction();    // both default to a no-op if unset
        btn[b]->releaseAction();
    }
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------
static void drawDayAndBins() {
    tft.setFreeFont(FF24);
    tft.setTextSize(2);
    tft.fillRect(DIGITAL_X, DIGITAL_Y + 100, 280, 100, TFT_BLACK);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setCursor(DIGITAL_X + 80, DIGITAL_Y + 180);

    char dayName[5];
    if (strftime(dayName, sizeof(dayName), "%a", &timeinfo) > 0) {
        tft.print(dayName);
    }

    const BinType binType = nextBinType(daysSinceBinEpoch(timeinfo));

    tft.setFreeFont(FF19);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(1);
    tft.fillRect(BINS_X - 110, BINS_Y - 20, 260, 100, TFT_BLACK);
    tft.drawString("Bins:", BINS_X - 55, BINS_Y - 16);
    drawCircle(BINS_X + 50, BINS_Y, 20,
               binType == BIN_LANDFILL ? TFT_RED : TFT_YELLOW, true);
}

static void printClock() {
    static uint32_t lastUpdate = 0;
    if (millis() - lastUpdate < CLOCK_UPDATE_MS) return;
    lastUpdate = millis();

    // Short timeout, and skip the draw entirely until NTP has actually synced,
    // rather than briefly rendering a 1970 date.
    if (!getLocalTime(&timeinfo, TIME_LOOKUP_MS)) return;

    tft.setFreeFont(FF17);
    tft.setTextSize(2);

    const int16_t ypos = DIGITAL_Y;
    int16_t xpos = DIGITAL_X;

    if (lastDrawnMinute != timeinfo.tm_min) {  // redraw per minute to limit flicker
        lastDrawnMinute = timeinfo.tm_min;

        tft.setTextColor(TFT_BLACK, TFT_BLACK);
        tft.drawString("88:88", xpos, ypos, 7);  // overwrite the old text to clear it
        tft.setTextColor(TFT_GREEN);

        if (timeinfo.tm_hour < 10) xpos += tft.drawChar('0', xpos, ypos, 7);
        xpos += tft.drawNumber(timeinfo.tm_hour, xpos, ypos, 7);
        xcolon = xpos;
        xpos += tft.drawChar(':', xpos, ypos, 7);
        if (timeinfo.tm_min < 10) xpos += tft.drawChar('0', xpos, ypos, 7);
        tft.drawNumber(timeinfo.tm_min, xpos, ypos, 7);
    }

    // Flash the colon on odd seconds
    tft.setTextColor(timeinfo.tm_sec % 2 ? 0x39C4 : TFT_GREEN, TFT_BLACK);
    tft.drawChar(':', xcolon, ypos, 7);

    if (lastDrawnDay != timeinfo.tm_mday) {
        lastDrawnDay = timeinfo.tm_mday;
        drawDayAndBins();
    }

    tft.setTextSize(1);
}

static void updateSensors() {
    static uint32_t lastRead = 0;
    if (!ahtPresent) return;
    if (millis() - lastRead < SENSOR_READ_MS) return;
    lastRead = millis();

    if (!aht.getEvent(&humidityEvent, &tempEvent)) {
        // Stop publishing rather than repeat a reading the sensor never gave us
        if (sensorsValid) Serial.println("AHT read failed");
        sensorsValid = false;
        return;
    }
    sensorsValid = true;

    drawValue(static_cast<int>(tempEvent.temperature), shownInsideTemp,
              READOUT_TEMP_X, READOUT_INSIDE_Y, 'c');
    drawValue(static_cast<int>(humidityEvent.relative_humidity), shownInsideHumid,
              READOUT_HUMID_X, READOUT_INSIDE_Y, '%');
}

static void updateOutsideDisplay() {
    // drawValue is a no-op when unchanged, so this is cheap to call every pass.
    if (outsideTempValid)
        drawValue(outsideTemp, shownOutsideTemp, READOUT_TEMP_X, READOUT_OUTSIDE_Y, 'c');
    if (outsideHumidValid)
        drawValue(outsideHumid, shownOutsideHumid, READOUT_HUMID_X, READOUT_OUTSIDE_Y, '%');
}

// ---------------------------------------------------------------------------
// OTA
// ---------------------------------------------------------------------------
static void setupOTA() {
    ArduinoOTA
        .onStart([]() {
            String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
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

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    Serial.println("\nStarting");

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);
    WiFi.begin(WIFI_SSID, WIFI_PWD);

    // Bounded wait. The clock and the local sensor work without WiFi, so a
    // router slower to boot than we are must not strand the display forever.
    uint32_t started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < WIFI_BOOT_TIMEOUT_MS) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("WiFi connected, IP address: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("No WiFi at boot, continuing - loop() will keep retrying");
    }

    // Zero padded hex. The old decimal concatenation was ambiguous: MAC bytes
    // 0x01,0x12 and 0x11,0x02 both rendered as "112".
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(mqttClientId, sizeof(mqttClientId), "esp32-%02X%02X%02X%02X",
             mac[5], mac[4], mac[3], mac[2]);
    snprintf(screenStateTopic, sizeof(screenStateTopic),
             "home/screen/%02X%02X%02X%02X/state", mac[5], mac[4], mac[3], mac[2]);
    snprintf(availabilityTopic, sizeof(availabilityTopic),
             "home/screen/%02X%02X%02X%02X/availability", mac[5], mac[4], mac[3], mac[2]);
    Serial.printf("State topic:        %s\n", screenStateTopic);
    Serial.printf("Availability topic: %s\n", availabilityTopic);

    configTzTime(TIMEZONE, "nz.pool.ntp.org");

    tft.begin();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);
    tft.setFreeFont(FF18);

    touch_calibrate();
    initButtons();

    pubSubClient.setServer(MQTT_BROKER, MQTT_PORT);
    pubSubClient.setCallback(mqttCallback);
    mqttConnect();  // one attempt; ensureMqtt() retries if it fails

    // Carry on without the sensor rather than spinning forever: the clock, the
    // bins and the outside readings are all still useful, and OTA still works.
    ahtPresent = aht.begin();
    if (!ahtPresent) Serial.println("Could not find AHT? Check wiring");

    setupOTA();
}

void loop() {
    ArduinoOTA.handle();

    ensureWifi();
    ensureMqtt();
    pubSubClient.loop();

    handleTouch();
    printClock();
    updateSensors();
    updateOutsideDisplay();
    publishSensors();
}
