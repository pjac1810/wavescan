/* SPDX-License-Identifier: GPL-3.0-only */
/* WAVESCAN: an 8-voice wavetable synthesizer for the M-VAVE FM-1, after the PPG Wave 2.2 / 2.3.
 *
 * The panel (README.md has the full map):
 *   HOME        PERFORM: TABLE, WAVES A, CUTOFF, EMPHASIS
 *   SCL         OSCILLATORS (again: MIX, STEP/SMOOTH, KEY>WAVES, TABLE)
 *   ENV         ENV1 filter, again ENV2 loudness, again ENV3 (waves or pitch)
 *   EDIT        FILTER                 LFO   LFO (again: its destinations)
 *   FX          DELAY and stereo spread                   GLO   VOICE: POLY/MONO, GLIDE, DRIVE, VOLUME
 *   KNOB1..4    the page's four parameters            SELECT  the wavetable      ALGORITHM  WAVES A
 *   PRESETS     the sound: F1..F8, U1..U8             SAVE    store into a U slot
 *   keys        play (8 voices)  OCT- OCT+ the octave         PLAY  HOLD (the notes keep sounding)
 *   USB-MIDI / TRS   notes, pitch bend, CC 20..59 the parameters */
#include "fm1.h"
#include "presets.h"
#include "synth.h"
#include "tables.h"
#include "wavetables.h"

/* ---------------------------------------------------------------- colours: a dark panel, an amber LCD */

#define C_BG RGB(10, 14, 26)
#define C_HEAD RGB(20, 28, 48)
#define C_TEXT RGB(230, 232, 240)
#define C_DIM RGB(110, 118, 140)
#define C_FAINT RGB(36, 44, 66)
#define C_LCD RGB(28, 22, 8)
#define C_LCD_T RGB(255, 196, 70)
#define C_LCD_D RGB(150, 110, 40)
#define C_WAVE RGB(255, 150, 40)
#define C_WAVE_B RGB(80, 200, 255)

static int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static char *put(char *d, const char *s) {
  while (*s) *d++ = *s++;
  *d = 0;
  return d;
}
static char *put_int(char *d, int32_t v) {
  char b[12];
  format_int(b, v);
  return put(d, b);
}
static int center_text(int cx, int y, const char *s, uint16_t c, int font) {
  return text(cx - text_width(s, font) / 2, y, s, c, font);
}
static uint16_t mix565(uint16_t a, uint16_t b, int t) {
  int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31, br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
  return (uint16_t)(((ar + (((br - ar) * t) >> 8)) << 11) | ((ag + (((bg - ag) * t) >> 8)) << 5) | (ab + (((bb - ab) * t) >> 8)));
}
/* 2^(m / 1000) x 1000, for the displays */
static int32_t exp2_milli(int32_t m) {
  int32_t oct = m / 1000, r = m - oct * 1000;
  int32_t f = 1000 + (r * (656 + (344 * r) / 1000)) / 1000;
  return oct >= 0 ? f << oct : f >> -oct;
}

/* ---------------------------------------------------------------- the parameters */

enum { K_NUM, K_BIP, K_TIME, K_LIST, K_HZ, K_RATE, K_SEMI, K_CENT, K_PCT, K_TABLE, K_DTIME };
typedef struct {
  const char *label;
  uint8_t kind;
  int16_t lo, hi;
  const char *const *names;
} pdef_t;

static const char *const N_INTERP[] = {"STEP", "SMOOTH"};
static const char *const N_E3[] = {"WAVES", "PITCH", "WAVE A"};
static const char *const N_LFO[] = {"TRI", "SAW DN", "SAW UP", "SQUARE", "RANDOM"};
static const char *const N_MODE[] = {"POLY", "MONO"};

