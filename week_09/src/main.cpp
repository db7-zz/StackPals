#include <M5Unified.h>
#include <Avatar.h>

using namespace m5avatar;

// 实例化 Avatar 对象，这就是屏幕上那张脸1
Avatar avatar;

// 定义番茄钟的三个状态
enum PomoState { IDLE, WORK, BREAK };
PomoState currentState = IDLE;

// 定义时间时长 (单位：毫秒)
const uint32_t WORK_TIME = 10 * 1000;  
const uint32_t BREAK_TIME = 5 * 1000; 

// 【修改点】：名字从 timerStart 改为了 pomoStartTime，避免和 ESP32 底层硬件定时器函数撞名
uint32_t pomoStartTime = 0;
uint32_t lastUpdate = 0;

void setup() {
    // 1. 初始化 M5 硬件环境 (屏幕、喇叭、触控等)
    auto cfg = M5.config();
    M5.begin(cfg);
    
    // 2. 初始化 Avatar
    avatar.init();
    
    // 3. 设置初始表情和提示语
    avatar.setExpression(Expression::Neutral);
    avatar.setSpeechText("BtnA:Work | BtnB:Break | BtnC:Reset");
}

void loop() {
    // 刷新硬件状态，这行必须有，否则无法检测按键
    M5.update();
    uint32_t now = millis();

    // ========= 第一部分：按键交互逻辑 =========
    // 按下左侧虚拟键 (BtnA) -> 开始专注
    if (M5.BtnA.wasPressed()) {
        currentState = WORK;
        pomoStartTime = now; // 【修改点】
        avatar.setExpression(Expression::Angry); // 用“生气”表情代表专注和严肃
        M5.Speaker.tone(1000, 100);              // 滴一声反馈
    }
    // 按下中间虚拟键 (BtnB) -> 开始休息
    if (M5.BtnB.wasPressed()) {
        currentState = BREAK;
        pomoStartTime = now; // 【修改点】
        avatar.setExpression(Expression::Happy); // 休息时表现出开心
        M5.Speaker.tone(1000, 100);
    }
    // 按下右侧虚拟键 (BtnC) -> 重置/打断
    if (M5.BtnC.wasPressed()) {
        currentState = IDLE;
        avatar.setExpression(Expression::Neutral);
        avatar.setSpeechText("Ready!");
        M5.Speaker.tone(1000, 100);
    }

    // ========= 第二部分：番茄钟计时与 UI 刷新逻辑 =========
    if (currentState != IDLE) {
        // 每隔 1 秒 (1000ms) 更新一次屏幕上的倒计时，避免频繁刷新导致画面闪烁
        if (now - lastUpdate > 1000) { 
            lastUpdate = now;
            uint32_t elapsed = now - pomoStartTime; // 【修改点】
            uint32_t targetTime = (currentState == WORK) ? WORK_TIME : BREAK_TIME;
            
            // 判断是否倒计时结束
            if (elapsed >= targetTime) {
                currentState = IDLE;
                avatar.setExpression(Expression::Sleepy); // 时间到了，表现出“疲惫/闭眼”
                avatar.setSpeechText("Time's Up!");
                M5.Speaker.tone(2000, 500); // 长鸣一声作为闹钟提醒
            } else {
                // 计算剩余时间并格式化为 MM:SS
                uint32_t remainingSec = (targetTime - elapsed) / 1000;
                char buf[32];
                snprintf(buf, sizeof(buf), "%s: %02d:%02d", 
                         (currentState == WORK) ? "Focus" : "Relax",
                         remainingSec / 60, remainingSec % 60);
                
                // 将倒计时文字显示在 Avatar 的对话框中
                avatar.setSpeechText(buf);
            }
        }
    }
}
