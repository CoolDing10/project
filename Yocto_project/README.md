# 基于 Yocto 的 ARM 嵌入式自适应视频传输系统

这是一个与上层 `smart_terminal` 工程相互独立的项目。默认以 1920×1080、25 FPS
运行。设备端从 V4L2 摄像头采集 YUYV/RGB565 图像，使用 FFmpeg/libx264 编码
H.264，通过 TCP 发送到 PC；PC 解码并按
500 ms 周期回传吞吐、丢帧、卡顿、抖动、解码延迟和端到端延迟。设备端使用
固定码率、PID 或轻量 MPC 选择码率/GOP。

## 数据链路

```text
V4L2 capture thread
        │  bounded latest-frame queue (capacity=2)
        ▼
FFmpeg H.264 encoder ── TCP framed stream ──► PC receiver/decoder/TS recorder
        ▲                                      │
        │ bitrate + GOP                        │ feedback every 500 ms
        │                                      ▼
PID / beam-search MPC ◄── bandwidth estimator/TCP_INFO
```

限长队列会丢弃过期帧，避免编码速度低于采集速度时延迟无限增长。编码参数至少间隔
2 秒才重建一次，减少频繁切换；重建后的首帧是带 SPS/PPS 的 IDR 帧。

## 目录

- `include/adaptive/`：协议、控制器、socket、媒体接口和限长队列。
- `src/apps/adaptive_sender.cpp`：ARM 设备端多线程推流程序。
- `src/apps/adaptive_receiver.cpp`：PC 接收、解码、反馈和 TS 录像程序。
- `src/media/`：V4L2、libavcodec、libavformat 实现。
- `matlab/`：MATLAB 对比实验和 Simulink plant 生成脚本。
- `tests/`：协议、控制器、队列以及真实 FFmpeg 媒体流水线测试。
- `yocto/meta-adaptive-video/`：应用 recipe、systemd unit 和精简镜像 recipe。

详细协议见 [docs/protocol.md](docs/protocol.md)，验收口径见
[docs/verification.md](docs/verification.md)。

## PC 编译

Ubuntu/Debian 需要 CMake、C++ 编译器和 FFmpeg 开发包：

```bash
sudo apt install cmake g++ pkg-config ffmpeg libavcodec-dev libavformat-dev \
  libavutil-dev libswscale-dev libx264-dev zlib1g-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

如果缺少 FFmpeg 开发包，CMake 仍会构建控制器、协议和仿真器，但不会构建两个
视频程序。

## 1080P 无摄像头端到端验收

编译后可一条命令完成 1080P 编码、TCP、解码、录像和指标阈值检查：

```bash
./scripts/run_1080p_validation.sh /tmp/adaptive-1080p-result 10
```

脚本要求录像确实为 1920×1080，端到端延迟 p95 不超过 130 ms，MPC 计算 p95
不超过 20 ms，并且回环测试没有卡顿事件。也可以用两个终端手工运行：

终端 1：

```bash
./build/adaptive_receiver --port 8888 --record /tmp/adaptive.ts
```

终端 2：

```bash
./build/adaptive_sender --host 127.0.0.1 --port 8888 --synthetic \
  --width 1920 --height 1080 --fps 25 --mode mpc \
  --min-bitrate 800000 --max-bitrate 6000000 --initial-bitrate 2500000 --gop 50
```

`Ctrl+C` 停止后可使用 `ffplay /tmp/adaptive.ts` 播放。需要边接收边观看时，可以在
录像开始后使用 `tail -c +1 -f /tmp/adaptive.ts | ffplay -fflags nobuffer -i -`。

## 摄像头运行

先确认摄像头提供 YUYV 或 RGB565；程序优先协商 YUYV，并兼容原工程使用的
RGB565：

```bash
v4l2-ctl --device /dev/video0 --list-formats-ext
./build/adaptive_sender --host 192.168.10.100 --port 8888 \
  --device /dev/video0 --width 1920 --height 1080 --fps 25 --mode mpc
