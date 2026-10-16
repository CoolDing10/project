/*
 * video_monitor.h — “边缘监控”核心页面声明
 *
 * CameraStreamingPage 本身主要负责业务编排：
 *   CaptureThread 采集 → 本地预览 / H.264 推流 / 画面变化检测；
 *   检测事件 → SQLite / UI / MQTT / 自动 H.264 录像；
 *   MediaArchiveServer → 向 PC 提供事件录像列表和下载。
 */
#ifndef VIDEO_MONITOR_H // 防止本头文件被重复包含。
#define VIDEO_MONITOR_H // 定义头文件保护宏。

#include "monitoring_event.h" // 引入事件等级和 MonitoringEvent 公共数据结构。

#include <QImage>  // 保存最近摄像头帧并作为信号槽参数传递。
#include <QThread> // 声明网络、视觉和归档三个工作线程成员。
#include <QWidget> // CameraStreamingPage 的 Qt 页面基类。

class CaptureThread;        // 前向声明 V4L2 摄像头采集线程。
class EventStore;           // 前向声明 SQLite 事件存储对象。
class MediaArchiveServer;   // 前向声明录像列表和下载服务。
class QCheckBox;            // 前向声明布尔配置复选框。
class QComboBox;            // 前向声明 TCP/UDP 传输方式选择框。
class QJsonObject;          // 前向声明 MQTT JSON 消息对象。
class QLabel;               // 前向声明画面和状态标签。
class QLineEdit;            // 前向声明 IP/主机名输入框。
class QListWidget;          // 前向声明事件列表。
class QPushButton;          // 前向声明开始采集按钮。
class QResizeEvent;         // 前向声明页面缩放事件。
class QSpinBox;             // 前向声明端口和阈值数值框。
class QTimer;               // 前向声明心跳和重连定时器。
class QMqttClient;          // 前向声明 Qt MQTT 客户端。
class TcpFrameSender;       // 前向声明 FFmpeg H.264 编码和发送器。
class VisionEventDetector;  // 前向声明画面变化检测器。

class CameraStreamingPage final : public QWidget // final 表示边缘监控页面不再被继承。
{
    Q_OBJECT // 启用 Qt 信号槽和跨线程元对象调用。

public:
    /**
     * 创建页面 UI、恢复配置并初始化所有后台服务。
     * @param parent Qt 父控件，负责页面在主窗口对象树中的生命周期。
     */
    explicit CameraStreamingPage(QWidget *parent = nullptr);

    /** 按生产者到消费者顺序停止线程、保存设置并释放 EventStore。 */
    ~CameraStreamingPage() override;

signals:
    /**
     * 把新的网络推流配置发送到 TcpFrameSender 所在工作线程。
     * @param host PC 接收端 IP 地址或主机名。
     * @param port PC 接收端视频监听端口。
     * @param enabled true 开始推流，false 停止推流。
     * @param transport "tcp" 或 "udp"。
     */
    void senderConfigurationChanged(const QString &host,
                                    quint16 port,
                                    bool enabled,
                                    const QString &transport);

    /**
     * 请求网络线程开始写 H.264 事件录像。
     * @param filePath 录像文件完整路径。
     * @param durationMs 自动停止前的录像时长，单位毫秒。
     */
    void recordingRequested(const QString &filePath, int durationMs);

    /** 请求网络线程立即停止当前事件录像。 */
    void recordingStopRequested();

    /**
     * 把最新变化检测配置发送到视觉工作线程。
     * @param enabled 是否执行帧差检测。
     * @param changedPixelPercent 触发事件所需的变化像素百分比。
     */
    void detectorConfigurationChanged(bool enabled,
                                      int changedPixelPercent);

protected:
    /**
     * 页面尺寸改变后重新缩放最近一帧预览。
     * @param event Qt 传入的尺寸变化事件。
     */
    void resizeEvent(QResizeEvent *event) override;

private slots:
    /** @param image CaptureThread 分发到主线程的本地预览帧。 */
    void updateLocalPreview(const QImage &image);

    /** @param capturing true 表示采集中，用于更新按钮文字和选中状态。 */
    void updateCaptureButtonText(bool capturing);

    /** 根据“采集、变化检测”开关更新分析帧分发状态。 */
    void updateAnalysisFrameDistribution();

    /** @param enabled 用户是否启用网络推流。 */
    void networkStreamingToggled(bool enabled);

    /** 保存并下发当前 PC 地址、端口和 TCP/UDP 方式。 */
    void applyServerEndpoint();

    /** @param enabled 用户是否启用实时画面变化检测。 */
    void visionDetectionToggled(bool enabled);

    /** @param enabled 用户是否要求连接 MQTT 服务器。 */
    void mqttMonitoringToggled(bool enabled);

    /**
     * 接收视觉线程的逐帧分析结果。
     * @param image 本次分析的原始帧。
     * @param changedRatio 变化像素比例，范围 0.0～1.0。
     */
    void handleAnalysisCompleted(const QImage &image, double changedRatio);

    /** @param changedRatio 触发视觉事件这一帧的变化像素比例。 */
    void handleVisualMotionDetected(double changedRatio);

    /** 定时向设备状态主题发布 MQTT 心跳 JSON。 */
    void publishHeartbeat();

    /**
     * 同步页面中的事件录像状态。
     * @param active true 表示正在录像，false 表示录像已经停止。
     * @param filePath 本次事件录像文件路径。
     */
    void handleRecordingStateChanged(bool active, const QString &filePath);

