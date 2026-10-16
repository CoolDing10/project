/*
 * media_archive_server.cpp — 事件录像列表与下载服务实现
 *
 * 这是一个运行在独立工作线程中的单客户端 TCP 状态机。每条连接只处理
 * 一条 list 或 download 请求，响应完成后主动断开，从而避免多个请求的
 * JSON 头和二进制文件载荷在同一字节流中相互混淆。
 */
#include "media_archive_server.h" // 引入 MediaArchiveServer 类声明和成员定义。

#include <QCoreApplication> // 获取应用程序目录，用于定位 data/recordings。
#include <QDateTime>        // 把录像修改时间转换为带毫秒的 ISO 8601 文本。
#include <QDebug>           // 输出服务状态、请求错误和传输错误日志。
#include <QDir>             // 扫描录像目录并进行规范化路径拼接。
#include <QFile>            // 以固定大小分块读取事件录像文件。
#include <QFileInfo>        // 检查文件名、类型、权限、大小和修改时间。
#include <QHostAddress>     // 使用 AnyIPv4 监听开发板所有 IPv4 网卡。
#include <QJsonArray>       // 把多条录像元数据组织成 JSON 数组。
#include <QJsonDocument>    // 在 QByteArray 与 JSON 文档之间解析和序列化。
#include <QJsonObject>      // 表示协议请求、响应头和单个录像元数据。
#include <QTcpServer>       // 监听 8890 端口并接受 PC 端连接。
#include <QTcpSocket>       // 接收请求并异步发送列表或录像文件。

namespace // 匿名命名空间限制协议常量只在本实现文件中可见。
{
const int MaximumRequestBytes = 4096;          // 单条 JSON 请求最多 4 KiB，防止异常输入耗尽内存。
const qint64 TransferChunkBytes = 64 * 1024;   // 每次从录像文件读取 64 KiB，平衡内存和系统调用次数。
const qint64 MaximumQueuedBytes = 512 * 1024;  // Qt 待发送缓存最多积压 512 KiB，超过后暂停读取文件。
} // 结束当前实现文件专用常量区域。

/**
 * 构造阶段仅初始化状态；QTcpServer和QFile必须在initialize()所在线程创建，
 * 这样它们的线程归属与archiveWorkerThread一致。
 * @param parent Qt父对象；项目传 nullptr，随后把本对象移动到归档线程。
 */
MediaArchiveServer::MediaArchiveServer(QObject *parent)
    : QObject(parent),                    // 把Qt父对象交给QObject基类管理。
      server(nullptr),                    // 尚未创建端口监听器。
      client(nullptr),                    // 启动时没有PC客户端连接。
      transferFile(nullptr),              // 启动时没有正在读取的文件。
      transferBytesRemaining(0),          // 剩余发送字节初始为0。
      transferQueuedCompletely(false)     // 尚不存在已完整排队的文件传输。
{
} // 构造阶段不做网络和文件 I/O。

/**
 * 初始化归档网络服务；重复调用时直接返回，避免重复监听同一端口。
 * @param port 要监听的 TCP 端口，项目默认传入 8890。
 */
void MediaArchiveServer::initialize(quint16 port)
{
    // server非空说明初始化已经执行过，即使listen失败也不重复创建对象。
    if (server) // server 非空代表 initialize() 已经执行过。
        return; // 不重复创建监听器，也不重复绑定同一个端口。

    // 子对象使用this作为父对象，归档服务析构时由Qt自动回收。
    server = new QTcpServer(this); // server 随本对象析构，且线程归属与本对象相同。
    // QFile复用于每一次下载，避免每个请求重复分配文件对象。
    transferFile = new QFile(this); // 文件对象随服务析构，并在各次下载间复用。
    // 监听器发现新连接时，转入acceptConnection()完成套接字配置。
    connect(server, &QTcpServer::newConnection,          // 发送者及其“有新连接”信号。
            this, &MediaArchiveServer::acceptConnection); // 接收者及处理连接的槽函数。

    // AnyIPv4允许PC通过开发板任意IPv4网卡访问8890端口。
    if (!server->listen(QHostAddress::AnyIPv4, port)) // 尝试在全部 IPv4 网卡上绑定指定端口。
    {
        // 拼接Qt提供的具体错误，例如端口占用或权限不足。
        const QString message = QStringLiteral("媒体归档服务监听失败：%1") // 错误文本模板。
                .arg(server->errorString()); // %1 替换为 QTcpServer 返回的具体错误。
        // 保留终端诊断输出，便于开发板现场排查。
        qWarning() << message; // 写入开发板控制台日志。
        // 同时把失败状态通知上层UI。
        emit statusChanged(message); // 跨线程通知边缘监控页面更新服务状态。
        return; // 监听失败，本次初始化到此结束。
    }

    // 监听成功后同时输出绑定地址和实际端口。
    const QString message = QStringLiteral("媒体归档服务已监听 0.0.0.0:%1") // 成功状态模板。
            .arg(port); // 把实际监听端口替换到 %1。
    qInfo() << message; // 记录本地服务启动日志。
    emit statusChanged(message); // 把成功状态送回页面线程。
}

