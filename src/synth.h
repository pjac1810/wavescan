/* SPDX-License-Identifier: GPL-3.0-only */
/* WAVESCAN: an 8-voice wavetable synthesizer for the M-VAVE FM-1, after the PPG Wave 2.2 / 2.3 and Waldorf's
 * PPG Wave 3.V. Per voice: two wavetable oscillators (A and B) reading 8-bit single-cycle waves, a 24 dB low-pass
 * with emphasis (the SSM2044's role), an overdrive after it, three envelopes (ENV1 filter, ENV2 loudness, ENV3
 * free: waves or pitch). One LFO for all voices, with a delay per note. A stereo delay at the end. */
#ifndef WAVESCAN_SYNTH_H
#define WAVESCAN_SYNTH_H
#include <stdint.h>

enum {
  /* oscillators */
  P_TABLE, P_WAVA, P_WAVB, P_SEMI, P_DETUNE, P_MIX, P_INTERP, P_KEYWAVE,
  /* ENV1 (filter), ENV2 (loudness), ENV3 (free) */
  P_E1A, P_E1D, P_E1S, P_E1R, P_E2A, P_E2D, P_E2S, P_E2R, P_E3A, P_E3D, P_E3AMT, P_E3DST,
  /* filter */
  P_CUT, P_RES, P_E1VCF, P_KEYVCF,
  /* LFO */
  P_LSHAPE, P_LRATE, P_LDELAY, P_LWAVE, P_LPITCH, P_LVCF, P_LAMP,
  /* voice, effects */
  P_MODE, P_GLIDE, P_DRIVE, P_VOL, P_DMIX, P_DTIME, P_DFB, P_SPREAD,
  P_COUNT
};

enum { INTERP_STEP, INTERP_SMOOTH };          /* 2.2: waves and samples step; 2.3-style: both interpolated */
enum { E3_WAVES, E3_PITCH, E3_WAVE_A };        /* ENV3's destination */
enum { LFO_TRI, LFO_SAWDN, LFO_SAWUP, LFO_SQR, LFO_RND, LFO_SHAPES };
enum { MODE_POLY, MODE_MONO };

#define VOICES 8

extern volatile int16_t PAR[P_COUNT];

void synth_init(void);
void synth_note_on(int note);
void synth_note_off(int note);
void synth_all_off(void);
void synth_bend(int32_t semis_q8); /* pitch bend, semitones x 256 */
void synth_render(int32_t *out, uint32_t frames);

/* for the panel */
extern volatile int16_t synth_wave_now; /* the wave position of the newest voice, x 256 (0 .. 63 x 256) */
extern volatile uint8_t synth_active;   /* voices sounding */
extern volatile int32_t synth_lfo_now;  /* +-32767 */

#endif
