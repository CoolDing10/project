#include "archive_browser.h" // 引入 ArchiveBrowser 页面声明。

#include <QAbstractSocket> // 提供 socket 状态与错误枚举。
#include <QDateTime>       // 解析归档文件 ISO 修改时间。
#include <QDesktopServices> // 调用系统文件管理器打开下载目录。
#include <QDir>            // 创建目录和安全拼接路径。
#include <QFile>           // 顺序读取旧版 MJPEG 文件。
#include <QFileInfo>       // 提取纯文件名、后缀和父目录。
#include <QHBoxLayout>     // 构建顶部控制栏与左右内容区。
#include <QJsonArray>      // 解析服务端 files 数组。
#include <QJsonDocument>   // 序列化请求并解析响应 JSON。
#include <QJsonObject>     // 保存请求、响应头和单个文件元数据。
#include <QLabel>          // 显示状态、标题和 MJPEG 图像。
#include <QLineEdit>       // 输入开发板地址。
#include <QListWidget>     // 显示归档文件列表。
#include <QListWidgetItem> // 保存每个文件的文本和隐藏角色数据。
#include <QMediaContent>   // 把本地 URL 包装成播放器媒体源。
#include <QMediaPlayer>    // 播放下载后的 H.264 文件。
#include <QPixmap>         // 把 QImage 缩放并显示到 QLabel。
#include <QPushButton>     // 刷新、下载和打开目录按钮。
#include <QResizeEvent>    // 页面尺寸变化事件类型。
#include <QSaveFile>       // 先写临时文件再原子提交下载结果。
#include <QSpinBox>        // 输入归档服务 TCP 端口。
#include <QStandardPaths>  // 获取桌面环境的标准下载目录。
#include <QStackedWidget>  // 切换 QLabel 与 QVideoWidget。
#include <QTcpSocket>      // 建立归档协议短连接。
#include <QTimer>          // 按约 15 FPS 推进 MJPEG 帧。
#include <QUrl>            // 构造本地文件 URL。
#include <QVBoxLayout>     // 构建页面与列表面板垂直布局。
#include <QVideoWidget>    // 显示 QMediaPlayer 视频输出。

namespace { // 文件内部常量、角色编号和显示辅助函数。
const int CategoryRole = Qt::UserRole + 1;             // 列表项隐藏保存recordings类别。
const int NameRole = Qt::UserRole + 2;                 // 列表项隐藏保存服务端原始文件名。
const int SizeRole = Qt::UserRole + 3;                 // 列表项隐藏保存服务端报告的文件大小。
const qint64 MaximumListPayload = 8 * 1024 * 1024;     // 列表JSON最大8 MiB，防止异常响应耗尽内存。
const int MjpegFrameIntervalMs = 66;                   // 约15 FPS的本地MJPEG回放周期。

/** 把字节数格式化成适合文件列表显示的KB或MB文本。 */
QString readableSize(qint64 bytes) // bytes 是原始文件大小，返回 KB 或 MB 文本。
{
    // 大于等于1 MiB时显示一位小数的MB。
    if (bytes >= 1024 * 1024) // 1MiB 以上改用 MB 单位。
        return QStringLiteral("%1 MB").arg(bytes / 1024.0 / 1024.0, 0, 'f', 1); // 除两次 1024 并保留一位小数。
    // 小文件至少显示1 KB，避免用户误以为文件为空。
    return QStringLiteral("%1 KB").arg(qMax<qint64>(1, bytes / 1024)); // 小于 1KB 也至少显示 1KB。
}
} // namespace：协议显示常量和工具函数不导出到其他编译单元。

