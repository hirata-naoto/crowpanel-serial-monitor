// Elecrow CrowPanel 5インチ（ESP32-S3）向け LovyanGFX ドライバ設定
// 800×480 RGB パラレル LCD + GT911 タッチコントローラ + PWM バックライトを定義する。

#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include <driver/i2c.h>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>

// Elecrow CrowPanel 5インチ専用 LovyanGFX デバイスクラス
class LGFX : public lgfx::LGFX_Device {
 public:
  LGFX() {
    // ----- パネル設定（解像度・オフセット） -----
    {
      auto cfg = panel_.config();
      cfg.memory_width = 800;   // パネルのメモリ幅（ピクセル）
      cfg.memory_height = 480;  // パネルのメモリ高さ（ピクセル）
      cfg.panel_width = 800;    // 表示幅（ピクセル）
      cfg.panel_height = 480;   // 表示高さ（ピクセル）
      cfg.offset_x = 0;         // X方向オフセット
      cfg.offset_y = 0;         // Y方向オフセット
      panel_.config(cfg);
    }

    // ----- RGB パラレルバス設定（データピン・同期信号・タイミング） -----
    {
      auto cfg = bus_.config();
      cfg.panel = &panel_;
      // 16ビット RGB データバス（D0-D15）のGPIO割り当て
      cfg.pin_d0 = GPIO_NUM_8;
      cfg.pin_d1 = GPIO_NUM_3;
      cfg.pin_d2 = GPIO_NUM_46;
      cfg.pin_d3 = GPIO_NUM_9;
      cfg.pin_d4 = GPIO_NUM_1;
      cfg.pin_d5 = GPIO_NUM_5;
      cfg.pin_d6 = GPIO_NUM_6;
      cfg.pin_d7 = GPIO_NUM_7;
      cfg.pin_d8 = GPIO_NUM_15;
      cfg.pin_d9 = GPIO_NUM_16;
      cfg.pin_d10 = GPIO_NUM_4;
      cfg.pin_d11 = GPIO_NUM_45;
      cfg.pin_d12 = GPIO_NUM_48;
      cfg.pin_d13 = GPIO_NUM_47;
      cfg.pin_d14 = GPIO_NUM_21;
      cfg.pin_d15 = GPIO_NUM_14;
      cfg.pin_henable = GPIO_NUM_40;  // データイネーブル (DE)
      cfg.pin_vsync = GPIO_NUM_41;    // 垂直同期信号 (VSYNC)
      cfg.pin_hsync = GPIO_NUM_39;    // 水平同期信号 (HSYNC)
      cfg.pin_pclk = GPIO_NUM_0;      // ピクセルクロック (PCLK)
      cfg.freq_write = 12000000;      // ピクセルクロック周波数: 12 MHz
      // HSYNC タイミング設定（パネルデータシート準拠）
      cfg.hsync_polarity = 0;  // アクティブLow
      cfg.hsync_front_porch = 8;
      cfg.hsync_pulse_width = 4;
      cfg.hsync_back_porch = 43;
      // VSYNC タイミング設定
      cfg.vsync_polarity = 0;  // アクティブLow
      cfg.vsync_front_porch = 8;
      cfg.vsync_pulse_width = 4;
      cfg.vsync_back_porch = 12;
      cfg.pclk_active_neg = 1;  // ピクセルクロック立ち下がりエッジでサンプリング
      cfg.de_idle_high = 0;
      cfg.pclk_idle_high = 0;
      bus_.config(cfg);
    }
    panel_.setBus(&bus_);

    // ----- バックライト設定（PWM制御, GPIO2） -----
    {
      auto cfg = backlight_.config();
      cfg.pin_bl = GPIO_NUM_2;  // バックライト PWM ピン
      backlight_.config(cfg);
    }
    panel_.light(&backlight_);

    // ----- タッチコントローラ設定（GT911, I2C1, addr=0x14） -----
    {
      auto cfg = touch_.config();
      cfg.x_min = 0;              // タッチ有効範囲 X最小値
      cfg.x_max = 799;            // タッチ有効範囲 X最大値
      cfg.y_min = 0;              // タッチ有効範囲 Y最小値
      cfg.y_max = 479;            // タッチ有効範囲 Y最大値
      cfg.pin_int = -1;           // 割り込みピン（未使用）
      cfg.pin_rst = -1;           // リセットピン（未使用）
      cfg.bus_shared = false;     // I2Cバスを他デバイスと共有しない
      cfg.offset_rotation = 0;    // 回転オフセット
      cfg.i2c_port = I2C_NUM_1;   // I2C ポート番号
      cfg.pin_sda = GPIO_NUM_19;  // I2C SDA ピン
      cfg.pin_scl = GPIO_NUM_20;  // I2C SCL ピン
      cfg.freq = 400000;          // I2C 通信速度: 400 kHz (Fast Mode)
      cfg.i2c_addr = 0x14;        // GT911 I2C アドレス
      touch_.config(cfg);
      panel_.setTouch(&touch_);
    }

    setPanel(&panel_);
  }

 private:
  lgfx::Bus_RGB bus_;          // RGB パラレルバスドライバ
  lgfx::Panel_RGB panel_;      // RGB パネルドライバ
  lgfx::Light_PWM backlight_;  // PWM バックライト制御
  lgfx::Touch_GT911 touch_;    // GT911 タッチコントローラドライバ
};
