/*
 * vision_event_detector.h — 画面变化检测器声明
 *
 * CameraStreamingPage 把本对象 moveToThread() 到分析线程。
 * CaptureThread 发送 QImage 后，本对象比较当前灰度帧和上一灰度帧，
 * 计算变化像素比例，并在连续变化且冷却时间结束后发出监控事件。
 */
#ifndef VISION_EVENT_DETECTOR_H // 防止头文件被重复包含。
#define VISION_EVENT_DETECTOR_H // 定义头文件保护宏。

#include <QElapsedTimer> // 用单调时钟计算两个视觉事件之间的冷却时间。
#include <QImage>        // 输入帧、上一灰度帧和分析结果都使用 QImage。
#include <QObject>       // QObject 提供线程归属、信号和槽机制。

class VisionEventDetector final : public QObject // final 禁止继承，QObject 提供跨线程槽调用。
{
    Q_OBJECT // 启用 Qt 元对象系统，使 signals 和 slots 可以工作。

public:
    /**
     * 创建检测器，默认关闭检测，并设置默认变化比例阈值 12%。
     * @param parent Qt 父对象；传 nullptr 便于之后把对象移动到工作线程。
     */
    explicit VisionEventDetector(QObject *parent = nullptr);

public slots:
    /**
     * 更新检测开关和触发阈值，同时清零连续变化帧计数。
     * @param enabled true 开启画面变化检测，false 关闭检测。
     * @param changedPixelPercent 变化像素百分比，内部限制到 1～90。
     */
    void configure(bool enabled, int changedPixelPercent);

    /**
     * 分析一帧图像；通常由 CaptureThread 的跨线程信号调用。
     * @param frame 摄像头产生的完整图像，只读引用避免函数入口额外复制。
     */
    void analyzeFrame(const QImage &frame);

signals:
    /**
     * 每次有效分析后返回原图和本帧变化比例，用于页面显示分析结果。
     * @param frame 本次被分析的原始画面。
     * @param changedRatio 变化像素数/总像素数，范围为 0.0～1.0。
     */
    void analysisCompleted(QImage frame, double changedRatio);

    /**
     * 满足连续帧和冷却时间条件后发出真正的监控事件。
     * @param changedRatio 触发事件这一帧的变化比例，范围为 0.0～1.0。
     */
    void visualMotionDetected(double changedRatio);

private:
    QImage previousGrayFrame;         // 上一次缩放后的灰度帧，是当前帧差分的比较基准。
    QElapsedTimer eventCooldownTimer; // 记录上次事件时间，防止同一变化连续生成大量事件。
    bool detectionEnabled;            // 当前是否执行变化计算；关闭时只转发分析完成信号。
    double changedRatioThreshold;     // 事件阈值的小数形式，例如 12% 保存为 0.12。
    int consecutiveChangedFrames;     // 连续超过阈值的帧数，用于过滤单帧噪声和误报。
};

#endif // VISION_EVENT_DETECTOR_H：结束头文件保护。
