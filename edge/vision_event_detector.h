/*
 * vision_event_detector.h — 独立工作线程中的画面变化检测器
 */
#ifndef VISION_EVENT_DETECTOR_H
#define VISION_EVENT_DETECTOR_H

#include <QElapsedTimer>
#include <QImage>
#include <QObject>

class VisionEventDetector final : public QObject
{
    Q_OBJECT

public:
    explicit VisionEventDetector(QObject *parent = nullptr);
public slots:
    void configure(bool enabled, int changedPixelPercent);
    void analyzeFrame(const QImage &frame);

signals:
    void analysisCompleted(QImage frame, double changedRatio);
    void visualMotionDetected(QImage frame, double changedRatio);

private:
    QImage previousGrayFrame;
    QElapsedTimer eventCooldownTimer;
    bool detectionEnabled;
    double changedRatioThreshold;
    int consecutiveChangedFrames;
};

#endif // VISION_EVENT_DETECTOR_H
