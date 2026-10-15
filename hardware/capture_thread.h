/*
 * ============================================================================
 * capture_thread.h — V4L2 摄像头采集线程 + 帧分发器
 * ============================================================================
 *
 * 继承 QThread，在独立线程中使用 V4L2 接口从 USB 摄像头采集 YUV 帧，
 * 转换为 QImage 后通过信号分发给三个消费者：
 *   localPreviewReady  → UI 线程（QLabel 显示）
 *   networkFrameReady  → TcpFrameSender（H.264 编码 + TCP/UDP 发送）
 *   analysisFrameReady → VisionEventDetector（运动检测 + 人脸检测）
 *
 * 当前测试配置：
 *   三条单帧背压门控均关闭，使用固定 FPS 控制分发速度。
 *   门控成员和 frameHandled 回调仍保留，可通过 cpp 文件顶部开关恢复。
 *
 * V4L2 参数：
 *   设备节点    : /dev/video1
 *   缓冲区数量   : 3（mmap 方式）
 *   像素格式    : YUYV → RGB888
 *
 * Copyright (C) Deng Zhimao Co., Ltd. 2021-2030.
 * All rights reserved.
 */
#ifndef CAPTURE_THREAD_H
#define CAPTURE_THREAD_H

// 原子操作库：所有门控标志和计数器都用 std::atomic，保证跨线程读写安全（不加锁）
#include <atomic>

// Qt 图像类：QImage 用于封装一帧像素数据，通过信号传递给消费者
#include <QImage>
// Qt 互斥锁：保护人脸框坐标（由视觉线程写入、采集线程读取）
#include <QMutex>
// Qt 矩形：人脸检测框的坐标（x, y, width, height）
#include <QRect>
// Qt 线程基类：CaptureThread 继承它，重写 run() 实现独立线程中的采集循环
#include <QThread>
// Qt 动态数组：存储人脸检测框列表（QVector<QRect>）
#include <QVector>

// 摄像头设备节点：Linux 下 USB 摄像头通常注册为 /dev/video0 或 /dev/video1
#define VIDEO_DEV "/dev/video1"
// V4L2 缓冲区数量：3 个 mmap 缓冲区构成流水线（驱动填一个、应用处理一个、第三个排队）
#define VIDEO_BUFFER_COUNT 3

/*
 * 采集流水线统计数据快照结构体。
 * 只包含原子计数器的值，主线程可直接读取这些副本，不会阻塞 V4L2 采集线程。
 * UI 通过相邻两次快照的差值计算实时 FPS 和各阶段丢帧数量。
 */
struct CapturePipelineStatistics
{
    quint64 capturedFrames = 0;        // 累计采集帧数（从摄像头 DQBUF 出来的总帧数）
    quint64 previewFrames = 0;         // 累计本地预览帧数（发送给 UI 显示的帧数）
    quint64 networkFrames = 0;         // 累计网络推流帧数（送往 H.264 编码器的帧数）
    quint64 analysisFrames = 0;        // 累计视觉分析帧数（发送给 AI 检测的帧数）
    quint64 previewDroppedFrames = 0;  // 本地预览丢帧数（UI 处理不过来时丢弃的帧数）
    quint64 networkDroppedFrames = 0;  // 网络推流丢帧数（网络发送不过来时丢弃的帧数）
    quint64 analysisDroppedFrames = 0; // 视觉分析丢帧数（AI 推理不过来时丢弃的帧数）
    quint64 frameCopyFailures = 0;     // 帧深拷贝失败次数（内存不足导致 copy() 失败）
};

class CaptureThread : public QThread
{
Q_OBJECT // Qt 元对象宏：启用信号槽机制，必须写在类定义内

    public :
    // 构造函数：parent 指定 Qt 父对象（用于 Qt 对象树生命周期管理）
    explicit CaptureThread(QObject *parent = nullptr);
    // 析构函数：安全停止采集线程（置标志 → 请求中断 → 等待线程退出）
    ~CaptureThread() override;

