# libnsfemu [![QQ](https://img.shields.io/badge/QQ_Group-495782587-52B6EF?style=social&logo=tencent-qq&logoColor=000&logoWidth=20)](http://qm.qq.com/cgi-bin/qm/qr?_wv=1027&k=mma4msRKd372Z6dWpmBp4JZ9RL4Jrf8X&authKey=gccTx0h0RaH5b8B8jtuPJocU7MgFRUznqbV%2FLgsKdsK8RqZE%2BOhnETQ7nYVTp1W0&noverify=0&group_code=495782587)

English | [简体中文](README.zh-CN.md)

A tiny C NSF playback library.

- NSF playback core in plain C.
- In-memory NSF loading, no file I/O dependency.
- 6502 CPU emulator.
- Base NES 2A03 APU emulation: pulse 1/2, triangle, noise, and DMC.
- Signed 16-bit little-endian PCM output at 8000..192000 Hz.
- Mono output, or stereo by duplicating mono to L/R.
- Per-channel output mask. Muting is applied only at APU sound output; CPU execution and APU write callbacks still run.
- Expansion audio is intentionally ignored by default, or rejected with `NSFEMU_ERROR_UNSUPPORTED_EXPANSION`.
- Bit-exact with libgme on the tested NSF corpus.
- Zero-difference base APU register write logs against patched libgme.
- Zero-difference PCM output across tested sample rates.
- Around 39KB x64 Release DLL with MSVC.
- Slightly faster than libgme on the aggregate benchmark corpus.
- Best-effort track duration / music-vs-sfx probing without audio rendering via `nsfemu_probe_length()`.
- Optional runtime APU-write activity tracking via `nsfemu_set_activity_tracking()` and `nsfemu_get_activity()`.

## Origin

libnsfemu is a C port derived from Game_Music_Emu/libgme's NSF playback
implementation, reduced to a small base-NES-APU-only library interface.

## Migrating from libgme C API

libnsfemu keeps the playback flow close to libgme, but exposes only the NSF
subset needed by this library. The usual libgme flow:

```c
Music_Emu *emu = NULL;
gme_open_data(data, size, &emu, sample_rate);
gme_start_track(emu, track);
gme_play(emu, frame_count * 2, pcm);
gme_delete(emu);
```

maps to:

```c
nsfemu_t *emu = NULL;
nsfemu_create(&emu);
nsfemu_set_sample_rate(emu, sample_rate);
nsfemu_load_memory(emu, data, size, NSFEMU_LOAD_DEFAULT);
nsfemu_start_track(emu, track);
nsfemu_render_s16le(emu, pcm, frame_count * 2 * sizeof(int16_t),
                    frame_count, 2);
nsfemu_destroy(emu);
```

To detect tracks that become quiet during normal playback, enable activity
tracking before or after `nsfemu_start_track()`, render PCM normally, then query
activity after each render block:

```c
nsfemu_set_activity_tracking(emu, 1);
nsfemu_render_s16le(emu, pcm, frame_count * 2 * sizeof(int16_t),
                    frame_count, 2);

nsfemu_activity_t activity;
nsfemu_get_activity(emu, 180, &activity);
if (activity.status == NSFEMU_ACTIVITY_ENDED) {
    /* Stop playback or switch UI back to Play. */
}
```

`gme_play()` takes a count of 16-bit output samples and libgme playback is
stereo, so render `N` stereo frames by passing `N * 2`. `nsfemu_render_s16le()`
takes a frame count plus an explicit channel count; render one frame with
`frame_count = 1`, or render a whole block with any larger count. Like
`gme_open_data()` and `gme_load_data()`, `nsfemu_load_memory()` copies the input
NSF bytes, so the caller may release its original buffer after a successful load.

| libgme C API | libnsfemu C API | Notes |
| --- | --- | --- |
| `gme_open_data(data, size, &emu, rate)` | `nsfemu_create()`, `nsfemu_set_sample_rate()`, `nsfemu_load_memory()` | Memory input only; NSF bytes are copied. |
| `gme_new_emu(gme_nsf_type, rate)`, `gme_load_data()` | `nsfemu_create()`, `nsfemu_set_sample_rate()`, `nsfemu_load_memory()` | libnsfemu supports NSF only. |
| `gme_open_file()`, `gme_load_file()` | Caller reads the file, then `nsfemu_load_memory()` | libnsfemu has no file I/O dependency. |
| `gme_delete(emu)` | `nsfemu_destroy(emu)` | Frees the emulator. |
| `gme_start_track(emu, track)` | `nsfemu_start_track(emu, track)` | Track index is zero-based in both APIs. |
| `gme_play(emu, sample_count, pcm)` | `nsfemu_render_s16le(emu, pcm, out_size, frame_count, channels)` | libgme count is samples; libnsfemu count is frames. |
| `gme_track_count(emu)` | `nsfemu_get_info(emu, &info)` then `info.track_count` | Header metadata is copied into `nsfemu_info_t`. |
| `gme_track_info()` | `nsfemu_get_info()` and `nsfemu_probe_length()` | No M3U/NSFE metadata parser; callers choose a probe duration in milliseconds. |
| `gme_mute_voice()`, `gme_mute_voices()` | `nsfemu_set_channel_mask()` | libgme mask bits mute voices; libnsfemu mask bits enable voices. |
| `gme_err_t` string or `NULL` | `nsfemu_error_t` plus `nsfemu_error_string()` | Errors are enum values. |
| `gme_ignore_silence()`, `gme_set_fade()`, `gme_track_ended()` | No direct equivalent | Playback is explicit; render as many frames as the caller needs. |
| `gme_seek()`, `gme_set_tempo()`, `gme_set_equalizer()` | No direct equivalent | Not part of the small NSF-only API. |
| `gme_voice_count()`, `gme_voice_name()` | `NSFEMU_CHANNEL_*` constants | Fixed base APU channels: pulse 1/2, triangle, noise, DMC. |

## Build

```bat
call "C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
cmake -S . -B build -G Ninja
cmake --build build
```

For performance measurements, use a Release build:

```bat
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
```

## Smoke Test

```bat
build\nsfemu_smoke.exe "local\nsf\Contra JP [Probotector] (1988-02-09)(Konami).nsf" 0 44100
```

If `local\nsf` is present at configure time, CTest also includes API coverage and
the local libgme comparison gate:

```bat
ctest --test-dir build-release --output-on-failure
```

## libgme Comparison

`nsfemu_compare_gme` dynamically loads the patched `gme.dll` from:

```text
C:\_\3rd\vcpkg\installed\x64-windows\bin\gme.dll
```

It loads NSF bytes into both engines, disables libgme playback limits and silence skipping, mutes libgme expansion voices, compares main APU write logs, and compares stereo PCM bytes.

```bat
build\nsfemu_compare_gme.exe "local\nsf\Contra JP [Probotector] (1988-02-09)(Konami).nsf" 0 44100 2
```

The final argument is render duration in seconds.

To run the full local correctness gate over all 10 NSF fixtures and the required
sample rates:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\run_local_compare.ps1 build-release\nsfemu_compare_gme.exe local\nsf 2
```

To run an aggregate no-callback performance comparison against libgme:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\run_local_perf.ps1 build-release\nsfemu_compare_gme.exe local\nsf 44100 120
```

## License

libnsfemu is licensed under the GNU Lesser General Public License version 2.1
or later (`LGPL-2.1-or-later`). See [LICENSE](LICENSE).
