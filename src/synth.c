/* SPDX-License-Identifier: GPL-3.0-only */
/* WAVESCAN's sound. Integers only: audio Q15, phases 32-bit, envelope levels Q30.
 *
 * The oscillators read 8-bit waves from the tables (wavetables.h). STEP (the 2.2's way) jumps from wave to wave
 * and from sample to sample at the full table size: the grit and aliasing of the original. SMOOTH crossfades
 * the two nearest waves, interpolates between samples and picks a band-limited copy of the table for higher
 * notes. Modulation (envelopes, LFO, glide, filter coefficients) runs every CTRL frames, as the original's
 * control rate did, so sweeps keep a little of its stepping. */
#include "fm1.h"
#include "synth.h"
#include "tables.h"
#include "wavetables.h"

#ifndef RING_PUBLISH
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#endif

#define CTRL 16u
#define Q15 32767
#define ONE30 1073741824

volatile int16_t PAR[P_COUNT];
volatile int16_t synth_wave_now;
volatile uint8_t synth_active;
volatile int32_t synth_lfo_now;

static inline int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline int32_t sat15(int32_t x) {
  if (x >= 49152) return Q15;
  if (x <= -49152) return -Q15;
  int32_t x2 = (int32_t)(((int64_t)x * x) >> 15);
  int32_t x3 = (int32_t)(((int64_t)x2 * x) >> 15);
  return x - ((x3 * 4855) >> 15);
}

/* ---------------------------------------------------------------- envelopes (per control block) */

enum { ST_IDLE, ST_ATTACK, ST_DECAY, ST_SUSTAIN, ST_RELEASE };
typedef struct {
  uint8_t st;
  int32_t lvl; /* Q30 */
} env_t;

typedef struct {
  int32_t astep;    /* Q30 a block */
  uint32_t dc, rc;  /* Q16 a block */
  int32_t sus;      /* Q30 */
} envc_t;

/* a 1/e decay over ms, as a coefficient for one block of CTRL frames (Q16) */
static uint32_t coef_block(int32_t ms) {
  int32_t n = ms * 441; /* frames x 10 */
  if (n <= (int32_t)CTRL * 10) return 0;
  return 65536u - (uint32_t)(10485760 / n); /* 1 - CTRL / frames */
}
static int32_t attack_step(int32_t ms) {
  int32_t blocks = (ms * 441) / ((int32_t)CTRL * 10);
  return blocks < 1 ? ONE30 : ONE30 / blocks;
}
static void envc(envc_t *c, int a, int d, int s, int r) {
  c->astep = attack_step(ENV_MS[clampi(a, 0, 127)]);
  c->dc = coef_block(ENV_MS[clampi(d, 0, 127)]);
  c->rc = coef_block(ENV_MS[clampi(r, 0, 127)]);
  c->sus = (int32_t)(((int64_t)clampi(s, 0, 127) * ONE30) / 127);
}

static void env_step(env_t *e, const envc_t *c) {
  switch (e->st) {
    case ST_ATTACK:
      e->lvl += c->astep;
      if (e->lvl >= ONE30) e->lvl = ONE30, e->st = ST_DECAY;
      break;
    case ST_DECAY:
      e->lvl = c->sus + (int32_t)(((int64_t)(e->lvl - c->sus) * c->dc) >> 16);
      if (e->lvl - c->sus < 65536 && e->lvl - c->sus > -65536) e->lvl = c->sus, e->st = ST_SUSTAIN;
      break;
    case ST_SUSTAIN:
      e->lvl = c->sus;
      break;
    case ST_RELEASE:
      e->lvl = (int32_t)(((int64_t)e->lvl * c->rc) >> 16);
      if (e->lvl < 32768) e->lvl = 0, e->st = ST_IDLE;
      break;
    default:
      break;
  }
}

/* ---------------------------------------------------------------- the 24 dB low-pass (zero-delay feedback) */

typedef struct {
  int32_t s1, s2, s3, s4;
} filt_t;
typedef struct {
  int32_t G, G2, G3, G4, k, inv;
} fcoef_t;

static void filt_coef(fcoef_t *c, int32_t G, int32_t k) {
  c->G = G;
  c->G2 = (int32_t)(((int64_t)G * G) >> 16);
  c->G3 = (int32_t)(((int64_t)c->G2 * G) >> 16);
  c->G4 = (int32_t)(((int64_t)c->G3 * G) >> 16);
  c->k = k;
  c->inv = (int32_t)(0xFFFFFFFFu / (uint32_t)(65536 + (int32_t)(((int64_t)k * c->G4) >> 12)));
}

