/*
 * ESP32-CAM CYD Display  v3.0
 * ---------------------------
 * LIVE mode   : polls ESP32-CAM every 10 s, shows full-screen frame
 * GALLERY mode: browse last 24 saved photos (SPIFFS ring buffer)
 * Capture btn : tap bottom-centre in LIVE mode → saves frame to gallery + SD
 * Scheduled   : up to 3 daily HH:MM auto-captures (software RTC, NVS-persisted)
 * SD card     : manual + scheduled captures saved as /photos/capNNNNN.jpg
 *
 * Touch zones (LIVE mode):
 *   Centre (upper)  → toggle LIVE / GALLERY
 *   Bottom-centre   → CAPTURE (save to gallery + SD)
 *   Bottom bar      → cycle filter
 *
 * Touch zones (GALLERY mode):
 *   Left edge       → older photo
 *   Right edge      → newer photo
 *   Centre (upper)  → toggle LIVE / GALLERY
 *   Bottom bar      → cycle filter
 *
 * Boot: hold centre 3 s → settings menu (set clock + 3 schedules)
 *
 * Hardware: ESP32-2432S028R (CYD)
 */

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>
#include <JPEGDEC.h>
#include <SPIFFS.h>
#include <XPT2046_Touchscreen.h>
#include <Preferences.h>

// ── Display pins (HSPI) ───────────────────────────────────────────
#define TFT_DC   2
#define TFT_CS   15
#define TFT_SCK  14
#define TFT_MOSI 13
#define TFT_MISO 12
#define TFT_BL   21

// ── SD card (shares HSPI bus with display, separate CS) ───────────
#define SD_CS    5

// ── Touch pins (XPT2046 on VSPI with custom pins) ─────────────────
#define XPT2046_CS   33
#define XPT2046_IRQ  36
#define XPT2046_CLK  25
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define TOUCH_DEBOUNCE_MS 300

// ── Network ───────────────────────────────────────────────────────
#define AP_SSID  "StickVCam"
#define AP_PASS  ""
#define AP_IP    "192.168.5.1"

// ── Colours ───────────────────────────────────────────────────────
#define C_BLACK   0x0000
#define C_WHITE   0xFFFF
#define C_GREEN   0x07E0
#define C_GRAY    0x7BEF
#define C_DKGRAY  0x2104
#define C_CYAN    0x07FF
#define C_YELLOW  0xFFE0
#define C_RED     0xF800

// ── Layout ────────────────────────────────────────────────────────
#define SCREEN_W    320
#define SCREEN_H    240
#define STATUS_H     20   // bottom status bar height
#define NAV_W        64   // left/right gallery navigation width
#define CAP_Y       193   // capture button zone top y
#define CAP_H        27   // capture button zone height

// ── Gallery (SPIFFS ring buffer) ──────────────────────────────────
#define MAX_PHOTOS   24
#define MAX_JPEG    (50 * 1024)

// ── Filters ───────────────────────────────────────────────────────
enum FilterMode { FILTER_NONE=0, FILTER_GRAY, FILTER_INVERT, FILTER_SEPIA, FILTER_COUNT };
static const char* filterNames[] = { "None", "Gray", "Invert", "Sepia" };

// ── Modes / device types ──────────────────────────────────────────
enum DisplayMode { MODE_LIVE=0, MODE_GALLERY };
enum DeviceType  { DEV_NONE=0, DEV_C3_RELAY, DEV_ESPCAM };

// ── Hardware objects ──────────────────────────────────────────────
static Arduino_DataBus* bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, TFT_MISO);
static Arduino_GFX*     gfx = new Arduino_ILI9341(bus, -1, 1, false);
static JPEGDEC          jpeg;
static SPIClass         touchSPI(VSPI);
static XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);
static SPIClass         sdSPI(HSPI);    // shares HSPI bus with display (different CS)

// ── Shared JPEG buffer ────────────────────────────────────────────
static uint8_t jpegBuf[MAX_JPEG];

// ── Display state ─────────────────────────────────────────────────
static FilterMode    curFilter    = FILTER_NONE;
static DisplayMode   displayMode  = MODE_LIVE;
static unsigned long lastTouchMs  = 0;

// ── Gallery state ─────────────────────────────────────────────────
static int totalSaved = 0;
static int viewIndex  = 0;
static int galleryCount() { return totalSaved < MAX_PHOTOS ? totalSaved : MAX_PHOTOS; }

// ── Network state ─────────────────────────────────────────────────
static char     c3IP[20]      = "";
static bool     c3Found       = false;
static uint32_t lastC3Count   = 0;
static char     camIP[20]     = "";
static bool     camFound      = false;
static int      camFailCount  = 0;

// ── SD state ──────────────────────────────────────────────────────
static bool     sdOk      = false;
static uint32_t sdCounter = 0;   // persisted running filename counter

