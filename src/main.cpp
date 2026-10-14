// main.cpp —— Arduino 入口：唤醒蠕虫，让四步循环跑起来
#include <Arduino.h>
#include "worm/Worm.h"

static Worm worm;

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println(F("==== AI Worm @ ESP32-S3 ===="));
    worm.begin();
    delay(1000); // 给网络 / 日志一点缓冲
}

void loop() {
    worm.loop();
}
