/*
 * M5Stack Core2 番茄钟 v6
 * -------------------------------------------------------
 * 按键：
 *   BtnA 单击    — 开始 / 暂停 / 恢复计时
 *   BtnA 长按    — 语音命令（说"开始X分钟" / "停止"）
 *   BtnB 单击    — 统计图（任意按键/点击返回）
 *   BtnC 单击    — 进入设置（再按保存退出）
 *   Touch 单击   — 子界面返回主界面
 *
 * 显示 Style（设置里切换）：
 *   Style 1 — StackChan 表情 + 倒计时
 *   Style 2 — 仅倒计时大字
 *   Style 3 — 仅 StackChan 表情（全屏）
 *
 * 语音（百度 AI）：
 *   - WiFi 连接后自动获取 Access Token
 *   - ASR：百度语音识别（16kHz PCM，普通话/英文）
 *   - TTS：百度语音合成（PCM 直推 Speaker）
 *   - 离线 fallback：节奏蜂鸣
 */

#include <M5Unified.h>
#include <Preferences.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <cmath>
#include <cstring>


// ============================================================
// 枚举
// ============================================================
enum AppMode    { MODE_CLOCK, MODE_STATS, MODE_SETTINGS };
enum PomoState  { IDLE, WORK, SHORT_BRK, LONG_BRK };
enum SettingsSel{ SEL_DUR, SEL_LOOK, SEL_RING, SEL_COUNT };

// ============================================================
// 常量
// ============================================================
static const uint32_t DUR_OPTS[]   = { 0, 5, 10, 15, 20, 25, 30, 45, 60 }; // 添加 0 作为 5秒的代号
static const uint8_t  DUR_CNT      = 9; // 长度从 8 改为 9
static const uint32_t SHORT_BRK_MS     = 5  * 60 * 1000UL;
static const uint32_t LONG_BRK_MS      = 15 * 60 * 1000UL;
static const int      LONG_INTERVAL    = 4;
static const char*    DAY_LBL[]        = { "Mo","Tu","We","Th","Fr","Sa","Su" };
static const uint32_t SLEEP_TIMEOUT_MS = 30000UL;

// 麦克风 / 录音
static const int      MIC_RATE         = 16000;
static const int      MIC_BUF_N        = 512;
static const int32_t  VOICE_THR        = 1500;
static const uint32_t VOICE_MAX_MS     = 5000;
static const uint32_t SILENCE_MS       = 800;
static const int      AUDIO_MAX_SAMPLES = MIC_RATE * 5;  // 5 秒最大录音

// ============================================================
// 颜色助手
// ============================================================
#define C_BG    ((uint16_t)0x0000)
#define C_WHITE ((uint16_t)0xFFFF)

static inline uint16_t cDim()     { return M5.Lcd.color565( 70,  70,  70); }
static inline uint16_t cAccent()  { return M5.Lcd.color565( 70, 190, 110); }
static inline uint16_t cYellow()  { return M5.Lcd.color565(255, 210,  50); }
static inline uint16_t cRed()     { return M5.Lcd.color565(220,  55,  55); }
static inline uint16_t cSelbg()   { return M5.Lcd.color565( 40,  40, 110); }
static inline uint16_t cVoicebg() { return M5.Lcd.color565(  8,  18,  35); }
static inline uint16_t cFace()    { return M5.Lcd.color565(200, 220, 255); }  // StackChan 脸色


// ============================================================
// WiFi & 百度 AI 凭据（请在此修改）
// ============================================================
static const char* WIFI_SSID     = "REDMI K80";
static const char* WIFI_PASS     = "wzy20050824";
static const char* BAIDU_API_KEY = "DVZRlhQYBKxxWeVaiGh3SStL";
static const char* BAIDU_SEC_KEY = "S4Xza9YUc5V3SKC9QXy86W5uYCKZZ4w1";

// ============================================================
// 全局状态
// ============================================================
static AppMode     gMode       = MODE_CLOCK;
static PomoState   gPomo       = IDLE;
static SettingsSel gSetSel     = SEL_DUR;
static Preferences gPrefs;

static uint8_t  gDurIdx    = 4;    // 默认 25 分钟（index 4）
static uint32_t gWorkMs    = 0;
static uint32_t gStartMs   = 0;
static uint32_t gPauseAcc  = 0;
static bool     gPaused    = false;
static int      gCycle     = 0;

static int      gWeekly[7] = { 0 };
static int      gToday     = 3;    // 默认周四

static uint8_t  gRingStyle = 0;
static uint8_t  gLookStyle = 0;    // 0=Style1  1=Style2  2=Style3

static bool     gNeedRedraw = true;
static bool     gAudioBusy = false;
// 横幅
static char     gBannerText[80] = "";
static uint32_t gBannerEndMs    = 0;
static bool     gBannerOn       = false;

// StackChan 眨眼动画
static uint32_t gEmojiTick  = 0;
static uint8_t  gEmojiFrame = 0;  // 0=正常  1=眨眼