// ── Software clock ────────────────────────────────────────────────
static uint8_t       clockH = 12, clockM = 0, clockS = 0;
static unsigned long clockLastMs = 0;

// ── Schedules ─────────────────────────────────────────────────────
#define NUM_SCHEDULES 3
static uint8_t schedH[NUM_SCHEDULES]        = {8, 12, 18};
static uint8_t schedM[NUM_SCHEDULES]        = {0,  0,  0};
static bool    schedEn[NUM_SCHEDULES]       = {false, false, false};
static int     schedFiredMin[NUM_SCHEDULES] = {-1, -1, -1};

// ── Preferences (NVS) ─────────────────────────────────────────────
static Preferences prefs;

// ═══════════════════════════════════════════════════════════════════
// ── Clock ──────────────────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════

static void saveClock() {
    prefs.begin("cyd", false);
    prefs.putUChar("h",     clockH);
    prefs.putUChar("m",     clockM);
    prefs.putUChar("s",     clockS);
    prefs.putULong("sdcnt", sdCounter);
    prefs.end();
}

static void loadClock() {
    prefs.begin("cyd", true);
    clockH    = prefs.getUChar("h",      12);
    clockM    = prefs.getUChar("m",       0);
    clockS    = prefs.getUChar("s",       0);
    sdCounter = prefs.getULong("sdcnt",   0);
    for (int i = 0; i < NUM_SCHEDULES; i++) {
        char kh[5], km[5], ke[5];
        snprintf(kh, 5, "sh%d", i);
        snprintf(km, 5, "sm%d", i);
        snprintf(ke, 5, "se%d", i);
        schedH[i]  = prefs.getUChar(kh, schedH[i]);
        schedM[i]  = prefs.getUChar(km, schedM[i]);
        schedEn[i] = prefs.getBool(ke, false);
    }
    prefs.end();
    clockLastMs = millis();
    Serial.printf("[CLK] Restored %02u:%02u:%02u  sdCnt=%lu\n",
                  clockH, clockM, clockS, (unsigned long)sdCounter);
}

static void saveSchedules() {
    prefs.begin("cyd", false);
    for (int i = 0; i < NUM_SCHEDULES; i++) {
        char kh[5], km[5], ke[5];
        snprintf(kh, 5, "sh%d", i);
        snprintf(km, 5, "sm%d", i);
        snprintf(ke, 5, "se%d", i);
        prefs.putUChar(kh, schedH[i]);
        prefs.putUChar(km, schedM[i]);
        prefs.putBool(ke,  schedEn[i]);
    }
    prefs.end();
}

static void clockTick() {
    unsigned long now = millis();
    unsigned long elapsed = (now - clockLastMs) / 1000;
    if (elapsed == 0) return;
    clockLastMs += elapsed * 1000;

    static uint8_t prevM = 255;
    clockS += elapsed;
    while (clockS >= 60) { clockS -= 60; clockM++; }
    while (clockM >= 60) { clockM -= 60; clockH++; }
    if (clockH >= 24) {
        clockH = 0;
        for (int i = 0; i < NUM_SCHEDULES; i++) schedFiredMin[i] = -1;
    }
    if (clockM != prevM) {
        prevM = clockM;
        saveClock();
    }
}

static void fmtTimeFull(char* buf) { snprintf(buf, 9, "%02u:%02u:%02u", clockH, clockM, clockS); }
static void fmtTimeHM(char* buf)   { snprintf(buf, 6, "%02u:%02u",      clockH, clockM); }

// ═══════════════════════════════════════════════════════════════════
// ── SD card ────────────────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════

static void initSD() {
    // HSPI on the CYD SD card slot's physical pins (GPIO 18/19/23/5)
    // — separate from display (VSPI 14/12/13) and touch (VSPI 25/39/32)
    sdSPI.begin(18, 19, 23, SD_CS);
    if (!SD.begin(SD_CS, sdSPI, 4000000)) {
        Serial.println("[SD] Mount failed or no card inserted");
        return;
    }
    if (!SD.exists("/photos")) SD.mkdir("/photos");
    sdOk = true;
    uint64_t free = SD.totalBytes() - SD.usedBytes();
    Serial.printf("[SD] Ready — %llu MB free\n", free / (1024ULL * 1024ULL));
}

static bool saveToSD(uint8_t* data, size_t len) {
    if (!sdOk) return false;
    char path[32];
    snprintf(path, sizeof(path), "/photos/cap%05lu.jpg", (unsigned long)sdCounter);
    File f = SD.open(path, FILE_WRITE);
    if (!f) { Serial.printf("[SD] Open failed: %s\n", path); return false; }
    bool ok = (f.write(data, len) == len);
    f.close();
    if (ok) {
        sdCounter++;
        saveClock();
        Serial.printf("[SD] Saved %s (%zu B)\n", path, len);
    }
    return ok;
}

