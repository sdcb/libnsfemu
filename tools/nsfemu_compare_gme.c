#include "nsfemu.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

typedef struct Music_Emu Music_Emu;
typedef const char *gme_err_t;
typedef void (__cdecl *gme_apu_write_callback_t)(void *, int, int, int, int);

typedef gme_err_t (__cdecl *gme_open_data_fn)(const void *, long, Music_Emu **, int);
typedef void (__cdecl *gme_delete_fn)(Music_Emu *);
typedef gme_err_t (__cdecl *gme_start_track_fn)(Music_Emu *, int);
typedef gme_err_t (__cdecl *gme_play_fn)(Music_Emu *, long, int16_t *);
typedef void (__cdecl *gme_ignore_silence_fn)(Music_Emu *, int);
typedef void (__cdecl *gme_set_autoload_playback_limit_fn)(Music_Emu *, int);
typedef void (__cdecl *gme_mute_voices_fn)(Music_Emu *, int);
typedef void (__cdecl *gme_set_apu_write_callback_fn)(Music_Emu *, gme_apu_write_callback_t, void *);

typedef struct gme_api {
    HMODULE dll;
    gme_open_data_fn open_data;
    gme_delete_fn delete_;
    gme_start_track_fn start_track;
    gme_play_fn play;
    gme_ignore_silence_fn ignore_silence;
    gme_set_autoload_playback_limit_fn set_autoload_playback_limit;
    gme_mute_voices_fn mute_voices;
    gme_set_apu_write_callback_fn set_apu_write_callback;
} gme_api_t;

typedef struct write_hash {
    uint64_t hash;
    uint64_t count;
    uint64_t cap;
    struct write_record *records;
} write_hash_t;

typedef struct write_record {
    int frame;
    int sequence;
    int address;
    int value;
} write_record_t;

static void hash_u32(write_hash_t *h, uint32_t v)
{
    for (int i = 0; i < 4; ++i) {
        h->hash ^= (uint8_t)(v >> (i * 8));
        h->hash *= 1099511628211ull;
    }
}

static void add_write_hash(write_hash_t *h, int frame, int sequence, int address, int value)
{
    hash_u32(h, (uint32_t)frame);
    hash_u32(h, (uint32_t)sequence);
    hash_u32(h, (uint32_t)address);
    hash_u32(h, (uint32_t)value);
    if (h->records && h->count < h->cap) {
        h->records[h->count].frame = frame;
        h->records[h->count].sequence = sequence;
        h->records[h->count].address = address;
        h->records[h->count].value = value;
    }
    h->count++;
}

static void __cdecl on_gme_write(void *user, int frame, int sequence, int address, int value)
{
    add_write_hash((write_hash_t *)user, frame, sequence, address, value);
}

static void on_ours_write(void *user, int frame, int sequence, int address, int value)
{
    add_write_hash((write_hash_t *)user, frame, sequence, address, value);
}