// WiFi / 百度
static String   gAccessToken = "";

// 屏保
static uint32_t gLastActiveMs = 0;
static bool     gScreenSleep  = false;

// 录音缓冲（PSRAM）
static int16_t* gAudioBuf = nullptr;
static int      gAudioLen = 0;
static uint32_t gNextRedrawMs = 0;

// ============================================================
// 前向声明
// ============================================================
void drawClock();
void drawStats();
void drawSettings();
void tickTimer();
void speakText(const char* text, int fallbackSyllables = 4);
void handleVoiceCommand();

// ============================================================
// NVS —— 持久化
// ============================================================
void nvLoad() {
    gPrefs.begin("pomo", false);
    gDurIdx    = gPrefs.getUChar("dur",   4);
    gRingStyle = gPrefs.getUChar("ring",  0);
    gLookStyle = gPrefs.getUChar("look",  0);
    gToday     = gPrefs.getInt("today",   3);
    char k[8];
    for (int i = 0; i < 7; i++) {
        snprintf(k, sizeof(k), "w%d", i);
        gWeekly[i] = gPrefs.getInt(k, 0);
    }
}

void nvSaveFocus() {
    gWeekly[gToday]++;
    char k[8];
    snprintf(k, sizeof(k), "w%d", gToday);
    gPrefs.putInt(k, gWeekly[gToday]);
}

void nvSaveSettings() {
    gPrefs.putUChar("dur",  gDurIdx);
    gPrefs.putUChar("ring", gRingStyle);
    gPrefs.putUChar("look", gLookStyle);
}

// ============================================================
// 声音
// ============================================================
static void ringDone() {

    if (gAudioBusy) return;
    gAudioBusy = true;

    if (M5.Speaker.isPlaying()) {
        M5.Speaker.stop();
    }

    int melody[] = {
        1047, 1319, 1568, 2093,
        1568, 1760, 2093
    };

    int dur[] = {
        120, 120, 120, 220,
        120, 120, 300
    };

    for (int i = 0; i < 7; i++) {
        M5.Speaker.tone(melody[i], dur[i]);

        uint32_t start = millis();
        while (millis() - start < dur[i]) {
            M5.update();
            delay(1);
        }
    }

    M5.Speaker.stop();

    gAudioBusy = false;
}

void ringClick() {
    M5.Speaker.tone(900, 50);
}

// 离线 TTS fallback：节奏蜂鸣（音节数模拟语气）
void ttsBeep(int syllables) {
    for (int i = 0; i < syllables; i++) {
        M5.Speaker.tone(1100 - i * 35, 110); delay(130);
        M5.Speaker.tone(850  + i * 20,  70); delay(90);
    }
    delay(150);
}

// ============================================================
// 横幅（屏幕顶部短暂提示）
// ============================================================
void showBanner(const char* text, uint32_t durationMs = 2500) {
    strncpy(gBannerText, text, sizeof(gBannerText) - 1);
    gBannerText[sizeof(gBannerText) - 1] = '\0';
    gBannerEndMs = millis() + durationMs;
    gBannerOn    = true;

    M5.Lcd.fillRoundRect(4, 4, 312, 30, 6, M5.Lcd.color565(20, 60, 40));
    M5.Lcd.drawRoundRect(4, 4, 312, 30, 6, cAccent());
    M5.Lcd.setTextColor(C_WHITE);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setCursor(12, 14);
    M5.Lcd.print(text);
}

void drawStackChan(int cx, int cy, int r, uint8_t frame, bool isWork) {
    // 1. 完全不画脸部底色和轮廓，直接画表情
    
    // 2. 基础参数计算
    int eyeSpacing = r * 0.4;    // 眼睛稍微分得开一点，更有呆萌感
    int eyeY       = cy - r * 0.1; 
    int eyeSize    = max(3, r / 12); // 白色点点的大小
    uint16_t eyeCol = C_WHITE;       // 眼睛改为白色

    // 3. 绘制表情
    if (frame == 1 || gPaused) {
        // 【眨眼/暂停】：眼睛变成很短的水平白线
        M5.Lcd.drawLine(cx - eyeSpacing - 5, eyeY, cx - eyeSpacing + 5, eyeY, eyeCol);
        M5.Lcd.drawLine(cx + eyeSpacing - 6, eyeY, cx + eyeSpacing + 6, eyeY, eyeCol);
    } 
    else {
        // 【正常/专注】：白色点点眼
        M5.Lcd.fillCircle(cx - eyeSpacing, eyeY, eyeSize, eyeCol);
        M5.Lcd.fillCircle(cx + eyeSpacing, eyeY, eyeSize, eyeCol);
    }

    // 4. 嘴巴：始终是一条极简横线
    // 宽度根据状态微调，isWork 时嘴巴短一点显得严肃，平时稍微长一点
    int mouthW = isWork ? 6 : 10;
    int mouthY = cy + r * 0.25;
    
    // 画一条粗一点的横线（画两遍增加厚度）
    M5.Lcd.drawLine(cx - mouthW, mouthY, cx + mouthW, mouthY, eyeCol);
    M5.Lcd.drawLine(cx - mouthW, mouthY + 1, cx + mouthW, mouthY + 1, eyeCol);
}

