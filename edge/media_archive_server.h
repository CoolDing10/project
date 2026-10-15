#ifndef MEDIA_ARCHIVE_SERVER_H
#define MEDIA_ARCHIVE_SERVER_H

#include <QByteArray>
#include <QObject>

class QFile;       // Qt文件对象：按块读取抓拍图片或MJPEG录像。
class QTcpServer;  // Qt TCP监听器：负责监听PC端发起的归档连接。
class QTcpSocket;  // Qt TCP连接：表示当前唯一的PC归档客户端。

/*
 * 开发板媒体归档服务。
 *
 * 实时 JPEG 视频仍使用 8888 端口；本服务单独监听 8890，避免抓拍或录像
 * 下载占用实时视频连接。请求使用单行 JSON，响应为“单行 JSON 头 + 定长
 * 载荷”。录像按 64 KiB 分块发送，不把整个文件读入内存。
 */
class MediaArchiveServer final : public QObject
{
    Q_OBJECT // 启用Qt元对象能力，使本类可以声明信号和槽。

public:
    /**
     * @brief 创建媒体归档服务器对象，但此时还不会占用网络端口。
     * @param parent Qt父对象；父对象析构时会自动回收本对象。
     */
    explicit MediaArchiveServer(QObject *parent = nullptr);

public slots:
    /**
     * @brief 创建QTcpServer并开始监听归档下载端口。
     * @param port TCP监听端口；默认8890，与8888实时视频端口相互独立。
     */
    void initialize(quint16 port = 8890);

    /** @brief 停止当前传输、关闭客户端并停止监听端口。 */
    void stop();

signals:
    /**
     * @brief 将服务启动、停止或错误信息通知UI线程。
     * @param status 可直接显示或打印的中文状态文本。
     */
    void statusChanged(QString status);

private slots:
    /** @brief 接收等待队列中的新连接，并让最新连接成为当前客户端。 */
    void acceptConnection();

    /** @brief 累积客户端字节，读取以换行符结尾的一条JSON请求。 */
    void readRequest();

    /** @brief 在Qt发送缓冲区有空间时继续分块写入归档文件。 */
    void continueFileTransfer();

    /** @brief 清理已经断开的客户端以及未完成传输的状态。 */
    void clientDisconnected();

private:
    /**
     * @brief 解析并分发一条完整请求。
     * @param requestLine 不包含末尾换行符的紧凑JSON字节串。
     */
    void processRequest(const QByteArray &requestLine);

    /** @brief 扫描抓拍和录像目录，并向PC返回JSON文件清单。 */
    void sendArchiveList();

    /**
     * @brief 校验请求路径并启动一个定长文件下载。
     * @param category 归档类别，只允许snapshots或recordings。
     * @param name 不含目录部分的文件名，用于阻止目录穿越攻击。
     */
    void startFileTransfer(const QString &category, const QString &name);

    /** @brief 向客户端返回失败响应并结束连接。 */
    void sendError(const QString &message);

    /**
     * @brief 写入一行JSON响应头，接收端据此确定后续载荷边界。
     * @param type 响应类型，例如list或file。
     * @param payloadSize 紧随响应头之后的二进制载荷字节数。
     * @param name 文件下载时携带的原始文件名；列表响应可为空。
     */
    void sendResponseHeader(const QString &type, qint64 payloadSize,
                            const QString &name = QString());

    /**
     * @brief 将受信任的类别转换为开发板上的归档目录。
     * @return 合法类别对应的绝对/可解析目录；非法类别返回空字符串。
     */
    QString archiveDirectory(const QString &category) const;

    /** @brief 主动关闭当前连接并将所有请求、文件传输状态复位。 */
    void closeClient();

    QTcpServer *server;               // 端口监听器，只在initialize()中创建一次。
    QTcpSocket *client;               // 当前PC客户端；协议设计为单客户端串行请求。
    QFile *transferFile;              // 当前正在下载的文件，避免整文件一次性载入内存。
    QByteArray requestBuffer;         // TCP接收缓存，用来处理请求被拆成多个数据包的情况。
    qint64 transferBytesRemaining;    // 本次响应尚未排入TCP发送缓冲区的字节数。
    bool transferQueuedCompletely;    // true表示文件内容已全部交给QTcpSocket等待发送。
};

#endif // MEDIA_ARCHIVE_SERVER_H
