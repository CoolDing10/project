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
 *   6. loadSettings()/saveSettings() — 配置持久化
 *   7. updateLocalPreview()/displayPreviewImage() — 帧显示
 *   8. 网络推流与画面变化检测
 *   9. MQTT 心跳与远程命令
 *  10. 析构函数
 *
 * 数据流图（第2周核心链路）：
 *   V4L2 → CaptureThread::run()
 *     ├── localPreviewReady → updateLocalPreview() [主线程]
 *     │     └── displayPreviewImage() → cameraPreviewLabel
 *     ├── networkFrameReady → TcpFrameSender::sendFrame() [独立线程]
 *     └── analysisFrameReady → VisionEventDetector::analyzeFrame() [独立线程]
 *   事件 → EventStore(SQLite) + 自动事件录像 + MQTT push
 */
#include "video_monitor.h" // 声明 CameraStreamingPage 页面类及其信号、槽和成员变量。

#include "capture_thread.h" // 声明 CaptureThread，负责从 V4L2 摄像头持续取得图像帧。
// 网络与存储层
#include "tcp_frame_sender.h"     // FFmpeg H.264 编码 + TCP/UDP 推流器
#include "event_store.h"          // SQLite 事件存储
#include "media_archive_server.h" // PC端事件录像归档服务
// 视觉检测层
#include "vision_event_detector.h" // 画面变化检测

// MQTT 通信（需要 qtmqtt 模块）
#include <mqtt/qmqttclient.h> // 提供 QMqttClient、MQTT 连接状态和消息收发接口。

// Qt Widgets：UI 控件
#include <QCheckBox>        // 提供可勾选的功能开关控件。
#include <QComboBox>        // 提供 TCP/UDP 传输方式下拉选择框。
#include <QCoreApplication> // 用于取得程序可执行文件所在目录。
#include <QDateTime>        // 用于生成事件时间戳和录像文件名。
#include <QDebug>           // 提供 qInfo()、qWarning() 日志输出。
#include <QDir>             // 用于安全地拼接录像目录和文件路径。
#include <QFileInfo>        // 用于从完整录像路径中提取文件名。
#include <QFormLayout>      // 提供“标签—输入控件”形式的表单布局。
#include <QFrame>           // 提供滚动区域边框样式枚举 QFrame::NoFrame。
#include <QGroupBox>        // 提供带标题的功能分组容器。
#include <QHBoxLayout>      // 提供水平方向的控件布局。
// Qt JSON 处理
#include <QJsonDocument>  // 负责 QByteArray 与 JSON 文档之间的序列化和解析。
#include <QJsonObject>    // 表示 MQTT 消息中使用的 JSON 键值对象。
#include <QLabel>         // 显示预览画面和各项文字状态。
#include <QLineEdit>      // 输入推流服务器和 MQTT 服务器地址。
#include <QListWidget>    // 显示最近发生的监控事件列表。
#include <QMetaObject>    // 提供跨线程 invokeMethod() 调用。
#include <QPixmap>        // 将 QImage 转为 QLabel 可显示的像素图。
#include <QPushButton>    // 提供摄像头采集启停按钮。
#include <QResizeEvent>   // 描述页面尺寸变化事件。
#include <QScrollArea>    // 让右侧控制面板在小屏幕上可以滚动。
#include <QSettings>      // 持久化服务器地址、端口和检测阈值。
#include <QSignalBlocker> // 临时阻止控件发出信号，避免程序修改状态时递归触发槽。
#include <QSpinBox>       // 输入端口号和运动检测阈值。
#include <QTimer>         // 实现 MQTT 心跳和断线重连定时任务。
#include <QVBoxLayout>    // 提供垂直方向的控件布局。

/*
 * ===== 匿名命名空间：系统常量与辅助函数 =====
 * 匿名命名空间中的内容仅在当前 .cpp 文件中可见（相当于 C 中的 static 函数/变量）。
 * linker 不会导出这些符号，避免命名冲突。
 */
namespace // 匿名命名空间使下面的常量和辅助函数只对本实现文件可见。
{
    /*
     * ===== 事件与告警阈值常量 =====
     * 这些值是经过实际测试调优的经验值，可根据应用场景调整。
     */

