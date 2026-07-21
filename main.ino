// Elecrow CrowPanel 5インチ向け VT100 シリアルモニター
// UART1 → TFT液晶ターミナル表示 / USB(PC)へパススルー

#include <Arduino.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "LGFX_Elecrow_5inch.hpp"

namespace {
// ----- 通信設定 -----
constexpr uint32_t kTargetBaudRate = 115200;  // UART1（外部デバイスとの通信）のボーレート
constexpr uint32_t kUsbSerialBaudRate = 115200;  // USBシリアル（PC側モニター用）のボーレート
constexpr uint32_t kUsbSerialWaitTimeoutMs = 1000;  // USBシリアル接続待機の最大時間（ミリ秒）
constexpr size_t kUsbPassthroughChunkSize = 64;  // USBへまとめて送る最大バイト数
constexpr int kTargetRxPin = 44;  // UART1 RXピン（外部デバイスTXと接続）
constexpr int kTargetTxPin = 43;  // UART1 TXピン（外部デバイスRXと接続）

// ----- 表示設定 -----
constexpr uint8_t kCellWidth = 12;        // 1文字あたりの幅（ピクセル）
constexpr uint8_t kCellHeight = 16;       // 1文字あたりの高さ（ピクセル）
constexpr uint8_t kTabWidth = 8;          // タブストップの幅（文字数）
constexpr size_t kMaxCsiBufferSize = 32;  // CSIシーケンスの最大バッファサイズ
constexpr uint32_t kCursorBlinkMs = 500;  // カーソル点滅の周期（ミリ秒）
constexpr uint32_t kLoopDelayMs = 2;      // メインループの待機時間（ミリ秒）
constexpr uint16_t kDefaultForeground = TFT_GREEN;  // デフォルト文字色（緑）
constexpr uint16_t kDefaultBackground = TFT_BLACK;  // デフォルト背景色（黒）
}  // namespace

// TFT液晶へのターミナル描画を管理するクラス。
// 文字セル配列の管理、VT100エスケープシーケンスの解析、
// ダーティフラグによる差分描画を担当する。
class TerminalView {
 public:
  // 1文字分の描画情報
  struct Cell {
    char ch = ' ';                     // 表示する文字（デフォルトはスペース）
    uint16_t fg = kDefaultForeground;  // 文字色
    uint16_t bg = kDefaultBackground;  // 背景色
  };

  // ターミナルを初期化し、ディスプレイに接続する。
  // ディスプレイの解像度からカラム数・行数を計算して文字セル配列を確保し、
  // 起動メッセージを表示する。
  void begin(LGFX& display) {
    display_ = &display;
    display_->init();
    display_->setRotation(0);
    display_->setBrightness(192);
    display_->setColorDepth(16);
    display_->setFont(&fonts::Font0);
    display_->setTextSize(2);
    display_->setTextWrap(false, false);
    display_->fillScreen(kDefaultBackground);

    cols_ = std::max<uint16_t>(1, display_->width() / kCellWidth);
    rows_ = std::max<uint16_t>(1, display_->height() / kCellHeight);
    cells_.assign(static_cast<size_t>(cols_) * rows_, Cell{});
    dirty_rows_.assign(rows_, true);

    cursor_col_ = 0;
    cursor_row_ = 0;
    saved_col_ = 0;
    saved_row_ = 0;
    resetAttributes();
    cursor_visible_ = true;
    blink_state_ = true;
    last_blink_ms_ = millis();

    writeString("Elecrow 5\" LovyanGFX VT100 serial monitor\r\n");
    writeString("UART1 monitor ready.\r\n");
    writeString("UART1 RX=GPIO44 TX=GPIO43 115200bps\r\n\r\n");
    render(true);
  }

  // 受信した1バイトをターミナル状態機械に渡す。
  // 通常文字・制御文字・エスケープシーケンスを判別して処理する。
  void feed(uint8_t byte) {
    switch (state_) {
      case ParseState::kGround:
        handleGround(byte);
        break;
      case ParseState::kEscape:
        handleEscape(byte);
        break;
      case ParseState::kCsi:
        handleCsi(byte);
        break;
      case ParseState::kEscapeIntermediate:
        handleEscapeIntermediate(byte);
        break;
      case ParseState::kOsc:
        handleOsc(byte);
        break;
      case ParseState::kOscEsc:
        handleOscEsc(byte);
        break;
    }
  }