/** 停止服务时先终止活动传输，再关闭监听器，确保文件句柄不会泄漏。 */
void MediaArchiveServer::stop()
{
    // 复用统一清理函数关闭套接字、文件及协议状态。
    closeClient(); // 终止当前请求并释放当前客户端。
    // server可能尚未initialize，因此关闭前必须判空。
    if (server) // initialize() 可能尚未执行，因此先判空。
        server->close(); // 停止接受新连接，但保留 QObject 由父对象回收。
    // 保留明确的停止日志，区分主动退出和网络异常。
    qInfo() << "媒体归档服务已停止"; // 输出主动停止日志。
}

/**
 * 取出Qt监听队列里的全部连接。协议当前只支持单客户端，因此保留最后一个
 * 连接，并主动清理旧连接，避免卡死客户端长期占用服务。
 */
void MediaArchiveServer::acceptConnection()
{
    // hasPendingConnections()可能一次报告多个排队连接，所以使用while取完。
    while (server && server->hasPendingConnections()) // 逐一取完系统监听队列中的连接。
    {
        // incoming的所有权稍后由deleteLater()安全释放。
        QTcpSocket *incoming = server->nextPendingConnection(); // incoming 是本轮取出的新连接。
        /* 当前协议一次只服务一个请求；新连接会替换已失效/卡住的旧连接。 */
        closeClient(); // 清理上一个客户端、文件和半包状态。
        // 将最新连接记录为当前协议连接。
        client = incoming; // 只保留最后取出的连接作为当前服务对象。
    }
    // 防御nextPendingConnection()异常返回空指针的情况。
    if (!client) // 防御 Qt 异常返回空连接的情况。
        return; // 没有有效套接字就不能继续配置信号。

    // 启用TCP KeepAlive，使系统能够发现长时间失效的对端。
    client->setSocketOption(QAbstractSocket::KeepAliveOption, 1); // 1 表示启用 TCP KeepAlive。
    // 收到请求字节时进入readRequest()累积并按换行符拆包。
    connect(client, &QTcpSocket::readyRead,            // 客户端有新请求字节到达。
            this, &MediaArchiveServer::readRequest);   // 调用 readRequest() 累积并拆行。
    // 内核发送走一部分数据后继续填充文件块，实现非阻塞流式下载。
    connect(client, &QTcpSocket::bytesWritten,         // Qt 每实际写出一批网络字节就发出信号。
            this, [this](qint64 /* 本次写出的字节数无需单独使用 */)
            { continueFileTransfer(); });              // 缓冲区出现空间后继续排入录像数据。
    // 对端关闭时释放套接字并复位传输状态。
    connect(client, &QTcpSocket::disconnected,             // 对端或本端完成 TCP 断开。
            this, &MediaArchiveServer::clientDisconnected); // 延迟释放套接字并复位状态。
    // 新连接不能继承上一个连接残留的半包请求。
    requestBuffer.clear(); // 清空旧连接没有组成完整请求的残留字节。
    // 新连接开始时不存在活动文件载荷。
    transferBytesRemaining = 0; // 新连接尚未选择下载文件。
    transferQueuedCompletely = false; // 新连接也没有已排队完成的载荷。
}

