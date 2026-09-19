//*********************************************************************************
//**
//**  ML_SD.ino  -  SD card backup of memories and settings
//**
//**  What is saved (text file SD_BACKUP_FILE, human readable, with checksum):
//**    - the controller settings (controller_settings, as hex)
//**    - all frequency/position presets (preset[])
//**    - the current frequency/position state (running[], delta_Pos[], stepper_track[], ant)
//**
//**  Save     : a snapshot is taken by the main task, then a background task writes the
//**             file (temp file, previous backup kept as .bak) so that the stepper timing
//**             is not disturbed by the slow SD card.
//**  Restore  : the file is read and checked by the background task, then written into the
//**             EEPROM by the main task and the controller restarts.  Only settings and presets
//**             are taken from the card, the live stepper position is kept
//**             (except at boot with a blank EEPROM, where the saved position is used too).
//**  Autosave : when presets/settings changed and stayed unchanged for SD_AUTOSAVE_DELAY,
//**             and the stepper is idle.
//**
//**  The SD chip select is EXIO4 of the CH422G I/O expander (not a GPIO).  It is held low after
//**  start-up, the SD task never touches the I2C bus (Wire is not thread safe).
//**
//**  Only basic types are used in function signatures (Arduino IDE prototype generator).
//**
//*********************************************************************************

#if defined(ESP32) && SD_ENABLED

static SPIClass sd_spi(HSPI);

typedef struct
{
  uint8_t   coldstart;
  uint8_t   ant;
  settings  cs;
  var_track run[3];
  int32_t   delta[3];
  int32_t   track[3];
  var_track pre[MAX_PRESETS];
} sd_snapshot_t;

static sd_snapshot_t sd_snap;

static volatile uint8_t  sd_status       = SD_ST_NONE;
static volatile uint16_t sd_msg_id       = 0;
static char              sd_msg[34]      = "SD: pas de carte";
static bool              sd_ready        = false;      // card mounted
static volatile uint8_t  sd_req          = 0;          // 0 none, 1 save, 2 restore
static volatile bool     sd_restore_ready = false;     // restore data read and valid, waiting for the main task
static volatile uint32_t sd_saved_hash   = 0;
static uint32_t          sd_snap_hash    = 0;
static bool              sd_need_first   = false;      // no backup file yet: create one when idle
static bool              sd_task_started = false;
static uint32_t          sd_restart_at   = 0;
static volatile uint32_t sd_fail_ms      = 0;          // time of the last failed save (0 = none), autosave waits after a failure
static bool              sd_usb_report   = false;

#define SD_FNV_INIT  2166136261UL
#define SD_LINE_MAX  (sizeof(settings) * 2 + 24)

static uint32_t sd_fnv(uint32_t h, const uint8_t *p, size_t n)
{
  while (n--)
  {
    h ^= *p++;
    h *= 16777619UL;
  }
  return h;
}

static void sd_set_msg(uint8_t st, const char *m)
{
  strncpy(sd_msg, m, sizeof(sd_msg) - 1);
  sd_msg[sizeof(sd_msg) - 1] = 0;
  sd_status = st;
  sd_msg_id++;
}

// Hash of what is worth a backup (used to notice changes)
static uint32_t sd_data_hash(void)
{
  uint32_t h = SD_FNV_INIT;
  h = sd_fnv(h, (const uint8_t *)&controller_settings, sizeof(controller_settings));
  h = sd_fnv(h, (const uint8_t *)preset, sizeof(preset));
  return h;
}

static void sd_take_snapshot(void)
{
  sd_snap.coldstart = COLDSTART_REF;
  sd_snap.ant       = ant;
  memcpy(&sd_snap.cs, &controller_settings, sizeof(settings));
  memcpy(sd_snap.run,   running,       sizeof(sd_snap.run));
  memcpy(sd_snap.delta, delta_Pos,     sizeof(sd_snap.delta));
  memcpy(sd_snap.track, stepper_track, sizeof(sd_snap.track));
  memcpy(sd_snap.pre,   preset,        sizeof(sd_snap.pre));
  sd_snap_hash = sd_data_hash();
}

