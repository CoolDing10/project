/*
 * ============================================================================
 * video_monitor.cpp — 边缘监控页面（CameraStreamingPage）完整实现
 * ============================================================================
 *
 * 本文件是系统最核心、最复杂的实现文件（约 2000 行）。
 * CameraStreamingPage 是系统的"导演/编排器"角色。
 *
 * 代码结构导航：
 *   1. 匿名命名空间   — 系统常量、辅助函数（文件搜索、CPU 读取、自动清理）
 *   2. 构造函数       — 创建所有 UI 控件和后台组件
 *   3. initializeUi() — 搭建 6 组 GroupBox 布局
 *   4. initializeServices()— 创建工作线程 + moveToThread + 信号连接（第2周核心）
 *   5. initializeMqtt()    — MQTT 客户端初始化
 *   6. initializeSensors() — 板载 I2C 传感器初始化（边缘监控学习版停用）
 *   7. loadSettings()      — 从 QSettings 恢复配置
 *   8. saveSettings()      — 保存配置到 QSettings
 *   9. updateLocalPreview()/displayPreviewImage() — 第2周核心：帧显示
 *  10. updatePerformancePanel() — 第2周核心：性能监控
 *  11. resizeEvent()/updateCaptureButtonText()
 *  12. 网络推流槽函数
 *  13. 视觉检测槽函数
 *  14. 传感器规则引擎
 *  15. 手动操作（抓拍/录像）
 *  16. MQTT 心跳与远程命令
 *  17. 析构函数
 *
 * 数据流图（第2周核心链路）：
 *   V4L2 → CaptureThread::run()
 *     ├── localPreviewReady → updateLocalPreview() [主线程]
 *     │     └── displayPreviewImage() → cameraPreviewLabel
 *     ├── networkFrameReady → TcpFrameSender::sendFrame() [独立线程]
 *     └── analysisFrameReady → VisionEventDetector::analyzeFrame() [独立线程]
 *       ↑ 每帧一次                  ↑ 到达确认 frameHandled
 *       └── 背压门控 ──────────────┘
 *
 *   事件 → EventStore(SQLite) + 自动事件录像 + MQTT push
 */
#include "video_monitor.h"

// 硬件抽象层：采集与传感器驱动
#include "ap3216c.h"        // AP3216C 光强/接近/红外传感器（仅 ARM）
#include "capture_thread.h" // V4L2 摄像头采集线程
#include "icm20608.h"       // ICM20608 6轴 IMU（加速度 + 温度）
// 网络与存储层
#include "tcp_frame_sender.h"     // FFmpeg H.264 编码 + TCP/UDP 推流器
#include "event_store.h"          // SQLite 事件存储
#include "media_archive_server.h" // PC端抓拍/录像归档服务
// 视觉检测层
#include "vision_event_detector.h" // 运动检测 + 人脸检测（OpenCV）

// MQTT 通信（需要 qtmqtt 模块）
#include <mqtt/qmqttclient.h>

// Qt Widgets：UI 控件
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
// Qt JSON 处理
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMetaObject>
#include <QPixmap>
#include <QPainter>
#include <QPen>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QtMath>

// Linux 系统调用（用于读取 /proc 文件系统获取 CPU/内存信息）
#if defined(__linux__)
#include <unistd.h>
#endif

/*
 * ===== 匿名命名空间：系统常量与辅助函数 =====
 * 匿名命名空间中的内容仅在当前 .cpp 文件中可见（相当于 C 中的 static 函数/变量）。
 * linker 不会导出这些符号，避免命名冲突。
 */
namespace
{
    /*
     * ===== 事件与告警阈值常量 =====
     * 这些值是经过实际测试调优的经验值，可根据应用场景调整。
     */

    // 事件触发时自动录像 10 秒（由 TcpFrameSender 的定时器控制停止）
    // 10000 ms = 10 秒
    const int EventRecordingDurationMs = 10000;
    // UI 事件列表最多保留 100 条，超出的旧记录自动删除，防止内存无限增长
    const int MaximumVisibleEvents = 100;
    // AP3216C 接近传感器：读数 >= 100 表示有物体非常靠近设备
    const int ProximityAlertThreshold = 100;
    // ICM20608 加速度计：|合加速度 - 1.0g| >= 0.55g 判定为异常震动
    // 静止时合加速度约为 1.0g（仅重力），碰撞/跌落时显著偏离
    const double VibrationAlertThreshold = 0.55;
    // ICM20608 温度传感器：超过 60℃ 触发过热告警
    const double TemperatureAlertThreshold = 60.0;
    // 传感器事件冷却：同一类型事件 15 秒内不重复触发，避免告警风暴
    const int SensorEventCooldownMs = 15000;
    // 温度事件冷却时间更长（60 秒），因为温度变化通常较缓慢
    const int TemperatureEventCooldownMs = 60000;

    /*
     * locateProjectAsset() — 在多个候选目录中搜索项目资源文件
     * 参数 relativePath：相对于项目根目录的路径（如 "models/face.xml"）
     * 返回第一个存在文件的绝对路径，都不存在时返回第一个候选路径。
     *
     * 桌面运行和 ARM 运行时的可执行文件路径不同，所以需要 fallback。
     * 搜索顺序：exe 目录 → 当前目录 → exe/../project → cwd/../project
     */
    QString locateProjectAsset(const QString &relativePath)
    {
        const QStringList roots = {                                        // 候选搜索根目录列表
                                   QCoreApplication::applicationDirPath(), // ① 可执行文件所在目录
                                   QDir::currentPath(),                    // ② 进程当前工作目录
                                   QDir::cleanPath(QCoreApplication::applicationDirPath() +
                                                   QStringLiteral("/../project")),                        // ③ exe 上一级的 project 目录
                                   QDir::cleanPath(QDir::currentPath() + QStringLiteral("/../project"))}; // ④ cwd 上一级的 project 目录
        for (const QString &root : roots)                                                                 // 遍历所有候选根目录
        {
            const QString candidate = QDir(root).filePath(relativePath); // 拼接根目录 + 相对路径
            if (QFile::exists(candidate))                                // 文件是否存在？
                return QDir::cleanPath(candidate);                       // 存在 → 返回规范化的绝对路径
        }
        return QDir(roots.first()).filePath(relativePath); // 都不存在 → 返回第一个候选路径（调用方自行检查）
    }

    /*
     * severityText() — 将事件严重等级枚举转换为 UI 可显示的中文文本
     * 参数 severity：MonitoringEventSeverity 枚举值
     * 返回对应的中文字符串
     */
    QString severityText(MonitoringEventSeverity severity)
    {
        switch (severity) // 根据枚举值分支
        {
        case MonitoringEventSeverity::Critical:
            return QStringLiteral("严重"); // 需要立即关注
        case MonitoringEventSeverity::Warning:
            return QStringLiteral("警告"); // 需要关注
        case MonitoringEventSeverity::Information:
        default:
            return QStringLiteral("信息"); // 常规通知
        }
    }

    /*
     * severityColor() — 将事件严重等级映射到 UI 显示颜色
     * 参数 severity：MonitoringEventSeverity 枚举值
     * 返回对应的 QColor
     *   Critical     → 红色 #ff5555（需要立即关注）
     *   Warning      → 橙色 #ffaa33（需要关注）
     *   Information  → 蓝色 #55ccff（常规通知）
     */
    QColor severityColor(MonitoringEventSeverity severity)
    {
        switch (severity) // 根据枚举值分支
        {
        case MonitoringEventSeverity::Critical:
            return QColor(QStringLiteral("#ff5555")); // 红色
        case MonitoringEventSeverity::Warning:
            return QColor(QStringLiteral("#ffaa33")); // 橙色
        case MonitoringEventSeverity::Information:
        default:
            return QColor(QStringLiteral("#55ccff")); // 蓝色
        }
    }

    /*
     * readCpuTicks() — 通过 /proc 文件系统读取 CPU 时间片计数
     * 参数 systemTicks：输出参数，系统总 CPU 时间片（所有核）
     * 参数 processTicks：输出参数，当前进程 CPU 时间片
     * 返回 true 表示读取成功。
     *
     * Linux /proc 文件系统提供进程级别的 CPU 使用统计：
     *   - /proc/stat    第一行 "cpu user nice system idle ..." 是所有 CPU 核的总时间片
     *   - /proc/self/stat 当前进程的统计信息，格式为：
     *     pid (comm) state ppid ... utime stime ...
     *     其中 utime（第14字段，索引 11 从 0 计）是用户态时间片，
     *     stime（第15字段，索引 12 从 0 计）是内核态时间片
     *
     * 相邻两次采样做差后，除以 sysconf(_SC_CLK_TCK)（通常为 100），
     * 再乘以 CPU 核数，可得 CPU 使用率百分比。
     *
     * 注意：/proc/self/stat 中 comm 字段可能包含空格和括号，
     * 所以需要先找到最后一个 ')' 来确定命令名的结束位置。
     */
    bool readCpuTicks(quint64 *systemTicks, quint64 *processTicks)
    {
#if !defined(__linux__)
        // 非 Linux 平台（如 Windows/macOS）无法使用 /proc 文件系统
        Q_UNUSED(systemTicks) // 标记参数已使用（消除编译警告）
        Q_UNUSED(processTicks)
        return false; // 返回 false 表示读取失败
#else
        QFile systemStat(QStringLiteral("/proc/stat"));       // 打开系统 CPU 统计文件
        QFile processStat(QStringLiteral("/proc/self/stat")); // 打开当前进程统计文件
        if (!systemStat.open(QIODevice::ReadOnly) ||          // 只读方式打开（两个都成功才继续）
            !processStat.open(QIODevice::ReadOnly))
            return false; // 任意文件打不开 → 失败

        // 解析 /proc/stat 第一行：cpu  total_ticks ...（空格分隔）
        const QList<QByteArray> systemFields =
            systemStat.readLine().simplified().split(' '); // 读第一行 → 压缩空白 → 按空格拆分
        if (systemFields.size() < 5 || systemFields.first() != "cpu")
            return false; // 至少要有 cpu + 4 个数字字段
        // 累加 user + nice + system + idle + iowait + irq + softirq + steal（从索引 1 开始）
        quint64 total = 0;
        for (int index = 1; index < systemFields.size(); ++index)
            total += systemFields.at(index).toULongLong(); // 累加每个字段的数值（无符号 64 位）

        // 解析 /proc/self/stat：需要跳过进程名中的括号
        const QByteArray processLine = processStat.readAll(); // 读取全部内容
        // 找到最后一个 ')'，其后第 2 个字段是 state，再往后才是 utime/stime
        const int commandEnd = processLine.lastIndexOf(')'); // 查找最后一个右括号的位置
        if (commandEnd < 0)
            return false; // 找不到括号 → 格式异常
        // 跳过 ") " 后拆分剩余字段（mid 从 commandEnd+2 开始跳过 ") "）
        const QList<QByteArray> processFields =
            processLine.mid(commandEnd + 2).simplified().split(' ');
        if (processFields.size() <= 12)
            return false; // 至少需要 13 个字段

        *systemTicks = total; // 写入输出参数：系统总 tick
        // 进程 CPU 时间 = 用户态 + 内核态（utime[11] + stime[12]，从 0 开始计）
        *processTicks = processFields.at(11).toULongLong() + // utime
                        processFields.at(12).toULongLong();  // stime
        return true;                                         // 成功返回
#endif
    }

    /*
     * processResidentMemoryMb() — 通过 /proc/self/statm 读取进程驻留内存（RSS）
     * 返回驻留内存大小（MB）。
     *
     * /proc/self/statm 格式：size resident shared text lib data dt
     *   第 1 字段（索引 0）：进程总虚拟内存页面数
     *   第 2 字段（索引 1）：驻留物理内存页面数（RSS）  ← 我们关心的
     *
     * 计算公式：RSS(MB) = 页面数 × 页面大小 / 1024 / 1024
     *   sysconf(_SC_PAGESIZE) 通常返回 4096（4KB）
     */
    double processResidentMemoryMb()
    {
#if !defined(__linux__)
        return 0.0; // 非 Linux 平台返回 0
#else
        QFile statm(QStringLiteral("/proc/self/statm")); // 打开进程内存统计文件
        if (!statm.open(QIODevice::ReadOnly))
            return 0.0;                                                           // 打开失败 → 返回 0
        const QList<QByteArray> fields = statm.readAll().simplified().split(' '); // 按空格拆分
        if (fields.size() < 2)
            return 0.0; // 至少需要 2 个字段
        // 第 2 字段（索引 1）是进程驻留物理页面数
        const quint64 residentPages = fields.at(1).toULongLong();
        // 页面数 × 页面大小（字节） → ÷ (1024×1024) = MB
        return double(residentPages) * double(sysconf(_SC_PAGESIZE)) /
               (1024.0 * 1024.0);
#endif
    }
} // 匿名命名空间结束

/*
 * ===== CameraStreamingPage 构造函数 =====
 * 参数 parent：Qt 父控件指针（通常是 QStackedWidget 的某个页面）
 *
 * 执行顺序（四步）：
 *   1. 初始化列表：所有指针 → nullptr，基本类型 → 0
 *   2. qRegisterMetaType：注册跨线程信号槽所需的元类型
 *   3. initializeUi()：创建所有 UI 控件和 GroupBox 布局
 *   4. loadSettings()：从 QSettings 恢复上次保存的配置
 *   5. initializeServices()：创建工作线程 + 信号连接（第2周核心）
 *
 * 初始化列表说明：
 *   所有指针初始化为 nullptr，确保在构造函数中途失败时析构安全。
 *   基本类型初始化为 0，性能采样初始值置零以保证首次计算正确。
 */
CameraStreamingPage::CameraStreamingPage(QWidget *parent)
    : QWidget(parent),                       // 调用 QWidget 基类构造函数
                                             // ---- UI 控件初始化为 nullptr（确保构造中途失败时析构安全）----
      cameraPreviewLabel(nullptr),           // 摄像头实时预览 QLabel
      startCaptureButton(nullptr),           // 采集启停按钮
      manualSnapshotButton(nullptr),         // 手动抓拍按钮
      manualRecordingButton(nullptr),        // 手动录像按钮
      localPreviewCheckBox(nullptr),         // 本地预览开关
      networkStreamingCheckBox(nullptr),     // TCP 推流开关
      visionDetectionCheckBox(nullptr),      // 运动检测开关
      faceDetectionCheckBox(nullptr),        // 人脸检测开关
      sensorMonitoringCheckBox(nullptr),     // 传感器监控开关
      alarmLinkageCheckBox(nullptr),         // LED/蜂鸣器告警联动开关
      automaticRecordingCheckBox(nullptr),   // 事件自动录像开关
      mqttEnabledCheckBox(nullptr),          // MQTT 连接开关
      streamTransportComboBox(nullptr),      // TCP/UDP 推流方式
      serverAddressEdit(nullptr),            // TCP 推流目标 IP
      serverPortSpinBox(nullptr),            // TCP 推流端口
      mqttHostEdit(nullptr),                 // MQTT 服务器 IP
      mqttPortSpinBox(nullptr),              // MQTT 端口
      motionThresholdSpinBox(nullptr),       // 运动检测阈值
      faceInferenceIntervalSpinBox(nullptr), // 人脸推理间隔
      faceConfidenceSpinBox(nullptr),        // 人脸置信度
      connectionStatusLabel(nullptr),        // TCP 连接状态
      mqttStatusLabel(nullptr),              // MQTT 状态
      sensorStatusLabel(nullptr),            // 传感器状态
      visionStatusLabel(nullptr),            // 运动检测状态
      faceStatusLabel(nullptr),              // 人脸检测状态
      recordingStatusLabel(nullptr),         // 录像状态
      performanceStatusLabel(nullptr),       // 性能面板
      eventList(nullptr),                    // 事件列表
      // ---- 后台线程与组件初始化为 nullptr ----
      cameraCaptureThread(nullptr), // V4L2 采集线程
      networkWorkerThread(nullptr), // 网络推流工作线程
      frameSender(nullptr),         // TCP 帧发送器
      visionWorkerThread(nullptr),  // 视觉检测工作线程
      visionDetector(nullptr),      // 视觉检测器
      archiveWorkerThread(nullptr), // 媒体归档线程
      archiveServer(nullptr),       // 归档服务
      eventStore(nullptr),          // SQLite 事件存储
      alarmActuator(nullptr),       // 告警执行器
      ambientLightSensor(nullptr),  // AP3216C 光强传感器
      motionSensor(nullptr),        // ICM20608 IMU 传感器
      mqttClient(nullptr),          // MQTT 客户端
      heartbeatTimer(nullptr),      // MQTT 心跳定时器
      mqttReconnectTimer(nullptr),  // MQTT 重连定时器
      performanceTimer(nullptr),    // 性能面板刷新定时器
      // ---- 状态变量初始化 ----
      deviceId(QStringLiteral("imx6ull-001")), // 设备唯一标识（MQTT 主题中使用）
      eventStoreReady(false),                  // SQLite 数据库就绪标志
      eventRecordingActive(false),             // 当前是否有事件录像在进行
      manualRecordingRequested(false),         // 用户是否发起了手动录像
      detectedFaceCount(0),                    // 当前检测到的人脸数量
      // ---- 性能采样历史值（均为 0，首次采样时直接使用当前值）----
      previousPerformanceSampleMs(0),   // 上次性能采样时间戳
      previousCapturedFrames(0),        // 上次采集帧累加数
      previousPreviewFrames(0),         // 上次预览帧累加数
      previousAnalysisFrames(0),        // 上次分析帧累加数
      previousSentFrames(0),            // 上次网络发送帧累加数
      previousSentBytes(0),             // 上次网络发送字节累加数
      previousEncodedFrames(0),         // 上次 H.264 编码帧累加数
      previousEncodeMicroseconds(0),    // 上次编码耗时累加数（微秒）
      previousSystemCpuTicks(0),        // 上次系统 CPU tick 数
      previousProcessCpuTicks(0)        // 上次进程 CPU tick 数
{
    initializeUi();       // 步骤 1：创建所有 UI 控件并布局
    loadSettings();       // 步骤 2：从 QSettings 加载上次保存的配置
    initializeServices(); // 步骤 3：创建子线程 + 连接信号槽（第 2 周核心）
}