// ============================================================
// 进度弧（双线加粗）
// ============================================================
void drawArc(int cx, int cy, int r, float ratio, uint16_t col) {
    M5.Lcd.drawCircle(cx, cy, r,     cDim());
    M5.Lcd.drawCircle(cx, cy, r - 1, cDim());
    const int segs = 90;
    for (int s = 0; s < segs; s++) {
        if ((float)s / segs > ratio) break;
        float a1 = -M_PI / 2.0f + (float)s       / segs * 2.0f * M_PI;
        float a2 = -M_PI / 2.0f + (float)(s + 1) / segs * 2.0f * M_PI;
        M5.Lcd.drawLine(
            cx + (int)(r       * cosf(a1)), cy + (int)(r       * sinf(a1)),
            cx + (int)(r       * cosf(a2)), cy + (int)(r       * sinf(a2)), col);
        M5.Lcd.drawLine(
            cx + (int)((r - 1) * cosf(a1)), cy + (int)((r - 1) * sinf(a1)),
            cx + (int)((r - 1) * cosf(a2)), cy + (int)((r - 1) * sinf(a2)), col);
    }
}

// ============================================================
// 绘制：主计时界面（三种 Style）
// ============================================================
void drawClock() {
    M5.Lcd.fillScreen(C_BG);

    if (gPomo == IDLE) {
        // IDLE：StackChan 居中 + READY 文字
        int cx = 160, cy = 95;
        drawStackChan(cx, cy, 52, 0, true);
        M5.Lcd.setTextColor(C_WHITE);
        M5.Lcd.setTextSize(2);
        M5.Lcd.setCursor(cx - 38, cy + 65);
        M5.Lcd.print("READY");
        M5.Lcd.setTextSize(1);
        M5.Lcd.setTextColor(cDim());
        M5.Lcd.setCursor(cx - 52, cy + 87);
        M5.Lcd.printf("Duration: %d min", DUR_OPTS[gDurIdx]);
        M5.Lcd.setTextColor(cAccent());
        M5.Lcd.setCursor(cx - 78, cy + 102);
        M5.Lcd.print("Hold [A]=voice  [A]=start");

    } else {
        // --- 核心修正：计时逻辑 ---
        uint32_t totalMs = (gPomo == WORK) ? gWorkMs :
                           (gPomo == SHORT_BRK ? SHORT_BRK_MS : LONG_BRK_MS);
        
        uint32_t elapsed = gPaused
                           ? gPauseAcc
                           : (millis() - gStartMs) + gPauseAcc;
        
        if (elapsed > totalMs) elapsed = totalMs;

        uint32_t remSec  = (totalMs - elapsed) / 1000;
        float    ratio   = 1.0f - (float)elapsed / (float)totalMs;
        
        // --- 视觉优化：工作用 Accent 色，休息用 Yellow 色 ---
        uint16_t arcCol  = (gPomo == WORK) ? cAccent() : cYellow();
        bool     isWork  = (gPomo == WORK);

        char tbuf[10];
        snprintf(tbuf, sizeof(tbuf), "%02u:%02u", remSec / 60, remSec % 60);

        if (gLookStyle == 0) {
            // ── Style 1：StackChan（上）+ 倒计时（下）
            int cx = 160, cy = 80;
            int tR = 42;
            drawArc(cx, cy, tR + 18, ratio, arcCol);
            drawStackChan(cx, cy, tR, gEmojiFrame, isWork);

            M5.Lcd.setTextSize(3);
            M5.Lcd.setTextColor(C_WHITE);
            M5.Lcd.setCursor(cx - (int)strlen(tbuf) * 9, cy + tR + 22);
            M5.Lcd.print(tbuf);

            M5.Lcd.setTextSize(1);
            M5.Lcd.setTextColor(arcCol);
            M5.Lcd.setCursor(cx - 32, cy + tR + 50);
            if      (gPomo == WORK)      M5.Lcd.printf("Focus #%d", gCycle + 1);
            else if (gPomo == SHORT_BRK) M5.Lcd.print("Short break");
            else                         M5.Lcd.print("Long break!");

        } else if (gLookStyle == 1) {
            // ── Style 2：仅倒计时大字
            int cx = 160, cy = 112;
            drawArc(cx, cy, 90, ratio, arcCol);
            M5.Lcd.setTextSize(4);
            M5.Lcd.setTextColor(C_WHITE);
            M5.Lcd.setCursor(cx - (int)strlen(tbuf) * 12, cy - 22);
            M5.Lcd.print(tbuf);
            M5.Lcd.setTextSize(2);
            M5.Lcd.setTextColor(arcCol);
            M5.Lcd.setCursor(cx - 50, cy + 25);
            if      (gPomo == WORK)      M5.Lcd.printf("Focus #%d", gCycle + 1);
            else if (gPomo == SHORT_BRK) M5.Lcd.print("Short Brk");
            else                         M5.Lcd.print("Long Brk!");

        } else {
            // ── Style 3：仅 StackChan 大表情（全屏）
            int cx = 160, cy = 105;
            int tR = 72;
            drawArc(cx, cy, tR + 10, ratio, arcCol);
            drawStackChan(cx, cy, tR, gEmojiFrame, isWork);

            M5.Lcd.setTextSize(1);
            M5.Lcd.setTextColor(arcCol);
            M5.Lcd.setCursor(cx - 32, cy + tR + 18);
            if      (gPomo == WORK)      M5.Lcd.printf("Focus #%d", gCycle + 1);
            else if (gPomo == SHORT_BRK) M5.Lcd.print("Short break");
            else                         M5.Lcd.print("Long break!");
        }

        // 暂停标志
        if (gPaused) {
            M5.Lcd.fillRoundRect(118, 186, 84, 18, 4, cRed());
            M5.Lcd.setTextColor(C_WHITE);
            M5.Lcd.setTextSize(1);
            M5.Lcd.setCursor(134, 193);
            M5.Lcd.print("PAUSED");
        }
    }

    // 底部状态栏
    M5.Lcd.setTextColor(cDim());
    M5.Lcd.setTextSize(1);
    M5.Lcd.setCursor(4, 205);
    // 这里显示今天星期几和已完成的专注次数
    M5.Lcd.printf("[%s] done:%d  Style%d",
                  DAY_LBL[gToday], gWeekly[gToday], gLookStyle + 1);

    M5.Lcd.setCursor(4,   222);
    if      (gPomo == IDLE) M5.Lcd.print("[A]Start ");
    else if (!gPaused)      M5.Lcd.print("[A]Pause ");
    else                    M5.Lcd.print("[A]Resume");
    M5.Lcd.setCursor(118, 222); M5.Lcd.print("[B]Stats");
    M5.Lcd.setCursor(228, 222); M5.Lcd.print("[C]Setup");
}
// ============================================================
// 绘制：统计图
// ============================================================
void drawStats() {
    M5.Lcd.fillScreen(C_BG);
    M5.Lcd.setTextColor(C_WHITE);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(8, 6);
    M5.Lcd.print("Weekly Focus");
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(cYellow());
    M5.Lcd.setCursor(210, 10);
    M5.Lcd.printf("Today:%d", gWeekly[gToday]);

    const int ox = 36, oy = 192, gh = 138;
    M5.Lcd.drawLine(ox, oy, ox + 252, oy,      cDim());
    M5.Lcd.drawLine(ox, oy, ox,       oy - gh, cDim());

    int maxV = 1;
    for (int i = 0; i < 7; i++) if (gWeekly[i] > maxV) maxV = gWeekly[i];

    const int bw = 26, gap = 7;
    for (int i = 0; i < 7; i++) {
        int bh  = (gWeekly[i] * gh) / maxV;
        int x   = ox + gap + i * (bw + gap);
        uint16_t col = (i == gToday) ? cYellow() : cAccent();
        if (bh > 0) M5.Lcd.fillRect(x, oy - bh, bw, bh, col);
        M5.Lcd.setTextColor(cDim());
        M5.Lcd.setCursor(x + 3, oy + 4);
        M5.Lcd.print(DAY_LBL[i]);
        if (gWeekly[i] > 0) {
            M5.Lcd.setTextColor(C_WHITE);
            M5.Lcd.setCursor(x + 3, oy - bh - 12);
            M5.Lcd.print(gWeekly[i]);
        }
    }

    M5.Lcd.setTextColor(cDim());
    M5.Lcd.setCursor(8, 222);
    M5.Lcd.print("Any button / tap = return");
}