//-----------------------------------------------------------------------------
//   Backup file: writer
//-----------------------------------------------------------------------------
static bool sd_write_backup(void)
{
  char     line[SD_LINE_MAX + 8];
  uint32_t h  = SD_FNV_INIT;
  bool     ok = true;
  int      n;
  uint16_t i;
  uint8_t  a;

  if (!SD.exists(SD_BACKUP_DIR)) SD.mkdir(SD_BACKUP_DIR);
  File f = SD.open(SD_BACKUP_TMP, FILE_WRITE);
  if (!f) return false;

#define SD_W(...)  do { n = snprintf(line, sizeof(line), __VA_ARGS__); \
                        if (f.write((const uint8_t *)line, n) != (size_t)n) ok = false; \
                        h = sd_fnv(h, (const uint8_t *)line, n); } while (0)

  SD_W("# ML_v500 backup - restore with the SD button of the touch screen or $sdload\n");
  SD_W("FORMAT 1\n");
  SD_W("COLDSTART %02X\n", (unsigned)sd_snap.coldstart);
  SD_W("MAX_PRESETS %u\n", (unsigned)MAX_PRESETS);
  SD_W("SETTINGS_SIZE %u\n", (unsigned)sizeof(settings));
  {
    const uint8_t *p = (const uint8_t *)&sd_snap.cs;
    n = snprintf(line, sizeof(line), "SETTINGS ");
    for (i = 0; i < sizeof(settings); i++) n += snprintf(line + n, sizeof(line) - n, "%02X", p[i]);
    n += snprintf(line + n, sizeof(line) - n, "\n");
    if (f.write((const uint8_t *)line, n) != (size_t)n) ok = false;
    h = sd_fnv(h, (const uint8_t *)line, n);
  }
  SD_W("ANT %u\n", (unsigned)sd_snap.ant);
  for (a = 0; a < 3; a++)
  {
    SD_W("RUNNING %u %ld %ld\n", (unsigned)a, (long)sd_snap.run[a].Frq, (long)sd_snap.run[a].Pos);
    SD_W("DELTA %u %ld\n", (unsigned)a, (long)sd_snap.delta[a]);
    SD_W("TRACK %u %ld\n", (unsigned)a, (long)sd_snap.track[a]);
  }
  for (i = 0; i < MAX_PRESETS; i++)
  {
    if (sd_snap.pre[i].Frq != 0 || sd_snap.pre[i].Pos != 1000000)      // unused presets are not listed
      SD_W("PRESET %u %ld %ld\n", (unsigned)i, (long)sd_snap.pre[i].Frq, (long)sd_snap.pre[i].Pos);
  }
  n = snprintf(line, sizeof(line), "END %08lX\n", (unsigned long)h);
  if (f.write((const uint8_t *)line, n) != (size_t)n) ok = false;
#undef SD_W

  f.flush();
  f.close();
  if (!ok)
  {
    SD.remove(SD_BACKUP_TMP);
    return false;
  }

  // Keep the previous backup as .bak, then put the new one in place
  if (SD.exists(SD_BACKUP_FILE))
  {
    if (SD.exists(SD_BACKUP_PREV)) SD.remove(SD_BACKUP_PREV);
    SD.rename(SD_BACKUP_FILE, SD_BACKUP_PREV);
  }
  if (!SD.rename(SD_BACKUP_TMP, SD_BACKUP_FILE)) return false;
  return true;
}

//-----------------------------------------------------------------------------
//   Backup file: reader.  Fills sd_snap.  Returns 0 = OK, 1 = no file, 2 = invalid
//-----------------------------------------------------------------------------
static int16_t sd_hex_byte(const char *s)      // returns 0..255, or -1 if not hexadecimal
{
  int16_t v = 0;
  for (uint8_t k = 0; k < 2; k++)
  {
    char c = s[k];
    if      (c >= '0' && c <= '9') v = (int16_t)((v << 4) | (c - '0'));
    else if (c >= 'A' && c <= 'F') v = (int16_t)((v << 4) | (c - 'A' + 10));
    else if (c >= 'a' && c <= 'f') v = (int16_t)((v << 4) | (c - 'a' + 10));
    else return -1;
  }
  return v;
}