/*
 * ===== 析构函数：安全退出所有线程 + 保存配置 =====
 *
 * 线程退出顺序（非常重要，不能乱）：
 *   1. 先停传感器轮询（避免在传感器回调中访问已析构对象）
 *   2. 停所有定时器
 *   3. 断 MQTT 连接
 *   4. 停采集线程（CaptureThread），wait() 等它退出
 *   5. 停视觉检测线程（quit + wait）
 *   6. 停媒体归档服务线程
 *   7. 停网络推流线程（通过 BlockingQueuedConnection 调用 stop + quit + wait）
 *   8. 最后 delete eventStore
 */
CameraStreamingPage::~CameraStreamingPage()
{
    saveSettings(); // 保存 CheckBox、IP、端口等配置到 QSettings
    // 先停传感器轮询（避免回调中访问已析构的 this）
    if (ambientLightSensor)
        ambientLightSensor->setPollingEnabled(false); // 停止 AP3216C 轮询
    if (motionSensor)
        motionSensor->setPollingEnabled(false); // 停止 ICM20608 轮询
    // 停所有定时器
    if (heartbeatTimer)
        heartbeatTimer->stop(); // 停止 MQTT 心跳定时器
    if (mqttReconnectTimer)
        mqttReconnectTimer->stop(); // 停止 MQTT 重连定时器
    if (performanceTimer)
        performanceTimer->stop(); // 停止性能面板刷新定时器
    // 断 MQTT 连接
    if (mqttClient && mqttClient->state() != QMqttClient::Disconnected)
        mqttClient->disconnectFromHost(); // 主动断开 MQTT 连接

    // 停止采集线程（先停采集，再处置消费线程，避免生产者向已死的消费者发信号）
    cameraCaptureThread->setCaptureEnabled(false); // ① 置采集标志为 false → while 循环退出
    cameraCaptureThread->wait();                   // ② 阻塞等待 run() 完全结束

    // 停止视觉检测线程
    if (visionWorkerThread->isRunning())
    {
        visionWorkerThread->quit(); // 请求退出事件循环
        visionWorkerThread->wait(); // 等待线程结束
    }
    // 停止媒体归档服务线程
    if (archiveWorkerThread && archiveWorkerThread->isRunning())
    {
        QMetaObject::invokeMethod(archiveServer, "stop",         // 跨线程同步调用 stop() 方法
                                  Qt::BlockingQueuedConnection); // 阻塞直到目标线程执行完毕
        archiveWorkerThread->quit();                             // 退出事件循环
        archiveWorkerThread->wait();                             // 等待线程退出
    }
    // 停止网络推流线程（跨线程同步调用 stop，确保资源安全释放）
    if (networkWorkerThread->isRunning())
    {
        QMetaObject::invokeMethod(frameSender, "stop",           // 跨线程同步调用 stop() 方法
                                  Qt::BlockingQueuedConnection); // 阻塞直到目标线程执行完毕
        networkWorkerThread->quit();                             // 退出事件循环
        networkWorkerThread->wait();                             // 等待线程退出
    }

    delete eventStore;    // 释放 SQLite 事件存储（最后释放）
    eventStore = nullptr; // 置空防止野指针
}

/*
 * ===== initializeUi() — 构建完整的监控页面 UI 布局 =====
 *
 * 页面布局结构（水平分割）：
 *   ┌─────────────────────────────────────────────────────────┐
 *   │  previewPanel（左侧，stretch=1）  │  controlScrollArea   │
 *   │  ┌─────────────────────────────┐  │  （右侧，固定宽度）   │
 *   │  │ previewHeader               │  │  ┌────────────────┐  │
 *   │  │  + cameraPreviewLabel       │  │  │ streamGroup    │  │
 *   │  │    （摄像头实时画面）          │  │  │ monitoringGroup│  │
 *   │  └─────────────────────────────┘  │  │ performance    │  │
 *   │                                  │  │ mqttGroup      │  │
 *   │                                  │  │ eventGroup     │  │
 *   │                                  │  └────────────────┘  │
 *   └─────────────────────────────────────────────────────────┘
 *
 * 当前显示 5 个 GroupBox（从上到下）：
 *   1. streamGroup    — TCP 推流目标配置（IP + 端口 + 状态）
 *   2. monitoringGroup— 核心监控控制（采集/预览/检测/自动事件录像）
 *   3. performance    — 实时性能面板（FPS/CPU/内存/丢帧）
 *   4. mqttGroup      — MQTT 连接配置与状态
 *   5. eventGroup     — 最近事件列表
 */
void CameraStreamingPage::initializeUi()
{
    // ===== 页面基础样式（暗色主题样式表） =====
    setObjectName(QStringLiteral("edgeMonitoringPage"));               // 设置对象名（CSS 选择器使用）
    setStyleSheet(QStringLiteral(                                      // 设置暗色主题全局样式表
        "#edgeMonitoringPage { background: #1f1f22; color: #f5f7fa; }" // 页面背景深灰色
        "#edgeControlPanel, #edgeControlScrollArea, "
        "#edgeControlScrollArea > QWidget > QWidget { background: #1f1f22; }"
        "QGroupBox { background: #29292e; color: #f5f7fa; " // GroupBox 深灰色背景
        "border: 1px solid #68686f; border-radius: 6px; "
        "margin-top: 12px; padding: 12px 6px 6px 6px; font-weight: 600; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 9px; "
        "padding: 0 4px; color: #ffffff; }"
        "QLabel { color: #eef0f3; background: transparent; }" // 标签浅色文字
        "QLabel:disabled { color: #aeb3ba; }"
        "QCheckBox { color: #eef0f3; spacing: 8px; background: transparent; }"
        "QCheckBox:disabled { color: #aeb3ba; }"
        "QCheckBox::indicator { width: 24px; height: 24px; }" // 复选框指示器放大（适合触摸屏）
        "QCheckBox::indicator:unchecked { background: #202126; "
        "border: 2px solid #d9dde3; border-radius: 4px; }"
        "QCheckBox::indicator:unchecked:hover { background: #30343a; "
        "border-color: #55b7ff; }"
        "QCheckBox::indicator:unchecked:disabled { background: #33343a; "
        "border-color: #858a91; }"
        "QPushButton { min-height: 34px; border-radius: 6px; background: #e8e8e8; "
        "color: #202020; padding: 3px 8px; }"
        "QPushButton:checked { background: #d9534f; color: white; }" // 按下状态红色
        "QPushButton:disabled { background: #44444a; color: #bfc3c8; }"
        "QLineEdit, QSpinBox { min-height: 28px; background: #ffffff; "
        "color: #17191c; border: 1px solid #85858b; border-radius: 3px; }"
        "QLineEdit:disabled, QSpinBox:disabled { background: #d7d7da; "
        "color: #55585d; }"
        "QListWidget { background: #151515; color: #f5f5f5; "
        "border: 1px solid #68686f; }"
        "#previewPanel { background: #151a21; border: 1px solid #303a47; "
        "border-radius: 9px; }" // 预览面板深蓝灰色
        "#previewTitle { color: #f5f8fb; font-size: 15px; font-weight: 600; }"
        "#previewHint { color: #8291a3; font-size: 11px; }"
        /* 仅放大右侧控制区滚动条，适合 1024x600 触摸屏操作。 */
        "#edgeControlScrollArea QScrollBar:vertical { "
        "background: #202126; width: 22px; margin: 2px; "
        "border: 1px solid #454952; border-radius: 9px; }"
        "#edgeControlScrollArea QScrollBar::handle:vertical { "
        "background: #8f98a6; min-height: 76px; margin: 2px; "
        "border: 1px solid #b7c0cc; border-radius: 7px; }"
        "#edgeControlScrollArea QScrollBar::handle:vertical:hover, "
        "#edgeControlScrollArea QScrollBar::handle:vertical:pressed { "
        "background: #48a9e6; border-color: #82cdf7; }"
        "#edgeControlScrollArea QScrollBar::add-line:vertical, "
        "#edgeControlScrollArea QScrollBar::sub-line:vertical { height: 0; }"
        "#edgeControlScrollArea QScrollBar::add-page:vertical, "
        "#edgeControlScrollArea QScrollBar::sub-page:vertical { "
        "background: transparent; }"));

    // ===== 摄像头预览区域（左侧主画面） =====
    cameraPreviewLabel = new QLabel(QStringLiteral("等待采集画面"), this); // 创建预览 QLabel
    cameraPreviewLabel->setObjectName(QStringLiteral("cameraPreview"));    // 设置对象名（CSS 选择器）
    cameraPreviewLabel->setAlignment(Qt::AlignCenter);                     // 文字居中显示
    cameraPreviewLabel->setMinimumSize(320, 240);                          // 最小尺寸 320×240
    cameraPreviewLabel->setSizePolicy(QSizePolicy::Expanding,              // 水平方向可扩展
                                      QSizePolicy::Expanding);             // 垂直方向可扩展
    cameraPreviewLabel->setStyleSheet(                                     // 预览区域深色背景
        "#cameraPreview { background: #05070a; color: #8793a3; "
        "border: 1px solid #26313d; border-radius: 6px; }");

    // ===== 右侧控制面板容器 =====
    QWidget *controlPanel = new QWidget(this);                       // 控制面板父容器
    controlPanel->setObjectName(QStringLiteral("edgeControlPanel")); // 对象名（CSS 选择器）
    QVBoxLayout *controlLayout = new QVBoxLayout(controlPanel);      // 垂直布局管理器
    controlLayout->setContentsMargins(6, 6, 6, 6);                   // 布局内边距
    controlLayout->setSpacing(10);                                   // 组件间距

    // ===== GroupBox 1: TCP/UDP 推流配置 =====
    QGroupBox *streamGroup = new QGroupBox(QStringLiteral("实时视频推流配置"), controlPanel);
    QFormLayout *streamLayout = new QFormLayout(streamGroup);                                // 表单布局（标签-输入框）
    streamTransportComboBox = new QComboBox(streamGroup);
    streamTransportComboBox->addItem(QStringLiteral("H.264/TCP（可靠传输）"), QStringLiteral("tcp"));
    streamTransportComboBox->addItem(QStringLiteral("H.264/RTP/UDP（低延迟）"), QStringLiteral("udp"));
    serverAddressEdit = new QLineEdit(QStringLiteral("192.168.10.100"), streamGroup);        // IP 输入框（默认值）
    serverPortSpinBox = new QSpinBox(streamGroup);                                           // 端口选择器
    serverPortSpinBox->setRange(1, 65535);                                                   // 端口范围 1-65535
    serverPortSpinBox->setValue(8888);                                                       // 默认端口 8888
    connectionStatusLabel = new QLabel(QStringLiteral("实时推流未开启"), streamGroup);         // 发送状态显示
    connectionStatusLabel->setWordWrap(true);                                                // 允许自动换行
    streamLayout->addRow(QStringLiteral("传输方式"), streamTransportComboBox);
    streamLayout->addRow(QStringLiteral("服务器IP"), serverAddressEdit);                     // 添加 IP 行
    streamLayout->addRow(QStringLiteral("端口"), serverPortSpinBox);                         // 添加端口行
    streamLayout->addRow(QStringLiteral("连接状态"), connectionStatusLabel);                 // 添加状态行

    // ===== GroupBox 2: 监控控制 =====
    QGroupBox *monitoringGroup = new QGroupBox(QStringLiteral("监控控制"), controlPanel);
    QVBoxLayout *monitoringLayout = new QVBoxLayout(monitoringGroup);                            // 垂直布局
    startCaptureButton = new QPushButton(QStringLiteral("开始采集摄像头数据"), monitoringGroup); // 采集按钮
    startCaptureButton->setCheckable(true);                                                      // 按钮可切换状态（按下/弹起）
    localPreviewCheckBox = new QCheckBox(QStringLiteral("本地实时预览"), monitoringGroup);       // 预览开关
    networkStreamingCheckBox = new QCheckBox(QStringLiteral("实时视频推流"), monitoringGroup); // 推流开关
    visionDetectionCheckBox = new QCheckBox(QStringLiteral("视觉变化检测"), monitoringGroup);    // 运动检测
    faceDetectionCheckBox = new QCheckBox(QStringLiteral("人脸识别检测"), monitoringGroup);      // 人脸检测
    sensorMonitoringCheckBox = new QCheckBox(QStringLiteral("传感器规则监控"), monitoringGroup); // 传感器
    alarmLinkageCheckBox = new QCheckBox(QStringLiteral("LED/蜂鸣器报警"), monitoringGroup);     // 告警联动
    automaticRecordingCheckBox = new QCheckBox(QStringLiteral("事件自动录像"), monitoringGroup); // 自动录像
    // 运动检测阈值 SpinBox
    motionThresholdSpinBox = new QSpinBox(monitoringGroup);
    motionThresholdSpinBox->setRange(1, 80);                 // 阈值范围 1%-80%
    motionThresholdSpinBox->setValue(12);                    // 默认阈值 12%
    motionThresholdSpinBox->setSuffix(QStringLiteral(" %")); // 后缀显示 "%"
    // 人脸推理间隔 SpinBox（功能代码保留，学习版不显示、不连接）
    faceInferenceIntervalSpinBox = new QSpinBox(monitoringGroup);
    faceInferenceIntervalSpinBox->setRange(1, 30);                  // 间隔范围 1-30 帧
    faceInferenceIntervalSpinBox->setValue(3);                      // 默认每 3 帧推理一次
    faceInferenceIntervalSpinBox->setSuffix(QStringLiteral(" 帧")); // 后缀 " 帧"
    // 人脸置信度 SpinBox（功能代码保留，学习版不显示、不连接）
    faceConfidenceSpinBox = new QSpinBox(monitoringGroup);
    faceConfidenceSpinBox->setRange(30, 95);                // 置信度范围 30%-95%
    faceConfidenceSpinBox->setValue(60);                    // 默认 60%
    faceConfidenceSpinBox->setSuffix(QStringLiteral(" %")); // 后缀 "%"
    // 手动操作按钮
    manualSnapshotButton = new QPushButton(QStringLiteral("手动抓拍"), monitoringGroup); // 抓拍按钮
    manualRecordingButton = new QPushButton(QStringLiteral("开始手动录像"),              // 录像按钮
                                            monitoringGroup);
    manualRecordingButton->setCheckable(true); // 可切换状态
    // 状态标签
    visionStatusLabel = new QLabel(QStringLiteral("视觉检测已关闭"), // 运动检测状态
                                   monitoringGroup);
    visionStatusLabel->setWordWrap(true);                          // 允许换行
    faceStatusLabel = new QLabel(QStringLiteral("人脸检测已关闭"), // 人脸检测状态
                                 monitoringGroup);
    faceStatusLabel->setWordWrap(true);                                 // 允许换行
    faceStatusLabel->setStyleSheet(QStringLiteral("color:#65d5ff;"));   // 蓝色文字
    recordingStatusLabel = new QLabel(QStringLiteral("事件录像未启动"), // 录像状态
                                      monitoringGroup);
    recordingStatusLabel->setWordWrap(true); // 允许换行
    // 按顺序添加到布局
    monitoringLayout->addWidget(startCaptureButton);        // 采集按钮
    monitoringLayout->addWidget(localPreviewCheckBox);      // 预览开关
    monitoringLayout->addWidget(networkStreamingCheckBox);  // 推流开关
    monitoringLayout->addWidget(visionDetectionCheckBox);   // 运动检测
    // 学习版关闭人脸检测 UI。对象仍由父对象管理，原布局代码移到文件末尾。
    faceDetectionCheckBox->hide();
    faceInferenceIntervalSpinBox->hide();
    faceConfidenceSpinBox->hide();
    faceStatusLabel->hide();
    QFormLayout *thresholdLayout = new QFormLayout();       // 阈值子布局
    thresholdLayout->addRow(QStringLiteral("画面变化阈值"), // 阈值行
                            motionThresholdSpinBox);
    monitoringLayout->addLayout(thresholdLayout);            // 添加阈值子布局
    // 边缘监控页不再显示传感器规则入口；“板级设备”页保持不变。
    sensorMonitoringCheckBox->hide();
    // LED/蜂鸣器、手动抓拍和手动录像入口关闭；自动事件录像保持不变。
    alarmLinkageCheckBox->hide();
    manualSnapshotButton->hide();
    manualRecordingButton->hide();
    monitoringLayout->addWidget(automaticRecordingCheckBox); // 自动录像
    monitoringLayout->addWidget(visionStatusLabel);          // 运动状态
    monitoringLayout->addWidget(recordingStatusLabel);       // 录像状态

    // 旧传感器状态对象仅为以后恢复功能而保留，不加入当前页面布局。
    QGroupBox *sensorGroup = new QGroupBox(QStringLiteral("传感器状态"), controlPanel);
    QVBoxLayout *sensorLayout = new QVBoxLayout(sensorGroup);                        // 垂直布局
    sensorStatusLabel = new QLabel(QStringLiteral("传感器监控已关闭"), sensorGroup); // 传感器状态标签
    sensorStatusLabel->setWordWrap(true);                                            // 允许换行
    sensorLayout->addWidget(sensorStatusLabel);                                      // 添加到布局
    sensorGroup->hide(); // 边缘监控学习版不显示传感器状态；板级设备页不受影响。

    // ===== GroupBox 4: 实时性能监控面板 =====
    QGroupBox *performanceGroup = new QGroupBox(
        QStringLiteral("实时性能"), controlPanel);                      // 性能面板标题
    QVBoxLayout *performanceLayout = new QVBoxLayout(performanceGroup); // 垂直布局
    performanceStatusLabel = new QLabel(                                // 性能数据标签
        QStringLiteral("采集/预览/分析：0.0 / 0.0 / 0.0 FPS\n"          // 初始显示全零
                       "发送：0.0 FPS  0.00 Mbps  H.264 0.0 ms\n"
                       "丢帧(预览/发送/分析)：0 / 0 / 0\n"
                       "CPU：0.0%  内存：0.0 MB"),
        performanceGroup);
    performanceStatusLabel->setWordWrap(true);            // 允许换行
    performanceStatusLabel->setStyleSheet(QStringLiteral( // 等宽字体青色
        "color:#9fe4ff; font-family:monospace; font-size:11px;"));
    performanceLayout->addWidget(performanceStatusLabel); // 添加到布局

    // ===== GroupBox 5: MQTT 设备管理 =====
    QGroupBox *mqttGroup = new QGroupBox(QStringLiteral("MQTT设备管理"), controlPanel);
    QFormLayout *mqttLayout = new QFormLayout(mqttGroup);                       // 表单布局
    mqttHostEdit = new QLineEdit(QStringLiteral("192.168.10.100"), mqttGroup);  // MQTT 服务器 IP
    mqttPortSpinBox = new QSpinBox(mqttGroup);                                  // MQTT 端口
    mqttPortSpinBox->setRange(1, 65535);                                        // 端口范围
    mqttPortSpinBox->setValue(1883);                                            // 默认 MQTT 端口 1883
    mqttEnabledCheckBox = new QCheckBox(QStringLiteral("连接MQTT"), mqttGroup); // 连接开关
    mqttStatusLabel = new QLabel(QStringLiteral("MQTT已断开"), mqttGroup);      // MQTT 状态
    mqttStatusLabel->setWordWrap(true);                                         // 允许换行
    mqttLayout->addRow(QStringLiteral("服务器"), mqttHostEdit);                 // 服务器行
    mqttLayout->addRow(QStringLiteral("端口"), mqttPortSpinBox);                // 端口行
    mqttLayout->addRow(mqttEnabledCheckBox);                                    // 开关行
    mqttLayout->addRow(mqttStatusLabel);                                        // 状态行

    // ===== GroupBox 6: 最近事件列表 =====
    QGroupBox *eventGroup = new QGroupBox(QStringLiteral("最近事件"), controlPanel);
    QVBoxLayout *eventLayout = new QVBoxLayout(eventGroup); // 垂直布局
    eventList = new QListWidget(eventGroup);                // 事件列表控件
    eventList->setMinimumHeight(150);                       // 最小高度 150px
    eventLayout->addWidget(eventList);                      // 添加到布局

    // 将当前启用的 5 个 GroupBox 按顺序添加到控制面板垂直布局
    controlLayout->addWidget(streamGroup);      // ① TCP 推流
    controlLayout->addWidget(monitoringGroup);  // ② 监控控制
    controlLayout->addWidget(performanceGroup); // ③ 性能面板
    controlLayout->addWidget(mqttGroup);        // ⑤ MQTT
    controlLayout->addWidget(eventGroup);       // ⑥ 事件列表
    controlLayout->addStretch();                // 底部弹性空间（把控件推到顶部）

    // ===== 滚动区域包裹控制面板（内容过多时可滚动） =====
    QScrollArea *controlScrollArea = new QScrollArea(this);                    // 滚动区域
    controlScrollArea->setObjectName(QStringLiteral("edgeControlScrollArea")); // 对象名
    controlScrollArea->setWidgetResizable(true);                               // 子控件可随滚动区域调整大小
    controlScrollArea->setFrameShape(QFrame::NoFrame);                         // 无边框
    controlScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);   // 关闭水平滚动条
    controlScrollArea->setWidget(controlPanel);                                // 将控制面板放入滚动区域
    controlScrollArea->setMinimumWidth(270);                                   // 最小宽度 270px
    controlScrollArea->setMaximumWidth(320);                                   // 最大宽度 320px（防止过宽）

    // ===== 左侧预览面板（标题 + 画面） =====
    QWidget *previewPanel = new QWidget(this);                                       // 预览面板容器
    previewPanel->setObjectName(QStringLiteral("previewPanel"));                     // 对象名（CSS 选择器）
    QLabel *previewTitle = new QLabel(QStringLiteral("实时监控画面"), previewPanel); // 标题
    previewTitle->setObjectName(QStringLiteral("previewTitle"));                     // 标题对象名
    QLabel *previewHint = new QLabel(                                                // 提示文字
        QStringLiteral("实时预览、画面变化检测与事件录像"),
        previewPanel);
    previewHint->setObjectName(QStringLiteral("previewHint"));  // 提示对象名
    QHBoxLayout *previewHeader = new QHBoxLayout();             // 标题栏水平布局
    previewHeader->addWidget(previewTitle);                     // 标题（左）
    previewHeader->addStretch();                                // 弹性空间（把提示推到右边）
    previewHeader->addWidget(previewHint);                      // 提示（右）
    QVBoxLayout *previewLayout = new QVBoxLayout(previewPanel); // 预览面板垂直布局
    previewLayout->setContentsMargins(10, 9, 10, 10);           // 内边距
    previewLayout->setSpacing(8);                               // 组件间距
    previewLayout->addLayout(previewHeader);                    // 添加标题栏
    previewLayout->addWidget(cameraPreviewLabel, 1);            // 添加预览 QLabel（stretch=1，占主要空间）

    // ===== 顶层水平布局：左侧预览 + 右侧控制面板 =====
    QHBoxLayout *pageLayout = new QHBoxLayout(this); // 页面顶层水平布局
    pageLayout->setContentsMargins(12, 12, 12, 12);  // 外边距 12px
    pageLayout->setSpacing(12);                      // 左右间距 12px
    pageLayout->addWidget(previewPanel, 1);          // 左侧：预览（stretch=1，占主要空间）
    pageLayout->addWidget(controlScrollArea);        // 右侧：控制面板（固定宽度）
} // initializeUi() 结束