  // ダーティフラグが立っている行のみ再描画する。
  // force=true の場合は全行を強制再描画する。
  // カーソル点滅の更新もここで行う。
  void render(bool force = false) {
    if (millis() - last_blink_ms_ >= kCursorBlinkMs) {
      blink_state_ = !blink_state_;
      last_blink_ms_ = millis();
      markCellDirty(cursor_row_);
    }

    if (force) {
      std::fill(dirty_rows_.begin(), dirty_rows_.end(), true);
    }

    for (uint16_t row = 0; row < rows_; ++row) {
      if (!dirty_rows_[row]) {
        continue;
      }
      for (uint16_t col = 0; col < cols_; ++col) {
        drawCell(col, row,
                 cursor_visible_ && blink_state_ && row == cursor_row_ && col == cursor_col_);
      }
      dirty_rows_[row] = false;
    }
  }

  // ヌル終端文字列を1バイトずつ feed() に渡す。
  void writeString(const char* text) {
    while (*text != '\0') {
      feed(static_cast<uint8_t>(*text++));
    }
  }

 private:
  // VT100パーサーの状態
  // kGround:              通常文字入力待ち
  // kEscape:              ESC受信後
  // kCsi:                 ESC[ 受信後（CSIシーケンス収集中）
  // kEscapeIntermediate:  ESC ( / ) / * / + 受信後（文字セット指定: 後続1バイトを消費して無視）
  // kOsc:                 ESC] 受信後（OSCシーケンス収集中: BELまたはST終端まで無視）
  // kOscEsc:              OSC収集中にESCを受信した後（STの '\' を確認中）
  enum class ParseState { kGround, kEscape, kCsi, kEscapeIntermediate, kOsc, kOscEsc };

  LGFX* display_ = nullptr;
  std::vector<Cell> cells_;       // 全セルの配列（row * cols_ + col でインデックス）
  std::vector<bool> dirty_rows_;  // 再描画が必要な行のフラグ
  uint16_t cols_ = 0;             // 画面の列数
  uint16_t rows_ = 0;             // 画面の行数
  uint16_t cursor_col_ = 0;       // 現在のカーソル列
  uint16_t cursor_row_ = 0;       // 現在のカーソル行
  uint16_t saved_col_ = 0;        // 保存されたカーソル列（ESC s / ESC 7 で保存）
  uint16_t saved_row_ = 0;        // 保存されたカーソル行
  uint16_t current_fg_ = kDefaultForeground;  // 現在の文字色
  uint16_t current_bg_ = kDefaultBackground;  // 現在の背景色
  bool bold_ = false;                         // 太字属性
  int fg_index_ = -1;           // ANSIカラーインデックス（-1 = デフォルト）
  int bg_index_ = -1;           // ANSI背景カラーインデックス（-1 = デフォルト）
  bool bg_bright_ = false;      // 背景色のブライト属性
  bool inverse_ = false;        // 反転属性（文字色と背景色を入れ替え）
  bool cursor_visible_ = true;  // カーソルの表示/非表示
  bool blink_state_ = true;     // 点滅の現在状態（true=カーソル表示中）
  uint32_t last_blink_ms_ = 0;  // 前回の点滅切り替え時刻
  ParseState state_ = ParseState::kGround;  // 現在のパーサー状態
  std::string csi_buffer_;  // CSIシーケンスのパラメータ蓄積バッファ

  // (col, row) のセルへの参照を返す
  Cell& cell(uint16_t col, uint16_t row) {
    return cells_[static_cast<size_t>(row) * cols_ + col];
  }

  // 通常入力状態（kGround）でのバイト処理。
  // 制御文字（CR/LF/BS/TAB/ESC）と表示可能文字を振り分ける。
  void handleGround(uint8_t byte) {
    switch (byte) {
      case 0x1B:
        state_ = ParseState::kEscape;
        return;
      case '\r':
        setCursor(cursor_row_, 0);
        return;
      case '\n':
        newLine();
        return;
      case '\b':
        if (cursor_col_ > 0) {
          setCursor(cursor_row_, cursor_col_ - 1);
        }
        return;
      case '\t': {
        uint16_t next_tab = static_cast<uint16_t>(((cursor_col_ / kTabWidth) + 1U) * kTabWidth);
        while (cursor_col_ < cols_ && cursor_col_ < next_tab) {
          putCharacter(' ');
        }
        return;
      }
      default:
        break;
    }

    if (byte >= 0x20 && byte <= 0x7E) {
      putCharacter(static_cast<char>(byte));
    }
  }