static int8_t sd_read_file(const char *path)
{
  char     line[SD_LINE_MAX + 8];
  uint32_t h = SD_FNV_INIT;
  uint32_t file_hash = 0;
  bool     got_end = false, got_format = false, got_cold = false, got_settings = false, size_ok = false;
  uint16_t i;

  File f = SD.open(path, FILE_READ);
  if (!f) return 1;

  memset(&sd_snap, 0, sizeof(sd_snap));
  for (i = 0; i < MAX_PRESETS; i++) { sd_snap.pre[i].Frq = 0; sd_snap.pre[i].Pos = 1000000; }
  for (i = 0; i < 3; i++)           { sd_snap.run[i].Frq = 0; sd_snap.run[i].Pos = 1000000; sd_snap.track[i] = 1000000; }

  bool ok = true;
  while (ok && f.available())
  {
    int n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    if (n >= (int)sizeof(line) - 1) { ok = false; break; }            // line too long
    if (n > 0 && line[n - 1] == '\r') n--;
    line[n] = 0;

    if (!strncmp(line, "END ", 4))
    {
      file_hash = (uint32_t)strtoul(line + 4, NULL, 16);
      got_end = true;
      break;
    }
    h = sd_fnv(h, (const uint8_t *)line, n);
    h = sd_fnv(h, (const uint8_t *)"\n", 1);

    long a = 0, b = 0, c = 0;
    unsigned u = 0;
    if (line[0] == '#' || line[0] == 0) continue;
    else if (sscanf(line, "FORMAT %u", &u) == 1)              { if (u == 1) got_format = true; else ok = false; }
    else if (sscanf(line, "COLDSTART %x", &u) == 1)           { if (u == COLDSTART_REF) { got_cold = true; sd_snap.coldstart = (uint8_t)u; } else ok = false; }
    else if (sscanf(line, "SETTINGS_SIZE %u", &u) == 1)       { if (u == sizeof(settings)) size_ok = true; else ok = false; }
    else if (!strncmp(line, "SETTINGS ", 9))
    {
      if (!size_ok || strlen(line + 9) != sizeof(settings) * 2) { ok = false; }
      else
      {
        uint8_t *p = (uint8_t *)&sd_snap.cs;
        for (i = 0; i < sizeof(settings) && ok; i++)
        {
          int16_t v = sd_hex_byte(line + 9 + 2 * i);
          if (v < 0) ok = false; else p[i] = (uint8_t)v;
        }
        if (ok) got_settings = true;
      }
    }
    else if (sscanf(line, "ANT %u", &u) == 1)                 { if (u < 3) sd_snap.ant = (uint8_t)u; else ok = false; }
    else if (sscanf(line, "RUNNING %ld %ld %ld", &a, &b, &c) == 3) { if (a >= 0 && a < 3) { sd_snap.run[a].Frq = b; sd_snap.run[a].Pos = c; } else ok = false; }
    else if (sscanf(line, "DELTA %ld %ld", &a, &b) == 2)      { if (a >= 0 && a < 3) sd_snap.delta[a] = b; else ok = false; }
    else if (sscanf(line, "TRACK %ld %ld", &a, &b) == 2)      { if (a >= 0 && a < 3) sd_snap.track[a] = b; else ok = false; }
    else if (sscanf(line, "PRESET %ld %ld %ld", &a, &b, &c) == 3) { if (a >= 0 && a < MAX_PRESETS) { sd_snap.pre[a].Frq = b; sd_snap.pre[a].Pos = c; } else ok = false; }
    // MAX_PRESETS and unknown lines are ignored
  }
  f.close();
  if (ok && got_end && got_format && got_cold && got_settings && h == file_hash) return 0;
  return 2;
}

// Try the newest backup, then the previous one
static int8_t sd_read_backup(void)
{
  int8_t r = sd_read_file(SD_BACKUP_FILE);
  if (r == 0) return 0;
  int8_t r2 = sd_read_file(SD_BACKUP_PREV);
  if (r2 == 0) return 0;
  return (r == 1 && r2 == 1) ? 1 : 2;
}

