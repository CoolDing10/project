#include "vision_event_detector.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QtGlobal>

#include <opencv2/dnn/dnn.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/objdetect/objdetect.hpp>

#include <cstdlib>
#include <vector>

namespace {
const int AnalysisWidth = 160;              // 帧差图宽度：小尺寸可显著降低逐像素运算量。
const int AnalysisHeight = 120;             // 帧差图高度，与160宽组成4:3分析图。
const int PixelDifferenceThreshold = 30;    // 两帧灰度差达到30才把像素视为变化。
const int RequiredConsecutiveFrames = 2;    // 连续两帧超阈值才确认运动，抑制瞬时噪点。
const int VisualEventCooldownMs = 10000;    // 两次运动事件至少间隔10秒。
// 人脸检测专用常量已随原实现移到文件末尾备份区。
} // namespace：检测参数只在本实现文件中可见。

/** 初始化检测配置、缓存和跨线程性能计数器。 */
VisionEventDetector::VisionEventDetector(QObject *parent)
    : QObject(parent),                                   // 初始化QObject父子关系。
      // 学习版只保留画面变化检测，不创建 DNN/Haar 对象。
      faceClassifier(nullptr),
      faceDnn(nullptr),
      detectionEnabled(false),                           // 默认关闭运动检测。
      faceDetectionEnabled(false),                       // 默认关闭人脸检测。
      changedRatioThreshold(0.12),                       // 默认12%像素变化触发运动候选。
      consecutiveChangedFrames(0),                       // 尚无连续变化帧。
      faceInferenceInterval(3),                          // 默认每3个分析帧推理一次。
      faceConfidencePercent(60),                         // 默认DNN置信度阈值60%。
      analysisFrameSequence(0),                          // 分析帧编号从0开始。
      analyzedFrameCount(0),                             // 性能计数器全部从0开始。
      inferenceRunCount(0),
      detectedFaceCount(0),
      totalAnalysisMicroseconds(0),
      totalInferenceMicroseconds(0),
      lastInferenceMicroseconds(0),
      activeFaceBackend(FaceBackendDisabled)             // 初始无活动人脸后端。
{
}

/** 释放OpenCV对象；析构发生在视觉工作线程退出后的安全阶段。 */
VisionEventDetector::~VisionEventDetector()
{
    // 与构造函数中的new一一对应。
    delete faceDnn;
    delete faceClassifier;
}

/** 原子读取统计量，避免UI线程读取时与视觉线程写入产生数据竞争。 */
VisionPipelineStatistics VisionEventDetector::statistics() const
{
    // snapshot是按值返回的稳定副本，不向外暴露内部原子变量。
    VisionPipelineStatistics snapshot;
    snapshot.analyzedFrames = analyzedFrameCount.load();
    snapshot.inferenceRuns = inferenceRunCount.load();
    snapshot.detectedFaces = detectedFaceCount.load();
    snapshot.totalAnalysisMicroseconds = totalAnalysisMicroseconds.load();
    snapshot.totalInferenceMicroseconds = totalInferenceMicroseconds.load();
    snapshot.lastInferenceMicroseconds = lastInferenceMicroseconds.load();
    snapshot.activeFaceBackend = activeFaceBackend.load();
    return snapshot;
}

/** 更新运动检测开关和阈值，同时重置上一轮连续检测状态。 */
void VisionEventDetector::configure(bool enabled, int changedPixelPercent)
{
    // 保存画面变化检测开关。
    detectionEnabled = enabled;
    // qBound限制异常UI输入，再把百分数转换为0~1比例。
    changedRatioThreshold = qBound(1, changedPixelPercent, 90) / 100.0;
    // 参数变化后重新累计连续帧，避免使用旧阈值状态误触发。
    consecutiveChangedFrames = 0;
    // 关闭时丢弃上一灰度帧，下次开启的首帧只建立基线。
    if (!enabled)
        previousGrayFrame = QImage();

    // 输出实际配置值，便于开发板现场确认UI设置是否已送达工作线程。
    qInfo() << "视觉事件检测配置：启用=" << enabled
            << "变化像素阈值=" << changedPixelPercent << '%';
}

/** 人脸检测接口保留为空实现，避免影响现有类接口和元对象调用。 */
void VisionEventDetector::configureFaceDetection(
        bool enabled, const QString &classifierPath,
        const QString &dnnConfigurationPath, const QString &dnnModelPath,
        int inferenceInterval, int confidencePercent)
{
    Q_UNUSED(enabled)
    Q_UNUSED(classifierPath)
    Q_UNUSED(dnnConfigurationPath)
    Q_UNUSED(dnnModelPath)
    Q_UNUSED(inferenceInterval)
    Q_UNUSED(confidencePercent)
    faceDetectionEnabled = false;
    activeFaceBackend.store(FaceBackendDisabled);
}