static const pdef_t PD[P_COUNT] = {
    [P_TABLE] = {"TABLE", K_TABLE, 0, WT_TABLES - 1, 0}, [P_WAVA] = {"WAVES A", K_NUM, 0, 63, 0},
    [P_WAVB] = {"WAVES B", K_NUM, 0, 63, 0},             [P_SEMI] = {"B SEMI", K_SEMI, -24, 24, 0},
    [P_DETUNE] = {"DETUNE", K_CENT, -50, 50, 0},         [P_MIX] = {"A / B", K_PCT, 0, 127, 0},
    [P_INTERP] = {"WAVES", K_LIST, 0, 1, N_INTERP},      [P_KEYWAVE] = {"KEY>WAV", K_BIP, -63, 63, 0},
    [P_E1A] = {"ATTACK", K_TIME, 0, 127, 0},             [P_E1D] = {"DECAY", K_TIME, 0, 127, 0},
    [P_E1S] = {"SUSTAIN", K_NUM, 0, 127, 0},             [P_E1R] = {"RELEASE", K_TIME, 0, 127, 0},
    [P_E2A] = {"ATTACK", K_TIME, 0, 127, 0},             [P_E2D] = {"DECAY", K_TIME, 0, 127, 0},
    [P_E2S] = {"SUSTAIN", K_NUM, 0, 127, 0},             [P_E2R] = {"RELEASE", K_TIME, 0, 127, 0},
    [P_E3A] = {"ATTACK", K_TIME, 0, 127, 0},             [P_E3D] = {"DECAY", K_TIME, 0, 127, 0},
    [P_E3AMT] = {"AMOUNT", K_BIP, -63, 63, 0},           [P_E3DST] = {"TO", K_LIST, 0, 2, N_E3},
    [P_CUT] = {"CUTOFF", K_HZ, 0, 127, 0},               [P_RES] = {"EMPHAS", K_NUM, 0, 127, 0},
    [P_E1VCF] = {"ENV1", K_BIP, -127, 127, 0},           [P_KEYVCF] = {"KEYTRK", K_PCT, 0, 127, 0},
    [P_LSHAPE] = {"SHAPE", K_LIST, 0, LFO_SHAPES - 1, N_LFO}, [P_LRATE] = {"RATE", K_RATE, 0, 127, 0},
    [P_LDELAY] = {"DELAY", K_TIME, 0, 127, 0},           [P_LWAVE] = {">WAVES", K_BIP, -63, 63, 0},
    [P_LPITCH] = {">PITCH", K_BIP, -63, 63, 0},          [P_LVCF] = {">FILTER", K_BIP, -127, 127, 0},
    [P_LAMP] = {">LOUD", K_NUM, 0, 127, 0},              [P_MODE] = {"VOICES", K_LIST, 0, 1, N_MODE},
    [P_GLIDE] = {"GLIDE", K_TIME, 0, 127, 0},            [P_DRIVE] = {"DRIVE", K_NUM, 0, 127, 0},
    [P_VOL] = {"VOLUME", K_NUM, 0, 127, 0},              [P_DMIX] = {"DLY MIX", K_NUM, 0, 127, 0},
    [P_DTIME] = {"TIME", K_DTIME, 0, 127, 0},            [P_DFB] = {"FEEDBK", K_PCT, 0, 127, 0},
    [P_SPREAD] = {"SPREAD", K_NUM, 0, 127, 0},
};

