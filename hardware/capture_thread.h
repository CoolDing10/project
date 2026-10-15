/*
 * capture_thread.h — V4L2 摄像头采集线程与三路固定帧率分发器
 *
 * CaptureThread 在独立线程中完成 V4L2 采集，再将同一份深拷贝图像分发给：
 *   localPreviewReady  → 主线程本地预览
 *   networkFrameReady  → 网络线程 H.264 编码与发送
 *   analysisFrameReady → 视觉线程画面变化检测
 *
 * 各链路只按 video_pipeline_config.h 中的固定帧率分发。
 */
#ifndef CAPTURE_THREAD_H
#define CAPTURE_THREAD_H

#include <atomic>

#include <QImage>
#include <QThread>

#define VIDEO_DEV "/dev/video1"
#define VIDEO_BUFFER_COUNT 3

class CaptureThread : public QThread
{
    Q_OBJECT

public:
    explicit CaptureThread(QObject *parent = nullptr);
    ~CaptureThread() override;

signals:
    void localPreviewReady(QImage image);
    void networkFrameReady(QImage image);
    void analysisFrameReady(QImage image);

public slots:
    void setCaptureEnabled(bool enabled);
    void setNetworkStreamingEnabled(bool enabled);
    void setLocalPreviewEnabled(bool enabled);
    void setAnalysisEnabled(bool enabled);

private:
    void run() override;

    std::atomic_bool captureEnabled;
    std::atomic_bool networkStreamingEnabled;
    std::atomic_bool localPreviewEnabled;
    std::atomic_bool analysisEnabled;

};

#endif // CAPTURE_THREAD_H