// ═══════════════════════════════════════════════════════════════════
// ── Filters & JPEG ─────────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════

static void applyFilter(uint16_t* pixels, int count) {
    if (curFilter == FILTER_NONE) return;
    for (int i = 0; i < count; i++) {
        uint16_t le = (pixels[i] >> 8) | (pixels[i] << 8);
        uint8_t r5 = (le >> 11) & 0x1F;
        uint8_t g6 = (le >>  5) & 0x3F;
        uint8_t b5 =  le        & 0x1F;
        int r = (r5 << 3) | (r5 >> 2);
        int g = (g6 << 2) | (g6 >> 4);
        int b = (b5 << 3) | (b5 >> 2);
        switch (curFilter) {
            case FILTER_GRAY: {
                int lum = (r * 77 + g * 150 + b * 29) >> 8;
                r = g = b = lum; break;
            }
            case FILTER_INVERT:
                r = 255-r; g = 255-g; b = 255-b; break;
            case FILTER_SEPIA: {
                int nr = min(255, (r*101 + g*197 + b*48) >> 8);
                int ng = min(255, (r* 89 + g*176 + b*43) >> 8);
                int nb = min(255, (r* 70 + g*137 + b*34) >> 8);
                r=nr; g=ng; b=nb; break;
            }
            default: break;
        }
        uint16_t result = ((r>>3)<<11) | ((g>>2)<<5) | (b>>3);
        pixels[i] = (result>>8) | (result<<8);
    }
}

int jpegDrawCB(JPEGDRAW* pDraw) {
    applyFilter(pDraw->pPixels, pDraw->iWidth * pDraw->iHeight);
    gfx->draw16bitBeRGBBitmap(pDraw->x, pDraw->y, pDraw->pPixels, pDraw->iWidth, pDraw->iHeight);
    return 1;
}

// ═══════════════════════════════════════════════════════════════════
// ── Display helpers ─────────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════

static void drawStatusBar() {
    gfx->fillRect(0, SCREEN_H - STATUS_H, SCREEN_W, STATUS_H, C_DKGRAY);
    gfx->setTextSize(1);

    // Left: clock HH:MM:SS
    char timeBuf[9];
    fmtTimeFull(timeBuf);
    gfx->setTextColor(C_YELLOW);
    gfx->setCursor(4, SCREEN_H - STATUS_H + 6);
    gfx->print(timeBuf);

    // Centre: LIVE label or filter name
    const char* mid    = (displayMode == MODE_LIVE) ? "LIVE" : filterNames[curFilter];
    uint16_t    midCol = (displayMode == MODE_LIVE) ? C_GREEN : C_CYAN;
    gfx->setTextColor(midCol);
    gfx->setCursor((SCREEN_W - (int)strlen(mid) * 6) / 2, SCREEN_H - STATUS_H + 6);
    gfx->print(mid);

    // Right: cam IP (LIVE) or photo index (GALLERY)
    if (displayMode == MODE_LIVE) {
        const char* right = camFound ? camIP : "no cam";
        gfx->setTextColor(camFound ? C_GREEN : C_GRAY);
        gfx->setCursor(SCREEN_W - (int)strlen(right) * 6 - 4, SCREEN_H - STATUS_H + 6);
        gfx->print(right);
    } else {
        char right[12];
        int gc = galleryCount();
        snprintf(right, sizeof(right), gc > 0 ? "%d/%d" : "0/0", viewIndex + 1, gc);
        gfx->setTextColor(C_WHITE);
        gfx->setCursor(SCREEN_W - (int)strlen(right) * 6 - 4, SCREEN_H - STATUS_H + 6);
        gfx->print(right);
    }
}

static void drawCaptureBtnOverlay() {
    // Small shutter button centred above status bar
    int cx = SCREEN_W / 2;
    int cy = CAP_Y + CAP_H / 2;
    gfx->fillCircle(cx, cy, 13, C_DKGRAY);
    gfx->drawCircle(cx, cy, 13, C_WHITE);
    gfx->fillCircle(cx, cy,  9, C_GRAY);
    gfx->fillCircle(cx, cy,  4, C_WHITE);
}

static void drawNavArrows() {
    int gc    = galleryCount();
    if (gc <= 1) return;
    int midY  = (SCREEN_H - STATUS_H) / 2;
    if (viewIndex < gc - 1)
        gfx->fillTriangle(8, midY, 22, midY-14, 22, midY+14, C_GRAY);
    if (viewIndex > 0)
        gfx->fillTriangle(SCREEN_W-8, midY, SCREEN_W-22, midY-14, SCREEN_W-22, midY+14, C_GRAY);
}