/** 读取TCP字节流，并以换行符作为单行JSON请求的结束标记。 */
void MediaArchiveServer::readRequest()
{
    // 下载进行期间不允许同一连接再发第二条请求，保持协议状态机简单可靠。
    if (!client || transferBytesRemaining > 0 || transferQueuedCompletely) // 必须有客户端且没有进行中的响应。
        return; // 单连接只允许处理一条请求，下载期间拒绝读取第二条命令。

    // readAll()只取得当前已到达字节；TCP拆包时需要继续等待后续readyRead。
    requestBuffer.append(client->readAll()); // 把本次到达字节追加到跨 readyRead 保存的缓存。
    // 请求超过4 KiB通常表示客户端异常或没有发送换行符。
    if (requestBuffer.size() > MaximumRequestBytes) // 未完成请求已经超过协议上限。
    {
        sendError(QStringLiteral("请求过大")); // 返回错误并准备断开异常客户端。
        return; // 不再继续寻找换行符或解析 JSON。
    }

    // 搜索一条请求的协议结束符。
    const int newline = requestBuffer.indexOf('\n'); // 保存第一个换行符在缓存中的下标。
    // 尚未收齐一行时保留缓存，等待下次readyRead。
    if (newline < 0) // -1 代表请求结束符还没有随 TCP 字节流到达。
        return; // 保留 requestBuffer，等待下一次 readyRead 继续追加。

    // 截取第一行并移除首尾空白，随后交给JSON解析器。
    const QByteArray requestLine = requestBuffer.left(newline).trimmed(); // 取换行符前内容并去掉首尾空白。
    // 单连接只处理一个请求，因此清空其余缓存。
    requestBuffer.clear(); // 单请求连接不保留换行符之后的额外数据。
    processRequest(requestLine); // 把完整请求交给 JSON 协议分发函数。
}

/**
 * 解析请求对象，并根据 command 字段分发列表或下载操作。
 * @param requestLine 不含换行符的一条 JSON 请求字节。
 */
void MediaArchiveServer::processRequest(const QByteArray &requestLine)
{
    // error保存解析失败的类型和偏移，document保存解析结果。
    QJsonParseError error; // 接收 JSON 解析错误码和发生位置。
    const QJsonDocument document = QJsonDocument::fromJson( // 把 UTF-8 请求解析成 JSON 文档。
        requestLine, &error); // requestLine 是输入，error 是解析结果输出参数。
    // 协议只接受JSON对象，不接受数组、数字或损坏的JSON。
    if (error.error != QJsonParseError::NoError || !document.isObject()) // 要求语法正确且顶层为对象。
    {
        sendError(QStringLiteral("请求JSON无效")); // 返回统一协议错误。
        return; // 无效文档不能继续访问 command 字段。
    }

    // 提取请求对象以及命令名称。
    const QJsonObject request = document.object(); // 取得顶层请求对象的只读副本。
    const QString command = request.value(QStringLiteral("command")).toString(); // 提取命令字符串。
    // list返回全部媒体元数据，不携带文件内容。
    if (command == QLatin1String("list")) // list 请求只查询元数据。
    {
        sendArchiveList(); // 扫描目录并返回 JSON 文件清单。
    // download要求客户端同时提供类别和纯文件名。
    }
    else if (command == QLatin1String("download")) // download 请求需要类别和文件名。
    {
        startFileTransfer(
            request.value(QStringLiteral("category")).toString(), // 第1参数：受白名单约束的类别。
            request.value(QStringLiteral("name")).toString());    // 第2参数：不含目录的文件名。
    // 未知命令必须显式返回错误，不能静默断开。
    }
    else // command 缺失或不是 list/download。
    {
        sendError(QStringLiteral("不支持的归档命令")); // 明确告诉客户端命令不受支持。
    }
}

/**
 * 只把白名单类别映射到应用程序目录下的数据目录。
 * @param category 客户端请求的归档类别。
 * @return recordings 的实际目录；非法类别返回空 QString。
 */
QString MediaArchiveServer::archiveDirectory(const QString &category) const
{
    // 白名单限制是后续路径安全校验的第一层。
    if (category != QLatin1String("recordings")) // 当前只允许访问事件录像类别。
        return QString(); // 空路径通知调用者类别不合法。
    // 数据目录与可执行文件并列，符合开发板/opt/project/bin部署结构。
    return QDir(QCoreApplication::applicationDirPath()) // 以程序所在目录为路径根。
            .filePath(QStringLiteral("data/%1").arg(category)); // 拼成 data/recordings。
}