static void value_text(char *d, int p) {
  int32_t v = PAR[p];
  const pdef_t *pd = &PD[p];
  switch (pd->kind) {
    case K_LIST: put(d, pd->names[clampi(v, pd->lo, pd->hi)]); return;
    case K_BIP:
    case K_SEMI: put_int(put(d, v > 0 ? "+" : ""), v); return;
    case K_CENT: put(put_int(put(d, v > 0 ? "+" : ""), v), "c"); return;
    case K_PCT: put(put_int(d, (v * 100) / 127), "%"); return;
    case K_TABLE: {
      char *e = put_int(put(d, v + 1 < 10 ? "0" : ""), v + 1);
      (void)e;
      return;
    }
    case K_TIME: {
      int32_t ms = (p == P_GLIDE || p == P_LDELAY) && v == 0 ? 0 : ENV_MS[clampi(v, 0, 127)];
      if (p == P_GLIDE) ms /= 4;
      if (v == 0 && p == P_GLIDE) put(d, "OFF");
      else if (ms < 1000) put(put_int(d, ms), "ms");
      else if (ms < 10000) put(put_int(put(put_int(d, ms / 1000), "."), (ms % 1000) / 100), "s");
      else put(put_int(d, ms / 1000), "s");
      return;
    }
    case K_HZ: { /* the cutoff, 20 Hz .. 16 kHz */
      int32_t hz = (20 * exp2_milli((v * 8 * 9644) / 1023)) / 1000;
      if (hz < 1000) put(put_int(d, hz), "Hz");
      else put(put_int(put(put_int(d, hz / 1000), "."), (hz % 1000) / 100), "k");
      return;
    }
    case K_RATE: { /* 0.03 .. 40 Hz */
      int32_t mhz = (30 * exp2_milli((v * 10380) / 127)) / 1000;
      if (mhz < 1000) put(put_int(put(d, mhz < 100 ? "0.0" : "0."), mhz < 100 ? mhz / 10 : mhz / 10), "Hz");
      else if (mhz < 10000) put(put_int(put(put_int(d, mhz / 1000), "."), (mhz % 1000) / 100), "Hz");
      else put(put_int(d, mhz / 1000), "Hz");
      return;
    }
    case K_DTIME: put(put_int(d, ((440 + v * 82) * 10) / 221), "ms"); return;
    default: put_int(d, v); return;
  }
}

/* ---------------------------------------------------------------- the pages */

typedef struct {
  const char *title;
  int8_t p[4];
} page_t;

enum { PG_HOME, PG_OSC, PG_OSC2, PG_E1, PG_E2, PG_E3, PG_FILT, PG_LFO, PG_LFO2, PG_FX, PG_VOICE, PG_COUNT };
static const page_t PAGES[PG_COUNT] = {
    [PG_HOME] = {"PERFORM", {P_TABLE, P_WAVA, P_CUT, P_RES}},
    [PG_OSC] = {"OSC A + B", {P_WAVA, P_WAVB, P_SEMI, P_DETUNE}},
    [PG_OSC2] = {"OSC MIX", {P_MIX, P_INTERP, P_KEYWAVE, P_TABLE}},
    [PG_E1] = {"ENV1 FILTER", {P_E1A, P_E1D, P_E1S, P_E1R}},
    [PG_E2] = {"ENV2 LOUDNESS", {P_E2A, P_E2D, P_E2S, P_E2R}},
    [PG_E3] = {"ENV3 WAVES", {P_E3A, P_E3D, P_E3AMT, P_E3DST}},
    [PG_FILT] = {"FILTER 24dB", {P_CUT, P_RES, P_E1VCF, P_KEYVCF}},
    [PG_LFO] = {"LFO", {P_LSHAPE, P_LRATE, P_LDELAY, P_LWAVE}},
    [PG_LFO2] = {"LFO MOD", {P_LPITCH, P_LVCF, P_LAMP, -1}},
    [PG_FX] = {"DELAY", {P_DMIX, P_DTIME, P_DFB, P_SPREAD}},
    [PG_VOICE] = {"VOICE", {P_MODE, P_GLIDE, P_DRIVE, P_VOL}},
};
static const struct {
  uint8_t btn, first, count;
} PAGE_BTN[] = {{BTN_HOME, PG_HOME, 1}, {BTN_SEL, PG_OSC, 2}, {BTN_ENV, PG_E1, 3}, {BTN_EDIT, PG_FILT, 1},
                {BTN_LFO, PG_LFO, 2},   {BTN_FX, PG_FX, 1},   {BTN_GLO, PG_VOICE, 1}};
#define PAGE_BTNS (int)(sizeof PAGE_BTN / sizeof PAGE_BTN[0])

/* ---------------------------------------------------------------- flash */

#define STORE_MAGIC 0x57534331u /* "WSC1" */
static struct {
  uint32_t magic;
  uint8_t cur;
  int8_t oct;
  uint8_t pad[2];
  preset_t user[USER_COUNT];
} store;

