#ifndef VIDEO_PIPELINE_CONFIG_H // 防止视频流水线配置被重复定义。
#define VIDEO_PIPELINE_CONFIG_H // 定义本头文件的保护宏。

namespace VideoPipelineConfig // 集中保存采集、预览、编码和分析共同使用的帧率配置。
{
// 普通本地预览、网络推流、图像分析和 H.264 编码统一使用 30 FPS。
constexpr int StandardFramesPerSecond = 30; // 编译期整数常量，表示标准链路每秒最多处理 30 帧。

// 推流时降低本地预览刷新率，给编码和网络发送留出 CPU 时间。
constexpr int StreamingPreviewFramesPerSecond = 10; // 推流期间本地 UI 每秒最多刷新 10 帧。
} // namespace VideoPipelineConfig：结束配置命名空间。

#endif // VIDEO_PIPELINE_CONFIG_H：结束头文件保护。