// ============================================================
// 绘制：设置菜单
// ============================================================
void drawSettings() {
    M5.Lcd.fillScreen(M5.Lcd.color565(12, 12, 30));
    M5.Lcd.setTextColor(cYellow());
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(10, 6);
    M5.Lcd.print("Settings");

    const char* labels[SEL_COUNT] = { "Duration", "Display Style", "Ring" };
    char values[SEL_COUNT][30];
    if (DUR_OPTS[gDurIdx] == 0) {
    snprintf(values[SEL_DUR], 30, "5 sec (Test)");
} else {
    snprintf(values[SEL_DUR], 30, "%d min", DUR_OPTS[gDurIdx]);
}
    const char* styleDesc[] = {
        "Style1: Face+Timer",
        "Style2: Timer only",
        "Style3: Face only"
    };
    strncpy(values[SEL_LOOK], styleDesc[gLookStyle], 30);
    strncpy(values[SEL_RING], gRingStyle == 0 ? "Digital" : "Crescendo", 30);

    for (int i = 0; i < SEL_COUNT; i++) {
        bool     sel = (i == (int)gSetSel);
        uint16_t bg  = sel ? cSelbg() : M5.Lcd.color565(22, 22, 50);
        M5.Lcd.fillRoundRect(6, 40 + i * 56, 308, 50, 8, bg);
        if (sel) M5.Lcd.drawRoundRect(6, 40 + i * 56, 308, 50, 8, cAccent());

        M5.Lcd.setTextSize(1);
        M5.Lcd.setTextColor(sel ? C_WHITE : cDim());
        M5.Lcd.setCursor(16, 48 + i * 56);
        M5.Lcd.print(labels[i]);

        M5.Lcd.setTextSize(2);
        M5.Lcd.setTextColor(sel ? cAccent() : C_WHITE);
        M5.Lcd.setCursor(16, 62 + i * 56);
        M5.Lcd.print(values[i]);
    }

    M5.Lcd.setTextColor(cDim());
    M5.Lcd.setTextSize(1);
    M5.Lcd.setCursor(4, 222);
    M5.Lcd.print("[A]=change  [B]=next item  [C]=save");
}

