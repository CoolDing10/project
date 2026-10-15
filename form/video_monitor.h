/*
 * ============================================================================
 * video_monitor.h — 边缘监控页面（系统核心页面）
 * ============================================================================
 *
 * CameraStreamingPage 是整个 i.MX6ULL 智能终端的"心脏页面"。
 * 它是一个"编排器/导演"角色——自己不直接操作摄像头、不自己做 AI 推理，
 * 而是把各项工作委托给下层组件，通过 Qt 信号槽把它们串联起来。
 *
 * 组件协作全景图：
 *   ┌─────────────────────────────────────────────────────────────┐
 *   │                    CameraStreamingPage                       │
 *   │                     （导演/编排器）                           │
 *   │                                                             │
 *   │  ┌──────────┐  ┌──────────────┐  ┌───────────────────────┐  │
 *   │  │ UI 控件   │  │ 子线程组件    │  │ 传感器与执行器         │  │
 *   │  │          │  │              │  │                       │  │
 *   │  │ 预览Label │  │CaptureThread │  │ Ap3216cSensor (光强)   │  │
 *   │  │ CheckBox  │  │ TcpFrameSender│  │ Icm20608Sensor(IMU)   │  │
 *   │  │ QLineEdit │  │VisionDetector│  │ AlarmActuator(告警)    │  │
 *   │  │ QListWidget│ │ QMqttClient  │  │ EventStore(SQLite)     │  │
 *   │  └──────────┘  └──────────────┘  └───────────────────────┘  │
 *   └─────────────────────────────────────────────────────────────┘
 *
 * 数据流（三个消费者）：
 *   CaptureThread::run()
 *     ├── localPreviewReady    → updateLocalPreview()     [主线程显示]
 *     ├── networkFrameReady    → TcpFrameSender::sendFrame() [独立线程]
 *     └── analysisFrameReady   → VisionEventDetector::analyzeFrame() [独立线程]
 *
 * 线程模型：
 *   - 主线程：UI 控件、事件处理、信号槽
 *   - networkWorkerThread：TcpFrameSender（H.264 编码 + TCP/UDP 发送）
 *   - visionWorkerThread：VisionEventDetector（OpenCV 运动检测 + 人脸检测）
 *   - CaptureThread（内部 QThread）：V4L2 采集循环
 *   - archiveWorkerThread：MediaArchiveServer（抓拍/录像分块下载）
 */
#ifndef VIDEO_MONITOR_H
#define VIDEO_MONITOR_H

#include "monitoring_event.h"

#include <QHash>
#include <QImage>
#include <QRect>
#include <QThread>
#include <QVector>
#include <QWidget>

// 前向声明：只告诉编译器"存在这些类"，减少头文件依赖（不需要完整定义）
class AlarmActuator;
class Ap3216cSensor;
class CaptureThread;
class EventStore;
class Icm20608Sensor;
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

// final 阻止其他类再继承 CameraStreamingPage（设计上不再需要子类）
class CameraStreamingPage final : public QWidget
{
Q_OBJECT // Qt 元对象宏：启用信号槽、反射等 Qt 特性

    public :
    // 构造函数：parent 是 Qt 对象树中的父控件（如 QStackedWidget 的某个页面）
    explicit CameraStreamingPage(QWidget *parent = nullptr);
    // 析构函数：停止所有后台线程、保存配置、释放资源
    ~CameraStreamingPage() override;

signals: // ===== 向外发送的信号（所有信号函数由 MOC 自动生成实现） =====
    /*
     * ===== 跨线程信号（发往独立工作线程） =====
     * 这些信号通过 Qt::QueuedConnection 跨线程传递配置参数，
     * 避免主线程直接操作工作线程的对象。
     */

