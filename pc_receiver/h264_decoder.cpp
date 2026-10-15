// 包含本模块的头文件声明
#include "h264_decoder.h"

// C 标准库字符串操作函数（memcpy 等）
#include <cstring>

// FFmpeg C 库头文件，使用 extern "C" 包裹以避免 C++ 名称修饰问题
extern "C"
{
// FFmpeg 编解码核心 API：avcodec_find_decoder、avcodec_open2、avcodec_send_packet、avcodec_receive_frame 等
#include <libavcodec/avcodec.h>
// FFmpeg 错误码定义和错误描述函数：AVERROR、av_strerror
#include <libavutil/error.h>
// FFmpeg 像素格式转换 API：sws_getCachedContext、sws_scale、sws_freeContext
#include <libswscale/swscale.h>
}

// 匿名命名空间：工具函数仅当前 .cpp 文件可见
namespace
{
    // 将 FFmpeg 错误码转换为可读的错误描述字符串
    // @param code FFmpeg API 返回的负值错误码
    // @return 人类可读的错误描述字符串
    QString ffmpegError(int code)
    {
        // FFmpeg 错误描述缓冲区
        char text[AV_ERROR_MAX_STRING_SIZE] = {0};
        // 调用 FFmpeg API 将错误码转为字符串
        av_strerror(code, text, sizeof(text));
        // 转为 QString 返回
        return QString::fromLocal8Bit(text);
    }
}

// 构造函数：所有 FFmpeg 资源指针初始化为 nullptr（惰性初始化）
H264Decoder::H264Decoder()
    : context(nullptr),     // 解码器上下文为空
      frame(nullptr),       // 解码输出帧为空
      packet(nullptr),      // 输入数据包为空
      scaleContext(nullptr) // 像素转换器为空
{
}

// 析构函数：按序释放所有 FFmpeg 资源（先释放像素转换器，再释放包、帧、上下文）
H264Decoder::~H264Decoder()
{
    // 释放像素格式转换上下文（如果存在）
    if (scaleContext)
        sws_freeContext(scaleContext);
    // 释放输入数据包（av_packet_free 同时释放包并置空指针）
    av_packet_free(&packet);
    // 释放解码输出帧
    av_frame_free(&frame);
    // 释放解码器上下文
    avcodec_free_context(&context);
}

// 初始化 FFmpeg H.264 解码器
// @param error 可选输出参数：失败时接收错误描述
// @return true 成功，false 失败
bool H264Decoder::initialize(QString *error)
{
    // 如果已初始化，直接返回成功（幂等性保证）
    if (context)
        return true;
    // 查找 H.264 解码器
    auto codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec)
    {
        // FFmpeg 未编译 H.264 解码支持
        if (error)
            *error = QStringLiteral("FFmpeg 中未找到 H.264 解码器");
        return false;
    }
    // 分配解码器上下文
    context = avcodec_alloc_context3(codec);
    // 分配解码输出帧
    frame = av_frame_alloc();
    // 分配输入数据包
    packet = av_packet_alloc();
    // 检查内存分配是否全部成功
    if (!context || !frame || !packet)
    {
        if (error)
            *error = QStringLiteral("FFmpeg 解码器内存分配失败");
        // 释放可能部分分配的资源
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&context);
        return false;
    }
    // 启用低延迟解码标志
    context->flags |= AV_CODEC_FLAG_LOW_DELAY;
    // 启用快速解码标志
    context->flags2 |= AV_CODEC_FLAG2_FAST;
    // 解码线程数设为 1（单线程保证帧顺序，适合嵌入式场景）
    context->thread_count = 1;
    // 打开解码器（无额外选项）
    const int result = avcodec_open2(context, codec, nullptr);
    if (result < 0)
    {
        // 解码器打开失败
        if (error)
            *error = QStringLiteral("H.264 解码器初始化失败：%1").arg(ffmpegError(result));
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&context);
        return false;
    }
    return true;
}

