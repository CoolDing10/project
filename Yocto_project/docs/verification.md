# 验收与数据口径

## 功能验收

1. `ctest` 五项全部通过，其中 `fullhd_pipeline_test` 必须完成 1920×1080、25 FPS
   编码、解码和 MPEG-TS 封装。
2. PC 先启动 receiver，设备断网启动 sender；恢复网络后 2 秒内自动连接。
3. 运行中断网再恢复，接收端从新 IDR 恢复，不崩溃、不无限积压。
4. `systemctl status adaptive-video` 为 active，杀死进程后 systemd 自动拉起。
5. 摄像头断开、PC 关闭、非法协议数据均不会导致进程越界或永久阻塞。

## 指标定义

- 端到端延迟：`接收完成时间 - 采集时间戳`。两端先使用 chrony/NTP 同步；另用
  画面计时器加高速摄像机抽样校验。
- 卡顿事件：相邻完整视频消息到达间隔超过 `max(200 ms, 3/FPS)`。
- 卡顿率：累计卡顿时长 / 总播放时长。仅统计事件数量不足以支撑简历结论。
- 丢帧率：视频序号缺口 /（已接收帧 + 序号缺口）。
- 控制耗时：控制器 `update()` 的 wall-clock 时间，报告 p50、p95、p99、最大值。
- 画质：相同源视频和网络轨迹下，用 PSNR/SSIM（可再加 VMAF）比较接收视频；
  “提升百分比”必须写清所用指标和计算公式。

## 对照实验

每种网络轨迹分别测试 fixed、PID、MPC，每组至少重复 5 次：

| 轨迹 | 带宽 | 延迟/抖动 | 丢包 |
|---|---|---|---|
| 稳定 | 8 Mbps | 20±5 ms | 0% |
| 阶跃 | 8 Mbps→1.5 Mbps→5 Mbps | 40±10 ms | 1% |
| 波动 | 1–6 Mbps | 60±30 ms | 2–5% |

固定源、分辨率、FPS、测试时长和随机种子。输出原始 CSV、汇总表和脚本版本；只有
实测达到要求后，才能在简历中写 `<100 MB`、`0.13 s`、`0.02 s` 或 `35%`。

每轮通过 `--metrics results/<mode>-<run>.csv` 单独保存，再执行：

```bash
./scripts/summarize_metrics.py results/*.csv
```

1080P 回环验收可直接运行；脚本强制检查分辨率、端到端 p95 小于 130 ms、控制计算
p95 小于 20 ms且无卡顿事件：

```bash
./scripts/run_1080p_validation.sh /tmp/adaptive-1080p-result 10
```

固定码率与 MPC 使用同一个参考视频、网络轨迹和起止帧录制完成后，执行画质验收：

```bash
./scripts/compare_quality.py reference.mp4 fixed.ts mpc.ts --required-improvement 35
```

没有现成参考文件时，可用确定性 synthetic 源完成可重复的三组 1080P 对照：

```bash
./scripts/run_quality_validation.sh /tmp/adaptive-quality-result 10
```

这里的“提升 35%”明确定义为 SSIM 误差 `(1-SSIM)` 的下降比例，同时输出 PSNR，
避免把 SSIM 数值直接相除得到没有清晰含义的百分比。

脚本输出的是“卡顿事件/分钟”，不是卡顿时长占比。若要声称“卡顿率下降”，还需从
接收端播放器记录实际冻结时长，并按“冻结总时长 / 播放总时长”计算。

## 长稳测试

- 板端连续运行 24 小时起步，目标 72 小时。
- 每分钟记录 RSS、CPU、温度、发送/接收字节、队列丢帧和重连次数。
- 验收：无崩溃，RSS 无持续线性增长，断网恢复成功率 100%，日志不会占满文件系统。

