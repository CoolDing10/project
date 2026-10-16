#include "vision_event_detector.h"

#include <QDebug>
#include <QtGlobal>

#include <cstdlib>

namespace
{
constexpr int AnalysisWidth = 160;
constexpr int AnalysisHeight = 120;
constexpr int PixelDifferenceThreshold = 30;
constexpr int RequiredConsecutiveFrames = 2;
constexpr int VisualEventCooldownMs = 10000;
}

VisionEventDetector::VisionEventDetector(QObject *parent)
    : QObject(parent),
      detectionEnabled(false),
      changedRatioThreshold(0.12),
      consecutiveChangedFrames(0)
{
}

void VisionEventDetector::configure(bool enabled, int changedPixelPercent)
{
    detectionEnabled = enabled;
    changedRatioThreshold = qBound(1, changedPixelPercent, 90) / 100.0;
    consecutiveChangedFrames = 0;
    if (!enabled)
        previousGrayFrame = QImage();

    qInfo() << "视觉事件检测配置：启用=" << enabled
            << "变化像素阈值=" << changedPixelPercent << '%';
}

void VisionEventDetector::analyzeFrame(const QImage &frame)
{
    if (frame.isNull())
    {
        qWarning() << "视觉检测器收到空图像";
        return;
    }

    if (!detectionEnabled)
    {
        previousGrayFrame = QImage();
        emit analysisCompleted(frame, 0.0);
        return;
    }

    const QImage grayFrame = frame.scaled(
        AnalysisWidth, AnalysisHeight,
        Qt::IgnoreAspectRatio, Qt::FastTransformation)
        .convertToFormat(QImage::Format_Grayscale8);
    if (grayFrame.isNull())
    {
        qWarning() << "视觉检测缩放或灰度转换失败";
        return;
    }

    int changedPixelCount = 0;
    const int totalPixelCount = grayFrame.width() * grayFrame.height();
    if (!previousGrayFrame.isNull())
    {
        for (int row = 0; row < grayFrame.height(); ++row)
        {
            const uchar *currentLine = grayFrame.constScanLine(row);
            const uchar *previousLine = previousGrayFrame.constScanLine(row);
            for (int column = 0; column < grayFrame.width(); ++column)
            {
                if (std::abs(int(currentLine[column]) -
                             int(previousLine[column])) >= PixelDifferenceThreshold)
                    ++changedPixelCount;
            }
        }
    }

    const double changedRatio = totalPixelCount > 0
        ? double(changedPixelCount) / totalPixelCount
        : 0.0;
    previousGrayFrame = grayFrame;
    emit analysisCompleted(frame, changedRatio);

    if (changedRatio >= changedRatioThreshold)
        ++consecutiveChangedFrames;
    else
        consecutiveChangedFrames = 0;

    const bool cooldownExpired = !eventCooldownTimer.isValid() ||
        eventCooldownTimer.elapsed() >= VisualEventCooldownMs;
    if (cooldownExpired &&
        consecutiveChangedFrames >= RequiredConsecutiveFrames)
    {
        eventCooldownTimer.restart();
        consecutiveChangedFrames = 0;
        emit visualMotionDetected(changedRatio);
    }
}