/** 创建PC归档页的控件、布局、网络状态机和MJPEG播放定时器。 */
ArchiveBrowser::ArchiveBrowser(QWidget *parent) // parent 是主窗口中的标签页父控件。
    : QWidget(parent), // 初始化QWidget父子关系。
      socket(new QTcpSocket(this)), // 创建复用的归档TCP客户端。
      deviceAddressEdit(new QLineEdit(QStringLiteral("192.168.10.50"), this)), // 默认开发板IP。
      archivePortSpinBox(new QSpinBox(this)), // 创建端口输入框。
      refreshButton(new QPushButton(QStringLiteral("刷新全部归档"), this)), // 列表请求按钮。
      downloadButton(new QPushButton(QStringLiteral("下载并查看"), this)), // 文件请求按钮。
      statusLabel(new QLabel(QStringLiteral("点击刷新，读取开发板中的事件录像"), this)), // 状态提示。
      archiveList(new QListWidget(this)), // 文件清单控件。
      previewLabel(new QLabel(QStringLiteral("选择一段事件录像"), this)), // 媒体预览控件。
      previewStack(new QStackedWidget(this)), // 图片与 H.264 视频输出切换容器。
      h264Player(new QMediaPlayer(this)), // H.264 录像播放器。
      h264VideoOutput(new QVideoWidget(this)), // H.264 视频输出。
      mjpegPlaybackTimer(new QTimer(this)), // 录像逐帧播放定时器。
      mjpegFile(new QFile(this)), // 本地录像读取对象。
      requestKind(NoRequest), // 启动时网络状态为空闲。
      requestedSize(0), // 尚无请求载荷长度。
      responseHeaderParsed(false), // 尚未解析响应头。
      payloadBytesRemaining(0), // 尚无待接收载荷。
      downloadFile(nullptr) // 尚未创建原子下载临时文件。
{
    // TCP端口合法范围为1~65535，0不作为用户可选监听端口。
    archivePortSpinBox->setRange(1, 65535); // 限制为合法 TCP 端口范围。
    // 与开发板MediaArchiveServer默认端口一致。
    archivePortSpinBox->setValue(8890); // 使用开发板归档服务默认端口。
    // 防止IP输入框在窗口缩小时被布局压缩得无法辨认。
    deviceAddressEdit->setMinimumWidth(150); // 保证完整 IPv4 地址可见。
    // 长错误文本允许自动换行而不是撑宽整个窗口。
    statusLabel->setWordWrap(true); // 允许长路径和错误文本换行。
    // 给文件名、时间和大小信息保留足够显示宽度。
    archiveList->setMinimumWidth(340); // 给文件名、大小和日期留出空间。
    // 图片和录像帧在黑色预览区域居中显示。
    previewLabel->setAlignment(Qt::AlignCenter); // 图片和提示文字居中。
    previewLabel->setMinimumSize(480, 320);      // 设置预览区域最小尺寸。
    // 独立设置深色预览区域，未显示图片时仍与监控平台主题一致。
    previewLabel->setStyleSheet(QStringLiteral( // 设置静态/MJPEG 预览区暗色样式，
        "background:#05070a; color:#96a4b5; border:1px solid #354150; " // 包含背景、文字和边框，
        "border-radius:7px;")); // 最后设置圆角并结束样式。
    h264VideoOutput->setStyleSheet(QStringLiteral("background:#05070a;")); // H.264 输出区使用相同深色底。
    h264Player->setVideoOutput(h264VideoOutput); // 将播放器画面绑定到 QVideoWidget。
    previewStack->addWidget(previewLabel);       // 索引 0 放图片/MJPEG 标签。
    previewStack->addWidget(h264VideoOutput);    // 索引 1 放 H.264 视频控件。
    previewStack->setCurrentWidget(previewLabel); // 初始显示提示标签。

    // 打开下载目录按钮只在构造函数内连接，无需保存为成员。
    QPushButton *openDirectoryButton = new QPushButton( // 仅构造阶段使用局部指针，父对象负责释放，
        QStringLiteral("打开电脑下载目录"), this);      // 设置按钮文字和父对象。
    // controls保存页面顶部的连接参数和操作按钮。
    QHBoxLayout *controls = new QHBoxLayout(); // 顶部连接参数和操作按钮水平布局。
    controls->addWidget(new QLabel(QStringLiteral("开发板IP："), this)); // 添加地址说明标签。
    controls->addWidget(deviceAddressEdit);    // 添加设备地址输入框。
    controls->addWidget(new QLabel(QStringLiteral("归档端口："), this)); // 添加端口说明标签。
    controls->addWidget(archivePortSpinBox);   // 添加端口输入框。
    controls->addWidget(refreshButton);        // 添加刷新列表按钮。
    controls->addWidget(downloadButton);       // 添加下载按钮。
    controls->addWidget(openDirectoryButton);  // 添加打开本地目录按钮。
    // 弹性空间吸收右侧空白，避免按钮被均匀拉宽。
    controls->addStretch(); // 把控件保持在左侧，右侧吸收剩余空间。

    // listPanel把标题和归档列表组织成左侧竖向面板。
    QWidget *listPanel = new QWidget(this); // 左侧归档列表容器。
    QVBoxLayout *listLayout = new QVBoxLayout(listPanel); // 容器内部垂直布局。
    // 外层布局已经负责页面边距，子面板无需重复留白。
    listLayout->setContentsMargins(0, 0, 0, 0); // 避免与外层边距叠加。
    listLayout->addWidget(new QLabel(QStringLiteral("开发板媒体归档"), listPanel)); // 添加列表标题。
    listLayout->addWidget(archiveList, 1); // 列表使用 stretch=1 占满剩余高度。

    // content按2:3比例分配文件列表与媒体预览空间。
    QHBoxLayout *content = new QHBoxLayout(); // 主体左右分栏布局。
    content->addWidget(listPanel, 2);         // 左侧列表占 2 份宽度。
    content->addWidget(previewStack, 3);      // 右侧预览占 3 份宽度。

    // 页面顶层采用“控制栏、状态栏、主体内容”的垂直结构。
    QVBoxLayout *layout = new QVBoxLayout(this); // 页面顶层布局并安装到 this。
    layout->addLayout(controls);                 // 第一行放控制栏。
    layout->addWidget(statusLabel);              // 第二行放状态提示。
    layout->addLayout(content, 1);               // 主体区域占剩余高度。

    // 用户点击刷新时发起列表请求。
    connect(refreshButton, &QPushButton::clicked, // 用户点击刷新按钮，
            this, &ArchiveBrowser::refreshArchive); // 发起列表请求。
    // 用户点击下载时处理当前选中项。
    connect(downloadButton, &QPushButton::clicked, // 用户点击下载按钮，
            this, &ArchiveBrowser::downloadSelected); // 下载当前选择项。
    // 使用系统默认文件管理器打开本地下载根目录。
    connect(openDirectoryButton, &QPushButton::clicked, // 点击打开目录按钮，
            this, &ArchiveBrowser::openDownloadDirectory); // 调用系统文件管理器。
    // 双击列表项等价于选择后点击下载按钮。
    connect(archiveList, &QListWidget::itemDoubleClicked, // 双击任意列表项时，
            this, [this](QListWidgetItem *) { downloadSelected(); }); // 忽略项参数，按当前选择下载。
    // TCP三段式状态机：连接成功发送请求、可读时解析、错误时复位。
    connect(socket, &QTcpSocket::connected, // 异步连接成功时，
            this, &ArchiveBrowser::sendRequest); // 发送当前请求 JSON。
    connect(socket, &QTcpSocket::readyRead, // socket 出现可读字节时，
            this, &ArchiveBrowser::readResponse); // 累积并解析协议响应。
    connect(socket, // 监听归档 TCP 客户端错误信号，
            QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error), // 指定带错误枚举的重载，
            this, [this](QAbstractSocket::SocketError) { handleSocketError(); }); // 忽略枚举，统一读取 errorString()。
    // 每66ms尝试解码下一张JPEG，形成约15 FPS播放效果。
    mjpegPlaybackTimer->setInterval(MjpegFrameIntervalMs); // 设置约 15 FPS 的 66ms 周期。
    connect(mjpegPlaybackTimer, &QTimer::timeout, // 每次播放定时器到期，
            this, &ArchiveBrowser::showNextMjpegFrame); // 解析并显示下一张 JPEG。
}