    // 通知 TcpFrameSender 更新目标 IP/端口/启停状态
    // transport: "tcp" 或 "udp"
    void senderConfigurationChanged(const QString &host, quint16 port,
                                    bool enabled, const QString &transport);
    // 通知 TcpFrameSender 开始录像
    // filePath: 录像文件保存路径，durationMs: 录像时长（0 表示手动录像不限时长）
    void recordingRequested(const QString &filePath, int durationMs);
    // 通知 TcpFrameSender 停止当前录像
    void recordingStopRequested();
    // 通知 VisionEventDetector 更新运动检测参数
    // enabled: 是否启用运动检测，changedPixelPercent: 像素变化触发阈值
    void detectorConfigurationChanged(bool enabled,
                                      int changedPixelPercent);
    // 通知 VisionEventDetector 更新人脸检测参数及模型路径
    // classifierPath: 级联分类器 XML 路径
    // dnnConfigurationPath: DNN 配置文件路径
    // dnnModelPath: DNN 模型权重文件路径
    // inferenceInterval: AI 推理间隔（每 N 帧推理一次）
    // confidencePercent: 人脸检测置信度百分比
    void faceDetectorConfigurationChanged(
        bool enabled, const QString &classifierPath,
        const QString &dnnConfigurationPath, const QString &dnnModelPath,
        int inferenceInterval, int confidencePercent);

protected:
    // resizeEvent 重写：窗口大小变化时重新缩放预览画面（调用 displayPreviewImage）
    void resizeEvent(QResizeEvent *event) override;

private slots: // ===== 私有槽函数（响应各种信号） =====
    /*
     * ===== 采集回调 =====
     */
    // 本地预览帧到达：image 是来自 V4L2 采集线程的深拷贝 QImage
    void updateLocalPreview(const QImage &image);
    // 采集启停时更新按钮文字："开始采集" ↔ "停止采集"
    void updateCaptureButtonText(bool capturing);
    // 按预览/检测状态决定是否需要向视觉线程分发分析帧
    void updateAnalysisFrameDistribution();

    /*
     * ===== 网络推流 =====
     */
    // H.264/RTP 推流 CheckBox 切换时触发
    void networkStreamingToggled(bool enabled);
    // IP/端口编辑完成后调用：把 IP 和端口下发到 TcpFrameSender 并保存配置
    void applyServerEndpoint();

    /*
     * ===== 视觉检测 =====
     */
    // 运动检测 CheckBox 切换：enabled 为新的选中状态
    void visionDetectionToggled(bool enabled);
    // 人脸检测 CheckBox 切换：enabled 为新的选中状态
    void faceDetectionToggled(bool enabled);

    /*
     * ===== 传感器 =====
     */
    // 传感器监控 CheckBox 切换：enabled 为新的选中状态
    void sensorMonitoringToggled(bool enabled);

    /*
     * ===== MQTT =====
     */
    // MQTT 连接 CheckBox 切换：enabled 为新的选中状态
    void mqttMonitoringToggled(bool enabled);

    /*
     * ===== 视觉检测结果处理 =====
     */
    // 每帧分析完成：image 为分析后的图像，changedRatio 为像素变化比例
    void handleAnalysisCompleted(const QImage &image, double changedRatio);
    // 检测到画面运动事件（触发告警）：changedRatio 为变化比例
    void handleVisualMotionDetected(const QImage &image,
                                    double changedRatio);
    // 人脸分析完成：faceBoxes 为检测到的人脸坐标框列表
    void handleFaceAnalysisCompleted(const QImage &image,
                                     const QVector<QRect> &faceBoxes);
    // 检测到人脸存在：faceCount 为检测到的人脸数量
    void handleFacePresenceDetected(const QImage &image, int faceCount);
    // 人脸检测器状态变化：available 表示检测器是否可用，message 为状态描述
    void handleFaceDetectionStatusChanged(bool available,
                                          const QString &message);

    /*
     * ===== 传感器规则引擎 =====
     */
    // 检查光强/接近传感器的阈值规则并触发相应事件
    void updateAp3216cRules();
    // 检查加速度/温度传感器的阈值规则并触发相应事件
    void updateIcm20608Rules();

    /*
     * ===== 手动操作 =====
     */
    // 手动抓拍当前画面（保存 latestFrame 为 JPEG 文件）
    void takeManualSnapshot();
    // 手动录像启停：enabled 为 true 开始录像，false 停止
    void manualRecordingToggled(bool enabled);