/* ---------------------------------------------------------------- state */

static uint8_t page = PG_HOME, modified, saving, save_slot, hold;
static char cur_name[NAME_LEN + 1];
static uint8_t key_note[KEY_COUNT];
static uint8_t held_midi[128], hold_list[128];

static int8_t shown_page = -1;
static int32_t shown_col[4];
static uint32_t sh_head = ~0u, sh_wave = ~0u, sh_pos = ~0u, sh_foot = ~0u, led_k = ~0u, led_b = ~0u;
static uint32_t pos_t;

/* ---------------------------------------------------------------- drawing */

#define WAVE_Y 24
#define WAVE_H 92
#define POS_Y 118
#define LCD_Y 128
#define LCD_H 68
#define FOOT_Y 200

static void draw_head(void) {
  rect(0, 0, SCREEN_W, 22, C_HEAD);
  text(6, 3, page == PG_HOME ? "WAVESCAN" : PAGES[page].title, page == PG_HOME ? C_WAVE : C_LCD_T, FONT_SMALL);
  char b[24], *d = put(b, store.cur < FACTORY_COUNT ? "F" : "U");
  d = put_int(d, (store.cur % 8) + 1);
  d = put(put(d, " "), cur_name);
  if (modified) put(d, "*");
  int w = text_width(b, FONT_SMALL);
  if (w > 112) b[2] = modified ? '*' : 0, b[3] = 0, w = text_width(b, FONT_SMALL);
  text(SCREEN_W - 6 - w, 3, b, C_TEXT, FONT_SMALL);
}

/* the table as a cascade of waves, back to front; WAVES A and B stand out */
static void draw_waves(void) {
  rect(0, WAVE_Y, SCREEN_W, WAVE_H + 2, C_BG);
  int t = clampi(PAR[P_TABLE], 0, WT_TABLES - 1), wa = clampi(PAR[P_WAVA], 0, 63), wb = clampi(PAR[P_WAVB], 0, 63);
  for (int k = 15; k >= 0; k--) {
    int w = k * 4;
    int x0 = 6 + k * 7, y0 = WAVE_Y + WAVE_H - 16 - k * 4;
    int hi = (wa >= w && wa < w + 4) ? 1 : (wb >= w && wb < w + 4) ? 2 : 0;
    const int8_t *s = WT0[t][hi == 1 ? wa : hi == 2 ? wb : w];
    uint16_t c = hi == 1 ? C_WAVE : hi == 2 ? C_WAVE_B : mix565(C_FAINT, C_DIM, 255 - k * 12);
    int px = 0, py = 0;
    for (int j = 0; j < 128; j += 2) {
      int x = x0 + (j * 120) / 128, y = y0 - (s[j] * 14) / 127;
      if (j) line(px, py, x, y, c);
      px = x, py = y;
    }
  }
}

/* where the waves are now: a ruler 0..63, A and B set, the sounding position moving */
static void draw_pos(int live) {
  rect(0, POS_Y, SCREEN_W, 8, C_BG);
  rect(8, POS_Y + 3, 224, 1, C_FAINT);
  for (int i = 0; i <= 64; i += 8) rect(8 + (i * 224) / 64, POS_Y + 2, 1, 3, C_DIM);
  int xa = 8 + (PAR[P_WAVA] * 224) / 64, xb = 8 + (PAR[P_WAVB] * 224) / 64;
  rect(xb - 1, POS_Y, 3, 7, C_WAVE_B);
  rect(xa - 1, POS_Y, 3, 7, C_WAVE);
  if (live >= 0) rect(8 + (live * 224) / (64 * 256) - 1, POS_Y + 1, 3, 5, C_TEXT);
}