/** 同步设备地址，并移除IPv4映射IPv6地址常见的::ffff:前缀。 */
void ArchiveBrowser::setDeviceAddress(const QString &address) // address 是视频对端地址文本。
{
    // 使用副本规范化，避免修改调用者传入的常量引用。
    QString normalized = address; // 创建可修改副本用于去除映射前缀。
    // QTcpSocket在双栈系统上可能把192.168.10.50表示为::ffff:192.168.10.50。
    if (normalized.startsWith(QLatin1String("::ffff:"))) // 检查 IPv4 映射 IPv6 前缀。
        normalized.remove(0, 7); // 删除七个字符，仅保留 IPv4 部分。
    // 空地址不覆盖用户已经填写的有效开发板地址。
    if (!normalized.isEmpty()) // 只有规范化后仍有内容时，
        deviceAddressEdit->setText(normalized); // 才覆盖用户当前地址。
}

/** 使用统一入口发起归档列表请求。 */
void ArchiveBrowser::refreshArchive() // 发起不带文件参数的列表请求。
{
    beginRequest(ListRequest); // 统一入口会清理旧状态并异步连接服务端。
}

/** 验证当前选择，并从列表项自定义角色中取出下载参数。 */
void ArchiveBrowser::downloadSelected() // 下载 archiveList 当前选中项。
{
    // currentItem()返回用户当前选中的录像条目。
    QListWidgetItem *item = archiveList->currentItem(); // item 可能为空，指针不拥有对象。
    // 未选择时仅提示用户，不建立无意义的网络连接。
    if (!item) { // 没有当前选中条目时，
        statusLabel->setText(QStringLiteral("请先选择一个事件录像文件")); // 提示用户选择。
        return; // 不建立连接。
    }
    // 服务端类别、文件名和显示大小均来自受控列表响应。
    beginRequest(FileRequest, // 选择文件下载请求类型，
                 item->data(CategoryRole).toString(), // 读取隐藏类别，
                 item->data(NameRole).toString(),     // 读取服务端原文件名，
                 item->data(SizeRole).toLongLong());  // 读取预估文件大小。
}

/** 清理上一次状态、记录请求参数、锁定按钮并连接开发板。 */
void ArchiveBrowser::beginRequest(RequestKind kind, const QString &category, // kind/category 决定服务端动作与目录。
                                  const QString &name, qint64 size) // name 是文件名，size 是预估载荷长度。
{
    // 连接前去除首尾空白并拒绝空IP。
    if (deviceAddressEdit->text().trimmed().isEmpty()) { // 去空白后的目标地址不能为空。
        statusLabel->setText(QStringLiteral("请填写开发板IP")); // 给出缺少参数提示。
        return; // 不改变请求状态。
    }

    // abort立即终止可能残留的旧连接，避免不同响应字节混在一起。
    socket->abort(); // 立即结束可能残留的旧连接和待写数据。
    // 清除旧半包、临时文件和按钮状态。
    resetTransfer(); // 取消旧临时文件并清空半包缓存。
    // 保存本次请求上下文，后续connected/readyRead槽会使用这些成员。
    requestKind = kind;             // 保存本次请求类型。
    requestedCategory = category;   // 保存下载分类。
    requestedName = name;           // 保存目标文件名。
    requestedSize = size;           // 保存列表提供的预估大小。
    // 请求完成或失败前禁止重复点击形成并发状态机。
    refreshButton->setEnabled(false);  // 禁止并发列表请求。
    downloadButton->setEnabled(false); // 禁止并发文件请求。
    // 根据请求类型显示明确的当前操作。
    statusLabel->setText(kind == ListRequest // 根据请求类型选择状态文本，
        ? QStringLiteral("正在连接开发板并读取归档列表...") // 列表请求提示，
        : QStringLiteral("正在下载：%1").arg(name)); // 文件请求提示并带文件名。
    // 发起异步连接；成功后由connected信号调用sendRequest()。
    socket->connectToHost(deviceAddressEdit->text().trimmed(), // 目标开发板地址，
                          quint16(archivePortSpinBox->value())); // 目标归档服务端口。
}

/** 将当前请求上下文序列化为以换行符结尾的紧凑JSON。 */
void ArchiveBrowser::sendRequest() // TCP connected 后把当前请求上下文发送给开发板。
{
    // request保存与开发板协议一致的命令对象。
    QJsonObject request; // request 保存本次协议请求键值。
    // 列表请求发送list，文件请求发送download。
    request.insert(QStringLiteral("command"), requestKind == ListRequest // command 根据状态机决定，
                   ? QStringLiteral("list") : QStringLiteral("download")); // 分别请求列表或文件。
    // 下载请求还需要服务端白名单类别和纯文件名。
    if (requestKind == FileRequest) { // 文件请求需要两个附加定位字段。
        request.insert(QStringLiteral("category"), requestedCategory); // 写入服务端白名单类别。
        request.insert(QStringLiteral("name"), requestedName);         // 写入纯文件名。
    }
    // 换行符标记JSON请求结束，解决TCP没有消息边界的问题。
    socket->write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n'); // 发送紧凑 JSON 和协议换行边界。
}

