// 包含本类的头文件声明
#include "receiver_window.h" // 包含本类的头文件声明
// 引入事件录像归档浏览器控件
#include "archive_browser.h" // 引入事件录像归档浏览器控件
// 引入 RTP 分包协议定义（帧重组、分包解析）
#include "rtp_packet.h" // 引入 RTP 分包协议定义（帧重组、分包解析）

// 引入 Qt MQTT 客户端类，用于设备远程控制和状态订阅
#include <mqtt/qmqttclient.h> // 引入 Qt MQTT 客户端类，用于设备远程控制和状态订阅

// Qt 颜色类，用于事件列表项的前景色设置
#include <QColor> // Qt 颜色类，用于事件列表项的前景色设置
// Qt 日期时间类，用于获取当前时间戳
#include <QDateTime> // Qt 日期时间类，用于获取当前时间戳
// Qt 网格布局类，用于命令按钮的网格排列
#include <QGridLayout> // Qt 网格布局类，用于命令按钮的网格排列
// Qt 水平布局类，用于水平排列控件
#include <QHBoxLayout> // Qt 水平布局类，用于水平排列控件
// Qt JSON 文档类，用于解析 MQTT 消息中的 JSON 数据
#include <QJsonDocument> // Qt JSON 文档类，用于解析 MQTT 消息中的 JSON 数据
// Qt JSON 解析错误类，用于检测 JSON 解析是否成功
#include <QJsonParseError> // Qt JSON 解析错误类，用于检测 JSON 解析是否成功
// Qt 标签控件，用于显示文本信息
#include <QLabel> // Qt 标签控件，用于显示文本信息
// Qt 单行文本编辑框，用于输入 MQTT 主机地址和设备 ID
#include <QLineEdit> // Qt 单行文本编辑框，用于输入 MQTT 主机地址和设备 ID
// Qt 列表控件，用于显示设备事件和命令回执
#include <QListWidget> // Qt 列表控件，用于显示设备事件和命令回执
// Qt 列表项控件，列表中的每一项
#include <QListWidgetItem> // Qt 列表项控件，列表中的每一项
// Qt 像素图类，用于在标签上显示图像帧
#include <QPixmap> // Qt 像素图类，用于在标签上显示图像帧
// Qt 按钮控件，用于触发各种操作
#include <QPushButton> // Qt 按钮控件，用于触发各种操作
// Qt 窗口大小调整事件类，响应窗口缩放
#include <QResizeEvent> // Qt 窗口大小调整事件类，响应窗口缩放
// Qt 数值输入框，用于输入端口号
#include <QSpinBox> // Qt 数值输入框，用于输入端口号
// Qt 选项卡控件，用于切换"实时监控"和"事件录像"页面
#include <QTabWidget> // Qt 选项卡控件，用于切换"实时监控"和"事件录像"页面
// Qt TCP 服务器类，用于监听 TCP 推流连接
#include <QTcpServer> // Qt TCP 服务器类，用于监听 TCP 推流连接
// Qt TCP 套接字类，用于与推流客户端通信
#include <QTcpSocket> // Qt TCP 套接字类，用于与推流客户端通信
// Qt 定时器类，用于统计更新和不完整帧过期检查
#include <QTimer> // Qt 定时器类，用于统计更新和不完整帧过期检查
// Qt UDP 套接字类，用于接收 RTP/UDP 数据报
#include <QUdpSocket> // Qt UDP 套接字类，用于接收 RTP/UDP 数据报
// Qt 垂直布局类，用于垂直排列控件
#include <QVBoxLayout> // Qt 垂直布局类，用于垂直排列控件
// Qt 数据流类，用于从 TCP 字节流中解析帧头
#include <QDataStream> // Qt 数据流类，用于从 TCP 字节流中解析帧头

