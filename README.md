# TTPlayer Matroska 音频插件

独立 x86 `ttp_mkv.dll`，通过原版 Reader ABI 为播放器增加 `.mkv` / `.mka`
音频播放。只解码选定音轨，不显示视频、不加载视频解码器、不启动外部进程，
不依赖系统 MF/DirectShow 或其它音频插件。

## 安装和范围

关闭播放器，把运行包的 `AddIn/ttp_mkv.dll` 放入安装目录的 `AddIn`。
重建版按内容 Probe 路由；原版可按 MKV/MKA 后缀选择 Reader。

- 第一阶段：PCM（8/16/24/32-bit 整数、float32/64）、MPEG Layer I/II/III、AAC、AC3、DTS Core。
- 第二阶段：Opus、Vorbis、FLAC、ALAC、E-AC3、DTS-HD MA/HRA、DTS Express。
- 优先选择受支持且启用的默认音轨，否则第一个受支持且启用音轨。
- 原生解复用处理 SimpleBlock/BlockGroup、合帧、CodecPrivate、CodecDelay、DiscardPadding 和头移除。
- 输出保持解码器的真实整数/浮点类型，不把无损整数经 float32 转换；格式和有效位与字节一致。
- Unicode 标签、所选音轨覆盖全局标签、PNG/JPEG 附件封面读取；不提供标签/封面写入。
- 可定位、长度已知的 IStream（本地、归档成员及宿主提供的有限网络流）；短读与取消保留错误。

**定位策略：** 重建解复用器和解码器，从所选音轨起点预解码并丢弃至目标采样帧。
此策略覆盖所需预卷，重复定位与顺序解码一致，不依赖 Cues；长文件远距离定位会有明显开销。
不声明快速 Cues 定位、直播/增长流、加密、手动切轨、TrueHD/MLP、WMA 或任意 Matroska 编码。
原生 PCM 的非整字节位深（如 BitDepth=20）明确拒绝，避免上游回退为 PCM16；这不影响 ALAC/FLAC 的有效位支持。
音轨从首个可听样本归零，保留超过时间戳舍入容差的内部间隙。

## 构建与许可证

```powershell
./build.ps1 -Version 2026.11.12 -Package -SourcePackage
```

需要 VS 2026、CMake、Python 3.12+、Git for Windows Bash。固定 FFmpeg 9.0.2
静态裁剪后端，VC-LTL/YY-Thunks 保留 XP/Win7 目标（SSE2）；导入审计不等于旧系统实测。
构建与测试证据另见重建版 `docs/MATROSKA_AUDIO_20261112.md`，不把测试或样本放入本插件仓库。

运行包只包含 DLL 和 SHA256；另附完整对应源码包。MIT 适配代码与 LGPL 后端的
许可证不可混淆，依赖来源、补丁、重链接条件见 [third_party/ORIGIN.md](third_party/ORIGIN.md)。

独立仓库：[TTPlayerRebuild/TTPlayerMKV](https://github.com/TTPlayerRebuild/TTPlayerMKV)。播放器的 CodecAddIn 合集和统一 DLL 发布请求已登记本仓库；需先将接收工作流推送到默认分支并发布首次正式运行包，合集才能下载。

手动构建默认不启用条件检查；统一 `release-dlls` 入口同时传入 `release_a_version=true`、`only_if_unreleased=true`，仅在默认分支有未发布提交（或尚无正式 Release）时继续构建，已发布提交则跳过。该检查在既有发布并发组内执行，不改变 DLL 的构建、命名或附件规则。