/** 累积当前到达的TCP数据，先解析头部，再消费定长载荷。 */
void ArchiveBrowser::readResponse() // readyRead 时累积并推进响应状态机。
{
    // readAll只取本轮已经到达的字节，未完成部分保留在receiveBuffer。
    receiveBuffer.append(socket->readAll()); // 把本轮到达的所有字节追加到跨事件缓存。
    // 尚未取得完整头部时等待下一次readyRead，不能把JSON当载荷处理。
    if (!responseHeaderParsed && !parseResponseHeader()) // 头尚未完成且本次仍无法完整解析时，
        return; // 等待下一次 readyRead。
    // 头部解析完成后，缓存中可能已经同时带有一部分或全部载荷。
    consumePayload(); // 按头中 payloadSize 消费当前已有载荷。
}

/** 解析第一行JSON响应头，校验载荷长度，并为文件下载创建QSaveFile。 */
bool ArchiveBrowser::parseResponseHeader() // 解析换行前的 JSON 头并初始化载荷接收目标。
{
    // 服务端用换行符分隔响应头和后续二进制载荷。
    const int newline = receiveBuffer.indexOf('\n'); // newline 是协议头结束符在缓存中的下标，未找到为 -1。
    // 没有换行说明响应头仍未接收完整。
    if (newline < 0) { // 响应头尚未完整到达。
        // 正常JSON头远小于4 KiB，超过该值视为协议错误。
        if (receiveBuffer.size() > 4096) { // 没有换行且已超过合理头长度时判协议错误。
            statusLabel->setText(QStringLiteral("归档响应头无效")); // 显示错误。
            socket->abort();    // 立即中止异常连接。
            resetTransfer();    // 清理半包和临时文件。
        }
        return false; // 表示当前还不能进入载荷阶段。
    }

    // error接收JSON解析结果，document保存服务端响应对象。
    QJsonParseError error; // error 接收 JSON 语法解析结果。
    const QJsonDocument document = QJsonDocument::fromJson( // document 保存完整响应头文档，
        receiveBuffer.left(newline), &error); // 只解析换行符之前的字节。
    // 从缓存移除响应头和换行符，余下字节全部属于载荷。
    receiveBuffer.remove(0, newline + 1); // 删除 JSON 头及换行，剩余缓存从载荷开始。
    // 只接受格式正确的JSON对象响应。
    if (error.error != QJsonParseError::NoError || !document.isObject()) { // 必须语法正确且根节点为对象。
        statusLabel->setText(QStringLiteral("无法解析归档响应")); // 显示格式错误。
        socket->abort();    // 停止继续接收未知字节。
        resetTransfer();    // 清理请求状态。
        return false;       // 解析失败。
    }

    // header包含ok、type、payloadSize及可选name字段。
    const QJsonObject header = document.object(); // header 保存 ok、message 和 payloadSize 等字段。
    // 服务端业务校验失败时直接展示其中文错误并结束本次请求。
    if (!header.value(QStringLiteral("ok")).toBool()) { // ok=false 表示服务端拒绝业务请求。
        statusLabel->setText(QStringLiteral("开发板返回错误：%1") // 错误提示模板，
            .arg(header.value(QStringLiteral("message")).toString())); // 填入服务端 message。
        socket->disconnectFromHost(); // 正常关闭短连接。
        resetTransfer();              // 恢复 UI 和协议状态。
        return false;                 // 不进入载荷阶段。
    }

    // JSON数字转回本地64位载荷长度；缺失字段得到-1并判非法。
    payloadBytesRemaining = qint64( // 保存服务端声明的剩余定长载荷，
        header.value(QStringLiteral("payloadSize")).toDouble(-1)); // 缺失或非数值时使用 -1。
    // 禁止负长度，并限制列表JSON最大8 MiB以保护PC端内存。
    if (payloadBytesRemaining < 0 ||               // 长度不能为负；并且
            (requestKind == ListRequest &&         // 列表请求还需要
             payloadBytesRemaining > MaximumListPayload)) { // 不超过 8MiB 内存上限。
        statusLabel->setText(QStringLiteral("归档载荷长度不合法")); // 显示长度错误。
        socket->abort(); // 中止可能持续发送异常大数据的连接。
        resetTransfer(); // 清理当前状态。
        return false;    // 头校验失败。
    }

    // 文件请求需要先创建可靠的本地写入目标。
    if (requestKind == FileRequest) { // 文件载荷需要准备安全的本地写入目标。
        /* 以服务端本次实际声明的长度为准，用于显示下载进度。 */
        requestedSize = payloadBytesRemaining; // 用实际响应长度覆盖列表预估值。
        // 再次提取纯文件名，阻止服务端异常响应导致本地目录穿越。
        const QString safeName = QFileInfo(requestedName).fileName(); // safeName 去除任何目录成分。
        if (safeName != requestedName || safeName.isEmpty()) { // 原名含路径或为空时拒绝。
            statusLabel->setText(QStringLiteral("归档文件名无效")); // 显示安全校验错误。
            socket->abort(); // 停止下载。
            resetTransfer(); // 清理状态。
            return false;    // 不创建本地文件。
        }
        // 事件录像统一保存到imx6ull_media/recordings。
        const QString directory = QDir(downloadRoot()).filePath(requestedCategory); // directory 是类别子目录绝对路径。
        // 目录不存在时递归创建；失败则不能继续接收文件。
        if (!QDir().mkpath(directory)) { // 递归创建目录失败时无法继续写盘。
            statusLabel->setText(QStringLiteral("无法创建电脑下载目录")); // 提示目录错误。
            socket->abort(); // 停止接收文件。
            resetTransfer(); // 恢复可重试状态。
            return false;    // 头处理失败。
        }
        // 组合最终文件绝对路径。
        downloadedFilePath = QDir(directory).filePath(safeName); // 组合受控目录和纯文件名。
        // QSaveFile先写同目录临时文件，完成后原子替换目标文件。
        downloadFile = new QSaveFile(downloadedFilePath); // 下载期间写同目录临时文件。
        // 无写权限或磁盘问题会导致打开失败。
        if (!downloadFile->open(QIODevice::WriteOnly)) { // 无权限或磁盘错误时打开失败。
            statusLabel->setText(QStringLiteral("无法保存下载文件：%1") // 构造错误文本，
                                 .arg(downloadFile->errorString())); // 填入 QFile 原因。
            delete downloadFile;    // 释放失败对象。
            downloadFile = nullptr; // 清除悬空指针。
            socket->abort();        // 停止网络接收。
            resetTransfer();        // 恢复状态。
            return false;           // 不进入载荷阶段。
        }
    }

    // 标记状态机已经进入定长载荷阶段。
    responseHeaderParsed = true; // 状态机切换到定长载荷阶段。
    return true;                 // 头有效且下载目标已准备好。
}