static inline int32_t filt_sample(filt_t *f, int32_t in, const fcoef_t *c) {
  int32_t X = in * 256;
  X += (int32_t)(((int64_t)X * c->k) >> 15); /* the level, kept as the emphasis rises */
  int32_t G = c->G, iG = 65536 - G;
  int64_t sig = ((((int64_t)f->s1 * iG) >> 16) * c->G3 >> 16) + ((((int64_t)f->s2 * iG) >> 16) * c->G2 >> 16) +
                ((((int64_t)f->s3 * iG) >> 16) * G >> 16) + (((int64_t)f->s4 * iG) >> 16);
  int64_t y4e = ((((int64_t)X * c->G4) >> 16) + sig) * c->inv >> 16;
  int32_t u = X - (int32_t)((c->k * y4e) >> 12);
  u = sat15(u >> 9) * 512;
  int32_t v, y1, y2, y3, y4;
  v = (int32_t)(((int64_t)(u - f->s1) * G) >> 16), y1 = v + f->s1, f->s1 = y1 + v;
  v = (int32_t)(((int64_t)(y1 - f->s2) * G) >> 16), y2 = v + f->s2, f->s2 = y2 + v;
  v = (int32_t)(((int64_t)(y2 - f->s3) * G) >> 16), y3 = v + f->s3, f->s3 = y3 + v;
  v = (int32_t)(((int64_t)(y3 - f->s4) * G) >> 16), y4 = v + f->s4, f->s4 = y4 + v;
  return y4 >> 8;
}

/* ---------------------------------------------------------------- the oscillators */

typedef struct {
  const int8_t *w0, *w1; /* the two nearest waves (SMOOTH), or the one (STEP) */
  int32_t frac;          /* between them, 0..255 */
  uint8_t bits;          /* log2 of the wave's size */
} oscsel_t;

static void osc_select(oscsel_t *o, int table, int32_t posq8, uint32_t inc, int smooth) {
  int32_t p = clampi(posq8, 0, (WT_WAVES - 1) * 256);
  int lvl = 0;
  if (smooth) lvl = inc < 30900000u ? 0 : inc < 62800000u ? 1 : inc < 126600000u ? 2 : 3;
  int w = smooth ? p >> 8 : (p + 128) >> 8;
  int w1 = w + 1 < WT_WAVES ? w + 1 : w;
  switch (lvl) {
    case 0: o->w0 = WT0[table][w], o->w1 = WT0[table][w1], o->bits = 7; break;
    case 1: o->w0 = WT1[table][w], o->w1 = WT1[table][w1], o->bits = 6; break;
    case 2: o->w0 = WT2[table][w], o->w1 = WT2[table][w1], o->bits = 5; break;
    default: o->w0 = WT3[table][w], o->w1 = WT3[table][w1], o->bits = 4; break;
  }
  o->frac = smooth ? (p & 255) : 0;
}

static inline int32_t osc_step(const oscsel_t *o, uint32_t ph) { return o->w0[ph >> (32 - o->bits)] * 256; }

static inline int32_t osc_smooth(const oscsel_t *o, uint32_t ph) {
  uint32_t mask = (1u << o->bits) - 1u;
  uint32_t i0 = ph >> (32 - o->bits), i1 = (i0 + 1u) & mask;
  int32_t f = (int32_t)((ph >> (24 - o->bits)) & 255u);
  int32_t a = o->w0[i0] * (256 - f) + o->w0[i1] * f;
  if (!o->frac) return a;
  int32_t b = o->w1[i0] * (256 - f) + o->w1[i1] * f;
  return a + (((b - a) * o->frac) >> 8);
}

/* the pitch: semitones x 256 -> a phase step */
static uint32_t inc_q8(int32_t p) {
  p = clampi(p, 0, 127 * 256);
  int n = p >> 8, f = p & 255;
  uint32_t a = note_inc(n), b = note_inc(n + 1);
  return a + (uint32_t)(((uint64_t)(b - a) * (uint32_t)f) >> 8);
}

/* ---------------------------------------------------------------- voices */

typedef struct {
  uint8_t note, gate;
  uint32_t age;
  int32_t pitch, target; /* semitones x 256 */
  uint32_t pha, phb;
  env_t e1, e2, e3;
  int32_t amp;      /* Q15, the last block's loudness */
  int32_t lfo_fade; /* Q15: the LFO's delay */
  filt_t f;
} voice_t;

static voice_t V[VOICES];
static uint32_t age_n;
static int32_t bend;

