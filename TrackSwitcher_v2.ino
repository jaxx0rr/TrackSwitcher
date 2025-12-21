#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <Servo.h>
#include <EEPROM.h>

// ---------- WiFi ----------
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// ---------- Servo / turnout tuning ----------
const int SERVO_PIN = D5;

const int CENTER = 90;
const int OFFSET = 15;                  // +/- degrees from center
const int LEFT_POS  = CENTER - OFFSET;
const int RIGHT_POS = CENTER + OFFSET;

const int STEP_DELAY_MS = 12;           // smaller = faster
const int SETTLE_MS = 200;              // short settle at end of move

// ---------- EEPROM (persistent friendly name) ----------
const int EEPROM_SIZE = 128;
const int NAME_ADDR = 0;
const int NAME_MAX_LEN = 32;

String friendlyName;                    // persisted label (what app shows)

// ---------- State ----------
enum SwitchPos : uint8_t { POS_LEFT = 0, POS_RIGHT = 1 };
SwitchPos currentPos = POS_LEFT;

// ---------- Networking ----------
ESP8266WebServer server(80);
Servo servo;

String deviceId;                        // chip id hex
String deviceName;                      // mdns host name, e.g. switch-51ece9

// ---------- Helpers ----------
String readNameFromEeprom() {
    char buf[NAME_MAX_LEN + 1];

    for (int i = 0; i < NAME_MAX_LEN; i++) {
        uint8_t v = EEPROM.read(NAME_ADDR + i);

        // 0xFF is "remembered" blank on many boards
        if (v == 0xFF) {
            buf[i] = '\0';
            break;
        }

        buf[i] = (char)v;

        if (buf[i] == '\0') {
            break;
        }
    }

    buf[NAME_MAX_LEN] = '\0';
    String n(buf);
    n.trim();
    return n;
}

void writeNameToEeprom(const String& name) {
    String n = name;
    n.trim();

    if (n.length() > NAME_MAX_LEN) {
        n = n.substring(0, NAME_MAX_LEN);
    }

    // Write string with null-terminator, clear remaining bytes
    for (int i = 0; i < NAME_MAX_LEN; i++) {
        char c = (i < (int)n.length()) ? n[i] : '\0';
        EEPROM.write(NAME_ADDR + i, (uint8_t)c);

        if (c == '\0') {
            for (int j = i + 1; j < NAME_MAX_LEN; j++) {
                EEPROM.write(NAME_ADDR + j, 0);
            }
            break;
        }
    }

    EEPROM.commit();
}

void moveSlow(int fromDeg, int toDeg) {
    if (fromDeg < toDeg) {
        for (int p = fromDeg; p <= toDeg; p++) {
            servo.write(p);
            delay(STEP_DELAY_MS);
        }
    } else {
        for (int p = fromDeg; p >= toDeg; p--) {
            servo.write(p);
            delay(STEP_DELAY_MS);
        }
    }
}

void setPosition(SwitchPos target) {
    int fromDeg = (currentPos == POS_LEFT) ? LEFT_POS : RIGHT_POS;
    int toDeg   = (target == POS_LEFT) ? LEFT_POS : RIGHT_POS;

    servo.attach(SERVO_PIN);
    moveSlow(fromDeg, toDeg);
    delay(SETTLE_MS);
    servo.detach();                     // IMPORTANT for turnouts: no buzzing/heating

    currentPos = target;
}

// ---------- HTTP Handlers ----------
void handleRoot() {
    server.send(200, "text/plain",
        "TrackSwitch OK\n"
        "GET  /info\n"
        "POST /toggle\n"
        "POST /name?value=YourLabel\n"
    );
}

void handleInfo() {
    String json = "{";
    json += "\"type\":\"track-switch\",";
    json += "\"id\":\"" + deviceId + "\",";
    json += "\"name\":\"" + friendlyName + "\",";
    json += "\"mdns\":\"" + deviceName + ".local\",";
    json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    json += "\"pos\":\"" + String((currentPos == POS_LEFT) ? "left" : "right") + "\"";
    json += "}";
    server.send(200, "application/json", json);
}

void handleToggle() {
    SwitchPos next = (currentPos == POS_LEFT) ? POS_RIGHT : POS_LEFT;
    setPosition(next);

    String json = "{";
    json += "\"ok\":true,";
    json += "\"pos\":\"" + String((currentPos == POS_LEFT) ? "left" : "right") + "\"";
    json += "}";
    server.send(200, "application/json", json);
}

void handleSetName() {
    if (!server.hasArg("value")) {
        server.send(400, "text/plain", "Missing value");
        return;
    }

    String newName = server.arg("value");   // already URL-decoded by ESP8266WebServer
    newName.trim();

    if (newName.length() == 0) {
        server.send(400, "text/plain", "Empty name not allowed");
        return;
    }

    if (newName.length() > NAME_MAX_LEN) {
        newName = newName.substring(0, NAME_MAX_LEN);
    }

    writeNameToEeprom(newName);
    friendlyName = newName;

    String json = "{";
    json += "\"ok\":true,";
    json += "\"name\":\"" + friendlyName + "\"";
    json += "}";
    server.send(200, "application/json", json);
}

// ---------- Setup / Loop ----------
void setup() {
    Serial.begin(115200);
    delay(200);

    deviceId = String(ESP.getChipId(), HEX);
    deviceName = "switch-" + deviceId;

    EEPROM.begin(EEPROM_SIZE);

    friendlyName = readNameFromEeprom();
    if (friendlyName.length() == 0) {
        friendlyName = deviceName;          // default label until you rename
    }

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    while (WiFi.status() != WL_CONNECTED) {
        delay(250);
    }

    // Start in LEFT position
    currentPos = POS_LEFT;
    servo.attach(SERVO_PIN);
    servo.write(LEFT_POS);
    delay(300);
    servo.detach();

    // HTTP routes
    server.on("/", HTTP_GET, handleRoot);
    server.on("/info", HTTP_GET, handleInfo);
    server.on("/toggle", HTTP_POST, handleToggle);
    server.on("/name", HTTP_POST, handleSetName);
    server.begin();

    // mDNS host + service
    if (MDNS.begin(deviceName.c_str())) {
        MDNS.addService("trackswitch", "tcp", 80);
        MDNS.addServiceTxt("trackswitch", "tcp", "type", "track-switch");
        MDNS.addServiceTxt("trackswitch", "tcp", "id", deviceId);
        MDNS.addServiceTxt("trackswitch", "tcp", "mdns", deviceName);
    }

    Serial.println();
    Serial.print("Ready: http://");
    Serial.println(WiFi.localIP());
    Serial.print("mDNS name: ");
    Serial.print(deviceName);
    Serial.println(".local");
    Serial.print("Friendly name: ");
    Serial.println(friendlyName);
}

void loop() {
    server.handleClient();
    MDNS.update();
}