    // 事件触发时自动录像 10 秒（由 TcpFrameSender 的定时器控制停止）
    // 10000 ms = 10 秒
    const int EventRecordingDurationMs = 10000; // int 常量，单位为毫秒，作为 startRecording 的录像时长参数。
    // UI 事件列表最多保留 100 条，超出的旧记录自动删除，防止内存无限增长
    const int MaximumVisibleEvents = 100; // int 常量，限制 QListWidget 中保留的最大事件条数。
    /*
     * severityText() — 将事件严重等级枚举转换为 UI 可显示的中文文本
     * 参数 severity：MonitoringEventSeverity 枚举值
     * 返回对应的中文字符串
     */
    QString severityText(MonitoringEventSeverity severity) // severity 是待转换的事件等级，返回对应中文文本。
    {
        switch (severity) // 根据枚举值分支
        {
        case MonitoringEventSeverity::Critical: // 严重事件分支。
            return QStringLiteral("严重"); // 需要立即关注
        case MonitoringEventSeverity::Warning: // 警告事件分支。
            return QStringLiteral("警告"); // 需要关注
        case MonitoringEventSeverity::Information: // 普通信息事件分支。
        default:                                   // 未知枚举值也按普通信息处理，保证函数一定返回文本。
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
    QColor severityColor(MonitoringEventSeverity severity) // severity 是待映射的等级，返回 UI 使用的文字颜色。
    {
        switch (severity) // 根据枚举值分支
        {
        case MonitoringEventSeverity::Critical: // 严重事件使用红色。
            return QColor(QStringLiteral("#ff5555")); // 红色
        case MonitoringEventSeverity::Warning: // 警告事件使用橙色。
            return QColor(QStringLiteral("#ffaa33")); // 橙色
        case MonitoringEventSeverity::Information: // 普通信息使用蓝色。
        default:                                   // 未知等级也使用信息颜色，避免返回无效 QColor。
            return QColor(QStringLiteral("#55ccff")); // 蓝色
        }
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
 *   基本类型初始化为明确的默认状态。
 */
CameraStreamingPage::CameraStreamingPage(QWidget *parent) // parent 指向父控件，由 Qt 父子对象机制管理本页面寿命。
    : QWidget(parent),                       // 调用 QWidget 基类构造函数
                                             // ---- UI 控件初始化为 nullptr（确保构造中途失败时析构安全）----
      cameraPreviewLabel(nullptr),           // 摄像头实时预览 QLabel
      startCaptureButton(nullptr),           // 采集启停按钮
      localPreviewCheckBox(nullptr),         // 本地预览开关
      networkStreamingCheckBox(nullptr),     // TCP 推流开关
      visionDetectionCheckBox(nullptr),      // 运动检测开关
      automaticRecordingCheckBox(nullptr),   // 事件自动录像开关
      mqttEnabledCheckBox(nullptr),          // MQTT 连接开关
      streamTransportComboBox(nullptr),      // TCP/UDP 推流方式
      serverAddressEdit(nullptr),            // TCP 推流目标 IP
      serverPortSpinBox(nullptr),            // TCP 推流端口
      mqttHostEdit(nullptr),                 // MQTT 服务器 IP
      mqttPortSpinBox(nullptr),              // MQTT 端口
      motionThresholdSpinBox(nullptr),       // 运动检测阈值
      connectionStatusLabel(nullptr),        // TCP 连接状态
      mqttStatusLabel(nullptr),              // MQTT 状态
      visionStatusLabel(nullptr),            // 运动检测状态
      recordingStatusLabel(nullptr),         // 录像状态
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
      mqttClient(nullptr),          // MQTT 客户端
      heartbeatTimer(nullptr),      // MQTT 心跳定时器
      mqttReconnectTimer(nullptr),  // MQTT 重连定时器
      // ---- 状态变量初始化 ----
      deviceId(QStringLiteral("imx6ull-001")), // 设备唯一标识（MQTT 主题中使用）
      eventStoreReady(false),                  // SQLite 数据库就绪标志
      eventRecordingActive(false)              // 当前是否有事件录像在进行
{
    initializeUi();       // 步骤 1：创建所有 UI 控件并布局
    loadSettings();       // 步骤 2：从 QSettings 加载上次保存的配置
    initializeServices(); // 步骤 3：创建子线程 + 连接信号槽（第 2 周核心）
}
/*
 * ===== 析构函数：安全退出所有线程 + 保存配置 =====
 *
 * 线程退出顺序（非常重要，不能乱）：
 *   1. 停所有定时器和 MQTT
 *   2. 停采集线程（生产者）
 *   3. 停视觉、归档和网络工作线程（消费者）
 *   4. 最后释放事件数据库
 */
CameraStreamingPage::~CameraStreamingPage() // 页面销毁时依次停止生产者、消费者和外部连接。
{
    saveSettings(); // 保存 CheckBox、IP、端口等配置到 QSettings
    // 停所有定时器
    if (heartbeatTimer) // 指针非空才可调用定时器方法，兼容构造未完全结束的情况。
        heartbeatTimer->stop(); // 停止 MQTT 心跳定时器
    if (mqttReconnectTimer) // 指针非空时停止尚未触发的重连任务。
        mqttReconnectTimer->stop(); // 停止 MQTT 重连定时器
    // 断 MQTT 连接
    if (mqttClient && mqttClient->state() != QMqttClient::Disconnected) // 客户端存在且尚未断开时才发送断开请求。
        mqttClient->disconnectFromHost(); // 主动断开 MQTT 连接

    // 停止采集线程（先停采集，再处置消费线程，避免生产者向已死的消费者发信号）
    cameraCaptureThread->setCaptureEnabled(false); // ① 置采集标志为 false → while 循环退出
    cameraCaptureThread->wait();                   // ② 阻塞等待 run() 完全结束

    // 停止视觉检测线程
    if (visionWorkerThread->isRunning()) // 仅对仍在运行的视觉线程执行 quit()+wait()。
    {
        visionWorkerThread->quit(); // 请求退出事件循环
        visionWorkerThread->wait(); // 等待线程结束
    }
    // 停止媒体归档服务线程
    if (archiveWorkerThread && archiveWorkerThread->isRunning()) // 归档对象已创建且线程仍运行时执行同步停止。
    {
        QMetaObject::invokeMethod(archiveServer, "stop",         // 跨线程同步调用 stop() 方法
                                  Qt::BlockingQueuedConnection); // 阻塞直到目标线程执行完毕
        archiveWorkerThread->quit();                             // 退出事件循环
        archiveWorkerThread->wait();                             // 等待线程退出
    }
    // 停止网络推流线程（跨线程同步调用 stop，确保资源安全释放）
    if (networkWorkerThread->isRunning()) // 仅在线程运行时跨线程停止发送器并等待退出。
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
 *   │  └─────────────────────────────┘  │  │ mqttGroup      │  │
 *   │                                  │  │ eventGroup     │  │
 *   │                                  │  └────────────────┘  │
 *   └─────────────────────────────────────────────────────────┘
 *
 * 当前显示 4 个 GroupBox（从上到下）：
 *   1. streamGroup    — TCP 推流目标配置（IP + 端口 + 状态）
 *   2. monitoringGroup— 核心监控控制（采集/预览/检测/自动事件录像）
 *   3. mqttGroup      — MQTT 连接配置与状态
 *   4. eventGroup     — 最近事件列表
 */
void CameraStreamingPage::initializeUi() // 创建页面控件、设置样式，并建立控件的父子对象与布局关系。
{
    // ===== 页面基础样式（暗色主题样式表） =====
    setObjectName(QStringLiteral("edgeMonitoringPage"));               // 设置对象名（CSS 选择器使用）
    setStyleSheet(QStringLiteral(                                      // 设置暗色主题全局样式表
        "#edgeMonitoringPage { background: #1f1f22; color: #f5f7fa; }" // 页面背景深灰色
        "#edgeControlPanel, #edgeControlScrollArea, "                  // 选择右侧控制面板和滚动区域，
        "#edgeControlScrollArea > QWidget > QWidget { background: #1f1f22; }" // 统一其背景色，避免露出默认浅色底。
        "QGroupBox { background: #29292e; color: #f5f7fa; " // GroupBox 深灰色背景
        "border: 1px solid #68686f; border-radius: 6px; "              // 设置分组边框和圆角，
        "margin-top: 12px; padding: 12px 6px 6px 6px; font-weight: 600; }" // 留出标题空间并使用较粗字体。
        "QGroupBox::title { subcontrol-origin: margin; left: 9px; "    // 定位分组标题，
        "padding: 0 4px; color: #ffffff; }"                            // 设置标题内边距和白色文字。
        "QLabel { color: #eef0f3; background: transparent; }" // 标签浅色文字
        "QLabel:disabled { color: #aeb3ba; }"                          // 禁用标签使用较暗文字。
        "QCheckBox { color: #eef0f3; spacing: 8px; background: transparent; }" // 设置复选框文字、间距和透明背景。
        "QCheckBox:disabled { color: #aeb3ba; }"                       // 禁用复选框使用灰色文字。
        "QCheckBox::indicator { width: 24px; height: 24px; }" // 复选框指示器放大（适合触摸屏）
        "QCheckBox::indicator:unchecked { background: #202126; "       // 未勾选指示器使用深色底，
        "border: 2px solid #d9dde3; border-radius: 4px; }"             // 配浅色边框和圆角。
        "QCheckBox::indicator:unchecked:hover { background: #30343a; " // 鼠标悬停时提高背景亮度，
        "border-color: #55b7ff; }"                                    // 并用蓝色边框提示可点击。
        "QCheckBox::indicator:unchecked:disabled { background: #33343a; " // 禁用且未勾选时使用灰色底，
        "border-color: #858a91; }"                                    // 边框也降低亮度。
        "QPushButton { min-height: 34px; border-radius: 6px; background: #e8e8e8; " // 设置按钮最小高度、圆角和浅色底，
        "color: #202020; padding: 3px 8px; }"                          // 配深色文字和内边距。
        "QPushButton:checked { background: #d9534f; color: white; }" // 按下状态红色
        "QPushButton:disabled { background: #44444a; color: #bfc3c8; }" // 禁用按钮使用深灰背景和灰色文字。
        "QLineEdit, QSpinBox { min-height: 28px; background: #ffffff; "  // 输入控件保持足够触摸高度和白色底，
        "color: #17191c; border: 1px solid #85858b; border-radius: 3px; }" // 配深色文字、边框和小圆角。
        "QLineEdit:disabled, QSpinBox:disabled { background: #d7d7da; " // 禁用输入控件切换为浅灰底，
        "color: #55585d; }"                                           // 使用灰色文字表示不可编辑。
        "QListWidget { background: #151515; color: #f5f5f5; "         // 事件列表使用黑色底和浅色文字，
        "border: 1px solid #68686f; }"                                // 并添加灰色边框。
        "#previewPanel { background: #151a21; border: 1px solid #303a47; " // 预览面板使用蓝灰背景与边框，
        "border-radius: 9px; }" // 预览面板深蓝灰色
        "#previewTitle { color: #f5f8fb; font-size: 15px; font-weight: 600; }" // 预览标题使用较大加粗浅色字。
        "#previewHint { color: #8291a3; font-size: 11px; }"            // 预览提示使用较小的低对比度文字。
        /* 仅放大右侧控制区滚动条，适合 1024x600 触摸屏操作。 */
        "#edgeControlScrollArea QScrollBar:vertical { "                 // 选择控制区的竖直滚动条，
        "background: #202126; width: 22px; margin: 2px; "               // 加宽轨道以适合触摸操作，
        "border: 1px solid #454952; border-radius: 9px; }"              // 并设置边框与圆角。
        "#edgeControlScrollArea QScrollBar::handle:vertical { "         // 选择滚动条滑块，
        "background: #8f98a6; min-height: 76px; margin: 2px; "          // 保证滑块足够大且与轨道留间距，
        "border: 1px solid #b7c0cc; border-radius: 7px; }"              // 设置滑块边框和圆角。
        "#edgeControlScrollArea QScrollBar::handle:vertical:hover, "    // 鼠标悬停或
        "#edgeControlScrollArea QScrollBar::handle:vertical:pressed { " // 按住滑块时，
        "background: #48a9e6; border-color: #82cdf7; }"                 // 改为蓝色突出交互反馈。
        "#edgeControlScrollArea QScrollBar::add-line:vertical, "        // 选择滚动条的增加按钮和
        "#edgeControlScrollArea QScrollBar::sub-line:vertical { height: 0; }" // 减少按钮，并隐藏两端箭头。
        "#edgeControlScrollArea QScrollBar::add-page:vertical, "        // 选择滑块两侧的页面区域，
        "#edgeControlScrollArea QScrollBar::sub-page:vertical { "       // 同时覆盖增加和减少方向，
        "background: transparent; }"));                                // 使用透明背景并结束样式字符串。

    // ===== 摄像头预览区域（左侧主画面） =====
    cameraPreviewLabel = new QLabel(QStringLiteral("等待采集画面"), this); // 创建预览 QLabel
    cameraPreviewLabel->setObjectName(QStringLiteral("cameraPreview"));    // 设置对象名（CSS 选择器）
    cameraPreviewLabel->setAlignment(Qt::AlignCenter);                     // 文字居中显示
    cameraPreviewLabel->setMinimumSize(320, 240);                          // 最小尺寸 320×240
    cameraPreviewLabel->setSizePolicy(QSizePolicy::Expanding,              // 水平方向可扩展
                                      QSizePolicy::Expanding);             // 垂直方向可扩展
    cameraPreviewLabel->setStyleSheet(                                     // 预览区域深色背景
        "#cameraPreview { background: #05070a; color: #8793a3; "       // 预览区使用接近黑色的背景和灰蓝提示文字，
        "border: 1px solid #26313d; border-radius: 6px; }");            // 添加边框与圆角并结束局部样式。

    // ===== 右侧控制面板容器 =====
    QWidget *controlPanel = new QWidget(this);                       // 控制面板父容器
    controlPanel->setObjectName(QStringLiteral("edgeControlPanel")); // 对象名（CSS 选择器）
    QVBoxLayout *controlLayout = new QVBoxLayout(controlPanel);      // 垂直布局管理器
    controlLayout->setContentsMargins(6, 6, 6, 6);                   // 布局内边距
    controlLayout->setSpacing(10);                                   // 组件间距

    // ===== GroupBox 1: TCP/UDP 推流配置 =====
    QGroupBox *streamGroup = new QGroupBox(QStringLiteral("实时视频推流配置"), controlPanel); // 推流参数分组，父对象为控制面板。
    QFormLayout *streamLayout = new QFormLayout(streamGroup);                                // 表单布局（标签-输入框）
    streamTransportComboBox = new QComboBox(streamGroup); // 传输协议选择框，每项同时保存显示文字和内部协议值。
    streamTransportComboBox->addItem(QStringLiteral("H.264/TCP（可靠传输）"), QStringLiteral("tcp")); // 添加可靠的 TCP 选项，用户数据为 "tcp"。
    streamTransportComboBox->addItem(QStringLiteral("H.264/RTP/UDP（低延迟）"), QStringLiteral("udp")); // 添加低延迟 UDP 选项，用户数据为 "udp"。
    serverAddressEdit = new QLineEdit(QStringLiteral("192.168.10.100"), streamGroup);        // IP 输入框（默认值）
    serverPortSpinBox = new QSpinBox(streamGroup);                                           // 端口选择器
    serverPortSpinBox->setRange(1, 65535);                                                   // 端口范围 1-65535
    serverPortSpinBox->setValue(8888);                                                       // 默认端口 8888
    connectionStatusLabel = new QLabel(QStringLiteral("实时推流未开启"), streamGroup);         // 发送状态显示
    connectionStatusLabel->setWordWrap(true);                                                // 允许自动换行
    streamLayout->addRow(QStringLiteral("传输方式"), streamTransportComboBox); // 把协议标签和下拉框加入表单。
    streamLayout->addRow(QStringLiteral("服务器IP"), serverAddressEdit);                     // 添加 IP 行
    streamLayout->addRow(QStringLiteral("端口"), serverPortSpinBox);                         // 添加端口行
    streamLayout->addRow(QStringLiteral("连接状态"), connectionStatusLabel);                 // 添加状态行

    // ===== GroupBox 2: 监控控制 =====
    QGroupBox *monitoringGroup = new QGroupBox(QStringLiteral("监控控制"), controlPanel); // 核心采集、分析与录像开关分组。
    QVBoxLayout *monitoringLayout = new QVBoxLayout(monitoringGroup);                            // 垂直布局
    startCaptureButton = new QPushButton(QStringLiteral("开始采集摄像头数据"), monitoringGroup); // 采集按钮
    startCaptureButton->setCheckable(true);                                                      // 按钮可切换状态（按下/弹起）
    localPreviewCheckBox = new QCheckBox(QStringLiteral("本地实时预览"), monitoringGroup);       // 预览开关
    networkStreamingCheckBox = new QCheckBox(QStringLiteral("实时视频推流"), monitoringGroup); // 推流开关
    visionDetectionCheckBox = new QCheckBox(QStringLiteral("视觉变化检测"), monitoringGroup);    // 运动检测
    automaticRecordingCheckBox = new QCheckBox(QStringLiteral("事件自动录像"), monitoringGroup); // 自动录像
    // 运动检测阈值 SpinBox
    motionThresholdSpinBox = new QSpinBox(monitoringGroup); // 变化比例阈值输入框，数值单位为百分比。
    motionThresholdSpinBox->setRange(1, 80);                 // 阈值范围 1%-80%
    motionThresholdSpinBox->setValue(12);                    // 默认阈值 12%
    motionThresholdSpinBox->setSuffix(QStringLiteral(" %")); // 后缀显示 "%"
    // 状态标签
    visionStatusLabel = new QLabel(QStringLiteral("视觉检测已关闭"), // 运动检测状态
                                   monitoringGroup);                    // 父对象设为监控控制分组。
    visionStatusLabel->setWordWrap(true);                          // 允许换行
    recordingStatusLabel = new QLabel(QStringLiteral("事件录像未启动"), // 录像状态
                                      monitoringGroup);                  // 父对象设为监控控制分组。
    recordingStatusLabel->setWordWrap(true); // 允许换行
    // 按顺序添加到布局
    monitoringLayout->addWidget(startCaptureButton);        // 采集按钮
    monitoringLayout->addWidget(localPreviewCheckBox);      // 预览开关
    monitoringLayout->addWidget(networkStreamingCheckBox);  // 推流开关
    monitoringLayout->addWidget(visionDetectionCheckBox);   // 运动检测
    QFormLayout *thresholdLayout = new QFormLayout();       // 阈值子布局
    thresholdLayout->addRow(QStringLiteral("画面变化阈值"), // 阈值行
                            motionThresholdSpinBox);         // 将百分比输入框作为该行的字段控件。
    monitoringLayout->addLayout(thresholdLayout);            // 添加阈值子布局
    monitoringLayout->addWidget(automaticRecordingCheckBox); // 自动录像
    monitoringLayout->addWidget(visionStatusLabel);          // 运动状态
    monitoringLayout->addWidget(recordingStatusLabel);       // 录像状态

    // ===== GroupBox 3: MQTT 设备管理 =====
    QGroupBox *mqttGroup = new QGroupBox(QStringLiteral("MQTT设备管理"), controlPanel); // MQTT 地址、连接开关和状态分组。
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

    // ===== GroupBox 4: 最近事件列表 =====
    QGroupBox *eventGroup = new QGroupBox(QStringLiteral("最近事件"), controlPanel); // 最近事件列表的容器分组。
    QVBoxLayout *eventLayout = new QVBoxLayout(eventGroup); // 垂直布局
    eventList = new QListWidget(eventGroup);                // 事件列表控件
    eventList->setMinimumHeight(150);                       // 最小高度 150px
    eventLayout->addWidget(eventList);                      // 添加到布局

    // 将 4 个 GroupBox 按顺序添加到控制面板垂直布局
    controlLayout->addWidget(streamGroup);      // ① TCP 推流
    controlLayout->addWidget(monitoringGroup);  // ② 监控控制
    controlLayout->addWidget(mqttGroup);        // ③ MQTT
    controlLayout->addWidget(eventGroup);       // ④ 事件列表
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
        QStringLiteral("实时预览、画面变化检测与事件录像"),                         // 提示文字内容。
        previewPanel);                                                               // 父对象为左侧预览面板。
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
 */
void CameraStreamingPage::initializeServices() // 创建后台对象、划分线程归属，并连接页面的数据流和控制流。
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
    /*
     * ===== 第一组：UI 按钮 → 后端/控制逻辑 =====
     * 这些连接运行在主线程，AutoConnection 自动选择 DirectConnection。
     * 信号来源是 UI 控件（主线程），接收方是 CaptureThread 或其他主线程槽函数。
     */
    // 采集按钮 → 采集线程的 setCaptureEnabled(bool) 槽
    connect(startCaptureButton, &QPushButton::clicked,               // 按钮点击信号
            cameraCaptureThread, &CaptureThread::setCaptureEnabled); // 启动/停止 V4L2 采集
    // 采集按钮 → 更新按钮文字（"开始采集" ↔ "停止采集"）
    connect(startCaptureButton, &QPushButton::clicked,                // 同一个点击信号还用于更新页面文字和状态。
            this, &CameraStreamingPage::updateCaptureButtonText); // 更新按钮文字
    // 本地预览 CheckBox → 采集线程：开关本地预览帧分发
    connect(localPreviewCheckBox, &QCheckBox::clicked,                    // CheckBox 点击信号
            cameraCaptureThread, &CaptureThread::setLocalPreviewEnabled); // 控制是否发射 localPreviewReady
    connect(localPreviewCheckBox, &QCheckBox::clicked, this,              // 本地预览开关的点击信号仍在主线程处理。
            [this](bool) { updateAnalysisFrameDistribution(); });         // bool 参数在此无需使用；重新计算是否要向分析线程送帧。
    // 网络推流 CheckBox → networkStreamingToggled 槽（校验 IP 后配置 TcpFrameSender）
    connect(networkStreamingCheckBox, &QCheckBox::clicked,             // 点击推流复选框时传递新的 bool 状态。
            this, &CameraStreamingPage::networkStreamingToggled); // 校验 + 配置 + 保存
    // 运动检测 CheckBox → visionDetectionToggled 槽
    connect(visionDetectionCheckBox, &QCheckBox::clicked,              // 点击检测复选框时传递新的 bool 状态。
            this, &CameraStreamingPage::visionDetectionToggled); // 配置 VisionEventDetector

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
            Qt::QueuedConnection);                               // 排队执行，避免在主线程直接操作网络线程对象。
    // 主线程 → TcpFrameSender：请求停止录像
    connect(this, &CameraStreamingPage::recordingStopRequested, // 主线程信号
            frameSender, &TcpFrameSender::stopRecording,        // 网络线程槽
            Qt::QueuedConnection);                                   // 排队执行，避免主线程阻塞视觉分析。
    // TcpFrameSender → 主线程：TCP 连接状态文字更新
    connect(frameSender, &TcpFrameSender::statusChanged, // 网络线程信号
            connectionStatusLabel, &QLabel::setText,     // 主线程 UI 更新
            Qt::QueuedConnection);                       // 跨线程
    // TcpFrameSender → 主线程：录像状态变更通知
    connect(frameSender, &TcpFrameSender::recordingStateChanged,     // 网络线程信号
            this, &CameraStreamingPage::handleRecordingStateChanged, // 主线程槽
            Qt::QueuedConnection);                                    // 录像状态跨线程排队回主线程处理。
    // TcpFrameSender → 主线程：录像错误通知
    connect(frameSender, &TcpFrameSender::recordingError,     // 网络线程信号
            this, &CameraStreamingPage::handleRecordingError, // 主线程槽
            Qt::QueuedConnection);                                    // 录像错误跨线程排队回主线程显示。

    /*
     * ===== 第四组：视觉检测线程（线程 3）的信号连接 =====
     */
    // 线程 3 结束时 → 自动 delete visionDetector
    connect(visionWorkerThread, &QThread::finished, // 线程退出信号
            visionDetector, &QObject::deleteLater); // 延迟删除
    // 线程 4（archive）启动时 → 初始化 HTTP 归档服务（监听 8890）
    connect(archiveWorkerThread, &QThread::started,                    // 归档线程事件循环启动后发出 started 信号。
            archiveServer, [this]()                                   // Lambda 捕获页面指针，以访问 archiveServer 成员。
            { archiveServer->initialize(8890); });                     // 在线程 4 内启动监听 8890 端口的归档服务。
    // 线程 4 结束时 → 删除 archiveServer
    connect(archiveWorkerThread, &QThread::finished,                   // 归档线程结束信号。
            archiveServer, &QObject::deleteLater);                    // 安排安全释放归档服务对象。
    // archive 状态日志
    connect(archiveServer, &MediaArchiveServer::statusChanged, this,   // 接收归档线程产生的状态文本。
            [](const QString &status)                                 // status 参数是归档服务给出的可读状态说明。
            { qInfo() << status; },                                   // 将状态写入应用日志，Lambda 不保存任何状态。
            Qt::QueuedConnection);                                    // 跨线程排队到主线程执行日志回调。
    // 主线程 → VisionEventDetector：运动检测参数配置（跨线程）
    connect(this, &CameraStreamingPage::detectorConfigurationChanged, // 主线程信号
            visionDetector, &VisionEventDetector::configure,          // 视觉线程槽
            Qt::QueuedConnection);                                    // 配置请求排队送入视觉工作线程。
    // VisionEventDetector → 主线程：每帧分析完成（变化比例更新）
    connect(visionDetector, &VisionEventDetector::analysisCompleted, // 视觉线程信号
            this, &CameraStreamingPage::handleAnalysisCompleted,     // 主线程槽
            Qt::QueuedConnection);                                    // 分析结果排队回到主线程更新 UI。
    // VisionEventDetector → 主线程：检测到画面运动事件（记录事件并可自动录像）
    connect(visionDetector, &VisionEventDetector::visualMotionDetected, // 视觉线程信号
            this, &CameraStreamingPage::handleVisualMotionDetected,     // 主线程槽
            Qt::QueuedConnection);                                    // 告警排队回到主线程统一记录。

    /*
     * ===== 第五组：输入框参数变更 → 即时应用 =====
     */
    // 服务器 IP 编辑完成 → 保存并更新推流配置
    connect(serverAddressEdit, &QLineEdit::editingFinished,   // 编辑完成信号（回车或失焦）
            this, &CameraStreamingPage::applyServerEndpoint); // 应用新地址
    // 端口变化 → 保存并更新推流配置
    connect(serverPortSpinBox,                                         // 监听推流端口输入框的值变化。
            QOverload<int>::of(&QSpinBox::valueChanged), // valueChanged(int) 信号
            this, [this](int)                            // 忽略具体端口值
            { applyServerEndpoint(); });                 // 调用配置下发
    connect(streamTransportComboBox,                                  // 监听传输协议下拉框。
            QOverload<int>::of(&QComboBox::currentIndexChanged),       // 明确选择带 int 新索引参数的重载信号。
            this, [this](int) { applyServerEndpoint(); });             // 此处不使用索引值，统一从控件读取当前协议并应用。
    // 运动阈值变化 → 立即更新 VisionEventDetector 配置
    connect(motionThresholdSpinBox,                                    // 监听画面变化阈值输入框。
            QOverload<int>::of(&QSpinBox::valueChanged),               // 明确选择 valueChanged(int) 信号重载。
            this, [this](int threshold)                                                          // 捕获新阈值
            { emit detectorConfigurationChanged(                                                 // 发射跨线程信号
                  visionDetectionCheckBox->isChecked(), threshold); }); // 传递是否启用 + 阈值

    // ===== 启动工作线程 =====
    networkWorkerThread->start(); // 启动线程 2（开始事件循环）
    visionWorkerThread->start();  // 启动线程 3
    archiveWorkerThread->start(); // 启动线程 4

    initializeMqtt(); // 初始化 MQTT 客户端
} // initializeServices() 结束

/*
 * ===== initializeMqtt() — 初始化 MQTT 客户端 =====
 *
 * 主题结构：devices/{deviceId}/{status|events|commands}
 *   - status:   设备通过 publishHeartbeat() 定时上报 JSON 遥测数据
 *   - events:   告警事件推送（recordMonitoringEvent 中发布）
 *   - commands: PC 接收端通过此主题下发远程命令（handleMqttCommand 响应）
 *
 * 心跳机制：每 10 秒发布一次采集、推流、检测和录像状态
 * 重连机制：断开后 3 秒自动重连
 */
void CameraStreamingPage::initializeMqtt() // 创建 MQTT 客户端和定时器，并建立连接、消息与错误处理回调。
{
    mqttClient = new QMqttClient(this);      // 创建 MQTT 客户端
    heartbeatTimer = new QTimer(this);       // 心跳定时器
    mqttReconnectTimer = new QTimer(this);   // 重连定时器
    mqttReconnectTimer->setSingleShot(true); // 单次触发（不重复）
    mqttReconnectTimer->setInterval(3000);   // 断线后 3 秒自动重连
    heartbeatTimer->setInterval(10000);      // 每 10 秒发送一次心跳

    // MQTT 开关 CheckBox → 连接/断开
    connect(mqttEnabledCheckBox, &QCheckBox::clicked,                  // 用户点击 MQTT 开关时发出 enabled 布尔值。
            this, &CameraStreamingPage::mqttMonitoringToggled);        // 主线程槽根据 enabled 发起连接或主动断开。
    // 心跳定时器 → 发布设备遥测数据
    connect(heartbeatTimer, &QTimer::timeout,                          // 每到 10 秒定时周期发出 timeout。
            this, &CameraStreamingPage::publishHeartbeat);             // 构造并发布当前设备状态 JSON。
    // 重连定时器 → 尝试重新连接 MQTT 服务器
    connect(mqttReconnectTimer, &QTimer::timeout, this, [this]()       // 单次重连定时器到期后执行此 Lambda。
            {
        if (mqttEnabledCheckBox->isChecked() &&                        // 用户仍希望保持 MQTT 连接，并且
                mqttClient->state() == QMqttClient::Disconnected) {   // 客户端当前确实处于断开状态时才重连。
            mqttStatusLabel->setText(QStringLiteral("MQTT正在重新连接...")); // 在界面上提示本次重连动作。
            mqttClient->connectToHost();                               // 使用客户端中已经保存的主机和端口重新连接。
        } });                                                          // 结束重连条件和 Lambda。

    /*
     * MQTT 连接状态机处理。
     * 三种状态：
     *   Connected    — 订阅 commands 主题，启动心跳，锁定地址输入
     *   Connecting   — 显示连接中提示
     *   Disconnected — 如果用户勾选了连接，启动 3 秒重连定时器；否则显示已断开
     */
    connect(mqttClient, &QMqttClient::stateChanged,                    // MQTT 状态变化时发送新的 ClientState。
            this, [this](QMqttClient::ClientState state)               // state 参数表示已连接、连接中或已断开。
            {
        switch (state) {                                               // 根据客户端当前状态执行对应 UI 和定时器操作。
        case QMqttClient::Connected: {                                 // 已与 Broker 成功建立连接。
            mqttReconnectTimer->stop();                 // 连接成功，停止重连
            mqttStatusLabel->setText(QStringLiteral("MQTT已连接：%1") // 状态文字中包含当前设备 ID。
                                     .arg(deviceId));                   // 用 deviceId 替换字符串中的 %1。
            mqttHostEdit->setEnabled(false);            // 连接后锁定地址/端口
            mqttPortSpinBox->setEnabled(false);                         // 连接期间禁止修改端口。
            // 订阅远程命令主题（QoS 1：至少一次送达）
            auto subscription = mqttClient->subscribe(                 // subscription 保存订阅对象指针，用于判断订阅是否成功。
                        mqttTopic(QStringLiteral("commands")), 1);     // 订阅本设备 commands 主题，QoS 为 1。
            if (!subscription)                                         // 返回空指针说明订阅请求未创建成功。
                qWarning() << "MQTT命令主题订阅失败";                  // 写日志提示远程命令将无法收到。
            heartbeatTimer->start();                    // 启动心跳定时器
            publishHeartbeat();                         // 连接后立即发送首条心跳
            break;                                                      // Connected 分支处理结束。
        }
        case QMqttClient::Connecting:                                   // TCP/MQTT 握手仍在进行。
            mqttStatusLabel->setText(QStringLiteral("MQTT正在连接...")); // 向用户显示中间连接状态。
            break;                                                      // Connecting 分支处理结束。
        case QMqttClient::Disconnected:                                 // 当前没有与 Broker 建立连接。
            if (mqttEnabledCheckBox->isChecked()) {                     // 开关仍选中表示断开不是用户主动要求。
                // 用户期望连接但断开 → 自动重连
                mqttStatusLabel->setText(                               // 向用户说明当前断线和后续动作。
                            QStringLiteral("MQTT已断开，3秒后自动重连")); // 显示固定的 3 秒重连提示。
                mqttReconnectTimer->start();                            // 3 秒后触发一次自动重连。
            } else {                                                    // 开关未选中表示用户主动关闭连接。
                // 用户主动断开 → 不重连
                mqttStatusLabel->setText(QStringLiteral("MQTT已断开")); // 显示稳定的主动断开状态。
            }
            mqttHostEdit->setEnabled(true);             // 断开后恢复地址可编辑
            mqttPortSpinBox->setEnabled(true);                          // 断开后允许用户重新编辑端口。
            heartbeatTimer->stop();                     // 停止心跳
            break;                                                      // Disconnected 分支处理结束。
        } });                                                           // 结束状态 switch 和 Lambda。
    // MQTT 消息接收 → 如果是 commands 主题的消息，调用命令处理函数
    connect(mqttClient, &QMqttClient::messageReceived,                  // 任意已订阅主题收到消息时发出信号。
            this, [this](const QByteArray &message, const QMqttTopicName &topic) // message 是载荷；topic 是消息所属主题。
            {
        if (topic.name() == mqttTopic(QStringLiteral("commands")))     // 只处理属于当前设备的 commands 主题。
            handleMqttCommand(message); });                             // 把原始载荷交给统一远程命令解析函数。
    // MQTT 错误处理 → 显示错误码，等待自动重连
    connect(mqttClient, &QMqttClient::errorChanged,                     // MQTT 客户端错误码变化时触发。
            this, [this](QMqttClient::ClientError error)                // error 参数是 Qt MQTT 定义的连接错误枚举。
            {
        if (error == QMqttClient::NoError)                              // NoError 不是故障，无需更新错误提示。
            return;                                                     // 直接退出错误回调。
        mqttStatusLabel->setText(QStringLiteral("MQTT连接错误(%1)，等待重连") // UI 中显示数值错误码。
                                 .arg(int(error)));                     // 将枚举转换为 int 后填入 %1。
        qWarning() << "MQTT连接错误，错误码：" << int(error)           // 输出数值错误码，便于查 Qt 文档定位。
                   << "服务器：" << mqttClient->hostname()             // 同时输出发生错误的 Broker 主机名。
                   << mqttClient->port(); });                           // 输出 Broker 端口并结束 Lambda。
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
    const QString transport = settings.value(QStringLiteral("edge/streamTransport"), // transport 保存上次选择的协议标识。
                                             QStringLiteral("udp")).toString().toLower(); // 配置不存在时默认 UDP，并统一转为小写。
    const int transportIndex = streamTransportComboBox->findData(transport); // 按内部数据查找协议在下拉框中的索引。
    streamTransportComboBox->setCurrentIndex(transportIndex >= 0 ? transportIndex : 1); // 找不到旧值时回退到索引 1 的 UDP。

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
    settings.setValue(QStringLiteral("edge/streamTransport"),           // 键名：当前推流协议。
                      streamTransportComboBox->currentData().toString()); // 保存选项携带的 "tcp" 或 "udp" 数据。

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
 *   1. 空帧检查
 *   2. 缓存 latestFrame
 *   3. 缩放显示到 QLabel
 */
void CameraStreamingPage::updateLocalPreview(const QImage &image) // image是采集线程经队列连接交给主线程的预览帧。
{
    if (image.isNull()) // 判断跨线程传来的QImage是否包含有效像素数据。
    {
        qWarning() << "收到空的本地预览图像，已跳过显示"; // 保留空帧诊断提示，方便排查内存或采集错误。
        return; // 空帧不能继续转换QPixmap，因此提前结束槽函数。
    }
    latestFrame = image;        // 通过QImage隐式共享缓存最新帧。
    displayPreviewImage(image); // 调用统一显示函数，把图像缩放到预览QLabel。
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
 * ===== resizeEvent() — 窗口大小变化时重新缩放预览画面 =====
 */
void CameraStreamingPage::resizeEvent(QResizeEvent *event) // event 描述新的页面尺寸；本函数重写 QWidget 的尺寸变化处理。
{
    QWidget::resizeEvent(event); // 先交给基类完成 QWidget 自身的布局与尺寸更新。
    // 只有当本地预览开启且有缓存帧时，才重新缩放显示
    if (!latestFrame.isNull() && localPreviewCheckBox->isChecked()) // 有缓存帧且预览开启时才需要重绘。
        displayPreviewImage(latestFrame); // 按新的 QLabel 尺寸重新缩放缓存的最后一帧。
}

/*
 * ===== updateCaptureButtonText() — 采集启停时更新按钮文字和状态 =====
 */
void CameraStreamingPage::updateCaptureButtonText(bool capturing) // capturing 是按钮点击后的采集启用状态。
{
    // 根据采集状态切换按钮文字。
    startCaptureButton->setText(capturing                                    // 三元判断：capturing=true?
                                    ? QStringLiteral("停止采集摄像头数据")   // 正在采集 → 显示"停止"。
                                    : QStringLiteral("开始采集摄像头数据")); // 未采集 → 显示"开始"。
    updateAnalysisFrameDistribution(); // 采集状态变化后重新决定是否向视觉检测器发送分析帧。
    // 如果停止采集但录像还在进行中，强制停止录像（录像需要采集帧作为数据源）。
    if (!capturing && eventRecordingActive) // 正在停止采集且仍有活跃录像？
        emit recordingStopRequested();      // 跨线程通知 TcpFrameSender 停止录像写文件。
    publishHeartbeat();                     // 推流/分析/录像状态变更后立即发送心跳更新状态。
}

void CameraStreamingPage::updateAnalysisFrameDistribution() // 根据采集和检测两个开关统一控制分析帧分发。
{
    cameraCaptureThread->setAnalysisEnabled(                            // 把最终分析开关写入采集线程。
        startCaptureButton->isChecked() &&                              // 必须正在采集，才有图像可供分析；并且
        visionDetectionCheckBox->isChecked());                          // 用户必须已经开启视觉变化检测。
}

/*
 * ===== networkStreamingToggled() — TCP 推流开关 =====
 *
 * 启用时需要校验：IP 地址不能为空。
 * 注意：eventRecordingActive 为 true 时也需要强制开启网络帧分发，
 * 因为录像依赖 TcpFrameSender 接受帧数据。
 */
void CameraStreamingPage::networkStreamingToggled(bool enabled) // enabled 表示用户希望开启还是关闭实时网络推流。
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
                                    enabled,                            // 是否真正向远端发送编码后的实时视频。
                                    streamTransportComboBox->currentData().toString()); // 当前选中的 "tcp" 或 "udp" 协议。
    publishHeartbeat();                                                  // 状态变更后发送心跳。
}

/*
 * ===== applyServerEndpoint() — 应用服务器地址/端口配置变更 =====
 */
void CameraStreamingPage::applyServerEndpoint() // 保存当前端点；推流已开启时立即把新端点下发给发送器。
{
    saveSettings(); // 先将当前 IP/端口配置持久化到 QSettings。
    // 仅在 TCP 推流已开启时，才将新地址立即应用到 TcpFrameSender。
    // 如果推流未开启，只需保存配置即可；下次开启推流时 networkStreamingToggled 会读取最新值。
    if (networkStreamingCheckBox->isChecked()) // 推流 CheckBox 已勾选？
    {
        emit senderConfigurationChanged(serverAddressEdit->text().trimmed(), // 新 IP（去空白）。
                                        quint16(serverPortSpinBox->value()), // 新端口。
                                        true,                           // 保持推流启用状态，只更新连接参数。
                                        streamTransportComboBox->currentData().toString()); // 下发当前传输协议。
    }
}

/*
 * ===== visionDetectionToggled() — 运动检测开关 =====
 *
 * 启用时配置 VisionEventDetector 的像素变化阈值。
 * 同时确保采集线程开启分析帧分发（最高 30 FPS 给视觉线程）。
 */
void CameraStreamingPage::visionDetectionToggled(bool enabled) // enabled 是视觉变化检测复选框的新状态。
{
    updateAnalysisFrameDistribution(); // 同步 CaptureThread 是否需要产生 analysisFrameReady 信号。
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
 * ===== handleAnalysisCompleted() — 视觉检测每帧分析完成回调 =====
 *
 * 每帧分析完成后更新 latestFrame 缓存和画面变化比例显示。
 * 注意：这里不触发告警事件（告警由 visualMotionDetected 信号触发），
 * 仅更新 UI 状态文字。
 */
void CameraStreamingPage::handleAnalysisCompleted(const QImage &image, // image 是检测器处理本帧时使用的图像。
                                                  double changedRatio) // changedRatio 是变化像素占总像素的 0~1 比例。
{
    // 如果本地预览关闭，用分析帧更新 latestFrame 缓存。
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
 * 处理：记录事件到 SQLite + 添加到 UI 列表 + MQTT 推送 + 自动事件录像。
 */
void CameraStreamingPage::handleVisualMotionDetected(double changedRatio) // changedRatio 是触发告警时的变化像素比例。
{
    recordMonitoringEvent(                                 // 统一事件记录入口。
        QStringLiteral("visual_motion"),                   // 事件类型：画面运动。
        MonitoringEventSeverity::Warning,                  // 严重等级：警告（中）。
        QStringLiteral("camera"),                          // 事件来源：摄像头。
        QStringLiteral("检测到画面变化，变化像素比例 %1%") // 事件消息。
            .arg(changedRatio * 100.0, 0, 'f', 1),         // 插入变化百分比（1位小数）。
        true);                                             // true = 触发自动录像。
}

/*
 * ===== createRecordingPath() — 生成录像文件路径 =====
 *
 * H.264 Annex B 裸码流录像保存在 data/recordings 目录下。
 * 注意：这里只生成路径，目录由 TcpFrameSender 在开始录像时创建。
 */
QString CameraStreamingPage::createRecordingPath(const QString &eventType) const // eventType 将写入文件名以标识录像原因。
{
    // 构建 data/recordings 目录路径。
    const QString directoryPath = QDir(QCoreApplication::applicationDirPath())      // 从 exe 所在目录。
                                      .filePath(QStringLiteral("data/recordings")); // 拼接 data/recordings 子目录。
    // 返回完整文件路径：目录/时间戳_事件类型.h264
    // 注意：这里只生成路径字符串，目录由 TcpFrameSender::startRecording 在写入前创建。
    return QDir(directoryPath).filePath(                     // 规范化路径拼接。
        QStringLiteral("%1_%2.h264")                         // H.264 文件名模板。
            .arg(QDateTime::currentDateTime().toString(      // %1 = 当前时间戳。
                     QStringLiteral("yyyyMMdd_HHmmss_zzz")), // 精确到毫秒。
                 eventType));                                // %2 = 事件类型。
}

/*
 * ===== recordMonitoringEvent() — 统一的事件记录入口 =====
 *
 * 这是系统中最核心的事件处理函数，所有告警/通知都经过这里。
 * 执行以下操作（按顺序）：
 *   1. 如果 requestRecording=true 且自动录像已开启 → 启动 10 秒事件录像
 *   2. 写入 SQLite 数据库（eventStore->appendEvent）
 *   3. 添加到 UI 事件列表（addEventToList）
 *   4. 通过 MQTT 推送到 PC 接收端（publishMqttJson）
 */
void CameraStreamingPage::recordMonitoringEvent( // 接收一次监控事件，并统一完成录像、存储、显示和 MQTT 推送。
    const QString &eventType, MonitoringEventSeverity severity, // eventType 是事件分类；severity 是严重等级。
    const QString &source, const QString &message, bool requestRecording) // source 是来源；message 是描述；requestRecording 决定是否请求录像。
{
    // ----- 步骤1：构建事件结构体 -----
    MonitoringEvent event;                                    // 栈上创建事件对象。
    event.deviceId = deviceId;                                // 设备唯一标识（如 "imx6ull-001"）。
    event.eventType = eventType;                              // 事件类型字符串（如 "visual_motion"）。
    event.severity = severity;                                // 严重等级（Info/Warning/Critical）。
    event.source = source;                                    // 来源组件（如 "camera"/"ap3216c"/"icm20608"）。
    event.message = message;                                  // 人类可读事件消息。
    event.createdAt = QDateTime::currentDateTime();           // 记录事件发生时间戳。

    // ----- 步骤2：事件自动录像逻辑 -----
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

    // ----- 步骤3：写入 SQLite 数据库（持久化） -----
    if (eventStoreReady && !eventStore->appendEvent(&event))              // 数据库就绪 但 写入失败？
        qWarning() << "事件未能写入 SQLite：" << eventStore->lastError(); // 打印错误详情。

    // ----- 步骤4：添加到 UI 事件列表 -----
    addEventToList(event); // 在列表顶部插入新事件，超量自动删除旧条目。

    // ----- 步骤5：MQTT 推送（构建 JSON 并发布到 devices/{id}/events 主题） -----
    QJsonObject json;                                                  // 创建空 JSON 对象。
    json.insert(QStringLiteral("id"), double(event.id));               // 事件 ID（SQLite 自增主键）。
    json.insert(QStringLiteral("deviceId"), event.deviceId);           // 设备 ID。
    json.insert(QStringLiteral("eventType"), event.eventType);         // 事件类型字符串。
    json.insert(QStringLiteral("severity"), int(event.severity));      // 严重等级（int 枚举）。
    json.insert(QStringLiteral("source"), event.source);               // 来源组件。
    json.insert(QStringLiteral("message"), event.message);             // 事件描述消息。
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
 * Tooltip 显示：来源 / 录像路径
 */
void CameraStreamingPage::addEventToList(const MonitoringEvent &event) // event 包含待显示事件的时间、等级、来源、消息和录像路径。
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
    item->setToolTip(QStringLiteral("来源: %1\n录像: %2") // Tooltip 模板。
                         .arg(event.source,                  // %1 = 事件来源。
                              event.recordingPath.isEmpty()  // %2 = 录像路径。
                                  ? QStringLiteral("无")    // 无录像 → "无"。
                                  : event.recordingPath));   // 有录像 → 显示路径。

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
void CameraStreamingPage::handleRecordingStateChanged( // 接收发送器的录像启停结果并同步页面状态。
    bool active, const QString &filePath) // active 表示是否正在录像；filePath 是当前或刚完成的录像路径。
{
    eventRecordingActive = active; // 同步页面保存的录像活跃状态。
    if (active)                    // 录像已开始。
    {
        currentRecordingPath = filePath; // 记录当前录像文件路径。
        recordingStatusLabel->setText(QStringLiteral("事件录像中：%1") // 状态标签只展示简短文件名。
                                          .arg(QFileInfo(filePath).fileName())); // QFileInfo 从完整路径提取文件名。
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
                              filePath);                                  // 值：刚保存的 H.264 路径。
            settings.sync();                                              // 立即刷盘（供 OpenCV 页面读取）。
        }
        currentRecordingPath.clear(); // 清空路径缓存。

        // 如果 TCP 推流也没勾选，则停止网络帧分发（录像结束后不再需要分发给 TcpFrameSender）。
        if (!networkStreamingCheckBox->isChecked())                 // 推流 CheckBox 未勾选？
            cameraCaptureThread->setNetworkStreamingEnabled(false); // 关闭采集 → 网络帧分发。
    }
    publishHeartbeat(); // 状态变更后发送心跳。
}

/*
 * ===== handleRecordingError() — 录像错误处理 =====
 */
void CameraStreamingPage::handleRecordingError(const QString &message) // message 是 TcpFrameSender 返回的可读错误说明。
{
    eventRecordingActive = false;             // 错误发生后页面不再认为录像仍处于活动状态。
    currentRecordingPath.clear();             // 清除已失败或无效的录像路径。
    recordingStatusLabel->setText(message);   // 将具体错误展示给用户。
    // 如果 TCP 推流也没勾选，停止网络帧分发（与录像停止逻辑一致）。
    if (!networkStreamingCheckBox->isChecked())                 // 推流未勾选？
        cameraCaptureThread->setNetworkStreamingEnabled(false); // 关闭帧分发。
}

/*
 * ===== mqttMonitoringToggled() — MQTT 连接/断开开关 =====
 */
void CameraStreamingPage::mqttMonitoringToggled(bool enabled) // enabled 表示用户希望连接还是断开 MQTT Broker。
{
    if (!enabled) // 关闭开关时走主动断开分支。
    {
        // 断开连接：停止重连定时器和心跳，断开 MQTT
        mqttReconnectTimer->stop();       // 禁止主动断开之后仍被定时器重新连接。
        mqttClient->disconnectFromHost(); // 请求客户端关闭当前 MQTT 连接。
        return;                           // 主动断开不再执行后面的连接参数配置。
    }
    // 连接前校验：host 不能为空
    const QString host = mqttHostEdit->text().trimmed(); // host 保存去除首尾空白后的 Broker 地址。
    if (host.isEmpty())                                  // 空地址无法建立连接。
    {
        const QSignalBlocker blocker(mqttEnabledCheckBox);             // blocker 在当前作用域内阻止复位动作再次发出控件信号。
        mqttEnabledCheckBox->setChecked(false);                         // 将无效的开启状态恢复为未选中。
        mqttStatusLabel->setText(QStringLiteral("请先填写MQTT服务器")); // 告知用户缺少必要地址。
        return;                                                         // 参数无效，不调用 connectToHost()。
    }
    saveSettings(); // 连接前保存用户刚输入的 MQTT 地址和端口。
    // 配置 MQTT 客户端参数
    mqttClient->setHostname(host);                           // 设置要连接的 Broker 主机名或 IP。
    mqttClient->setPort(quint16(mqttPortSpinBox->value())); // 将 SpinBox 整数端口转为 MQTT 接口需要的 quint16。
    // ClientID 格式：imx6ull-001-edge-terminal（设备ID + 角色后缀）
    mqttClient->setClientId(deviceId + QStringLiteral("-edge-terminal")); // 为 Broker 设置可识别且相对唯一的客户端 ID。
    mqttClient->connectToHost();                                           // 异步开始连接；结果由 stateChanged 信号反馈。
}

/*
 * ===== mqttTopic() — 构建 MQTT 主题路径 =====
 *
 * 格式：devices/{deviceId}/{suffix}
 * 例如：devices/imx6ull-001/status
 */
QString CameraStreamingPage::mqttTopic(const QString &suffix) const // suffix 是 status、events、commands 等主题末级名称。
{
    return QStringLiteral("devices/%1/%2").arg(deviceId, suffix); // 把设备 ID 和后缀填入统一主题模板并返回。
}

/*
 * ===== publishMqttJson() — 发布 JSON 消息到指定 MQTT 主题 =====
 *
 * QoS 1（至少一次送达），不保留（retain=false）。
 * 仅在 MQTT 已连接时发布，断开时直接跳过。
 */
void CameraStreamingPage::publishMqttJson(const QString &topic,       // topic 是消息需要发布到的完整 MQTT 主题。
                                          const QJsonObject &object)  // object 是需要序列化并发送的 JSON 对象。
{
    if (!mqttClient || mqttClient->state() != QMqttClient::Connected) // 客户端不存在或尚未连接时不能发布。
        return;                                                        // 直接丢弃本次状态消息，不阻塞页面。
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact); // payload 是无缩进的紧凑 UTF-8 JSON 字节串。
    if (mqttClient->publish(QMqttTopicName(topic), payload, 1, false) < 0) // QoS=1、retain=false；负返回值表示发布失败。
        qWarning() << "MQTT发布失败：" << topic << payload;              // 输出主题和载荷，便于定位问题。
}

/*
 * ===== publishHeartbeat() — 发送设备心跳（遥测数据） =====
 *
 * 每 10 秒定时发送，也在状态变更时主动发送。
 * 上报采集、推流、画面变化检测、录像状态和时间戳。
 *
 * PC 接收端通过订阅 devices/{id}/status 主题获取设备实时状态。
 */
void CameraStreamingPage::publishHeartbeat() // 采集当前页面状态，组成 JSON 并发布到本设备 status 主题。
{
    QJsonObject status; // status 保存本次心跳的所有键值字段。
    status.insert(QStringLiteral("deviceId"), deviceId); // 标明是哪台设备发送的心跳。
    status.insert(QStringLiteral("online"), true); // 能发出本消息即表示程序在线。
    status.insert(QStringLiteral("captureEnabled"), startCaptureButton->isChecked()); // 上报摄像头采集开关状态。
    status.insert(QStringLiteral("streamingEnabled"), networkStreamingCheckBox->isChecked()); // 上报实时推流开关状态。
    status.insert(QStringLiteral("streamTransport"),                  // 上报当前推流协议字段。
                  streamTransportComboBox->currentData().toString());  // 取选项内部保存的 "tcp" 或 "udp"。
    status.insert(QStringLiteral("visionEnabled"), visionDetectionCheckBox->isChecked()); // 上报视觉检测开关状态。
    status.insert(QStringLiteral("recording"), eventRecordingActive); // 上报事件录像是否正在进行。
    status.insert(QStringLiteral("timestamp"),                        // 添加本次状态采集的时间戳。
                  QDateTime::currentDateTime().toString(Qt::ISODateWithMs)); // 使用带毫秒的 ISO 8601 字符串。
    publishMqttJson(mqttTopic(QStringLiteral("status")), status);      // 通过统一发布函数发送心跳对象。
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
 *   set_stream_endpoint          — 修改 TCP 推流目标地址
 *
 * 每个命令执行后都会回复 acknowledgement 到 command_ack 主题。
 */
void CameraStreamingPage::handleMqttCommand(const QByteArray &payload) // payload 是 commands 主题收到的原始 MQTT 消息体。
{
    QString command;               // command 保存规范化后的命令名称。
    QJsonObject request;           // request 保存 JSON 命令对象及其附带参数。
    QJsonParseError parseError;    // parseError 接收 JSON 解析结果和错误位置。
    // 尝试解析为 JSON
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError); // document 尝试把原始字节解析为 JSON 文档。
    if (parseError.error == QJsonParseError::NoError && document.isObject()) // 只有无解析错误且根节点是对象才按 JSON 命令处理。
    {
        request = document.object(); // 保存整个对象，后续 set_stream_endpoint 还要读取其他字段。
        command = request.value(QStringLiteral("command")).toString().trimmed(); // 读取 command 字段并去掉首尾空白。
    }
    else // JSON 解析失败或根节点不是对象时，尝试兼容旧版纯文本命令。
    {
        // 解析失败则视为纯文本命令（向后兼容）
        command = QString::fromUtf8(payload).trimmed(); // 将非 JSON 载荷直接按 UTF-8 命令名处理。
    }

    bool accepted = true;                          // accepted 表示命令是否合法并被执行，默认先按成功处理。
    QString result = QStringLiteral("命令已执行"); // result 保存要回复给 PC 端的执行说明。

    /*
     * 命令分发：通过模拟按钮点击来触发已有的业务逻辑，
     * 避免重复实现相同的功能代码。
     */
    // ===== start_capture：远程启动摄像头采集 =====
    if (command == QLatin1String("start_capture")) // 匹配启动采集命令。
    {
        if (!startCaptureButton->isChecked()) // 仅当尚未启动时执行
            startCaptureButton->click();      // 模拟点击按钮 → 触发 setCaptureEnabled(true)
    }
    // ===== stop_capture：远程停止摄像头采集 =====
    else if (command == QLatin1String("stop_capture")) // 匹配停止采集命令。
    {
        if (startCaptureButton->isChecked()) // 仅当正在采集时执行
            startCaptureButton->click();     // 模拟点击按钮 → 触发 setCaptureEnabled(false)
    }
    // ===== stream_on：远程开启 TCP 实时推流 =====
    else if (command == QLatin1String("stream_on")) // 匹配开启实时推流命令。
    {
        if (!networkStreamingCheckBox->isChecked()) // 仅当推流未开启时执行
        {
            networkStreamingCheckBox->setChecked(true); // 更新 UI 复选框为选中状态
            networkStreamingToggled(true);              // 调用槽函数校验配置并启动推流
        }
    }
    // ===== stream_off：远程停止 TCP 实时推流 =====
    else if (command == QLatin1String("stream_off")) // 匹配关闭实时推流命令。
    {
        if (networkStreamingCheckBox->isChecked()) // 仅当推流已开启时执行
        {
            networkStreamingCheckBox->setChecked(false); // 更新 UI 复选框为未选中状态
            networkStreamingToggled(false);              // 调用槽函数停止推流
        }
    }
    // ===== vision_on：远程开启视觉运动检测 =====
    else if (command == QLatin1String("vision_on")) // 匹配开启视觉变化检测命令。
    {
        if (!visionDetectionCheckBox->isChecked()) // 仅当运动检测未开启时执行
        {
            visionDetectionCheckBox->setChecked(true); // 更新 UI 复选框为选中状态
            visionDetectionToggled(true);              // 调用槽函数配置并启动运动检测
        }
    }
    // ===== vision_off：远程停止视觉运动检测 =====
    else if (command == QLatin1String("vision_off")) // 匹配关闭视觉变化检测命令。
    {
        if (visionDetectionCheckBox->isChecked()) // 仅当运动检测已开启时执行
        {
            visionDetectionCheckBox->setChecked(false); // 更新 UI 复选框为未选中状态
            visionDetectionToggled(false);              // 调用槽函数停止运动检测
        }
    }
    // ===== set_stream_endpoint：远程修改 TCP 推流目标地址 =====
    else if (command == QLatin1String("set_stream_endpoint")) // 匹配更新推流目标和协议的命令。
    {
        const QString host = request.value(QStringLiteral("host")).toString().trimmed(); // 从 JSON 提取目标 IP
        const int port = request.value(QStringLiteral("port")).toInt();                  // 从 JSON 提取目标端口
        const QString transport = request.value(QStringLiteral("transport")) // transport 保存命令指定的传输协议。
                                      .toString(streamTransportComboBox->currentData().toString()) // 缺少字段时沿用当前协议。
                                      .toLower();                              // 转小写后再进行 tcp/udp 合法性判断。
        if (host.isEmpty() || port <= 0 || port > 65535 ||             // 地址为空或端口超出有效范围时拒绝；或者
            (transport != QLatin1String("tcp") && transport != QLatin1String("udp"))) // 协议既不是 TCP 也不是 UDP 时拒绝。
        {
            accepted = false;                                    // 标记命令未接受
            result = QStringLiteral("视频服务端地址或端口无效"); // 设置错误原因
        }
        else // 三项端点参数均合法，可以写入 UI 并应用。
        {
            serverAddressEdit->setText(host);  // 更新 UI 中的 IP 输入框
            serverPortSpinBox->setValue(port); // 更新 UI 中的端口选择器
            streamTransportComboBox->setCurrentIndex(                   // 将远程指定的协议同步到页面下拉框。
                streamTransportComboBox->findData(transport));          // 按内部协议字符串找到对应选项索引。
            applyServerEndpoint();             // 应用新配置并同步到 TcpFrameSender
        }
    }
    // ===== 未知命令处理 =====
    else // 没有命中任何已支持的远程命令。
    {
        accepted = false;                                     // 标记命令未被识别
        result = QStringLiteral("未知命令：%1").arg(command); // 将命令名嵌入错误消息
    }

    qInfo() << "收到MQTT远程命令：" << command << "结果：" << result; // 记录命令名和最终处理结果。

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
    publishHeartbeat(); // 立即上报执行命令后的最新设备状态。
}