/** 扫描事件录像并生成紧凑JSON列表响应。 */
void MediaArchiveServer::sendArchiveList()
{
    // files最终包含所有可下载文件的类别、名称、大小和修改时间。
    QJsonArray files; // 最终作为响应对象 files 字段的数组值。
    const QString category = QStringLiteral("recordings"); // 所有条目的合法业务类别固定为 recordings。
    const QDir directory(archiveDirectory(category)); // 构造 data/recordings 目录访问对象。
    // 新录像统一为 H.264；保留旧 MJPEG 文件的下载兼容性。
    const QStringList filters = {QStringLiteral("*.h264"), // 当前统一产生的 H.264 裸流录像。
                                 QStringLiteral("*.mjpeg"), // 兼容以前生成的 .mjpeg 录像。
                                 QStringLiteral("*.mjpg")}; // 兼容另一种旧 MJPEG 扩展名。
    // 仅列出可读普通文件，并按照修改时间从新到旧排列。
    const QFileInfoList entries = directory.entryInfoList( // 获取满足筛选条件的文件信息数组。
        filters,                                           // 只匹配上面三种录像扩展名。
        QDir::Files | QDir::Readable,                       // 排除目录和不可读文件。
        QDir::Time);                                       // 按修改时间从新到旧排列。
    // 把每个QFileInfo转换成与PC端约定的JSON对象。
    for (const QFileInfo &entry : entries) // entry 是当前遍历到的一段录像元数据。
    {
        QJsonObject item; // 为这一段录像创建独立 JSON 对象。
        item.insert(QStringLiteral("category"), category); // 告诉客户端下载时应回传 recordings。
        item.insert(QStringLiteral("name"), entry.fileName()); // 只传纯文件名，不暴露开发板绝对路径。
        // JSON数字使用double；对本项目媒体文件大小仍能保持整数精度。
        item.insert(QStringLiteral("size"), double(entry.size())); // 保存文件字节数。
        item.insert(QStringLiteral("modified"),               // 保存文件最后修改时间字段。
                    entry.lastModified().toString(             // 从 QFileInfo 取得时间并转成文本。
                        Qt::ISODateWithMs));                    // 使用带毫秒的标准 ISO 8601 格式。
        files.append(item); // 将当前录像对象加入响应数组。
    }

    // 使用根对象包装数组，为以后增加设备ID等字段保留扩展空间。
    QJsonObject payloadObject; // 顶层响应载荷对象。
    payloadObject.insert(QStringLiteral("files"), files); // 把全部录像条目放进 files 字段。
    // Compact格式减少局域网传输字节和PC端等待时间。
    const QByteArray payload = QJsonDocument(payloadObject) // 把 JSON 对象包装为可序列化文档。
            .toJson(QJsonDocument::Compact); // 序列化为无缩进 UTF-8 字节，减少网络流量。
    // 必须先发带长度的响应头，接收端才能正确切分TCP字节流。
    sendResponseHeader(QStringLiteral("list"), payload.size()); // 声明响应类型和后续 JSON 精确长度。
    // 连接可能在扫描目录期间被对端关闭，因此写入前再次判空。
    if (client) // 扫描目录期间客户端可能已经断开，所以再次检查指针。
    {
        client->write(payload); // 将列表 JSON 交给 Qt 的异步 TCP 发送缓冲区。
        // 单请求单连接：载荷写入Qt缓冲区后进入优雅断开状态。
        client->disconnectFromHost(); // 等待缓冲数据发完后执行正常 TCP 断开。
    }
    qInfo() << "媒体归档列表已发送，文件数=" << files.size(); // 输出本次返回的录像数量。
}

/**
 * 校验文件类别和名称，打开文件并固定本次响应的载荷长度。
 * @param category 客户端传回的类别，必须严格等于 recordings。
 * @param name 客户端选择的纯文件名，不允许目录分隔符或路径跳转。
 */
