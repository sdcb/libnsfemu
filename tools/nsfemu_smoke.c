#include "nsfemu.h"

#include <stdio.h>
#include <stdlib.h>

static unsigned char *read_all(const char *path, size_t *size_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
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

static const char *probe_status_string(nsfemu_length_probe_status_t status)
{
    switch (status) {
    case NSFEMU_LENGTH_PROBE_ENDED: return "ended";
    case NSFEMU_LENGTH_PROBE_ACTIVE_AT_LIMIT: return "active_at_limit";
    case NSFEMU_LENGTH_PROBE_NO_WRITES: return "no_writes";
    default: return "unknown";
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: nsfemu_smoke <file.nsf> [track0] [sample_rate]\n");
        return 2;
    }

    int track = argc >= 3 ? atoi(argv[2]) : 0;
    int sample_rate = argc >= 4 ? atoi(argv[3]) : 44100;
    size_t size = 0;
    unsigned char *data = read_all(argv[1], &size);
    if (!data) {
        fprintf(stderr, "failed to read %s\n", argv[1]);
        return 2;
    }

    nsfemu_t *emu = NULL;
    nsfemu_error_t err = nsfemu_create(&emu);
    if (err == NSFEMU_OK) {
        err = nsfemu_set_sample_rate(emu, sample_rate);
    }
    if (err == NSFEMU_OK) {
        err = nsfemu_load_memory(emu, data, size, NSFEMU_LOAD_DEFAULT);
    }
    if (err == NSFEMU_OK) {
        nsfemu_info_t info;
        err = nsfemu_get_info(emu, &info);
        if (err == NSFEMU_OK) {
            printf("title=%s tracks=%u chip_flags=0x%02X\n", info.title, info.track_count, info.chip_flags);
        }
    }
    if (err == NSFEMU_OK) {
        nsfemu_length_probe_t probe;
        nsfemu_error_t probe_err = nsfemu_probe_length(data, size, track, 60000, &probe);
        if (probe_err == NSFEMU_OK) {
            printf("probe=%s limit_ms=%u last_write_ms=%u frames=%u writes=%u\n",
                probe_status_string(probe.status),
                probe.probe_milliseconds,
                probe.last_write_milliseconds,
                probe.frames_observed,
                probe.writes_observed);
        }
    }
    if (err == NSFEMU_OK) {
        err = nsfemu_start_track(emu, track);
    }
    if (err == NSFEMU_OK) {
        size_t frames = (size_t)sample_rate;
        unsigned char *pcm = (unsigned char *)malloc(frames * 2 * 2);
        if (!pcm) {
            err = NSFEMU_ERROR_OUT_OF_MEMORY;
        } else {
            err = nsfemu_render_s16le(emu, pcm, frames * 2 * 2, frames, 2);
            if (err == NSFEMU_OK) {
                unsigned hash = 2166136261u;
                for (size_t i = 0; i < frames * 2 * 2; ++i) {
                    hash ^= pcm[i];
                    hash *= 16777619u;
                }
                printf("rendered=%zu stereo frames hash=0x%08X\n", frames, hash);
            }
            free(pcm);
        }
    }

    if (err != NSFEMU_OK) {
        fprintf(stderr, "error: %s\n", nsfemu_error_string(err));
    }
    nsfemu_destroy(emu);
    free(data);
    return err == NSFEMU_OK ? 0 : 1;
}