/*
 * ===== initializeServices() — 初始化后台服务（第 2 周核心） =====
 *
 * 创建线程模型（共 4 个子线程，不含主线程）：
 *   1. cameraCaptureThread (QThread 子类，自带事件循环)
 *   2. networkWorkerThread  + frameSender (moveToThread)
 *   3. visionWorkerThread   + visionDetector (moveToThread)
 *   4. archiveWorkerThread  + archiveServer（8890 媒体归档服务）
 *
 * 信号跨线程连接（第 2 周核心 = 第二组信号）：
 *   CaptureThread::networkFrameReady  → TcpFrameSender::sendFrame   (QueuedConnection)
 *   CaptureThread::analysisFrameReady → VisionEventDetector::analyzeFrame (QueuedConnection)
 *   CaptureThread::localPreviewReady  → updateLocalPreview()        (QueuedConnection)
 *
 * frameSender::frameHandled    → CaptureThread::networkFrameHandled  (DirectConnection)
 * visionDetector::frameHandled → CaptureThread::analysisFrameHandled (DirectConnection)
 */
void CameraStreamingPage::initializeServices()
{
    // ===== 创建四个线程及其关联组件 =====
    cameraCaptureThread = new CaptureThread(this);    // 线程 1：V4L2 采集线程（QThread 子类）
    networkWorkerThread = new QThread(this);          // 线程 2：网络推流工作线程
    frameSender = new TcpFrameSender();               // H.264 编码 + TCP/UDP 帧发送器
    frameSender->moveToThread(networkWorkerThread);   // 将 frameSender 移到线程 2（其槽函数在线程 2 执行）
    visionWorkerThread = new QThread(this);           // 线程 3：视觉检测工作线程
    visionDetector = new VisionEventDetector();       // OpenCV 画面变化检测器
    visionDetector->moveToThread(visionWorkerThread); // 将 visionDetector 移到线程 3
    archiveWorkerThread = new QThread(this);          // 线程 4：媒体归档下载服务
    archiveServer = new MediaArchiveServer();         // HTTP 媒体文件列表与下载服务
    archiveServer->moveToThread(archiveWorkerThread); // 移到线程 4

    // ===== 初始化 SQLite 事件存储 =====
    eventStore = new EventStore();              // 创建事件数据库对象
    eventStoreReady = eventStore->initialize(); // 打开/创建 SQLite 数据库文件
    if (eventStoreReady)                        // 数据库就绪
    {
        // 启动时从数据库加载最近 30 条历史事件到 UI 列表
        const QVector<MonitoringEvent> recent = eventStore->recentEvents(30); // 查询最近 30 条
        for (int index = recent.size() - 1; index >= 0; --index)              // 从旧到新倒序遍历
            addEventToList(recent.at(index));                                 // 逐一添加到 UI 事件列表
    }
    else // 数据库不可用
    {
        qWarning() << "事件数据库不可用：" << eventStore->lastError(); // 打印错误信息
    }
    // 学习版不创建 LED/蜂鸣器告警执行器；原创建代码移到文件末尾备份区。

    /*
     * ===== 第一组：UI 按钮 → 后端/控制逻辑 =====
     * 这些连接运行在主线程，AutoConnection 自动选择 DirectConnection。
     * 信号来源是 UI 控件（主线程），接收方是 CaptureThread 或其他主线程槽函数。
     */
    // 采集按钮 → 采集线程的 setCaptureEnabled(bool) 槽
    connect(startCaptureButton, &QPushButton::clicked,               // 按钮点击信号
            cameraCaptureThread, &CaptureThread::setCaptureEnabled); // 启动/停止 V4L2 采集
    // 采集按钮 → 更新按钮文字（"开始采集" ↔ "停止采集"）
    connect(startCaptureButton, &QPushButton::clicked,
            this, &CameraStreamingPage::updateCaptureButtonText); // 更新按钮文字
    // 本地预览 CheckBox → 采集线程：开关本地预览帧分发
    connect(localPreviewCheckBox, &QCheckBox::clicked,                    // CheckBox 点击信号
            cameraCaptureThread, &CaptureThread::setLocalPreviewEnabled); // 控制是否发射 localPreviewReady
    connect(localPreviewCheckBox, &QCheckBox::clicked, this,
            [this](bool) { updateAnalysisFrameDistribution(); });
    // 网络推流 CheckBox → networkStreamingToggled 槽（校验 IP 后配置 TcpFrameSender）
    connect(networkStreamingCheckBox, &QCheckBox::clicked,
            this, &CameraStreamingPage::networkStreamingToggled); // 校验 + 配置 + 保存
    // 运动检测 CheckBox → visionDetectionToggled 槽
    connect(visionDetectionCheckBox, &QCheckBox::clicked,
            this, &CameraStreamingPage::visionDetectionToggled); // 配置 VisionEventDetector
    // 停用功能的 UI 信号连接集中保存在文件末尾，主调用链只保留启用功能。

    /*
     * ===== 第二组：采集线程 → 三个消费者（跨线程 QueuedConnection）=====
     * 这是第 2 周学习的核心：从 V4L2 采集线程分发帧到三个不同的消费者线程。
     * 不指定连接方式时，Qt 自动选择 QueuedConnection（因为发送者和接收者在不同线程）。
     */
    // 本地预览帧 → 主线程的 updateLocalPreview() 槽（跨线程，必须 Queued）
    // captureEnabled 里的采集线程 emit，主线程的槽接收并更新 QLabel
    connect(cameraCaptureThread, &CaptureThread::localPreviewReady, // 采集线程信号（非主线程）
            this, &CameraStreamingPage::updateLocalPreview);        // 主线程槽函数
    // 网络推流帧 → TcpFrameSender::sendFrame()（线程 2 的槽函数）
    connect(cameraCaptureThread, &CaptureThread::networkFrameReady, // 采集线程信号
            frameSender, &TcpFrameSender::sendFrame,                // 网络线程槽（H.264 编码 + TCP/UDP 发送）
            Qt::QueuedConnection);                                  // 显式指定跨线程队列连接
    // 视觉分析帧 → VisionEventDetector::analyzeFrame()（线程 3 的槽函数）
    connect(cameraCaptureThread, &CaptureThread::analysisFrameReady, // 采集线程信号
            visionDetector, &VisionEventDetector::analyzeFrame,      // 视觉线程槽（画面变化检测）
            Qt::QueuedConnection);                                   // 显式指定跨线程队列连接

    /*
     * ===== 第三组：网络推流线程（线程 2）的信号连接 =====
     */
    // 线程 2 启动时 → 立即初始化 TCP socket
    connect(networkWorkerThread, &QThread::started,    // QThread::start() 发射的信号
            frameSender, &TcpFrameSender::initialize); // 初始化 TCP 连接
    // 线程 2 结束时 → 自动 delete frameSender（Qt 对象树管理）
    connect(networkWorkerThread, &QThread::finished, // QThread::quit() + wait() 后发射
            frameSender, &QObject::deleteLater);     // 下次事件循环中 delete
    // 主线程 → TcpFrameSender：配置目标 IP/端口（跨线程）
    connect(this, &CameraStreamingPage::senderConfigurationChanged, // 主线程信号
            frameSender, &TcpFrameSender::configure,                // 网络线程槽
            Qt::QueuedConnection);                                  // 跨线程队列连接
    // 主线程 → TcpFrameSender：请求开始录像
    connect(this, &CameraStreamingPage::recordingRequested, // 主线程信号
            frameSender, &TcpFrameSender::startRecording,   // 网络线程槽
            Qt::QueuedConnection);
    // 主线程 → TcpFrameSender：请求停止录像
    connect(this, &CameraStreamingPage::recordingStopRequested, // 主线程信号
            frameSender, &TcpFrameSender::stopRecording,        // 网络线程槽
            Qt::QueuedConnection);
    // TcpFrameSender → CaptureThread：帧处理完成确认（Direct，两者都在非主线程）
    connect(frameSender, &TcpFrameSender::frameHandled,               // 网络线程信号
            cameraCaptureThread, &CaptureThread::networkFrameHandled, // 采集线程槽（打开网络门控）
            Qt::DirectConnection);                                    // 同属非主线程，Direct 更高效
    // TcpFrameSender → 主线程：TCP 连接状态文字更新
    connect(frameSender, &TcpFrameSender::statusChanged, // 网络线程信号
            connectionStatusLabel, &QLabel::setText,     // 主线程 UI 更新
            Qt::QueuedConnection);                       // 跨线程
    // TcpFrameSender → 主线程：录像状态变更通知
    connect(frameSender, &TcpFrameSender::recordingStateChanged,     // 网络线程信号
            this, &CameraStreamingPage::handleRecordingStateChanged, // 主线程槽
            Qt::QueuedConnection);
    // TcpFrameSender → 主线程：录像错误通知
    connect(frameSender, &TcpFrameSender::recordingError,     // 网络线程信号
            this, &CameraStreamingPage::handleRecordingError, // 主线程槽
            Qt::QueuedConnection);

    /*
     * ===== 第四组：视觉检测线程（线程 3）的信号连接 =====
     */
    // 线程 3 结束时 → 自动 delete visionDetector
    connect(visionWorkerThread, &QThread::finished, // 线程退出信号
            visionDetector, &QObject::deleteLater); // 延迟删除
    // 线程 4（archive）启动时 → 初始化 HTTP 归档服务（监听 8890）
    connect(archiveWorkerThread, &QThread::started,
            archiveServer, [this]()
            { archiveServer->initialize(8890); }); // Lambda 调用初始化
    // 线程 4 结束时 → 删除 archiveServer
    connect(archiveWorkerThread, &QThread::finished,
            archiveServer, &QObject::deleteLater);
    // archive 状态日志
    connect(archiveServer, &MediaArchiveServer::statusChanged, this, [](const QString &status)
            { qInfo() << status; }, // 打印日志
            Qt::QueuedConnection);
    // 主线程 → VisionEventDetector：运动检测参数配置（跨线程）
    connect(this, &CameraStreamingPage::detectorConfigurationChanged, // 主线程信号
            visionDetector, &VisionEventDetector::configure,          // 视觉线程槽
            Qt::QueuedConnection);
    // 人脸检测结果链在学习版中关闭，运动检测连接保持不变。
    // VisionEventDetector → CaptureThread：分析帧处理完成确认（Direct）
    connect(visionDetector, &VisionEventDetector::frameHandled,        // 视觉线程信号
            cameraCaptureThread, &CaptureThread::analysisFrameHandled, // 采集线程槽（打开分析门控）
            Qt::DirectConnection);                                     // 同属非主线程
    // VisionEventDetector → 主线程：每帧分析完成（变化比例更新）
    connect(visionDetector, &VisionEventDetector::analysisCompleted, // 视觉线程信号
            this, &CameraStreamingPage::handleAnalysisCompleted,     // 主线程槽
            Qt::QueuedConnection);
    // VisionEventDetector → 主线程：检测到画面运动事件（记录事件并可自动录像）
    connect(visionDetector, &VisionEventDetector::visualMotionDetected, // 视觉线程信号
            this, &CameraStreamingPage::handleVisualMotionDetected,     // 主线程槽
            Qt::QueuedConnection);
    // 停用的人脸结果连接集中保存在文件末尾。

    /*
     * ===== 第五组：输入框参数变更 → 即时应用 =====
     */
    // 服务器 IP 编辑完成 → 保存并更新推流配置
    connect(serverAddressEdit, &QLineEdit::editingFinished,   // 编辑完成信号（回车或失焦）
            this, &CameraStreamingPage::applyServerEndpoint); // 应用新地址
    // 端口变化 → 保存并更新推流配置
    connect(serverPortSpinBox,
            QOverload<int>::of(&QSpinBox::valueChanged), // valueChanged(int) 信号
            this, [this](int)                            // 忽略具体端口值
            { applyServerEndpoint(); });                 // 调用配置下发
    connect(streamTransportComboBox,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { applyServerEndpoint(); });
    // 运动阈值变化 → 立即更新 VisionEventDetector 配置
    connect(motionThresholdSpinBox,
            QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int threshold)                                                          // 捕获新阈值
            { emit detectorConfigurationChanged(                                                 // 发射跨线程信号
                  visionDetectionCheckBox->isChecked(), threshold); }); // 传递是否启用 + 阈值
    // 停用的人脸参数连接集中保存在文件末尾。

    // ===== 启动工作线程 =====
    networkWorkerThread->start(); // 启动线程 2（开始事件循环）
    visionWorkerThread->start();  // 启动线程 3
    archiveWorkerThread->start(); // 启动线程 4

    // ===== 启动性能监控定时器（每秒刷新一次性能面板） =====
    performanceTimer = new QTimer(this);                               // 创建定时器（this 为父对象）
    performanceTimer->setInterval(1000);                               // 间隔 1000ms = 1 秒
    connect(performanceTimer, &QTimer::timeout,                        // 定时器超时信号
            this, &CameraStreamingPage::updatePerformancePanel);       // 刷新性能面板
    previousPerformanceSampleMs = QDateTime::currentMSecsSinceEpoch(); // 记录初始采样时间戳
    readCpuTicks(&previousSystemCpuTicks, &previousProcessCpuTicks);   // 读取初始 CPU tick（后续做差）
    performanceTimer->start();                                         // 启动定时器

    // ===== 初始化 MQTT =====
    // 边缘监控页不初始化传感器规则；“板级设备”页仍按原逻辑访问板载设备。
    initializeMqtt();    // 初始化 MQTT 客户端
} // initializeServices() 结束

