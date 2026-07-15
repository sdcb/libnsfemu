# libnsfemu [![QQ](https://img.shields.io/badge/QQ_Group-495782587-52B6EF?style=social&logo=tencent-qq&logoColor=000&logoWidth=20)](http://qm.qq.com/cgi-bin/qm/qr?_wv=1027&k=mma4msRKd372Z6dWpmBp4JZ9RL4Jrf8X&authKey=gccTx0h0RaH5b8B8jtuPJocU7MgFRUznqbV%2FLgsKdsK8RqZE%2BOhnETQ7nYVTp1W0&noverify=0&group_code=495782587)

[English](README.md) | 简体中文

一个小型 C 语言 NSF 播放库。

- 纯 C 实现的 NSF 播放核心。
- 从内存加载 NSF，无文件 I/O 依赖。
- 6502 CPU 模拟器。
- 基础 NES 2A03 APU 模拟：方波 1/2、三角波、噪声和 DMC。
- 输出 8000..192000 Hz 的有符号 16 位小端 PCM。
- 支持单声道输出，也支持将单声道复制到 L/R 的立体声输出。
- 支持按声道设置输出掩码。静音只作用于 APU 声音输出；CPU 执行和 APU 写入回调仍会继续运行。
- 默认有意忽略扩展音频，也可以通过 `NSFEMU_ERROR_UNSUPPORTED_EXPANSION` 拒绝扩展音频。
- 在已测试的 NSF 语料上与 libgme 位级一致。
- 与打过补丁的 libgme 相比，基础 APU 寄存器写入日志零差异。
- 在已测试采样率下 PCM 输出零差异。
- 使用 MSVC 构建的 x64 Release DLL 约 39KB。
- 在聚合基准测试语料上略快于 libgme。
- 通过 `nsfemu_probe_length()` 在不渲染音频的情况下尽力探测曲目时长 / 音乐与音效。
- 可通过 `nsfemu_set_activity_tracking()` 和 `nsfemu_get_activity()` 启用运行时 APU 写入活动跟踪。

## 来源

libnsfemu 是从 Game_Music_Emu/libgme 的 NSF 播放实现派生而来的 C 移植版，
并裁剪为一个小型、仅支持基础 NES APU 的库接口。

## 从 libgme C API 迁移

libnsfemu 的播放流程与 libgme 接近，但只暴露本库需要的 NSF 子集。常见的
libgme 流程：

```c
Music_Emu *emu = NULL;
gme_open_data(data, size, &emu, sample_rate);
gme_start_track(emu, track);
gme_play(emu, frame_count * 2, pcm);
gme_delete(emu);
```

对应到：

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

如果要检测曲目在正常播放过程中变安静，可以在 `nsfemu_start_track()` 前后启用
活动跟踪，正常渲染 PCM，然后在每个渲染块之后查询活动状态：

```c
nsfemu_set_activity_tracking(emu, 1);
nsfemu_render_s16le(emu, pcm, frame_count * 2 * sizeof(int16_t),
                    frame_count, 2);

nsfemu_activity_t activity;
nsfemu_get_activity(emu, 180, &activity);
if (activity.status == NSFEMU_ACTIVITY_ENDED) {
    /* 停止播放，或将 UI 切回播放状态。 */
}
```

`gme_play()` 接收的是 16 位输出采样数量，并且 libgme 播放为立体声，因此渲染
`N` 个立体声帧时需要传入 `N * 2`。`nsfemu_render_s16le()` 接收帧数和显式
声道数；可以用 `frame_count = 1` 渲染一帧，也可以用更大的数量渲染一个完整块。
与 `gme_open_data()` 和 `gme_load_data()` 一样，`nsfemu_load_memory()` 会复制
输入的 NSF 字节，因此成功加载后调用方可以释放原始缓冲区。