static bool displayFromBuf(size_t sz) {
    gfx->fillScreen(C_BLACK);
    if (!jpeg.openRAM(jpegBuf, sz, jpegDrawCB)) return false;
    jpeg.setPixelType(RGB565_BIG_ENDIAN);
    jpeg.decode(0, 0, 0);
    jpeg.close();
    if (displayMode == MODE_LIVE)    drawCaptureBtnOverlay();
    if (displayMode == MODE_GALLERY) drawNavArrows();
    drawStatusBar();
    return true;
}

// ── SPIFFS gallery ────────────────────────────────────────────────
static void photoPath(int absIdx, char* buf, size_t len) {
    snprintf(buf, len, "/p%02d.jpg", absIdx % MAX_PHOTOS);
}

static void loadCount() {
    File f = SPIFFS.open("/count.dat", "r");
    if (!f) { totalSaved = 0; return; }
    if (f.size() >= 4) f.read((uint8_t*)&totalSaved, 4);
    f.close();
    if (totalSaved > 0) {
        char path[16];
        photoPath(totalSaved - 1, path, sizeof(path));
        if (!SPIFFS.exists(path)) totalSaved = 0;
    }
}

static void saveCount() {
    File f = SPIFFS.open("/count.dat", "w");
    if (!f) return;
    f.write((uint8_t*)&totalSaved, 4);
    f.close();
}

static bool savePhoto(uint8_t* data, size_t len) {
    char path[16];
    photoPath(totalSaved, path, sizeof(path));
    File f = SPIFFS.open(path, "w");
    if (!f) return false;
    bool ok = (f.write(data, len) == len);
    f.close();
    if (ok) { totalSaved++; saveCount(); viewIndex = 0; }
    return ok;
}

static bool redisplay() {
    int gc = galleryCount();
    if (gc == 0) return false;
    int absIdx = totalSaved - 1 - viewIndex;
    char path[16];
    photoPath(absIdx, path, sizeof(path));
    File f = SPIFFS.open(path, "r");
    if (!f) return false;
    size_t sz = f.size();
    if (sz == 0 || sz > MAX_JPEG) { f.close(); return false; }
    f.read(jpegBuf, sz);
    f.close();
    return displayFromBuf(sz);
}

static void drawWaiting() {
    gfx->fillScreen(C_BLACK);
    gfx->setTextColor(C_GREEN);
    gfx->setTextSize(2);
    gfx->setCursor(60, 28);
    gfx->print("StickVCam");

    gfx->setTextSize(1);
    char timeBuf[9];
    fmtTimeFull(timeBuf);
    gfx->setTextColor(C_YELLOW);
    gfx->setCursor(116, 60);
    gfx->print(timeBuf);

    gfx->setTextColor(C_GRAY);
    gfx->setCursor(60, 80);
    char apLine[32];
    snprintf(apLine, sizeof(apLine), "AP: %s  Stations: %d", AP_SSID, WiFi.softAPgetStationNum());
    gfx->print(apLine);

    gfx->setCursor(60, 98);
    gfx->setTextColor(camFound ? C_GREEN : C_GRAY);
    char camLine[40];
    snprintf(camLine, sizeof(camLine), camFound ? "CAM:   %s" : "CAM:   searching...", camIP);
    gfx->print(camLine);

    gfx->setCursor(60, 116);
    gfx->setTextColor(c3Found ? C_GREEN : C_GRAY);
    char c3Line[40];
    snprintf(c3Line, sizeof(c3Line), c3Found ? "Relay: %s" : "Relay: ---", c3IP);
    gfx->print(c3Line);

    char galLine[48];
    snprintf(galLine, sizeof(galLine), "Gallery: %d/%d   SD: %s",
             galleryCount(), MAX_PHOTOS, sdOk ? "OK" : "--");
    gfx->setTextColor(C_YELLOW);
    gfx->setCursor(60, 134);
    gfx->print(galLine);

    gfx->setTextColor(C_DKGRAY);
    gfx->setCursor(30, 165);
    gfx->print("Centre tap = LIVE/GALLERY");
    gfx->setCursor(30, 178);
    gfx->print("Boot-hold centre = Settings");
    drawStatusBar();
}

// ═══════════════════════════════════════════════════════════════════
// ── Settings menu ───────────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════

static bool inRect(int tx, int ty, int x, int y, int w, int h) {
    return tx >= x && tx < x+w && ty >= y && ty < y+h;
}

static void drawBtn(int x, int y, int w, int h,
                    const char* label, uint16_t bg, uint16_t fg) {
    gfx->fillRect(x, y, w, h, bg);
    gfx->drawRect(x, y, w, h, C_GRAY);
    int lx = x + (w - (int)strlen(label) * 6) / 2;
    int ly = y + (h - 8) / 2;
    gfx->setTextColor(fg);
    gfx->setTextSize(1);
    gfx->setCursor(lx, ly);
    gfx->print(label);
}