/*
 * ===== initializeSensors() — 初始化板载 I2C 传感器 =====
 *
 * 仅在 ARM Linux 平台有效，桌面环境（x86）使用 #ifdef 跳过。
 * AP3216C：光强 + 接近 + 红外传感器（I2C 地址 0x1E）
 * ICM20608：6 轴 IMU + 温度（I2C 地址 0x68 或 0x69）
 */
void CameraStreamingPage::initializeSensors()
{
#if defined(__linux__) && defined(__arm__)        // 仅在 ARM Linux 平台
    ambientLightSensor = new Ap3216cSensor(this); // 创建 AP3216C 光强传感器
    motionSensor = new Icm20608Sensor(this);      // 创建 ICM20608 IMU 传感器

    // AP3216C 传感器数据变化 → 更新规则引擎
    connect(ambientLightSensor, &Ap3216cSensor::sensorDataChanged, // 传感器数据更新信号
            this, &CameraStreamingPage::updateAp3216cRules);       // 检查光强/接近阈值
    // ICM20608 传感器数据变化 → 更新规则引擎
    connect(motionSensor, &Icm20608Sensor::sensorDataChanged, // IMU 数据更新信号
            this, &CameraStreamingPage::updateIcm20608Rules); // 检查加速度/温度阈值
#else
    // 桌面环境：没有板载 I2C 传感器，禁用相关 UI
    sensorMonitoringCheckBox->setText( // 修改文字提示
        QStringLiteral("传感器规则监控（仅ARM开发板）"));
    sensorStatusLabel->setText(QStringLiteral("桌面环境无板载传感器")); // 显示无传感器
#endif
}

/*
 * ===== initializeMqtt() — 初始化 MQTT 客户端 =====
 *
 * 主题结构：devices/{deviceId}/{status|events|commands}
 *   - status:   设备通过 publishHeartbeat() 定时上报 JSON 遥测数据
 *   - events:   告警事件推送（recordMonitoringEvent 中发布）
 *   - commands: PC 接收端通过此主题下发远程命令（handleMqttCommand 响应）
 *
 * 心跳机制：每 10 秒发布一次（包含捕获帧数、传感器数据、系统状态）
 * 重连机制：断开后 3 秒自动重连
 */
void CameraStreamingPage::initializeMqtt()
{
    mqttClient = new QMqttClient(this);      // 创建 MQTT 客户端
    heartbeatTimer = new QTimer(this);       // 心跳定时器
    mqttReconnectTimer = new QTimer(this);   // 重连定时器
    mqttReconnectTimer->setSingleShot(true); // 单次触发（不重复）
    mqttReconnectTimer->setInterval(3000);   // 断线后 3 秒自动重连
    heartbeatTimer->setInterval(10000);      // 每 10 秒发送一次心跳

    // MQTT 开关 CheckBox → 连接/断开
    connect(mqttEnabledCheckBox, &QCheckBox::clicked,
            this, &CameraStreamingPage::mqttMonitoringToggled);
    // 心跳定时器 → 发布设备遥测数据
    connect(heartbeatTimer, &QTimer::timeout,
            this, &CameraStreamingPage::publishHeartbeat);
    // 重连定时器 → 尝试重新连接 MQTT 服务器
    connect(mqttReconnectTimer, &QTimer::timeout, this, [this]()
            {
        if (mqttEnabledCheckBox->isChecked() &&
                mqttClient->state() == QMqttClient::Disconnected) {
            mqttStatusLabel->setText(QStringLiteral("MQTT正在重新连接..."));
            mqttClient->connectToHost();
        } });

    /*
     * MQTT 连接状态机处理。
     * 三种状态：
     *   Connected    — 订阅 commands 主题，启动心跳，锁定地址输入
     *   Connecting   — 显示连接中提示
     *   Disconnected — 如果用户勾选了连接，启动 3 秒重连定时器；否则显示已断开
     */
    connect(mqttClient, &QMqttClient::stateChanged,
            this, [this](QMqttClient::ClientState state)
            {
        switch (state) {
        case QMqttClient::Connected: {
            mqttReconnectTimer->stop();                 // 连接成功，停止重连
            mqttStatusLabel->setText(QStringLiteral("MQTT已连接：%1")
                                     .arg(deviceId));
            mqttHostEdit->setEnabled(false);            // 连接后锁定地址/端口
            mqttPortSpinBox->setEnabled(false);
            // 订阅远程命令主题（QoS 1：至少一次送达）
            auto subscription = mqttClient->subscribe(
                        mqttTopic(QStringLiteral("commands")), 1);
            if (!subscription)
                qWarning() << "MQTT命令主题订阅失败";
            heartbeatTimer->start();                    // 启动心跳定时器
            publishHeartbeat();                         // 连接后立即发送首条心跳
            break;
        }
        case QMqttClient::Connecting:
            mqttStatusLabel->setText(QStringLiteral("MQTT正在连接..."));
            break;
        case QMqttClient::Disconnected:
            if (mqttEnabledCheckBox->isChecked()) {
                // 用户期望连接但断开 → 自动重连
                mqttStatusLabel->setText(
                            QStringLiteral("MQTT已断开，3秒后自动重连"));
                mqttReconnectTimer->start();
            } else {
                // 用户主动断开 → 不重连
                mqttStatusLabel->setText(QStringLiteral("MQTT已断开"));
            }
            mqttHostEdit->setEnabled(true);             // 断开后恢复地址可编辑
            mqttPortSpinBox->setEnabled(true);
            heartbeatTimer->stop();                     // 停止心跳
            break;
        } });
    // MQTT 消息接收 → 如果是 commands 主题的消息，调用命令处理函数
    connect(mqttClient, &QMqttClient::messageReceived,
            this, [this](const QByteArray &message, const QMqttTopicName &topic)
            {
        if (topic.name() == mqttTopic(QStringLiteral("commands")))
            handleMqttCommand(message); });
    // MQTT 错误处理 → 显示错误码，等待自动重连
    connect(mqttClient, &QMqttClient::errorChanged,
            this, [this](QMqttClient::ClientError error)
            {
        if (error == QMqttClient::NoError)
            return;
        mqttStatusLabel->setText(QStringLiteral("MQTT连接错误(%1)，等待重连")
                                 .arg(int(error)));
        qWarning() << "MQTT连接错误，错误码：" << int(error)
                   << "服务器：" << mqttClient->hostname()
                   << mqttClient->port(); });
}

/*
 * ===== loadSettings() — 从 QSettings 恢复用户上次保存的配置 =====
 *
 * QSettings 在 Linux 上默认存储于 ~/.config/<AppName>/<AppName>.conf。
 * 所有配置项以 "edge/" 为前缀，与项目中其他页面的配置隔离。
 * 如果某配置项不存在（首次运行），使用默认值。
 */
/*
 * ===== loadSettings() — 从 QSettings 配置文件中恢复上次保存的 UI 参数 =====
 *
 * QSettings 使用应用的组织名和应用程序名构造（通常在 main.cpp 中通过
 * QCoreApplication::setOrganizationName()/setApplicationName() 设置）。
 * 在 Linux ARM 上，配置文件存储于 ~/.config/IMX6ULL/EdgeMonitorTerminal.conf。
 *
 * 如果某个键不存在（首次运行或配置文件被删除），使用 Qt 构造函数中的默认值。
 * loadSettings() 在构造函数中调用，此时 initializeUi() 已创建所有控件。
 */
void CameraStreamingPage::loadSettings() // 读取 QSettings 并设置到所有输入控件。
{
    QSettings settings; // 创建配置访问对象（组织名/应用名由 main.cpp 设定）。

    // ----- TCP 推流目标地址和端口 -----
    // settings.value("edge/streamHost", "192.168.10.100")：
    //   从配置文件读取键 "edge/streamHost" 的值；
    //   如果不存在，返回默认值 "192.168.10.100"（PC 端常用 IP）。
    serverAddressEdit->setText(                           // 将读取的 IP 地址字符串设置到输入框。
        settings.value(QStringLiteral("edge/streamHost"), // 键名，使用 QStringLiteral 避免堆分配。
                       QStringLiteral("192.168.10.100"))  // 默认值：局域网 PC IP。
            .toString());                                 // QVariant → QString 转换。
    // settings.value("edge/streamPort", 8888)：
    //   读取 TCP 推流端口；默认 8888（项目自定义推流端口，非 HTTP 的 80）。
    serverPortSpinBox->setValue(                          // 将读取的端口号设置到 SpinBox。
        settings.value(QStringLiteral("edge/streamPort"), // 键名。
                       8888)                              // 默认端口 8888（项目 TCP 帧协议专用）。
            .toInt());                                    // QVariant → int 转换。
    const QString transport = settings.value(QStringLiteral("edge/streamTransport"),
                                             QStringLiteral("udp")).toString().toLower();
    const int transportIndex = streamTransportComboBox->findData(transport);
    streamTransportComboBox->setCurrentIndex(transportIndex >= 0 ? transportIndex : 1);

    // ----- MQTT 服务器地址和端口 -----
    // settings.value("edge/mqttHost", "192.168.10.100")：
    //   读取 MQTT Broker 地址；默认指向与推流相同的 PC。
    mqttHostEdit->setText(                               // 设置 MQTT Host 输入框。
        settings.value(QStringLiteral("edge/mqttHost"),  // 键名。
                       QStringLiteral("192.168.10.100")) // 默认：局域网 PC IP。
            .toString());                                // QVariant → QString。
    // settings.value("edge/mqttPort", 1883)：
    //   读取 MQTT 端口；默认 1883（MQTT 标准端口，非 TLS 的 8883）。
    mqttPortSpinBox->setValue(                          // 设置 MQTT Port SpinBox。
        settings.value(QStringLiteral("edge/mqttPort"), // 键名。
                       1883)                            // 默认 MQTT 标准端口 1883。
            .toInt());                                  // QVariant → int。

    // ----- 视觉检测参数 -----
    // settings.value("edge/motionThresholdPercent", 12)：
    //   读取运动检测的变化像素比例阈值；默认 12%（变化像素 ≥ 12% 视为运动）。
    motionThresholdSpinBox->setValue(                                 // 设置运动阈值 SpinBox。
        settings.value(QStringLiteral("edge/motionThresholdPercent"), // 键名。
                       12)                                            // 默认阈值 12%。
            .toInt());                                                // QVariant → int。
}

/*
 * ===== saveSettings() — 将当前 UI 配置持久化到 QSettings =====
 * 在析构函数中调用，确保用户修改的参数在下次启动时能恢复。
 * 也在关键参数修改时调用（如 IP 地址编辑完成）。
 */
void CameraStreamingPage::saveSettings() const // 由析构函数和参数变更时调用，只读操作。
{
    QSettings settings; // 创建配置写入对象；析构时自动 sync() 刷盘。

    // 保存 TCP 推流目标 IP（去掉首尾空格避免无效空白）。
    settings.setValue(QStringLiteral("edge/streamHost"),    // 键名：推流目标 IP。
                      serverAddressEdit->text().trimmed()); // 值为输入框当前文本（trimmed 去首尾空白）。

    // 保存 TCP 推流目标端口。
    settings.setValue(QStringLiteral("edge/streamPort"), // 键名：推流端口。
                      serverPortSpinBox->value());       // 值为 SpinBox 当前整数值。
    settings.setValue(QStringLiteral("edge/streamTransport"),
                      streamTransportComboBox->currentData().toString());

    // 保存 MQTT Broker 地址。
    settings.setValue(QStringLiteral("edge/mqttHost"), // 键名：MQTT 服务器 IP。
                      mqttHostEdit->text().trimmed()); // 值为输入框去掉空白的文本。

    // 保存 MQTT Broker 端口。
    settings.setValue(QStringLiteral("edge/mqttPort"), // 键名：MQTT 端口。
                      mqttPortSpinBox->value());       // 值为 SpinBox 当前整数值。

    // 保存运动检测像素变化阈值（百分比）。
    settings.setValue(QStringLiteral("edge/motionThresholdPercent"), // 键名：运动阈值。
                      motionThresholdSpinBox->value());              // 值为当前百分比整数值。

}

/*
 * ===== updateLocalPreview() — 主线程处理来自采集线程的预览帧 =====
 *
 * 流程：
 *   1. 空帧检查 → 跳过并通知采集线程释放门控
 *   2. 缓存 latestFrame（用于手动抓拍/传感器事件附图）
 *   3. 缩放显示到 QLabel
 *   4. 通知采集线程帧已被消费（解除单帧门控，允许下一帧到来）
 */
void CameraStreamingPage::updateLocalPreview(const QImage &image) // image是采集线程经队列连接交给主线程的预览帧。
{
    if (image.isNull()) // 判断跨线程传来的QImage是否包含有效像素数据。
    {
        qWarning() << "收到空的本地预览图像，已跳过显示"; // 保留空帧诊断提示，方便排查内存或采集错误。
        cameraCaptureThread->localPreviewFrameHandled();  // 即使空帧也释放门控，避免采集流水线永久停顿。
        return;                                           // 空帧不能继续转换QPixmap，因此提前结束槽函数。
    }
    // 缓存最新帧（用于手动抓拍等功能）
    latestFrame = image;        // 通过QImage隐式共享缓存最新帧，供抓拍和事件附图使用。
    displayPreviewImage(image); // 调用统一显示函数，把图像缩放到预览QLabel。
    // 通知采集线程：主线程已处理完这一帧，可以发下一帧了
    cameraCaptureThread->localPreviewFrameHandled(); // 通知采集线程可以提交下一张本地预览帧。
}

/*
 * ===== displayPreviewImage() — 将 QImage 缩放后显示到 QLabel =====
 *
 * 使用 Qt::FastTransformation 而非 SmoothTransformation，
 * 因为在 ARM 嵌入式设备上缩放操作对性能敏感，优先保证帧率。
 */
void CameraStreamingPage::displayPreviewImage(const QImage &image) // image是需要绘制到cameraPreviewLabel的原始画面。
{
    if (image.isNull() || cameraPreviewLabel->size().isEmpty()) // 同时验证图像和显示区域是否有效。
        return;                                                 // 无图像或控件尺寸为0时无法进行有效缩放。
    const QPixmap sourcePixmap = QPixmap::fromImage(image);     // 将CPU侧QImage转换成适合QLabel绘制的QPixmap。
    if (sourcePixmap.isNull())                                  // 检查转换是否因内存不足或图像格式异常而失败。
    {
        qWarning() << "摄像头预览 QPixmap 创建失败，可用内存可能不足"; // 输出明确诊断信息但不删除原提示。
        return;                                                        // 转换失败时不能调用scaled，直接保留上一帧画面。
    }
    // 保持宽高比缩放到 QLabel 尺寸，使用快速变换以减少 CPU 开销
    cameraPreviewLabel->setPixmap(sourcePixmap.scaled(   // 把缩放结果设置为预览标签当前画面。
        cameraPreviewLabel->size(), Qt::KeepAspectRatio, // 目标为标签尺寸，并保持摄像头原始宽高比。
        Qt::FastTransformation));                        // 使用快速插值降低i.MX6ULL每帧缩放CPU开销。
}