//-----------------------------------------------------------------------------
//   Write sd_snap into the EEPROM (marker last, so that an interrupted write is retried at boot)
//-----------------------------------------------------------------------------
static void sd_apply_to_eeprom(bool with_position)
{
  EEPROM_writeAnything(1, sd_snap.cs);
  EEPROM_writeAnything(EEPROM_PRESET_ADDR, sd_snap.pre);
  if (with_position)
  {
    EEPROM_writeAnything(100, sd_snap.run);
    EEPROM_writeAnything(124, sd_snap.delta);
    EEPROM_writeAnything(136, sd_snap.track);
  }
  EEPROM.write(0, COLDSTART_REF);
  EEPROM.commit();
}

// A brand new board / erased flash: every byte of the settings and position areas is 0xFF
static bool sd_eeprom_blank(void)
{
  uint16_t a;
  for (a = 0; a <= 34; a++)    if (EEPROM.read(a) != 0xFF) return false;
  for (a = 100; a < 148; a++)  if (EEPROM.read(a) != 0xFF) return false;
  return true;
}

//-----------------------------------------------------------------------------
//   Card mounting
//-----------------------------------------------------------------------------
// first = true : called from setup() in the main task, may use the I/O expander
static bool sd_mount(bool first)
{
  if (first)
  {
    ch422g_set(EXIO_SD_CS, true);                              // deselect while sending the wake-up clocks
    sd_spi.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, -1);
    sd_spi.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
    for (uint8_t i = 0; i < 10; i++) sd_spi.transfer(0xFF);    // >= 74 clocks
    sd_spi.endTransaction();
    ch422g_set(EXIO_SD_CS, false);                             // select, and keep it selected
    delay(5);
  }
  else
  {
    SD.end();
  }
  if (!SD.begin(SD_DUMMY_CS_PIN, sd_spi, SD_SPI_HZ)) return false;
  if (SD.cardType() == CARD_NONE) return false;
  if (!SD.exists(SD_BACKUP_DIR)) SD.mkdir(SD_BACKUP_DIR);
  return true;
}

//-----------------------------------------------------------------------------
//   Background task
//-----------------------------------------------------------------------------
static void sd_task(void *arg)
{
  for (;;)
  {
    uint8_t r = sd_req;
    if (r != 0)
    {
      if (!sd_ready) sd_ready = sd_mount(false);               // card inserted after boot?
      if (!sd_ready)
      {
        if (r == 1) sd_fail_ms = millis() | 1;
        sd_set_msg(SD_ST_NONE, "SD: pas de carte");
      }
      else if (r == 1)
      {
        bool ok = sd_write_backup();
        if (!ok)                                               // card may have been changed: remount once
        {
          sd_ready = sd_mount(false);
          ok = sd_ready && sd_write_backup();
        }
        if (ok)
        {
          sd_saved_hash = sd_snap_hash;
          sd_need_first = false;
          sd_fail_ms    = 0;
          sd_set_msg(SD_ST_OK, "SD: sauvegarde OK");
        }
        else
        {
          sd_fail_ms = millis() | 1;                           // never 0
          sd_set_msg(SD_ST_ERR, "SD: erreur ecriture");
        }
      }
      else if (r == 2)
      {
        int8_t rr = sd_read_backup();
        if (rr == 0)      sd_restore_ready = true;             // main task writes the EEPROM
        else if (rr == 1) sd_set_msg(SD_ST_ERR, "SD: aucune sauvegarde");
        else              sd_set_msg(SD_ST_ERR, "SD: sauvegarde invalide");
      }
      sd_req = 0;
    }
    vTaskDelay(pdMS_TO_TICKS(25));
  }
}

//-----------------------------------------------------------------------------
//   Public functions
//-----------------------------------------------------------------------------
uint8_t     sd_get_status(void)   { return sd_status; }
const char *sd_get_message(void)  { return sd_msg; }
uint16_t    sd_get_msg_id(void)   { return sd_msg_id; }
bool        sd_present(void)      { return sd_ready; }

