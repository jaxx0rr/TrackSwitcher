#include <WiFi.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>

#include <TFT_eSPI.h>
#include <TFT_Touch.h>

// ---------------- WiFi ----------------
const char* WIFI_SSID = "JX1 2.4GHz";
const char* WIFI_PASS = "Spartacus1";

// ---------------- Screen ----------------
static const int SCREEN_W = 240;
static const int SCREEN_H = 320;

// ---------------- Touch pins (from your working demo) ----------------
#define DOUT 39
#define DIN  32
#define DCS  33
#define DCLK 25

TFT_Touch touch = TFT_Touch(DCS, DCLK, DIN, DOUT);
TFT_eSPI tft = TFT_eSPI();

// ---------------- UI layout ----------------
static const int TOP_H = 28;
static const int MARGIN = 8;

static const int BTN_GAP = 4;
static const int BTN_MAX = 8;

static const int REF_W = 70;
static const int REF_H = 20;

// ---------------- Device cache ----------------
struct Device {
    String ip;
    uint16_t port;
    String name;  // friendly name from /info
    String pos;   // "left" / "right" / "unknown"
};

static Device devices[BTN_MAX];
static int deviceCount = 0;

// ---------------- Helpers ----------------
static void setStatus(const String& s);
static void drawUI();
static void drawTopBar();
static void drawButtons();
static void scanSwitches();
static bool toggleSwitch(int idx);

static String routeTextFromPos2(const String& pos) {
    // Your mapping: servo left => train goes right (so display inverted)
    if (pos == "left")  return "route: RIGHT";
    if (pos == "right") return "route: LEFT";
    return "route: unknown";
}

static String routeTextFromPos(const String& pos) {
    if (pos == "left")  return "route: ↑";   // straight
    if (pos == "right") return "route: ↱";   // diverge
    return "?";
}

static void drawStraightArrow(int x, int y, int h, uint16_t color) {
    // Up arrow: line + triangle head
    int mid = x;
    int top = y;
    int bottom = y + h;

    tft.drawLine(mid, bottom, mid, top + 6, color);
    tft.fillTriangle(mid, top, mid - 6, top + 10, mid + 6, top + 10, color);
}

static void drawDivergeArrow(int x, int y, int h, uint16_t color) {
    // L-shaped: up then right
    // x,y = top-left of arrow area, h = total height of arrow area

    int top = y;
    int bottom = y + h;

    int bendY = y + (h / 2);     // bend at vertical center
    int stemX = x;              // vertical line x
    int headX = x + 18;         // triangle tip x (further right looks nicer)

    // vertical stem (from bottom to bend)
    tft.drawLine(stemX, bottom, stemX, bendY, color);

    // horizontal run (from bend to just before arrow head)
    tft.drawLine(stemX, bendY, headX - 8, bendY, color);

    // right-pointing arrow head (triangle)
    tft.fillTriangle(
        headX,      bendY,       // tip (right)
        headX - 10, bendY - 6,    // base top-left
        headX - 10, bendY + 6,    // base bottom-left
        color
    );
}



static String extractJsonString(const String& body, const char* key) {
    String k = String("\"") + key + "\":\"";
    int s = body.indexOf(k);
    if (s < 0) return "";
    s += k.length();
    int e = body.indexOf("\"", s);
    if (e < 0) return "";
    return body.substring(s, e);
}

static bool httpGetInfo(const String& ip, uint16_t port, String& outName, String& outPos) {
    HTTPClient http;
    http.setTimeout(2500);

    String url = "http://" + ip + ":" + String(port) + "/info";
    if (!http.begin(url)) return false;

    int code = http.GET();
    if (code != 200) {
        http.end();
        return false;
    }

    String body = http.getString();
    http.end();

    if (body.indexOf("\"type\":\"track-switch\"") < 0) return false;

    outName = extractJsonString(body, "name");
    outPos  = extractJsonString(body, "pos");
    if (outName.length() == 0) outName = "switch";
    if (outPos.length() == 0)  outPos = "unknown";

    return true;
}

static bool httpPostToggle(const String& ip, uint16_t port, String& newPosOut) {
    HTTPClient http;
    http.setTimeout(3000);

    String url = "http://" + ip + ":" + String(port) + "/toggle";
    if (!http.begin(url)) return false;

    http.addHeader("Content-Type", "application/json");
    int code = http.POST("{}");
    if (code != 200) {
        http.end();
        return false;
    }

    String body = http.getString();
    http.end();

    if (body.indexOf("\"ok\":true") < 0) return false;

    String p = extractJsonString(body, "pos");
    if (p.length()) newPosOut = p;
    return true;
}

// ---------------- Drawing ----------------
static String g_status = "boot";

static void setStatus(const String& s) {
    g_status = s;
    drawTopBar();
}

static void drawTopBar() {
    tft.fillRect(0, 0, SCREEN_W, TOP_H, TFT_BLACK);

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(MARGIN, 8);
    tft.print("Status: ");
    tft.print(g_status);

    int rx = SCREEN_W - MARGIN - REF_W;
    int ry = (TOP_H - REF_H) / 2;

    // Clamp just in case
    if (rx < MARGIN) rx = MARGIN;

    tft.drawRoundRect(rx, ry, REF_W, REF_H, 4, TFT_WHITE);
    tft.setCursor(rx + 8, ry + 6);
    tft.print("REFRESH");
}