/*
 * ===== updatePerformancePanel() — 每秒刷新一次性能监控面板 =====
 *
 * 通过对比相邻两次采样的计数器差值，计算实时性能指标：
 *   - 采集 FPS、预览 FPS、分析 FPS、发送 FPS
 *   - TCP/UDP 发送带宽（Mbps）、H.264 编码耗时（ms）
 *   - AI 推理 FPS 和推理耗时（ms）
 *   - 各阶段累计丢帧数
 *   - CPU 使用率（通过 /proc 文件系统差值计算）
 *   - 进程驻留内存（RSS，MB）
 *
 * 注意：所有统计值来自原子计数器（CaptureThread/TcpFrameSender/VisionEventDetector），
 * 本函数运行在主线程，读取原子变量不会阻塞任何工作线程。
 */
void CameraStreamingPage::updatePerformancePanel()
{
    if (!cameraCaptureThread || !frameSender || !visionDetector ||
        !performanceStatusLabel)
        return;

    // ===== 计算距离上次采样的时间间隔（秒） =====
    const qint64 now = QDateTime::currentMSecsSinceEpoch();            // 获取当前毫秒时间戳
    const qint64 elapsedMs = now - previousPerformanceSampleMs;        // 两次采样之间的毫秒差值
    const double elapsedSeconds = qMax<qint64>(1, elapsedMs) / 1000.0; // 转为秒，至少 1ms 防止除以零

    // ===== 获取各子系统的原子统计快照 =====
    const CapturePipelineStatistics capture =                             // V4L2 采集线程的统计快照
        cameraCaptureThread->statistics();                                // 读原子的帧数和丢帧数
    const FrameOutputStatistics output = frameSender->statistics();       // TCP 推流线程的统计快照

    // ===== 计算各阶段实时 FPS（差值 / 时间间隔） =====
    const double captureFps =                                                     // 采集 FPS：从摄像头 DQBUF 出来的原始帧率
        double(capture.capturedFrames - previousCapturedFrames) / elapsedSeconds; // (本次累加 - 上次累加) / 秒数
    const double previewFps =                                                     // 预览 FPS：实际发送到主线程 UI 的帧率
        double(capture.previewFrames - previousPreviewFrames) / elapsedSeconds;   // 受 FPS 限速(30fps) + 门控丢帧影响
    const double analysisFps =                                                    // 分析 FPS：实际发送到 AI 检测的帧率
        double(capture.analysisFrames - previousAnalysisFrames) / elapsedSeconds; // 受 FPS 限速(30fps) + 门控丢帧影响
    const double sentFps =                                                        // 网络发送 FPS：实际 TCP 发送成功的帧率
        double(output.sentFrames - previousSentFrames) / elapsedSeconds;          // 受 FPS 限速(30fps) + 网络带宽影响

    // ===== 计算发送带宽（Mbps） =====
    // 字节差 × 8(bit/byte) / 秒 / 1000000 = Mbps
    const double sentMegabitsPerSecond = // TCP 推流带宽消耗（兆比特/秒）
        double(output.sentBytes - previousSentBytes) * 8.0 / elapsedSeconds / 1000000.0;

    // ===== 计算单帧 H.264 编码平均耗时（ms） =====
    const quint64 encodedFrameDelta =                                                         // 本周期内编码的帧数
        output.encodedFrames - previousEncodedFrames;                                         // 本次累加 - 上次累加
    const quint64 encodeTimeDelta =                                                           // 本周期内编码总耗时（微秒）
        output.totalEncodeMicroseconds - previousEncodeMicroseconds;                          // 本次累加 - 上次累加
    const double averageEncodeMs = encodedFrameDelta > 0                                      // 三元运算符：有编码帧才计算
                                       ? double(encodeTimeDelta) / encodedFrameDelta / 1000.0 // 平均微秒 → 毫秒
                                       : 0.0;                                                 // 无编码帧 → 平均耗时 0

    // ===== 计算 CPU 使用率（通过 /proc 文件系统的 tick 差值计算） =====
    double cpuPercent = 0.0;                                       // CPU 使用率初始为 0
    quint64 currentSystemTicks = 0;                                // 输出参数：当前系统总 CPU tick
    quint64 currentProcessTicks = 0;                               // 输出参数：当前进程 CPU tick
    if (readCpuTicks(&currentSystemTicks, &currentProcessTicks) && // 读取 /proc/stat + /proc/self/stat
        currentSystemTicks > previousSystemCpuTicks &&             // 系统 tick 必须递增（防止回绕）
        currentProcessTicks >= previousProcessCpuTicks)            // 进程 tick 必须非减
    {
        const quint64 systemDelta =                                      // 系统 CPU 时间片增量
            currentSystemTicks - previousSystemCpuTicks;                 // 本次系统 tick - 上次系统 tick
        const quint64 processDelta =                                     // 进程 CPU 时间片增量
            currentProcessTicks - previousProcessCpuTicks;               // 本次进程 tick - 上次进程 tick
        const int processorCount = qMax(1, QThread::idealThreadCount()); // 获取 CPU 核数，至少为 1
        cpuPercent = double(processDelta) * processorCount * 100.0 /     // CPU% = 进程tick × 核数 × 100 / 系统tick
                     double(systemDelta);                                // 乘以核数是因为 /proc/stat 统计的是所有核的总和
        previousSystemCpuTicks = currentSystemTicks;                     // 保存当前系统 tick 供下次做差
        previousProcessCpuTicks = currentProcessTicks;                   // 保存当前进程 tick 供下次做差
    }

    // ===== 格式化性能面板显示文本（使用等宽字体确保列对齐） =====
    // 人脸 AI 指标已随功能停用；只显示当前有效的数据链指标。
    performanceStatusLabel->setText(QStringLiteral(
                                        "采集/预览/分析：%1 / %2 / %3 FPS\n"
                                        "发送：%4 FPS  %5 Mbps  %6 %7 ms\n"
                                        "累计丢帧(预览/发送/分析)：%8 / %9 / %10\n"
                                        "CPU：%11%  内存：%12 MB  拷贝失败：%13")
                                        .arg(captureFps, 0, 'f', 1)
                                        .arg(previewFps, 0, 'f', 1)
                                        .arg(analysisFps, 0, 'f', 1)
                                        .arg(sentFps, 0, 'f', 1)
                                        .arg(sentMegabitsPerSecond, 0, 'f', 2)
                                        .arg(QStringLiteral("H.264"))
                                        .arg(averageEncodeMs, 0, 'f', 1)
                                        .arg(capture.previewDroppedFrames)
                                        .arg(capture.networkDroppedFrames + output.droppedFrames)
                                        .arg(capture.analysisDroppedFrames)
                                        .arg(cpuPercent, 0, 'f', 1)
                                        .arg(processResidentMemoryMb(), 0, 'f', 1)
                                        .arg(capture.frameCopyFailures));

    // ===== 保存当前采样值，供下一次计算差值使用 =====
    previousPerformanceSampleMs = now;                                 // 保存本次采样时间戳
    previousCapturedFrames = capture.capturedFrames;                   // 保存采集帧累加数
    previousPreviewFrames = capture.previewFrames;                     // 保存预览帧累加数
    previousAnalysisFrames = capture.analysisFrames;                   // 保存分析帧累加数
    previousSentFrames = output.sentFrames;                            // 保存网络发送帧累加数
    previousSentBytes = output.sentBytes;                              // 保存网络发送字节累加数
    previousEncodedFrames = output.encodedFrames;                      // 保存 H.264 编码帧累加数
    previousEncodeMicroseconds = output.totalEncodeMicroseconds;       // 保存编码耗时累加数（微秒）
}

/*
 * ===== resizeEvent() — 窗口大小变化时重新缩放预览画面 =====
 */
void CameraStreamingPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // 只有当本地预览开启且有缓存帧时，才重新缩放显示
    if (!latestFrame.isNull() && localPreviewCheckBox->isChecked())
        displayPreviewImage(latestFrame);
}

/*
 * ===== updateCaptureButtonText() — 采集启停时更新按钮文字和状态 =====
 */
void CameraStreamingPage::updateCaptureButtonText(bool capturing)
{
    // 根据采集状态切换按钮文字。
    startCaptureButton->setText(capturing                                    // 三元判断：capturing=true?
                                    ? QStringLiteral("停止采集摄像头数据")   // 正在采集 → 显示"停止"。
                                    : QStringLiteral("开始采集摄像头数据")); // 未采集 → 显示"开始"。
    updateAnalysisFrameDistribution();
    // 如果停止采集但录像还在进行中，强制停止录像（录像需要采集帧作为数据源）。
    if (!capturing && eventRecordingActive) // 正在停止采集且仍有活跃录像？
        emit recordingStopRequested();      // 跨线程通知 TcpFrameSender 停止录像写文件。
    publishHeartbeat();                     // 推流/分析/录像状态变更后立即发送心跳更新状态。
}

void CameraStreamingPage::updateAnalysisFrameDistribution()
{
    // 本地预览本身会更新 latestFrame。只有开启画面变化检测，或关闭本地预览后仍需为
    // MQTT 远程抓拍保留最新帧时，才让采集线程向视觉线程分发图像。
    const bool needsAnalysis = visionDetectionCheckBox->isChecked() ||
                               !localPreviewCheckBox->isChecked();
    cameraCaptureThread->setAnalysisEnabled(
        startCaptureButton->isChecked() && needsAnalysis);
}

/*
 * ===== networkStreamingToggled() — TCP 推流开关 =====
 *
 * 启用时需要校验：IP 地址不能为空。
 * 注意：eventRecordingActive 为 true 时也需要强制开启网络帧分发，
 * 因为录像依赖 TcpFrameSender 接受帧数据。
 */
void CameraStreamingPage::networkStreamingToggled(bool enabled)
{
    if (enabled && serverAddressEdit->text().trimmed().isEmpty()) // 启用推流但 IP 为空？
    {
        // IP 为空时拒绝开启，提示用户先填写地址。
        networkStreamingCheckBox->setChecked(false);                      // 强制复位 CheckBox 为未选中。
        connectionStatusLabel->setText(QStringLiteral("请先填写电脑IP")); // 显示提示信息。
        return;                                                           // 提前返回，不执行后续配置。
    }
    // 推流开启 或 事件录像活跃 → 让采集线程向网络线程分发帧（setNetworkStreamingEnabled）。
    // eventRecordingActive 为 true 时也需要分发帧，因为录像由 TcpFrameSender 写文件。
    cameraCaptureThread->setNetworkStreamingEnabled( // 控制采集线程是否发射 networkFrameReady 信号。
        enabled || eventRecordingActive);            // 推流或录像任一活跃即开启分发。
    // 跨线程配置 TcpFrameSender 的目标地址（主机 + 端口 + 是否启用）。
    emit senderConfigurationChanged(serverAddressEdit->text().trimmed(), // 目标 IP（去首尾空白）。
                                    quint16(serverPortSpinBox->value()), // 目标端口（转为 16 位无符号）。
                                    enabled,
                                    streamTransportComboBox->currentData().toString());
    publishHeartbeat();                                                  // 状态变更后发送心跳。
}

/*
 * ===== applyServerEndpoint() — 应用服务器地址/端口配置变更 =====
 */
void CameraStreamingPage::applyServerEndpoint()
{
    saveSettings(); // 先将当前 IP/端口配置持久化到 QSettings。
    // 仅在 TCP 推流已开启时，才将新地址立即应用到 TcpFrameSender。
    // 如果推流未开启，只需保存配置即可；下次开启推流时 networkStreamingToggled 会读取最新值。
    if (networkStreamingCheckBox->isChecked()) // 推流 CheckBox 已勾选？
    {
        emit senderConfigurationChanged(serverAddressEdit->text().trimmed(), // 新 IP（去空白）。
                                        quint16(serverPortSpinBox->value()), // 新端口。
                                        true,
                                        streamTransportComboBox->currentData().toString());
    }
}

/*
 * ===== visionDetectionToggled() — 运动检测开关 =====
 *
 * 启用时配置 VisionEventDetector 的像素变化阈值。
 * 同时确保采集线程开启分析帧分发（最高 30 FPS 给视觉线程）。
 */
void CameraStreamingPage::visionDetectionToggled(bool enabled)
{
    updateAnalysisFrameDistribution();
    // 跨线程配置 VisionEventDetector：是否启用 + 像素变化阈值。
    emit detectorConfigurationChanged(enabled,                          // enabled=true 开启运动检测。
                                      motionThresholdSpinBox->value()); // 当前阈值百分比值。
    // 更新运动检测状态标签。
    visionStatusLabel->setText(enabled                                              // 三元判断：启用？
                                   ? QStringLiteral("画面变化检测已开启，等待分析") // 已开启提示。
                                   : QStringLiteral("视觉检测已关闭"));             // 已关闭提示。
    publishHeartbeat();                                                             // 状态变更后发送心跳。
}

/*
 * ===== faceDetectionToggled() — 人脸检测开关 =====
 *
 * 启用时：
 *   1. 搜索并加载 DNN 模型文件（优先使用轻量 DNN，失败时自动回退 Haar）
 *   2. 启用人脸框叠加（CaptureThread 在预览帧上画绿色检测框）
 *   3. 配置 VisionEventDetector 的推理间隔和置信度
 *
 * 模型文件搜索使用 locateProjectAsset()，在多个候选目录中查找。
 */
void CameraStreamingPage::faceDetectionToggled(bool enabled)
{
    // ----- 搜索 DNN 模型文件路径（使用 locateProjectAsset 在多个候选目录中查找） -----
    // Haar 级联分类器（DNN 不可用时的回退方案）。
    const QString classifierPath = locateProjectAsset(QStringLiteral( // 搜索 haarcascade 文件。
        "opencv_src/face-haar/haarcascade_frontalface_alt2.xml"));    // 正面人脸 Haar 级联模型。
    // DNN 模型定义文件（prototxt）：描述神经网络层结构和参数。
    const QString dnnConfigurationPath = locateProjectAsset(QStringLiteral( // 搜索 deploy.prototxt。
        "opencv_src/face-dnn/deploy.prototxt"));                            // SSD 网络结构定义。
    // DNN 模型权重文件（caffemodel）：训练好的权重数据。
    const QString dnnModelPath = locateProjectAsset(QStringLiteral(       // 搜索 caffemodel 文件。
        "opencv_src/face-dnn/res10_300x300_ssd_iter_140000.caffemodel")); // ResNet10-SSD 预训练权重。

    // 启用人脸框叠加（CaptureThread 在预览帧上画绿色检测框，不需要跨线程传图像数据）。
    cameraCaptureThread->setFaceOverlayEnabled(enabled); // enabled=true 开始画框。
    updateAnalysisFrameDistribution();

    // 跨线程传递模型路径和参数到 VisionEventDetector。
    emit faceDetectorConfigurationChanged(     // 主线程发射，视觉线程通过 QueuedConnection 接收。
        enabled,                               // 是否启用人脸检测。
        classifierPath,                        // Haar 级联文件路径（DNN 回退用）。
        dnnConfigurationPath,                  // DNN prototxt 文件路径。
        dnnModelPath,                          // DNN caffemodel 文件路径。
        faceInferenceIntervalSpinBox->value(), // AI 推理抽帧间隔（每隔 N 帧推理一次）。
        faceConfidenceSpinBox->value());       // DNN 检测置信度阈值。

    // 更新人脸检测状态标签。
    faceStatusLabel->setText(enabled                                                        // 启用？
                                 ? QStringLiteral("正在加载轻量DNN，失败时自动回退Haar...") // 加载中提示。
                                 : QStringLiteral("边缘AI已关闭"));                         // 已关闭提示。
    if (!enabled)                                                                           // 关闭人脸检测？
    {
        detectedFaceCount = 0;   // 清零人脸计数（避免心跳上报脏数据）。
        latestFaceBoxes.clear(); // 清空检测框缓存。
    }
    publishHeartbeat(); // 状态变更后发送心跳。
}

/*
 * ===== sensorMonitoringToggled() — 传感器监控开关 =====
 *
 * 仅在 ARM 平台有效（桌面环境 ambientLightSensor/motionSensor 为 nullptr）。
 * 启用后开始轮询 AP3216C 和 ICM20608 传感器数据。
 */
void CameraStreamingPage::sensorMonitoringToggled(bool enabled)
{
    if (!ambientLightSensor || !motionSensor) // 传感器对象为 nullptr（桌面环境）？
    {
        // 桌面环境：没有板载 I2C 传感器，阻止开启。
        const QSignalBlocker blocker(sensorMonitoringCheckBox);                 // 临时阻止信号发射（避免递归）。
        sensorMonitoringCheckBox->setChecked(false);                            // 强制复位为未选中。
        sensorStatusLabel->setText(QStringLiteral("当前环境无可用板载传感器")); // 提示用户。
        qWarning() << "传感器监控只能在 i.MX6ULL 开发板上启用";                 // 日志警告。
        return;                                                                 // 提前返回。
    }
    // ARM 平台：启停传感器轮询（传感器驱动内部使用定时器周期读取 I2C 数据）。
    ambientLightSensor->setPollingEnabled(enabled); // 启停 AP3216C 光强/接近/红外轮询。
    motionSensor->setPollingEnabled(enabled);       // 启停 ICM20608 六轴 IMU 轮询。
    updateAnalysisFrameDistribution();
    if (!enabled)                                                             // 关闭传感器监控？
        sensorStatusLabel->setText(QStringLiteral("传感器监控已关闭"));       // 更新状态提示。
    publishHeartbeat();                                                       // 状态变更后发送心跳。
}

/*
 * ===== handleAnalysisCompleted() — 视觉检测每帧分析完成回调 =====
 *
 * 每帧分析完成后更新 latestFrame 缓存和画面变化比例显示。
 * 注意：这里不触发告警事件（告警由 visualMotionDetected 信号触发），
 * 仅更新 UI 状态文字。
 */