  // ESC受信後のバイト処理。
  // '[' → CSI開始、'7'/'8' → カーソル保存/復元（DEC）、'c' → ターミナルリセット
  // '('/')''*''+' → 文字セット指定（後続1バイト消費して無視）
  // ']' → OSCシーケンス開始
  void handleEscape(uint8_t byte) {
    switch (byte) {
      case '[':
        csi_buffer_.clear();
        state_ = ParseState::kCsi;
        return;
      case '7':
        saved_col_ = cursor_col_;
        saved_row_ = cursor_row_;
        state_ = ParseState::kGround;
        return;
      case '8':
        setCursor(saved_row_, saved_col_);
        state_ = ParseState::kGround;
        return;
      case 'c':
        resetTerminal();
        state_ = ParseState::kGround;
        return;
      case '(':
      case ')':
      case '*':
      case '+':
        state_ = ParseState::kEscapeIntermediate;
        return;
      case ']':
        state_ = ParseState::kOsc;
        return;
      default:
        state_ = ParseState::kGround;
        return;
    }
  }

  // CSIシーケンス収集中のバイト処理。
  // パラメータ文字（0-9, ';', '?'）はバッファに蓄積し、
  // それ以外のバイトをコマンドとして executeCsi() に渡す。
  void handleCsi(uint8_t byte) {
    if ((byte >= '0' && byte <= '9') || byte == ';' || byte == '?') {
      if (csi_buffer_.size() < kMaxCsiBufferSize) {
        csi_buffer_.push_back(static_cast<char>(byte));
      }
      return;
    }

    executeCsi(static_cast<char>(byte));
    state_ = ParseState::kGround;
  }

  // 文字セット指定シーケンス（ESC ( / ) / * / +）の後続バイト処理。
  // 指定文字セットは無視し、後続の1バイトを消費してkGroundに戻る。
  void handleEscapeIntermediate(uint8_t /*byte*/) {
    state_ = ParseState::kGround;
  }

  // OSCシーケンス収集中のバイト処理。
  // BEL (0x07) でシーケンス終了、ESC (0x1B) はST終端の可能性があるため
  // kOscEsc に遷移する。それ以外のバイトはすべて無視する。
  void handleOsc(uint8_t byte) {
    if (byte == 0x07) {
      state_ = ParseState::kGround;
    } else if (byte == 0x1B) {
      state_ = ParseState::kOscEsc;
    }
  }

  // OSCシーケンス内でESCを受信した後のバイト処理。
  // '\' (ST: String Terminator) ならシーケンス終了。
  // ESC が連続した場合は kOscEsc にとどまる（ESC ESC \ の対応）。
  // それ以外はOSC収集継続。
  void handleOscEsc(uint8_t byte) {
    if (byte == '\\') {
      state_ = ParseState::kGround;
    } else if (byte != 0x1B) {
      // ESC以外はOSC収集に戻る（ESCの場合はkOscEscにとどまる）
      state_ = ParseState::kOsc;
    }
  }

  // CSIコマンドを解析して実行する。
  // 対応コマンド: A/B/C/D（カーソル移動）、H/f（位置指定）、
  //              J（画面消去）、K（行消去）、m（SGR）、s/u（保存/復元）
  void executeCsi(char command) {
    bool private_mode = !csi_buffer_.empty() && csi_buffer_.front() == '?';
    std::string payload = private_mode ? csi_buffer_.substr(1) : csi_buffer_;
    std::vector<int> params = parseParameters(payload);

    if (private_mode && (command == 'h' || command == 'l')) {
      if (!params.empty() && params.front() == 25) {
        cursor_visible_ = command == 'h';
        markCellDirty(cursor_row_);
      }
      return;
    }

    switch (command) {
      case 'A':
        moveCursorRelative(-static_cast<int>(clampCount(params, 1)), 0);
        break;
      case 'B':
        moveCursorRelative(static_cast<int>(clampCount(params, 1)), 0);
        break;
      case 'C':
        moveCursorRelative(0, static_cast<int>(clampCount(params, 1)));
        break;
      case 'D':
        moveCursorRelative(0, -static_cast<int>(clampCount(params, 1)));
        break;
      case 'H':
      case 'f': {
        int target_row = params.size() > 0 ? std::max(params[0], 1) - 1 : 0;
        int target_col = params.size() > 1 ? std::max(params[1], 1) - 1 : 0;
        setCursor(static_cast<uint16_t>(target_row), static_cast<uint16_t>(target_col));
        break;
      }
      case 'J':
        eraseInDisplay(params.empty() ? 0 : params[0]);
        break;
      case 'K':
        eraseInLine(params.empty() ? 0 : params[0]);
        break;
      case 'm':
        applySelectGraphicRendition(params);
        break;
      case 's':
        saved_col_ = cursor_col_;
        saved_row_ = cursor_row_;
        break;
      case 'u':
        setCursor(saved_row_, saved_col_);
        break;
      default:
        break;
    }
  }

