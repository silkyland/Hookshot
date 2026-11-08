/* (android) waveOut on AAudio (API 26+; minSdk 30), what host/posix/win32_posix.c's SDL waveOut is on Linux:
   the blocks written in order go to the stream, each marked WHDR_DONE once written (AAudio's blocking write
   returns when the frame is accepted; the device's own latency covers the rest, as SDL's queue did). One worker
   thread per open device drains the queue; waveOutReset stops it and marks every header done. nn_audio.c polls
   WHDR_DONE itself, so no completion callback is needed. */
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <android/log.h>
#include <aaudio/AAudio.h>

#include "../hos.h"
#include <windows.h>
#include <mmsystem.h>

typedef struct {
    AAudioStream *stream;
    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int quit;
    int bpf;               /* bytes a frame (all channels): what dwBufferLength counts in */
    WAVEHDR *q[64];
    int head, count;
} AWave;

static void *awave_main(void *p) {
    AWave *w = (AWave *)p;
    for (;;) {
        pthread_mutex_lock(&w->mu);
        while (!w->count && !w->quit) pthread_cond_wait(&w->cv, &w->mu);
        WAVEHDR *h = w->count ? w->q[w->head] : NULL;
        if (h) {
            w->head = (w->head + 1) % 64;
            w->count--;
        }
        pthread_mutex_unlock(&w->mu);
        if (!h) {
            if (w->quit) break;
            continue;
        }
        const int64_t frames = w->bpf ? (int64_t)h->dwBufferLength / w->bpf : 0;
        int64_t off = 0;
        while (off < frames && !w->quit) {
            const int64_t n = AAudioStream_write(w->stream, (const uint8_t *)h->lpData + off * w->bpf,
                                                 (int32_t)(frames - off), 1000000000LL);
            if (n <= 0) break;
            off += n;
        }
        h->dwFlags |= WHDR_DONE;
    }
    return NULL;
}

MMRESULT waveOutOpen(HWAVEOUT *out, UINT device, const WAVEFORMATEX *f, DWORD_PTR cb, DWORD_PTR inst, DWORD flags) {
    (void)device, (void)cb, (void)inst, (void)flags;
    if (f->wFormatTag != WAVE_FORMAT_PCM || f->nChannels < 1 || f->nChannels > 8 || f->wBitsPerSample != 16)
        return MMSYSERR_ERROR;   /* (AAudio has no 8-bit PCM; the game's mix is 16-bit) */
    AAudioStreamBuilder *b = NULL;
    AAudioStream *s = NULL;
    aaudio_result_t r = AAudio_createStreamBuilder(&b);
    if (r == AAUDIO_OK) {
        /* (the setters return void in the NDK; the result comes from openStream) */
        AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setSampleRate(b, (int32_t)f->nSamplesPerSec);
        AAudioStreamBuilder_setChannelCount(b, (int32_t)f->nChannels);
        AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        r = AAudioStreamBuilder_openStream(b, &s);
    }
    if (b) AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK || !s) {
        hos_log("audio: AAudio stream: %s", r == AAUDIO_OK ? "no stream" : AAudio_convertResultToText(r));
        return MMSYSERR_ERROR;
    }
    AAudioStream_requestStart(s);
    AWave *w = (AWave *)calloc(1, sizeof *w);
    w->stream = s;
    w->bpf = f->nBlockAlign ? (int)f->nBlockAlign : (int)f->nChannels * (f->wBitsPerSample / 8);
    pthread_mutex_init(&w->mu, NULL);
    pthread_cond_init(&w->cv, NULL);
    if (pthread_create(&w->thread, NULL, awave_main, w)) {
        AAudioStream_close(s);
        free(w);
        return MMSYSERR_ERROR;
    }
    *out = (HWAVEOUT)w;
    return MMSYSERR_NOERROR;
}

MMRESULT waveOutPrepareHeader(HWAVEOUT hw, WAVEHDR *h, UINT size) { (void)hw, (void)size; h->dwFlags |= WHDR_PREPARED; return 0; }
MMRESULT waveOutUnprepareHeader(HWAVEOUT hw, WAVEHDR *h, UINT size) { (void)hw, (void)size; h->dwFlags &= ~(DWORD)WHDR_PREPARED; return 0; }
MMRESULT waveOutWrite(HWAVEOUT hw, WAVEHDR *h, UINT size) {
    (void)size;
    AWave *w = (AWave *)hw;
    pthread_mutex_lock(&w->mu);
    if (w->count == 64) {   /* (the device is behind: the block is dropped, as on the SDL queue) */
        pthread_mutex_unlock(&w->mu);
        h->dwFlags |= WHDR_DONE;
        return 0;
    }
    w->q[(w->head + w->count) % 64] = h;
    w->count++;
    pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->mu);
    return 0;
}
MMRESULT waveOutReset(HWAVEOUT hw) {
    AWave *w = (AWave *)hw;
    pthread_mutex_lock(&w->mu);
    while (w->count) {
        WAVEHDR *h = w->q[w->head];
        w->head = (w->head + 1) % 64;
        w->count--;
        h->dwFlags |= WHDR_DONE;
    }
    pthread_mutex_unlock(&w->mu);
    return 0;
}
MMRESULT waveOutClose(HWAVEOUT hw) {
    AWave *w = (AWave *)hw;
    if (!w) return 0;
    waveOutReset(hw);
    pthread_mutex_lock(&w->mu);
    w->quit = 1;
    pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->mu);
    pthread_join(w->thread, NULL);
    AAudioStream_requestStop(w->stream);
    AAudioStream_close(w->stream);
    pthread_mutex_destroy(&w->mu);
    pthread_cond_destroy(&w->cv);
    free(w);
    return 0;
}