/* ---- the queue from the panel (loop() pushes, audio() takes) */
enum { EV_ON, EV_OFF, EV_ALL };
static struct {
  uint8_t t, n;
} q[32];
static volatile uint8_t qw, qr;
static void push(uint8_t t, uint8_t n) {
  uint8_t nx = (uint8_t)((qw + 1) & 31);
  if (nx == qr) return;
  q[qw].t = t, q[qw].n = n;
  RING_PUBLISH();
  qw = nx;
}
void synth_note_on(int note) { push(EV_ON, (uint8_t)clampi(note, 0, 127)); }
void synth_note_off(int note) { push(EV_OFF, (uint8_t)clampi(note, 0, 127)); }
void synth_all_off(void) { push(EV_ALL, 0); }
void synth_bend(int32_t s) { bend = s; }

/* mono: the held notes, last one on top */
static uint8_t held[16], nheld;

static void voice_start(voice_t *v, int note, int legato) {
  v->note = (uint8_t)note, v->gate = 1, v->age = ++age_n;
  v->target = note * 256;
  if (!legato || PAR[P_GLIDE] == 0) v->pitch = v->target;
  if (!legato) {
    v->e1.st = v->e2.st = v->e3.st = ST_ATTACK;
    v->e3.lvl = 0, v->e1.lvl = v->e1.st == ST_IDLE ? 0 : v->e1.lvl;
    v->lfo_fade = 0;
  }
}

static void note_on(int note) {
  if (PAR[P_MODE] == MODE_MONO) {
    int legato = nheld > 0 && V[0].gate;
    if (nheld < 16) held[nheld++] = (uint8_t)note;
    voice_start(&V[0], note, legato);
    return;
  }
  int best = -1;
  for (int i = 0; i < VOICES; i++)
    if (V[i].note == note && V[i].e2.st != ST_IDLE) best = i; /* the same key again */
  if (best < 0)
    for (int i = 0; i < VOICES; i++)
      if (V[i].e2.st == ST_IDLE) {
        best = i;
        break;
      }
  if (best < 0) { /* steal: the oldest released voice, else the oldest */
    uint32_t oldest = 0xFFFFFFFFu;
    for (int i = 0; i < VOICES; i++)
      if (!V[i].gate && V[i].age < oldest) oldest = V[i].age, best = i;
    if (best < 0)
      for (int i = 0; i < VOICES; i++)
        if (V[i].age < oldest) oldest = V[i].age, best = i;
  }
  voice_start(&V[best], note, 0);
}

static void note_off(int note) {
  if (PAR[P_MODE] == MODE_MONO) {
    int j = 0;
    for (int i = 0; i < nheld; i++)
      if (held[i] != note) held[j++] = held[i];
    nheld = (uint8_t)j;
    if (nheld) voice_start(&V[0], held[nheld - 1], 1); /* back to the note still held */
    else if (V[0].note == note) V[0].gate = 0, V[0].e1.st = V[0].e2.st = ST_RELEASE;
    return;
  }
  for (int i = 0; i < VOICES; i++)
    if (V[i].note == note && V[i].gate) V[i].gate = 0, V[i].e1.st = V[i].e2.st = ST_RELEASE;
}

/* ---------------------------------------------------------------- the LFO */

static uint32_t lfo_ph;
static int32_t lfo_rnd;

static int32_t lfo_value(int shape) {
  int32_t s = (int32_t)(lfo_ph >> 16) - 32768;
  switch (shape) {
    case LFO_SAWDN: return -s;
    case LFO_SAWUP: return s;
    case LFO_SQR: return lfo_ph < 2147483648u ? Q15 : -Q15;
    case LFO_RND: return lfo_rnd;
    default: return 2 * (s < 0 ? -s : s) - 32768;
  }
}

/* ---------------------------------------------------------------- the delay (stereo, half rate) */

#define DL_N 11025 /* 0.5 s at 22.05 kHz */
static int16_t dl_l[DL_N] POOL, dl_r[DL_N] POOL;
static int32_t dl_w, dl_len = 6000 << 8, dl_half, dl_accl, dl_accr, dl_ol, dl_or, dl_pl, dl_pr, dl_lp_l, dl_lp_r;

static inline int32_t dl_read(const int16_t *b, int32_t dq8) {
  int32_t i = dl_w - (dq8 >> 8), f = dq8 & 255;
  if (i < 0) i += DL_N;
  int32_t j = i - 1;
  if (j < 0) j += DL_N;
  return (b[i] * (256 - f) + b[j] * f) >> 8;
}

