/*
 * Microphone: MP34DT06J PDM mic on DFSDM1 (CKOUT PD10, DATIN2 PE7), 16 kHz mono
 *
 * DFSDM1 Filter 0 turns the 2 MHz PDM bit stream into 16-bit PCM in hardware
 * (FastSinc, oversampling 125) and DMA1 Stream 0 writes it into a ring buffer.
 * Every 20 ms block is high-pass filtered (DC removal), amplified and queued
 * in a FIFO that the network code reads from the main loop.
 */
#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

#define AUDIO_SAMPLE_RATE     16000U
#define AUDIO_BLOCK_SAMPLES   320U      /* 20 ms per DMA half-buffer */

/* Start / stop capturing (the mic only gets its clock while running) */
int audio_start(void);
void audio_stop(void);
int audio_running(void);

/* Samples waiting in the FIFO */
uint32_t audio_available(void);

/* Copy up to max samples out of the FIFO; returns the number copied */
uint32_t audio_read(int16_t *out, uint32_t max);

/* Drop everything queued (e.g. when a new listener connects) */
void audio_flush(void);

/*
 * Level statistics since the last call, for the status line:
 * peak in dBFS (-96..0) and the number of blocks dropped because the FIFO
 * was full.
 */
void audio_take_stats(int *peak_dbfs, uint32_t *dropped_blocks);

#endif /* AUDIO_H */