static void drawButtons() {
    // Clear area below top bar
    tft.fillRect(0, TOP_H, SCREEN_W, SCREEN_H - TOP_H, TFT_BLACK);

    int usableH = SCREEN_H - TOP_H - MARGIN;
    int btnH = (usableH - (BTN_MAX * BTN_GAP)) / BTN_MAX;
    if (btnH < 22) btnH = 22;

    int y = TOP_H + MARGIN;

    for (int i = 0; i < BTN_MAX; i++) {
        if (i >= deviceCount) break;

        int x = MARGIN;
        int w = SCREEN_W - (MARGIN * 2) - 6;  // extra shrink
        int h = btnH;

        // Button box
        tft.drawRoundRect(x, y, w, h, 6, TFT_WHITE);

        // ---------- Line 1: Name ----------
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setTextSize(1);
        tft.setCursor(x + 8, y + 4);
        tft.print(devices[i].name);

        // ---------- Arrow graphic (right side) ----------
        int ax = x + w - 22;   // more right
        int ay = y + 2;        // more up
        int ah = h - 6;

        if (devices[i].pos == "left") {
            drawDivergeArrow(ax, ay, ah, TFT_WHITE);   // swapped
        } else if (devices[i].pos == "right") {
            drawStraightArrow(ax, ay, ah, TFT_WHITE);  // swapped
        } else {
            tft.setTextSize(2);
            tft.setCursor(ax, ay);
            tft.print("?");
        }

        // ---------- Line 2: IP ----------
        tft.setTextSize(1);
        tft.setCursor(x + 8, y + 16);
        tft.print(devices[i].ip);
        tft.print(":");
        tft.print(devices[i].port);

        y += h + BTN_GAP;
    }

    if (deviceCount == 0) {
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setTextSize(2);
        tft.setCursor(20, TOP_H + 40);
        tft.print("No switches found");
    }
}


static void drawUI() {
    drawTopBar();
    drawButtons();
}

// ---------------- Scan + Toggle ----------------
static void scanSwitches() {
    setStatus("scanning...");

    deviceCount = 0;

    // Find services: _trackswitch._tcp (service name "trackswitch")
    int n = MDNS.queryService("trackswitch", "tcp");
    if (n <= 0) {
        setStatus("none found");
        drawButtons();
        return;
    }

    int limit = n;
    if (limit > BTN_MAX) limit = BTN_MAX;

    for (int i = 0; i < limit; i++) {
        Device d;
        d.port = MDNS.port(i);

        // Some cores don't have MDNS.IP(i), so resolve hostname -> IP
        String host = MDNS.hostname(i);
        IPAddress ip = MDNS.queryHost(host);
        if (ip.toString() == "0.0.0.0") {
            continue;
        }
        d.ip = ip.toString();

        d.name = host;
        d.pos = "unknown";

        String friendly, pos;
        if (httpGetInfo(d.ip, d.port, friendly, pos)) {
            d.name = friendly;
            d.pos = pos;
        }

        devices[deviceCount++] = d;
        if (deviceCount >= BTN_MAX) break;
    }

    setStatus("idle");
    drawButtons();
}

static bool toggleSwitch(int idx) {
    if (idx < 0 || idx >= deviceCount) return false;

    setStatus("toggling...");
    String newPos;
    bool ok = httpPostToggle(devices[idx].ip, devices[idx].port, newPos);

    if (ok) {
        if (newPos.length()) {
            devices[idx].pos = newPos;
        }
        setStatus("ok");
        drawButtons();
        return true;
    }

    setStatus("toggle failed");
    return false;
}

// ---------------- Touch handling ----------------
static bool isInRect(int x, int y, int rx, int ry, int rw, int rh) {
    return (x >= rx && x < (rx + rw) && y >= ry && y < (ry + rh));
}

static void handleTouch(int tx, int ty) {
    // Refresh button hit test
    int rx = SCREEN_W - MARGIN - REF_W;
    int ry = (TOP_H - REF_H) / 2;

    if (isInRect(tx, ty, rx, ry, REF_W, REF_H)) {
        scanSwitches();
        return;
    }

    // Device buttons
    int usableH = SCREEN_H - TOP_H - MARGIN;
    int btnH = (usableH - (BTN_MAX * BTN_GAP)) / BTN_MAX;
    if (btnH < 22) btnH = 22;

    int x = MARGIN;
    int w = SCREEN_W - (MARGIN * 2);
    int y = TOP_H + MARGIN;

    for (int i = 0; i < deviceCount; i++) {
        if (isInRect(tx, ty, x, y, w, btnH)) {
            toggleSwitch(i);
            return;
        }
        y += btnH + BTN_GAP;
    }
}

static bool touchWasDown = false;
static uint32_t lastTouchMs = 0;

static void pollTouch() {
    bool down = touch.Pressed();

    if (down) {
        uint32_t now = millis();
        if (now - lastTouchMs > 40) {   // reduced debounce
            lastTouchMs = now;

            uint16_t tx = 0, ty = 0;
            for (int i = 0; i < 3; i++) {
                tx += touch.X();
                ty += touch.Y();
            }
            tx /= 3;
            ty /= 3;

            handleTouch((SCREEN_W - 1) - (int)ty, (int)tx);
        }
    }

    touchWasDown = down;
}

// ---------------- Setup / Loop ----------------
void setup() {
    Serial.begin(115200);

    tft.begin();
    tft.invertDisplay(false);   // fixes your “inverted colors” issue
    tft.setRotation(0);         // landscape

    tft.fillScreen(TFT_WHITE);

    // Touch calibration (your working values)
    touch.setCal(526, 3443, 750, 3377, 320, 240, 1);

    tft.fillScreen(TFT_BLACK);
    setStatus("wifi...");

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    while (WiFi.status() != WL_CONNECTED) {
        pollTouch();   // keep UI responsive during connect
        delay(20);
    }

    setStatus("mdns...");
    MDNS.begin("jx-panel");

    setStatus("ready");
    scanSwitches();
}

void loop() {
    pollTouch();
    delay(5);
}
