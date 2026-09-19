//*********************************************************************************
//**
//**  ML_GFX.ino  -  Touch screen for the Waveshare ESP32-S3-Touch-LCD-7 (800x480)
//**
//**  - CH422G I/O expander bring-up (LCD reset, backlight, touch reset, SD chip select)
//**  - RGB panel driver (GFX Library for Arduino, Arduino_ESP32RGBPanel)
//**  - Imitation of the HD44780 20x4 character LCD (dot matrix look) from the virt_lcd[] buffer
//**  - Touch buttons (GT911 controller): encoder emulation, MENU, UP, DOWN, TUNE, RECAL, ANT, SD
//**  - SD card popup (save / restore, see ML_SD.ino)
//**
//**  Screen layout (800 x 480):
//**    y =   8..256   LCD bezel with 20 x 4 characters (36 x 54 pixels per character)
//**    y = 260..286   status line (SD state)
//**    y = 290..370   row 1: encoder buttons  << < > >>
//**    y = 380..468   row 2: MENU UP DOWN TUNE RECAL ANT SD
//**
//**  Only functions with basic types are used in signatures (Arduino IDE prototype generator).
//**
//*********************************************************************************

#if defined(ESP32)

//-----------------------------------------------------------------------------
//   CH422G I/O expander
//   0x24 : system parameters (bit0 = IO_OE, EXIO0..7 are outputs)
//   0x38 : output register EXIO0..7
//-----------------------------------------------------------------------------
#define CH422G_ADDR_SET   0x24
#define CH422G_ADDR_OUT   0x38

static uint8_t ch422g_out = 0;
static bool    ch422g_ok  = false;

static bool ch422g_write_out(void)
{
  Wire.beginTransmission(CH422G_ADDR_OUT);
  Wire.write(ch422g_out);
  return (Wire.endTransmission() == 0);
}

// Set one expander output. Called from the main task only (Wire is not thread safe).
void ch422g_set(uint8_t bit, bool level)
{
  if (!ch422g_ok) return;
  if (level) ch422g_out |= (uint8_t)(1 << bit);
  else       ch422g_out &= (uint8_t)~(1 << bit);
  ch422g_write_out();
}

//
// Board bring-up. Wire.begin() must have been called.
// Leaves: LCD out of reset, backlight OFF, touch controller out of reset (I2C address 0x5D),
//         SD card deselected, USB port connected to the ESP32-S3.
//
bool board_init(void)
{
  // INT held low while the touch controller leaves reset selects I2C address 0x5D
  pinMode(TOUCH_INT_PIN, OUTPUT);
  digitalWrite(TOUCH_INT_PIN, LOW);

  Wire.beginTransmission(CH422G_ADDR_SET);
  Wire.write(0x01);
  if (Wire.endTransmission() != 0)
  {
    ch422g_ok = false;
    pinMode(TOUCH_INT_PIN, INPUT);
    return false;
  }
  ch422g_ok = true;

  ch422g_out = (uint8_t)(1 << EXIO_SD_CS);     // all low (touch in reset, LCD in reset, BL off, USB mode), SD deselected
  ch422g_write_out();
  delay(120);
  ch422g_set(EXIO_LCD_RST, true);              // LCD out of reset
  delay(100);
  ch422g_set(EXIO_TP_RST, true);               // touch out of reset, INT is low -> address 0x5D
  delay(200);
  pinMode(TOUCH_INT_PIN, INPUT);
  return true;
}

//-----------------------------------------------------------------------------
//   GT911 touch controller
//-----------------------------------------------------------------------------
static uint8_t gt911_addr  = 0x5D;
static bool    gt911_found = false;