    /*
     * ===== MQTT 心跳与性能面板 =====
     */
    // 定时向 MQTT 服务器发布设备心跳状态（每 10 秒）
    void publishHeartbeat();
    // 录像状态变更：active 为是否正在录像，filePath 为录像文件路径
    void handleRecordingStateChanged(bool active, const QString &filePath);
    // 录像出错处理：message 为错误描述
    void handleRecordingError(const QString &message);
    // 每秒刷新一次性能监控面板（FPS、CPU、内存等指标）
    void updatePerformancePanel();

private: // ===== 私有成员函数 =====
    // 构建全部 UI 控件（按钮、标签、输入框、布局等）
    void initializeUi();
    // 创建子线程组件（CaptureThread/TcpFrameSender/VisionEventDetector）并连接信号槽
    void initializeServices();
    // 初始化 MQTT 客户端（连接到云端 IoT 平台）
    void initializeMqtt();
    // 初始化板载传感器（AP3216C 光强、ICM20608 IMU）
    void initializeSensors();
    // 从 QSettings 加载上次保存的持久化配置（IP、端口、阈值等）
    void loadSettings();
    // 将当前 UI 配置保存到 QSettings（析构时和参数变更时调用）
    void saveSettings() const;
    // 将 QImage 等比例缩放到 QLabel 尺寸并显示到预览区域
    void displayPreviewImage(const QImage &image);
    // 保存抓拍图片到磁盘
    // eventType: 事件类型（用于生成文件名），返回保存后的文件路径
    QString saveSnapshot(const QImage &image, const QString &eventType) const;
    // 生成录像文件完整路径
    // eventType: 事件类型，返回 "录像目录/日期_事件类型_时间.avi"
    QString createRecordingPath(const QString &eventType) const;
    // 记录一条监控事件（写入 SQLite + 触发录像/告警等联动）
    // eventType: 事件类型，severity: 严重程度，source: 事件来源
    // message: 事件描述，eventImage: 附带截图，requestRecording: 是否需要录像
    void recordMonitoringEvent(const QString &eventType,
                               MonitoringEventSeverity severity,
                               const QString &source,
                               const QString &message,
                               const QImage &eventImage,
                               bool requestRecording);
    // 将事件添加到 UI 的事件列表中
    void addEventToList(const MonitoringEvent &event);
    // 检查某个规则的冷却时间是否已过
    // ruleName: 规则名称（如 "motion"），cooldownMs: 冷却时长（毫秒）
    // 返回 true 表示冷却已过可以触发
    bool ruleCooldownElapsed(const QString &ruleName, int cooldownMs);
    // 更新传感器状态标签（汇总光强、IMU、温度等信息）
    void updateSensorStatusLabel();
    // 向 MQTT 发布 JSON 格式消息
    void publishMqttJson(const QString &topic, const QJsonObject &object);
    // 处理从 MQTT 收到的远程命令（如远程抓拍、重启等）
    void handleMqttCommand(const QByteArray &payload);
    // 生成完整的 MQTT 主题路径（设备ID + 后缀）
    QString mqttTopic(const QString &suffix) const;

    /*
     * ========== UI 控件（主线程拥有，不要跨线程直接访问） ==========
     */
    QLabel *cameraPreviewLabel;             // 摄像头实时预览画面（QLabel 显示 QPixmap）
    QPushButton *startCaptureButton;        // 采集启停按钮（点击切换文字"开始"/"停止"）
    QPushButton *manualSnapshotButton;      // 手动抓拍按钮
    QPushButton *manualRecordingButton;     // 手动录像按钮（点击切换文字）
    QCheckBox *localPreviewCheckBox;        // 本地预览开关 CheckBox
    QCheckBox *networkStreamingCheckBox;    // H.264/RTP 推流开关
    QCheckBox *visionDetectionCheckBox;     // 运动检测开关 CheckBox
    QCheckBox *faceDetectionCheckBox;       // 人脸检测开关 CheckBox
    QCheckBox *sensorMonitoringCheckBox;    // 传感器监控开关 CheckBox
    QCheckBox *alarmLinkageCheckBox;        // LED/蜂鸣器告警联动开关
    QCheckBox *automaticRecordingCheckBox;  // 事件自动录像开关
    QCheckBox *mqttEnabledCheckBox;         // MQTT 连接开关
    QComboBox *streamTransportComboBox;     // TCP/UDP 推流方式
    QLineEdit *serverAddressEdit;           // 推流目标地址输入框
    QSpinBox *serverPortSpinBox;            // TCP/UDP 推流目标端口号
    QLineEdit *mqttHostEdit;                // MQTT 服务器 IP 地址
    QSpinBox *mqttPortSpinBox;              // MQTT 服务器端口（默认 1883）
    QSpinBox *motionThresholdSpinBox;       // 运动检测像素变化阈值（1-80%）
    QSpinBox *faceInferenceIntervalSpinBox; // 人脸 AI 推理间隔（每 N 帧检测一次）
    QSpinBox *faceConfidenceSpinBox;        // 人脸检测置信度阈值（30-95%）
    QLabel *connectionStatusLabel;          // TCP/UDP 推流状态文字
    QLabel *mqttStatusLabel;                // MQTT 连接状态文字
    QLabel *sensorStatusLabel;              // 传感器数据汇总显示（光强/IMU/温度）
    QLabel *visionStatusLabel;              // 运动检测状态文字："运动中"/"静止"
    QLabel *faceStatusLabel;                // 人脸检测状态文字："检测到 3 人脸"
    QLabel *recordingStatusLabel;           // 录像状态文字："正在录像"/"空闲"
    QLabel *performanceStatusLabel;         // 性能监控面板（FPS/CPU/内存）
    QListWidget *eventList;                 // 最近事件列表（最多显示 N 条）