// ============================================================
// 百度 TTS（在线，PCM16 16kHz → M5 Speaker）
// ============================================================
static bool ttsBaidu(const char* text) {
    if (WiFi.status() != WL_CONNECTED) return false;
    if (gAccessToken.length() == 0) return false;

    HTTPClient http;
    String body = "tex=" + String(text)
        + "&lan=zh&ctp=1&cuid=core2&tok=" + gAccessToken
        + "&spd=5&pit=5&vol=9&per=4&aue=6";

    http.begin("https://tsn.baidu.com/text2audio");
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    int code = http.POST(body);
    if (code != 200) {
        http.end();
        return false;
    }

    WiFiClient* stream = http.getStreamPtr();

    const int BUF_SIZE = 1024;
    uint8_t raw[BUF_SIZE];

    M5.Speaker.setVolume(180);

    while (http.connected()) {

        int avail = stream->available();
        if (avail <= 0) {
            delay(2);
            continue;
        }

        int n = stream->readBytes(raw, min(avail, BUF_SIZE));

        // ⚠️ 保证 16-bit 对齐
        int samples = n / 2;

        // 防止奇数 byte
        if (n % 2 != 0) n--;

        while (M5.Speaker.isPlaying()) { delay(1); }
        M5.Speaker.playRaw((int16_t*)raw, samples, 16000, false, 1, 0);
    }

    http.end();
    return true;
}
// ============================================================
// 统一语音输出入口（必须有！）
// ============================================================
void speakTask(void* p) {
    char* text = (char*)p;

    bool ok = ttsBaidu(text);

    if (!ok) {
        ttsBeep(4);
    }

    free(text);
    vTaskDelete(NULL);
}
void speakText(const char* text, int fallbackSyllables) {
    char* msg = strdup(text);

    xTaskCreate(
        speakTask,
        "tts",
        8192,
        msg,
        1,
        NULL
    );
}
// ============================================================
// 计时 Tick（每秒调用）
// ============================================================
void tickTimer() {
    if (gPomo == IDLE || gPaused) return;

    uint32_t totalMs = (gPomo == WORK)      ? gWorkMs      :
                       (gPomo == SHORT_BRK) ? SHORT_BRK_MS : LONG_BRK_MS;
    
    uint32_t elapsed = (millis() - gStartMs) + gPauseAcc;
    if (elapsed < totalMs) return;

    ringDone();

    if (gPomo == WORK) {
        gCycle++;
        nvSaveFocus();
        
        if (gCycle % LONG_INTERVAL == 0) {
            gPomo = LONG_BRK;
            speakText("休息结束，开始专注", 5);
            showBanner("Long break! Well done~", 3000);
        } else {
            gPomo = SHORT_BRK;
                speakText("专注完成，休息一下吧", 5);
            showBanner("Focus done! Short break~", 3000);
        }
    } else {
        // --- 这里是改动重点：休息结束后的处理 ---
        // 如果你想休息完自动开始下一个番茄钟：
        gPomo = WORK;
            speakText("休息结束，开始专注", 5); // 提示词更连贯
        showBanner("Break over. Focus Start!", 3000);
    }

    // 重置所有计时参数，进入下一个阶段
    gPauseAcc = 0;
    gPaused   = false;
    gStartMs  = millis();
    gNextRedrawMs = millis() + 300;
}

// ============================================================
// 屏保 & 活动检测
// ============================================================
void updateActivity() {
    bool active = M5.BtnA.wasPressed() || M5.BtnB.wasPressed() || M5.BtnC.wasPressed() || M5.BtnA.wasHold() || M5.Touch.getCount() > 0;
    if (active) {
        gLastActiveMs = millis();
        if (gScreenSleep) {
            M5.Display.setBrightness(200);
            gScreenSleep = false;
        }
    }
}