  // CSIパラメータ文字列をセミコロン区切りで整数配列に変換する。
  // 空フィールドは 0 として扱う。
  std::vector<int> parseParameters(const std::string& payload) const {
    if (payload.empty()) {
      return {};
    }

    std::vector<int> params;
    size_t start = 0;
    while (start <= payload.size()) {
      size_t end = payload.find(';', start);
      std::string token =
          payload.substr(start, end == std::string::npos ? std::string::npos : end - start);
      params.push_back(token.empty() ? 0 : std::atoi(token.c_str()));
      if (end == std::string::npos) {
        break;
      }
      start = end + 1;
    }
    return params;
  }

  // params が空または非正の場合は fallback を、それ以外は params[0] を返す。
  // カーソル移動量など「省略時は1」の引数に使用する。
  uint16_t clampCount(const std::vector<int>& params, uint16_t fallback) const {
    int count = params.empty() || params.front() <= 0 ? fallback : params.front();
    return static_cast<uint16_t>(std::max(0, count));
  }

  // SGR (Select Graphic Rendition) エスケープシーケンスを処理し、
  // 文字色・背景色・太字・反転などの属性を更新する。
  void applySelectGraphicRendition(const std::vector<int>& params) {
    if (params.empty()) {
      resetAttributes();
      return;
    }

    for (int code : params) {
      switch (code) {
        case 0:
          resetAttributes();
          break;
        case 1:
          bold_ = true;
          if (fg_index_ >= 0) {
            current_fg_ = mapAnsiColor(fg_index_, true, false);
          }
          break;
        case 7:
          inverse_ = true;
          break;
        case 22:
          bold_ = false;
          if (fg_index_ >= 0) {
            current_fg_ = mapAnsiColor(fg_index_, false, false);
          } else {
            current_fg_ = kDefaultForeground;
          }
          break;
        case 27:
          inverse_ = false;
          break;
        case 39:
          fg_index_ = -1;
          current_fg_ = kDefaultForeground;
          break;
        case 49:
          bg_index_ = -1;
          bg_bright_ = false;
          current_bg_ = kDefaultBackground;
          break;
        default:
          if (code >= 30 && code <= 37) {
            fg_index_ = code - 30;
            current_fg_ = mapAnsiColor(fg_index_, bold_, false);
          } else if (code >= 40 && code <= 47) {
            bg_index_ = code - 40;
            bg_bright_ = false;
            current_bg_ = mapAnsiColor(bg_index_, false, true);
          } else if (code >= 90 && code <= 97) {
            fg_index_ = code - 90;
            current_fg_ = mapAnsiColor(fg_index_, true, false);
          } else if (code >= 100 && code <= 107) {
            bg_index_ = code - 100;
            bg_bright_ = true;
            current_bg_ = mapAnsiColor(bg_index_, true, true);
          }
          break;
      }
    }
  }

  // ANSIカラーインデックス（0-7）をRGB565色値に変換する。
  // bright=true でブライトカラー、background=true で背景色扱い（黒の処理が異なる）。
  uint16_t mapAnsiColor(int index, bool bright, bool background) const {
    static constexpr uint16_t colors[8] = {
        TFT_BLACK, TFT_RED, TFT_GREEN, TFT_YELLOW, TFT_BLUE, TFT_MAGENTA, TFT_CYAN, TFT_WHITE,
    };
    static constexpr uint16_t bright_colors[8] = {
        0x7BEF,    // bright black
        0xFBEA,    // bright red
        0x87F0,    // bright green
        0xFFF0,    // bright yellow
        0x7D7C,    // bright blue
        0xFD5F,    // bright magenta
        0x87FF,    // bright cyan
        TFT_WHITE  // bright white
    };

    uint16_t color = bright ? bright_colors[index % 8] : colors[index % 8];
    if (background && color == TFT_BLACK) {
      return bright ? 0x4208 : TFT_BLACK;
    }
    return color;
  }