/** 从接收缓存消费不超过剩余长度的数据，并更新列表缓存或下载文件。 */
void ArchiveBrowser::consumePayload() // 消费缓存中属于当前响应的载荷字节。
{
    // 只有成功解析响应头后才知道载荷目标和边界。
    if (!responseHeaderParsed) // 未知载荷长度和目标时不能消费。
        return;                // 等待头解析。
    // consumed取“剩余协议长度”和“当前缓存长度”中的较小值。
    const qint64 consumed = qMin<qint64>(payloadBytesRemaining, // consumed 不能超过协议剩余长度，
                                         receiveBuffer.size()); // 也不能超过当前实际缓存。
    // 当前可能只收到响应头而没有载荷，此时等待下一次readyRead。
    if (consumed > 0) { // 当前缓存确实含有可处理载荷。
        // 从缓存头部复制本轮可以消费的载荷块。
        const QByteArray chunk = receiveBuffer.left(int(consumed)); // chunk 复制本轮载荷块。
        // 删除已经消费的字节，保留可能存在的后续数据。
        receiveBuffer.remove(0, int(consumed)); // 从缓存移除已处理字节。
        // 列表载荷较小，完整累计后一次性解析JSON。
        if (requestKind == ListRequest) { // 列表响应需要完整 JSON 后统一解析。
            listPayload.append(chunk);    // 把块累计到列表载荷。
        // 文件载荷直接流式写盘，避免大录像全部占用内存。
        } else if (!downloadFile || downloadFile->write(chunk) != chunk.size()) { // 文件对象缺失或短写即失败。
            statusLabel->setText(QStringLiteral("电脑端写入下载文件失败")); // 显示本地写盘错误。
            socket->abort(); // 中止连接避免继续接收。
            resetTransfer(); // 取消临时文件。
            return;          // 结束本轮处理。
        }
        // 更新尚未接收的协议载荷字节数。
        payloadBytesRemaining -= consumed; // 精确减少尚未接收的协议字节数。
        // 文件下载期间根据总大小和剩余大小实时计算整数百分比。
        if (requestKind == FileRequest && requestedSize > 0) { // 文件下载且总长度有效时更新进度。
            const int percent = int((requestedSize - payloadBytesRemaining) * // 已收字节乘 100，
                                    100 / requestedSize); // 除总长度得到整数百分比。
            statusLabel->setText(QStringLiteral("正在下载：%1  %2%") // 进度文本模板，
                                 .arg(requestedName).arg(percent)); // 填入文件名和百分比。
        }
    }
    // 定长载荷恰好收完时提交结果；不依赖服务端关闭连接判断边界。
    if (payloadBytesRemaining == 0) // 定长载荷已经恰好收齐时，
        completeRequest();          // 提交并恢复空闲状态。
}

/** 根据请求类型处理完整载荷，随后恢复为空闲状态。 */
void ArchiveBrowser::completeRequest() // 完成当前归档请求；该函数没有参数和返回值。
{
    // 列表请求把完整JSON载荷转换为可交互列表。
    if (requestKind == ListRequest) { // 当前完成的是“获取归档列表”请求。
        populateArchiveList(listPayload); // 解析累计的JSON载荷并刷新列表控件。
    // 文件请求必须存在一个成功打开的QSaveFile。
    } else if (requestKind == FileRequest && downloadFile) { // 当前完成的是文件下载且临时文件对象有效。
        // commit关闭临时文件并原子替换最终文件名。
        if (!downloadFile->commit()) { // 原子提交失败时保留错误信息供用户排查。
            statusLabel->setText(QStringLiteral("下载文件提交失败：%1") // 构造提交失败提示文本，
                                 .arg(downloadFile->errorString())); // 填入QSaveFile返回的具体错误原因。
        } else { // 临时文件提交成功时进入下载完成处理。
            // 提示最终路径，并立即加载刚下载的媒体。
            statusLabel->setText(QStringLiteral("已下载到电脑：%1") // 构造下载成功提示文本，
                                 .arg(downloadedFilePath)); // 填入电脑端最终保存路径。
            displayDownloadedFile(downloadedFilePath); // 立即打开并预览刚下载的媒体文件。
        }
        // commit完成后QSaveFile对象不再使用。
        delete downloadFile; // 释放本次下载使用的QSaveFile对象。
        downloadFile = nullptr; // 清空悬空指针，表示当前没有下载文件。
    }
    // 单请求协议完成后主动结束TCP连接。
    socket->disconnectFromHost(); // 请求完成后主动关闭与开发板归档服务的连接。
    // 清空协议状态，为下一次刷新或下载做准备。
    requestKind = NoRequest; // 标记当前没有正在执行的归档请求。
    responseHeaderParsed = false; // 下一次请求需要重新解析响应头。
    receiveBuffer.clear(); // 清除TCP接收缓存中的本次协议数据。
    listPayload.clear(); // 清除列表请求累计的JSON正文。
    // 重新允许用户发起操作。
    refreshButton->setEnabled(true); // 恢复“刷新列表”按钮。
    downloadButton->setEnabled(true); // 恢复“下载所选”按钮。
}

