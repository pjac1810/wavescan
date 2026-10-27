/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test for WAVESCAN: each factory sound plays a chord, then a line, to build/render-out/N-NAME.wav; the CPU
 * time of 8 voices is measured; then a fuzz of random settings and notes checks the output stays in range.
 *   tests/run.sh */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "presets.h"
#include "synth.h"

int32_t sine(uint32_t phase) {
  int32_t x = (int32_t)phase >> 16, ax = x < 0 ? -x : x, y, ay;
  y = (x * (32768 - ax)) >> 13;
  ay = y < 0 ? -y : y;
  y += ((((y * ay) >> 15) - y) * 7373) >> 15;
  return y > 32767 ? 32767 : y < -32767 ? -32767 : y;
}
static const uint32_t INC_C9[12] = {815363807u, 863847862u, 915214929u, 969636441u, 1027294024u, 1088380105u,
                                    1153098554u, 1221665363u, 1294309365u, 1371273005u, 1452813141u, 1539201906u};
uint32_t note_inc(int note) {
  if (note < 0) note = 0;
  if (note > 127) note = 127;
  return INC_C9[note % 12] >> (10 - note / 12);
}
static uint32_t rs = 0x1234567u;
uint32_t random32(void) {
  rs ^= rs << 13, rs ^= rs >> 17, rs ^= rs << 5;
  return rs;
}

#define MAXF (44100 * 10)
static int16_t buf[MAXF * 2];
static uint32_t pos;
static int fail;

static void render(uint32_t frames) {
  static int32_t blk[512];
  while (frames) {
    uint32_t n = frames > 256 ? 256 : frames;
    memset(blk, 0, sizeof blk);
    synth_render(blk, n);
    for (uint32_t i = 0; i < 2 * n; i++) {
      if (blk[i] > 32767 || blk[i] < -32767) fail = 1;
      if (pos * 2 + i < MAXF * 2) buf[pos * 2 + i] = (int16_t)blk[i];
    }
    pos += n, frames -= n;
  }
}

static void save(const char *path) {
  FILE *f = fopen(path, "wb");
  uint32_t frames = pos < MAXF ? pos : MAXF, data = frames * 4, riff = 36 + data, rate = 44100, br = rate * 4, fmt = 16;
  uint16_t pcm = 1, ch = 2, al = 4, bits = 16;
  fwrite("RIFF", 1, 4, f), fwrite(&riff, 4, 1, f), fwrite("WAVEfmt ", 1, 8, f), fwrite(&fmt, 4, 1, f);
  fwrite(&pcm, 2, 1, f), fwrite(&ch, 2, 1, f), fwrite(&rate, 4, 1, f), fwrite(&br, 4, 1, f);
  fwrite(&al, 2, 1, f), fwrite(&bits, 2, 1, f), fwrite("data", 1, 4, f), fwrite(&data, 4, 1, f);
  fwrite(buf, 2, frames * 2, f);
  fclose(f);
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : ".";
  synth_init();
  printf("%-12s %7s %7s %7s\n", "sound", "peak", "rms", "clip%");
  for (int k = 0; k < FACTORY_COUNT; k++) {
    for (int i = 0; i < P_COUNT; i++) PAR[i] = FACTORY[k].p[i];
    synth_all_off(), render(30000), pos = 0;
    int mono = PAR[P_MODE] == MODE_MONO, base = mono ? 40 : 52;
    static const int CH[4] = {0, 4, 7, 11};
    if (!mono) {
      for (int i = 0; i < 4; i++) synth_note_on(base + CH[i]);
      render(44100 * 2);
      for (int i = 0; i < 4; i++) synth_note_off(base + CH[i]);
      render(22050);
    }
    static const int LINE[8] = {0, 7, 12, 10, 7, 3, 5, 12};
    for (int i = 0; i < 8; i++) {
      synth_note_on(base + 12 + LINE[i]);
      render(9000);
      if (!mono || i == 7) synth_note_off(base + 12 + LINE[i]);
      if (mono && i < 7) synth_note_off(base + 12 + LINE[i]);
      render(2000);
    }
    render(44100);
    double sq = 0;
    int peak = 0, clip = 0;
    uint32_t n = (pos < MAXF ? pos : MAXF) * 2;
    for (uint32_t i = 0; i < n; i++) {
      int v = abs(buf[i]);
      sq += (double)v * v, peak = v > peak ? v : peak, clip += v >= 32700;
    }
    printf("%-12s %7d %7.0f %7.3f\n", FACTORY[k].name, peak, sqrt(sq / n), 100.0 * clip / n);
    char path[512];
    snprintf(path, sizeof path, "%s/%d-%s.wav", dir, k + 1, FACTORY[k].name);
    for (char *c = path + strlen(dir) + 1; *c; c++)
      if (*c == ' ') *c = '_';
    save(path);
    pos = 0;
  }

  /* CPU: 8 voices held, 10 s, both oscillator modes */
  for (int m = 0; m < 2; m++) {
    for (int i = 0; i < P_COUNT; i++) PAR[i] = FACTORY[0].p[i];
    PAR[P_INTERP] = (int16_t)m, PAR[P_DMIX] = 40;
    synth_all_off(), render(44100), pos = 0;
    for (int i = 0; i < 8; i++) synth_note_on(48 + i * 3);
    clock_t t0 = clock();
    render(44100 * 10);
    pos = 0;
    printf("cpu (%s, 8 voices): %.1f ms for 10 s on the host\n", m ? "SMOOTH" : "STEP", (clock() - t0) * 1000.0 / CLOCKS_PER_SEC);
  }

  /* fuzz */
  srand(5);
  for (int b = 0; b < 20000; b++) {
    int p = rand() % P_COUNT, v = rand() % 255 - 127;
    PAR[p] = (int16_t)v; /* anything, out of range too: the engine must clamp */
    if (p == P_TABLE || p == P_INTERP || p == P_LSHAPE || p == P_MODE || p == P_E3DST) PAR[p] = (int16_t)(rand() % 3);
    int r = rand() % 4;
    if (r == 0) synth_note_on(rand() % 128);
    else if (r == 1) synth_note_off(rand() % 128);
    if (b % 777 == 0) synth_all_off();
    synth_bend((rand() % 1025) - 512);
    render(256);
    pos = 0;
  }
  printf("fuzz: 20000 blocks, %s\n", fail ? "FAILED" : "ok");
  return fail;
}