    /** @param message TcpFrameSender 返回的录像错误说明。 */
    void handleRecordingError(const QString &message);

private:
    /** 创建边缘监控页面全部控件和布局。 */
    void initializeUi();

    /** 创建采集、网络、视觉、归档、数据库对象，并建立信号槽关系。 */
    void initializeServices();

    /** 创建 MQTT 客户端、心跳/重连定时器并连接 MQTT 信号。 */
    void initializeMqtt();

    /** 从 QSettings 读取上次保存的服务器、端口和开关配置。 */
    void loadSettings();

    /** 把当前页面配置写回 QSettings。 */
    void saveSettings() const;

    /** @param image 要按预览标签尺寸等比例显示的摄像头图像。 */
    void displayPreviewImage(const QImage &image);

    /**
     * 生成新的 H.264 事件录像路径。
     * @param eventType 写入文件名的事件类型。
     * @return data/recordings 下带毫秒时间戳的 .h264 路径。
     */
    QString createRecordingPath(const QString &eventType) const;

    /**
     * 统一执行事件录像、SQLite 保存、UI 显示和 MQTT 发布。
     * @param eventType 机器可读事件类型。
     * @param severity 事件严重等级。
     * @param source 产生事件的模块名。
     * @param message 面向用户的事件说明。
     * @param requestRecording true 表示满足条件时请求自动录像。
     */
    void recordMonitoringEvent(const QString &eventType,
                               MonitoringEventSeverity severity,
                               const QString &source,
                               const QString &message,
                               bool requestRecording);

    /** @param event 要插入页面顶部的完整监控事件。 */
    void addEventToList(const MonitoringEvent &event);

    /**
     * 把 JSON 序列化后发布到指定 MQTT 主题。
     * @param topic 完整 MQTT 主题。
     * @param object 要发布的 JSON 对象。
     */
    void publishMqttJson(const QString &topic, const QJsonObject &object);

    /** @param payload 从 MQTT 命令主题收到的 UTF-8 JSON 字节。 */
    void handleMqttCommand(const QByteArray &payload);

    /**
     * 根据设备 ID 生成完整 MQTT 主题。
     * @param suffix 主题末段，例如 status、event 或 command。
     * @return devices/<deviceId>/<suffix>。
     */
    QString mqttTopic(const QString &suffix) const;

    QLabel *cameraPreviewLabel;          // 显示本地摄像头画面或视觉分析结果。
    QPushButton *startCaptureButton;     // 控制 CaptureThread 开始或停止采集。
    QCheckBox *localPreviewCheckBox;     // 控制是否向主线程分发本地预览帧。
    QCheckBox *networkStreamingCheckBox; // 控制是否编码并向 PC 推送视频。
    QCheckBox *visionDetectionCheckBox;  // 控制是否向视觉线程分发分析帧。
    QCheckBox *automaticRecordingCheckBox; // 控制视觉事件是否自动录制 10 秒 H.264。
    QCheckBox *mqttEnabledCheckBox;      // 控制是否连接 MQTT 服务器。
    QComboBox *streamTransportComboBox;  // 选择 TCP 或 RTP/UDP 推流方式。
    QLineEdit *serverAddressEdit;        // 输入 PC 视频接收端 IP 或主机名。
    QSpinBox *serverPortSpinBox;         // 输入 PC 视频接收端口。
    QLineEdit *mqttHostEdit;             // 输入 MQTT Broker IP 或主机名。
    QSpinBox *mqttPortSpinBox;           // 输入 MQTT Broker TCP 端口。
    QSpinBox *motionThresholdSpinBox;    // 输入画面变化像素百分比阈值。
    QLabel *connectionStatusLabel;       // 显示 H.264 编码和网络连接状态。
    QLabel *mqttStatusLabel;             // 显示 MQTT 连接状态。
    QLabel *visionStatusLabel;           // 显示变化检测状态和实时变化比例。
    QLabel *recordingStatusLabel;        // 显示事件录像是否正在进行。
    QListWidget *eventList;              // 按新到旧显示最近监控事件。

    CaptureThread *cameraCaptureThread; // V4L2 采集生产者线程对象。
    QThread *networkWorkerThread;       // 承载 TcpFrameSender 的网络/编码工作线程。
    TcpFrameSender *frameSender;        // 执行 FFmpeg 编码、TCP/UDP 发送和 H.264 录像。
    QThread *visionWorkerThread;        // 承载 VisionEventDetector 的分析线程。
    VisionEventDetector *visionDetector; // 执行相邻帧差分检测。
    QThread *archiveWorkerThread;       // 承载 MediaArchiveServer 的归档线程。
    MediaArchiveServer *archiveServer;  // 监听 8890 并提供录像列表和下载。
    EventStore *eventStore;             // 保存和读取 SQLite 监控事件。
    QMqttClient *mqttClient;            // 发布状态/事件并订阅远程命令。
    QTimer *heartbeatTimer;             // 定时发布设备状态心跳。
    QTimer *mqttReconnectTimer;         // MQTT 意外断线后的延迟重连定时器。

    QString deviceId;             // MQTT 主题和事件数据使用的设备唯一编号。
    QString currentRecordingPath; // 当前正在写入或刚完成的 H.264 录像路径。
    QImage latestFrame;           // 最近收到的预览帧，用于页面缩放时重新绘制。
    bool eventStoreReady;         // true 表示 SQLite 初始化成功，可以写入事件。
    bool eventRecordingActive;    // true 表示 TcpFrameSender 当前正在录制事件视频。
};

#endif // VIDEO_MONITOR_H：结束头文件保护。