| libgme C API | libnsfemu C API | 说明 |
| --- | --- | --- |
| `gme_open_data(data, size, &emu, rate)` | `nsfemu_create()`, `nsfemu_set_sample_rate()`, `nsfemu_load_memory()` | 仅支持内存输入；NSF 字节会被复制。 |
| `gme_new_emu(gme_nsf_type, rate)`, `gme_load_data()` | `nsfemu_create()`, `nsfemu_set_sample_rate()`, `nsfemu_load_memory()` | libnsfemu 仅支持 NSF。 |
| `gme_open_file()`, `gme_load_file()` | 调用方读取文件，然后调用 `nsfemu_load_memory()` | libnsfemu 没有文件 I/O 依赖。 |
| `gme_delete(emu)` | `nsfemu_destroy(emu)` | 释放模拟器。 |
| `gme_start_track(emu, track)` | `nsfemu_start_track(emu, track)` | 两个 API 的曲目索引都是从 0 开始。 |
| `gme_play(emu, sample_count, pcm)` | `nsfemu_render_s16le(emu, pcm, out_size, frame_count, channels)` | libgme 计数单位是采样；libnsfemu 计数单位是帧。 |
| `gme_track_count(emu)` | `nsfemu_get_info(emu, &info)` 后读取 `info.track_count` | 头部元数据会复制到 `nsfemu_info_t`。 |
| `gme_track_info()` | `nsfemu_get_info()` 和 `nsfemu_probe_length()` | 没有 M3U/NSFE 元数据解析器；调用方自行选择以毫秒为单位的探测时长。 |
| `gme_mute_voice()`, `gme_mute_voices()` | `nsfemu_set_channel_mask()` | libgme 的掩码位表示静音声部；libnsfemu 的掩码位表示启用声部。 |
| `gme_err_t` 字符串或 `NULL` | `nsfemu_error_t` 加 `nsfemu_error_string()` | 错误是枚举值。 |
| `gme_ignore_silence()`, `gme_set_fade()`, `gme_track_ended()` | 无直接对应项 | 播放是显式的；调用方按需渲染任意帧数。 |
| `gme_seek()`, `gme_set_tempo()`, `gme_set_equalizer()` | 无直接对应项 | 不属于这个小型 NSF 专用 API。 |
| `gme_voice_count()`, `gme_voice_name()` | `NSFEMU_CHANNEL_*` 常量 | 固定的基础 APU 声道：方波 1/2、三角波、噪声、DMC。 |

## 构建

```bat
call "C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
cmake -S . -B build -G Ninja
cmake --build build
```

如需性能测量，请使用 Release 构建：

```bat
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
```

## 冒烟测试

```bat
build\nsfemu_smoke.exe "local\nsf\Contra JP [Probotector] (1988-02-09)(Konami).nsf" 0 44100
```

如果配置时存在 `local\nsf`，CTest 还会包含 API 覆盖测试和本地 libgme 对比门禁：

```bat
ctest --test-dir build-release --output-on-failure
```

## libgme 对比

`nsfemu_compare_gme` 会从以下路径动态加载打过补丁的 `gme.dll`：

```text
C:\_\3rd\vcpkg\installed\x64-windows\bin\gme.dll
```

它会将 NSF 字节加载到两个引擎中，禁用 libgme 的播放限制和静音跳过，静音 libgme
扩展音频声部，对比主 APU 写入日志，并对比立体声 PCM 字节。

```bat
build\nsfemu_compare_gme.exe "local\nsf\Contra JP [Probotector] (1988-02-09)(Konami).nsf" 0 44100 2
```

最后一个参数是渲染时长，单位为秒。

运行完整的本地正确性门禁，覆盖全部 10 个 NSF 夹具和所需采样率：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\run_local_compare.ps1 build-release\nsfemu_compare_gme.exe local\nsf 2
```

运行针对 libgme 的聚合无回调性能对比：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\run_local_perf.ps1 build-release\nsfemu_compare_gme.exe local\nsf 44100 120
```

## 许可证

libnsfemu 使用 GNU Lesser General Public License version 2.1 or later
（`LGPL-2.1-or-later`）授权。详见 [LICENSE](LICENSE)。
