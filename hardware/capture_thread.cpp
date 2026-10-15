/*
 * Copyright (C) Deng Zhimao Co., Ltd. 2021-2030.
 * All rights reserved.
 */
#include "capture_thread.h"
#include "video_pipeline_config.h"

#include <QDebug>
#include <QElapsedTimer>

#if defined(__linux__) && defined(__arm__)
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>
#endif

/*
 * ===== CaptureThread 构造函数 =====
 * 参数 parent：Qt 父对象指针（nullptr 表示无父对象）
 * 初始化列表：将所有 atomic 标志置为 false
 * 函数体为空：所有初始化都在初始化列表中完成
 */
CaptureThread::CaptureThread(QObject *parent)
    : QThread(parent),                 // 调用基类 QThread 的构造函数
      captureEnabled(false),           // 采集开关初始关闭
      networkStreamingEnabled(false),  // 网络推流初始关闭
      localPreviewEnabled(false),      // 本地预览初始关闭
      analysisEnabled(false)           // 视觉分析初始关闭
{
    // 构造函数体为空：所有成员已在初始化列表中完成赋值
}

/*
 * ===== CaptureThread 析构函数 =====
 * 三步安全退出：
 *   1. captureEnabled.store(false)  → 通知采集循环停止
 *   2. requestInterruption()         → 设置中断标志（while 条件检查）
 *   3. wait()                        → 阻塞等待线程 run() 执行完毕
 */
CaptureThread::~CaptureThread()
{
    // 将采集使能标志置为 false（原子存储），采集循环中的 while 条件将变为 false
    captureEnabled.store(false);
    // 请求线程中断：isInterruptionRequested() 将返回 true，双重保险确保退出
    requestInterruption();
    // 阻塞等待线程 run() 函数返回（超时则死等），确保资源已被 cleanup() 释放
    wait();
}

/*
 * ===== setCaptureEnabled() — 采集总开关 =====
 * 参数 enabled：true 启动采集线程，false 停止采集线程
 * store() 是 std::atomic 的写入方法，保证写操作是原子的。
 * QThread::start() 会创建新线程并在其中执行 run()。
 * requestInterruption() 设置中断标志让 run() 中的 while 条件变为 false。
 */
void CaptureThread::setCaptureEnabled(bool enabled)
{
    captureEnabled.store(enabled); // 原子写入：更新采集使能标志

    if (enabled)
    {
        // 用户点击"开始采集"
        if (!isRunning())     // 如果线程还没有在运行
            QThread::start(); // 创建新线程 → 调用 run() → 进入 V4L2 采集循环
    }
    else
    {
        // 用户点击"停止采集"
        requestInterruption(); // 请求线程中断：isInterruptionRequested() 返回 true
    }
}

/*
 * ===== setNetworkStreamingEnabled() — 网络推流开关 =====
 * 参数 enabled：true 开始向 TcpFrameSender 发送帧，false 停止
 * 只在采集循环中被读取（poll→DQBUF→判断 networkEnabled→决定是否发射 networkFrameReady）
 */
void CaptureThread::setNetworkStreamingEnabled(bool enabled)
{
    networkStreamingEnabled.store(enabled); // 原子写入：更新网络推流标识
}

/*
 * ===== setLocalPreviewEnabled() — 本地预览开关 =====
 * 参数 enabled：true 开始向主线程发送预览帧，false 停止
 */
void CaptureThread::setLocalPreviewEnabled(bool enabled)
{
    localPreviewEnabled.store(enabled); // 原子写入：更新本地预览标识
}

/*
 * ===== setAnalysisEnabled() — 视觉分析开关 =====
 * 参数 enabled：true 开始向 VisionEventDetector 发送帧，false 停止
 */
void CaptureThread::setAnalysisEnabled(bool enabled)
{
    analysisEnabled.store(enabled); // 原子写入：更新视觉分析标识
}