// Call from setup(), after board_init()
void sd_init(void)
{
  sd_ready = sd_mount(true);
  if (sd_ready)
  {
    sd_need_first = !SD.exists(SD_BACKUP_FILE);
    sd_set_msg(SD_ST_READY, "SD: prete");
  }
  else sd_set_msg(SD_ST_NONE, "SD: pas de carte");

  if (!sd_task_started)
  {
    xTaskCreatePinnedToCore(sd_task, "ml_sd", 8192, NULL, 1, NULL, 0);
    sd_task_started = true;
  }
}

// Call from setup() before the EEPROM is used.  Returns true if the EEPROM was filled from the card.
bool sd_boot_restore_if_blank(void)
{
#if SD_AUTORESTORE_BLANK
  if (!sd_ready) return false;
  if (!sd_eeprom_blank()) return false;
  if (sd_read_backup() != 0) return false;
  sd_apply_to_eeprom(true);
  sd_set_msg(SD_ST_OK, "SD: restauree (boot)");
  return true;
#else
  return false;
#endif
}

bool sd_request_save(void)
{
  if (!sd_task_started) return false;
  if (sd_req != 0 || sd_restore_ready || sd_status == SD_ST_BUSY) return false;
  sd_take_snapshot();
  sd_set_msg(SD_ST_BUSY, "SD: sauvegarde...");
  sd_req = 1;
  return true;
}

bool sd_request_restore(void)
{
  if (!sd_task_started) return false;
  if (sd_req != 0 || sd_restore_ready || sd_status == SD_ST_BUSY) return false;
  sd_set_msg(SD_ST_BUSY, "SD: lecture...");
  sd_req = 2;
  return true;
}

// Ask sd_service() to print the result of the next operation on the USB serial port
void sd_report_to_usb(void)
{
  sd_usb_report = true;
}

// Call every 100 ms from the main loop
void sd_service(void)
{
  static uint8_t  divider     = 0;
  static bool     have_base   = false;
  static uint32_t seen_hash   = 0;
  static uint16_t stable      = 0;                // in units of 500 ms
  bool idle = !flag.stepper_active && !flag.frq_store && !flag.config_menu && !swr.tune;

  if (sd_restart_at != 0)
  {
    if ((int32_t)(millis() - sd_restart_at) >= 0) SOFT_RESET();
    return;
  }

  // Restore data read from the card, write it into the EEPROM when the stepper is quiet
  if (sd_restore_ready && idle)
  {
    sd_restore_ready = false;
    sd_apply_to_eeprom(false);
    sd_set_msg(SD_ST_OK, "SD: restauree, reboot");
    sd_restart_at = millis() + 1500;
    if (sd_usb_report) { Serial.println(sd_msg); sd_usb_report = false; }
    return;
  }

  if (sd_usb_report && sd_status != SD_ST_BUSY && !sd_restore_ready)
  {
    Serial.println(sd_msg);
    sd_usb_report = false;
  }

#if SD_AUTOSAVE
  if (!sd_ready || ++divider < 5) return;
  divider = 0;

  uint32_t h = sd_data_hash();
  if (!have_base)
  {
    have_base = true;
    seen_hash = h;
    sd_saved_hash = h;          // what is in the EEPROM at boot is the reference
    stable = 0;
    return;
  }
  if (h != seen_hash) { seen_hash = h; stable = 0; }
  else if (stable < 60000) stable++;

  bool changed = (h != sd_saved_hash) || sd_need_first;
  bool wait_after_failure = (sd_fail_ms != 0) && ((millis() - sd_fail_ms) < 30000UL);   // do not hammer a failing card
  if (changed && idle && stable >= (SD_AUTOSAVE_DELAY / 5) && sd_status != SD_ST_BUSY &&
      sd_req == 0 && !wait_after_failure)
  {
    sd_request_save();
  }
#endif
}

#else   // no SD support

uint8_t     sd_get_status(void)   { return SD_ST_NONE; }
const char *sd_get_message(void)  { return "SD: desactivee"; }
uint16_t    sd_get_msg_id(void)   { return 0; }
bool        sd_present(void)      { return false; }
void        sd_init(void)         { }
bool        sd_boot_restore_if_blank(void) { return false; }
bool        sd_request_save(void)    { return false; }
bool        sd_request_restore(void) { return false; }
void        sd_report_to_usb(void)   { }
void        sd_service(void)         { }

#endif