  // SGR属性をすべてデフォルト値にリセットする。
  void resetAttributes() {
    bold_ = false;
    fg_index_ = -1;
    bg_index_ = -1;
    bg_bright_ = false;
    inverse_ = false;
    current_fg_ = kDefaultForeground;
    current_bg_ = kDefaultBackground;
  }

  // ED（Erase in Display）コマンドを実行する。
  // mode 0: カーソル位置から画面末尾まで消去
  // mode 1: 画面先頭からカーソル位置まで消去
  // mode 2: 画面全体を消去してカーソルを原点に移動
  void eraseInDisplay(int mode) {
    switch (mode) {
      case 2:
        clearAll();
        setCursor(0, 0);
        break;
      case 1:
        for (uint16_t row = 0; row <= cursor_row_; ++row) {
          uint16_t limit = row == cursor_row_ ? cursor_col_ + 1 : cols_;
          for (uint16_t col = 0; col < limit; ++col) {
            clearCell(col, row);
          }
        }
        break;
      case 0:
      default:
        for (uint16_t row = cursor_row_; row < rows_; ++row) {
          uint16_t begin = row == cursor_row_ ? cursor_col_ : 0;
          for (uint16_t col = begin; col < cols_; ++col) {
            clearCell(col, row);
          }
        }
        break;
    }
  }

  // EL（Erase in Line）コマンドを実行する。
  // mode 0: カーソル位置から行末まで消去
  // mode 1: 行頭からカーソル位置まで消去
  // mode 2: 行全体を消去
  void eraseInLine(int mode) {
    switch (mode) {
      case 2:
        for (uint16_t col = 0; col < cols_; ++col) {
          clearCell(col, cursor_row_);
        }
        break;
      case 1:
        for (uint16_t col = 0; col <= cursor_col_; ++col) {
          clearCell(col, cursor_row_);
        }
        break;
      case 0:
      default:
        for (uint16_t col = cursor_col_; col < cols_; ++col) {
          clearCell(col, cursor_row_);
        }
        break;
    }
  }

  // 全セルをデフォルト状態にクリアする。
  void clearAll() {
    for (uint16_t row = 0; row < rows_; ++row) {
      for (uint16_t col = 0; col < cols_; ++col) {
        clearCell(col, row);
      }
    }
  }

  // 指定セルをデフォルト状態にリセットし、行をダーティとしてマークする。
  void clearCell(uint16_t col, uint16_t row) {
    Cell& target = cell(col, row);
    target = Cell{};
    markCellDirty(row);
  }

  // 現在のカーソル位置に文字を書き込み、カーソルを1列進める。
  // 行末に達した場合は改行する。
  void putCharacter(char ch) {
    Cell& target = cell(cursor_col_, cursor_row_);
    target.ch = ch;
    target.fg = inverse_ ? current_bg_ : current_fg_;
    target.bg = inverse_ ? current_fg_ : current_bg_;
    markCellDirty(cursor_row_);

    if (cursor_col_ + 1 >= cols_) {
      newLine();
    } else {
      setCursor(cursor_row_, cursor_col_ + 1);
    }
  }

  // カーソルを指定位置に移動する。境界値クランプを行い、関連行をダーティとしてマークする。
  void setCursor(uint16_t row, uint16_t col) {
    uint16_t old_row = cursor_row_;
    cursor_row_ = std::min<uint16_t>(row, rows_ - 1);
    cursor_col_ = std::min<uint16_t>(col, cols_ - 1);
    markCellDirty(old_row);
    markCellDirty(cursor_row_);
  }