static void draw_col(int c) {
  int x0 = c * 60, cx = x0 + 30;
  rect(x0 + 2, LCD_Y + 2, 56, LCD_H - 4, C_LCD);
  int p = PAGES[page].p[c];
  if (p < 0) return;
  const pdef_t *pd = &PD[p];
  char b[16];
  center_text(cx, LCD_Y + 6, pd->label, C_LCD_D, FONT_SMALL);
  value_text(b, p);
  center_text(cx, LCD_Y + 26, b, C_LCD_T, FONT_SMALL);
  int32_t v = PAR[p], w = 46;
  rect(cx - w / 2, LCD_Y + 50, w, 4, RGB(60, 46, 16));
  if (pd->lo < 0) {
    int32_t len = (v * (w / 2)) / pd->hi;
    if (len > 0) rect(cx, LCD_Y + 50, len, 4, C_LCD_T);
    else if (len < 0) rect(cx + len, LCD_Y + 50, -len, 4, C_LCD_T);
  } else {
    rect(cx - w / 2, LCD_Y + 50, ((v - pd->lo) * w) / (pd->hi - pd->lo), 4, C_LCD_T);
  }
}

static void draw_save(void) {
  rect(0, LCD_Y, SCREEN_W, LCD_H, C_BG);
  rect(2, LCD_Y + 2, SCREEN_W - 4, LCD_H - 4, C_LCD);
  char b[24];
  put_int(put(b, "SAVE TO U"), save_slot + 1);
  center_text(120, LCD_Y + 8, b, C_LCD_T, FONT_BIG);
  center_text(120, LCD_Y + 46, "PRESETS slot  SAVE ok", C_LCD_D, FONT_SMALL);
}

static void draw_foot(void) {
  rect(0, FOOT_Y, SCREEN_W, SCREEN_H - FOOT_Y, C_BG);
  line(0, FOOT_Y, SCREEN_W - 1, FOOT_Y, C_FAINT);
  char b[24], *d = put(b, "T");
  int t = clampi(PAR[P_TABLE], 0, WT_TABLES - 1);
  d = put_int(put(d, t + 1 < 10 ? "0" : ""), t + 1);
  put(put(d, " "), WT_NAMES[t]);
  text(6, FOOT_Y + 4, b, C_WAVE, FONT_SMALL);
  text(6, FOOT_Y + 22, PAR[P_MODE] ? "MONO" : "POLY", C_DIM, FONT_SMALL);
  d = put(b, "OCT");
  if (store.oct >= 0) d = put(d, "+");
  put_int(d, store.oct);
  text(56, FOOT_Y + 22, b, C_DIM, FONT_SMALL);
  text(112, FOOT_Y + 22, PAR[P_INTERP] ? "2.3" : "2.2", C_DIM, FONT_SMALL);
  if (hold) text(146, FOOT_Y + 22, "HOLD", C_WAVE, FONT_SMALL);
}

static void draw_voices(int n) { /* eight lamps: the voices sounding */
  for (int i = 0; i < VOICES; i++) rect(184 + i * 6, FOOT_Y + 26, 4, 8, i < n ? C_WAVE : C_FAINT);
}

/* ---------------------------------------------------------------- presets */

static void load_patch(const int16_t *p, const char *name) {
  for (int i = 0; i < P_COUNT; i++) PAR[i] = (int16_t)clampi(p[i], PD[i].lo, PD[i].hi);
  for (int i = 0; i < NAME_LEN && (cur_name[i] = name[i]); i++) {
  }
  cur_name[NAME_LEN] = 0;
  modified = 0;
}

static void load_slot(int s) {
  s = (s + 16) % 16;
  store.cur = (uint8_t)s;
  synth_all_off();
  if (s < FACTORY_COUNT) load_patch(FACTORY[s].p, FACTORY[s].name);
  else load_patch(store.user[s - 8].p, store.user[s - 8].name);
  shown_page = -1;
}

static void set_param(int p, int32_t v) {
  v = clampi(v, PD[p].lo, PD[p].hi);
  if (v == PAR[p]) return;
  if (p == P_MODE) synth_all_off();
  PAR[p] = (int16_t)v;
  modified = 1;
}

static void turn(int p, int32_t det) {
  if (p < 0 || !det) return;
  int32_t a = det < 0 ? -det : det, step = PD[p].kind == K_LIST || PD[p].kind == K_TABLE ? 1 : (a > 1 ? 4 : 1);
  if (PD[p].kind == K_LIST || PD[p].kind == K_TABLE) det = det > 0 ? 1 : -1;
  set_param(p, PAR[p] + det * step);
}