// 匿名命名空间：这些常量仅在当前 .cpp 文件内可见
namespace // 匿名命名空间：这些常量仅在当前 .cpp 文件内可见
{
    // 最大合理的时钟延迟阈值（毫秒）：超过此值认为两端时钟未同步
    constexpr qint64 MaximumSensibleClockLatencyMs = 10000; // 最大合理的时钟延迟阈值（毫秒）：超过此值认为两端时钟未同步
    // TCP 帧魔术字："IMXV"，与发送端一致，用于帧边界识别
    constexpr quint32 TcpFrameMagic = 0x494d5856; // "IMXV"
    // TCP 帧协议版本号（必须与发送端一致）
    constexpr quint16 TcpFrameVersion = 2; // TCP 帧协议版本号（必须与发送端一致）
    // TCP 帧头大小：28 字节
    constexpr quint16 TcpFrameHeaderSize = 28; // TCP 帧头大小：28 字节
    // TCP 最大载荷大小：10 MB，超过此值的载荷视为无效（防止恶意数据）
    constexpr quint32 MaximumTcpPayloadSize = 10 * 1024 * 1024; // TCP 最大载荷大小：10 MB，超过此值的载荷视为无效（防止恶意数据）
    // TCP 帧魔术字字节数组，用于快速前缀匹配（QByteArray::startsWith）
    const QByteArray TcpFrameMagicBytes("IMXV", 4); // TCP 帧魔术字字节数组，用于快速前缀匹配（QByteArray::startsWith）
}
// 构造函数：创建 PC 端监控窗口的所有 UI 控件和网络资源
// @param parent 父窗口指针
ReceiverWindow::ReceiverWindow(QWidget *parent) // 定义上方说明的成员函数；参数含义见函数前注释。
    : QWidget(parent),                                                                        // 调用基类构造函数
      videoSocket(new QUdpSocket(this)),                                                      // 创建 UDP 套接字，用于接收 RTP/UDP 视频流
      tcpServer(new QTcpServer(this)),                                                        // 创建 TCP 服务器，用于监听 TCP 推流连接
      tcpClient(nullptr),                                                                     // TCP 客户端初始为 nullptr（连接到来时创建）
      reassemblyTimer(new QTimer(this)),                                                      // 重组定时器，用于周期性清理不完整帧
      portSpinBox(new QSpinBox(this)),                                                        // 监听端口选择框
      listenButton(new QPushButton(QStringLiteral("开始监听"), this)),                        // 监听/停止切换按钮
      statusLabel(new QLabel(QStringLiteral("尚未监听"), this)),                              // 状态信息显示标签
      statisticsLabel(new QLabel(QStringLiteral("FPS: 0.0  带宽: 0.00 Mbps"), this)),         // 统计信息标签
      frameLabel(new QLabel(QStringLiteral("等待 H.264/TCP 或 H.264/RTP/UDP 视频流"), this)), // 视频显示标签
      archiveBrowser(new ArchiveBrowser(this)),                                               // 创建事件录像归档浏览器
      mqttClient(new QMqttClient(this)),                                                      // 创建 MQTT 客户端
      mqttHostEdit(new QLineEdit(QStringLiteral("192.168.10.100"), this)),                    // MQTT 服务器地址输入框（默认 IP）
      mqttPortSpinBox(new QSpinBox(this)),                                                    // MQTT 端口选择框
      deviceIdEdit(new QLineEdit(QStringLiteral("imx6ull-001"), this)),                       // 设备 ID 输入框
      mqttConnectButton(new QPushButton(QStringLiteral("连接MQTT"), this)),                   // MQTT 连接/断开按钮
      mqttStatusLabel(new QLabel(QStringLiteral("MQTT已断开"), this)),                        // MQTT 连接状态标签
      deviceStatusLabel(new QLabel(QStringLiteral("尚未收到设备心跳"), this)),                // 设备状态显示标签
      eventList(new QListWidget(this)),                                                       // 设备事件/命令回执列表
      reassembler(80),                                                                        // 帧重组器：超时时间 80ms
      intervalBytes(0),                                                                       // 统计周期内的字节数清零
      intervalFrames(0),                                                                      // 统计周期内的帧数清零
      lastSequence(0),                                                                        // 最后接收的帧序列号清零
      activeStreamId(0),                                                                      // 当前活跃的流 ID 清零
      hasActiveStream(false),                                                                 // 无活跃流
      waitingForKeyFrame(true),                                                               // 等待关键帧（IDR 帧）
      activeTcpTransport(false)                                                               // 默认非 TCP 传输模式
{
    // 设置窗口标题
    setWindowTitle(QStringLiteral("i.MX6ULL TCP/UDP 边缘视觉监控平台")); // 设置窗口标题
    // 设置窗口初始大小为 1100x720 像素
    resize(1100, 720); // 设置窗口初始大小为 1100x720 像素
    // 设置对象名称，用于 QSS 样式选择器
    setObjectName(QStringLiteral("pcMonitorWindow")); // 设置对象名称，用于 QSS 样式选择器
    // 设置全局 QSS 样式表：深色主题、圆角按钮、输入框样式等
    setStyleSheet(QStringLiteral( // 设置全局 QSS 样式表：深色主题、圆角按钮、输入框样式等
        "#pcMonitorWindow { background:#171c24; color:#eef2f7; }" // 执行该行对应的具体操作。
        "QLabel { color:#e8edf3; }" // 执行该行对应的具体操作。
        "QLineEdit, QSpinBox { min-height:28px; background:#f8fafc; color:#17202a; border:1px solid #8793a3; border-radius:4px; }" // 执行该行对应的具体操作。
        "QPushButton { min-height:32px; padding:2px 10px; background:#e8edf2; color:#17202a; border:none; border-radius:5px; }" // 执行该行对应的具体操作。
        "QPushButton:hover { background:#ffffff; }" // 执行该行对应的具体操作。
        "QListWidget { background:#10151c; color:#e8edf3; border:1px solid #354150; border-radius:6px; }")); // 执行该行对应的具体操作。

    // 设置视频监听端口范围 1~65535
    portSpinBox->setRange(1, 65535); // 设置视频监听端口范围 1~65535
    // 默认监听端口 8888
    portSpinBox->setValue(8888); // 默认监听端口 8888
    // 设置 MQTT 端口范围 1~65535
    mqttPortSpinBox->setRange(1, 65535); // 设置 MQTT 端口范围 1~65535
    // 默认 MQTT 端口 1883（标准 MQTT 端口）
    mqttPortSpinBox->setValue(1883); // 默认 MQTT 端口 1883（标准 MQTT 端口）
    // 设置状态标签最小宽度 260px，保证文本不换行
    statusLabel->setMinimumWidth(260); // 设置状态标签最小宽度 260px，保证文本不换行
    // 视频帧标签居中对齐
    frameLabel->setAlignment(Qt::AlignCenter); // 视频帧标签居中对齐
    // 视频帧标签最小尺寸 480x360
    frameLabel->setMinimumSize(480, 360); // 视频帧标签最小尺寸 480x360
    // 视频帧标签样式：黑底白字
    frameLabel->setStyleSheet(QStringLiteral("background:black;color:white;")); // 视频帧标签样式：黑底白字
    // 设备状态标签启用自动换行
    deviceStatusLabel->setWordWrap(true); // 设备状态标签启用自动换行
    // 事件列表最小宽度 300px
    eventList->setMinimumWidth(300); // 事件列表最小宽度 300px

    // === 视频监听控制行 ===
    // 创建水平布局：监听端口标签 + 端口输入框 + 监听按钮 + 状态标签（拉伸） + 统计标签
    QHBoxLayout *videoControls = new QHBoxLayout; // 定义并初始化本行使用的变量。
    videoControls->addWidget(new QLabel(QStringLiteral("TCP/UDP监听端口："), this)); // 执行该行对应的具体操作。
    videoControls->addWidget(portSpinBox); // 执行该行对应的具体操作。
    videoControls->addWidget(listenButton); // 执行该行对应的具体操作。
    videoControls->addWidget(statusLabel, 1); // stretch factor=1，占用剩余空间
    videoControls->addWidget(statisticsLabel); // 执行该行对应的具体操作。

    // === MQTT 控制行 ===
    // 创建水平布局：MQTT服务器标签 + 地址输入框 + 端口标签 + 端口输入框 + 设备ID标签 + 设备ID输入框 + 连接按钮 + 状态标签
    QHBoxLayout *mqttControls = new QHBoxLayout; // 定义并初始化本行使用的变量。
    mqttControls->addWidget(new QLabel(QStringLiteral("MQTT服务器："), this)); // 执行该行对应的具体操作。
    mqttControls->addWidget(mqttHostEdit); // 执行该行对应的具体操作。
    mqttControls->addWidget(new QLabel(QStringLiteral("端口："), this)); // 执行该行对应的具体操作。
    mqttControls->addWidget(mqttPortSpinBox); // 执行该行对应的具体操作。
    mqttControls->addWidget(new QLabel(QStringLiteral("设备ID："), this)); // 执行该行对应的具体操作。
    mqttControls->addWidget(deviceIdEdit); // 执行该行对应的具体操作。
    mqttControls->addWidget(mqttConnectButton); // 执行该行对应的具体操作。
    mqttControls->addWidget(mqttStatusLabel, 1); // 执行该行对应的具体操作。

    // === 远程命令按钮网格 ===
    // 创建网格布局，4 列排列命令按钮
    QGridLayout *commandLayout = new QGridLayout; // 创建网格布局，4 列排列命令按钮
    // 定义命令按钮列表：标题 → MQTT 命令字符串
    const struct // 定义命令按钮列表：标题 → MQTT 命令字符串
    {
        const char *title; // 定义并初始化本行使用的变量。
        const char *command; // 定义并初始化本行使用的变量。
    } commands[] = { // 执行该行对应的具体操作。
        {"开始采集", "start_capture"}, {"停止采集", "stop_capture"}, // 执行该行对应的具体操作。
        {"开启推流", "stream_on"}, {"停止推流", "stream_off"}, // 执行该行对应的具体操作。
        {"开启视觉检测", "vision_on"}, {"停止视觉检测", "vision_off"}}; // 执行该行对应的具体操作。
    // 遍历所有命令，逐个创建按钮并放入网格
    for (int i = 0; i < int(sizeof(commands) / sizeof(commands[0])); ++i) // 遍历所有命令，逐个创建按钮并放入网格
    {
        // 创建按钮，文本为中文标题
        QPushButton *button = new QPushButton(QString::fromUtf8(commands[i].title), this); // 创建按钮，文本为中文标题
        // 获取对应的 MQTT 命令字符串
        const QString command = QString::fromLatin1(commands[i].command); // 获取对应的 MQTT 命令字符串
        // 按钮点击 → 发布对应的 MQTT 命令
        connect(button, &QPushButton::clicked, this, // 按钮点击 → 发布对应的 MQTT 命令
                [this, command] // 执行该行对应的具体操作。
                { publishCommand(command); }); // 执行该行对应的具体操作。
        // 放入网格：行 = i/4，列 = i%4（每行 4 个按钮）
        commandLayout->addWidget(button, i / 4, i % 4); // 放入网格：行 = i/4，列 = i%4（每行 4 个按钮）
    }

    // === 视频面板（左侧/主区域）===
    // 视频面板容器
    QWidget *videoPanel = new QWidget(this); // 视频面板容器
    // 垂直布局，放视频帧和设备状态
    QVBoxLayout *videoLayout = new QVBoxLayout(videoPanel); // 垂直布局，放视频帧和设备状态
    // 去除外边距
    videoLayout->setContentsMargins(0, 0, 0, 0); // 去除外边距
    // 添加视频帧标签，stretch=1 占用主要空间
    videoLayout->addWidget(frameLabel, 1); // 添加视频帧标签，stretch=1 占用主要空间
    // 添加设备状态标签
    videoLayout->addWidget(deviceStatusLabel); // 添加设备状态标签

    // === 事件面板（右侧）===
    // 事件面板容器
    QWidget *eventPanel = new QWidget(this); // 事件面板容器
    // 垂直布局
    QVBoxLayout *eventLayout = new QVBoxLayout(eventPanel); // 垂直布局
    // 去除外边距
    eventLayout->setContentsMargins(0, 0, 0, 0); // 去除外边距
    // 添加标题标签
    eventLayout->addWidget(new QLabel(QStringLiteral("设备事件/命令回执"), this)); // 添加标题标签
    // 添加事件列表，stretch=1 占用主要空间
    eventLayout->addWidget(eventList, 1); // 添加事件列表，stretch=1 占用主要空间

    // === 视频 + 事件水平布局 ===
    // 创建水平布局：视频面板占 3 份宽度，事件面板占 2 份宽度
    QHBoxLayout *contentLayout = new QHBoxLayout; // 创建水平布局：视频面板占 3 份宽度，事件面板占 2 份宽度
    contentLayout->addWidget(videoPanel, 3); // 执行该行对应的具体操作。
    contentLayout->addWidget(eventPanel, 2); // 执行该行对应的具体操作。

    // === 实时监控选项卡 ===
    // 创建"实时监控"页面
    QWidget *liveTab = new QWidget(this); // 创建"实时监控"页面
    liveTab->setLayout(contentLayout); // 执行该行对应的具体操作。

    // === 选项卡控件 ===
    // 创建 QTabWidget，包含"实时监控"和"事件录像"两个页面
    QTabWidget *tabs = new QTabWidget(this); // 创建 QTabWidget，包含"实时监控"和"事件录像"两个页面
    tabs->addTab(liveTab, QStringLiteral("实时监控")); // 执行该行对应的具体操作。
    tabs->addTab(archiveBrowser, QStringLiteral("事件录像")); // 执行该行对应的具体操作。
    // 切换到"事件录像"页面时自动刷新归档列表
    connect(tabs, &QTabWidget::currentChanged, this, [this, tabs](int index) // 切换到"事件录像"页面时自动刷新归档列表
            {
        if (tabs->widget(index) == archiveBrowser) archiveBrowser->refreshArchive(); }); // 判断本行条件是否成立。

    // === 主布局 ===
    // 创建窗口主垂直布局
    QVBoxLayout *layout = new QVBoxLayout(this); // 创建窗口主垂直布局
    // 依次添加：视频控制栏、MQTT 控制栏、命令按钮网格、选项卡（stretch=1 占满剩余空间）
    layout->addLayout(videoControls); // 执行该行对应的具体操作。
    layout->addLayout(mqttControls); // 执行该行对应的具体操作。
    layout->addLayout(commandLayout); // 执行该行对应的具体操作。
    layout->addWidget(tabs, 1); // 执行该行对应的具体操作。

    // === 信号与槽连接 ===
    // 监听按钮点击 → 切换监听状态
    connect(listenButton, &QPushButton::clicked, this, &ReceiverWindow::toggleListening); // 监听按钮点击 → 切换监听状态
    // UDP 数据报就绪 → 读取并处理数据报
    connect(videoSocket, &QUdpSocket::readyRead, this, &ReceiverWindow::readPendingDatagrams); // UDP 数据报就绪 → 读取并处理数据报
    // TCP 新连接到来 → 接受 TCP 连接
    connect(tcpServer, &QTcpServer::newConnection, this, // TCP 新连接到来 → 接受 TCP 连接
            &ReceiverWindow::acceptTcpConnection); // 定义上方说明的成员函数；参数含义见函数前注释。
    // 重组定时器每 20ms 触发一次 → 清理超时的不完整帧
    reassemblyTimer->setInterval(20); // 重组定时器每 20ms 触发一次 → 清理超时的不完整帧
    connect(reassemblyTimer, &QTimer::timeout, this, &ReceiverWindow::expireIncompleteFrames); // 定义上方说明的成员函数；参数含义见函数前注释。
    // MQTT 连接按钮点击 → 连接/断开 MQTT
    connect(mqttConnectButton, &QPushButton::clicked, this, &ReceiverWindow::toggleMqttConnection); // MQTT 连接按钮点击 → 连接/断开 MQTT
    // MQTT 连接状态变更时更新 UI
    connect(mqttClient, &QMqttClient::stateChanged, this, // MQTT 连接状态变更时更新 UI
            [this](QMqttClient::ClientState state) // 执行该行对应的具体操作。
            {
                if (state == QMqttClient::Connected) // 判断本行条件是否成立。
                {
                    // 已连接：更新状态标签、按钮文本，禁用输入框
                    mqttStatusLabel->setText(QStringLiteral("MQTT已连接")); // 已连接：更新状态标签、按钮文本，禁用输入框
                    mqttConnectButton->setText(QStringLiteral("断开MQTT")); // 执行该行对应的具体操作。
                    mqttHostEdit->setEnabled(false); // 执行该行对应的具体操作。
                    mqttPortSpinBox->setEnabled(false); // 执行该行对应的具体操作。
                    deviceIdEdit->setEnabled(false); // 执行该行对应的具体操作。
                    subscribeDeviceTopics(); // 执行该行对应的具体操作。
                }
                else if (state == QMqttClient::Connecting) // 进入上一条件不成立时的处理分支。
                {
                    // 正在连接
                    mqttStatusLabel->setText(QStringLiteral("MQTT正在连接...")); // 正在连接
                }
                else // 进入上一条件不成立时的处理分支。
                {
                    // 已断开：恢复输入框和按钮状态
                    mqttStatusLabel->setText(QStringLiteral("MQTT已断开")); // 已断开：恢复输入框和按钮状态
                    mqttConnectButton->setText(QStringLiteral("连接MQTT")); // 执行该行对应的具体操作。
                    mqttHostEdit->setEnabled(true); // 执行该行对应的具体操作。
                    mqttPortSpinBox->setEnabled(true); // 执行该行对应的具体操作。
                    deviceIdEdit->setEnabled(true); // 执行该行对应的具体操作。
                }
            }); // 执行该行对应的具体操作。
    // MQTT 消息接收 → 处理设备状态/事件消息
    connect(mqttClient, &QMqttClient::messageReceived, this, // MQTT 消息接收 → 处理设备状态/事件消息
            [this](const QByteArray &message, const QMqttTopicName &topic) // 执行该行对应的具体操作。
            {
                handleMqttMessage(message, topic.name()); // 执行该行对应的具体操作。
            }); // 执行该行对应的具体操作。

    // 初始化 H.264 解码器，失败则显示错误信息
    QString decoderError; // 初始化 H.264 解码器，失败则显示错误信息
    if (!decoder.initialize(&decoderError)) // 判断本行条件是否成立。
        statusLabel->setText(decoderError); // 执行该行对应的具体操作。
    // 自动启动监听
    toggleListening(); // 自动启动监听
}