void MediaArchiveServer::startFileTransfer(const QString &category,
                                            const QString &name)
{
    // category只有通过白名单时才会得到非空目录。
    const QString directoryPath = archiveDirectory(category); // 白名单通过时得到 data/recordings 路径。
    /* 只接受纯文件名，禁止 ../ 或绝对路径访问归档目录以外的文件。 */
    if (directoryPath.isEmpty() ||                         // 类别不在白名单中。
        name.isEmpty() ||                                  // 文件名不能为空。
        QFileInfo(name).fileName() != name)                // 清理路径后发生变化，说明包含目录成分。
    {
        sendError(QStringLiteral("归档文件参数无效")); // 返回参数校验错误。
        return; // 不允许任何未验证路径继续接触文件系统。
    }

    // fileInfo表示白名单目录与客户端文件名拼接后的候选文件。
    const QFileInfo fileInfo(                              // 保存候选文件的属性和路径信息。
        QDir(directoryPath).filePath(name));               // 只在白名单目录下拼接纯文件名。
    // canonicalPath消除符号链接和../，用于最终目录边界校验。
    const QString canonicalDirectory =                     // 保存录像根目录的真实规范路径。
        QDir(directoryPath).canonicalPath();               // 解析符号链接，并消除 . 和 ..。
    const QString canonicalFile = fileInfo.canonicalFilePath(); // 保存候选文件的真实规范路径。
    // 同时检查存在性、类型、权限以及规范路径是否仍位于归档目录内。
    if (!fileInfo.exists() ||                              // 文件必须真实存在。
        !fileInfo.isFile() ||                              // 不能下载目录或特殊文件。
        !fileInfo.isReadable() ||                          // 当前进程必须拥有读权限。
        canonicalDirectory.isEmpty() ||                    // 录像根目录必须能解析成真实路径。
        !canonicalFile.startsWith(                         // 文件真实路径必须仍在录像根目录内。
            canonicalDirectory + QDir::separator()))       // 加目录分隔符避免相似前缀绕过检查。
    {
        sendError(QStringLiteral("归档文件不存在或不可读")); // 返回统一文件访问错误。
        return; // 阻止不存在文件、符号链接逃逸和目录穿越。
    }

    // 复用QFile对象并指向已经通过安全检查的规范路径。
    transferFile->setFileName(canonicalFile); // 使用通过安全检查的真实路径，而不是原始输入。
    // 以只读方式打开，服务端不会修改开发板上的媒体文件。
    if (!transferFile->open(QIODevice::ReadOnly)) // 只读打开，绝不改写录像内容。
    {
        sendError(QStringLiteral("归档文件打开失败：%1") // 构造文件打开失败消息。
                  .arg(transferFile->errorString()));       // 附加 QFile 提供的系统错误。
        return; // 文件句柄不可用，不能发送响应载荷。
    }

    /* 固定本次传输长度；即使正在录像的文件继续增长也不破坏协议边界。 */
    transferBytesRemaining = fileInfo.size(); // 固定本次响应声明的文件字节数。
    // 空文件没有数据块需要排队，可在响应头发送完成后直接断开。
    transferQueuedCompletely = transferBytesRemaining == 0; // 空文件无需执行读取循环。
    if (transferQueuedCompletely) // 空文件已经等价于完整载荷排队完成。
        transferFile->close(); // 立即释放不再使用的文件描述符。
    // 响应头携带固定长度和文件名，PC端据此创建下载任务。
    sendResponseHeader(QStringLiteral("file"),             // 响应类型为文件下载。
                       transferBytesRemaining,              // 声明接下来需要读取的固定字节数。
                       name);                               // 把安全的原始文件名返回给 PC。
    qInfo() << "开始发送媒体归档：" << canonicalFile      // 输出实际发送文件路径。
            << "字节=" << transferBytesRemaining;         // 输出固定传输长度。
    // 主动发送第一批文件数据，后续批次由bytesWritten信号驱动。
    continueFileTransfer(); // 不等待 bytesWritten，主动把第一批数据排入套接字。
}

/**
 * 使用QTcpSocket写缓冲区水位实现背压：缓冲未满时连续读取64 KiB文件块，
 * 达到512 KiB后返回事件循环，待网络实际发送后再由bytesWritten继续。
 */