// 重置解码器内部状态：刷新内部缓冲区
void H264Decoder::reset()
{
    // 刷新解码器内部缓冲区（丢帧后需要清除旧的参考帧和状态）
    if (context)
        avcodec_flush_buffers(context);
}

// 解码一帧 H.264 访问单元为 QImage
// @param accessUnit H.264 访问单元（一帧完整的编码数据）
// @param image 输出参数：解码后的 QImage（RGB888）
// @param error 可选输出参数：失败时接收错误描述
// @return true 成功产生图像，false 失败
bool H264Decoder::decode(const QByteArray &accessUnit, QImage *image, QString *error)
{
    // 输出参数空检查、输入数据空检查、解码器未初始化则尝试初始化
    if (!image || accessUnit.isEmpty() || (!context && !initialize(error)))
        return false;
    // 释放 packet 中的旧数据引用（避免内存泄漏）
    av_packet_unref(packet);
    // 为 packet 分配 accessUnit 大小的新缓冲区
    const int allocation = av_new_packet(packet, accessUnit.size());
    if (allocation < 0)
    {
        // 内存分配失败
        if (error)
            *error = ffmpegError(allocation);
        return false;
    }
    // 将 H.264 码流数据拷贝到 packet 的数据缓冲区
    memcpy(packet->data, accessUnit.constData(), size_t(accessUnit.size()));
    // 将编码数据包送入解码器
    int result = avcodec_send_packet(context, packet);
    if (result < 0)
    {
        // 送入失败
        if (error)
            *error = QStringLiteral("avcodec_send_packet: %1").arg(ffmpegError(result));
        return false;
    }

    // 是否产生了图像输出（一次 send_packet 可能产生多帧）
    bool produced = false;
    // 循环从解码器取出解码后的帧
    while (true)
    {
        // 从解码器接收解码后的帧
        result = avcodec_receive_frame(context, frame);
        // EAGAIN：解码器需要更多输入数据；EOF：已刷新完毕
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
            break;
        if (result < 0)
        {
            // 解码错误
            if (error)
                *error = QStringLiteral("avcodec_receive_frame: %1").arg(ffmpegError(result));
            return false;
        }
        // 创建 RGB888 格式的目标 QImage，尺寸与解码帧一致
        QImage decoded(frame->width, frame->height, QImage::Format_RGB888);
        // 获取或创建像素格式转换上下文（YUV420P → RGB888）
        // sws_getCachedContext 会复用已有 context 如果参数未变
        scaleContext = sws_getCachedContext(
            scaleContext,                              // 现有的转换上下文（可为 nullptr）
            frame->width,                              // 源宽度
            frame->height,                             // 源高度
            static_cast<AVPixelFormat>(frame->format), // 源像素格式（通常为 YUV420P）
            frame->width,                              // 目标宽度（与源相同，不做缩放）
            frame->height,                             // 目标高度
            AV_PIX_FMT_RGB24,                          // 目标像素格式：RGB888
            SWS_FAST_BILINEAR,                         // 快速双线性插值算法
            nullptr, nullptr, nullptr);                // 无额外参数
        // 检查转换上下文和图像是否有效
        if (!scaleContext || decoded.isNull())
        {
            if (error)
                *error = QStringLiteral("解码图像格式转换失败");
            return false;
        }
        // 设置目标数据指针数组：RGB 只有平面 0
        uint8_t *destination[4] = {decoded.bits(), nullptr, nullptr, nullptr};
        // 设置目标行跨度数组
        int lines[4] = {decoded.bytesPerLine(), 0, 0, 0};
        // 执行像素格式转换：YUV420P → RGB888
        sws_scale(scaleContext, frame->data, frame->linesize, 0, frame->height,
                  destination, lines);
        // 将转换后的图像赋给输出参数
        *image = decoded;
        // 标记已产生输出
        produced = true;
    }
    // 返回是否成功产出了图像
    return produced;
}