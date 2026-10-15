/*
 * video_monitor.h — 边缘监控页面
 *
 * 页面负责连接 V4L2 采集、H.264 网络发送、画面变化检测、自动事件录像、
 * SQLite 事件存储、媒体归档服务和 MQTT。硬件传感器由“板级设备”页面管理。
 */
#ifndef VIDEO_MONITOR_H
#define VIDEO_MONITOR_H

#include "monitoring_event.h"

#include <QImage>
#include <QThread>
#include <QWidget>

class CaptureThread;
class EventStore;
class MediaArchiveServer;
class QCheckBox;
class QComboBox;
class QJsonObject;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QResizeEvent;
class QSpinBox;
class QTimer;
class QMqttClient;
class TcpFrameSender;
class VisionEventDetector;

class CameraStreamingPage final : public QWidget
{
    Q_OBJECT

public:
    explicit CameraStreamingPage(QWidget *parent = nullptr);
    ~CameraStreamingPage() override;

signals:
    void senderConfigurationChanged(const QString &host, quint16 port,
                                    bool enabled, const QString &transport);
    void recordingRequested(const QString &filePath, int durationMs);
    void recordingStopRequested();
    void detectorConfigurationChanged(bool enabled,
                                      int changedPixelPercent);

protected:
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void updateLocalPreview(const QImage &image);
    void updateCaptureButtonText(bool capturing);
    void updateAnalysisFrameDistribution();
    void networkStreamingToggled(bool enabled);
    void applyServerEndpoint();
    void visionDetectionToggled(bool enabled);
    void mqttMonitoringToggled(bool enabled);
    void handleAnalysisCompleted(const QImage &image, double changedRatio);
    void handleVisualMotionDetected(const QImage &image, double changedRatio);
    void publishHeartbeat();
    void handleRecordingStateChanged(bool active, const QString &filePath);
    void handleRecordingError(const QString &message);

private:
    void initializeUi();
    void initializeServices();
    void initializeMqtt();
    void loadSettings();
    void saveSettings() const;
    void displayPreviewImage(const QImage &image);
    QString saveSnapshot(const QImage &image, const QString &eventType) const;
    QString createRecordingPath(const QString &eventType) const;
    void recordMonitoringEvent(const QString &eventType,
                               MonitoringEventSeverity severity,
                               const QString &source,
                               const QString &message,
                               const QImage &eventImage,
                               bool requestRecording);
    void addEventToList(const MonitoringEvent &event);
    void publishMqttJson(const QString &topic, const QJsonObject &object);
    void handleMqttCommand(const QByteArray &payload);
    QString mqttTopic(const QString &suffix) const;

    QLabel *cameraPreviewLabel;
    QPushButton *startCaptureButton;
    QCheckBox *localPreviewCheckBox;
    QCheckBox *networkStreamingCheckBox;
    QCheckBox *visionDetectionCheckBox;
    QCheckBox *automaticRecordingCheckBox;
    QCheckBox *mqttEnabledCheckBox;
    QComboBox *streamTransportComboBox;
    QLineEdit *serverAddressEdit;
    QSpinBox *serverPortSpinBox;
    QLineEdit *mqttHostEdit;
    QSpinBox *mqttPortSpinBox;
    QSpinBox *motionThresholdSpinBox;
    QLabel *connectionStatusLabel;
    QLabel *mqttStatusLabel;
    QLabel *visionStatusLabel;
    QLabel *recordingStatusLabel;
    QListWidget *eventList;

    CaptureThread *cameraCaptureThread;
    QThread *networkWorkerThread;
    TcpFrameSender *frameSender;
    QThread *visionWorkerThread;
    VisionEventDetector *visionDetector;
    QThread *archiveWorkerThread;
    MediaArchiveServer *archiveServer;
    EventStore *eventStore;
    QMqttClient *mqttClient;
    QTimer *heartbeatTimer;
    QTimer *mqttReconnectTimer;

    QString deviceId;
    QString currentRecordingPath;
    QImage latestFrame;
    bool eventStoreReady;
    bool eventRecordingActive;

};

#endif // VIDEO_MONITOR_H