void CameraStreamingPage::handleAnalysisCompleted(const QImage &image,
                                                  double changedRatio)
{
    // 如果本地预览关闭，用分析帧更新 latestFrame 缓存（供手动抓拍/传感器事件附图使用）。
    if (!image.isNull() && !localPreviewCheckBox->isChecked()) // 有图像且预览未开启？
        latestFrame = image;                                   // 通过 QImage 隐式共享缓存帧数据。
    // 仅当运动检测开启时更新 UI 状态文字。
    if (visionDetectionCheckBox->isChecked()) // 运动检测已开启？
    {
        visionStatusLabel->setText(QStringLiteral("画面变化：%1%  阈值：%2%")  // 格式化显示。
                                       .arg(changedRatio * 100.0, 0, 'f', 1)   // %1 = 变化比例（百分比，1位小数）。
                                       .arg(motionThresholdSpinBox->value())); // %2 = 当前阈值（整数百分比）。
    }
}

/*
 * ===== handleVisualMotionDetected() — 运动检测告警事件 =====
 *
 * 当连续多帧画面变化超过阈值时，VisionEventDetector 发出此信号。
 * 处理：记录事件到 SQLite + 添加到 UI 列表 + MQTT 推送 + LED/蜂鸣器。
 */
void CameraStreamingPage::handleVisualMotionDetected(const QImage &image,
                                                     double changedRatio)
{
    recordMonitoringEvent(                                 // 统一事件记录入口。
        QStringLiteral("visual_motion"),                   // 事件类型：画面运动。
        MonitoringEventSeverity::Warning,                  // 严重等级：警告（中）。
        QStringLiteral("camera"),                          // 事件来源：摄像头。
        QStringLiteral("检测到画面变化，变化像素比例 %1%") // 事件消息。
            .arg(changedRatio * 100.0, 0, 'f', 1),         // 插入变化百分比（1位小数）。
        image,                                             // 事件附图（当前帧）。
        true);                                             // true = 触发自动录像。
}

/*
 * ===== handleFaceAnalysisCompleted() — 人脸分析完成回调 =====
 *
 * 更新人脸数量和检测框缓存（用于手动抓拍时在图片上画框）。
 */
void CameraStreamingPage::handleFaceAnalysisCompleted(
    const QImage &image, const QVector<QRect> &faceBoxes)
{
    // 如果本地预览关闭，用分析帧更新 latestFrame 缓存。
    if (!image.isNull() && !localPreviewCheckBox->isChecked()) // 有图像且预览未开启？
        latestFrame = image;                                   // QImage 隐式共享，不深拷贝数据。
    latestFaceBoxes = faceBoxes;                               // 缓存最新检测框坐标队列。
    detectedFaceCount = faceBoxes.size();                      // 更新人脸计数（心跳中上报）。
    // 仅当人脸检测开启时更新 UI 状态文字。
    if (faceDetectionCheckBox->isChecked()) // 人脸检测已开启？
    {
        faceStatusLabel->setText(detectedFaceCount > 0                                        // 三元：检测到人脸？
                                     ? QStringLiteral("检测到 %1 张人脸，标注已同步到视频流") // 有人脸。
                                           .arg(detectedFaceCount)                            // 插入人脸数量。
                                     : QStringLiteral("人脸检测运行中，当前未发现人脸"));     // 无人脸。
    }
}

/*
 * ===== handleFacePresenceDetected() — 人脸检测告警事件 =====
 *
 * 人脸首次出现或数量变化时触发。在图片上绘制绿色检测框后保存。
 */
void CameraStreamingPage::handleFacePresenceDetected(const QImage &image,
                                                     int faceCount)
{
    // 创建带标注的副本（在图片上画绿色人脸框，不修改原始 QImage）。
    QImage annotatedImage = image.convertToFormat(QImage::Format_RGB32); // 转换为 RGB32 格式供 QPainter 绘制。
    if (!annotatedImage.isNull() && !latestFaceBoxes.isEmpty())          // 副本有效且有检测框？
    {
        QPainter painter(&annotatedImage);                            // 在副本上创建 QPainter。
        QPen pen(QColor(46, 220, 135));                               // 绿色画笔（RGB：46, 220, 135）。
        pen.setWidth(3);                                              // 线宽 3 像素（适合 320×240 分辨率）。
        painter.setPen(pen);                                          // 应用画笔到 painter。
        for (const QRect &box : latestFaceBoxes)                      // 遍历所有检测框。
            painter.drawRect(box.intersected(annotatedImage.rect())); // 绘制框（与图像矩形求交防越界）。
    } // QPainter 在此作用域结束时自动析构，释放绘制资源。
    // 记录人脸检测事件（带绿色框的标注图片作为附图）。
    recordMonitoringEvent(
        QStringLiteral("face_detected"),                       // 事件类型：人脸检测。
        MonitoringEventSeverity::Warning,                      // 严重等级：警告。
        QStringLiteral("camera"),                              // 来源：摄像头。
        QStringLiteral("检测到人脸，数量：%1").arg(faceCount), // 消息：人脸数量。
        annotatedImage.isNull() ? image : annotatedImage,      // 附图：标注图（失败则用原图）。
        true);                                                 // true = 触发自动录像。
}

/*
 * ===== handleFaceDetectionStatusChanged() — 人脸检测模型加载状态 =====
 *
 * 如果模型加载失败（如 DNN 和 Haar 都不可用），自动关闭人脸检测。
 */
void CameraStreamingPage::handleFaceDetectionStatusChanged(
    bool available, const QString &message)
{
    faceStatusLabel->setText(message);                    // 显示模型加载状态消息（如"轻量DNN已就绪"或"Haar回退中"）。
    if (!available && faceDetectionCheckBox->isChecked()) // 模型不可用但 CheckBox 仍勾选？
    {
        // 模型加载失败 → 强制关闭人脸检测，避免空转浪费 CPU（每次分析帧都尝试加载会无限循环）。
        const QSignalBlocker blocker(faceDetectionCheckBox);                      // 临时阻止 CheckBox 信号（避免递归调用）。
        faceDetectionCheckBox->setChecked(false);                                 // 复位 CheckBox 为未选中。
        cameraCaptureThread->setFaceOverlayEnabled(false);                        // 关闭预览帧上的人脸框叠加。
        updateAnalysisFrameDistribution();
    }
}

/*
 * ===== updateAp3216cRules() — AP3216C 传感器规则检查 =====
 *
 * 规则：接近传感器读数 >= ProximityAlertThreshold(100) → 触发告警。
 * 使用冷却机制避免同一事件短时间内重复触发。
 */
void CameraStreamingPage::updateAp3216cRules()
{
    // 缓存原始读数（用于传感器状态显示和 MQTT 心跳上报）。
    ambientLightValue = ambientLightSensor->ambientLight(); // 环境光亮度（lux 字符串）。
    proximityValue = ambientLightSensor->proximity();       // 接近传感器读数。
    infraredValue = ambientLightSensor->infrared();         // 红外传感器读数。
    updateSensorStatusLabel();                              // 更新传感器状态汇总显示。

    // ----- 接近传感器阈值检查 -----
    bool valid = false;                                  // 转换有效性标志。
    const int proximity = proximityValue.toInt(&valid);  // QString → int，通过指针回写有效性。
    if (valid && proximity >= ProximityAlertThreshold && // 值有效 且 超过阈值(100) 且
        ruleCooldownElapsed(QStringLiteral("proximity"), // 冷却时间已过？
                            SensorEventCooldownMs))      // 冷却时间 15 秒。
    {
        recordMonitoringEvent(                             // 记录接近告警事件。
            QStringLiteral("proximity_alert"),             // 事件类型：接近告警。
            MonitoringEventSeverity::Warning,              // 等级：警告。
            QStringLiteral("ap3216c"),                     // 来源：AP3216C 传感器。
            QStringLiteral("接近传感器超过阈值：%1 >= %2") // 消息：当前值 vs 阈值。
                .arg(proximity)                            // 插入当前值。
                .arg(ProximityAlertThreshold),             // 插入阈值。
            latestFrame,                                   // 附图：最新缓存帧。
            true);                                         // 触发自动录像。
    }
}

/*
 * ===== updateIcm20608Rules() — ICM20608 传感器规则检查 =====
 *
 * 规则1：|合加速度 - 1.0| >= VibrationAlertThreshold(0.55) → 震动告警
 * 规则2：芯片温度 >= TemperatureAlertThreshold(60.0) → 过热告警
 *
 * 合加速度 = sqrt(ax² + ay² + az²)，单位 g。
 * 静止时合加速度 ≈ 1.0g（仅重力加速度）。
 */
void CameraStreamingPage::updateIcm20608Rules()
{
    // ----- 有效性标志初始化 -----
    bool xValid = false;           // X 轴加速度是否有效。
    bool yValid = false;           // Y 轴加速度是否有效。
    bool zValid = false;           // Z 轴加速度是否有效。
    bool temperatureValid = false; // 温度读数是否有效。

    // 从 ICM20608 传感器读取三轴加速度和芯片温度。
    const double accelerationX = motionSensor->accelerationX().toDouble(&xValid);       // X 轴（g），回写有效性。
    const double accelerationY = motionSensor->accelerationY().toDouble(&yValid);       // Y 轴（g）。
    const double accelerationZ = motionSensor->accelerationZ().toDouble(&zValid);       // Z 轴（g）。
    const double temperature = motionSensor->temperature().toDouble(&temperatureValid); // 温度（℃）。

    // 缓存原始读数字符串（用于传感器状态显示和 MQTT 心跳上报）。
    accelerationSummary = QStringLiteral("%1/%2/%3 g")             // 格式：x.xx/y.yy/z.zz g。
                              .arg(motionSensor->accelerationX(),  // %1 = X 轴原始字符串。
                                   motionSensor->accelerationY(),  // %2 = Y 轴原始字符串。
                                   motionSensor->accelerationZ()); // %3 = Z 轴原始字符串。
    temperatureValue = motionSensor->temperature();                // 缓存温度原始字符串。
    updateSensorStatusLabel();                                     // 更新传感器状态汇总。

    // ----- 规则1：震动检测 -----
    // 合加速度 magnitude = sqrt(ax² + ay² + az²)。
    // 静止时 magnitude ≈ 1.0g（仅重力加速度），碰撞/跌落时显著偏离。
    if (xValid && yValid && zValid) // 三轴数据都有效？
    {
        const double magnitude = qSqrt(accelerationX * accelerationX + // 计算合加速度的平方根。
                                       accelerationY * accelerationY + // ay²。
                                       accelerationZ * accelerationZ); // az²。
        // |magnitude - 1.0| >= VibrationAlertThreshold(0.55) → 震动告警。
        if (qAbs(magnitude - 1.0) >= VibrationAlertThreshold && // 偏离 1.0g 超过 0.55g？
            ruleCooldownElapsed(QStringLiteral("vibration"),    // 震动冷却期已过？
                                SensorEventCooldownMs))         // 冷却 15 秒。
        {
            recordMonitoringEvent(                              // 记录震动告警事件。
                QStringLiteral("vibration_alert"),              // 事件类型：震动告警。
                MonitoringEventSeverity::Warning,               // 等级：警告。
                QStringLiteral("icm20608"),                     // 来源：ICM20608 IMU。
                QStringLiteral("检测到异常震动，合加速度 %1 g") // 消息：合加速度值。
                    .arg(magnitude, 0, 'f', 2),                 // 插入合加速度（2位小数）。
                latestFrame,                                    // 附图：最新帧。
                true);                                          // 触发自动录像。
        }
    }

    // ----- 规则2：温度检测 -----
    // 超过 TemperatureAlertThreshold(60.0) 触发严重告警。
    if (temperatureValid && temperature >= TemperatureAlertThreshold && // 温度有效 且 ≥ 60℃ 且
        ruleCooldownElapsed(QStringLiteral("temperature"),              // 温度冷却期已过？
                            TemperatureEventCooldownMs))                // 冷却 60 秒（温度变化慢）。
    {
        recordMonitoringEvent(                   // 记录温度告警事件。
            QStringLiteral("temperature_alert"), // 事件类型：温度告警。
            MonitoringEventSeverity::Critical,   // 等级：严重（Critical）！
            QStringLiteral("icm20608"),          // 来源：ICM20608 芯片温度。
            QStringLiteral("板载温度过高：%1 ℃") // 消息：当前温度值。
                .arg(temperature, 0, 'f', 1),    // 插入温度（1位小数）。
            latestFrame,                         // 附图：最新帧。
            true);                               // 触发自动录像。
    }
}

/*
 * ===== updateSensorStatusLabel() — 更新传感器状态汇总显示 =====
 *
 * 格式：ALS(环境光) / IR(红外) / PS(接近) / 加速度 / 温度。
 * 传感器未启用时显示 "--"。
 */
void CameraStreamingPage::updateSensorStatusLabel()
{
    sensorStatusLabel->setText(QStringLiteral(                      // 格式化传感器状态汇总文本。
                                   "ALS: %1  IR: %2  PS: %3\n"      // 第一行：光环境/红外/接近。
                                   "加速度: %4\n"                   // 第二行：三轴加速度。
                                   "温度: %5 ℃")                    // 第三行：芯片温度。
                                   .arg(ambientLightValue.isEmpty() // %1 = 环境光（无数据显示 "--"）。
                                            ? QStringLiteral("--")  // 传感器未启用。
                                            : ambientLightValue,    // 传感器已启用，显示读数值。
                                        infraredValue.isEmpty()     // %2 = 红外（无数据显示 "--"）。
                                            ? QStringLiteral("--")
                                            : infraredValue,
                                        proximityValue.isEmpty() // %3 = 接近（无数据显示 "--"）。
                                            ? QStringLiteral("--")
                                            : proximityValue,
                                        accelerationSummary.isEmpty() // %4 = 加速度汇总（无数据显示 "--"）。
                                            ? QStringLiteral("--")
                                            : accelerationSummary,
                                        temperatureValue.isEmpty() // %5 = 温度（无数据显示 "--"）。
                                            ? QStringLiteral("--")
                                            : temperatureValue));
}

/*
 * ===== ruleCooldownElapsed() — 传感器事件冷却检查 =====
 *
 * 使用 QHash 记录每个规则的最后触发时间。
 * 如果距离上次触发不足 cooldownMs，返回 false（跳过本次告警）。
 * 否则更新最后触发时间并返回 true。
 *
 * 冷却机制的作用：防止传感器噪声导致同一事件在短时间内频繁触发
 * （如震动传感器在持续震动时可能每秒触发多次）。
 */
bool CameraStreamingPage::ruleCooldownElapsed(const QString &ruleName,
                                              int cooldownMs)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();       // 获取当前毫秒时间戳。
    const qint64 lastTime = lastRuleEventTime.value(ruleName, 0); // 从 Hash 表获取该规则上次触发时间（首次为 0）。
    if (lastTime != 0 && now - lastTime < cooldownMs)             // 不是首次触发 且 距离上次不足冷却时间？
        return false;                                             // 返回 false（仍在冷却中，跳过本次触发）。
    lastRuleEventTime.insert(ruleName, now);                      // 更新该规则的最后触发时间为当前时间。
    return true;                                                  // 返回 true（允许本次触发）。
}

/*
 * ===== saveSnapshot() — 将 QImage 保存为 JPEG 文件 =====
 *
 * 保存路径：<exe目录>/data/snapshots/<时间戳>_<事件类型>.jpg
 * JPEG 质量 80，在文件大小和画质之间取得平衡。
 * 返回保存的文件路径，失败返回空字符串。
 */
QString CameraStreamingPage::saveSnapshot(const QImage &image,
                                          const QString &eventType) const
{
    if (image.isNull())   // 无有效图像数据？
        return QString(); // 返回空字符串表示保存失败。

    // 创建 data/snapshots 目录（如果不存在）。
    const QString directoryPath = QDir(QCoreApplication::applicationDirPath())     // 从 exe 所在目录出发。
                                      .filePath(QStringLiteral("data/snapshots")); // 拼接 data/snapshots 子目录。
    if (!QDir().mkpath(directoryPath))                                             // 递归创建目录（含所有父目录）？
    {
        qWarning() << "创建抓拍目录失败：" << directoryPath; // 目录创建失败日志。
        return QString();                                    // 返回空字符串。
    }

    // 文件名格式：20240923_143052_123_visual_motion.jpg
    //   yyyyMMdd_HHmmss_zzz = 年月日_时分秒_毫秒
    const QString fileName = QStringLiteral("%1_%2.jpg")                          // 文件名模板。
                                 .arg(QDateTime::currentDateTime().toString(      // %1 = 当前时间戳字符串。
                                          QStringLiteral("yyyyMMdd_HHmmss_zzz")), // 精确到毫秒的格式。
                                      eventType);                                 // %2 = 事件类型（如 visual_motion）。

    // 拼接完整保存路径：目录 + 文件名。
    const QString filePath = QDir(directoryPath).filePath(fileName); // 规范化路径拼接。

    // 保存为 JPEG 格式，质量 80（在文件大小和画质之间平衡）。
    if (!image.save(filePath, "JPEG", 80)) // QImage::save 返回 bool。
    {
        qWarning() << "保存事件抓拍失败：" << filePath; // 保存失败日志。
        return QString();                               // 返回空字符串。
    }
    qInfo() << "事件抓拍已保存：" << filePath; // 成功日志。
    return filePath;                           // 返回完整文件路径。
}

/*
 * ===== createRecordingPath() — 生成录像文件路径 =====
 *
 * MJPEG 格式录像保存在 data/recordings 目录下。
 * 注意：这里只生成路径，目录由 TcpFrameSender 在开始录像时创建。
 */