static void drawSettingsPage(int page,
                              uint8_t tH, uint8_t tM,
                              uint8_t sH[], uint8_t sM[], bool sEn[]) {
    gfx->fillScreen(C_BLACK);

    // Title bar
    gfx->fillRect(0, 0, SCREEN_W, 22, C_DKGRAY);
    static const char* titles[] = {"SET TIME", "SCHEDULE 1", "SCHEDULE 2", "SCHEDULE 3"};
    gfx->setTextColor(C_CYAN);
    gfx->setTextSize(1);
    gfx->setCursor((SCREEN_W - (int)strlen(titles[page]) * 6) / 2, 7);
    gfx->print(titles[page]);

    // Page dots
    for (int i = 0; i < 4; i++) {
        if (i == page) gfx->fillCircle(SCREEN_W - 52 + i*13, 11, 4, C_GREEN);
        else           gfx->drawCircle(SCREEN_W - 52 + i*13, 11, 4, C_GRAY);
    }

    if (page == 0) {
        gfx->setTextColor(C_YELLOW);
        gfx->setTextSize(1);
        gfx->setCursor(10, 38);
        gfx->print("Current Time:");

        // Hour
        drawBtn(20,  58, 44, 32, "-", C_DKGRAY, C_WHITE);
        char hBuf[4]; snprintf(hBuf, 4, "%02u", tH);
        gfx->setTextSize(3); gfx->setTextColor(C_WHITE);
        gfx->setCursor(72, 63); gfx->print(hBuf);
        drawBtn(132, 58, 44, 32, "+", C_DKGRAY, C_WHITE);

        gfx->setTextSize(3); gfx->setTextColor(C_WHITE);
        gfx->setCursor(184, 63); gfx->print(":");

        // Minute
        drawBtn(198, 58, 44, 32, "-", C_DKGRAY, C_WHITE);
        char mBuf[4]; snprintf(mBuf, 4, "%02u", tM);
        gfx->setTextSize(3); gfx->setTextColor(C_WHITE);
        gfx->setCursor(250, 63); gfx->print(mBuf);
        drawBtn(295, 58, 22, 32, "+", C_DKGRAY, C_WHITE);

        gfx->setTextSize(1);
        gfx->setTextColor(C_GRAY);
        gfx->setCursor(10, 115);
        gfx->print("Clock restores on reboot from");
        gfx->setCursor(10, 127);
        gfx->print("last saved time (approx).");
    } else {
        int si = page - 1;

        // Enable/Disable toggle
        drawBtn(20, 35, 120, 36,
                sEn[si] ? "ENABLED" : "DISABLED",
                sEn[si] ? 0x0300 : C_DKGRAY, C_WHITE);

        gfx->setTextColor(C_YELLOW);
        gfx->setTextSize(1);
        gfx->setCursor(10, 90);
        gfx->print("Time:");

        // Hour
        drawBtn(20,  105, 44, 32, "-", C_DKGRAY, C_WHITE);
        char hBuf[4]; snprintf(hBuf, 4, "%02u", sH[si]);
        gfx->setTextSize(3);
        gfx->setTextColor(sEn[si] ? C_WHITE : C_GRAY);
        gfx->setCursor(72, 110); gfx->print(hBuf);
        drawBtn(132, 105, 44, 32, "+", C_DKGRAY, C_WHITE);

        gfx->setTextSize(3); gfx->setTextColor(C_WHITE);
        gfx->setCursor(184, 110); gfx->print(":");

        // Minute
        drawBtn(198, 105, 44, 32, "-", C_DKGRAY, C_WHITE);
        char mBuf[4]; snprintf(mBuf, 4, "%02u", sM[si]);
        gfx->setTextSize(3);
        gfx->setTextColor(sEn[si] ? C_WHITE : C_GRAY);
        gfx->setCursor(250, 110); gfx->print(mBuf);
        drawBtn(295, 105, 22, 32, "+", C_DKGRAY, C_WHITE);

        gfx->setTextSize(1);
    }

    // Bottom navigation
    drawBtn(10,  198, 80, 34, "< BACK", C_DKGRAY, C_WHITE);
    if (page < 3) drawBtn(230, 198, 80, 34, "NEXT >",  C_DKGRAY, C_WHITE);
    else          drawBtn(230, 198, 80, 34, "SAVE",    0x0300,   C_WHITE);
}