/**
 * 学习版只完成一帧的画面变化检测。本函数由 QueuedConnection
 * 投递到视觉工作线程，结束前必须发射frameHandled()释放采集端门控。
 */
void VisionEventDetector::analyzeFrame(const QImage &frame)
{
    // totalAnalysisTimer覆盖本函数全部处理，用于性能面板计算平均分析耗时。
    QElapsedTimer totalAnalysisTimer;
    totalAnalysisTimer.start();
    // 空帧无法分析，但仍需释放上游门控，否则后续帧会永久被丢弃。
    if (frame.isNull()) {
        qWarning() << "视觉检测器收到空图像";
        emit frameHandled();
        return;
    }

    // 学习版只有画面变化检测；关闭时只回传该帧作为抓拍缓存。
    if (!detectionEnabled) {
    // 不再执行 160x120 缩放、灰度转换和逐像素帧差，避免无业务需求时
    // 与单核 i.MX6ULL 上的 H.264 软件编码争抢 CPU。
    previousGrayFrame = QImage();
    cachedFaceBoxes.clear();
    emit analysisCompleted(frame, 0.0);
    analyzedFrameCount.fetch_add(1);
    totalAnalysisMicroseconds.fetch_add(
                static_cast<quint64>(totalAnalysisTimer.nsecsElapsed() / 1000));
    emit frameHandled();
    return;
    }

    // 先缩小到160x120，再转灰度，把帧差计算量固定为19200个像素。
    const QImage grayFrame = frame.scaled(
                AnalysisWidth, AnalysisHeight,
                Qt::IgnoreAspectRatio, Qt::FastTransformation)
            .convertToFormat(QImage::Format_Grayscale8);
    // 内存不足等情况可能得到空图，同样需要通知上游本帧已结束。
    if (grayFrame.isNull()) {
        qWarning() << "视觉检测缩放或灰度转换失败";
        emit frameHandled();
        return;
    }

    // changedPixelCount记录与上一帧差异达到灰度阈值的像素数量。
    int changedPixelCount = 0;
    // totalPixelCount是变化比例的分母；正常固定为160*120。
    const int totalPixelCount = grayFrame.width() * grayFrame.height();
    // 第一帧没有历史基线，只保存图像而不产生变化像素。
    if (!previousGrayFrame.isNull()) {
        // 按扫描行访问QImage，避免逐像素调用pixel()造成额外开销。
        for (int row = 0; row < grayFrame.height(); ++row) {
            // currentLine指向当前灰度图第row行的只读字节。
            const uchar *currentLine = grayFrame.constScanLine(row);
            // previousLine指向上一灰度图相同行。
            const uchar *previousLine = previousGrayFrame.constScanLine(row);
            // 对该行每个8位灰度像素执行绝对差计算。
            for (int column = 0; column < grayFrame.width(); ++column) {
                // 灰度差达到30才计数，过滤传感器噪声和轻微曝光波动。
                if (std::abs(int(currentLine[column]) - int(previousLine[column])) >=
                        PixelDifferenceThreshold) {
                    ++changedPixelCount;
                }
            }
        }
    }

    // 把变化像素数量转换为0~1比例；分母异常为0时安全返回0。
    const double changedRatio = totalPixelCount > 0
            ? double(changedPixelCount) / totalPixelCount : 0.0;
    // 当前帧成为下一次分析的比较基线；QImage使用隐式共享降低复制成本。
    previousGrayFrame = grayFrame;
    // 无论是否启用事件触发，都把变化比例返回UI用于实时观察。
    emit analysisCompleted(frame, changedRatio);

    // 只有检测开启且本帧超过阈值时才累计连续变化帧。
    if (detectionEnabled && changedRatio >= changedRatioThreshold)
        ++consecutiveChangedFrames;
    else
        consecutiveChangedFrames = 0;

    // 无有效计时器表示从未触发；否则必须经过10秒冷却。
    const bool cooldownExpired = !eventCooldownTimer.isValid() ||
            eventCooldownTimer.elapsed() >= VisualEventCooldownMs;
    // 开关、冷却和连续帧三个条件同时成立才产生一次业务事件。
    if (detectionEnabled && cooldownExpired &&
            consecutiveChangedFrames >= RequiredConsecutiveFrames) {
        // 从当前事件重新开始冷却计时。
        eventCooldownTimer.restart();
        // 事件已经消费连续状态，避免下一帧立即再次命中。
        consecutiveChangedFrames = 0;
        emit visualMotionDetected(frame, changedRatio);
    }

    // 人脸识别流水线已停用，原实现集中保存在文件末尾。

    // 本帧全部算法结束后更新总帧数和总耗时。
    analyzedFrameCount.fetch_add(1);
    totalAnalysisMicroseconds.fetch_add(
                static_cast<quint64>(totalAnalysisTimer.nsecsElapsed() / 1000));
    // 最后释放采集线程的视觉单帧门控，允许提交下一帧。
    emit frameHandled();
}

