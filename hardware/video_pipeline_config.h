#ifndef VIDEO_PIPELINE_CONFIG_H
#define VIDEO_PIPELINE_CONFIG_H

namespace VideoPipelineConfig
{
// 普通本地预览、网络推流、图像分析和 H.264 编码统一使用 30 FPS。
constexpr int StandardFramesPerSecond = 30;

// 推流时降低本地预览刷新率，给编码和网络发送留出 CPU 时间。
constexpr int StreamingPreviewFramesPerSecond = 10;
}

#endif // VIDEO_PIPELINE_CONFIG_H