/* ---------------------------------------------------------------- notes: keys, MIDI, HOLD */

static void note_on(int n) {
  if (hold && hold_list[n]) synth_note_off(n), hold_list[n] = 0;
  synth_note_on(n);
}
static void note_off(int n) {
  if (hold) hold_list[n] = 1;
  else synth_note_off(n);
}
static void hold_release(void) {
  for (int n = 0; n < 128; n++)
    if (hold_list[n]) hold_list[n] = 0, synth_note_off(n);
}

/* ---------------------------------------------------------------- setup and loop */

void setup(void) {
  if (settings_load(&store, sizeof store) || store.magic != STORE_MAGIC) {
    store.magic = STORE_MAGIC, store.cur = 0, store.oct = 0;
    for (int i = 0; i < USER_COUNT; i++) store.user[i] = FACTORY[i];
  }
  store.oct = (int8_t)clampi(store.oct, -3, 3);
  synth_init();
  load_slot(store.cur);
  led_backlight(1);
  clear(C_BG);
}

void loop(void) {
  uint32_t now = millis();
  int32_t e;

  /* ---- buttons */
  if (saving) {
    if ((e = encoder(ENC_PRESETS))) save_slot = (uint8_t)((save_slot + 8 + (e > 0 ? 1 : -1)) % 8), shown_page = -1;
    if (button_pressed(BTN_SAVE) || button_pressed(BTN_OCT_UP)) {
      preset_t *u = &store.user[save_slot];
      for (int i = 0; i < P_COUNT; i++) u->p[i] = PAR[i];
      for (int i = 0; i <= NAME_LEN; i++) u->name[i] = cur_name[i];
      store.cur = (uint8_t)(8 + save_slot), modified = 0, saving = 0;
      settings_save(&store, sizeof store);
      shown_page = -1;
    } else if (button_pressed(BTN_OCT_DOWN) || button_pressed(BTN_HOME)) {
      saving = 0, shown_page = -1;
    }
  } else {
    for (int i = 0; i < PAGE_BTNS; i++)
      if (button_pressed(PAGE_BTN[i].btn)) {
        int f = PAGE_BTN[i].first, c = PAGE_BTN[i].count;
        page = (uint8_t)((page >= f && page < f + c) ? f + (page - f + 1) % c : f);
      }
    if (button_pressed(BTN_SAVE)) saving = 1, save_slot = store.cur >= 8 ? store.cur - 8 : 0, shown_page = -1;
    if ((e = encoder(ENC_PRESETS))) load_slot(store.cur + (e > 0 ? 1 : -1));
    if (button_pressed(BTN_OCT_DOWN) || button_pressed(BTN_OCT_UP)) {
      if (button_held(BTN_OCT_DOWN) && button_held(BTN_OCT_UP)) store.oct = 0;
      else store.oct = (int8_t)clampi(store.oct + (button_pressed(BTN_OCT_UP) ? 1 : -1), -3, 3);
    }
  }
  if (button_pressed(BTN_PLAY)) {
    hold ^= 1;
    if (!hold) hold_release();
  }

  /* ---- encoders */
  for (int c = 0; c < 4; c++) turn(PAGES[page].p[c], encoder(ENC_KNOB1 + c));
  turn(P_TABLE, encoder(ENC_SELECT));
  turn(P_WAVA, encoder(ENC_ALGORITHM));

  /* ---- keys and MIDI */
  for (int k = 0; k < KEY_COUNT; k++) {
    if (key_pressed(k)) key_note[k] = (uint8_t)clampi(KEY_FIRST_NOTE + k + store.oct * 12, 0, 127), note_on(key_note[k]);
    if (key_released(k) && key_note[k]) note_off(key_note[k]), key_note[k] = 0;
  }
  uint8_t st, d1, d2;
  while (midi_read(&st, &d1, &d2)) {
    uint8_t t = st & 0xF0;
    if (t == 0x90 && d2) note_on(d1), held_midi[d1 & 127] = 1;
    else if (t == 0x80 || (t == 0x90 && !d2)) note_off(d1), held_midi[d1 & 127] = 0;
    else if (t == 0xE0) synth_bend(((((int32_t)d2 << 7) | d1) - 8192) * 512 / 8192); /* +-2 semitones */
    else if (t == 0xB0 && d1 >= 20 && d1 < 20 + P_COUNT) {
      int p = d1 - 20;
      set_param(p, PD[p].lo + ((PD[p].hi - PD[p].lo) * d2 + 63) / 127);
    } else if (t == 0xB0 && d1 == 123) synth_all_off();
  }

  /* ---- LEDs: the page's button, HOLD, the keys sounding */
  uint32_t lk = 0, lb = 0;
  for (int k = 0; k < KEY_COUNT; k++)
    if (key_held(k)) lk |= 1u << k;
  for (int i = 0; i < PAGE_BTNS; i++)
    if (page >= PAGE_BTN[i].first && page < PAGE_BTN[i].first + PAGE_BTN[i].count) lb |= 1u << PAGE_BTN[i].btn;
  if (hold) lb |= 1u << BTN_PLAY;
  if (saving) lb |= 1u << BTN_SAVE;
  if (lk != led_k) {
    for (int i = 0; i < KEY_COUNT; i++)
      if ((lk ^ led_k) >> i & 1u) led_key(i, lk >> i & 1u);
    led_k = lk;
  }
  if (lb != led_b) {
    for (int i = 0; i < BTN_COUNT; i++)
      if ((lb ^ led_b) >> i & 1u) led_button(i, lb >> i & 1u);
    led_b = lb;
  }

  /* ---- the screen, only what changed */
  if (shown_page != (int8_t)page) {
    clear(C_BG);
    shown_page = (int8_t)page;
    sh_head = sh_wave = sh_pos = sh_foot = ~0u;
    for (int c = 0; c < 4; c++) shown_col[c] = -99999;
    rect(0, LCD_Y, SCREEN_W, LCD_H, C_BG);
  }
  uint32_t h = (uint32_t)store.cur | (uint32_t)modified << 5 | (uint32_t)page << 6 | (uint32_t)saving << 10;
  if (h != sh_head) sh_head = h, draw_head();
  h = (uint32_t)PAR[P_TABLE] | (uint32_t)PAR[P_WAVA] << 6 | (uint32_t)PAR[P_WAVB] << 12;
  if (h != sh_wave) sh_wave = h, draw_waves(), sh_pos = ~0u;
  if (now - pos_t >= 40) { /* the moving position, ~25 times a second */
    pos_t = now;
    int live = synth_active ? synth_wave_now : -1;
    h = (uint32_t)(live + 1) | (uint32_t)PAR[P_WAVA] << 16 | (uint32_t)PAR[P_WAVB] << 22;
    if (h != sh_pos) sh_pos = h, draw_pos(live);
    static int lastn = -1;
    if (synth_active != lastn) lastn = synth_active, draw_voices(synth_active);
  }
  if (saving) {
    if (shown_col[0] != -1000 - save_slot) {
      draw_save();
      for (int c = 0; c < 4; c++) shown_col[c] = -1000 - save_slot;
    }
  } else {
    for (int c = 0; c < 4; c++) {
      int p = PAGES[page].p[c];
      int32_t v = p < 0 ? -1 : PAR[p];
      if (v != shown_col[c]) shown_col[c] = v, draw_col(c);
    }
  }
  h = (uint32_t)PAR[P_TABLE] | (uint32_t)PAR[P_MODE] << 6 | (uint32_t)(store.oct + 4) << 7 | (uint32_t)PAR[P_INTERP] << 10 |
      (uint32_t)hold << 11;
  if (h != sh_foot) sh_foot = h, draw_foot(), draw_voices(synth_active);
}

/* ---------------------------------------------------------------- the sound */

void audio(int32_t *out, uint32_t frames) { synth_render(out, frames); }