// ============================================================
// WiFi 连接（阻塞最多 8 秒）
// ============================================================
static void connectWiFiBlocking() {
    if (WiFi.status() == WL_CONNECTED) return;
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    uint32_t t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 8000) {
        delay(200);
    }
}

// ============================================================
// 百度 Access Token（只取一次，永久缓存在内存）
// ============================================================
static void ensureToken() {
    if (gAccessToken.length() > 0)     return;
    if (WiFi.status() != WL_CONNECTED) return;
    

    HTTPClient http;
    String body = "grant_type=client_credentials&client_id=";
    body += BAIDU_API_KEY;
    body += "&client_secret=";
    body += BAIDU_SEC_KEY;

    http.begin("https://aip.baidubce.com/oauth/2.0/token");
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    int code = http.POST(body);

    if (code == 200) {
        String res = http.getString();
        int a = res.indexOf("access_token\":\"");
        if (a != -1) {
            a += 15;
            int b = res.indexOf("\"", a);
            if (b != -1) gAccessToken = res.substring(a, b);
        }
    }
    http.end();
}

// ============================================================
// 录音（保存 PCM 到 PSRAM / fallback 静态缓冲）
// ============================================================
static void recordUntilSilence() {
    // 分配 PSRAM（仅第一次）
    gAudioBuf = (int16_t*)ps_malloc(AUDIO_MAX_SAMPLES * sizeof(int16_t));
    if (!gAudioBuf) {
        gAudioBuf = (int16_t*)malloc(AUDIO_MAX_SAMPLES * sizeof(int16_t));
    }
    gAudioLen = 0;

    // ── 录音 UI
    M5.Lcd.fillScreen(cVoicebg());
    // 麦克风图标（简洁版）
    M5.Lcd.fillRoundRect(148, 44, 26, 46, 12, cAccent());
    M5.Lcd.fillRect(158, 88, 6, 16, cAccent());
    M5.Lcd.fillRect(146, 102, 28, 4, cAccent());

    M5.Lcd.setTextColor(C_WHITE);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(55, 138);
    M5.Lcd.print("Listening...");
    M5.Lcd.setTextColor(cDim());
    M5.Lcd.setTextSize(1);
    M5.Lcd.setCursor(24, 163);
    M5.Lcd.print("e.g. \"kai shi wu fen zhong\"");
    M5.Lcd.setCursor(24, 176);
    M5.Lcd.print("     \"start 25 minutes\"");

    uint32_t startMs     = millis();
    uint32_t lastSoundMs = startMs;
    int16_t  micFrame[MIC_BUF_N];

    while (true) {
        M5.Mic.record(micFrame, MIC_BUF_N, MIC_RATE);

        // RMS 计算
        int64_t sum = 0;
        for (int i = 0; i < MIC_BUF_N; i++) sum += (int64_t)micFrame[i] * micFrame[i];
        int32_t rms = (int32_t)sqrtf((float)(sum / MIC_BUF_N));

        // 追加到缓冲
        if (gAudioLen + MIC_BUF_N < AUDIO_MAX_SAMPLES) {
            memcpy(gAudioBuf + gAudioLen, micFrame, MIC_BUF_N * sizeof(int16_t));
            gAudioLen += MIC_BUF_N;
        }

        uint32_t now = millis();
        if (rms > VOICE_THR) lastSoundMs = now;

        // 音量条
        int barW = constrain((int)(rms / 20), 0, 280);
        M5.Lcd.fillRect(20, 200, 280, 14, M5.Lcd.color565(15, 15, 40));
        M5.Lcd.fillRect(20, 200, barW, 14,
            rms > VOICE_THR ? cAccent() : M5.Lcd.color565(40, 90, 55));

        M5.update();

        // 停止条件：手指抬起 / 静音 800ms / 超 5 秒
        bool fingerUp = (M5.Touch.getCount() == 0);
        if ((fingerUp && now - startMs > 500)         ||
             now - startMs > VOICE_MAX_MS              ||
            (now - lastSoundMs > SILENCE_MS && now - startMs > 600)) {
            break;
        }
    }
}

// ============================================================
// 百度 ASR（POST PCM，返回 JSON 字符串）
// ============================================================
static String recognizeSpeech() {
    if (!gAudioBuf || gAudioLen == 0)  return "";
    if (WiFi.status() != WL_CONNECTED) return "OFFLINE";
    ensureToken();
    if (gAccessToken.length() == 0)    return "";

    String url = "https://vop.baidu.com/server_api?dev_pid=1537&cuid=core2&token="
               + gAccessToken;

    HTTPClient http;
    http.begin(url);
    http.addHeader("Content-Type", "audio/pcm;rate=16000");
    int code = http.POST((uint8_t*)gAudioBuf, gAudioLen * sizeof(int16_t));

    String res = "";
    if (code == 200) res = http.getString();
    http.end();
    return res;
}

