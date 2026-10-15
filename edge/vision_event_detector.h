/*
 * ============================================================================
 * vision_event_detector.h — OpenCV 视觉事件检测器（运动检测 + 人脸检测）
 * ============================================================================
 *
 * 工作在独立 QThread 中（通过 moveToThread 移入）。
 * 收到 analysisFrameReady → analyzeFrame()，执行两种检测：
 *
 * 1. 运动检测（帧差法）：
 *    - 当前帧与前一帧的灰度图逐像素比较
 *    - 变化像素比例 >= 阈值（如 5%）→ 运动事件
 *    - 冷却时间 10s，避免同一运动重复触发
 *
 * 2. 人脸检测（Haar Cascade / DNN 双后端）：
 *    - Haar:  OpenCV CascadeClassifier，速度快但准确率中等
 *    - DNN:   OpenCV dnn::Net（ResNet-10 SSD），准确率优于 Haar
 *    - 每 N 帧推理一次（由 inferenceInterval 控制）
 *    - 冷却时间 15s
 *
 * 检测结果通过信号通知 CameraStreamingPage（主线程）进行告警和录像处理。
 */
#ifndef VISION_EVENT_DETECTOR_H
#define VISION_EVENT_DETECTOR_H

#include <atomic>

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QRect>
#include <QString>
#include <QVector>

#include <opencv2/dnn/dnn.hpp>

namespace cv
{
    class CascadeClassifier; // 前置声明Haar分类器，减少头文件传播和编译时间。
}

/** @brief 视觉工作线程的只读性能快照，由原子计数器生成。 */
struct VisionPipelineStatistics
{
    quint64 analyzedFrames = 0;             // 已完成运动检测/人脸调度的总帧数。
    quint64 inferenceRuns = 0;              // 实际执行Haar或DNN推理的次数。
    quint64 detectedFaces = 0;              // 历次推理检测到的人脸数量累计值。
    quint64 totalAnalysisMicroseconds = 0;  // 所有分析帧累计耗时，单位微秒。
    quint64 totalInferenceMicroseconds = 0; // 所有人脸推理累计耗时，单位微秒。
    quint64 lastInferenceMicroseconds = 0;  // 最近一次人脸推理耗时，单位微秒。
    int activeFaceBackend = 0;              // 当前后端，取值见FaceBackend枚举。
};

class VisionEventDetector final : public QObject
{
    Q_OBJECT // 注册信号槽和跨线程元调用信息。

public:
    /** @brief 人脸检测后端状态，供性能面板和运行时回退逻辑共同使用。 */
    enum FaceBackend
    {
        FaceBackendDisabled = 0,      // 人脸检测关闭或模型不可用。
        FaceBackendHaar = 1,          // 使用轻量但精度较低的Haar级联分类器。
        FaceBackendLightweightDnn = 2 // 使用ResNet-10 SSD轻量DNN模型。
    };

    /** @param parent Qt父对象；通常为空，随后由moveToThread管理线程归属。 */
    explicit VisionEventDetector(QObject *parent = nullptr);

    /** @brief 释放通过裸指针持有的OpenCV分类器和DNN网络。 */
    ~VisionEventDetector() override;

    /** @brief 原子读取各项计数器，返回不会撕裂的性能统计副本。 */
    VisionPipelineStatistics statistics() const;

public slots:
    /**
     * @brief 更新帧差运动检测配置并清空连续触发状态。
     * @param enabled 是否执行运动检测。
     * @param changedPixelPercent 触发所需的变化像素百分比，内部限制为1~90。
     */
    void configure(bool enabled, int changedPixelPercent);

    /**
     * @brief 加载人脸模型并选择DNN或Haar后端。
     * @param enabled 是否启用人脸检测。
     * @param classifierPath Haar XML分类器路径，作为DNN失败时的回退模型。
     * @param dnnConfigurationPath Caffe网络结构prototxt路径。
     * @param dnnModelPath Caffe模型权重路径。
     * @param inferenceInterval 每隔多少个分析帧执行一次人脸推理。
     * @param confidencePercent DNN接受检测结果的最低置信度百分比。
     */
    void configureFaceDetection(bool enabled,
                                const QString &classifierPath,
                                const QString &dnnConfigurationPath,
                                const QString &dnnModelPath,
                                int inferenceInterval,
                                int confidencePercent);

    /**
     * @brief 对一帧图像执行帧差分析，并按计划运行人脸推理。
     * @param frame 来自采集线程的深拷贝图像；本槽在视觉工作线程中执行。
     */
    void analyzeFrame(const QImage &frame);

signals:
    void frameHandled(); // 告诉采集线程本帧已消费，可解除单帧门控。
    void analysisCompleted(QImage frame, double changedRatio); // 返回帧差比例供UI显示。
    void visualMotionDetected(QImage frame, double changedRatio); // 通知业务层产生运动事件。
    void faceAnalysisCompleted(QImage frame, QVector<QRect> faceBoxes); // 返回人脸框用于叠加显示。
    void facePresenceDetected(QImage frame, int faceCount); // 冷却期满时产生一次人脸事件。
    void faceDetectionStatusChanged(bool available, QString message); // 报告后端加载/回退状态。

private:
    QImage previousGrayFrame;                 // 上一分析帧的160x120灰度图，用于逐像素帧差。
    QElapsedTimer eventCooldownTimer;         // 运动事件冷却计时器，防止告警风暴。
    QElapsedTimer faceEventCooldownTimer;     // 人脸事件冷却计时器。
    cv::CascadeClassifier *faceClassifier;   // Haar回退检测器，生命周期由本类管理。
    cv::dnn::Net *faceDnn;                   // 轻量Caffe DNN网络，生命周期由本类管理。
    bool detectionEnabled;                   // 是否启用帧差运动检测。
    bool faceDetectionEnabled;               // 是否启用人脸推理与人脸事件。
    double changedRatioThreshold;             // 运动检测触发阈值，范围0.01~0.90。
    int consecutiveChangedFrames;             // 连续超过阈值的帧数，用于过滤瞬时噪声。
    int faceInferenceInterval;                // 人脸推理抽帧间隔，降低i.MX6ULL CPU负载。
    int faceConfidencePercent;                // DNN最低置信度，范围30%~95%。
    quint64 analysisFrameSequence;             // 分析帧序号，用取模判断本帧是否需要推理。
    QVector<QRect> cachedFaceBoxes;            // 非推理帧复用的最近一次人脸框。

    std::atomic<quint64> analyzedFrameCount;             // 跨线程读取的分析帧计数。
    std::atomic<quint64> inferenceRunCount;              // 跨线程读取的推理次数。
    std::atomic<quint64> detectedFaceCount;              // 跨线程读取的人脸累计数。
    std::atomic<quint64> totalAnalysisMicroseconds;      // 跨线程读取的分析累计耗时。
    std::atomic<quint64> totalInferenceMicroseconds;     // 跨线程读取的推理累计耗时。
    std::atomic<quint64> lastInferenceMicroseconds;      // 跨线程读取的最近推理耗时。
    std::atomic<int> activeFaceBackend;                  // 跨线程读取的后端枚举值。
};

#endif // VISION_EVENT_DETECTOR_H