/** 解析服务端文件数组并创建带隐藏元数据的可视列表项。 */
void ArchiveBrowser::populateArchiveList(const QByteArray &payload) // payload是服务端返回的完整JSON列表正文。
{
    // 尝试把完整列表载荷解析为JSON文档。
    QJsonParseError error; // error接收JSON解析失败的位置和错误类型。
    const QJsonDocument document = QJsonDocument::fromJson(payload, &error); // payload是服务端返回的完整JSON字节串。
    // 根节点必须为对象，其中files字段应为数组。
    if (error.error != QJsonParseError::NoError || !document.isObject()) { // JSON语法错误或根节点不是对象时拒绝使用。
        statusLabel->setText(QStringLiteral("归档列表数据无效")); // 在界面上报告协议数据无效。
        return; // 终止列表更新，保留现有程序状态。
    }

    // 刷新采用全量替换，先移除旧条目避免重复显示。
    archiveList->clear(); // 删除控件中上一次刷新得到的所有列表项。
    // 读取根对象中的files数组；字段缺失时得到空数组。
    const QJsonArray files = document.object() // 取得JSON根对象，
            .value(QStringLiteral("files")).toArray(); // 再取files字段并转换成文件数组。
    // 将每个服务端文件对象转换成一个QListWidgetItem。
    for (const QJsonValue &value : files) { // 逐个处理服务端返回的归档文件描述对象。
        // file保存当前条目的协议字段。
        const QJsonObject file = value.toObject(); // file保存当前数组元素的对象字段。
        const QString category = file.value(QStringLiteral("category")).toString(); // category是服务端使用的归档分类名。
        const QString name = file.value(QStringLiteral("name")).toString(); // name是下载请求需要携带的原始文件名。
        const qint64 size = qint64(file.value(QStringLiteral("size")).toDouble()); // size是文件总字节数，使用64位整数保存。
        // ISODateWithMs与开发板生成格式一致，保留毫秒解析能力。
        const QDateTime modified = QDateTime::fromString( // modified保存解析后的文件修改时间，
            file.value(QStringLiteral("modified")).toString(), Qt::ISODateWithMs); // 输入采用带毫秒的ISO日期格式。
        // 第一行显示类型和文件名，第二行显示大小和修改时间。
        QListWidgetItem *item = new QListWidgetItem( // item是将加入归档列表的新条目，所有权随后交给archiveList。
            QStringLiteral("[录像] %1\n%2  %3") // 两行显示模板：名称、大小和修改时间。
                .arg(name, readableSize(size), // 第一个和第二个占位符分别填文件名、易读大小，
                     modified.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")))); // 第三个占位符填本地显示格式的时间。
        // 自定义角色保存请求下载所需的原始数据，不从显示文本反向解析。
        item->setData(CategoryRole, category); // 保存分类，供下载请求直接读取。
        item->setData(NameRole, name); // 保存原始文件名，避免从显示文本中截取。
        item->setData(SizeRole, size); // 保存文件大小，供下载进度计算使用。
        archiveList->addItem(item); // 把条目加入列表并将其所有权交给列表控件。
    }
    // 根据数组是否为空给出清晰的最终刷新结果。
    statusLabel->setText(files.isEmpty() // 根据files数组是否为空选择最终状态文本，
        ? QStringLiteral("开发板归档目录目前为空") // 空数组表示服务端当前没有归档文件，
        : QStringLiteral("已读取 %1 个文件；双击即可下载并查看") // 非空时提示文件数量和操作方式，
              .arg(files.size())); // 用数组元素数替换%1占位符。
}

/** 计算跨桌面环境可写的统一下载根目录。 */
QString ArchiveBrowser::downloadRoot() const // 返回电脑端归档文件的统一下载目录，且不修改对象状态。
{
    // 优先使用系统配置的Downloads目录，而不是硬编码/home/用户名。
    QString root = QStandardPaths::writableLocation( // root保存当前用户可写的下载目录，
        QStandardPaths::DownloadLocation); // 请求操作系统标准“下载”位置。
    // 极简桌面环境可能没有配置下载目录，此时回退到用户主目录。
    if (root.isEmpty()) // 系统没有配置标准下载目录时，
        root = QDir::homePath(); // 使用当前用户主目录作为安全回退路径。
    // 所有项目媒体集中放入imx6ull_media子目录。
    return QDir(root).filePath(QStringLiteral("imx6ull_media")); // 返回下载目录下的项目专用子目录路径。
}

/** 停止旧预览，再按归档类别显示图片或播放录像。 */
void ArchiveBrowser::displayDownloadedFile(const QString &filePath) // filePath是刚下载到电脑的媒体文件完整路径。
{
    // 切换媒体前停止定时器，避免它继续读取旧文件。
    mjpegPlaybackTimer->stop(); // 停止旧MJPEG文件的逐帧播放定时器。
    // 若旧录像仍打开，先释放文件描述符。
    if (mjpegFile->isOpen()) // 旧MJPEG文件仍保持打开时，
        mjpegFile->close(); // 关闭它并释放文件描述符。
    h264Player->stop(); // 同时停止可能正在播放的H.264媒体。
    // 清除上一个MJPEG尚未组成完整JPEG的残留字节。
    mjpegBuffer.clear(); // 丢弃属于上一个MJPEG文件的未完整帧字节。

    // 新版事件录像是 H.264 Annex B 裸码流，交给 Qt Multimedia 解码。
    if (QFileInfo(filePath).suffix().compare( // 读取filePath的扩展名并执行不区分大小写比较，
            QStringLiteral("h264"), Qt::CaseInsensitive) == 0) { // 扩展名为h264时走视频播放器分支。
        currentPreviewImage = QImage(); // 清空静态图缓存，避免窗口缩放时覆盖视频页面。
        previewStack->setCurrentWidget(h264VideoOutput); // 切换堆叠控件到视频输出页面。
        h264Player->setMedia(QMediaContent(QUrl::fromLocalFile(filePath))); // 把本地H.264文件路径设置为播放器媒体源。
        h264Player->play(); // 启动Qt Multimedia解码和播放。
        return; // H.264分支完成，不再执行下方MJPEG读取逻辑。
    }

    // 旧版 .mjpeg/.mjpg 归档仍按连续 JPEG 录像处理。
    previewStack->setCurrentWidget(previewLabel); // 非H.264文件使用QLabel显示逐帧解码结果。
    mjpegFile->setFileName(filePath); // 设置将要读取的旧版MJPEG归档路径。
    // 本地文件无法打开时显示具体QFile错误。
    if (!mjpegFile->open(QIODevice::ReadOnly)) { // 以只读方式打开失败时不能播放。
        previewLabel->setText(QStringLiteral("录像打开失败：%1") // 构造文件打开失败提示，
                              .arg(mjpegFile->errorString())); // 填入QFile返回的具体错误文本。
        return; // 终止当前文件的预览初始化。
    }
    // 立即显示第一帧，避免等待一个定时周期后预览才有画面。
    showNextMjpegFrame(); // 同步读取并显示MJPEG第一帧。
    // 后续每66ms继续读取一帧。
    mjpegPlaybackTimer->start(); // 按构造函数设置的周期继续播放后续帧。
}

/** 从MJPEG连续字节中寻找FFD8起始标记和FFD9结束标记并解码。 */
bool ArchiveBrowser::readOneMjpegFrame(QImage *image) // image接收解码帧；返回值表示是否成功得到一帧。
{
    // 调用者必须提供输出对象，并且录像文件必须已打开。
    if (!image || !mjpegFile->isOpen()) // image为空或文件未打开时无法输出帧。
        return false; // 用false通知调用者本次读取失败。

    // restarted防止空文件或损坏文件在循环播放逻辑中无限seek。
    bool restarted = false; // 记录本次调用是否已经从文件头回绕过一次。
    while (true) { // 持续读块和搜索边界，直到得到一帧或确认失败。
        // JPEG SOI标记FFD8确定一帧起点。
        int start = mjpegBuffer.indexOf(QByteArray("\xff\xd8", 2)); // start是JPEG起始标记在缓存中的偏移，未找到为-1。
        // 丢弃SOI之前不属于JPEG帧的噪声或残留字节。
        if (start > 0) // 起始标记前存在无效字节时，
            mjpegBuffer.remove(0, start); // 删除这些字节，使缓存从FFD8开始。
        // 尚未找到SOI时只保留最后一个字节，以处理标记跨读取块边界的情况。
        if (start < 0 && mjpegBuffer.size() > 1) // 当前缓存完全没有FFD8且长度超过1时，
            mjpegBuffer = mjpegBuffer.right(1); // 仅保留尾字节，以防它是跨块FFD8中的FF。

        // 从SOI之后搜索JPEG EOI结束标记FFD9。
        const int end = mjpegBuffer.indexOf(QByteArray("\xff\xd9", 2), 2); // end是从SOI之后找到的EOI标记偏移。
        // 找到完整边界后取出单帧并从缓存删除。
        if (end >= 0) { // 找到结束标记说明缓存中至少有一幅完整JPEG。
            const QByteArray jpeg = mjpegBuffer.left(end + 2); // jpeg包含从FFD8到FFD9的完整帧字节。
            mjpegBuffer.remove(0, end + 2); // 从缓存移除已消费帧，留下后续帧数据。
            // 让Qt JPEG插件把字节解码到调用者提供的QImage。
            return image->loadFromData(jpeg, "JPEG"); // 解码到输出参数image，并把解码是否成功返回调用者。
        }

        // 当前缓存不足一帧时继续从文件读取64 KiB。
        const QByteArray chunk = mjpegFile->read(64 * 1024); // chunk保存本次从文件读取的最多64 KiB新数据。
        // 有新字节则追加后重新搜索边界。
        if (!chunk.isEmpty()) { // 文件仍有可读取数据时，
            mjpegBuffer.append(chunk); // 追加到跨读取操作保留的组帧缓存。
            continue; // 回到循环顶部重新搜索完整JPEG边界。
        }
        // 到文件末尾后只回绕一次，实现循环播放且避免损坏文件死循环。
        if (restarted || !mjpegFile->seek(0)) // 已回绕过一次仍无帧，或无法定位到文件头时，
            return false; // 判定文件没有可解码帧并结束读取。
        // 从文件头重新读取前清除尾部残留。
        mjpegBuffer.clear(); // 清除文件尾部遗留的不完整JPEG数据。
        restarted = true; // 标记已回绕，防止空文件导致无限循环。
    }
}

/** 定时器回调：读取成功则显示，读取失败则停止播放并提示。 */
void ArchiveBrowser::showNextMjpegFrame() // 读取并显示下一幅MJPEG图像帧。
{
    // frame接收本次解码的JPEG图像。
    QImage frame; // frame是本次从MJPEG文件解码得到的输出图像。
    if (readOneMjpegFrame(&frame)) // 成功读取并解码一帧时，
        displayImage(frame); // 将这一帧缩放后显示在预览标签中。
    else { // 没有读到有效帧时进入播放失败处理。
        // 无法解码时停止重复尝试，避免持续占用CPU。
        mjpegPlaybackTimer->stop(); // 停止后续定时回调，避免反复失败。
        previewLabel->setText(QStringLiteral("录像中没有可解码的JPEG帧")); // 向用户说明停止播放的原因。
    }
}

/** 缓存原图并按预览控件当前尺寸保持宽高比缩放。 */
void ArchiveBrowser::displayImage(const QImage &image) // image是需要缩放并显示的原始图像。
{
    // 保存未缩放图像，窗口变化时可以从原图重新生成而不累计失真。
    currentPreviewImage = image; // 保存函数参数中的原始图像，供窗口缩放时再次使用。
    previewStack->setCurrentWidget(previewLabel); // 确保当前显示静态图像页面而不是视频输出页面。
    // SmoothTransformation用于PC端高质量预览，性能压力远低于开发板。
    previewLabel->setPixmap(QPixmap::fromImage(image).scaled( // 将QImage转换为QLabel可显示的QPixmap并缩放，
        previewLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)); // 目标为标签大小，同时保持比例并使用平滑算法。
}

/** 取消未完成传输并恢复UI；不会清除已经成功显示的预览。 */
void ArchiveBrowser::resetTransfer() // 清理未完成传输的缓存、文件和界面状态。
{
    // 回到尚未解析新响应头的初始状态。
    responseHeaderParsed = false; // 标记下一批网络数据尚未解析响应头。
    payloadBytesRemaining = 0; // 清零尚待接收的正文长度。
    receiveBuffer.clear(); // 丢弃本次失败请求遗留的TCP字节。
    listPayload.clear(); // 丢弃未完成的JSON列表正文。
    // 存在临时下载时取消写入，QSaveFile会删除未提交的临时内容。
    if (downloadFile) { // 存在未完成下载使用的临时文件对象时，
        downloadFile->cancelWriting(); // 取消原子写入并删除临时文件内容。
        delete downloadFile; // 释放QSaveFile对象占用的资源。
        downloadFile = nullptr; // 清空指针，避免后续误用已释放对象。
    }
    // 清除本次未成功提交的最终路径记录。
    downloadedFilePath.clear(); // 清除未完成请求对应的目标文件路径。
    // 允许用户重新发起请求。
    refreshButton->setEnabled(true); // 允许用户重新刷新归档列表。
    downloadButton->setEnabled(true); // 允许用户重新发起文件下载。
}

/** 处理连接拒绝、超时等网络错误，并避免空闲期断开产生误报。 */
void ArchiveBrowser::handleSocketError() // 响应当前归档TCP连接产生的错误信号。
{
    // 正常disconnectFromHost后也可能出现套接字状态变化，空闲时无需提示。
    if (requestKind == NoRequest) // 当前没有业务请求时，套接字变化无需作为错误提示。
        return; // 忽略空闲连接产生的信号。
    // 把Qt底层错误文本与业务提示组合显示。
    statusLabel->setText(QStringLiteral("无法访问开发板归档服务：%1") // 构造网络访问失败提示，
                         .arg(socket->errorString())); // 填入QTcpSocket提供的底层错误说明。
    requestKind = NoRequest; // 结束当前请求状态，避免继续解析残留数据。
    resetTransfer(); // 取消临时文件并恢复按钮和协议缓存。
}

/** 确保下载根目录存在，并交给操作系统默认文件管理器打开。 */
void ArchiveBrowser::openDownloadDirectory() // 创建并打开电脑端归档下载目录。
{
    // 即使尚未下载过文件，也先创建目录供用户查看。
    QDir().mkpath(downloadRoot()); // 递归创建项目下载目录；目录已存在时也视为成功。
    QDesktopServices::openUrl(QUrl::fromLocalFile(downloadRoot())); // 请求系统文件管理器打开该本地目录。
}

/** QWidget尺寸变化后使用原始缓存图重新计算预览缩放。 */
void ArchiveBrowser::resizeEvent(QResizeEvent *event) // event描述本次窗口尺寸变化。
{
    // 先让QWidget基类完成布局和默认尺寸事件处理。
    QWidget::resizeEvent(event); // 把event交给基类，保证标准布局更新正常执行。
    // 没有已显示媒体时无需执行缩放。
    if (previewStack->currentWidget() == previewLabel && // 当前正在显示静态图片页面，并且
        !currentPreviewImage.isNull()) // 已缓存一幅有效的原始预览图时，
        displayImage(currentPreviewImage); // 按标签的新尺寸重新缩放并显示。
}
