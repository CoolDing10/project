// 防止头文件被重复包含的头文件保护宏
#ifndef H264_DECODER_H
#define H264_DECODER_H

// Qt 字节数组类，用于传递 H.264 码流数据
#include <QByteArray>
// Qt 图像类，用于返回解码后的视频帧
#include <QImage>
// Qt 字符串类，用于返回错误信息
#include <QString>

// 前向声明：FFmpeg 编解码器上下文，管理 H.264 解码参数和状态
struct AVCodecContext;
// 前向声明：FFmpeg 视频帧，存储解码后的 YUV 原始帧数据
struct AVFrame;
// 前向声明：FFmpeg 数据包，存储输入的 H.264 压缩数据
struct AVPacket;
// 前向声明：FFmpeg 像素格式转换上下文，用于 YUV → RGB 转换
struct SwsContext;

// H.264 解码器类：使用 FFmpeg 将 H.264 Annex B 码流解码为 QImage（RGB888 格式）
class H264Decoder
{
public:
    // 构造函数：初始化所有 FFmpeg 资源指针为 nullptr
    H264Decoder();

    // 析构函数：释放所有 FFmpeg 资源
    ~H264Decoder();

    // 初始化 FFmpeg H.264 解码器（仅在首次解码或 reset 后调用，重复调用无副作用）
    // @param error 可选输出参数：初始化失败时接收错误描述字符串
    // @return true 初始化成功，false 失败
    bool initialize(QString *error = nullptr);

    // 重置解码器内部状态（刷新内部缓冲区），通常在丢帧或流切换时调用
    void reset();

    // 解码一帧 H.264 访问单元
    // @param accessUnit H.264 访问单元（一帧完整的编码数据，可包含多个 NAL 单元）
    // @param image 输出参数：解码后的 QImage（RGB888 格式）
    // @param error 可选输出参数：解码失败时接收错误描述字符串
    // @return true 解码成功并产生图像，false 解码失败
    bool decode(const QByteArray &accessUnit, QImage *image,
                QString *error = nullptr);

private:
    // FFmpeg 解码器上下文：存储解码器配置和状态
    AVCodecContext *context;
    // FFmpeg 解码输出帧：存储解码后的 YUV420P 原始数据
    AVFrame *frame;
    // FFmpeg 输入数据包：存储待解码的 H.264 压缩数据
    AVPacket *packet;
    // FFmpeg 像素格式转换上下文：YUV420P → RGB888 转换
    SwsContext *scaleContext;
};

// 头文件保护宏结束
#endif