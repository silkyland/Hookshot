/* Diagnostic: the game's real bundled decoder + I420->NV12 copy, without guest/render work.
   Build with build_dav1d_bench.py, run on a user-provided AV1 IVF remux (never re-encode).
   Usage: dav1d-bench input.ivf [threads:0=auto] [frame-delay:0=auto] */
#include <dav1d/dav1d.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    Dav1dContext *dav1d;
    long n_in, n_out;
    int dead, eos_in;
    uint8_t *nv12;
    size_t nv12_size;
} Av1;
typedef void (*mf_av1_picture_fn)(void *, const uint8_t *, const uint8_t *, int, int, int, int64_t);
static void hos_log(const char *format, ...) {
    va_list args; va_start(args, format); vfprintf(stderr, format, args); fputc('\n', stderr); va_end(args);
}
#include "game_dav1d.inc"

static double seconds(clockid_t clock) {
    struct timespec ts; clock_gettime(clock, &ts); return ts.tv_sec + ts.tv_nsec / 1e9;
}
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t checksum, pictures;
static int last_w, last_h;
__attribute__((noinline)) static void picture(void *user, const uint8_t *y, const uint8_t *uv,
                                             int w, int h, int stride, int64_t pts) {
    (void)user; (void)pts;
    if (stride != w || w <= 0 || h <= 0) abort();
    for (size_t i = 0; i < (size_t)w * h; i += 4096) checksum = checksum * 33 + y[i];
    for (size_t i = 0; i < (size_t)w * h / 2; i += 4096) checksum = checksum * 33 + uv[i];
    pictures++; last_w = w; last_h = h;
}
static int compare(const void *a, const void *b) {
    const double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static char report[4096];
static int bench_main(int argc, char **argv) {
    checksum = pictures = 0;
    report[0] = 0;
    if (argc < 2 || argc > 4) { fprintf(stderr, "Usage: %s input.ivf [threads] [delay]\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f || fseek(f, 0, SEEK_END)) { perror("input"); return 2; }
    long length = ftell(f);
    if (length < 32 || length > 1024L * 1024 * 1024) return 2;
    rewind(f);
    uint8_t *input = malloc((size_t)length);
    if (!input || fread(input, 1, length, f) != (size_t)length) return 2;
    fclose(f);
    if (memcmp(input, "DKIF", 4) || input[6] != 32 || input[7] || memcmp(input + 8, "AV01", 4)) return 2;
    const uint32_t rate = le32(input + 16), scale = le32(input + 20), expected = le32(input + 24);
    if (!rate || !scale) return 2;
    Dav1dSettings settings; dav1d_default_settings(&settings);
    if (argc > 2) settings.n_threads = atoi(argv[2]);
    if (argc > 3) settings.max_frame_delay = atoi(argv[3]);
    Av1 decoder = {0};
    if (dav1d_open(&decoder.dav1d, &settings) < 0) return 2;
    size_t capacity = 4096, count = 0;
    double *times = malloc(capacity * sizeof *times);
    if (!times) return 2;
    const double start = seconds(CLOCK_MONOTONIC), cpu_start = seconds(CLOCK_PROCESS_CPUTIME_ID);
    for (size_t offset = 32; offset < (size_t)length;) {
        if ((size_t)length - offset < 12) return 2;
        const uint32_t bytes = le32(input + offset);
        const uint64_t ts = le32(input + offset + 4) | (uint64_t)le32(input + offset + 8) << 32;
        offset += 12;
        if (bytes > (size_t)length - offset) return 2;
        const double before = seconds(CLOCK_MONOTONIC);
        d1_decode(&decoder, input + offset, bytes, (int64_t)(ts * (double)scale / rate * 1e9), picture, NULL);
        const double elapsed = (seconds(CLOCK_MONOTONIC) - before) * 1000;
        if (count == capacity) { capacity *= 2; double *next = realloc(times, capacity * sizeof *times); if (!next) return 2; times = next; }
        times[count++] = elapsed;
        if (decoder.dead) return 2;
        offset += bytes;
    }
    d1_decode(&decoder, NULL, 0, 0, picture, NULL);
    const double elapsed = seconds(CLOCK_MONOTONIC) - start, cpu = seconds(CLOCK_PROCESS_CPUTIME_ID) - cpu_start;
    if (!count || !pictures || (expected && pictures != expected)) {
        fprintf(stderr, "Incomplete: pictures %" PRIu64 "/%u\n", pictures, expected); return 1;
    }
    qsort(times, count, sizeof *times, compare);
    snprintf(report, sizeof report, "{\"version\":\"%s\",\"threads\":%d,\"frame_delay\":%d,\"frames\":%" PRIu64 ",\"expected\":%u,"
           "\"width\":%d,\"height\":%d,\"seconds\":%.6f,\"fps\":%.3f,\"cpu_seconds\":%.6f,"
           "\"input_call_p50_ms\":%.3f,\"input_call_p95_ms\":%.3f,\"input_call_max_ms\":%.3f,"
           "\"checksum\":\"%016" PRIx64 "\"}\n", dav1d_version(), settings.n_threads, settings.max_frame_delay,
           pictures, expected, last_w, last_h, elapsed, pictures / elapsed, cpu,
           times[(count - 1) / 2], times[(count - 1) * 95 / 100], times[count - 1], checksum);
    dav1d_close(&decoder.dav1d); free(decoder.nv12); free(times); free(input);
    return 0;
}

#ifndef HOOKSHOT_BENCH_JNI
int main(int argc, char **argv) {
    int result = bench_main(argc, argv);
    if (!result) puts(report);
    return result;
}
#else
#include <jni.h>
#include <sched.h>
JNIEXPORT jstring JNICALL Java_dev_hookshot_diagnostics_ProbeActivity_benchmark(
    JNIEnv *env, jclass type, jstring path, jint threads, jint delay, jint mask) {
    (void)type;
    cpu_set_t cpus; CPU_ZERO(&cpus);
    for (int i = 0; i < 8; i++) if (mask & (1 << i)) CPU_SET(i, &cpus);
    if (sched_setaffinity(0, sizeof cpus, &cpus))
        return (*env)->NewStringUTF(env, "{\"error\":\"sched_setaffinity failed\"}");
    const char *input = (*env)->GetStringUTFChars(env, path, NULL);
    if (!input) return NULL;
    char t[16], d[16]; snprintf(t, sizeof t, "%d", threads); snprintf(d, sizeof d, "%d", delay);
    char *argv[] = {"dav1d-bench", (char *)input, t, d};
    const int result = bench_main(4, argv);
    (*env)->ReleaseStringUTFChars(env, path, input);
    if (result) snprintf(report, sizeof report, "{\"error\":\"native benchmark failed\",\"code\":%d}", result);
    return (*env)->NewStringUTF(env, report);
}
#endif