// ============================================================
// 解析 ASR 结果中的分钟数
// ============================================================
static int parseMinutes(const String& result) {
    // 先提取 "result":["..."] 中的文本
    String text = result;
    int ri = result.indexOf("\"result\":[\"");
    if (ri != -1) {
        int s = ri + 11;
        int e = result.indexOf("\"", s);
        if (e != -1) text = result.substring(s, e);
    }

    // 阿拉伯数字（1~120）
    for (int i = 0; i < (int)text.length(); i++) {
        if (isDigit(text[i])) {
            int num = 0, j = i;
            while (j < (int)text.length() && isDigit(text[j]))
                num = num * 10 + (text[j++] - '0');
            if (num >= 1 && num <= 120) return num;
        }
    }

    // 中文数字（长词优先，避免"二十"被"二"截断）
    static const struct { const char* cn; int val; } cn[] = {
        {"六十",60},{"四十五",45},{"三十",30},{"二十五",25},
        {"二十",20},{"十五",15},{"十",10},
        {"九",9},{"八",8},{"七",7},{"六",6},
        {"五",5},{"四",4},{"三",3},{"二",2},{"一",1}
    };
    for (auto& n : cn)
        if (text.indexOf(n.cn) != -1) return n.val;

    return -1;
}

static bool isStopCmd(const String& r) {
    return r.indexOf("停")   != -1 || r.indexOf("结束") != -1 ||
           r.indexOf("stop") != -1 || r.indexOf("cancel") != -1;
}

// ============================================================
// 语音命令主流程
//   1. 录音
//   2. 联网 + 获取 Token（首次）
//   3. ASR 识别
//   4. 解析并执行（开始计时 / 停止 / 离线默认）
// ============================================================
void handleVoiceCommand() {
    // 1. 录音
    recordUntilSilence();

    // 2. 识别中提示
    M5.Lcd.fillScreen(cVoicebg());
    M5.Lcd.setTextColor(cYellow());
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(50, 108);
    M5.Lcd.print("Recognizing...");

    // 3. 确保 WiFi + Token
    connectWiFiBlocking();
    ensureToken();

    // 4. ASR
    String result  = recognizeSpeech();
    int    minutes = parseMinutes(result);
    bool   stopCmd = isStopCmd(result);

    if (stopCmd) {

    speakText("好的，已停止计时", 3);

    gPomo     = IDLE;
    gPaused   = false;
    gPauseAcc = 0;
    gStartMs  = 0;

    showBanner("Timer stopped.", 2000);
}

else if (minutes > 0) {

    char ttsMsg[64];

    if (minutes >= 60) {
        snprintf(ttsMsg, sizeof(ttsMsg),"开始专注%d分钟，请加油", minutes);
    } else {
        snprintf(ttsMsg, sizeof(ttsMsg),"开始%d分钟专注", minutes);
    }

    speakText(ttsMsg, 6);

    gWorkMs = minutes * 60000UL;
    gPomo     = WORK;
    gPaused   = false;
    gPauseAcc = 0;
    gStartMs  = millis();

    showBanner("Focus started!", 2500);
}

else if (result == "OFFLINE") {

    char ttsMsg[52];
    snprintf(ttsMsg, sizeof(ttsMsg),
             "离线模式，开始计时%d分钟", DUR_OPTS[gDurIdx]);

    speakText(ttsMsg, 5);

    gWorkMs = (DUR_OPTS[gDurIdx] == 0)
                ? 5000UL
                : DUR_OPTS[gDurIdx] * 60000UL;

    gPomo     = WORK;
    gPaused   = false;
    gPauseAcc = 0;
    gStartMs  = millis();

    showBanner("Offline mode started", 2500);
}

else {

    M5.Lcd.fillScreen(cVoicebg());
    M5.Lcd.setTextColor(cRed());
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(38, 95);
    M5.Lcd.print("Not recognized");

    ttsBeep(1);
    delay(2000);
}

gMode = MODE_CLOCK;
gNeedRedraw = true;
}
// ============================================================
// Setup
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(500);
    auto cfg = M5.config();
    M5.begin(cfg);
    M5.Speaker.setVolume(200);

    // 麦克风配置
    auto mc       = M5.Mic.config();
    mc.sample_rate = MIC_RATE;
    mc.stereo      = false;
    M5.Mic.config(mc);
    M5.Mic.begin();

    // 持久化加载
    nvLoad();
    gWorkMs = (DUR_OPTS[gDurIdx] == 0) ? 5000UL : (DUR_OPTS[gDurIdx] * 60000UL);
    gLastActiveMs = millis();

    // 显示初始化
    M5.Lcd.setRotation(1);
    M5.Lcd.fillScreen(C_BG);
    gNeedRedraw = true;

    // WiFi（后台连接；语音命令时再阻塞等待）
    WiFi.setAutoConnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    // 启动后稍等 WiFi，成功则立即拿 Token
    // （避免第一次语音命令时等待太久）
    uint32_t t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 5000) delay(200);
    if (WiFi.status() == WL_CONNECTED) ensureToken();
    if (WiFi.status() == WL_CONNECTED) {
    ensureToken();

    Serial.print("Core2 IP: ");
    Serial.println(WiFi.localIP());
}
}