void MediaArchiveServer::continueFileTransfer()
{
    // 没有活动连接时不能继续访问套接字。
    if (!client) // client 为空表示连接已断开或从未建立。
        return; // 不能访问空套接字。
    // 文件已全部交给Qt后，只需等待发送缓冲区清空再优雅断开。
    if (transferQueuedCompletely) // 文件载荷已经全部交给 QTcpSocket。
    {
        if (client->bytesToWrite() == 0) // Qt 发送缓存也已经清空。
            client->disconnectFromHost(); // 所有数据已发出，可以正常断开。
        return; // 无论缓存是否清空，都不再读取文件。
    }
    // 非空传输必须有一个已打开的只读文件。
    if (!transferFile || !transferFile->isOpen()) // 防止初始化失败或文件被提前关闭。
        return; // 没有有效文件时无法生成下一数据块。

    // 只要仍有数据且网络缓冲未超过高水位，就继续排入文件块。
    while (transferBytesRemaining > 0 &&                 // 协议声明的载荷仍有字节尚未排队。
           client->bytesToWrite() < MaximumQueuedBytes)  // Qt TCP 缓冲尚未达到 512 KiB 高水位。
    {
        // 最后一块可能小于64 KiB，不能读取超过协议声明的固定长度。
        const qint64 wanted = qMin(                        // 计算本轮最多应读取多少字节。
            TransferChunkBytes,                            // 普通块最大为 64 KiB。
            transferBytesRemaining);                      // 最后一块不能超过剩余协议长度。
        const QByteArray chunk = transferFile->read(wanted); // 从当前文件位置读取一个数据块。
        // 预期仍有数据时读到空块，说明文件读取发生异常或文件被截断。
        if (chunk.isEmpty()) // 剩余长度大于零时不应读到 EOF 或空数据。
        {
            qWarning() << "媒体归档读取中断：" << transferFile->fileName(); // 记录异常文件。
            client->abort(); // 立即断开，避免 PC 把不完整载荷误认为完整文件。
            return; // 停止本轮传输。
        }
        // write()把数据交给Qt发送缓冲区，并不会阻塞等待对端接收。
        const qint64 accepted = client->write(chunk); // 返回被 Qt 发送缓存接受的字节数。
        // 对QTcpSocket而言正常写入应接受整个QByteArray；否则终止损坏传输。
        if (accepted != chunk.size()) // 正常情况下 QByteArray 应被完整排入缓冲。
        {
            qWarning() << "媒体归档网络写入失败：" << client->errorString(); // 输出套接字错误。
            client->abort(); // 终止已经无法保证长度边界的响应。
            return; // 不再读取或排入后续文件块。
        }
        // 从尚未排队的协议载荷长度中扣除本次已接受字节。
        transferBytesRemaining -= accepted; // 更新还没有交给套接字的载荷字节数。
    }

    // 剩余长度归零表示完整载荷已经进入Qt网络发送队列。
    if (transferBytesRemaining == 0) // 所有固定长度文件数据都已成功排队。
    {
        transferQueuedCompletely = true; // 转入只等待网络缓存清空的状态。
        // 文件内容已经不再需要，尽早释放开发板文件描述符。
        transferFile->close(); // 后续 bytesWritten 回调不再访问文件。
    }
    // 若网络也已全部发完，则结束本次单请求连接。
    if (transferQueuedCompletely && client->bytesToWrite() == 0) // 文件和网络缓存都已经发送完。
        client->disconnectFromHost(); // 优雅关闭本次单请求连接。
}

/**
 * 构造统一的单行 JSON 成功响应头。
 * @param type 响应类型，list 代表列表，file 代表录像文件。
 * @param payloadSize 紧随 JSON 头之后的载荷精确字节数。
 * @param name 文件响应返回的文件名；列表响应为空。
 */
void MediaArchiveServer::sendResponseHeader(const QString &type,
                                             qint64 payloadSize,
                                             const QString &name)
{
    // 对端可能已断开；此时不再创建无效响应。
    if (!client) // 响应构造前客户端可能已经断开。
        return; // 没有套接字时不能发送响应。
    // header保存PC端解析响应所需的最小元数据。
    QJsonObject header; // 保存 PC 解析载荷所需的协议字段。
    header.insert(QStringLiteral("ok"), true); // true 表示请求处理成功。
    header.insert(QStringLiteral("type"), type); // 告诉 PC 后续载荷是 list 还是 file。
    // Qt JSON没有64位整数类型，使用double编码本项目可接受的文件长度。
    header.insert(QStringLiteral("payloadSize"), double(payloadSize)); // 声明载荷边界。
    // 只有文件响应才需要返回文件名。
    if (!name.isEmpty()) // 列表响应没有文件名，因此不写空字段。
        header.insert(QStringLiteral("name"), name); // 文件响应携带建议保存的文件名。
    // Compact JSON后追加换行符，接收端先readLine()再按长度读取载荷。
    client->write(                                             // 把完整协议头排入 TCP 发送缓存。
        QJsonDocument(header).toJson(QJsonDocument::Compact) + // 序列化无缩进 JSON。
        '\n');                                                 // 换行符标记 JSON 头结束。
}