static void runSettingsMenu() {
    // Work on local copies; commit only on SAVE
    uint8_t tH = clockH, tM = clockM;
    uint8_t sH[NUM_SCHEDULES], sM[NUM_SCHEDULES];
    bool    sEn[NUM_SCHEDULES];
    for (int i = 0; i < NUM_SCHEDULES; i++) {
        sH[i] = schedH[i]; sM[i] = schedM[i]; sEn[i] = schedEn[i];
    }

    int page = 0;
    drawSettingsPage(page, tH, tM, sH, sM, sEn);

    unsigned long lastTouch = 0;
    while (true) {
        if (!ts.tirqTouched() || !ts.touched()) { delay(20); continue; }
        unsigned long now = millis();
        if (now - lastTouch < 200) continue;
        lastTouch = now;

        TS_Point p = ts.getPoint();
        int tx = map(p.x, 200, 3700, 0, SCREEN_W);
        int ty = map(p.y, 200, 3700, 0, SCREEN_H);

        // BACK
        if (inRect(tx, ty, 10, 198, 80, 34)) {
            if (page > 0) { page--; drawSettingsPage(page, tH, tM, sH, sM, sEn); }
            continue;
        }
        // NEXT / SAVE
        if (inRect(tx, ty, 230, 198, 80, 34)) {
            if (page < 3) {
                page++;
                drawSettingsPage(page, tH, tM, sH, sM, sEn);
            } else {
                // Commit
                clockH = tH; clockM = tM; clockS = 0;
                clockLastMs = millis();
                for (int i = 0; i < NUM_SCHEDULES; i++) {
                    schedH[i] = sH[i]; schedM[i] = sM[i]; schedEn[i] = sEn[i];
                    schedFiredMin[i] = -1;
                }
                saveClock();
                saveSchedules();
                Serial.printf("[MENU] Saved %02u:%02u, schedules committed\n", clockH, clockM);
                return;
            }
            continue;
        }

        // Page controls
        if (page == 0) {
            if (inRect(tx, ty,  20,  58, 44, 32)) { tH = (tH == 0) ? 23 : tH-1; drawSettingsPage(page, tH, tM, sH, sM, sEn); }
            if (inRect(tx, ty, 132,  58, 44, 32)) { tH = (tH+1) % 24;            drawSettingsPage(page, tH, tM, sH, sM, sEn); }
            if (inRect(tx, ty, 198,  58, 44, 32)) { tM = (tM == 0) ? 59 : tM-1;  drawSettingsPage(page, tH, tM, sH, sM, sEn); }
            if (inRect(tx, ty, 295,  58, 22, 32)) { tM = (tM+1) % 60;            drawSettingsPage(page, tH, tM, sH, sM, sEn); }
        } else {
            int si = page - 1;
            if (inRect(tx, ty,  20,  35, 120, 36)) { sEn[si] = !sEn[si];                            drawSettingsPage(page, tH, tM, sH, sM, sEn); }
            if (inRect(tx, ty,  20, 105,  44, 32)) { sH[si] = (sH[si]==0) ? 23 : sH[si]-1;          drawSettingsPage(page, tH, tM, sH, sM, sEn); }
            if (inRect(tx, ty, 132, 105,  44, 32)) { sH[si] = (sH[si]+1) % 24;                      drawSettingsPage(page, tH, tM, sH, sM, sEn); }
            if (inRect(tx, ty, 198, 105,  44, 32)) { sM[si] = (sM[si]==0) ? 59 : sM[si]-1;          drawSettingsPage(page, tH, tM, sH, sM, sEn); }
            if (inRect(tx, ty, 295, 105,  22, 32)) { sM[si] = (sM[si]+1) % 60;                      drawSettingsPage(page, tH, tM, sH, sM, sEn); }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
// ── Network ─────────────────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════

static DeviceType probeDevice(const char* ip) {
    char url[48];
    snprintf(url, sizeof(url), "http://%s/status", ip);
    HTTPClient http;
    http.begin(url);
    http.setTimeout(300);
    int code = http.GET();
    if (code != 200) { http.end(); return DEV_NONE; }
    String body = http.getString();
    http.end();
    StaticJsonDocument<128> doc;
    if (deserializeJson(doc, body)) return DEV_NONE;
    const char* type = doc["type"] | "";
    if (strcmp(type, "cam") == 0) return DEV_ESPCAM;
    return DEV_C3_RELAY;
}

static void findDevices() {
    for (int i = 2; i <= 20 && !(c3Found && camFound); i++) {
        char ip[20];
        snprintf(ip, sizeof(ip), "192.168.5.%d", i);
        DeviceType dt = probeDevice(ip);
        if (dt == DEV_C3_RELAY && !c3Found) {
            strlcpy(c3IP, ip, sizeof(c3IP));
            c3Found = true;
            Serial.printf("[CYD] C3 relay at %s\n", c3IP);
        } else if (dt == DEV_ESPCAM && !camFound) {
            strlcpy(camIP, ip, sizeof(camIP));
            camFound = true;
            camFailCount = 0;
            Serial.printf("[CYD] ESP32-CAM at %s\n", camIP);
        }
    }
}

static uint32_t getC3Count() {
    char url[48];
    snprintf(url, sizeof(url), "http://%s/status", c3IP);
    HTTPClient http;
    http.begin(url);
    http.setTimeout(1000);
    int code = http.GET();
    if (code != 200) { http.end(); return lastC3Count; }
    String body = http.getString();
    http.end();
    StaticJsonDocument<64> doc;
    if (deserializeJson(doc, body)) return lastC3Count;
    return doc["count"] | lastC3Count;
}

// Fetch from C3 relay (StickV snapshots), optionally display
static bool fetchFromC3(bool showNow = true) {
    char url[48];
    snprintf(url, sizeof(url), "http://%s/latest.jpg", c3IP);
    HTTPClient http;
    http.begin(url);
    http.setTimeout(6000);
    int code = http.GET();
    if (code != 200) { http.end(); return false; }
    int len = http.getSize();
    if (len <= 0 || len > MAX_JPEG) { http.end(); return false; }
    WiFiClient* stream = http.getStreamPtr();
    size_t got = 0;
    unsigned long t = millis();
    while (http.connected() && got < (size_t)len && millis() - t < 6000) {
        size_t avail = stream->available();
        if (avail) got += stream->readBytes(jpegBuf + got, min(avail, (size_t)(len - got)));
        delay(1);
    }
    http.end();
    if (got < 4) return false;
    savePhoto(jpegBuf, got);
    if (showNow) displayFromBuf(got);
    return true;
}

// Fetch from ESP32-CAM; optionally save to gallery and/or SD
static bool fetchLive(bool saveGallery = false, bool saveSd = false) {
    char url[48];
    snprintf(url, sizeof(url), "http://%s/latest.jpg", camIP);
    HTTPClient http;
    http.begin(url);
    http.setTimeout(5000);
    int code = http.GET();
    if (code != 200) { http.end(); return false; }
    int len = http.getSize();
    if (len <= 0 || len > MAX_JPEG) { http.end(); return false; }
    WiFiClient* stream = http.getStreamPtr();
    size_t got = 0;
    unsigned long t = millis();
    while (http.connected() && got < (size_t)len && millis() - t < 5000) {
        size_t avail = stream->available();
        if (avail) got += stream->readBytes(jpegBuf + got, min(avail, (size_t)(len - got)));
        delay(1);
    }
    http.end();
    if (got < 4) return false;
    if (saveGallery) {
        savePhoto(jpegBuf, got);
        Serial.printf("[CYD] Saved to gallery (%zu B, %d/%d)\n", got, galleryCount(), MAX_PHOTOS);
    }
    if (saveSd) saveToSD(jpegBuf, got);
    return displayFromBuf(got);
}

// Manual capture triggered by the shutter button
static void captureNow() {
    Serial.println("[CYD] Manual capture");
    // Brief white flash as shutter feedback
    gfx->fillRect(SCREEN_W/2 - 40, CAP_Y, 80, CAP_H, C_WHITE);
    delay(80);
    fetchLive(true, true);
}

// ═══════════════════════════════════════════════════════════════════
// ── Scheduled captures ──────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════

static void checkSchedules() {
    if (!camFound) return;
    int curMin = clockH * 60 + clockM;
    for (int i = 0; i < NUM_SCHEDULES; i++) {
        if (!schedEn[i]) continue;
        int sMin = schedH[i] * 60 + schedM[i];
        if (curMin == sMin && schedFiredMin[i] != sMin) {
            schedFiredMin[i] = sMin;
            Serial.printf("[SCHED] Slot %d firing at %02u:%02u\n", i+1, clockH, clockM);
            fetchLive(true, true);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
// ── Touch handling ──────────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════

static void handleTouch() {
    if (!ts.tirqTouched() || !ts.touched()) return;
    unsigned long now = millis();
    if (now - lastTouchMs < TOUCH_DEBOUNCE_MS) return;
    lastTouchMs = now;

    TS_Point p = ts.getPoint();
    int tx = map(p.x, 200, 3700, 0, SCREEN_W);
    int ty = map(p.y, 200, 3700, 0, SCREEN_H);
    int gc = galleryCount();

    // Bottom status bar → cycle filter
    if (ty >= SCREEN_H - STATUS_H) {
        curFilter = (FilterMode)((curFilter + 1) % FILTER_COUNT);
        if (displayMode == MODE_GALLERY && gc > 0) redisplay();
        else drawStatusBar();
        return;
    }

    // Capture button (LIVE mode, centre-bottom zone above status bar)
    if (displayMode == MODE_LIVE && camFound &&
        ty >= CAP_Y && ty < SCREEN_H - STATUS_H &&
        tx >= SCREEN_W/2 - 40 && tx <= SCREEN_W/2 + 40) {
        captureNow();
        return;
    }

    // Centre zone (upper screen) → toggle LIVE / GALLERY
    if (tx >= NAV_W && tx <= SCREEN_W - NAV_W) {
        displayMode = (displayMode == MODE_LIVE) ? MODE_GALLERY : MODE_LIVE;
        Serial.printf("[CYD] Mode: %s\n", displayMode == MODE_LIVE ? "LIVE" : "GALLERY");
        if (displayMode == MODE_GALLERY) {
            if (gc > 0) redisplay();
            else        drawWaiting();
        } else {
            if (!camFound || !fetchLive()) drawWaiting();
        }
        return;
    }

    // Left / right nav — GALLERY mode only
    if (displayMode != MODE_GALLERY || gc == 0) return;
    if (tx < NAV_W && viewIndex < gc - 1) {
        viewIndex++;
        redisplay();
    } else if (tx > SCREEN_W - NAV_W && viewIndex > 0) {
        viewIndex--;
        redisplay();
    }
}

// ═══════════════════════════════════════════════════════════════════
// ── Setup ────────────────────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);

    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);
    gfx->begin();
    gfx->fillScreen(C_BLACK);

    touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
    ts.begin(touchSPI);
    ts.setRotation(1);

    loadClock();   // restore clock + schedules + SD counter from NVS

    if (!SPIFFS.begin(true)) {
        Serial.println("[CYD] SPIFFS mount failed");
    } else {
        loadCount();
        Serial.printf("[CYD] SPIFFS ok — %d photos in gallery\n", galleryCount());
    }

    initSD();

    IPAddress apIP(192, 168, 5, 1);
    IPAddress netmask(255, 255, 255, 0);
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(apIP, apIP, netmask);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.printf("[CYD] AP up: %s\n", AP_SSID);

    // ── Boot-hold: 3 s countdown — centre touch enters settings ──────
    gfx->setTextColor(C_WHITE);
    gfx->setTextSize(1);
    gfx->setCursor(40, 88);
    gfx->print("Hold centre for Settings...");
    bool enterSettings = false;
    unsigned long holdStart = millis();
    while (millis() - holdStart < 3000) {
        int barW = (int)((3000 - (millis() - holdStart)) * 240 / 3000);
        gfx->fillRect(40, 112, 240, 10, C_DKGRAY);
        gfx->fillRect(40, 112, barW,  10, C_GREEN);
        if (ts.tirqTouched() && ts.touched()) {
            TS_Point p = ts.getPoint();
            int tx = map(p.x, 200, 3700, 0, SCREEN_W);
            if (tx >= NAV_W && tx <= SCREEN_W - NAV_W) {
                enterSettings = true;
                break;
            }
        }
        delay(40);
    }
    if (enterSettings) runSettingsMenu();

    drawWaiting();
}

// ═══════════════════════════════════════════════════════════════════
// ── Loop ─────────────────────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════

void loop() {
    static unsigned long lastProbe     = 0;
    static unsigned long lastPoll      = 0;
    static unsigned long lastLivePoll  = 0;
    static unsigned long stationZeroAt = 0;
    unsigned long now = millis();

    clockTick();
    checkSchedules();
    handleTouch();

    // Station dropout debounce — allow 8 s before marking devices lost
    int stations = WiFi.softAPgetStationNum();
    if (stations == 0) {
        if (stationZeroAt == 0) stationZeroAt = now;
        if (now - stationZeroAt > 8000 && (c3Found || camFound)) {
            Serial.println("[CYD] All stations gone — resetting");
            c3Found = false; camFound = false;
            strlcpy(c3IP,  "", sizeof(c3IP));
            strlcpy(camIP, "", sizeof(camIP));
            camFailCount = 0;
            drawWaiting();
        }
    } else {
        stationZeroAt = 0;
    }

    // Device discovery — only while cam not yet found
    if (!camFound && now - lastProbe >= 3000) {
        lastProbe = now;
        findDevices();
        if (!camFound) {
            drawWaiting();
        } else if (displayMode == MODE_LIVE) {
            lastLivePoll = now - (10000 - 2000);  // first live frame in ~2 s
        }
    }

    // Live feed — poll every 10 s
    if (displayMode == MODE_LIVE && camFound && now - lastLivePoll >= 10000) {
        lastLivePoll = now;
        if (!fetchLive()) {
            if (++camFailCount >= 10) {
                Serial.println("[CYD] ESP32-CAM lost");
                camFound = false;
                strlcpy(camIP, "", sizeof(camIP));
                camFailCount = 0;
                drawWaiting();
            }
        } else {
            camFailCount = 0;
        }
    }

    // C3 relay — poll for new StickV snapshots every 500 ms
    if (c3Found && now - lastPoll >= 500) {
        lastPoll = now;
        uint32_t count = getC3Count();
        if (count != lastC3Count) {
            lastC3Count = count;
            fetchFromC3(displayMode == MODE_GALLERY);
        }
    }
}