/*
 * ===== run() — 线程主函数（V4L2 采集流水线核心实现） =====
 *
 * 由 QThread::start() 在独立线程中调用，不阻塞主线程。
 * 完整 V4L2 流程：
 *   open → S_FMT → REQBUFS → QUERYBUF → mmap → QBUF
 *        → STREAMON → (poll → DQBUF → 处理 → QBUF) 循环
 *        → STREAMOFF → munmap → 释放缓冲区 → close
 *
 * #if 条件编译：在非 ARM Linux 平台（如 x86 PC）上，此函数直接返回，
 *    因为 V4L2 是 Linux 内核接口，macOS/Windows 不支持。
 */
void CaptureThread::run()
{
    /*
     * ===== 平台检查：只有 ARM Linux 才执行 V4L2 代码 =====
     * 在 x86 PC 上开发调试时，摄像头采集功能不启用。
     */
#if !defined(__linux__) || !defined(__arm__)
    // 直接访问 V4L2 的实现只在 Linux ARM 开发板上启用。
    captureEnabled.store(false); // 非 ARM Linux 平台：强制关闭采集
    return;                      // 直接返回，不执行任何 V4L2 操作
#else
    // ===== V4L2 采集仅在 ARM Linux 平台编译和执行 =====

    /*
     * ===== MappedBuffer 结构体：保存一个 mmap() 映射的 V4L2 缓冲区 =====
     * start  缓冲区映射到本进程后的虚拟地址（可直接当数组访问像素数据）
     * length 映射长度（字节数），munmap() 释放时需要这个值
     */
    struct MappedBuffer
    {
        void *start;   // 缓冲区映射到本进程后的虚拟地址
        size_t length; // 映射长度，释放时传给 munmap()
    };

    // 应用最多管理 VIDEO_BUFFER_COUNT（当前为 3）个缓冲区的数组
    // MAP_FAILED 表示该元素尚未成功建立映射（mmap 失败时的返回值）
    MappedBuffer buffers[VIDEO_BUFFER_COUNT]; // 静态数组，3 个槽位
    for (unsigned int i = 0; i < VIDEO_BUFFER_COUNT; ++i)
    {
        buffers[i].start = MAP_FAILED; // 初始化所有缓冲区为"未映射"状态
        buffers[i].length = 0;         // 映射长度初始为 0
    }

    // ===== 采集状态变量 =====
    int videoFd = -1;              // open() 返回的摄像头设备文件描述符，-1 表示未打开
    unsigned int mappedCount = 0;  // 已成功 mmap 的缓冲区数量（用于 cleanup 中逐个 munmap）
    bool streaming = false;        // 流是否已启动（VIDIOC_STREAMON 成功 → true）
    bool buffersRequested = false; // 是否已申请缓冲区（VIDIOC_REQBUFS 成功 → true）

    /*
     * ===== logIoctlError Lambda：V4L2 操作失败时打印日志 =====
     * 参数 operation：失败的 ioctl 操作名称（如 "VIDIOC_S_FMT"）
     * 使用 errno 获取最后一次系统调用的错误码，std::strerror 将其转为可读信息
     */
    auto logIoctlError = [](const char *operation)
    {
        // qWarning 打印警告级别日志，全局变量 errno 保存最近一次系统调用的错误码
        qWarning("%s failed: %s", operation, std::strerror(errno));
    };

    /*
     * ===== cleanup Lambda：资源安全回收 =====
     * [&] 表示按引用捕获本函数中的局部变量（videoFd、buffers、streaming 等）
     * 无论初始化进行到哪个阶段失败，调用 cleanup() 都可以安全回收已分配的资源。
     * 释放顺序与申请顺序基本相反：STREAMOFF → munmap → REQBUFS(0) → close
     */
    auto cleanup = [&]()
    {
        // videoFd < 0 表示设备没有成功打开，无资源可释放
        if (videoFd < 0)
            return; // 直接返回，什么都不做

        if (streaming)
        {
            // 声明缓冲区类型为视频采集（VIDIOC_STREAMOFF 的参数要求）
            enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            // ioctl(fd, request, arg)：发送控制命令给设备驱动
            //   videoFd          要控制的设备文件描述符
            //   VIDIOC_STREAMOFF 停止指定类型的视频流
            //   &type            指向缓冲区类型的地址（enum 变量的指针）
            if (ioctl(videoFd, VIDIOC_STREAMOFF, &type) < 0)
                logIoctlError("VIDIOC_STREAMOFF"); // 记录失败但继续清理
            streaming = false;                     // 更新状态标志
        }

        // 逐个取消 mmap 映射
        for (unsigned int i = 0; i < mappedCount; ++i)
        {
            // 只处理已成功映射的缓冲区
            if (buffers[i].start != MAP_FAILED)
            {
                // munmap(address, length)：取消用户空间的内存映射
                //   address = mmap() 当初返回的虚拟地址
                //   length  = 当初映射的字节数
                if (munmap(buffers[i].start, buffers[i].length) < 0)
                    logIoctlError("munmap");
                // 重置为未映射状态
                buffers[i].start = MAP_FAILED;
                buffers[i].length = 0;
            }
        }

        if (buffersRequested)
        {
            // 对 VIDIOC_REQBUFS 传入 count=0，通知驱动释放队列中先前分配的所有 MMAP 缓冲区
            struct v4l2_requestbuffers releaseBuffers = {};    // {} 清零整个结构体
            releaseBuffers.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; // 指定视频采集队列
            releaseBuffers.memory = V4L2_MEMORY_MMAP;          // 指定 mmap 方式
            releaseBuffers.count = 0;                          // count=0 表示释放所有缓冲区
            if (ioctl(videoFd, VIDIOC_REQBUFS, &releaseBuffers) < 0)
                logIoctlError("VIDIOC_REQBUFS(count=0)");
            buffersRequested = false; // 缓冲区已释放
        }

        // 关闭设备文件描述符；之后这个整数不能再用于 ioctl
        close(videoFd);
        videoFd = -1; // 标记为已关闭
    };

    /*
     * ===== 第 1 步：打开摄像头设备 =====
     * open(path, flags)：
     *   VIDEO_DEV  目标 V4L2 设备节点 = "/dev/video1"
     *   O_RDWR     以读写方式打开，V4L2 流式采集要求此模式
     *   O_NONBLOCK 非阻塞模式；暂时无数据时 DQBUF 返回 EAGAIN，
     *              因此后面先用 poll() 等待数据就绪
     * 返回值：成功返回非负整数的文件描述符，失败返回 -1
     */
    videoFd = open(VIDEO_DEV, O_RDWR | O_NONBLOCK);
    if (videoFd < 0)
    {
        // 打开失败：打印错误信息并退出
        qWarning("Unable to open %s: %s", VIDEO_DEV, std::strerror(errno));
        captureEnabled.store(false); // 同步采集状态为"已停止"
        return;                      // 直接返回，线程结束
    }

    /*
     * ===== 第 2 步：协商图像格式（VIDIOC_S_FMT） =====
     * v4l2_format 描述应用希望获得的视频格式。
     * {} 将整个结构体清零，避免保留字段中出现未初始化的随机值。
     */
    struct v4l2_format format = {};                   // 清零初始化
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;        // 单平面视频采集队列
    format.fmt.pix.width = 640;                       // 请求的图像宽度（像素）
    format.fmt.pix.height = 480;                      // 请求的图像高度（像素）
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565; // 每像素 16 bit（R5G6B5）
    format.fmt.pix.field = V4L2_FIELD_ANY;            // 场类型交给驱动选择（逐行/隔行）
    format.fmt.pix.colorspace = V4L2_COLORSPACE_SRGB; // 请求 sRGB 色彩空间

    /*
     * VIDIOC_S_FMT 与驱动协商图像格式。
     * 第三个参数必须指向 v4l2_format。调用成功后，驱动可能会修改
     * width、height、pixelformat、bytesperline、sizeimage 等字段，
     * 所以后续必须使用调用返回后的 format，而不能只相信请求值。
     */
    if (ioctl(videoFd, VIDIOC_S_FMT, &format) < 0)
    {
        logIoctlError("VIDIOC_S_FMT"); // 记录失败原因
        cleanup();                     // 回收已打开的设备文件描述符
        captureEnabled.store(false);   // 同步状态
        return;
    }

    /*
     * 检查驱动是否真正接受了 RGB565 格式。
     * 后面使用 QImage::Format_RGB16 解释缓冲区字节，因此驱动实际返回的
     * 像素格式必须仍是 RGB565，否则相同字节会被错误地解释为颜色。
     */
    if (format.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565)
    {
        qWarning("%s did not accept RGB565", VIDEO_DEV); // 打印警告
        cleanup();                                       // 回收资源
        captureEnabled.store(false);                     // 同步状态
        return;
    }

    /*
     * ===== 第 3 步：申请 MMAP 缓冲区（VIDIOC_REQBUFS） =====
     * 请求驱动为视频采集队列建立 MMAP 缓冲区：
     *   count  期望数量（不保证驱动一定原样满足）
     *   type   必须和 format.type、后续 buffer.type 保持一致
     *   memory 表示缓冲区由驱动管理，并通过 mmap 映射给应用
     */
    struct v4l2_requestbuffers request = {};    // 清零初始化
    request.count = VIDEO_BUFFER_COUNT;         // 请求 3 个缓冲区
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; // 视频采集类型
    request.memory = V4L2_MEMORY_MMAP;          // 使用 mmap 内存映射方式

    // 第三个参数必须指向 v4l2_requestbuffers
    if (ioctl(videoFd, VIDIOC_REQBUFS, &request) < 0)
    {
        logIoctlError("VIDIOC_REQBUFS");
        cleanup();
        captureEnabled.store(false);
        return;
    }
    buffersRequested = true; // 标记缓冲区已申请（cleanup 中需要释放）

    /*
     * ioctl 返回后 request.count 是驱动实际建立的数量。
     * 本地数组最多只能容纳 VIDEO_BUFFER_COUNT 个，所以取两者较小值。
     * std::min(a, b)：返回 a 和 b 中较小的值。
     * static_cast<unsigned int>(VIDEO_BUFFER_COUNT)：C 风格类型转换的安全替代。
     */
    const unsigned int bufferCount =
        std::min(request.count, static_cast<unsigned int>(VIDEO_BUFFER_COUNT));
    // 至少两个缓冲区：一个驱动在填充，一个应用在读取，形成流水线
    if (bufferCount < 2)
    {
        qWarning("%s returned only %u capture buffer(s)", VIDEO_DEV, bufferCount);
        cleanup();
        captureEnabled.store(false);
        return;
    }

    /*
     * ===== 第 4 步：查询缓冲区信息并 mmap 映射（QUERYBUF → mmap） =====
     * 逐一查询驱动缓冲区的信息（长度、偏移量），并映射到本进程地址空间。
     */
    for (unsigned int i = 0; i < bufferCount; ++i)
    { // 遍历每一个缓冲区
        // v4l2_buffer：描述单个 V4L2 缓冲区
        struct v4l2_buffer buffer = {};            // 清零初始化
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; // 查询采集队列
        buffer.memory = V4L2_MEMORY_MMAP;          // 查询 MMAP 缓冲区
        buffer.index = i;                          // 要查询的缓冲区编号（0, 1, 2, ...）

        /*
         * VIDIOC_QUERYBUF：查询第 i 个缓冲区的物理信息。
         * 成功后驱动返回：
         *   buffer.length   第 i 个缓冲区的总长度（字节数）
         *   buffer.m.offset 第 i 个缓冲区用于 mmap 的设备偏移量
         */
        if (ioctl(videoFd, VIDIOC_QUERYBUF, &buffer) < 0)
        {
            logIoctlError("VIDIOC_QUERYBUF");
            cleanup();
            captureEnabled.store(false);
            return;
        }

        buffers[i].length = buffer.length; // 保存缓冲区长度（后面 munmap 需要）
        /*
         * mmap(addr, length, prot, flags, fd, offset)：
         *   nullptr                 由内核选择用户空间的虚拟地址
         *   buffer.length           要映射的字节数
         *   PROT_READ | PROT_WRITE  映射区域可读、可写
         *   MAP_SHARED              与驱动共享映射内容（驱动写入后用户立即可见）
         *   videoFd                 /dev/video1 的文件描述符
         *   buffer.m.offset         QUERYBUF 返回的设备偏移量
         *
         * mmap 不会复制整帧图像，而是建立用户虚拟地址到驱动物理缓冲区的
         * 映射关系。成功后可通过 buffers[i].start 直接访问图像字节。
         * 这是零拷贝的核心：用户代码读的就是 DMA 硬件写入的原始数据。
         */
        buffers[i].start = mmap(nullptr, buffer.length,
                                PROT_READ | PROT_WRITE, MAP_SHARED,
                                videoFd, buffer.m.offset);
        // mmap 失败返回 MAP_FAILED（即 (void *)-1），不是 nullptr
        if (buffers[i].start == MAP_FAILED)
        {
            logIoctlError("mmap");
            cleanup();
            captureEnabled.store(false);
            return;
        }
        ++mappedCount; // 已成功映射的缓冲区数 +1
    } // for 循环结束：所有缓冲区已 mmap 完毕

    /*
     * ===== 第 5 步：将所有空缓冲区入队（QBUF） =====
     * QBUF 后缓冲区的所有权暂时交给驱动；驱动开始用 DMA 向其中填入摄像头数据。
     * 应用必须等 DQBUF 成功后（驱动交还所有权）才能读取对应缓冲区中的完整图像。
     */
    for (unsigned int i = 0; i < bufferCount; ++i)
    {
        struct v4l2_buffer buffer = {};            // 清零初始化
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; // 视频采集队列
        buffer.memory = V4L2_MEMORY_MMAP;          // mmap 方式
        buffer.index = i;                          // 第 i 个缓冲区

        // VIDIOC_QBUF：将空缓冲区放入驱动的入队队列
        if (ioctl(videoFd, VIDIOC_QBUF, &buffer) < 0)
        {
            logIoctlError("VIDIOC_QBUF");
            cleanup();
            captureEnabled.store(false);
            return;
        }
    }

    /*
     * ===== 第 6 步：启动视频流（VIDIOC_STREAMON） =====
     * 第三个参数是缓冲区类型的地址（enum v4l2_buf_type 变量的指针），而不是 v4l2_buffer。
     * STREAMON 前必须至少有缓冲区经过 QBUF 入队，否则驱动无处存放采集到的图像。
     */
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE; // 视频采集流类型
    if (ioctl(videoFd, VIDIOC_STREAMON, &type) < 0)
    {
        logIoctlError("VIDIOC_STREAMON");
        cleanup();
        captureEnabled.store(false);
        return;
    }
    streaming = true; // 流已启动，cleanup 中需要执行 STREAMOFF

    /*
     * ===== 第 7 步：初始化三条消费者链路的固定 FPS 限速 =====
     * 三条链路只使用时间间隔限制帧发送频率。
     */
    QElapsedTimer localPreviewFrameTimer;
    localPreviewFrameTimer.start();
    const qint64 localPreviewFrameIntervalMs =
        1000 / VideoPipelineConfig::StandardFramesPerSecond;
    const qint64 localPreviewStreamingFrameIntervalMs =
        1000 / VideoPipelineConfig::StreamingPreviewFramesPerSecond;

    QElapsedTimer networkFrameTimer;
    networkFrameTimer.start();
    const qint64 networkFrameIntervalMs =
        1000 / VideoPipelineConfig::StandardFramesPerSecond;

    QElapsedTimer analysisFrameTimer;
    analysisFrameTimer.start();
    const qint64 analysisFrameIntervalMs =
        1000 / VideoPipelineConfig::StandardFramesPerSecond;

    // copy 警告定时器：防止 QImage::copy() 失败时日志刷屏
    QElapsedTimer frameCopyWarningTimer;  // 警告计时器
    bool frameCopyWarningPrinted = false; // 本周期内是否已打印过警告

    /*
     * ===== 第 8 步：主采集循环 =====
     * 两个条件任何一个不满足都会安全退出：
     *   captureEnabled == false         用户点击了"停止采集"
     *   isInterruptionRequested() == true Qt 收到了线程中断请求（析构时触发）
     */
    while (captureEnabled.load() && !isInterruptionRequested())
    {
        /*
         * ===== 8.1 等待摄像头数据就绪（poll） =====
         * pollfd 描述要等待的文件描述符及事件：
         *   fd     等待 videoFd（/dev/video1）
         *   events POLLIN：等待设备产生可取出的采集数据
         *   revents poll() 返回时填充：实际发生的事件类型
         */
        struct pollfd descriptor = {}; // 清零初始化
        descriptor.fd = videoFd;       // 要等待的设备文件描述符
        descriptor.events = POLLIN;    // 关注"有数据可读"事件

        /*
         * poll(fds, nfds, timeout)：
         *   &descriptor 指向 pollfd 数组的指针（这里只含一个元素）
         *   1           数组元素数量
         *   200         最长等待 200 毫秒（超时后 poll 返回 0）
         * 返回值：<0 出错，=0 超时，>0 有事件发生（即摄像头有数据）
         */
        const int pollResult = poll(&descriptor, 1, 200); // 阻塞等待，最多 200ms
        if (pollResult < 0)
        {
            // poll 返回值 < 0 表示出错
            if (errno == EINTR)    // 被信号打断不是摄像头故障，重新等待即可
                continue;          // 回到 while 循环开头
            logIoctlError("poll"); // 真正的 poll 错误
            break;                 // 退出 while 循环
        }
        // poll 返回 0 表示 200ms 超时（摄像头无新数据），重新检查 while 条件
        if (pollResult == 0)
            continue;
        // 检查设备是否出现错误（POLLERR）、挂断（POLLHUP）或无效（POLLNVAL）
        // & 是按位与运算：检查 revents 中是否包含这些错误标志位
        if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))
        {
            qWarning("Camera poll failed, revents=0x%x", descriptor.revents);
            break; // 设备错误不能继续采集，退出循环
        }

        /*
         * ===== 8.2 从驱动队列取出一帧（DQBUF） =====
         * DQBUF（Dequeue Buffer）：从驱动完成队列取出一帧已填充的图像数据。
         * DQBUF 前只需说明队列类型和内存方式；调用成功后驱动会把
         * buffer.index、bytesused、timestamp、sequence 等结果字段填好。
         */
        struct v4l2_buffer buffer = {};            // 清零初始化
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; // 指定操作视频采集队列
        buffer.memory = V4L2_MEMORY_MMAP;          // 指定缓冲区为 mmap 方式

        if (ioctl(videoFd, VIDIOC_DQBUF, &buffer) < 0)
        {
            /*
             * 两种可安全重试的错误：
             *   EAGAIN：非阻塞设备暂时没有完成的帧（因为前面已经 poll 过了，这里极少出现）
             *   EINTR：被信号打断
             */
            if (errno == EAGAIN || errno == EINTR)
                continue; // 安全地重新进入循环
            logIoctlError("VIDIOC_DQBUF");
            break; // 不可恢复的错误
        }

        // 安全检查：驱动返回的缓冲区索引必须在有效范围内
        // || 是逻辑或：索引越界 或 缓冲区未映射 → 出错
        if (buffer.index >= bufferCount ||
            buffers[buffer.index].start == MAP_FAILED)
        {
            qWarning("Driver returned invalid capture buffer index %u", buffer.index);
            break;
        }

        /*
         * ===== 8.3 创建 QImage（零拷贝引用 mmap 缓冲区） =====
         * bytesperline 是驱动返回的每行实际跨度（字节数）。
         * 可能因内存对齐而大于 width * 2。若旧驱动返回 0，
         * 则按 RGB565 每像素 2 字节计算后备值。
         */
        const int bytesPerLine = format.fmt.pix.bytesperline != 0                    // 三元运算符：判断驱动是否返回了有效值
                                     ? static_cast<int>(format.fmt.pix.bytesperline) // 是 → 使用驱动返回的值
                                     : static_cast<int>(format.fmt.pix.width * 2);   // 否 → 后备计算：640 × 2 = 1280
        /*
         * QImage(data, width, height, bytesPerLine, format)：
         *   data         DQBUF 返回索引所对应的 mmap 缓冲区虚拟地址
         *                 注意：static_cast<const uchar*> 将 void* 转为 uchar*（QImage 要求的类型）
         *   width/height VIDIOC_S_FMT 后驱动确认的实际尺寸
         *   bytesPerLine 驱动缓冲区中相邻两行的字节跨度
         *   Format_RGB16 与 V4L2_PIX_FMT_RGB565 对应（每像素 16 bit）
         *
         * 关键：frame 只引用驱动缓冲区，并不拥有这些像素数据。
         * 这是浅引用而非深拷贝，必须在 QBUF 之前执行 frame.copy()。
         */
        const QImage frame(
            static_cast<const uchar *>(buffers[buffer.index].start), // mmap 映射的虚拟地址
            static_cast<int>(format.fmt.pix.width),                  // 宽度：640
            static_cast<int>(format.fmt.pix.height),                 // 高度：480
            bytesPerLine,                                            // 行跨度
            QImage::Format_RGB16);                                   // 像素格式 RGB565
        // ===== 8.4 读取三个消费者的使能标志 =====
        const bool localDisplayEnabled = localPreviewEnabled.load(); // 本地预览是否启用
        const bool networkEnabled = networkStreamingEnabled.load();  // RTP/UDP 推流是否启用
        const bool visionAnalysisEnabled = analysisEnabled.load();   // 图像分析是否启用
        bool previewThisFrame = false;                               // 本帧是否发送给本地预览
        bool sendThisFrame = false;                                  // 本帧是否发送给网络
        bool analyzeThisFrame = false;                               // 本帧是否发送给图像分析

        /*
         * ===== 8.5 固定 FPS 限速 =====
         * 普通预览、网络推流、图像分析统一为 30 FPS；推流时预览为 10 FPS。
         */

        // 本地预览：普通模式 30 FPS，推流期间 10 FPS
        const qint64 activePreviewIntervalMs =
            networkEnabled ? localPreviewStreamingFrameIntervalMs
                           : localPreviewFrameIntervalMs;
        if (localDisplayEnabled &&
            localPreviewFrameTimer.elapsed() >= activePreviewIntervalMs)
            previewThisFrame = true;
        // 网络推流：最多 30 FPS
        if (networkEnabled &&
            networkFrameTimer.elapsed() >= networkFrameIntervalMs)
            sendThisFrame = true;
        // 图像分析：最多 30 FPS
        if (visionAnalysisEnabled &&
            analysisFrameTimer.elapsed() >= analysisFrameIntervalMs)
            analyzeThisFrame = true;

        /*
         * ===== 8.6 深拷贝 + 归还缓冲区（QBUF） =====
         * 只有任一消费者需要这一帧时才深拷贝一次。
         * frame.copy() 创建一份拥有独立内存的 QImage 副本。
         * 复制必须发生在 QBUF 之前：QBUF 后驱动可能立即用新帧数据覆盖
         * mmap 缓冲区，此时 frame 引用的原始数据就变成垃圾了。
         */
        QImage image;                                              // 声明空的 QImage（默认为 null）
        if (previewThisFrame || sendThisFrame || analyzeThisFrame) // 至少有一个消费者需要此帧
            image = frame.copy();                                  // 深拷贝：从 mmap 缓冲区复制到独立内存

        /*
         * 本帧处理完成后必须再次入队，把缓冲区所有权还给驱动。
         * 如果一直 DQBUF 而不 QBUF，可用缓冲区会耗尽（3个都被应用占着），
         * 驱动无处存放新图像，采集将停止。这里直接复用 DQBUF 返回的 buffer，
         * 其中 index 仍指向刚刚处理完的那个缓冲区。
         */
        if (ioctl(videoFd, VIDIOC_QBUF, &buffer) < 0)
        {
            logIoctlError("VIDIOC_QBUF");
            break; // QBUF 失败 → 停止采集
        }

        /*
         * ===== 8.7 空图像检查 =====
         * 内存不足时 QImage::copy() 会返回空图像（isNull() == true）。
         * 此时不能把空图像交给 QPixmap 或 JPEG 编码器。
         */
        if ((previewThisFrame || sendThisFrame || analyzeThisFrame) &&
            image.isNull())
        {
            // 限速日志输出：最多每秒打印一次警告
            if (!frameCopyWarningPrinted || frameCopyWarningTimer.elapsed() >= 1000)
            {
                qWarning() << "摄像头帧深拷贝失败，可用内存可能不足";
                frameCopyWarningTimer.restart(); // 重启计时器
                frameCopyWarningPrinted = true;  // 标记已打印
            }
            continue; // 跳过本帧，回到 while 循环顶部
        }

        /*
         * ===== 8.8 发射分析帧信号 =====
         * 分析线程接收当前深拷贝图像，执行画面变化检测。
         */
        if (analyzeThisFrame)
        {
            analysisFrameTimer.restart();   // 从本次发送重新计算分析帧间隔
            emit analysisFrameReady(image); // 发射信号 → VisionEventDetector::analyzeFrame()
        }

        /*
         * ===== 8.9 发射本地预览帧信号 =====
         * 本地显示使用当前深拷贝图像。
         * 这个信号发往主线程的 updateLocalPreview() 槽函数，
         * 通过 Qt::QueuedConnection 将 QImage 投递到主线程事件队列。
         */
        if (previewThisFrame)
        {
            localPreviewFrameTimer.restart(); // 从本次发送重新计算预览帧间隔
            emit localPreviewReady(image);    // 发射信号 → CameraStreamingPage::updateLocalPreview()
        }

        /*
         * ===== 8.10 发射网络推流帧信号 =====
         * 网络帧以固定 30 FPS 上限投递给编码发送线程。
         */
        if (sendThisFrame)
        {
            networkFrameTimer.restart();   // 从本次发送重新计算网络帧间隔
            emit networkFrameReady(image); // 发射信号 → TcpFrameSender::sendFrame()
        }

    } // while 循环结束

    /*
     * ===== 第 9 步：资源清理 =====
     * 正常停止（用户点击停止采集）和循环中的错误都汇聚到同一个资源清理出口。
     * cleanup() 执行：STREAMOFF → munmap → REQBUFS(0) → close
     */
    cleanup();

    // 将公开状态同步为"采集已经结束"
    captureEnabled.store(false); // 采集已停止
#endif // __linux__ && __arm__
} // run() 结束