/* ---------------------------------------------------------------- render */

void synth_init(void) {
  for (int i = 0; i < VOICES; i++) V[i] = (voice_t){0};
  for (int i = 0; i < DL_N; i++) dl_l[i] = dl_r[i] = 0;
}

void synth_render(int32_t *out, uint32_t frames) {
  /* the panel's notes */
  while (qr != qw) {
    uint8_t t = q[qr].t, n = q[qr].n;
    qr = (uint8_t)((qr + 1) & 31);
    if (t == EV_ON) note_on(n);
    else if (t == EV_OFF) note_off(n);
    else {
      nheld = 0;
      for (int i = 0; i < VOICES; i++)
        if (V[i].gate) V[i].gate = 0, V[i].e1.st = V[i].e2.st = ST_RELEASE;
    }
  }

  int table = clampi(PAR[P_TABLE], 0, WT_TABLES - 1), smooth = PAR[P_INTERP] == INTERP_SMOOTH;
  envc_t c1, c2, c3;
  envc(&c1, PAR[P_E1A], PAR[P_E1D], PAR[P_E1S], PAR[P_E1R]);
  envc(&c2, PAR[P_E2A], PAR[P_E2D], PAR[P_E2S], PAR[P_E2R]);
  envc(&c3, PAR[P_E3A], PAR[P_E3D], 0, 0);
  uint32_t glide_c = PAR[P_GLIDE] ? coef_block(ENV_MS[clampi(PAR[P_GLIDE], 0, 127)] / 4 + 1) : 0;
  int32_t lfo_fade_step = PAR[P_LDELAY] ? attack_step(ENV_MS[clampi(PAR[P_LDELAY], 0, 127)]) >> 15 : Q15;
  int32_t drive = 1024 + clampi(PAR[P_DRIVE], 0, 127) * 32, vol = clampi(PAR[P_VOL], 0, 127);
  int32_t spread = clampi(PAR[P_SPREAD], 0, 127);
  int32_t res_k = clampi(PAR[P_RES], 0, 127) * 137;
  int32_t mixb = clampi(PAR[P_MIX], 0, 127), mixa = 127 - mixb;
  int32_t semi = clampi(PAR[P_SEMI], -24, 24) * 256 + (clampi(PAR[P_DETUNE], -50, 50) * 256) / 100;

  int newest = -1;
  uint32_t newest_age = 0;
  uint8_t active = 0;

  for (uint32_t b0 = 0; b0 < frames; b0 += CTRL) {
    uint32_t n = frames - b0 < CTRL ? frames - b0 : CTRL;
    /* the LFO, once a block */
    uint32_t prev = lfo_ph;
    lfo_ph += LFO_INC[clampi(PAR[P_LRATE], 0, 127)] * n;
    if (lfo_ph < prev) lfo_rnd = (int32_t)(random32() >> 16) - 32768;
    int32_t lfo = lfo_value(PAR[P_LSHAPE]);
    synth_lfo_now = lfo;

    for (int vi = 0; vi < VOICES; vi++) {
      voice_t *v = &V[vi];
      if (v->e2.st == ST_IDLE) {
        v->amp = 0;
        continue;
      }
      active++;
      if (v->age > newest_age) newest_age = v->age, newest = vi;
      env_step(&v->e1, &c1), env_step(&v->e2, &c2), env_step(&v->e3, &c3);
      int32_t e1 = v->e1.lvl >> 15, e2 = v->e2.lvl >> 15, e3 = v->e3.lvl >> 15;
      v->lfo_fade = v->lfo_fade + lfo_fade_step > Q15 ? Q15 : v->lfo_fade + lfo_fade_step;
      int32_t vl = (lfo * v->lfo_fade) >> 15;

      /* pitch: glide, ENV3, LFO, bend */
      if (glide_c) v->pitch = v->target + (int32_t)(((int64_t)(v->pitch - v->target) * glide_c) >> 16);
      else v->pitch = v->target;
      int32_t p = v->pitch + bend + ((vl * PAR[P_LPITCH]) >> 12);
      if (PAR[P_E3DST] == E3_PITCH) p += (e3 * PAR[P_E3AMT]) >> 9;
      uint32_t inca = inc_q8(p), incb = inc_q8(p + semi);

      /* the waves: position x 256, with ENV3, the LFO and the key */
      int32_t wmod = ((vl * PAR[P_LWAVE]) >> 7) + (((v->note - 60) * PAR[P_KEYWAVE] * 256) / 48);
      int32_t e3w = (e3 * PAR[P_E3AMT]) >> 7;
      int32_t wa = PAR[P_WAVA] * 256 + wmod + (PAR[P_E3DST] != E3_PITCH ? e3w : 0);
      int32_t wb = PAR[P_WAVB] * 256 + wmod + (PAR[P_E3DST] == E3_WAVES ? e3w : 0);
      if (vi == newest) synth_wave_now = (int16_t)clampi(wa, 0, 63 * 256);
      oscsel_t oa, ob;
      osc_select(&oa, table, wa, inca, smooth);
      osc_select(&ob, table, wb, incb, smooth);

      /* the filter: cutoff with ENV1, the key and the LFO */
      int32_t ci = PAR[P_CUT] * 8 + ((e1 * PAR[P_E1VCF]) >> 12) + (((v->note - 60) * PAR[P_KEYVCF] * 9) / 127) +
                   ((vl * PAR[P_LVCF]) >> 13);
      fcoef_t fc;
      filt_coef(&fc, FILT_G[clampi(ci, 0, 1023)], res_k);

      /* loudness: ENV2, the LFO's tremolo; ramped over the block */
      int32_t amp = e2;
      if (PAR[P_LAMP]) amp = (amp * (Q15 - (((vl + Q15) >> 1) * PAR[P_LAMP] >> 7))) >> 15;
      int32_t a0 = v->amp, da = (amp - a0) / (int32_t)n;
      v->amp = amp;
      int32_t pan = (vi & 1) ? spread : -spread; /* -127 .. 127 */
      int32_t gl = (128 - pan) * vol, gr = (128 + pan) * vol; /* /2^14 at the end */

      uint32_t pa = v->pha, pb = v->phb;
      for (uint32_t i = 0; i < n; i++) {
        int32_t sa = smooth ? osc_smooth(&oa, pa) : osc_step(&oa, pa);
        int32_t sb = smooth ? osc_smooth(&ob, pb) : osc_step(&ob, pb);
        pa += inca, pb += incb;
        int32_t s = (sa * mixa + sb * mixb) >> 8; /* (int8 x 256) x 127 / 256: about Q15 / 2 */
        s = filt_sample(&v->f, s, &fc);
        s = sat15((s * drive) >> 10);
        int32_t a = a0 + da * (int32_t)i;
        s = (s * a) >> 15;
        out[2 * (b0 + i)] += (s * gl) >> 14;
        out[2 * (b0 + i) + 1] += (s * gr) >> 14;
      }
      v->pha = pa, v->phb = pb;
    }
  }
  synth_active = (uint8_t)(active / ((frames + CTRL - 1) / CTRL));

  /* the delay (ping-pong, half rate), and the final soft clip */
  int32_t dmix = clampi(PAR[P_DMIX], 0, 127), dfb = clampi(PAR[P_DFB], 0, 127) * 230;
  int32_t dlen_to = (440 + clampi(PAR[P_DTIME], 0, 127) * 82) << 8; /* 20 .. ~490 ms */
  for (uint32_t i = 0; i < frames; i++) {
    int32_t l = out[2 * i], r = out[2 * i + 1];
    if (dmix) {
      dl_accl += l, dl_accr += r;
      if (dl_half) {
        dl_len += (dlen_to - dl_len) >> 10;
        int32_t rl = dl_read(dl_l, dl_len), rr = dl_read(dl_r, dl_len);
        dl_lp_l += ((rl - dl_lp_l) * 20000) >> 15, dl_lp_r += ((rr - dl_lp_r) * 20000) >> 15;
        int32_t inm = (dl_accl + dl_accr) >> 2;
        dl_l[dl_w] = (int16_t)clampi(sat15((inm >> 1) + ((dl_lp_r * dfb) >> 15)), -32767, 32767); /* ping */
        dl_r[dl_w] = (int16_t)clampi(sat15((dl_lp_l * dfb) >> 15), -32767, 32767);                /* pong */
        if (++dl_w >= DL_N) dl_w = 0;
        dl_pl = dl_ol, dl_pr = dl_or, dl_ol = rl, dl_or = rr;
        dl_accl = dl_accr = 0;
      }
      int32_t wl = dl_half ? dl_ol : (dl_pl + dl_ol) >> 1, wr = dl_half ? dl_or : (dl_pr + dl_or) >> 1;
      dl_half ^= 1;
      l += (wl * dmix * 2) >> 8, r += (wr * dmix * 2) >> 8;
    }
    out[2 * i] = sat15(l), out[2 * i + 1] = sat15(r);
  }
}