QString CameraStreamingPage::createRecordingPath(const QString &eventType) const
{
    // 构建 data/recordings 目录路径。
    const QString directoryPath = QDir(QCoreApplication::applicationDirPath())      // 从 exe 所在目录。
                                      .filePath(QStringLiteral("data/recordings")); // 拼接 data/recordings 子目录。
    // 返回完整文件路径：目录/时间戳_事件类型.mjpeg
    // 注意：这里只生成路径字符串，目录由 TcpFrameSender::startRecording 在写入前创建。
    return QDir(directoryPath).filePath(                     // 规范化路径拼接。
        QStringLiteral("%1_%2.mjpeg")                        // MJPEG 文件名模板。
            .arg(QDateTime::currentDateTime().toString(      // %1 = 当前时间戳。
                     QStringLiteral("yyyyMMdd_HHmmss_zzz")), // 精确到毫秒。
                 eventType));                                // %2 = 事件类型（或 "manual_recording"）。
}

/*
 * ===== takeManualSnapshot() — 保存当前画面（供远程 snapshot 命令复用） =====
 *
 * 使用最新缓存的 latestFrame 作为抓拍源。
 * 学习版直接保存当前帧；原人脸框叠加代码保留但不执行。
 * 抓拍事件记录为 Information 级别，不触发自动录像。
 */
void CameraStreamingPage::takeManualSnapshot()
{
    if (latestFrame.isNull()) // 最新缓存帧为空？
    {
        recordingStatusLabel->setText(                                   // 在录像状态标签中显示提示。
            QStringLiteral("尚无可抓拍画面，请开启本地预览或视觉检测")); // 提示用户先开启采集。
        qWarning() << "手动抓拍失败：尚无最新摄像头画面";                // 日志警告。
        return;                                                          // 提前返回。
    }

    // 创建 RGB32 格式副本作为抓拍图像。
    QImage snapshotImage = latestFrame.convertToFormat(QImage::Format_RGB32); // 深拷贝一份 RGB32 图像。
    // 记录抓拍事件（不触发自动录像，severity=Information，requestRecording=false）。
    recordMonitoringEvent(
        QStringLiteral("manual_snapshot"),                    // 事件类型：手动抓拍。
        MonitoringEventSeverity::Information,                 // 等级：信息（绿色，不触发告警）。
        QStringLiteral("operator"),                           // 来源：操作员。
        QStringLiteral("远程抓拍"),                           // 事件消息。
        snapshotImage.isNull() ? latestFrame : snapshotImage, // 附图：标注图（失败用原图）。
        false);                                               // false = 不触发自动录像。
}

/*
 * ===== manualRecordingToggled() — 手动录像启停 =====
 *
 * 手动录像时不限时长（durationMs=0），用户手动停止。
 * 启用流程：确保采集已启动 → 启用网络帧分发 → 通知 TcpFrameSender 开始录像。
 * 关闭流程：通知 TcpFrameSender 停止录像 → 保存 MJPEG 文件。
 */
void CameraStreamingPage::manualRecordingToggled(bool enabled)
{
    if (enabled) // 用户点击"开始录像"
    {
        // ----- 启动录像前校验：必须先开启摄像头采集 -----
        if (!startCaptureButton->isChecked()) // 采集未启动？
        {
            const QSignalBlocker blocker(manualRecordingButton); // 阻止按钮信号（避免递归）。
            manualRecordingButton->setChecked(false);            // 复位按钮为未按下。
            recordingStatusLabel->setText(                       // 提示用户先启动采集。
                QStringLiteral("请先启动摄像头采集，再开始手动录像"));
            qWarning() << "手动录像启动失败：摄像头尚未开始采集"; // 日志警告。
            return;                                               // 提前返回。
        }

        // ----- 启动手动录像 -----
        manualRecordingRequested = true;                                      // 标记用户主动发起了手动录像。
        eventRecordingActive = true;                                          // 标记当前有活跃录像（影响门控和状态）。
        currentRecordingPath = createRecordingPath(                           // 生成录像文件路径。
            QStringLiteral("manual_recording"));                              // 事件类型：手动录像。
        manualRecordingButton->setText(QStringLiteral("停止手动录像"));       // 按钮文字切换为"停止"。
        recordingStatusLabel->setText(QStringLiteral("正在启动手动录像...")); // 状态提示。

        // 强制开启网络帧分发（即使用户没勾选 TCP 推流）。
        // 原因：录像由 TcpFrameSender 负责写 MJPEG 文件，它需要接收帧数据。
        cameraCaptureThread->setNetworkStreamingEnabled(true); // 开启采集线程 → 网络线程的帧分发。

        // 跨线程通知 TcpFrameSender 开始录像。
        // durationMs=0 表示不启用自动停止定时器，由用户手动点击"停止"按钮结束。
        emit recordingRequested(currentRecordingPath, // 录像文件保存路径。
                                0);                   // 0 = 手动模式（不限时长）。
        qInfo() << "用户请求开始手动录像：" << currentRecordingPath;
        return; // 启动分支结束。
    }

    // ===== 停止手动录像 =====
    if (!manualRecordingRequested && !eventRecordingActive) // 没有在录像？
        return;                                             // 直接忽略（幂等性保护）。

    manualRecordingRequested = false;                                           // 清除手动录像请求标志。
    manualRecordingButton->setText(QStringLiteral("开始手动录像"));             // 按钮文字恢复为"开始"。
    recordingStatusLabel->setText(QStringLiteral("正在停止并保存手动录像...")); // 停止中提示。
    emit recordingStopRequested();                                              // 跨线程通知 TcpFrameSender 停止录像。
    qInfo() << "用户请求停止手动录像";
}

/*
 * ===== recordMonitoringEvent() — 统一的事件记录入口 =====
 *
 * 这是系统中最核心的事件处理函数，所有告警/通知都经过这里。
 * 执行以下操作（按顺序）：
 *   1. 保存抓拍图片到 data/snapshots/
 *   2. 如果是手动/远程抓拍，额外登记路径供 OpenCV 页面使用
 *   3. 如果 requestRecording=true 且自动录像已开启 → 启动 10 秒事件录像
 *   4. 写入 SQLite 数据库（eventStore->appendEvent）
 *   5. 添加到 UI 事件列表（addEventToList）
 *   6. 通过 MQTT 推送到 PC 接收端（publishMqttJson）
 *   7. 完成。LED/蜂鸣器联动在当前学习版中关闭
 */
void CameraStreamingPage::recordMonitoringEvent(
    const QString &eventType, MonitoringEventSeverity severity,
    const QString &source, const QString &message,
    const QImage &eventImage, bool requestRecording)
{
    // ----- 步骤1：构建事件结构体 -----
    MonitoringEvent event;                                    // 栈上创建事件对象。
    event.deviceId = deviceId;                                // 设备唯一标识（如 "imx6ull-001"）。
    event.eventType = eventType;                              // 事件类型字符串（如 "visual_motion"）。
    event.severity = severity;                                // 严重等级（Info/Warning/Critical）。
    event.source = source;                                    // 来源组件（如 "camera"/"ap3216c"/"icm20608"）。
    event.message = message;                                  // 人类可读事件消息。
    event.createdAt = QDateTime::currentDateTime();           // 记录事件发生时间戳。
    event.snapshotPath = saveSnapshot(eventImage, eventType); // 步骤2：保存抓拍图片并获得文件路径。

    // ----- 步骤3：手动/远程抓拍路径登记（供 OpenCV 页面读取最新抓拍） -----
    if (eventType == QLatin1String("manual_snapshot") && // 是手动/远程抓拍事件 且
        !event.snapshotPath.isEmpty())                   // 图片保存成功？
    {
        QSettings settings;                                          // 创建 QSettings 写入对象。
        settings.setValue(QStringLiteral("edge/latestSnapshotPath"), // 键名：最新抓拍路径。
                          event.snapshotPath);                       // 值：刚保存的 JPEG 路径。
        settings.sync();                                             // 立即刷盘（确保 OpenCV 页面能读到）。
        qInfo() << "最新手动/远程抓拍已登记给 OpenCV 页面："         // 日志记录。
                << event.snapshotPath;                               // 打印路径。
    }

    // ----- 步骤4：事件自动录像逻辑 -----
    // 条件：请求录像 + 自动录像 CheckBox 已勾选 + 采集正在运行。
    if (requestRecording && automaticRecordingCheckBox->isChecked() && // 需要录像 且 自动录像开启 且
        startCaptureButton->isChecked())                               // 采集已启动。
    {
        if (!eventRecordingActive) // 当前没有活跃录像？
        {
            currentRecordingPath = createRecordingPath(eventType); // 生成录像文件路径。
            eventRecordingActive = true;                           // 标记录像活跃。
            // 强制开启网络帧分发（录像由 TcpFrameSender 写文件）。
            cameraCaptureThread->setNetworkStreamingEnabled(true); // 采集 → 网络帧分发。
            // durationMs=EventRecordingDurationMs(10000)，10 秒后由 TcpFrameSender 定时器自动停止。
            emit recordingRequested(currentRecordingPath,      // 录像文件路径。
                                    EventRecordingDurationMs); // 自动停止时间：10000ms。
        }
        event.recordingPath = currentRecordingPath; // 记录当前录像路径到事件结构体。
    }

    // ----- 步骤5：写入 SQLite 数据库（持久化） -----
    if (eventStoreReady && !eventStore->appendEvent(&event))              // 数据库就绪 但 写入失败？
        qWarning() << "事件未能写入 SQLite：" << eventStore->lastError(); // 打印错误详情。

    // ----- 步骤6：添加到 UI 事件列表 -----
    addEventToList(event); // 在列表顶部插入新事件，超量自动删除旧条目。

    // ----- 步骤7：MQTT 推送（构建 JSON 并发布到 devices/{id}/events 主题） -----
    QJsonObject json;                                                  // 创建空 JSON 对象。
    json.insert(QStringLiteral("id"), double(event.id));               // 事件 ID（SQLite 自增主键）。
    json.insert(QStringLiteral("deviceId"), event.deviceId);           // 设备 ID。
    json.insert(QStringLiteral("eventType"), event.eventType);         // 事件类型字符串。
    json.insert(QStringLiteral("severity"), int(event.severity));      // 严重等级（int 枚举）。
    json.insert(QStringLiteral("source"), event.source);               // 来源组件。
    json.insert(QStringLiteral("message"), event.message);             // 事件描述消息。
    json.insert(QStringLiteral("snapshotPath"), event.snapshotPath);   // 抓拍图片路径。
    json.insert(QStringLiteral("recordingPath"), event.recordingPath); // 录像文件路径（可能为空）。
    json.insert(QStringLiteral("timestamp"),                           // 事件时间戳。
                event.createdAt.toString(Qt::ISODateWithMs));          // ISO 8601 格式带毫秒。
    publishMqttJson(mqttTopic(QStringLiteral("events")), json);        // 发布到 MQTT events 主题。

}

/*
 * ===== addEventToList() — 将事件添加到 UI 事件列表 =====
 *
 * 新事件插入列表顶部（最近的事件在最上面）。
 * 超过 MaximumVisibleEvents(100) 条时，删除最旧的条目。
 * 每行显示：时间戳 [严重等级] 事件消息
 * Tooltip 显示：来源 / 抓拍路径 / 录像路径
 */
void CameraStreamingPage::addEventToList(const MonitoringEvent &event)
{
    // 格式化时间戳（月-日 时:分:秒 格式）。
    const QString timestamp = event.createdAt.isValid()                                        // 时间戳有效？
                                  ? event.createdAt.toString(QStringLiteral("MM-dd HH:mm:ss")) // 格式化。
                                  : QStringLiteral("--");                                      // 无效 → 显示 "--"。

    // 创建列表项：时间戳 [严重等级] 事件消息。
    QListWidgetItem *item = new QListWidgetItem( // 堆分配列表项（Qt 对象树管理）。
        QStringLiteral("%1 [%2] %3")             // 格式模板。
            .arg(timestamp,                      // %1 = 时间戳。
                 severityText(event.severity),   // %2 = 严重等级中文文本。
                 event.message));                // %3 = 事件描述消息。

    item->setForeground(severityColor(event.severity)); // 根据严重等级设置文字颜色。
    // 设置 Tooltip（鼠标悬停显示详细信息）。
    item->setToolTip(QStringLiteral("来源: %1\n抓拍: %2\n录像: %3") // Tooltip 模板。
                         .arg(event.source,                         // %1 = 事件来源。
                              event.snapshotPath.isEmpty()          // %2 = 抓拍路径。
                                  ? QStringLiteral("无")            // 无抓拍 → "无"。
                                  : event.snapshotPath,             // 有抓拍 → 显示路径。
                              event.recordingPath.isEmpty()         // %3 = 录像路径。
                                  ? QStringLiteral("无")            // 无录像 → "无"。
                                  : event.recordingPath));          // 有录像 → 显示路径。

    // 插入到列表顶部（索引 0），最新事件在最上面。
    eventList->insertItem(0, item); // prepend 到 QListWidget。

    // 超过最大容量时，删除最旧的条目（列表底部 = 索引 count-1）。
    while (eventList->count() > MaximumVisibleEvents)       // 事件数 > 100？
        delete eventList->takeItem(eventList->count() - 1); // 删除底部条并释放内存。
}

/*
 * ===== handleRecordingStateChanged() — 录像状态变更回调 =====
 *
 * 由 TcpFrameSender 通过跨线程信号发回主线程。
 * active=true：录像已开始
 * active=false：录像已停止（文件已保存）
 */
void CameraStreamingPage::handleRecordingStateChanged(
    bool active, const QString &filePath)
{
    eventRecordingActive = active; // 更新活跃录像标志（影响门控和其他逻辑）。
    if (active)                    // 录像已开始。
    {
        currentRecordingPath = filePath; // 记录当前录像文件路径。
        if (manualRecordingRequested)    // 是用户手动启动的录像？
        {
            // 手动录像状态更新：按钮保持按下状态，不触发重复信号。
            const QSignalBlocker blocker(manualRecordingButton);                     // 阻止按钮信号。
            manualRecordingButton->setChecked(true);                                 // 强制按钮为按下状态。
            manualRecordingButton->setText(QStringLiteral("停止手动录像"));          // 按钮文字。
            recordingStatusLabel->setText(QStringLiteral("手动录像中：%1")           // 状态提示。
                                              .arg(QFileInfo(filePath).fileName())); // 只显示文件名。
        }
        else // 事件自动录像。
        {
            recordingStatusLabel->setText(QStringLiteral("事件录像中：%1")           // 状态提示。
                                              .arg(QFileInfo(filePath).fileName())); // 只显示文件名。
        }
    }
    else // 录像已停止（文件已保存）。
    {
        // 显示保存路径。
        recordingStatusLabel->setText(QStringLiteral("录像已保存：%1") // 完成提示。
                                          .arg(filePath));             // 完整文件路径。
        if (!filePath.isEmpty())                                       // 有有效路径？
        {
            QSettings settings;                                           // 创建 QSettings 写入对象。
            settings.setValue(QStringLiteral("edge/latestRecordingPath"), // 键名：最新录像路径。
                              filePath);                                  // 值：刚保存的 MJPEG 路径。
            settings.sync();                                              // 立即刷盘（供 OpenCV 页面读取）。
        }
        // 重置手动录像状态。
        manualRecordingRequested = false; // 清除手动录像标志。
        {
            const QSignalBlocker blocker(manualRecordingButton); // 阻止按钮信号。
            manualRecordingButton->setChecked(false);            // 复位按钮为未按下。
        }
        manualRecordingButton->setText(QStringLiteral("开始手动录像")); // 恢复按钮文字。
        currentRecordingPath.clear();                                   // 清空路径缓存。

        // 如果 TCP 推流也没勾选，则停止网络帧分发（录像结束后不再需要分发给 TcpFrameSender）。
        if (!networkStreamingCheckBox->isChecked())                 // 推流 CheckBox 未勾选？
            cameraCaptureThread->setNetworkStreamingEnabled(false); // 关闭采集 → 网络帧分发。
    }
    publishHeartbeat(); // 状态变更后发送心跳。
}

/*
 * ===== handleRecordingError() — 录像错误处理 =====
 */
void CameraStreamingPage::handleRecordingError(const QString &message)
{
    eventRecordingActive = false;     // 清除活跃录像标志。
    manualRecordingRequested = false; // 清除手动录像标志。
    {
        const QSignalBlocker blocker(manualRecordingButton); // 阻止按钮信号。
        manualRecordingButton->setChecked(false);            // 复位按钮为未按下。
    }
    manualRecordingButton->setText(QStringLiteral("开始手动录像")); // 恢复按钮文字。
    currentRecordingPath.clear();                                   // 清空路径缓存。
    recordingStatusLabel->setText(message);                         // 显示错误消息。
    // 如果 TCP 推流也没勾选，停止网络帧分发（与录像停止逻辑一致）。
    if (!networkStreamingCheckBox->isChecked())                 // 推流未勾选？
        cameraCaptureThread->setNetworkStreamingEnabled(false); // 关闭帧分发。
}

/*
 * ===== mqttMonitoringToggled() — MQTT 连接/断开开关 =====
 */
