#ifndef ARCHIVE_BROWSER_H
#define ARCHIVE_BROWSER_H

#include <QByteArray>
#include <QImage>
#include <QWidget>

class QFile;        // 顺序读取已经下载到电脑的MJPEG录像。
class QLabel;       // 显示状态文本、表单标签和图片/录像画面。
class QLineEdit;    // 输入开发板IPv4地址。
class QListWidget;  // 展示开发板返回的抓拍与录像文件列表。
class QPushButton;  // 刷新、下载以及打开目录按钮。
class QResizeEvent; // 窗口缩放事件类型。
class QSaveFile;    // 原子下载文件：成功commit前不会破坏同名正式文件。
class QSpinBox;     // 输入归档TCP端口。
class QTcpSocket;   // 连接开发板8890媒体归档服务。
class QTimer;       // 按固定间隔播放MJPEG中的下一JPEG帧。

/* PC端抓拍/录像浏览器：从开发板8890端口读取列表并按需下载。 */
class ArchiveBrowser final : public QWidget
{
    Q_OBJECT // 启用Qt信号槽以及元对象信息。

public:
    /** @brief 创建并布局PC归档浏览页面，同时初始化网络和播放状态机。 */
    explicit ArchiveBrowser(QWidget *parent = nullptr);

    /**
     * @brief 从实时视频连接同步开发板地址，减少用户重复输入。
     * @param address IPv4地址，兼容Qt可能返回的::ffff:IPv4形式。
     */
    void setDeviceAddress(const QString &address);

public slots:
    /** @brief 请求开发板重新扫描并返回全部抓拍和录像元数据。 */
    void refreshArchive();

protected:
    /** @brief 页面尺寸改变后按新预览区域重新等比缩放当前图像。 */
    void resizeEvent(QResizeEvent *event) override;

private slots:
    /** @brief 下载列表中当前选中的文件并在完成后自动预览。 */
    void downloadSelected();
    /** @brief TCP连接建立后发送一行JSON请求。 */
    void sendRequest();
    /** @brief 累积TCP字节并依次解析响应头与定长载荷。 */
    void readResponse();
    /** @brief 将套接字错误显示到UI并清理未完成请求。 */
    void handleSocketError();
    /** @brief 从本地MJPEG文件读取并显示下一张JPEG图像。 */
    void showNextMjpegFrame();
    /** @brief 调用桌面文件管理器打开统一下载目录。 */
    void openDownloadDirectory();

private:
    /** @brief 当前网络请求状态；单个ArchiveBrowser同一时刻只处理一个请求。 */
    enum RequestKind {
        NoRequest,   // 当前空闲，没有等待中的服务端响应。
        ListRequest, // 正在获取JSON归档列表。
        FileRequest  // 正在下载一个图片或MJPEG文件。
    };

    /**
     * @brief 复位旧连接并开始一次新的列表或文件请求。
     * @param kind 请求类型。
     * @param category 文件请求的类别，列表请求为空。
     * @param name 文件请求的纯文件名，列表请求为空。
     * @param size 列表中记录的预估大小；收到响应头后以服务端实际值覆盖。
     */
    void beginRequest(RequestKind kind, const QString &category = QString(),
                      const QString &name = QString(), qint64 size = 0);

    /** @brief 从receiveBuffer解析换行结尾的JSON头并初始化载荷接收目标。 */
    bool parseResponseHeader();

    /** @brief 按响应头声明的长度消费缓存，将数据加入列表或写入下载文件。 */
    void consumePayload();

    /** @brief 提交完整下载或解析列表，并恢复按钮及网络状态。 */
    void completeRequest();

    /** @brief 取消临时文件、清除半包数据并回到可发起请求状态。 */
    void resetTransfer();

    /** @brief 将服务端JSON列表转换成带自定义角色数据的QListWidgetItem。 */
    void populateArchiveList(const QByteArray &payload);

    /** @brief 返回~/Downloads/imx6ull_media；无下载目录时回退到用户主目录。 */
    QString downloadRoot() const;

    /** @brief 根据类别把已下载文件作为静态图片或MJPEG录像显示。 */
    void displayDownloadedFile(const QString &filePath,
                               const QString &category);

    /** @brief 保存当前图像，并按previewLabel尺寸等比平滑缩放。 */
    void displayImage(const QImage &image);

    /** @brief 从字节流搜索JPEG SOI/EOI标记，解码一帧MJPEG。 */
    bool readOneMjpegFrame(QImage *image);

    QTcpSocket *socket;                 // 归档协议的短连接TCP客户端。
    QLineEdit *deviceAddressEdit;       // 开发板IP输入框，默认192.168.10.50。
    QSpinBox *archivePortSpinBox;       // 归档服务端口输入框，默认8890。
    QPushButton *refreshButton;         // 发起ListRequest。
    QPushButton *downloadButton;        // 发起当前文件的FileRequest。
    QLabel *statusLabel;                // 显示连接、下载进度和错误信息。
    QListWidget *archiveList;           // 保存服务端文件条目及其隐藏元数据。
    QLabel *previewLabel;               // 显示抓拍图片或MJPEG当前帧。
    QTimer *mjpegPlaybackTimer;          // 每66ms推进一帧，约15 FPS。
    QFile *mjpegFile;                   // 当前打开的本地MJPEG录像。

    RequestKind requestKind;            // 当前请求状态机枚举值。
    QString requestedCategory;          // 当前文件类别：snapshots或recordings。
    QString requestedName;              // 当前下载的服务端文件名。
    qint64 requestedSize;               // 当前载荷总长度，用于计算下载百分比。
    bool responseHeaderParsed;          // 是否已经从TCP流中剥离完整JSON头。
    qint64 payloadBytesRemaining;        // 距离当前定长载荷完成还差多少字节。
    QByteArray receiveBuffer;            // TCP拆包/粘包接收缓存。
    QByteArray listPayload;              // ListRequest完整JSON载荷累计缓存。
    QSaveFile *downloadFile;             // FileRequest临时文件，成功后原子commit。
    QString downloadedFilePath;          // 当前文件最终保存到电脑的绝对路径。

    QByteArray mjpegBuffer;              // 尚未组成完整JPEG帧的录像字节缓存。
    QImage currentPreviewImage;           // 最近显示图像，窗口缩放时用于重新渲染。
};

#endif // ARCHIVE_BROWSER_H