    // 获取采集流水线的统计快照（主线程每秒调用一次，用于性能面板显示）
    CapturePipelineStatistics statistics() const;

signals:
    // 本地预览帧信号：通知主线程→cameraPreviewLabel，携带完整 QImage 帧数据
    void localPreviewReady(QImage image);
    // 网络推流帧信号：通知独立线程进行 H.264 编码和 TCP/UDP 发送
    // 独立线程处理可避免视频编码和发送阻塞 V4L2 采集。
    void networkFrameReady(QImage image);
    // 视觉分析帧信号：当前按固定 30 FPS 上限分发给检测线程
    void analysisFrameReady(QImage image);

public slots:
    // 启用/禁用摄像头采集（true→启动线程→run()开始执行，false→请求中断线程）
    void setCaptureEnabled(bool enabled);
    // 启用/禁用 TCP/UDP 网络推流（控制 networkFrameReady 信号是否发射）
    void setNetworkStreamingEnabled(bool enabled);
    // 启用/禁用本地预览（控制 localPreviewReady 信号是否发射）
    void setLocalPreviewEnabled(bool enabled);
    // 启用/禁用视觉分析（控制 analysisFrameReady 信号是否发射）
    void setAnalysisEnabled(bool enabled);
    // 启用/禁用人脸框叠加绘制（在本地预览帧上画绿色人脸框）
    void setFaceOverlayEnabled(bool enabled);
    // 更新人脸检测框坐标列表（由视觉检测线程通过 DirectConnection 调用）
    void updateFaceOverlay(QVector<QRect> faceBoxes);
    // 本地预览帧已被消费（主线程处理完一帧后调用，打开门控允许下一帧）
    void localPreviewFrameHandled();
    // 网络帧已被消费（网络线程处理完一帧后调用，打开门控允许下一帧）
    void networkFrameHandled();
    // 分析帧已被消费（视觉线程处理完一帧后调用，打开门控允许下一帧）
    void analysisFrameHandled();

private:
    // 线程主函数：V4L2 采集循环，由 QThread::start() 在新线程中执行
    void run() override;

    /*
     * ===== 控制标志（std::atomic_bool = 原子布尔值） =====
     * 使用 std::atomic 是因为这些标志会被主线程写入、采集线程读取。
     * 原子操作保证两个线程看到的值始终一致，不需要加锁。
     */
    std::atomic_bool captureEnabled;          // 摄像头采集总开关
    std::atomic_bool networkStreamingEnabled; // 网络推流使能标志
    std::atomic_bool localPreviewEnabled;     // 本地预览使能标志
    std::atomic_bool analysisEnabled;         // 视觉分析使能标志
    std::atomic_bool faceOverlayEnabled;      // 人脸框叠加使能标志
    /*
     * ===== 单帧背压门控（Frame Gate） =====
     * 每个消费者一个门控标志。true 表示"有一帧正在被处理"，
     * 生产者在门为 true 时丢弃新帧，防止帧在事件队列中堆积。
     */
    std::atomic_bool localPreviewFramePending; // 本地预览门控：最多只允许一帧等待 UI 线程处理
    std::atomic_bool networkFramePending;      // 网络门控：最多允许一帧等待网络线程处理，防止网络变慢时无限积压
    std::atomic_bool analysisFramePending;     // 视觉分析门控：只允许一帧在途，避免算法线程积压
    // 人脸框互斥锁：保护 latestFaceBoxes（视觉线程写、采集线程读）
    QMutex faceOverlayMutex;
    // 最新的人脸检测框坐标列表（由视觉检测线程更新，采集线程读取后绘制到预览帧上）
    QVector<QRect> latestFaceBoxes;

    /*
     * ===== 统计计数器（std::atomic<quint64> = 原子无符号 64 位整数） =====
     * 采集线程累加这些计数器，主线程通过 statistics() 读取快照。
     * 原子类型保证读取时不会出现"读到一半被修改"的脏数据。
     */
    std::atomic<quint64> capturedFrameCount;        // 累计 DQBUF 取出的帧数
    std::atomic<quint64> previewFrameCount;         // 累计发送给本地预览的帧数
    std::atomic<quint64> networkFrameCount;         // 累计发送给网络推流的帧数
    std::atomic<quint64> analysisFrameCount;        // 累计发送给视觉分析的帧数
    std::atomic<quint64> previewDroppedFrameCount;  // 本地预览累计丢帧数
    std::atomic<quint64> networkDroppedFrameCount;  // 网络推流累计丢帧数
    std::atomic<quint64> analysisDroppedFrameCount; // 视觉分析累计丢帧数
    std::atomic<quint64> frameCopyFailureCount;     // QImage::copy() 失败累计次数
};

#endif // CAPTURE_THREAD_H
