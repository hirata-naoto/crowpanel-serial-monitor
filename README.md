# crowpanel-serial-monitor

Elecrow CrowPanel 5インチ（ESP32-S3）を使ったシリアルモニター／VT100ターミナルエミュレーターです。  
内蔵UART1で外部デバイスと通信し、受信内容をTFT液晶にリアルタイム表示します。

---

## 特徴

- **VT100 / ANSI エスケープシーケンス対応**  
  カーソル移動・消去・SGR（文字色・背景色・太字・反転）などの主要シーケンスをサポート。
- **UART1 モニター表示**  
  外部デバイス（UART1）から受信したデータをTFT液晶に表示します。
- **カーソル点滅**  
  500 ms 周期でカーソルを点滅表示します。
- **ダーティフラグによる差分描画**  
  変更された行のみ再描画することで描画負荷を最小化します。

---

## 対応ハードウェア

| 項目 | 内容 |
|------|------|
| ボード | Elecrow CrowPanel 5.0" (ESP32-S3) |
| ディスプレイ | 800×480 RGB パラレル LCD |
| タッチ | GT911 (I2C, addr=0x14) |
| バックライト | PWM (GPIO2) |
| UART1 RX | GPIO44 |
| UART1 TX | GPIO43 |

---

## ピン配線（外部デバイス接続）

外部のマイコンやセンサーなど、モニタリングしたいデバイスと以下のように接続してください。

```
CrowPanel GPIO43 (TX) ──→ 対象デバイス RX
CrowPanel GPIO44 (RX) ←── 対象デバイス TX
GND ──────────────────── GND（共通グランド）
```

> **注意**: UARTのTX/RXは相互に交差接続します。ボーレートは両方とも 115200 bps に設定してください。

---

## 設定パラメータ（`main.ino`）

| 定数 | デフォルト値 | 説明 |
|------|-------------|------|
| `kTargetBaudRate` | 115200 | UART1（外部デバイスとの通信）のボーレート |
| `kTargetRxPin` | 44 | UART1 RXピン番号 |
| `kTargetTxPin` | 43 | UART1 TXピン番号 |
| `kCellWidth` | 12 | 1文字の幅（ピクセル） |
| `kCellHeight` | 16 | 1文字の高さ（ピクセル） |
| `kTabWidth` | 8 | タブ幅（文字数） |
| `kMaxCsiBufferSize` | 32 | CSIシーケンスの最大バッファサイズ |
| `kCursorBlinkMs` | 500 | カーソル点滅の周期（ミリ秒） |
| `kLoopDelayMs` | 2 | メインループの待機時間（ミリ秒） |
| `kDefaultForeground` | `TFT_GREEN` | デフォルト文字色 |
| `kDefaultBackground` | `TFT_BLACK` | デフォルト背景色 |

---

## ビルド方法

[Arduino IDE 2.x](https://www.arduino.cc/en/software) を使用します。

### 1. ESP32 ボードサポートのインストール

1. Arduino IDE の **File → Preferences** を開き、**Additional boards manager URLs** に以下を追加します。  
   ```
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```
2. **Tools → Board → Boards Manager** を開き、`esp32` を検索してインストールします。

### 2. ライブラリのインストール

**Sketch → Include Library → Manage Libraries** を開き、以下のライブラリをインストールします。

| ライブラリ | バージョン |
|-----------|-----------|
| LovyanGFX | 最新版 |

### 3. ボードの選択と書き込み

1. **Tools → Board → esp32 → ESP32S3 Dev Module** を選択します。
2. ボード設定は Elecrow 公式の以下ページを基準にしてください。  
   https://www.elecrow.com/wiki/ESP32_Display_5.0-inch_HMI_Arduino_Tutorial.html
3. 主要な設定例は以下の通りです（実際の環境に合わせて調整してください）。

   | 設定項目 | 値 |
   |---------|---|
   | Board | ESP32S3 Dev Module |
   | USB CDC On Boot | Enabled |
   | Flash Size | 4MB (32Mb) |
   | Partition Scheme | Huge APP |
   | PSRAM | OPI PSRAM |

4. CrowPanel を USB で接続し、**Tools → Port** で正しいポートを選択します。
5. **Sketch → Upload**（または `Ctrl+U`）でボードに書き込みます。

---

## 使い方

1. 外部デバイスを GPIO43/44 に接続し、電源を入れます。
2. Arduino IDE で **Sketch → Upload** を実行して CrowPanel にファームウェアを書き込みます。
3. 起動後、外部デバイスのUART1出力が液晶に表示されます。

---

## 対応エスケープシーケンス

| シーケンス | 機能 |
|-----------|------|
| `ESC[A` / `B` / `C` / `D` | カーソル移動（上/下/右/左） |
| `ESC[H` / `ESC[f` | カーソル位置指定 |
| `ESC[J` | 画面消去（0:カーソル以降 / 1:カーソルまで / 2:全消去） |
| `ESC[K` | 行消去（0:カーソル以降 / 1:カーソルまで / 2:全行） |
| `ESC[m` | SGR（文字色・背景色・太字・反転など） |
| `ESC[?25h` / `ESC[?25l` | カーソル表示/非表示 |
| `ESC[s` / `ESC[u` | カーソル位置の保存/復元 |
| `ESC 7` / `ESC 8` | カーソル位置の保存/復元（DEC方式） |
| `ESC c` | ターミナルリセット |

---

## ライセンス

このプロジェクトのライセンスはリポジトリのルートにある LICENSE ファイルを参照してください。