    /*
     * ========== 后台线程与组件 ==========
     */
    CaptureThread *cameraCaptureThread;  // V4L2 摄像头采集线程（内部包含独立的 QThread 循环）
    QThread *networkWorkerThread;        // 网络推流工作线程（QObject 线程，通过 moveToThread 迁移）
    TcpFrameSender *frameSender;         // H.264 编码 + TCP/UDP 发送器（生活在网络线程）
    QThread *visionWorkerThread;         // 视觉检测工作线程（QObject 线程）
    VisionEventDetector *visionDetector; // OpenCV 运动检测 + 人脸检测（生活在 visionWorkerThread）
    QThread *archiveWorkerThread;        // PC媒体归档下载服务线程
    MediaArchiveServer *archiveServer;   // 抓拍/录像列表与分块下载 HTTP 服务（监听 8890 端口）
    EventStore *eventStore;              // SQLite 事件数据库存储
    AlarmActuator *alarmActuator;        // LED 闪烁 + 蜂鸣器鸣叫执行器
    Ap3216cSensor *ambientLightSensor;   // AP3216C 光强/接近/红外传感器（仅在 ARM 平台可用）
    Icm20608Sensor *motionSensor;        // ICM20608 6轴 IMU 传感器（仅在 ARM 平台可用）
    QMqttClient *mqttClient;             // MQTT 客户端（遥测上报 + 接收远程命令）
    QTimer *heartbeatTimer;              // MQTT 心跳定时器（每隔 10 秒 publishHeartbeat）
    QTimer *mqttReconnectTimer;          // MQTT 断线自动重连定时器（3 秒间隔）
    QTimer *performanceTimer;            // 性能面板刷新定时器（每秒）

    /*
     * ========== 状态缓存 ==========
     */
    QString deviceId;                         // 设备唯一标识（默认 "imx6ull-001"）
    QString currentRecordingPath;             // 当前录像文件的完整路径
    QString ambientLightValue;                // 光强值字符串（从 AP3216C 传感器读取）
    QString proximityValue;                   // 接近传感器值字符串
    QString infraredValue;                    // 红外传感器值字符串
    QString accelerationSummary;              // 加速度汇总字符串（x/y/z g）
    QString temperatureValue;                 // 芯片温度字符串（℃）
    QImage latestFrame;                       // 最新一帧画面的缓存（用于手动抓拍和传感器事件截图）
    QVector<QRect> latestFaceBoxes;           // 最新人脸检测框坐标列表（用于叠加显示）
    QHash<QString, qint64> lastRuleEventTime; // 规则冷却时间记录表：规则名 → 上次触发时间戳
                                              // 防止同一传感器事件在短时间内频繁触发（如每50ms触发一次告警）
    bool eventStoreReady;                     // SQLite 事件数据库是否已初始化就绪
    bool eventRecordingActive;                // 事件触发的录像是否正在进行中
    bool manualRecordingRequested;            // 是否由用户手动触发的录像（与事件录像区分）
    int detectedFaceCount;                    // 当前帧检测到的人脸数量

    /*
     * ========== 性能计算缓存（差分法计算实时 FPS） ==========
     * 每次 updatePerformancePanel() 采样当前的累加计数器值，
     * 与上一次采样的对应值做差，除以时间间隔得到实时速率。
     * 例如：采集 FPS = (本次采集帧数 - 上次采集帧数) / 时间间隔秒数
     */
    qint64 previousPerformanceSampleMs;    // 上次采样的时间戳（毫秒）
    quint64 previousCapturedFrames;        // 上次采集帧累加数
    quint64 previousPreviewFrames;         // 上次预览帧累加数
    quint64 previousAnalysisFrames;        // 上次分析帧累加数
    quint64 previousSentFrames;            // 上次网络发送帧累加数
    quint64 previousSentBytes;             // 上次网络发送字节累加数
    quint64 previousEncodedFrames;         // 上次 H.264 编码帧累加数
    quint64 previousEncodeMicroseconds;    // 上次 H.264 编码耗时累加数（微秒）
    quint64 previousSystemCpuTicks;        // 上次读取的 /proc/stat CPU 总 tick 数
    quint64 previousProcessCpuTicks;       // 上次读取的 /proc/self/stat 进程 CPU tick 数
};

#if 0
/* 学习版停用成员的原声明备份；恢复时移回 CameraStreamingPage 私有成员区。 */
quint64 previousInferenceRuns;
quint64 previousInferenceMicroseconds;
#endif

#endif // VIDEO_MONITOR_H