#if 0
/*
 * ============================================================================
 * 学习版停用的人脸检测原代码备份区
 * 原代码从构造函数、configureFaceDetection() 和 analyzeFrame() 主流程移到这里。
 * 当前编译只保留画面变化检测。
 * 恢复时需按小标题移回原位置，不能直接把整个备份区改成 #if 1。
 * ============================================================================
 */

// 原人脸检测专用常量：
const int FaceAnalysisWidth = 320;
const int FaceEventCooldownMs = 15000;
const int DnnInputWidth = 300;
const int DnnInputHeight = 300;

// 原构造函数中的 OpenCV 对象初始化：
      faceClassifier(new cv::CascadeClassifier()),      // 预创建Haar分类器供加载或回退。
      faceDnn(new cv::dnn::Net()),                       // 预创建空DNN网络供Caffe模型加载。

// 原 configureFaceDetection() 实现：
    // 学习版已停用人脸检测。下面保留完整的 DNN/Haar 配置代码供以后恢复。
    // 限制推理间隔，防止0导致取模错误，也避免设置过大造成检测迟钝。
    faceInferenceInterval = qBound(1, inferenceInterval, 30);
    // 限制DNN置信度到实用范围，降低极端参数造成的误检/漏检。
    faceConfidencePercent = qBound(30, confidencePercent, 95);
    // 配置变化后从第0帧重新调度，并清除旧模型产生的人脸框。
    analysisFrameSequence = 0;
    cachedFaceBoxes.clear();

    // 用户关闭时无需加载模型，立即广播空结果清除UI叠加框。
    if (!enabled) {
        faceDetectionEnabled = false;
        activeFaceBackend.store(FaceBackendDisabled);
        emit faceAnalysisCompleted(QImage(), QVector<QRect>());
        emit faceDetectionStatusChanged(true, QStringLiteral("边缘AI已关闭"));
        qInfo() << "边缘AI人脸检测已关闭";
        return;
    }

    // Haar模型作为轻量回退方案；空路径或加载失败都会得到false。
    const bool haarAvailable = !classifierPath.isEmpty() &&
            faceClassifier->load(classifierPath.toStdString());
    // DNN是否成功加载由readNetFromCaffe结果和empty()共同确认。
    bool dnnAvailable = false;
    // 捕获OpenCV异常文本，后续既写日志也可用于诊断模型兼容问题。
    QString dnnError;
    // 配置文件和权重文件都存在路径时才尝试创建网络。
    if (!dnnConfigurationPath.isEmpty() && !dnnModelPath.isEmpty()) {
        try {
            // 将Caffe网络结构和权重解析为OpenCV dnn::Net。
            *faceDnn = cv::dnn::readNetFromCaffe(
                        dnnConfigurationPath.toStdString(),
                        dnnModelPath.toStdString());
            dnnAvailable = !faceDnn->empty();
        } catch (const cv::Exception &error) {
            // 模型格式、动态库或内存问题都可能通过cv::Exception报告。
            dnnError = QString::fromLocal8Bit(error.what());
        }
    }

    // 两个后端都不可用时必须关闭开关，避免每帧反复失败。
    if (!dnnAvailable && !haarAvailable) {
        faceDetectionEnabled = false;
        activeFaceBackend.store(FaceBackendDisabled);
        const QString message = QStringLiteral(
            "轻量DNN与Haar均加载失败，请检查模型目录：%1")
                .arg(dnnModelPath);
        qWarning() << message;
        if (!dnnError.isEmpty())
            qWarning() << "OpenCV DNN错误：" << dnnError;
        emit faceDetectionStatusChanged(false, message);
        return;
    }

    // 至少一个后端可用，人脸流水线正式进入运行状态。
    faceDetectionEnabled = true;
    // invalidate让第一次检测到人脸时无需等待15秒冷却。
    faceEventCooldownTimer.invalidate();
    // DNN优先；只有DNN不可用时才选择Haar。
    activeFaceBackend.store(dnnAvailable
                            ? FaceBackendLightweightDnn
                            : FaceBackendHaar);
    // 根据实际选择生成用户可理解的状态文本。
    const QString message = dnnAvailable
            ? QStringLiteral("轻量DNN人脸检测已开启（每%1帧推理，置信度%2%）")
                .arg(faceInferenceInterval).arg(faceConfidencePercent)
            : QStringLiteral("轻量DNN不可用，已自动回退Haar（每%1帧推理）")
                .arg(faceInferenceInterval);
    qInfo() << message
            << (dnnAvailable ? dnnModelPath : classifierPath);
    if (!dnnAvailable && !dnnError.isEmpty())
        qWarning() << "轻量DNN加载失败原因：" << dnnError;
    emit faceDetectionStatusChanged(true, message);