static int load_gme(gme_api_t *api)
{
    memset(api, 0, sizeof(*api));
    api->dll = LoadLibraryW(L"C:\\_\\3rd\\vcpkg\\installed\\x64-windows\\bin\\gme.dll");
    if (!api->dll) {
        api->dll = LoadLibraryW(L"gme.dll");
    }
    if (!api->dll) {
        fwprintf(stderr, L"failed to load gme.dll\n");
        return 0;
    }

#define LOAD(name) do { \
    api->name = (gme_##name##_fn)GetProcAddress(api->dll, "gme_" #name); \
    if (!api->name) { fprintf(stderr, "missing gme_%s\n", #name); return 0; } \
} while (0)

    LOAD(open_data);
    api->delete_ = (gme_delete_fn)GetProcAddress(api->dll, "gme_delete");
    LOAD(start_track);
    LOAD(play);
    LOAD(ignore_silence);
    api->set_autoload_playback_limit =
        (gme_set_autoload_playback_limit_fn)GetProcAddress(api->dll, "gme_set_autoload_playback_limit");
    api->mute_voices = (gme_mute_voices_fn)GetProcAddress(api->dll, "gme_mute_voices");
    api->set_apu_write_callback =
        (gme_set_apu_write_callback_fn)GetProcAddress(api->dll, "gme_set_apu_write_callback");
    if (!api->set_autoload_playback_limit || !api->mute_voices || !api->set_apu_write_callback || !api->delete_) {
        fprintf(stderr, "gme.dll lacks required patched exports\n");
        return 0;
    }
#undef LOAD
    return 1;
}

static unsigned char *read_all_w(const wchar_t *path, size_t *size_out)
{
    FILE *f = _wfopen(path, L"rb");
    if (!f) {
        return NULL;
    }
    _fseeki64(f, 0, SEEK_END);
    __int64 size = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    if (size <= 0 || size > LONG_MAX) {
        fclose(f);
        return NULL;
    }
    unsigned char *data = (unsigned char *)malloc((size_t)size);
    if (!data) {
        fclose(f);
        return NULL;
    }
    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size_out = (size_t)size;
    return data;
}

static int make_gme_emu(const gme_api_t *gme, const unsigned char *nsf, size_t nsf_size, int sample_rate, int track, Music_Emu **out)
{
    Music_Emu *emu = NULL;
    gme_err_t err = gme->open_data(nsf, (long)nsf_size, &emu, sample_rate);
    if (err) {
        fprintf(stderr, "gme_open_data: %s\n", err);
        return 0;
    }
    gme->set_autoload_playback_limit(emu, 0);
    gme->ignore_silence(emu, 1);
    gme->mute_voices(emu, ~0x1F);
    err = gme->start_track(emu, track);
    if (err) {
        fprintf(stderr, "gme_start_track: %s\n", err);
        gme->delete_(emu);
        return 0;
    }
    *out = emu;
    return 1;
}

static int make_our_emu(const unsigned char *nsf, size_t nsf_size, int sample_rate, int track, nsfemu_t **out)
{
    nsfemu_t *emu = NULL;
    nsfemu_error_t err = nsfemu_create(&emu);
    if (err == NSFEMU_OK) err = nsfemu_set_sample_rate(emu, sample_rate);
    if (err == NSFEMU_OK) err = nsfemu_load_memory(emu, nsf, nsf_size, NSFEMU_LOAD_DEFAULT);
    if (err == NSFEMU_OK) err = nsfemu_start_track(emu, track);
    if (err != NSFEMU_OK) {
        fprintf(stderr, "nsfemu: %s\n", nsfemu_error_string(err));
        nsfemu_destroy(emu);
        return 0;
    }
    *out = emu;
    return 1;
}

static double now_seconds(void)
{
    LARGE_INTEGER freq;
    LARGE_INTEGER counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)freq.QuadPart;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2) {
        fwprintf(stderr, L"usage: nsfemu_compare_gme <file.nsf> [track0] [sample_rate] [seconds]\n");
        return 2;
    }
    int track = argc >= 3 ? _wtoi(argv[2]) : 0;
    int sample_rate = argc >= 4 ? _wtoi(argv[3]) : 44100;
    int seconds = argc >= 5 ? _wtoi(argv[4]) : 5;
    if (seconds <= 0) {
        seconds = 1;
    }

    size_t nsf_size = 0;
    unsigned char *nsf = read_all_w(argv[1], &nsf_size);
    if (!nsf) {
        fwprintf(stderr, L"failed to read %ls\n", argv[1]);
        return 2;
    }

    gme_api_t gme;
    if (!load_gme(&gme)) {
        free(nsf);
        return 2;
    }

    Music_Emu *gme_emu = NULL;
    gme_err_t gme_err = gme.open_data(nsf, (long)nsf_size, &gme_emu, sample_rate);
    if (gme_err) {
        fprintf(stderr, "gme_open_data: %s\n", gme_err);
        free(nsf);
        return 1;
    }
    gme.set_autoload_playback_limit(gme_emu, 0);
    gme.ignore_silence(gme_emu, 1);
    gme.mute_voices(gme_emu, ~0x1F);
    enum { MAX_RECORDS = 200000 };
    write_record_t *gme_records = (write_record_t *)calloc(MAX_RECORDS, sizeof(*gme_records));
    write_record_t *our_records = (write_record_t *)calloc(MAX_RECORDS, sizeof(*our_records));
    write_hash_t gme_writes = {1469598103934665603ull, 0, MAX_RECORDS, gme_records};
    gme.set_apu_write_callback(gme_emu, on_gme_write, &gme_writes);
    gme_err = gme.start_track(gme_emu, track);
    if (gme_err) {
        fprintf(stderr, "gme_start_track: %s\n", gme_err);
        gme.delete_(gme_emu);
        free(nsf);
        return 1;
    }

    nsfemu_t *ours = NULL;
    nsfemu_error_t err = nsfemu_create(&ours);
    if (err == NSFEMU_OK) err = nsfemu_set_sample_rate(ours, sample_rate);
    if (err == NSFEMU_OK) err = nsfemu_load_memory(ours, nsf, nsf_size, NSFEMU_LOAD_DEFAULT);
    write_hash_t our_writes = {1469598103934665603ull, 0, MAX_RECORDS, our_records};
    if (err == NSFEMU_OK) nsfemu_set_apu_write_callback(ours, on_ours_write, &our_writes);
    if (err == NSFEMU_OK) err = nsfemu_start_track(ours, track);
    if (err != NSFEMU_OK) {
        fprintf(stderr, "nsfemu: %s\n", nsfemu_error_string(err));
        nsfemu_destroy(ours);
        gme.delete_(gme_emu);
        free(nsf);
        return 1;
    }

    size_t frames = (size_t)sample_rate * (size_t)seconds;
    size_t bytes = frames * 2 * sizeof(int16_t);
    unsigned char *our_pcm = (unsigned char *)malloc(bytes);
    int16_t *gme_pcm = (int16_t *)malloc(bytes);
    if (!our_pcm || !gme_pcm) {
        fprintf(stderr, "out of memory\n");
        free(our_pcm);
        free(gme_pcm);
        nsfemu_destroy(ours);
        gme.delete_(gme_emu);
        free(nsf);
        return 1;
    }

    double ours_start = now_seconds();
    err = nsfemu_render_s16le(ours, our_pcm, bytes, frames, 2);
    double ours_elapsed = now_seconds() - ours_start;
    double gme_start = now_seconds();
    gme_err = gme.play(gme_emu, (long)(frames * 2), gme_pcm);
    double gme_elapsed = now_seconds() - gme_start;
    if (err != NSFEMU_OK || gme_err) {
        fprintf(stderr, "render error: ours=%s gme=%s\n", nsfemu_error_string(err), gme_err ? gme_err : "ok");
    }

    size_t first_diff = bytes;
    for (size_t i = 0; i < bytes; ++i) {
        if (our_pcm[i] != ((unsigned char *)gme_pcm)[i]) {
            first_diff = i;
            break;
        }
    }

    printf("apu_writes ours=%llu/0x%016llX gme=%llu/0x%016llX %s\n",
        (unsigned long long)our_writes.count,
        (unsigned long long)our_writes.hash,
        (unsigned long long)gme_writes.count,
        (unsigned long long)gme_writes.hash,
        (our_writes.count == gme_writes.count && our_writes.hash == gme_writes.hash) ? "match" : "DIFF");
    if (our_writes.count != gme_writes.count || our_writes.hash != gme_writes.hash) {
        uint64_t n = our_writes.count < gme_writes.count ? our_writes.count : gme_writes.count;
        if (n > MAX_RECORDS) {
            n = MAX_RECORDS;
        }
        for (uint64_t i = 0; i < n; ++i) {
            write_record_t a = our_records[i];
            write_record_t b = gme_records[i];
            if (a.frame != b.frame || a.sequence != b.sequence || a.address != b.address || a.value != b.value) {
                printf("apu first_diff=%llu ours=(f=%d seq=%d addr=0x%04X val=0x%02X) gme=(f=%d seq=%d addr=0x%04X val=0x%02X)\n",
                    (unsigned long long)i,
                    a.frame, a.sequence, a.address, a.value & 0xFF,
                    b.frame, b.sequence, b.address, b.value & 0xFF);
                break;
            }
        }
    }
    if (first_diff == bytes) {
        printf("pcm bytes match (%zu bytes)\n", bytes);
    } else {
        printf("pcm first_diff=%zu ours=0x%02X gme=0x%02X total=%zu\n",
            first_diff, our_pcm[first_diff], ((unsigned char *)gme_pcm)[first_diff], bytes);
    }
    printf("timing ours=%.6fs gme=%.6fs ratio=%.3f\n",
        ours_elapsed, gme_elapsed, gme_elapsed > 0.0 ? ours_elapsed / gme_elapsed : 0.0);

    double our_bench_elapsed = 1.0e100;
    double gme_bench_elapsed = 1.0e100;
    const int bench_iterations = 5;
    for (int i = 0; i < bench_iterations; ++i) {
        Music_Emu *gme_bench = NULL;
        nsfemu_t *our_bench = NULL;
        if (make_gme_emu(&gme, nsf, nsf_size, sample_rate, track, &gme_bench) &&
            make_our_emu(nsf, nsf_size, sample_rate, track, &our_bench)) {
            double our_time;
            double gme_time;
            if ((i & 1) == 0) {
                double our_bench_start = now_seconds();
                (void)nsfemu_render_s16le(our_bench, our_pcm, bytes, frames, 2);
                our_time = now_seconds() - our_bench_start;
                double gme_bench_start = now_seconds();
                (void)gme.play(gme_bench, (long)(frames * 2), gme_pcm);
                gme_time = now_seconds() - gme_bench_start;
            } else {
                double gme_bench_start = now_seconds();
                (void)gme.play(gme_bench, (long)(frames * 2), gme_pcm);
                gme_time = now_seconds() - gme_bench_start;
                double our_bench_start = now_seconds();
                (void)nsfemu_render_s16le(our_bench, our_pcm, bytes, frames, 2);
                our_time = now_seconds() - our_bench_start;
            }
            if (our_time < our_bench_elapsed) {
                our_bench_elapsed = our_time;
            }
            if (gme_time < gme_bench_elapsed) {
                gme_bench_elapsed = gme_time;
            }
        }
        nsfemu_destroy(our_bench);
        if (gme_bench) {
            gme.delete_(gme_bench);
        }
    }
    if (our_bench_elapsed < 1.0e99 && gme_bench_elapsed < 1.0e99) {
        printf("timing_no_callback ours=%.6fs gme=%.6fs ratio=%.3f\n",
            our_bench_elapsed, gme_bench_elapsed,
            gme_bench_elapsed > 0.0 ? our_bench_elapsed / gme_bench_elapsed : 0.0);
    }

    free(our_pcm);
    free(gme_pcm);
    free(our_records);
    free(gme_records);
    nsfemu_destroy(ours);
    gme.delete_(gme_emu);
    FreeLibrary(gme.dll);
    free(nsf);
    return first_diff == bytes &&
        our_writes.count == gme_writes.count &&
        our_writes.hash == gme_writes.hash ? 0 : 1;
}