// 切换监听状态：启动或停止 TCP/UDP 监听
void ReceiverWindow::toggleListening() // 切换监听状态：启动或停止 TCP/UDP 监听
{
    // 如果当前已在监听（UDP 已绑定或 TCP 服务器在监听），则执行停止操作
    if (videoSocket->state() == QAbstractSocket::BoundState || tcpServer->isListening()) // 如果当前已在监听（UDP 已绑定或 TCP 服务器在监听），则执行停止操作
    {
        // 关闭 UDP 套接字
        videoSocket->close(); // 关闭 UDP 套接字
        // 关闭 TCP 服务器
        tcpServer->close(); // 关闭 TCP 服务器
        // 断开当前 TCP 客户端连接（如果有）
        if (tcpClient) // 断开当前 TCP 客户端连接（如果有）
            tcpClient->disconnectFromHost(); // 执行该行对应的具体操作。
        // 重置 TCP 会话状态（清空缓冲区、释放客户端指针）
        resetTcpSession(); // 重置 TCP 会话状态（清空缓冲区、释放客户端指针）
        // 停止帧重组定时器
        reassemblyTimer->stop(); // 停止帧重组定时器
        // 恢复端口输入框的可编辑状态
        portSpinBox->setEnabled(true); // 恢复端口输入框的可编辑状态
        // 按钮文本改为"开始监听"
        listenButton->setText(QStringLiteral("开始监听")); // 按钮文本改为"开始监听"
        // 更新状态信息
        statusLabel->setText(QStringLiteral("TCP/UDP监听已停止")); // 更新状态信息
        // 重置视频会话（解码器和 RTP 重组器状态）
        resetVideoSession(); // 重置视频会话（解码器和 RTP 重组器状态）
        return; // 返回本行给出的结果并结束当前函数。
    }
    // === 启动监听 ===
    // 获取端口号（从输入框读取）
    const quint16 port = quint16(portSpinBox->value()); // 获取端口号（从输入框读取）
    // 绑定 UDP 套接字到所有 IPv4 地址的指定端口，使用共享地址和端口复用选项
    if (!videoSocket->bind(QHostAddress::AnyIPv4, port, // 绑定 UDP 套接字到所有 IPv4 地址的指定端口，使用共享地址和端口复用选项
                           QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) // 定义并初始化本行使用的变量。
    {
        // UDP 绑定失败：显示错误
        statusLabel->setText(QStringLiteral("UDP监听失败：%1").arg(videoSocket->errorString())); // UDP 绑定失败：显示错误
        return; // 返回本行给出的结果并结束当前函数。
    }
    // 启动 TCP 服务器监听
    if (!tcpServer->listen(QHostAddress::AnyIPv4, port)) // 启动 TCP 服务器监听
    {
        // TCP 监听失败：保存错误信息，关闭 UDP，显示错误
        const QString error = tcpServer->errorString(); // TCP 监听失败：保存错误信息，关闭 UDP，显示错误
        videoSocket->close(); // 执行该行对应的具体操作。
        statusLabel->setText(QStringLiteral("TCP监听失败：%1").arg(error)); // 执行该行对应的具体操作。
        return; // 返回本行给出的结果并结束当前函数。
    }
    // 监听成功：禁用端口输入框（监听中不允许换端口）
    portSpinBox->setEnabled(false); // 监听成功：禁用端口输入框（监听中不允许换端口）
    // 按钮文本改为"停止监听"
    listenButton->setText(QStringLiteral("停止监听")); // 按钮文本改为"停止监听"
    // 显示监听状态
    statusLabel->setText(QStringLiteral("正在监听 H.264/TCP + H.264/RTP/UDP 0.0.0.0:%1") // 显示监听状态
                             .arg(port)); // 执行该行对应的具体操作。
    // 重启统计计时器
    statisticsTimer.restart(); // 重启统计计时器
    // 启动帧重组定时器（20ms 周期）
    reassemblyTimer->start(); // 启动帧重组定时器（20ms 周期）
}

// 重置 TCP 会话状态：清空接收缓冲区和释放客户端
void ReceiverWindow::resetTcpSession() // 重置 TCP 会话状态：清空接收缓冲区和释放客户端
{
    // 清空 TCP 接收缓冲区（可能残留不完整帧数据）
    tcpReceiveBuffer.clear(); // 清空 TCP 接收缓冲区（可能残留不完整帧数据）
    // 如果存在 TCP 客户端，使用 deleteLater 安全释放
    if (tcpClient) // 如果存在 TCP 客户端，使用 deleteLater 安全释放
    {
        tcpClient->deleteLater(); // 执行该行对应的具体操作。
        tcpClient = nullptr; // 执行该行对应的具体操作。
    }
}

// 接受 TCP 客户端连接（TcpServer::newConnection 信号处理）
void ReceiverWindow::acceptTcpConnection() // 接受 TCP 客户端连接（TcpServer::newConnection 信号处理）
{
    // 循环处理所有待处理的连接（可能同时有多个连接请求）
    while (tcpServer->hasPendingConnections()) // 循环处理所有待处理的连接（可能同时有多个连接请求）
    {
        // 获取下一个待处理的连接（返回 QTcpSocket 指针）
        QTcpSocket *incoming = tcpServer->nextPendingConnection(); // 获取下一个待处理的连接（返回 QTcpSocket 指针）
        // 如果已有旧客户端，断开并释放它（只保留最新一个连接）
        if (tcpClient) // 如果已有旧客户端，断开并释放它（只保留最新一个连接）
        {
            tcpClient->disconnect(this); // 断开所有与 this 相关的信号连接
            tcpClient->abort();          // 立即中止连接
            tcpClient->deleteLater();    // 安全释放
        }
        // 将新的连接赋给 tcpClient
        tcpClient = incoming; // 将新的连接赋给 tcpClient
    }
    // 如果最终没有客户端（理论上不会），返回
    if (!tcpClient) // 如果最终没有客户端（理论上不会），返回
        return; // 返回本行给出的结果并结束当前函数。
    // 清空 TCP 接收缓冲区
    tcpReceiveBuffer.clear(); // 清空 TCP 接收缓冲区
    // 启用 TCP_NODELAY：禁用 Nagle 算法，降低延迟
    tcpClient->setSocketOption(QAbstractSocket::LowDelayOption, 1); // 启用 TCP_NODELAY：禁用 Nagle 算法，降低延迟
    // 启用 TCP KeepAlive：定期探测连接是否存活
    tcpClient->setSocketOption(QAbstractSocket::KeepAliveOption, 1); // 启用 TCP KeepAlive：定期探测连接是否存活
    // 连接 TCP 数据就绪信号
    connect(tcpClient, &QTcpSocket::readyRead, this, &ReceiverWindow::readTcpFrames); // 连接 TCP 数据就绪信号
    // 连接 TCP 断开信号
    connect(tcpClient, &QTcpSocket::disconnected, this, // 连接 TCP 断开信号
            &ReceiverWindow::tcpClientDisconnected); // 定义上方说明的成员函数；参数含义见函数前注释。
    // 记录对端 IP 地址（用于归档浏览器）
    activePeer = tcpClient->peerAddress(); // 记录对端 IP 地址（用于归档浏览器）
    // 设置 TCP 传输模式标志
    activeTcpTransport = true; // 设置 TCP 传输模式标志
    // 重置解码器（新的 TCP 连接需要新的解码会话）
    decoder.reset(); // 重置解码器（新的 TCP 连接需要新的解码会话）
    // 重置统计计数器
    intervalBytes = intervalFrames = 0; // 重置统计计数器
    // 重启统计计时器
    statisticsTimer.restart(); // 重启统计计时器
    // 将设备 IP 传递给归档浏览器
    archiveBrowser->setDeviceAddress(activePeer.toString()); // 将设备 IP 传递给归档浏览器
    // 更新状态显示
    statusLabel->setText(QStringLiteral("正在接收 H.264/TCP %1:%2") // 更新状态显示
                             .arg(activePeer.toString()) // 执行该行对应的具体操作。
                             .arg(tcpClient->peerPort())); // 执行该行对应的具体操作。
}

// TCP 客户端断开连接处理
void ReceiverWindow::tcpClientDisconnected() // TCP 客户端断开连接处理
{
    // 获取发送信号的 socket 并转为 QTcpSocket*
    QTcpSocket *disconnected = qobject_cast<QTcpSocket *>(sender()); // 获取发送信号的 socket 并转为 QTcpSocket*
    // 安全检查：确保断开的是当前活跃的客户端
    if (disconnected != tcpClient) // 安全检查：确保断开的是当前活跃的客户端
        return; // 返回本行给出的结果并结束当前函数。
    // 更新状态：提示客户端已断开
    statusLabel->setText(QStringLiteral("H.264/TCP 客户端已断开，继续等待 TCP/UDP 视频流")); // 更新状态：提示客户端已断开
    // 清空 TCP 接收缓冲区
    tcpReceiveBuffer.clear(); // 清空 TCP 接收缓冲区
    // 安全释放 TCP 客户端
    tcpClient->deleteLater(); // 安全释放 TCP 客户端
    tcpClient = nullptr; // 执行该行对应的具体操作。
}

// 读取并解析 TCP 帧数据（TCP 自定义协议：28 字节帧头 + H.264 载荷）
void ReceiverWindow::readTcpFrames() // 读取并解析 TCP 帧数据（TCP 自定义协议：28 字节帧头 + H.264 载荷）
{
    // 如果客户端不存在，直接返回
    if (!tcpClient) // 如果客户端不存在，直接返回
        return; // 返回本行给出的结果并结束当前函数。
    // 读取所有可用数据
    const QByteArray incoming = tcpClient->readAll(); // 读取所有可用数据
    // 累加统计周期内的接收字节数
    intervalBytes += quint64(incoming.size()); // 累加统计周期内的接收字节数
    // 追加到接收缓冲区
    tcpReceiveBuffer.append(incoming); // 追加到接收缓冲区
    // 循环解析：一次可能收到多个完整帧
    while (true) // 循环解析：一次可能收到多个完整帧
    {
        // 数据不足一个帧头，等待更多数据
        if (tcpReceiveBuffer.size() < TcpFrameHeaderSize) // 数据不足一个帧头，等待更多数据
            return; // 返回本行给出的结果并结束当前函数。
        // 数据不是以 IMXV 魔术字开头 → 需要帧同步（查找下一个魔术字位置）
        if (!tcpReceiveBuffer.startsWith(TcpFrameMagicBytes)) // 数据不是以 IMXV 魔术字开头 → 需要帧同步（查找下一个魔术字位置）
        {
            // 从位置 1 开始搜索魔术字
            const int marker = tcpReceiveBuffer.indexOf(TcpFrameMagicBytes, 1); // 从位置 1 开始搜索魔术字
            if (marker < 0) // 判断本行条件是否成立。
            {
                // 未找到魔术字：仅保留最后 3 字节（可能包含部分魔术字），丢弃前面数据
                tcpReceiveBuffer = tcpReceiveBuffer.right(3); // 未找到魔术字：仅保留最后 3 字节（可能包含部分魔术字），丢弃前面数据
                return; // 返回本行给出的结果并结束当前函数。
            }
            // 找到魔术字：丢弃魔术字之前的所有无效数据
            tcpReceiveBuffer.remove(0, marker); // 找到魔术字：丢弃魔术字之前的所有无效数据
            continue; // 执行该行对应的具体操作。
        }

        // === 解析 28 字节 TCP 帧头（大端序）===
        // 创建数据流，绑定到接收缓冲区
        QDataStream stream(tcpReceiveBuffer); // 创建数据流，绑定到接收缓冲区
        // 设置大端字节序
        stream.setByteOrder(QDataStream::BigEndian); // 设置大端字节序
        // 帧头字段变量
        quint32 magic = 0, payloadSize = 0, sequence = 0; // 帧头字段变量
        quint16 version = 0, headerSize = 0, width = 0, height = 0; // 定义并初始化本行使用的变量。
        qint64 timestampMs = 0; // 定义并初始化本行使用的变量。
        // 从流中依次读取：魔术字 → 版本 → 头大小 → 载荷大小 → 帧序号 → 时间戳 → 宽度 → 高度
        stream >> magic >> version >> headerSize >> payloadSize >> sequence >> timestampMs >> width >> height; // 执行该行对应的具体操作。
        // 校验帧头合法性：魔术字匹配、版本匹配、头大小匹配、载荷非空且不超过上限、宽高有效
        if (magic != TcpFrameMagic || version != TcpFrameVersion || // 判断本行条件是否成立。
            headerSize != TcpFrameHeaderSize || payloadSize == 0 || // 执行该行对应的具体操作。
            payloadSize > MaximumTcpPayloadSize || width == 0 || height == 0) // 执行该行对应的具体操作。
        {
            // 帧头无效：丢弃 1 字节，重新同步
            tcpReceiveBuffer.remove(0, 1); // 帧头无效：丢弃 1 字节，重新同步
            continue; // 执行该行对应的具体操作。
        }
        // 计算完整帧的总大小（帧头 + 载荷）
        const qint64 totalSize = qint64(headerSize) + payloadSize; // 计算完整帧的总大小（帧头 + 载荷）
        // 数据不足一个完整帧，等待更多数据
        if (tcpReceiveBuffer.size() < totalSize) // 数据不足一个完整帧，等待更多数据
            return; // 返回本行给出的结果并结束当前函数。
        // 从缓冲区中提取 H.264 访问单元（载荷部分）
        const QByteArray accessUnit = tcpReceiveBuffer.mid(headerSize, payloadSize); // 从缓冲区中提取 H.264 访问单元（载荷部分）
        // 从缓冲区中移除已消耗的数据
        tcpReceiveBuffer.remove(0, int(totalSize)); // 从缓冲区中移除已消耗的数据

        // 如果刚从 UDP 模式切换到 TCP 模式，重置解码器和统计
        if (!activeTcpTransport) // 如果刚从 UDP 模式切换到 TCP 模式，重置解码器和统计
        {
            activeTcpTransport = true; // 执行该行对应的具体操作。
            decoder.reset(); // 执行该行对应的具体操作。
            intervalBytes = quint64(accessUnit.size() + headerSize); // 执行该行对应的具体操作。
            intervalFrames = 0; // 执行该行对应的具体操作。
            statisticsTimer.restart(); // 执行该行对应的具体操作。
        }
        // 创建解码耗时计时器
        QElapsedTimer decodeTimer; // 创建解码耗时计时器
        decodeTimer.start(); // 执行该行对应的具体操作。
        // 解码后的图像
        QImage image; // 解码后的图像
        // 解码错误描述
        QString decodeError; // 解码错误描述
        // 调用 H.264 解码器解码访问单元
        if (!decoder.decode(accessUnit, &image, &decodeError)) // 调用 H.264 解码器解码访问单元
        {
            // 解码失败：显示错误（非空错误信息才显示）
            if (!decodeError.isEmpty()) // 解码失败：显示错误（非空错误信息才显示）
                statusLabel->setText(QStringLiteral("H.264/TCP 解码失败：%1") // 执行该行对应的具体操作。
                                         .arg(decodeError)); // 执行该行对应的具体操作。
            continue; // 执行该行对应的具体操作。
        }
        // 计算时钟延迟：当前时间 - 发送方时间戳
        const qint64 clockLatency = QDateTime::currentMSecsSinceEpoch() - timestampMs; // 计算时钟延迟：当前时间 - 发送方时间戳
        // 端到端延迟：仅当时钟延迟在合理范围内才有效（0 ~ 10s），否则标记为不同步
        const qint64 endToEndLatency = clockLatency >= 0 && // 端到端延迟：仅当时钟延迟在合理范围内才有效（0 ~ 10s），否则标记为不同步
                                               clockLatency <= MaximumSensibleClockLatencyMs // 执行该行对应的具体操作。
                                           ? clockLatency // 执行该行对应的具体操作。
                                           : -1; // 执行该行对应的具体操作。
        // 显示解码后的图像到界面
        showFrame(image); // 显示解码后的图像到界面
        // 更新 FPS/带宽统计信息
        updateStatistics(sequence, decodeTimer.elapsed(), endToEndLatency); // 更新 FPS/带宽统计信息
    }
}

// 重置视频会话状态：重置 RTP 重组器和 H.264 解码器
void ReceiverWindow::resetVideoSession() // 重置视频会话状态：重置 RTP 重组器和 H.264 解码器
{
    // 重置 RTP 帧重组器状态
    reassembler.reset(); // 重置 RTP 帧重组器状态
    // 重置 H.264 解码器状态
    decoder.reset(); // 重置 H.264 解码器状态
    // 无活跃流
    hasActiveStream = false; // 无活跃流
    // 等待关键帧（IDR 帧）
    waitingForKeyFrame = true; // 等待关键帧（IDR 帧）
    // 重置统计计数器
    intervalBytes = intervalFrames = 0; // 重置统计计数器
}

// 读取并处理所有待处理的 UDP 数据报（RTP/UDP 模式）
void ReceiverWindow::readPendingDatagrams() // 读取并处理所有待处理的 UDP 数据报（RTP/UDP 模式）
{
    // 循环处理所有待读数据报
    while (videoSocket->hasPendingDatagrams()) // 循环处理所有待读数据报
    {
        // 分配与数据报大小相同的缓冲区
        QByteArray datagram(int(videoSocket->pendingDatagramSize()), Qt::Uninitialized); // 分配与数据报大小相同的缓冲区
        // 对端 IP 地址
        QHostAddress peer; // 对端 IP 地址
        // 对端端口号
        quint16 peerPort = 0; // 对端端口号
        // 读取数据报到缓冲区（同时获取对端地址和端口）
        if (videoSocket->readDatagram(datagram.data(), datagram.size(), &peer, &peerPort) < 0) // 读取数据报到缓冲区（同时获取对端地址和端口）
            continue; // 读取失败，跳过
        // 累加统计周期内的接收字节数
        intervalBytes += quint64(datagram.size()); // 累加统计周期内的接收字节数
        // RTP 分包头部
        VideoRtp::Header header; // RTP 分包头部
        // RTP 分包载荷
        QByteArray payload; // RTP 分包载荷
        // 解析 RTP 数据报（验证头部、提取载荷）
        if (!VideoRtp::parseDatagram(datagram, &header, &payload)) // 解析 RTP 数据报（验证头部、提取载荷）
            continue; // 解析失败，跳过
        // 如果刚从 TCP 模式切换到 UDP 模式，重置统计
        if (activeTcpTransport) // 如果刚从 TCP 模式切换到 UDP 模式，重置统计
        {
            activeTcpTransport = false; // 执行该行对应的具体操作。
            intervalBytes = quint64(datagram.size()); // 执行该行对应的具体操作。
            intervalFrames = 0; // 执行该行对应的具体操作。
            statisticsTimer.restart(); // 执行该行对应的具体操作。
        }
        // 检测流切换：无活跃流、流 ID 变化或对端 IP 变化 → 重置视频会话
        if (!hasActiveStream || header.streamId != activeStreamId || peer != activePeer) // 检测流切换：无活跃流、流 ID 变化或对端 IP 变化 → 重置视频会话
        {
            resetVideoSession(); // 执行该行对应的具体操作。
            hasActiveStream = true; // 执行该行对应的具体操作。
            activeStreamId = header.streamId; // 执行该行对应的具体操作。
            activePeer = peer; // 执行该行对应的具体操作。
            // 将设备 IP 传递给归档浏览器
            archiveBrowser->setDeviceAddress(peer.toString()); // 将设备 IP 传递给归档浏览器
            // 显示新的流信息
            statusLabel->setText(QStringLiteral("正在接收 %1:%2，流ID %3") // 显示新的流信息
                                     .arg(peer.toString()) // 执行该行对应的具体操作。
                                     .arg(peerPort) // 执行该行对应的具体操作。
                                     .arg(activeStreamId)); // 执行该行对应的具体操作。
            // 重启统计计时器
            statisticsTimer.restart(); // 重启统计计时器
        }
        // 将 RTP 分包推入重组器，立即处理可能产生的完整帧
        handleCompleteFrames(reassembler.push(header, payload, // 将 RTP 分包推入重组器，立即处理可能产生的完整帧
                                              QDateTime::currentMSecsSinceEpoch())); // 执行该行对应的具体操作。
    }
}

// 定时器触发的过期清理：将超时未完成的帧标记为过期并处理
void ReceiverWindow::expireIncompleteFrames() // 定时器触发的过期清理：将超时未完成的帧标记为过期并处理
{
    // 从重组器中取出所有超时分包，返回可能部分完整的帧
    handleCompleteFrames(reassembler.expire(QDateTime::currentMSecsSinceEpoch())); // 从重组器中取出所有超时分包，返回可能部分完整的帧
}

// 处理完整帧：解码并显示
// @param frames 重组器产生的一系列完整帧（可能为空）
void ReceiverWindow::handleCompleteFrames(const QVector<VideoRtp::CompleteFrame> &frames) // 定义上方说明的成员函数；参数含义见函数前注释。
{
    // 遍历每一帧完整帧
    for (const VideoRtp::CompleteFrame &frame : frames) // 遍历每一帧完整帧
    {
        // 如果帧前面发生了不连续（丢帧），重置解码器并等待新的关键帧
        if (frame.discontinuityBefore) // 如果帧前面发生了不连续（丢帧），重置解码器并等待新的关键帧
        {
            decoder.reset(); // 执行该行对应的具体操作。
            waitingForKeyFrame = true; // 执行该行对应的具体操作。
        }
        // 如果正在等待关键帧且当前不是关键帧，跳过此帧
        if (waitingForKeyFrame && !frame.keyFrame) // 如果正在等待关键帧且当前不是关键帧，跳过此帧
            continue; // 执行该行对应的具体操作。
        // 收到关键帧：结束等待状态
        if (frame.keyFrame) // 收到关键帧：结束等待状态
            waitingForKeyFrame = false; // 执行该行对应的具体操作。

        // 创建解码计时器
        QElapsedTimer decodeTimer; // 创建解码计时器
        decodeTimer.start(); // 执行该行对应的具体操作。
        // 解码后的图像
        QImage image; // 解码后的图像
        // 解码错误描述
        QString error; // 解码错误描述
        // 解码 H.264 访问单元
        if (!decoder.decode(frame.accessUnit, &image, &error)) // 解码 H.264 访问单元
        {
            // 解码失败：显示错误
            if (!error.isEmpty()) // 解码失败：显示错误
                statusLabel->setText(QStringLiteral("H.264解码失败：%1").arg(error)); // 执行该行对应的具体操作。
            continue; // 执行该行对应的具体操作。
        }
        // 本地处理延迟 = 重组耗时 + 解码耗时
        const qint64 localLatency = frame.reassemblyMilliseconds + decodeTimer.elapsed(); // 本地处理延迟 = 重组耗时 + 解码耗时
        // 当前时间戳（微秒）
        const qint64 nowUs = QDateTime::currentMSecsSinceEpoch() * 1000; // 当前时间戳（微秒）
        // 时钟延迟 = 当前时间 - 发送时间戳（转为毫秒）
        const qint64 clockLatency = (nowUs - qint64(frame.sendTimestampUs)) / 1000; // 时钟延迟 = 当前时间 - 发送时间戳（转为毫秒）
        // 端到端延迟：仅当时钟延迟在合理范围内才有效
        const qint64 endToEndLatency = clockLatency >= 0 && // 端到端延迟：仅当时钟延迟在合理范围内才有效
                                               clockLatency <= MaximumSensibleClockLatencyMs // 执行该行对应的具体操作。
                                           ? clockLatency // 执行该行对应的具体操作。
                                           : -1; // 执行该行对应的具体操作。
        // 显示解码图像
        showFrame(image); // 显示解码图像
        // 更新 FPS/带宽/丢帧统计
        updateStatistics(frame.sequence, localLatency, endToEndLatency); // 更新 FPS/带宽/丢帧统计
    }
}

// 在界面上显示解码后的视频帧
// @param image 解码后的 QImage 视频帧
void ReceiverWindow::showFrame(const QImage &image) // 定义上方说明的成员函数；参数含义见函数前注释。
{
    // 保存最后一帧（用于窗口缩放时重新渲染）
    lastFrame = image; // 保存最后一帧（用于窗口缩放时重新渲染）
    // 将 QImage 转为 QPixmap，按标签尺寸等比例缩放后设置到显示标签
    frameLabel->setPixmap(QPixmap::fromImage(lastFrame).scaled( // 将 QImage 转为 QPixmap，按标签尺寸等比例缩放后设置到显示标签
        frameLabel->size(), Qt::KeepAspectRatio, Qt::FastTransformation)); // 执行该行对应的具体操作。
}

// 更新 FPS、带宽、丢帧等统计信息（每 1 秒更新一次）
// @param sequence 当前帧的帧序列号
// @param localLatencyMs 本地处理延迟（解码+重组耗时，毫秒）
// @param endToEndLatencyMs 端到端延迟（毫秒），-1 表示时钟未同步
void ReceiverWindow::updateStatistics(quint32 sequence, qint64 localLatencyMs, // 定义上方说明的成员函数；参数含义见函数前注释。
                                      qint64 endToEndLatencyMs) // 定义并初始化本行使用的变量。
{
    // 记录最后处理的帧序列号
    lastSequence = sequence; // 记录最后处理的帧序列号
    // 统计周期内的帧数加 1
    ++intervalFrames; // 统计周期内的帧数加 1
    // 距离上次统计更新的经过时间
    const qint64 elapsed = statisticsTimer.elapsed(); // 距离上次统计更新的经过时间
    // 如果不足 1 秒，暂不更新统计显示
    if (elapsed < 1000) // 如果不足 1 秒，暂不更新统计显示
        return; // 返回本行给出的结果并结束当前函数。
    // 计算 FPS：帧数 / 秒数
    const double fps = intervalFrames * 1000.0 / elapsed; // 计算 FPS：帧数 / 秒数
    // 计算带宽 Mbps：字节数 * 8 / 秒数 / 1000
    const double mbps = intervalBytes * 8.0 / elapsed / 1000.0; // 计算带宽 Mbps：字节数 * 8 / 秒数 / 1000
    // 格式化端到端延迟字符串
    const QString e2e = endToEndLatencyMs >= 0 // 格式化端到端延迟字符串
                            ? QStringLiteral("%1 ms").arg(endToEndLatencyMs) // 执行该行对应的具体操作。
                            : QStringLiteral("时钟未同步"); // 执行该行对应的具体操作。
    // 根据传输模式显示不同的统计信息
    if (activeTcpTransport) // 根据传输模式显示不同的统计信息
        // TCP 模式统计：FPS、带宽、解码耗时、端到端延迟、最后一帧序号
        statisticsLabel->setText( // TCP 模式统计：FPS、带宽、解码耗时、端到端延迟、最后一帧序号
            QStringLiteral("TCP/H.264  FPS:%1  %2 Mbps  解码:%3 ms  端到端:%4  帧:%5") // 执行该行对应的具体操作。
                .arg(fps, 0, 'f', 1) // 执行该行对应的具体操作。
                .arg(mbps, 0, 'f', 2) // 执行该行对应的具体操作。
                .arg(localLatencyMs) // 执行该行对应的具体操作。
                .arg(e2e) // 执行该行对应的具体操作。
                .arg(lastSequence)); // 执行该行对应的具体操作。
    else // 进入上一条件不成立时的处理分支。
        // UDP 模式统计：FPS、带宽、丢帧数、丢片数、重组+解码耗时、端到端延迟、最后一帧序号
        statisticsLabel->setText( // 执行该行对应的具体操作。
            QStringLiteral("UDP/H.264  FPS:%1  %2 Mbps  丢帧:%3  丢片:%4  重组+解码:%5 ms  端到端:%6  帧:%7") // 执行该行对应的具体操作。
                .arg(fps, 0, 'f', 1) // 执行该行对应的具体操作。
                .arg(mbps, 0, 'f', 2) // 执行该行对应的具体操作。
                .arg(reassembler.lostFrames()) // 执行该行对应的具体操作。
                .arg(reassembler.lostFragments()) // 执行该行对应的具体操作。
                .arg(localLatencyMs) // 执行该行对应的具体操作。
                .arg(e2e) // 执行该行对应的具体操作。
                .arg(lastSequence)); // 执行该行对应的具体操作。
    // 重置统计周期计数器
    intervalBytes = intervalFrames = 0; // 重置统计周期计数器
    // 重启统计计时器
    statisticsTimer.restart(); // 重启统计计时器
}

// 切换 MQTT 连接状态：连接或断开
void ReceiverWindow::toggleMqttConnection() // 切换 MQTT 连接状态：连接或断开
{
    // 如果当前不是断开状态 → 执行断开操作
    if (mqttClient->state() != QMqttClient::Disconnected) // 如果当前不是断开状态 → 执行断开操作
    {
        mqttClient->disconnectFromHost(); // 执行该行对应的具体操作。
        return; // 返回本行给出的结果并结束当前函数。
    }
    // 检查服务器地址和设备 ID 是否已填写
    if (mqttHostEdit->text().trimmed().isEmpty() || deviceIdEdit->text().trimmed().isEmpty()) // 检查服务器地址和设备 ID 是否已填写
    {
        mqttStatusLabel->setText(QStringLiteral("请填写MQTT服务器和设备ID")); // 执行该行对应的具体操作。
        return; // 返回本行给出的结果并结束当前函数。
    }
    // 设置 MQTT 服务器主机名
    mqttClient->setHostname(mqttHostEdit->text().trimmed()); // 设置 MQTT 服务器主机名
    // 设置 MQTT 服务器端口
    mqttClient->setPort(quint16(mqttPortSpinBox->value())); // 设置 MQTT 服务器端口
    // 设置客户端 ID（pc-monitor- + 当前毫秒时间戳，确保唯一性）
    mqttClient->setClientId(QStringLiteral("pc-monitor-%1").arg(QDateTime::currentMSecsSinceEpoch())); // 设置客户端 ID（pc-monitor- + 当前毫秒时间戳，确保唯一性）
    // 发起异步连接
    mqttClient->connectToHost(); // 发起异步连接
}

// 根据后缀生成完整的设备 MQTT 主题路径
// @param suffix 主题后缀（如 "status", "events", "commands"）
// @return 完整的 MQTT 主题字符串，格式为 "devices/{deviceId}/{suffix}"
QString ReceiverWindow::deviceTopic(const QString &suffix) const // 定义上方说明的成员函数；参数含义见函数前注释。
{
    return QStringLiteral("devices/%1/%2").arg(deviceIdEdit->text().trimmed(), suffix); // 返回本行给出的结果并结束当前函数。
}

// 订阅设备相关的 MQTT 主题（状态、事件、命令回执）
void ReceiverWindow::subscribeDeviceTopics() // 订阅设备相关的 MQTT 主题（状态、事件、命令回执）
{
    // 需要订阅的主题后缀列表
    const QStringList suffixes = {QStringLiteral("status"), QStringLiteral("events"), // 需要订阅的主题后缀列表
                                  QStringLiteral("command_ack")}; // 执行该行对应的具体操作。
    // QoS 1 级别订阅每个主题
    for (const QString &suffix : suffixes) // QoS 1 级别订阅每个主题
        if (!mqttClient->subscribe(deviceTopic(suffix), 1)) // 判断本行条件是否成立。
            mqttStatusLabel->setText(QStringLiteral("订阅失败：%1").arg(suffix)); // 执行该行对应的具体操作。
}

// 处理接收到的 MQTT 消息：设备状态更新或事件/命令回执
// @param message MQTT 消息体（JSON 格式字节数组）
// @param topic MQTT 主题字符串
void ReceiverWindow::handleMqttMessage(const QByteArray &message, const QString &topic) // 定义上方说明的成员函数；参数含义见函数前注释。
{
    // JSON 解析错误对象
    QJsonParseError error; // JSON 解析错误对象
    // 解析 MQTT 消息为 JSON 文档
    const QJsonDocument document = QJsonDocument::fromJson(message, &error); // 解析 MQTT 消息为 JSON 文档
    // 如果解析失败或不是 Object 类型，使用空 Object
    const QJsonObject object = document.isObject() ? document.object() : QJsonObject(); // 如果解析失败或不是 Object 类型，使用空 Object

    // 如果是设备状态主题
    if (topic == deviceTopic(QStringLiteral("status"))) // 如果是设备状态主题
    {
        // JSON 解析成功且为 Object 类型时，格式化设备状态信息
        if (error.error == QJsonParseError::NoError) // JSON 解析成功且为 Object 类型时，格式化设备状态信息
            deviceStatusLabel->setText(QStringLiteral( // 执行该行对应的具体操作。
                                           "设备: %1  采集: %2  推流: %3  画面检测: %4  录像: %5\n" // 执行该行对应的具体操作。
                                           "时间: %6") // 执行该行对应的具体操作。
                                           // 设备 ID
                                           .arg(object.value("deviceId").toString(), // 设备 ID
                                                // 采集状态：ON/OFF
                                                object.value("captureEnabled").toBool() ? "ON" : "OFF", // 采集状态：ON/OFF
                                                // 推流状态：ON/OFF
                                                object.value("streamingEnabled").toBool() ? "ON" : "OFF", // 推流状态：ON/OFF
                                                // 视觉检测状态：ON/OFF
                                                object.value("visionEnabled").toBool() ? "ON" : "OFF", // 视觉检测状态：ON/OFF
                                                // 录像状态
                                                object.value("recording").toBool() ? "ON" : "OFF", // 录像状态
                                                // 时间戳
                                                object.value("timestamp").toString())); // 时间戳
        return; // 返回本行给出的结果并结束当前函数。
    }
    // 提取消息时间戳
    const QString timestamp = object.value(QStringLiteral("timestamp")).toString(); // 提取消息时间戳
    // 根据主题类型提取描述文本：
    // 事件主题 → 取 message 字段；命令回执主题 → 格式化 command + message
    const QString description = topic.endsWith(QLatin1String("/events")) // 事件主题 → 取 message 字段；命令回执主题 → 格式化 command + message
                                    ? object.value(QStringLiteral("message")).toString() // 执行该行对应的具体操作。
                                    : QStringLiteral("命令 %1：%2").arg(object.value(QStringLiteral("command")).toString(), object.value(QStringLiteral("message")).toString()); // 执行该行对应的具体操作。
    // 创建事件列表项：时间戳 + 描述
    QListWidgetItem *item = new QListWidgetItem(QStringLiteral("%1  %2").arg(timestamp, description)); // 创建事件列表项：时间戳 + 描述
    // 事件类型的消息使用橙色前景色
    if (topic.endsWith(QLatin1String("/events"))) // 事件类型的消息使用橙色前景色
        item->setForeground(QColor(QStringLiteral("#d35400"))); // 执行该行对应的具体操作。
    // 插入到列表顶部（最新的显示在最前面）
    eventList->insertItem(0, item); // 插入到列表顶部（最新的显示在最前面）
    // 限制事件列表最多保留 200 条，超出时从尾部删除
    while (eventList->count() > 200) // 限制事件列表最多保留 200 条，超出时从尾部删除
        delete eventList->takeItem(eventList->count() - 1); // 执行该行对应的具体操作。
}

// 通过 MQTT 向设备发布远程控制命令
// @param command 命令字符串（如 "start_capture"、"stream_on" 等）
// @param arguments 附加参数（JSON 对象）
void ReceiverWindow::publishCommand(const QString &command, const QJsonObject &arguments) // 定义上方说明的成员函数；参数含义见函数前注释。
{
    // 检查 MQTT 是否已连接
    if (mqttClient->state() != QMqttClient::Connected) // 检查 MQTT 是否已连接
    {
        mqttStatusLabel->setText(QStringLiteral("请先连接MQTT")); // 执行该行对应的具体操作。
        return; // 返回本行给出的结果并结束当前函数。
    }
    // 构造请求 JSON 对象（以附加参数为基础）
    QJsonObject request = arguments; // 构造请求 JSON 对象（以附加参数为基础）
    // 插入命令名称
    request.insert(QStringLiteral("command"), command); // 插入命令名称
    // 插入当前时间戳（ISO 格式，含毫秒）
    request.insert(QStringLiteral("timestamp"), QDateTime::currentDateTime().toString(Qt::ISODateWithMs)); // 插入当前时间戳（ISO 格式，含毫秒）
    // 发布到设备命令主题，QoS=1 至少一次传递，不保留消息
    if (mqttClient->publish(deviceTopic(QStringLiteral("commands")), // 发布到设备命令主题，QoS=1 至少一次传递，不保留消息
                            QJsonDocument(request).toJson(QJsonDocument::Compact), 1, false) < 0) // 执行该行对应的具体操作。
        mqttStatusLabel->setText(QStringLiteral("远程命令发布失败")); // 执行该行对应的具体操作。
}

// 窗口大小变化事件：重新缩放视频帧以适应新窗口尺寸
// @param event 窗口大小调整事件
void ReceiverWindow::resizeEvent(QResizeEvent *event) // 定义上方说明的成员函数；参数含义见函数前注释。
{
    // 调用基类处理（必须调用以保证 Qt 内部布局正常工作）
    QWidget::resizeEvent(event); // 调用基类处理（必须调用以保证 Qt 内部布局正常工作）
    // 如果有缓存的最后一帧，重新缩放到新尺寸
    if (!lastFrame.isNull()) // 如果有缓存的最后一帧，重新缩放到新尺寸
        showFrame(lastFrame); // 执行该行对应的具体操作。
}