// ============================================================
// Loop
// ============================================================
void loop() {
    M5.update();
    uint32_t now = millis();

    // 横幅超时
    if (gBannerOn && now > gBannerEndMs) {
        gBannerOn   = false;
        gNeedRedraw = true;
    }

    // StackChan 眨眼动画（计时中且未暂停）
    if (gPomo != IDLE && !gPaused && gMode == MODE_CLOCK) {
        if (gEmojiFrame == 0 && now - gEmojiTick > 3200) {
            gEmojiFrame = 1;
            gEmojiTick  = now;
            gNeedRedraw = true;
        } else if (gEmojiFrame == 1 && now - gEmojiTick > 180) {
            gEmojiFrame = 0;
            gNeedRedraw = true;
        }
    }

    // 每秒 Tick
    static uint32_t lastTick = 0;
    if (now - lastTick >= 1000) {
        lastTick = now;
        if (gMode == MODE_CLOCK) {
            tickTimer();
            if (gPomo != IDLE && !gPaused) gNeedRedraw = true;
        }
    }

    // 屏保
    updateActivity();
    if (!gScreenSleep && now - gLastActiveMs > SLEEP_TIMEOUT_MS) {
        M5.Display.setBrightness(0);
        gScreenSleep = true;
    }

    // ── 主界面 ──────────────────────────────────────────────
    if (gMode == MODE_CLOCK) {

        if (M5.BtnA.wasPressed()) {
            ringClick();
            if (gPomo == IDLE) {
                gWorkMs = (DUR_OPTS[gDurIdx] == 0)
                ? 5000UL
                : DUR_OPTS[gDurIdx] * 60000UL;
                gPomo     = WORK;
                gPaused   = false;
                gPauseAcc = 0;
                gStartMs  = now;
                showBanner("Focus starts! Good luck~");
            } else if (!gPaused) {
                gPaused    = true;
                if (gStartMs != 0) {
                    gPauseAcc += (now - gStartMs);
                }
                showBanner("Paused. Press A to resume.");
            } else {
                gPaused  = false;
                gStartMs = now;
                showBanner("Resumed!");
            }
            gNeedRedraw = true;
        }

        if (M5.BtnA.wasHold()) {
            handleVoiceCommand();
            return;
        }

        if (M5.BtnB.wasPressed()) {
            ringClick();
            gMode       = MODE_STATS;
            gNeedRedraw = true;
        }

        if (M5.BtnC.wasPressed()) {
            ringClick();
            gSetSel     = SEL_DUR;
            gMode       = MODE_SETTINGS;
            gNeedRedraw = true;
        }
    }

    // ── 统计图 ──────────────────────────────────────────────
    else if (gMode == MODE_STATS) {
        bool anyBtn = M5.BtnA.wasPressed() || M5.BtnB.wasPressed() || M5.BtnC.wasPressed();
        bool tap    = (M5.Touch.getCount() > 0 && M5.Touch.getDetail().wasClicked());
        if (anyBtn || tap) {
            ringClick();
            gMode       = MODE_CLOCK;
            gNeedRedraw = true;
        }
    }

    // ── 设置界面 ────────────────────────────────────────────
    else if (gMode == MODE_SETTINGS) {
        if (M5.BtnA.wasPressed()) {
            ringClick();
            switch (gSetSel) {
                case SEL_DUR:  gDurIdx    = (gDurIdx    + 1) % DUR_CNT; break;
                case SEL_LOOK: gLookStyle = (gLookStyle + 1) % 3;       break;
                case SEL_RING: gRingStyle = (gRingStyle + 1) % 2;       break;
                default: break;
            }
            gNeedRedraw = true;
        }
        if (M5.BtnB.wasPressed()) {
            ringClick();
            gSetSel     = (SettingsSel)((gSetSel + 1) % SEL_COUNT);
            gNeedRedraw = true;
        }
        if (M5.BtnC.wasPressed()) {
            ringClick();
            nvSaveSettings();
            gWorkMs = (DUR_OPTS[gDurIdx] == 0)
            ? 5000UL
            : DUR_OPTS[gDurIdx] * 60000UL;
            gMode       = MODE_CLOCK;
            gNeedRedraw = true;
            showBanner("Settings saved!");
        }
        if (M5.Touch.getCount() > 0 && M5.Touch.getDetail().wasClicked()) {
            gMode       = MODE_CLOCK;
            gNeedRedraw = true;
        }
    }

    // ── 重绘 ────────────────────────────────────────────────
    if (gNeedRedraw && millis() > gNextRedrawMs) {
        gNeedRedraw = false;
        switch (gMode) {
            case MODE_CLOCK:    drawClock();    break;
            case MODE_STATS:    drawStats();    break;
            case MODE_SETTINGS: drawSettings(); break;
            default: break;
        }
    }

    delay(20);
}