static bool gt911_read(uint16_t reg, uint8_t *buf, uint8_t len)
{
  Wire.beginTransmission(gt911_addr);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom((uint8_t)gt911_addr, (uint8_t)len) != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

static void gt911_clear_status(void)
{
  Wire.beginTransmission(gt911_addr);
  Wire.write((uint8_t)0x81);
  Wire.write((uint8_t)0x4E);
  Wire.write((uint8_t)0x00);
  Wire.endTransmission();
}

static void gt911_detect(void)
{
  gt911_found = false;
  const uint8_t addrs[2] = {0x5D, 0x14};
  for (uint8_t i = 0; i < 2; i++)
  {
    Wire.beginTransmission(addrs[i]);
    if (Wire.endTransmission() == 0)
    {
      gt911_addr  = addrs[i];
      gt911_found = true;
      break;
    }
  }
}

//
// Returns 0 = no new information, 1 = finger down (x,y valid), 2 = finger lifted.
// Point 1 starts at 0x814F: byte 0 = track id, 1..2 = X, 3..4 = Y (little endian).
//
static uint8_t touch_poll(uint16_t *x, uint16_t *y)
{
  uint8_t st;
  if (!gt911_found) return 0;
  if (!gt911_read(0x814E, &st, 1)) return 0;
  if (!(st & 0x80)) return 0;                 // no new coordinate frame
  uint8_t n = st & 0x0F;
  uint8_t r = 2;
  if (n > 0 && n <= 5)
  {
    uint8_t d[5];
    if (gt911_read(0x814F, d, 5))
    {
      *x = (uint16_t)(d[1] | (d[2] << 8));
      *y = (uint16_t)(d[3] | (d[4] << 8));
      r = 1;
    }
    else r = 0;
  }
  gt911_clear_status();
  return r;
}


#if GFX_ENABLED

#include "ML_Font5x7.h"

//-----------------------------------------------------------------------------
//   Geometry and colours
//-----------------------------------------------------------------------------
#define UI_SCR_W      800
#define UI_SCR_H      480

#define LCDG_DOT        5                 // one LCD dot = 5 x 5 pixels
#define LCDG_PITCH      6                 // dot pitch = dot + 1 pixel gap
#define LCDG_CELL_W    36                 // character cell: 5 dots + 1 dot gap
#define LCDG_CELL_H    54                 // 8 dot rows + 1 dot gap
#define LCDG_X0        40                 // top left of the character matrix
#define LCDG_Y0        24

#define UI_STATUS_Y   260
#define UI_PANEL_Y    258                 // lower part of the screen (buttons or SD popup)
#define UI_PANEL_H    222

#if LCD_THEME == 0                        // white on blue, backlit STN
  #define CL_LCD_GAP   RGB565(4, 24, 80)
  #define CL_LCD_OFF   RGB565(10, 44, 120)
  #define CL_LCD_ON    RGB565(225, 238, 255)
#else                                     // dark on green/yellow
  #define CL_LCD_GAP   RGB565(126, 146, 22)
  #define CL_LCD_OFF   RGB565(142, 162, 28)
  #define CL_LCD_ON    RGB565(18, 28, 6)
#endif
#define CL_BG          RGB565(14, 16, 20)
#define CL_BEZEL       RGB565(28, 30, 36)
#define CL_BTN         RGB565(44, 50, 62)
#define CL_BTN_EDGE    RGB565(100, 108, 126)
#define CL_BTN_EDGE_D  RGB565(58, 62, 72)
#define CL_BTN_DN      RGB565(0, 110, 190)
#define CL_TXT         RGB565(240, 244, 250)
#define CL_TXT_DIM     RGB565(110, 116, 128)
#define CL_OK          RGB565(60, 200, 90)
#define CL_ERR         RGB565(230, 70, 60)
#define CL_WARN        RGB565(240, 180, 40)

//-----------------------------------------------------------------------------
//   Buttons
//-----------------------------------------------------------------------------
#define UIB_NONE       -1
#define UIB_ENC_FL      0                 // << fast down
#define UIB_ENC_SL      1                 // <  single step down
#define UIB_ENC_SR      2                 // >  single step up
#define UIB_ENC_FR      3                 // >> fast up
#define UIB_MENU        4
#define UIB_UP          5
#define UIB_DOWN        6
#define UIB_TUNE        7
#define UIB_RECAL       8
#define UIB_ANT         9
#define UIB_SD         10
#define UIB_MAIN_COUNT 11
#define UIB_SD_SAVE    11                 // SD popup
#define UIB_SD_LOAD    12
#define UIB_SD_CLOSE   13
#define UIB_SD_YES     14
#define UIB_SD_NO      15
#define UIB_COUNT      16

#define UIM_MAIN        0
#define UIM_SD          1

static const int16_t ui_geo[UIB_COUNT][4] = {      // x, y, w, h
  {  20, 290, 181, 80}, { 213, 290, 181, 80}, { 406, 290, 181, 80}, { 599, 290, 181, 80},
  {  20, 380, 100, 88}, { 130, 380, 100, 88}, { 240, 380, 100, 88}, { 350, 380, 100, 88},
  { 460, 380, 100, 88}, { 570, 380, 100, 88}, { 680, 380, 100, 88},
  {  20, 372, 240, 88}, { 280, 372, 240, 88}, { 540, 372, 240, 88},
  {  20, 372, 370, 88}, { 410, 372, 370, 88}
};

static Arduino_ESP32RGBPanel *gfx_panel = NULL;
static Arduino_RGB_Display   *gfx       = NULL;
static bool     gfx_ok = false;

static uint8_t  lcd_shown[80];                     // what is currently drawn, per character position
static uint16_t lcd_cell_buf[LCDG_CELL_W * LCDG_CELL_H];

static uint8_t  ui_mode        = UIM_MAIN;
static bool     ui_sd_confirm  = false;
static int8_t   ui_pressed     = UIB_NONE;
static uint32_t ui_press_ms    = 0;
static uint32_t ui_next_repeat = 0;
static uint32_t ui_last_frame  = 0;
static uint32_t ui_last_action = 0;
static bool     ui_long_done   = false;

//-----------------------------------------------------------------------------
//   LCD imitation
//-----------------------------------------------------------------------------

// Fill rows[8] with the 5-bit dot pattern of a character (bit 4 = leftmost dot)
static void lcd_glyph_rows(uint8_t ch, uint8_t *rows)
{
  uint8_t r, c;
  for (r = 0; r < 8; r++) rows[r] = 0;
  if (ch < 7)                                      // bargraph symbols, same as lcd.createChar()
  {
    for (r = 0; r < 8; r++) rows[r] = LcdCustomChar[ch][r] & 0x1F;
  }
  else if (ch >= ML_FONT_FIRST && ch <= ML_FONT_LAST)
  {
    const uint8_t *g = &ml_font5x7[(ch - ML_FONT_FIRST) * 5];
    for (r = 0; r < 8; r++)
      for (c = 0; c < 5; c++)
        if ((g[c] >> r) & 1) rows[r] |= (uint8_t)(0x10 >> c);
  }
}

// Draw one character cell (idx = 0..79) with one bitmap transfer
static void lcd_draw_cell(uint8_t idx, uint8_t ch)
{
  uint8_t  rows[8];
  uint16_t line[LCDG_CELL_W];
  uint8_t  r, c, k;
  uint16_t i;

  lcd_glyph_rows(ch, rows);
  for (r = 0; r < 8; r++)
  {
    for (c = 0; c < 5; c++)
    {
      uint16_t col = (rows[r] & (0x10 >> c)) ? CL_LCD_ON : CL_LCD_OFF;
      for (k = 0; k < LCDG_DOT; k++) line[c * LCDG_PITCH + k] = col;
      line[c * LCDG_PITCH + LCDG_DOT] = CL_LCD_GAP;
    }
    for (k = 5 * LCDG_PITCH; k < LCDG_CELL_W; k++) line[k] = CL_LCD_GAP;

    uint16_t *dst = &lcd_cell_buf[r * LCDG_PITCH * LCDG_CELL_W];
    for (k = 0; k < LCDG_DOT; k++) memcpy(dst + k * LCDG_CELL_W, line, LCDG_CELL_W * sizeof(uint16_t));
    for (i = 0; i < LCDG_CELL_W; i++) dst[LCDG_DOT * LCDG_CELL_W + i] = CL_LCD_GAP;   // gap row
  }
  for (i = 8 * LCDG_PITCH * LCDG_CELL_W; i < LCDG_CELL_W * LCDG_CELL_H; i++) lcd_cell_buf[i] = CL_LCD_GAP;

  gfx->draw16bitRGBBitmap(LCDG_X0 + (idx % 20) * LCDG_CELL_W,
                          LCDG_Y0 + (idx / 20) * LCDG_CELL_H,
                          lcd_cell_buf, LCDG_CELL_W, LCDG_CELL_H);
}

// Draw the characters of virt_lcd[] that changed, at most 'budget' of them
static void lcd_render(uint8_t budget)
{
  static uint8_t pos = 0;
  for (uint8_t n = 0; n < 80 && budget > 0; n++)
  {
    uint8_t ch = (uint8_t)virt_lcd[pos];
    if (ch != lcd_shown[pos])
    {
      lcd_shown[pos] = ch;
      lcd_draw_cell(pos, ch);
      budget--;
    }
    if (++pos >= 80) pos = 0;
  }
}

static void lcd_draw_frame(void)
{
  gfx->fillRoundRect(24, 8, 752, 248, 10, CL_BEZEL);
  gfx->fillRect(LCDG_X0 - 6, LCDG_Y0 - 6, 20 * LCDG_CELL_W + 12, 4 * LCDG_CELL_H + 12, CL_LCD_GAP);
  memset(lcd_shown, 0xFF, sizeof(lcd_shown));     // force a complete redraw
}

//-----------------------------------------------------------------------------
//   Text helpers (built-in 5x7 font, 6 x 8 pixels per character at size 1)
//-----------------------------------------------------------------------------
static void ui_text(int16_t x, int16_t y, uint8_t size, uint16_t fg, uint16_t bg, const char *s)
{
  gfx->setTextSize(size);
  gfx->setTextColor(fg, bg);
  gfx->setCursor(x, y);
  gfx->print(s);
}

static void ui_text_centered(int16_t x, int16_t y, int16_t w, uint8_t size, uint16_t fg, uint16_t bg, const char *s)
{
  int16_t tw = (int16_t)(strlen(s) * 6 * size);
  ui_text(x + (w - tw) / 2, y, size, fg, bg, s);
}

//-----------------------------------------------------------------------------
//   Button state, labels and drawing
//-----------------------------------------------------------------------------
static bool ui_enabled(int8_t id)
{
  switch (id)
  {
    case UIB_TUNE:
      #if PSWR_AUTOTUNE
      return !flag.config_menu;
      #else
      return false;
      #endif
    case UIB_RECAL:
      #if RECALIBRATE
      return !flag.config_menu && !swr.tune;
      #else
      return false;
      #endif
    case UIB_ANT:
      #if ANT_CHG_2BANKS && !ANT1_CHANGEOVER
      return true;
      #else
      return false;                                // antenna follows the frequency
      #endif
    case UIB_SD:
      return true;
    case UIB_SD_SAVE:
    case UIB_SD_LOAD:
    case UIB_SD_YES:
      return (sd_get_status() != SD_ST_BUSY);
    case UIB_SD_NO:
      return (sd_get_status() != SD_ST_BUSY);
    default:
      return true;
  }
}

static const char *ui_label(int8_t id)
{
  static char buf[8];
  switch (id)
  {
    case UIB_ENC_FL:  return "<<";
    case UIB_ENC_SL:  return "<";
    case UIB_ENC_SR:  return ">";
    case UIB_ENC_FR:  return ">>";
    case UIB_MENU:    return flag.config_menu ? "ENTER" : "MENU";
    case UIB_UP:      return "UP";
    case UIB_DOWN:    return "DOWN";
    case UIB_TUNE:    return "TUNE";
    case UIB_RECAL:   return "RECAL";
    case UIB_ANT:     sprintf(buf, "ANT %u", (unsigned)(ant + 1)); return buf;
    case UIB_SD:      return "SD";
    case UIB_SD_SAVE: return "SAUVEGARDER";
    case UIB_SD_LOAD: return "RESTAURER";
    case UIB_SD_CLOSE:return "FERMER";
    case UIB_SD_YES:  return "OUI, RESTAURER";
    case UIB_SD_NO:   return "NON";
    default:          return "";
  }
}

static const char *ui_caption(int8_t id)
{
  switch (id)
  {
    case UIB_ENC_FL:  return flag.config_menu ? "PREV" : "FAST";
    case UIB_ENC_SL:  return flag.config_menu ? "PREV" : "STEP";
    case UIB_ENC_SR:  return flag.config_menu ? "NEXT" : "STEP";
    case UIB_ENC_FR:  return flag.config_menu ? "NEXT" : "FAST";
    default:          return "";
  }
}

static void ui_draw_button(int8_t id, bool pressed)
{
  int16_t x = ui_geo[id][0], y = ui_geo[id][1], w = ui_geo[id][2], h = ui_geo[id][3];
  bool     en   = ui_enabled(id);
  uint16_t fill = pressed ? CL_BTN_DN : CL_BTN;
  uint16_t fg   = en ? CL_TXT : CL_TXT_DIM;

  gfx->fillRoundRect(x, y, w, h, 10, fill);
  gfx->drawRoundRect(x, y, w, h, 10, en ? CL_BTN_EDGE : CL_BTN_EDGE_D);

  if (id <= UIB_ENC_FR)                            // big arrow + caption
  {
    ui_text_centered(x, y + 10, w, 4, fg, fill, ui_label(id));
    ui_text_centered(x, y + h - 24, w, 2, CL_TXT_DIM, fill, ui_caption(id));
  }
  else
  {
    ui_text_centered(x, y + (h - 16) / 2, w, 2, fg, fill, ui_label(id));
  }

  if (id == UIB_SD)                                // small status light
  {
    uint8_t  st  = sd_get_status();
    uint16_t col = (st == SD_ST_NONE || st == SD_ST_ERR) ? CL_ERR : (st == SD_ST_BUSY ? CL_WARN : CL_OK);
    gfx->fillCircle(x + w - 14, y + 14, 6, col);
  }
}

static void ui_draw_buttons_main(void)
{
  for (int8_t id = 0; id < UIB_MAIN_COUNT; id++) ui_draw_button(id, false);
}

static void ui_draw_status(void)
{
  uint8_t  st  = sd_get_status();
  uint16_t col = (st == SD_ST_ERR || st == SD_ST_NONE) ? CL_ERR : (st == SD_ST_BUSY ? CL_WARN : CL_OK);
  gfx->fillRect(0, UI_STATUS_Y, UI_SCR_W, 26, CL_BG);
  ui_text(20, UI_STATUS_Y + 5, 2, col, CL_BG, sd_get_message());
}

static void ui_draw_main(void)
{
  gfx->fillRect(0, UI_PANEL_Y, UI_SCR_W, UI_PANEL_H, CL_BG);
  ui_draw_status();
  ui_draw_buttons_main();
}

static void ui_draw_sd_panel(void)
{
  gfx->fillRect(0, UI_PANEL_Y, UI_SCR_W, UI_PANEL_H, CL_BG);
  ui_text(20, 268, 3, CL_TXT, CL_BG, "CARTE SD");
  uint8_t  st  = sd_get_status();
  uint16_t col = (st == SD_ST_ERR || st == SD_ST_NONE) ? CL_ERR : (st == SD_ST_BUSY ? CL_WARN : CL_OK);
  ui_text(20, 308, 2, col, CL_BG, sd_get_message());
  if (ui_sd_confirm)
  {
    ui_text(20, 336, 2, CL_WARN, CL_BG, "Remplacer memoires et reglages ?");
    ui_draw_button(UIB_SD_YES, false);
    ui_draw_button(UIB_SD_NO, false);
  }
  else
  {
    ui_text(20, 336, 2, CL_TXT_DIM, CL_BG, "Fichier : " SD_BACKUP_FILE);
    ui_draw_button(UIB_SD_SAVE, false);
    ui_draw_button(UIB_SD_LOAD, false);
    ui_draw_button(UIB_SD_CLOSE, false);
  }
}

//-----------------------------------------------------------------------------
//   Touch actions
//-----------------------------------------------------------------------------

// Hit test on the buttons of the current screen
static int8_t ui_hit(uint16_t x, uint16_t y)
{
  int8_t first, last;
  if (ui_mode == UIM_MAIN)      { first = 0;             last = UIB_MAIN_COUNT - 1; }
  else if (ui_sd_confirm)       { first = UIB_SD_YES;    last = UIB_SD_NO; }
  else                          { first = UIB_SD_SAVE;   last = UIB_SD_CLOSE; }
  for (int8_t id = first; id <= last; id++)
  {
    if ((int16_t)x >= ui_geo[id][0] && (int16_t)x < ui_geo[id][0] + ui_geo[id][2] &&
        (int16_t)y >= ui_geo[id][1] && (int16_t)y < ui_geo[id][1] + ui_geo[id][3]) return id;
  }
  return UIB_NONE;
}

// Encoder emulation. Tuning: ENC_TUNERESDIVIDE counts = one step, fast = 8 steps.
// Menu: ENC_MENURESDIVIDE counts = one menu item / one value step.
static void ui_enc_move(int8_t id)
{
  int32_t step;
  if (flag.config_menu) step = ENC_MENURESDIVIDE;
  else step = (id == UIB_ENC_SL || id == UIB_ENC_SR) ? ENC_TUNERESDIVIDE : 8 * ENC_TUNERESDIVIDE;
  if (id == UIB_ENC_FL || id == UIB_ENC_SL) step = -step;
  int32_t p = Enc.read() + step;
  if (p > 64) p = 64;                              // limit the backlog if the stepper is slower than the repeat
  if (p < -64) p = -64;
  Enc.write(p);
}

// Short tap on MENU. In the menu it is "Enter". Outside the menu the original single
// push button also recalibrated; that is now the RECAL button, so only the functions
// that need the button for the radio are kept here.
static void ui_menu_tap(void)
{
  if (flag.config_menu)
  {
    flag.short_push = true;
  }
  else if (controller_settings.trx[controller_settings.radioprofile].radio == MAX_RADIO)
  {
    pseudovfo_encoder_toggle();
  }
  else if (poll_rate[controller_settings.trx[controller_settings.radioprofile].radio] == 9999)
  {
    trx_poll();
  }
}

static void ui_press(int8_t id, uint32_t now)
{
  ui_press_ms    = now;
  ui_long_done   = false;
  ui_next_repeat = now + 400;
  ui_draw_button(id, true);

  switch (id)
  {
    case UIB_ENC_FL:
    case UIB_ENC_SL:
    case UIB_ENC_SR:
    case UIB_ENC_FR:
      ui_enc_move(id);
      break;

    case UIB_UP:                                   // same variables as the original push buttons
      up_toggle = true;
      up_button = true;
      break;

    case UIB_DOWN:
      dn_toggle = true;
      dn_button = true;
      break;

    case UIB_TUNE:
      #if PSWR_AUTOTUNE
      if (!flag.config_menu && !swr.tune)
      {
        swr.tune_request = true;
        SWRtune_timer = SWRTUNE_TIMEOUT;
      }
      #endif
      break;

    case UIB_RECAL:
      #if RECALIBRATE
      if (!flag.config_menu && !swr.tune) flag.stepper_recalibrate = true;
      #endif
      break;

    case UIB_ANT:
      #if ANT_CHG_2BANKS && !ANT1_CHANGEOVER
      ant1_changeover = (ant1_changeover == 0) ? 100000000 : 0;
      antenna_select(ant1_changeover);
      #if RS485STEPPER
      rs485_SelectAntenna(ant);
      #endif
      #endif
      break;

    default:                                       // MENU, SD and popup buttons act on release
      break;
  }
}

static void ui_hold(int8_t id, uint32_t now)
{
  switch (id)
  {
    case UIB_ENC_FL:
    case UIB_ENC_SL:
    case UIB_ENC_SR:
    case UIB_ENC_FR:
      if ((int32_t)(now - ui_next_repeat) >= 0)
      {
        ui_enc_move(id);
        ui_next_repeat = now + (flag.config_menu ? 250 : 60);
      }
      break;

    case UIB_MENU:                                 // long push opens the configuration menu
      if (!ui_long_done && !flag.config_menu && (now - ui_press_ms) >= ENACT_MAX)
      {
        flag.config_menu = true;
        ui_long_done = true;
      }
      break;

    default:
      break;
  }
}

static void ui_release(int8_t id, uint32_t now)
{
  uint32_t held = now - ui_press_ms;
  ui_pressed = UIB_NONE;

  switch (id)
  {
    case UIB_UP:
      up_button = false;
      up_toggle = false;
      break;

    case UIB_DOWN:
      dn_button = false;
      dn_toggle = false;
      break;

    case UIB_MENU:
      if (!ui_long_done && held >= ENACT_MIN) ui_menu_tap();
      break;

    case UIB_SD:
      ui_mode = UIM_SD;
      ui_sd_confirm = false;
      ui_draw_sd_panel();
      return;

    case UIB_SD_SAVE:
      sd_request_save();
      ui_draw_sd_panel();
      return;

    case UIB_SD_LOAD:
      ui_sd_confirm = true;
      ui_draw_sd_panel();
      return;

    case UIB_SD_YES:
      ui_sd_confirm = false;
      sd_request_restore();
      ui_draw_sd_panel();
      return;

    case UIB_SD_NO:
      ui_sd_confirm = false;
      ui_draw_sd_panel();
      return;

    case UIB_SD_CLOSE:
      ui_mode = UIM_MAIN;
      ui_draw_main();
      return;

    default:
      break;
  }
  if (id < UIB_MAIN_COUNT) ui_draw_button(id, false);
}

static void ui_touch_service(uint32_t now)
{
  uint16_t x = 0, y = 0;
  uint8_t  t = touch_poll(&x, &y);

  if (t != 0) ui_last_frame = now;

  if (t == 1)
  {
    ui_activity    = true;                         // wakes up the screensaver
    ui_last_action = now;
    if (ui_pressed == UIB_NONE)
    {
      int8_t id = ui_hit(x, y);
      if (id != UIB_NONE && ui_enabled(id))
      {
        ui_pressed = id;
        ui_press(id, now);
      }
    }
  }
  else if (t == 2 && ui_pressed != UIB_NONE)
  {
    ui_release(ui_pressed, now);
  }

  // Safety: the controller reports every ~10 ms while a finger is down. If the reports stop
  // (missed release, I2C problem) let go of the button so that UP/DOWN cannot stay pressed.
  if (ui_pressed != UIB_NONE && (now - ui_last_frame) > 200)
  {
    ui_release(ui_pressed, now);
  }

  if (ui_pressed != UIB_NONE) ui_hold(ui_pressed, now);
}

// Slow housekeeping (5 Hz): refresh labels/status when something changed
static void ui_refresh(uint32_t now)
{
  static uint16_t sig_main = 0xFFFF;
  static uint16_t sig_sd   = 0xFFFF;
  static uint16_t old_msg  = 0xFFFF;

  if (ui_pressed != UIB_NONE) return;              // do not redraw under the finger

  if (ui_mode == UIM_MAIN)
  {
    uint16_t sig = (flag.config_menu ? 1 : 0) | ((uint16_t)ant << 1) | ((uint16_t)sd_get_status() << 3) | (swr.tune ? 0x40 : 0);
    if (sig != sig_main)
    {
      sig_main = sig;
      ui_draw_buttons_main();
    }
    if (sd_get_msg_id() != old_msg)
    {
      old_msg = sd_get_msg_id();
      ui_draw_status();
    }
  }
  else
  {
    uint16_t sig = sd_get_status() | (uint16_t)(sd_get_msg_id() << 4);
    if (sig != sig_sd)
    {
      sig_sd = sig;
      ui_draw_sd_panel();
    }
    // Close the popup by itself so that the tuning buttons are never hidden for long
    if (sd_get_status() != SD_ST_BUSY && (now - ui_last_action) > (uint32_t)UI_SD_POPUP_TIMEOUT * 100UL)
    {
      ui_mode = UIM_MAIN;
      ui_sd_confirm = false;
      ui_draw_main();
      sig_main = 0xFFFF;
    }
  }
}

//-----------------------------------------------------------------------------
//   Public functions
//-----------------------------------------------------------------------------

// Bring up the RGB panel, draw the empty LCD and the buttons, then turn the backlight on.
// board_init() must have been called.
bool gfx_init(void)
{
  gfx_ok = false;
  gt911_detect();

  gfx_panel = new Arduino_ESP32RGBPanel(
      5 /* DE */, 3 /* VSYNC */, 46 /* HSYNC */, 7 /* PCLK */,
      1 /* R0 */, 2 /* R1 */, 42 /* R2 */, 41 /* R3 */, 40 /* R4 */,
      39 /* G0 */, 0 /* G1 */, 45 /* G2 */, 48 /* G3 */, 47 /* G4 */, 21 /* G5 */,
      14 /* B0 */, 38 /* B1 */, 18 /* B2 */, 17 /* B3 */, 10 /* B4 */,
      0 /* hsync_polarity */, 8 /* hsync_front_porch */, 4 /* hsync_pulse_width */, 8 /* hsync_back_porch */,
      0 /* vsync_polarity */, 8 /* vsync_front_porch */, 4 /* vsync_pulse_width */, 8 /* vsync_back_porch */,
      1 /* pclk_active_neg */, GFX_PCLK_HZ /* prefer_speed */, false /* useBigEndian */,
      0 /* de_idle_high */, 0 /* pclk_idle_high */, GFX_BOUNCE_PX /* bounce_buffer_size_px */);
  gfx = new Arduino_RGB_Display(UI_SCR_W, UI_SCR_H, gfx_panel, 0 /* rotation */, true /* auto_flush */);

  if (!gfx->begin())
  {
    // Most likely PSRAM is not enabled: Tools > PSRAM > "OPI PSRAM"
    return false;
  }
  gfx->setTextWrap(false);
  gfx->fillScreen(CL_BG);
  lcd_draw_frame();
  ui_mode = UIM_MAIN;
  ui_draw_main();
  lcd_render(80);                                  // empty LCD (all dots off)

  ch422g_set(EXIO_LCD_BL, true);                   // backlight on
  gfx_ok = true;
  return true;
}

// Called from virt_LCD_to_real_LCD(): draws changed LCD characters (a few per call, like the
// original code, so that the stepper timing is not disturbed) and serves the touch screen.
void gfx_service(void)
{
  static uint32_t last_touch = 0;
  static uint32_t last_ui    = 0;

  if (!gfx_ok) return;
  uint32_t now = millis();

  int16_t budget = 15 / ((step_rate > 0) ? step_rate : 1);
  if (budget < 1) budget = 1;
  lcd_render((uint8_t)budget);

  if ((now - last_touch) >= 20)
  {
    last_touch = now;
    ui_touch_service(now);
  }
  if ((now - last_ui) >= 200)
  {
    last_ui = now;
    ui_refresh(now);
  }
}

// Draw everything that changed right now (start-up messages)
void gfx_flush_now(void)
{
  if (!gfx_ok) return;
  lcd_render(80);
}

#else   // !GFX_ENABLED : no display driver, the virtual LCD is simply not shown

bool gfx_init(void)        { return false; }
void gfx_service(void)     { }
void gfx_flush_now(void)   { }

#endif  // GFX_ENABLED

#endif  // ESP32
