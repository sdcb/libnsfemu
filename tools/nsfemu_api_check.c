#include "nsfemu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int start_from_memory(nsfemu_t **out, const unsigned char *data, size_t size, int sample_rate)
{
    nsfemu_t *emu = NULL;
    nsfemu_error_t err = nsfemu_create(&emu);
    if (err == NSFEMU_OK) err = nsfemu_set_sample_rate(emu, sample_rate);
    if (err == NSFEMU_OK) err = nsfemu_load_memory(emu, data, size, NSFEMU_LOAD_DEFAULT);
    if (err == NSFEMU_OK) err = nsfemu_start_track(emu, 0);
    if (err != NSFEMU_OK) {
        fprintf(stderr, "start failed: %s\n", nsfemu_error_string(err));
        nsfemu_destroy(emu);
        return 0;
    }
    *out = emu;
    return 1;
}

static void count_apu_write(void *user_data, int frame, int sequence, int address, int value)
{
    (void)frame;
    (void)sequence;
    (void)address;
    (void)value;
    (*(int *)user_data)++;
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

static const char *activity_status_string(nsfemu_activity_status_t status)
{
    switch (status) {
    case NSFEMU_ACTIVITY_NO_WRITES: return "no_writes";
    case NSFEMU_ACTIVITY_ACTIVE: return "active";
    case NSFEMU_ACTIVITY_ENDED: return "ended";
    default: return "unknown";
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: nsfemu_api_check <base-apu.nsf> <expansion-flagged.nsf>\n");
        return 2;
    }

    size_t base_size = 0;
    size_t expansion_size = 0;
    unsigned char *base = read_all(argv[1], &base_size);
    unsigned char *expansion = read_all(argv[2], &expansion_size);
    if (!base || !expansion) {
        fprintf(stderr, "failed to read input files\n");
        free(base);
        free(expansion);
        return 2;
    }

    nsfemu_t *emu = NULL;
    nsfemu_error_t err = nsfemu_create(&emu);
    if (err != NSFEMU_OK || nsfemu_sample_rate(emu) != 44100) {
        fprintf(stderr, "default sample rate check failed\n");
        return 1;
    }
    if (nsfemu_set_sample_rate(emu, 7999) != NSFEMU_ERROR_UNSUPPORTED_SAMPLE_RATE ||
        nsfemu_set_sample_rate(emu, 192001) != NSFEMU_ERROR_UNSUPPORTED_SAMPLE_RATE ||
        nsfemu_set_sample_rate(emu, 192000) != NSFEMU_OK) {
        fprintf(stderr, "sample rate range check failed\n");
        return 1;
    }
    nsfemu_destroy(emu);

    if (nsfemu_create(&emu) != NSFEMU_OK) {
        return 1;
    }
    err = nsfemu_load_memory(emu, expansion, expansion_size, NSFEMU_LOAD_REJECT_UNSUPPORTED_EXPANSION);
    nsfemu_destroy(emu);
    if (err != NSFEMU_ERROR_UNSUPPORTED_EXPANSION) {
        fprintf(stderr, "unsupported expansion reject check failed: %s\n", nsfemu_error_string(err));
        return 1;
    }

    nsfemu_t *mono = NULL;
    nsfemu_t *stereo = NULL;
    if (!start_from_memory(&mono, base, base_size, 44100) ||
        !start_from_memory(&stereo, base, base_size, 44100)) {
        return 1;
    }
    short mono_pcm[1024];
    short stereo_pcm[2048];
    err = nsfemu_render_s16le(mono, mono_pcm, sizeof(mono_pcm), 1024, 1);
    if (err == NSFEMU_OK) {
        err = nsfemu_render_s16le(stereo, stereo_pcm, sizeof(stereo_pcm), 1024, 2);
    }
    if (err != NSFEMU_OK) {
        fprintf(stderr, "render check failed: %s\n", nsfemu_error_string(err));
        return 1;
    }
    for (int i = 0; i < 1024; ++i) {
        if (stereo_pcm[i * 2] != stereo_pcm[i * 2 + 1] || stereo_pcm[i * 2] != mono_pcm[i]) {
            fprintf(stderr, "mono/stereo duplication check failed at %d\n", i);
            return 1;
        }
    }

    nsfemu_activity_t activity;
    err = nsfemu_get_activity(mono, 180, &activity);
    if (err != NSFEMU_OK || activity.status != NSFEMU_ACTIVITY_NO_WRITES || activity.writes_observed != 0) {
        fprintf(stderr, "default activity tracking check failed: %s status=%s writes=%u\n",
            nsfemu_error_string(err),
            err == NSFEMU_OK ? activity_status_string(activity.status) : "error",
            err == NSFEMU_OK ? activity.writes_observed : 0);
        return 1;
    }
    nsfemu_destroy(mono);
    nsfemu_destroy(stereo);

    nsfemu_t *tracked = NULL;
    if (!start_from_memory(&tracked, base, base_size, 44100)) {
        return 1;
    }
    if (nsfemu_activity_tracking(tracked) != 0) {
        fprintf(stderr, "activity tracking default flag check failed\n");
        return 1;
    }
    nsfemu_set_activity_tracking(tracked, 1);
    if (nsfemu_activity_tracking(tracked) == 0) {
        fprintf(stderr, "activity tracking enable check failed\n");
        return 1;
    }
    for (int pass = 0; pass < 60; ++pass) {
        err = nsfemu_render_s16le(tracked, stereo_pcm, sizeof(stereo_pcm), 1024, 2);
        if (err != NSFEMU_OK) {
            fprintf(stderr, "tracked render failed: %s\n", nsfemu_error_string(err));
            return 1;
        }
    }
    err = nsfemu_get_activity(tracked, 180, &activity);
    if (err != NSFEMU_OK ||
        activity.status == NSFEMU_ACTIVITY_NO_WRITES ||
        activity.writes_observed == 0 ||
        activity.current_play_frame == 0) {
        fprintf(stderr, "runtime activity tracking check failed: %s status=%s frame=%u last=%u writes=%u\n",
            nsfemu_error_string(err),
            err == NSFEMU_OK ? activity_status_string(activity.status) : "error",
            err == NSFEMU_OK ? activity.current_play_frame : 0,
            err == NSFEMU_OK ? activity.last_write_play_frame : 0,
            err == NSFEMU_OK ? activity.writes_observed : 0);
        return 1;
    }
    nsfemu_set_activity_tracking(tracked, 0);
    if (nsfemu_activity_tracking(tracked) != 0) {
        fprintf(stderr, "activity tracking disable check failed\n");
        return 1;
    }
    nsfemu_destroy(tracked);

    nsfemu_t *muted = NULL;
    if (!start_from_memory(&muted, base, base_size, 44100)) {
        return 1;
    }
    nsfemu_set_channel_mask(muted, 0);
    memset(stereo_pcm, 0x7F, sizeof(stereo_pcm));
    err = nsfemu_render_s16le(muted, stereo_pcm, sizeof(stereo_pcm), 1024, 2);
    if (err != NSFEMU_OK) {
        fprintf(stderr, "muted render failed: %s\n", nsfemu_error_string(err));
        return 1;
    }
    for (int i = 0; i < 2048; ++i) {
        if (stereo_pcm[i] != 0) {
            fprintf(stderr, "muted output check failed at %d\n", i);
            return 1;
        }
    }
    nsfemu_destroy(muted);

    if (nsfemu_create(&muted) != NSFEMU_OK) {
        return 1;
    }
    err = nsfemu_set_sample_rate(muted, 44100);
    if (err == NSFEMU_OK) err = nsfemu_load_memory(muted, base, base_size, NSFEMU_LOAD_DEFAULT);
    int muted_write_count = 0;
    if (err == NSFEMU_OK) nsfemu_set_apu_write_callback(muted, count_apu_write, &muted_write_count);
    if (err == NSFEMU_OK) err = nsfemu_start_track(muted, 0);
    if (err == NSFEMU_OK) {
        nsfemu_set_channel_mask(muted, 0);
        memset(stereo_pcm, 0, sizeof(stereo_pcm));
        err = nsfemu_render_s16le(muted, stereo_pcm, sizeof(stereo_pcm), 1024, 2);
    }
    nsfemu_destroy(muted);
    if (err != NSFEMU_OK || muted_write_count <= 0) {
        fprintf(stderr, "muted write-log preservation check failed\n");
        return 1;
    }

    nsfemu_length_probe_t probe;
    err = nsfemu_probe_length(base, base_size, 0, 60000, &probe);
    if (err != NSFEMU_OK || probe.frames_observed == 0) {
        fprintf(stderr, "length probe check failed: %s\n", nsfemu_error_string(err));
        return 1;
    }

    printf("api checks passed; probe=%s limit_ms=%u last_write_ms=%u frames=%u writes=%u\n",
        probe_status_string(probe.status),
        probe.probe_milliseconds,
        probe.last_write_milliseconds,
        probe.frames_observed,
        probe.writes_observed);

    free(base);
    free(expansion);
    return 0;
}