// 原 analyzeFrame() 快速路径条件：
    // 原逻辑：运动和人脸两种检测都关闭时才走快速路径。
    if (!detectionEnabled && !faceDetectionEnabled) {

// 原 analyzeFrame() 人脸检测流水线：
    /*
     * 人脸识别流水线（DNN/Haar、检测框缓存和人脸事件）在学习版中停用。
     * 完整实现保留在此条件块内，需要恢复时再开启。
     */
    // 默认复用上一次检测框，使抽帧推理期间的显示不会频繁闪烁。
    QVector<QRect> detectedFaceBoxes = cachedFaceBoxes;
    // 通过帧序号取模决定本帧是否执行高成本人脸推理。
    const bool inferenceDue = faceDetectionEnabled &&
            (analysisFrameSequence % quint64(faceInferenceInterval) == 0);
    // 每个有效分析帧都推进序号。
    ++analysisFrameSequence;

    // 到达推理帧时重新计算检测框，而不是继续使用缓存。
    if (inferenceDue) {
        // 单独统计人脸推理耗时，便于评估DNN在i.MX6ULL上的负载。
        QElapsedTimer inferenceTimer;
        inferenceTimer.start();
        detectedFaceBoxes.clear();

        // 将原图等比缩放至320宽并转换为连续解释的RGB888三通道图。
        const QImage analysisImage = frame.scaledToWidth(
                    FaceAnalysisWidth, Qt::FastTransformation)
                .convertToFormat(QImage::Format_RGB888);
        // 缩放成功后才构造OpenCV矩阵视图。
        if (!analysisImage.isNull()) {
            // rgbImage只引用QImage内存，不复制像素；生命周期受当前作用域保护。
            cv::Mat rgbImage(analysisImage.height(), analysisImage.width(),
                             CV_8UC3,
                             const_cast<uchar *>(analysisImage.constBits()),
                             analysisImage.bytesPerLine());
            // 原子读取一次后端状态，减少本次推理中的跨线程状态变化影响。
            const int backend = activeFaceBackend.load();

            // DNN模式还要验证网络指针及模型非空。
            if (backend == FaceBackendLightweightDnn &&
                    faceDnn && !faceDnn->empty()) {
                try {
                    // OpenCV Caffe模型使用BGR通道顺序，因此先从RGB转换。
                    cv::Mat bgrImage;
                    cv::cvtColor(rgbImage, bgrImage, CV_RGB2BGR);
                    // 生成1x3x300x300的浮点Blob并减去模型训练均值。
                    const cv::Mat inputBlob = cv::dnn::blobFromImage(
                        bgrImage, 1.0,
                        cv::Size(DnnInputWidth, DnnInputHeight),
                        cv::Scalar(104.0, 177.0, 123.0), false, false);
                    // 设置网络输入并同步执行一次前向传播。
                    faceDnn->setInput(inputBlob);
                    const cv::Mat output = faceDnn->forward();
                    // SSD输出应为[1,1,N,7]，每行描述一个候选框。
                    if (output.dims == 4 && output.size[3] >= 7) {
                        // 把四维输出重解释为N行记录，不复制推理结果。
                        cv::Mat detections(output.size[2], output.size[3],
                                           CV_32F,
                                           const_cast<float *>(output.ptr<float>()));
                        // UI百分数转换为0~1浮点阈值。
                        const float minimumConfidence =
                                float(faceConfidencePercent) / 100.0f;
                        // 逐候选框过滤置信度并映射回原始帧坐标。
                        for (int row = 0; row < detections.rows; ++row) {
                            const float confidence = detections.at<float>(row, 2);
                            // 低置信候选直接跳过以降低误检。
                            if (confidence < minimumConfidence)
                                continue;
                            // SSD坐标是0~1归一化值，乘原始帧尺寸得到像素位置。
                            const int left = qRound(
                                detections.at<float>(row, 3) * frame.width());
                            const int top = qRound(
                                detections.at<float>(row, 4) * frame.height());
                            const int right = qRound(
                                detections.at<float>(row, 5) * frame.width());
                            const int bottom = qRound(
                                detections.at<float>(row, 6) * frame.height());
                            // 使用左上和右下点创建可能尚未规范化的矩形。
                            const QRect box(QPoint(left, top),
                                            QPoint(right, bottom));
                            // normalized修正反向坐标，intersected把框裁剪在图像边界内。
                            const QRect bounded = box.normalized()
                                    .intersected(frame.rect());
                            // 过滤过小框，避免噪声框进入业务事件和UI。
                            if (bounded.width() >= 16 && bounded.height() >= 16)
                                detectedFaceBoxes.append(bounded);
                        }
                    }
                } catch (const cv::Exception &error) {
                    // 推理异常保留日志，并在有Haar模型时进行运行时降级。
                    qWarning() << "轻量DNN推理失败：" << error.what();
                    if (faceClassifier && !faceClassifier->empty()) {
                        activeFaceBackend.store(FaceBackendHaar);
                        emit faceDetectionStatusChanged(
                            true, QStringLiteral("DNN推理失败，已自动回退Haar"));
                    } else {
                        // 没有可回退模型时关闭人脸检测，避免每帧重复抛异常。
                        faceDetectionEnabled = false;
                        activeFaceBackend.store(FaceBackendDisabled);
                        emit faceDetectionStatusChanged(
                            false, QStringLiteral("DNN推理失败且Haar不可用，边缘AI已关闭"));
                    }
                }
            }

            // 当前后端为Haar时，在缩小后的灰度图上执行级联检测。
            if (activeFaceBackend.load() == FaceBackendHaar &&
                    faceClassifier && !faceClassifier->empty()) {
                // Haar分类器以单通道灰度图作为输入。
                cv::Mat grayImage;
                cv::cvtColor(rgbImage, grayImage, CV_RGB2GRAY);
                // 直方图均衡增强不同光照条件下的人脸局部对比度。
                cv::equalizeHist(grayImage, grayImage);
                // faces接收320宽分析图坐标系中的OpenCV矩形。
                std::vector<cv::Rect> faces;
                // 1.2为金字塔缩放倍率，5为候选框最小邻居数，30x30为最小人脸。
                faceClassifier->detectMultiScale(
                            grayImage, faces, 1.2, 5, 0,
                            cv::Size(30, 30));
                // 计算分析图到原始图的水平和垂直坐标缩放系数。
                const double scaleX =
                        double(frame.width()) / analysisImage.width();
                const double scaleY =
                        double(frame.height()) / analysisImage.height();
                // 将每个Haar矩形映射回原始帧，供抓拍和预览统一使用。
                for (const cv::Rect &face : faces) {
                    detectedFaceBoxes.append(QRect(
                        qRound(face.x * scaleX), qRound(face.y * scaleY),
                        qRound(face.width * scaleX),
                        qRound(face.height * scaleY)));
                }
            }
        }

        // 缓存本次推理结果，后续非推理帧继续显示相同的人脸框。
        cachedFaceBoxes = detectedFaceBoxes;
        // 纳秒计时转换为微秒，避免统计数字过大且保持足够精度。
        const quint64 inferenceMicroseconds =
                static_cast<quint64>(inferenceTimer.nsecsElapsed() / 1000);
        // 原子更新性能面板使用的累计统计。
        inferenceRunCount.fetch_add(1);
        detectedFaceCount.fetch_add(
                    static_cast<quint64>(detectedFaceBoxes.size()));
        totalInferenceMicroseconds.fetch_add(inferenceMicroseconds);
        lastInferenceMicroseconds.store(inferenceMicroseconds);
    } else if (!faceDetectionEnabled) {
        // 人脸检测关闭时必须清空当前结果和缓存，立即移除旧叠加框。
        detectedFaceBoxes.clear();
        cachedFaceBoxes.clear();
    }
    // 每个分析帧都向采集/UI返回人脸框，推理间隔期间使用缓存值。
    emit faceAnalysisCompleted(frame, detectedFaceBoxes);

    // 人脸业务事件使用独立15秒冷却，不影响每帧框显示。
    const bool faceCooldownExpired = !faceEventCooldownTimer.isValid() ||
            faceEventCooldownTimer.elapsed() >= FaceEventCooldownMs;
    // 检测开启、有至少一张脸且冷却到期时才触发抓拍/录像业务。
    if (faceDetectionEnabled && !detectedFaceBoxes.isEmpty() &&
            faceCooldownExpired) {
        faceEventCooldownTimer.restart();
        emit facePresenceDetected(frame, detectedFaceBoxes.size());
    }
#endif