void CameraStreamingPage::mqttMonitoringToggled(bool enabled)
{
    if (!enabled)
    {
        // 断开连接：停止重连定时器和心跳，断开 MQTT
        mqttReconnectTimer->stop();
        mqttClient->disconnectFromHost();
        return;
    }
    // 连接前校验：host 不能为空
    const QString host = mqttHostEdit->text().trimmed();
    if (host.isEmpty())
    {
        const QSignalBlocker blocker(mqttEnabledCheckBox);
        mqttEnabledCheckBox->setChecked(false);
        mqttStatusLabel->setText(QStringLiteral("请先填写MQTT服务器"));
        return;
    }
    saveSettings();
    // 配置 MQTT 客户端参数
    mqttClient->setHostname(host);
    mqttClient->setPort(quint16(mqttPortSpinBox->value()));
    // ClientID 格式：imx6ull-001-edge-terminal（设备ID + 角色后缀）
    mqttClient->setClientId(deviceId + QStringLiteral("-edge-terminal"));
    mqttClient->connectToHost();
}

/*
 * ===== mqttTopic() — 构建 MQTT 主题路径 =====
 *
 * 格式：devices/{deviceId}/{suffix}
 * 例如：devices/imx6ull-001/status
 */
QString CameraStreamingPage::mqttTopic(const QString &suffix) const
{
    return QStringLiteral("devices/%1/%2").arg(deviceId, suffix);
}

/*
 * ===== publishMqttJson() — 发布 JSON 消息到指定 MQTT 主题 =====
 *
 * QoS 1（至少一次送达），不保留（retain=false）。
 * 仅在 MQTT 已连接时发布，断开时直接跳过。
 */
void CameraStreamingPage::publishMqttJson(const QString &topic,
                                          const QJsonObject &object)
{
    if (!mqttClient || mqttClient->state() != QMqttClient::Connected)
        return;
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (mqttClient->publish(QMqttTopicName(topic), payload, 1, false) < 0)
        qWarning() << "MQTT发布失败：" << topic << payload;
}

/*
 * ===== publishHeartbeat() — 发送设备心跳（遥测数据） =====
 *
 * 每 10 秒定时发送，也在状态变更时主动发送。
 * 学习版上报内容包括：采集状态、推流状态、画面变化检测状态、
 *   录像状态和时间戳。人脸、传感器状态字段暂时停用。
 *
 * PC 接收端通过订阅 devices/{id}/status 主题获取设备实时状态。
 */
void CameraStreamingPage::publishHeartbeat()
{
    QJsonObject status;
    status.insert(QStringLiteral("deviceId"), deviceId);
    status.insert(QStringLiteral("online"), true);
    status.insert(QStringLiteral("captureEnabled"), startCaptureButton->isChecked());
    status.insert(QStringLiteral("streamingEnabled"), networkStreamingCheckBox->isChecked());
    status.insert(QStringLiteral("streamTransport"),
                  streamTransportComboBox->currentData().toString());
    status.insert(QStringLiteral("visionEnabled"), visionDetectionCheckBox->isChecked());
    status.insert(QStringLiteral("recording"), eventRecordingActive);
    status.insert(QStringLiteral("timestamp"),
                  QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    publishMqttJson(mqttTopic(QStringLiteral("status")), status);
}

/*
 * ===== handleMqttCommand() — 处理来自 PC 接收端的远程命令 =====
 *
 * 命令格式（JSON）：
 *   {"command": "start_capture"}   — 简单命令
 *   {"command": "set_stream_endpoint", "host": "192.168.1.1", "port": 8888} — 带参数命令
 *
 * 还支持纯文本命令（向后兼容）。
 *
 * 支持的命令列表：
 *   start_capture/stop_capture   — 启停摄像头采集
 *   stream_on/stream_off         — 启停 TCP 推流
 *   vision_on/vision_off         — 启停运动检测
 *   face_on/face_off             — 已停用（原代码保留）
 *   sensors_on/sensors_off       — 已停用（原代码保留）
 *   snapshot                      — 远程抓拍
 *   alarm_on/alarm_off           — 已停用（原代码保留）
 *   set_stream_endpoint          — 修改 TCP 推流目标地址
 *
 * 每个命令执行后都会回复 acknowledgement 到 command_ack 主题。
 */
void CameraStreamingPage::handleMqttCommand(const QByteArray &payload)
{
    QString command;
    QJsonObject request;
    QJsonParseError parseError;
    // 尝试解析为 JSON
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error == QJsonParseError::NoError && document.isObject())
    {
        request = document.object();
        command = request.value(QStringLiteral("command")).toString().trimmed();
    }
    else
    {
        // 解析失败则视为纯文本命令（向后兼容）
        command = QString::fromUtf8(payload).trimmed();
    }

    bool accepted = true;
    QString result = QStringLiteral("命令已执行");

    /*
     * 命令分发：通过模拟按钮点击来触发已有的业务逻辑，
     * 避免重复实现相同的功能代码。
     */
    // ===== start_capture：远程启动摄像头采集 =====
    if (command == QLatin1String("start_capture"))
    {
        if (!startCaptureButton->isChecked()) // 仅当尚未启动时执行
            startCaptureButton->click();      // 模拟点击按钮 → 触发 setCaptureEnabled(true)
    }
    // ===== stop_capture：远程停止摄像头采集 =====
    else if (command == QLatin1String("stop_capture"))
    {
        if (startCaptureButton->isChecked()) // 仅当正在采集时执行
            startCaptureButton->click();     // 模拟点击按钮 → 触发 setCaptureEnabled(false)
    }
    // ===== stream_on：远程开启 TCP 实时推流 =====
    else if (command == QLatin1String("stream_on"))
    {
        if (!networkStreamingCheckBox->isChecked()) // 仅当推流未开启时执行
        {
            networkStreamingCheckBox->setChecked(true); // 更新 UI 复选框为选中状态
            networkStreamingToggled(true);              // 调用槽函数校验配置并启动推流
        }
    }
    // ===== stream_off：远程停止 TCP 实时推流 =====
    else if (command == QLatin1String("stream_off"))
    {
        if (networkStreamingCheckBox->isChecked()) // 仅当推流已开启时执行
        {
            networkStreamingCheckBox->setChecked(false); // 更新 UI 复选框为未选中状态
            networkStreamingToggled(false);              // 调用槽函数停止推流
        }
    }
    // ===== vision_on：远程开启视觉运动检测 =====
    else if (command == QLatin1String("vision_on"))
    {
        if (!visionDetectionCheckBox->isChecked()) // 仅当运动检测未开启时执行
        {
            visionDetectionCheckBox->setChecked(true); // 更新 UI 复选框为选中状态
            visionDetectionToggled(true);              // 调用槽函数配置并启动运动检测
        }
    }
    // ===== vision_off：远程停止视觉运动检测 =====
    else if (command == QLatin1String("vision_off"))
    {
        if (visionDetectionCheckBox->isChecked()) // 仅当运动检测已开启时执行
        {
            visionDetectionCheckBox->setChecked(false); // 更新 UI 复选框为未选中状态
            visionDetectionToggled(false);              // 调用槽函数停止运动检测
        }
    }
    // ===== snapshot：远程抓拍一张当前画面 =====
    else if (command == QLatin1String("snapshot"))
    {
        takeManualSnapshot(); // 复用已有的手动抓拍逻辑保存 JPEG 文件
    }
    // ===== set_stream_endpoint：远程修改 TCP 推流目标地址 =====
    else if (command == QLatin1String("set_stream_endpoint"))
    {
        const QString host = request.value(QStringLiteral("host")).toString().trimmed(); // 从 JSON 提取目标 IP
        const int port = request.value(QStringLiteral("port")).toInt();                  // 从 JSON 提取目标端口
        const QString transport = request.value(QStringLiteral("transport"))
                                      .toString(streamTransportComboBox->currentData().toString())
                                      .toLower();
        if (host.isEmpty() || port <= 0 || port > 65535 ||
            (transport != QLatin1String("tcp") && transport != QLatin1String("udp")))
        {
            accepted = false;                                    // 标记命令未接受
            result = QStringLiteral("视频服务端地址或端口无效"); // 设置错误原因
        }
        else
        {
            serverAddressEdit->setText(host);  // 更新 UI 中的 IP 输入框
            serverPortSpinBox->setValue(port); // 更新 UI 中的端口选择器
            streamTransportComboBox->setCurrentIndex(
                streamTransportComboBox->findData(transport));
            applyServerEndpoint();             // 应用新配置并同步到 TcpFrameSender
        }
    }
    // ===== 未知命令处理 =====
    else
    {
        accepted = false;                                     // 标记命令未被识别
        result = QStringLiteral("未知命令：%1").arg(command); // 将命令名嵌入错误消息
    }

    qInfo() << "收到MQTT远程命令：" << command << "结果：" << result;

    // ===== 回复命令确认 acknowledgement 到 command_ack 主题 =====
    // PC 接收端通过订阅 devices/{id}/command_ack 获取命令执行结果
    QJsonObject acknowledgement;                                                      // 创建空的 JSON 对象
    acknowledgement.insert(QStringLiteral("deviceId"), deviceId);                     // 设备唯一标识
    acknowledgement.insert(QStringLiteral("command"), command);                       // 回显原始命令名
    acknowledgement.insert(QStringLiteral("accepted"), accepted);                     // 命令是否被接受执行（bool）
    acknowledgement.insert(QStringLiteral("message"), result);                        // 执行结果描述（成功或错误原因）
    acknowledgement.insert(QStringLiteral("timestamp"),                               // 命令执行时间戳
                           QDateTime::currentDateTime().toString(Qt::ISODateWithMs)); // ISO 8601 格式带毫秒
    publishMqttJson(mqttTopic(QStringLiteral("command_ack")), acknowledgement);       // 通过 MQTT QoS 1 发布确认消息

    // 命令执行后立即发送一次心跳，确保 PC 端通过 status 主题看到最新设备状态
    publishHeartbeat();
}

#if 0
/*
 * ============================================================================
 * 学习版停用功能的原代码备份区
 *
 * 这里集中保存：人脸检测、边缘监控传感器状态/规则、LED/蜂鸣器报警、
 * 手动抓拍按钮和手动录像按钮。它们不会参与当前编译，也不会打断上面的
 * 主调用链。自动事件录像、事件数据库、MQTT 和“板级设备”页没有停用。
 * 恢复某项功能时，请把对应片段移回注释标出的原函数位置，不要直接把
 * 本区的 #if 0 改成 #if 1（这些片段来自多个函数，不能在文件作用域编译）。
 * ============================================================================
 */

// 原文件头依赖；恢复告警功能时移回 include 区域，并恢复 hardware.pri 条目：
#include "alarm_actuator.h"

// 原 CameraStreamingPage 构造函数初始化列表与元类型注册：
previousInferenceRuns(0),
previousInferenceMicroseconds(0),
qRegisterMetaType<QVector<QRect>>("QVector<QRect>");

// 原 initializeUi() 中的停用控件布局：
monitoringLayout->addWidget(faceDetectionCheckBox);
thresholdLayout->addRow(QStringLiteral("AI推理间隔"),
                        faceInferenceIntervalSpinBox);
thresholdLayout->addRow(QStringLiteral("AI置信度"),
                        faceConfidenceSpinBox);
monitoringLayout->addWidget(sensorMonitoringCheckBox);
monitoringLayout->addWidget(alarmLinkageCheckBox);
monitoringLayout->addWidget(automaticRecordingCheckBox);
monitoringLayout->addWidget(manualSnapshotButton);
monitoringLayout->addWidget(manualRecordingButton);
monitoringLayout->addWidget(faceStatusLabel);
controlLayout->addWidget(sensorGroup);

// 原 initializeServices() 中的告警对象及 UI 入口连接：
alarmActuator = new AlarmActuator(this);
connect(faceDetectionCheckBox, &QCheckBox::clicked,
        this, &CameraStreamingPage::faceDetectionToggled);
connect(sensorMonitoringCheckBox, &QCheckBox::clicked,
        this, &CameraStreamingPage::sensorMonitoringToggled);
connect(manualSnapshotButton, &QPushButton::clicked,
        this, &CameraStreamingPage::takeManualSnapshot);
connect(manualRecordingButton, &QPushButton::clicked,
        this, &CameraStreamingPage::manualRecordingToggled);

// 原 initializeServices() 中的人脸检测线程连接：
connect(this, &CameraStreamingPage::faceDetectorConfigurationChanged,
        visionDetector, &VisionEventDetector::configureFaceDetection,
        Qt::QueuedConnection);
connect(visionDetector, &VisionEventDetector::faceAnalysisCompleted,
        cameraCaptureThread,
        [this](const QImage &, const QVector<QRect> &faceBoxes)
        { cameraCaptureThread->updateFaceOverlay(faceBoxes); },
        Qt::DirectConnection);
connect(visionDetector, &VisionEventDetector::faceAnalysisCompleted,
        this, &CameraStreamingPage::handleFaceAnalysisCompleted,
        Qt::QueuedConnection);
connect(visionDetector, &VisionEventDetector::facePresenceDetected,
        this, &CameraStreamingPage::handleFacePresenceDetected,
        Qt::QueuedConnection);
connect(visionDetector, &VisionEventDetector::faceDetectionStatusChanged,
        this, &CameraStreamingPage::handleFaceDetectionStatusChanged,
        Qt::QueuedConnection);

connect(faceInferenceIntervalSpinBox, &QSpinBox::editingFinished,
        this, [this]()
        {
            saveSettings();
            if (faceDetectionCheckBox->isChecked())
                faceDetectionToggled(true);
        });
connect(faceConfidenceSpinBox, &QSpinBox::editingFinished,
        this, [this]()
        {
            saveSettings();
            if (faceDetectionCheckBox->isChecked())
                faceDetectionToggled(true);
        });
initializeSensors();

// 原 loadSettings() 中的人脸参数读取：
faceInferenceIntervalSpinBox->setValue(
    settings.value(QStringLiteral("edge/faceInferenceInterval"), 3).toInt());
faceConfidenceSpinBox->setValue(
    settings.value(QStringLiteral("edge/faceConfidencePercent"), 60).toInt());

// 原 saveSettings() 中的人脸参数保存：
settings.setValue(QStringLiteral("edge/faceInferenceInterval"),
                  faceInferenceIntervalSpinBox->value());
settings.setValue(QStringLiteral("edge/faceConfidencePercent"),
                  faceConfidenceSpinBox->value());

// 原 updatePerformancePanel() 中的人脸推理性能统计：
const VisionPipelineStatistics vision = visionDetector->statistics();
const quint64 inferenceRunDelta = vision.inferenceRuns - previousInferenceRuns;
const quint64 inferenceTimeDelta =
    vision.totalInferenceMicroseconds - previousInferenceMicroseconds;
const double inferenceFps = double(inferenceRunDelta) / elapsedSeconds;
const double averageInferenceMs = inferenceRunDelta > 0
        ? double(inferenceTimeDelta) / inferenceRunDelta / 1000.0
        : 0.0;
QString backendText = QStringLiteral("已关闭");
if (vision.activeFaceBackend == VisionEventDetector::FaceBackendLightweightDnn)
    backendText = QStringLiteral("轻量DNN");
else if (vision.activeFaceBackend == VisionEventDetector::FaceBackendHaar)
    backendText = QStringLiteral("Haar回退");
previousInferenceRuns = vision.inferenceRuns;
previousInferenceMicroseconds = vision.totalInferenceMicroseconds;

// 原 updateAnalysisFrameDistribution() 中的人脸分析条件：
const bool needsAnalysisWithFace = visionDetectionCheckBox->isChecked() ||
        faceDetectionCheckBox->isChecked() || !localPreviewCheckBox->isChecked();

// 原 takeManualSnapshot() 中的人脸框叠加：
if (faceDetectionCheckBox->isChecked() && !latestFaceBoxes.isEmpty() &&
    !snapshotImage.isNull())
{
    QPainter painter(&snapshotImage);
    QPen pen(QColor(46, 220, 135));
    pen.setWidth(3);
    painter.setPen(pen);
    for (const QRect &box : latestFaceBoxes)
        painter.drawRect(box.intersected(snapshotImage.rect()));
}

// 原 publishHeartbeat() 中的人脸与传感器字段：
status.insert(QStringLiteral("faceDetectionEnabled"),
              faceDetectionCheckBox->isChecked());
status.insert(QStringLiteral("facesDetected"), detectedFaceCount);
status.insert(QStringLiteral("sensorMonitoringEnabled"),
              sensorMonitoringCheckBox->isChecked());
status.insert(QStringLiteral("ambientLight"), ambientLightValue);
status.insert(QStringLiteral("proximity"), proximityValue);
status.insert(QStringLiteral("temperature"), temperatureValue);

// 原 recordMonitoringEvent() 中的 LED/蜂鸣器联动：
if (alarmLinkageCheckBox->isChecked() &&
    severity != MonitoringEventSeverity::Information)
    alarmActuator->pulse(2000);

// 原 handleMqttCommand() 中的人脸、传感器与告警命令：
else if (command == QLatin1String("face_on"))
{
    if (!faceDetectionCheckBox->isChecked())
    {
        faceDetectionCheckBox->setChecked(true);
        faceDetectionToggled(true);
    }
}
else if (command == QLatin1String("face_off"))
{
    if (faceDetectionCheckBox->isChecked())
    {
        faceDetectionCheckBox->setChecked(false);
        faceDetectionToggled(false);
    }
}
else if (command == QLatin1String("sensors_on"))
{
    if (!sensorMonitoringCheckBox->isChecked())
    {
        sensorMonitoringCheckBox->setChecked(true);
        sensorMonitoringToggled(true);
    }
}
else if (command == QLatin1String("sensors_off"))
{
    if (sensorMonitoringCheckBox->isChecked())
    {
        sensorMonitoringCheckBox->setChecked(false);
        sensorMonitoringToggled(false);
    }
}
else if (command == QLatin1String("alarm_on"))
{
    alarmActuator->setAlarmActive(true);
}
else if (command == QLatin1String("alarm_off"))
{
    alarmActuator->setAlarmActive(false);
}
#endif