/**
 * 记录错误、返回失败响应头，并按单请求协议关闭连接。
 * @param message 写入开发板日志和响应 JSON 的具体错误原因。
 */
void MediaArchiveServer::sendError(const QString &message)
{
    // 开发板日志保留具体原因，便于区分非法请求和文件系统问题。
    qWarning() << "媒体归档请求失败：" << message; // 在服务端保留完整诊断信息。
    // 没有客户端时只能记录本地日志。
    if (!client) // 请求可能在错误产生前已经断开。
        return; // 此时只能保留上面的本地日志。
    // 错误响应不包含后续载荷，因此payloadSize固定为0。
    QJsonObject header; // 构造没有二进制载荷的错误响应头。
    header.insert(QStringLiteral("ok"), false); // false 明确表示请求失败。
    header.insert(QStringLiteral("message"), message); // 把可读原因返回给 PC。
    header.insert(QStringLiteral("payloadSize"), 0); // 错误响应后没有载荷字节。
    client->write(                                             // 把错误头排入 TCP 发送缓存。
        QJsonDocument(header).toJson(QJsonDocument::Compact) + // 序列化为紧凑 JSON。
        '\n');                                                 // 使用换行符结束协议头。
    // 使用优雅断开，尽量保证错误JSON先发到PC端。
    client->disconnectFromHost(); // 尽量在错误 JSON 发出后正常关闭连接。
}

/** 响应QTcpSocket::disconnected，延迟销毁对象并复位下一连接所需状态。 */
void MediaArchiveServer::clientDisconnected()
{
    // sender()是触发当前槽的套接字；qobject_cast同时完成运行时类型检查。
    QTcpSocket *disconnected =                       // 保存实际发出 disconnected 信号的套接字。
        qobject_cast<QTcpSocket *>(sender());         // 运行时确认 sender() 确实是 QTcpSocket。
    // deleteLater()确保对象在所属线程事件循环中安全析构。
    if (disconnected) // 类型转换成功才拥有可释放的套接字指针。
        disconnected->deleteLater(); // 让所属线程事件循环在安全时机销毁对象。
    // 只有当前客户端断开时才清空成员指针。
    if (client == disconnected) // 信号来自当前记录的活动连接。
        client = nullptr; // 立即消除成员悬空指针风险。
    // 未完成下载不能把打开的文件遗留给下一连接。
    if (transferFile && transferFile->isOpen()) // 下载中断时文件可能仍然打开。
        transferFile->close(); // 释放开发板文件描述符并停止后续读取。
    // 清除文件长度、水位状态和半包请求。
    transferBytesRemaining = 0; // 下一连接不继承旧文件剩余长度。
    transferQueuedCompletely = false; // 下一连接重新进入未传输状态。
    requestBuffer.clear(); // 丢弃旧连接未组成完整 JSON 的字节。
}

/** 主动终止当前客户端；用于服务停止以及新连接替换旧连接。 */
void MediaArchiveServer::closeClient()
{
    // 首先关闭可能仍在读取的媒体文件。
    if (transferFile && transferFile->isOpen()) // 当前可能正在下载文件。
        transferFile->close(); // 先关闭文件，防止替换连接后继续读取旧录像。
    // 无论是否存在客户端，都先复位协议状态。
    transferBytesRemaining = 0; // 清除本次传输剩余长度。
    transferQueuedCompletely = false; // 清除载荷排队完成标志。
    requestBuffer.clear(); // 清除当前连接的 TCP 请求半包。
    // 没有客户端时清理工作已经完成。
    if (!client) // 没有活动连接时，文件和协议状态已经清理完毕。
        return; // 避免访问空套接字。
    // 断开本对象接收的全部套接字信号，防止abort后槽函数重复清理。
    client->disconnect(this); // 解除 client 发给本对象的 readyRead/bytesWritten/disconnected 信号。
    // abort立即丢弃未发送数据；本函数用于替换或停止，而非正常完成响应。
    client->abort(); // 立即关闭连接并丢弃尚未发送的缓存数据。
    // 在套接字所属线程安全销毁。
    client->deleteLater(); // 延迟到事件循环安全位置销毁套接字。
    // 立即清空悬空风险指针。
    client = nullptr; // 清空成员指针，使下一连接可以成为当前客户端。
}