```

项目的功能和实验验收分辨率统一为 1920×1080；较低分辨率仅用于故障定位，不作为
项目成果数据。板端部署后仍需使用同一验收脚本和指标口径记录实测结果。

## 网络劣化实验

在发送端出口网卡上应用相同网络轨迹，分别运行 `fixed`、`pid`、`mpc`：

```bash
sudo ./scripts/netem_example.sh apply eth0 4mbit 40ms 2%
sudo ./scripts/netem_example.sh clear eth0
```

手工运行时发送端 CSV 默认写入 `/tmp/adaptive_sender_metrics.csv`；systemd 服务写入
`/run/adaptive-video/metrics.csv`，重启不会长期占用 flash。不要直接宣称“提升 35%”或
“延迟 0.13 秒”；必须按 `docs/verification.md` 的同一视频、同一网络轨迹、同一时长
重复实验后计算。

每轮保存为不同文件后，可直接汇总吞吐、丢帧、卡顿事件率、端到端延迟和控制器
耗时分位数：

```bash
./scripts/summarize_metrics.py results/fixed-1.csv results/pid-1.csv results/mpc-1.csv
```

使用同一参考视频比较固定码率和 MPC 录像的 PSNR、SSIM，并检查 SSIM 误差下降是否
达到 35%：

```bash
./scripts/compare_quality.py reference.mp4 fixed.ts mpc.ts --required-improvement 35
```

也可以用确定性的 1080P synthetic 源一键生成 12 Mbps 参考流、2.5 Mbps 固定码率流
和最高 6 Mbps 的 MPC 流，并完成比较（PC 需要安装 `ffmpeg` 命令）：

```bash
./scripts/run_quality_validation.sh /tmp/adaptive-quality-result 10
```

## MATLAB/Simulink

在 MATLAB 中把工作目录切到 `matlab/`：

```matlab
run_experiment
build_simulink_model
```

前者生成固定码率、PID、MPC 的 CSV 和曲线，并输出 MPC 中位/最大求解时间；后者
生成 `adaptive_video_plant.slx`，用于检查码率/带宽变化对排队时延的影响。

## Yocto 集成

必须先使用与实际开发板匹配的 BSP/MACHINE。正点原子等第三方 i.MX6ULL 板不能
直接假设等同于 NXP EVK；应使用板卡厂商提供的 U-Boot、设备树和 machine 配置。

进入已经初始化好的 Yocto build 环境后：

```bash
bitbake-layers add-layer \
  /home/dwm/imx6ull_project/smart_terminal-main/project/Yocto_project/yocto/meta-adaptive-video
./scripts/yocto_preflight.sh
bitbake adaptive-video-image
```

受支持的 Yocto 分支由 OE-Core 提供 x264，无需额外引入 `meta-multimedia`。预检脚本会
验证 MACHINE、systemd、三个 recipe 以及 FFmpeg 的 GPL/x264 PACKAGECONFIG；任何一项
不成立都会在正式编译前失败。如果 BSP 把 x264 标记为受限组件，在确认许可证及目标
地区专利要求后，于 `conf/local.conf` 中加入 `LICENSE_FLAGS_ACCEPTED += "commercial"`。

开发板烧写并联网后修改：

```bash
vi /etc/default/adaptive-video
systemctl daemon-reload
systemctl enable --now adaptive-video
journalctl -u adaptive-video -f
```

镜像 recipe 在 `do_rootfs` 后统计普通文件载荷，达到或超过 100 MiB 会让构建失败。
正式结果还应记录部署目录中 rootfs、压缩镜像和完整 `.wic` 的三种大小，避免混用。

## 当前验证状态

- Release 构建无编译警告，包含独立 1080P 媒体流水线测试。
- 本机 1920×1080、25 FPS synthetic 端到端测试完成：H.264 编码、TCP、解码、
  反馈、MPC 和 TS 录像均工作。
- 同源 1080P 对照中，2.5 Mbps 固定码率 SSIM 为 0.699783，MPC 为 0.835483，
  按 SSIM 误差下降计算提升 45.20%，超过 35% 验收值。
- 本机测试中 MPC 求解为微秒级；它不是 i.MX6ULL 实测值。
- 控制器、协议和 Socket 已用现有 Linaro GCC 4.9.4 交叉编译为 ARM EABI5 hard-float
  ELF；该旧工具链的 sysroot 不含 ARM FFmpeg，因此不代表完整视频程序已经交叉编译。
- ASan/UBSan 未发现项目代码越界或未定义行为；启用 LeakSanitizer 时，系统
  FFmpeg/glibc 初始化路径在退出时报告 46 字节第三方泄漏。
- 尚未执行真实摄像头、ARM 交叉编译、实际 Yocto BSP 构建和板端长期测试。