  // 現在のカーソル位置から相対的にカーソルを移動する。画面端でクランプする。
  void moveCursorRelative(int row_delta, int col_delta) {
    int row = static_cast<int>(cursor_row_) + row_delta;
    int col = static_cast<int>(cursor_col_) + col_delta;
    row = std::max(0, std::min(row, static_cast<int>(rows_ - 1)));
    col = std::max(0, std::min(col, static_cast<int>(cols_ - 1)));
    setCursor(static_cast<uint16_t>(row), static_cast<uint16_t>(col));
  }

  // 改行処理。カーソルを行頭に戻し、最終行なら画面全体を1行上にスクロールする。
  void newLine() {
    uint16_t old_row = cursor_row_;
    cursor_col_ = 0;
    if (cursor_row_ + 1 >= rows_) {
      scrollUp();
    } else {
      cursor_row_++;
    }
    markCellDirty(old_row);
    markCellDirty(cursor_row_);
  }

  // 画面全体を1行上にスクロールする。最終行は空セルで埋める。
  void scrollUp() {
    for (uint16_t row = 1; row < rows_; ++row) {
      for (uint16_t col = 0; col < cols_; ++col) {
        cell(col, row - 1) = cell(col, row);
      }
      dirty_rows_[row - 1] = true;
    }
    for (uint16_t col = 0; col < cols_; ++col) {
      cell(col, rows_ - 1) = Cell{};
    }
    dirty_rows_[rows_ - 1] = true;
  }

  // ターミナルを完全リセットする（画面クリア・属性リセット・カーソル原点移動）。
  void resetTerminal() {
    clearAll();
    resetAttributes();
    setCursor(0, 0);
  }

  // 指定セルをTFTに描画する。
  // show_cursor=true のとき前景色と背景色を入れ替えてカーソルを表現する。
  void drawCell(uint16_t col, uint16_t row, bool show_cursor) {
    const Cell& source = cell(col, row);
    uint16_t fg = source.fg;
    uint16_t bg = source.bg;
    if (show_cursor) {
      std::swap(fg, bg);
    }

    int16_t x = static_cast<int16_t>(col * kCellWidth);
    int16_t y = static_cast<int16_t>(row * kCellHeight);
    display_->fillRect(x, y, kCellWidth, kCellHeight, bg);
    display_->setTextColor(fg, bg);
    display_->setCursor(x, y);
    display_->print(source.ch);
  }

  // 指定行をダーティとしてマークする（境界チェックあり）。
  void markCellDirty(uint16_t row) {
    if (row < dirty_rows_.size()) {
      dirty_rows_[row] = true;
    }
  }
};

// グローバルオブジェクト
LGFX display;                   // LovyanGFX ディスプレイドライバ
TerminalView terminal_view;     // VT100 ターミナル表示管理
HardwareSerial target_uart(1);  // UART1（外部デバイスとの通信用）
uint8_t usb_passthrough_buffer[kUsbPassthroughChunkSize];  // UART1受信データのUSB転送バッファ

// 初期化処理。ターミナル・UART1 を順に起動する。
void setup() {
  terminal_view.begin(display);      // ディスプレイ初期化・起動メッセージ表示
  Serial.begin(kUsbSerialBaudRate);  // USBシリアル開始
  const uint32_t usb_serial_wait_start = millis();
  while (!Serial) {
    const uint32_t elapsed_ms = millis() - usb_serial_wait_start;
    if (elapsed_ms >= kUsbSerialWaitTimeoutMs) {
      break;
    }
    delay(1);
  }
  target_uart.begin(kTargetBaudRate, SERIAL_8N1, kTargetRxPin, kTargetTxPin);  // UART1 開始
}

// メインループ。UART1受信データを液晶に表示し、最後に差分描画を行う。
void loop() {
  size_t usb_buffer_len = 0;
  const int available_bytes = target_uart.available();
  if (available_bytes > 0) {
    const size_t bytes_to_read =
        std::min(static_cast<size_t>(available_bytes), kUsbPassthroughChunkSize);
    usb_buffer_len = target_uart.readBytes(usb_passthrough_buffer, bytes_to_read);
    for (size_t i = 0; i < usb_buffer_len; ++i) {
      terminal_view.feed(usb_passthrough_buffer[i]);
    }
  }

  if (usb_buffer_len > 0 && Serial) {
    Serial.write(usb_passthrough_buffer, usb_buffer_len);
  }

  terminal_view.render();  // ダーティ行のみ再描画
  delay(kLoopDelayMs);     // CPUを解放して他処理に時間を与える
}
