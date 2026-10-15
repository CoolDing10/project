// 防止头文件被重复包含的头文件保护宏
#ifndef RTP_PACKET_H
#define RTP_PACKET_H

// 引入 Qt 字节数组类，用于存储序列化后的数据报和载荷数据
#include <QByteArray>
// 引入 Qt 字符串类，用于错误信息传递
#include <QString>
// 引入 Qt 全局定义（如 quint8/quint16/quint32/quint64 等固定宽度整数类型）
#include <QtGlobal>

// VideoRtp 命名空间：封装自定义 RTP 协议的所有常量和工具函数
namespace VideoRtp
{
    // 魔术字："RTPH"（0x52545048），数据报的起始标识，用于接收端校验数据完整性
    constexpr quint32 Magic = 0x52545048; // "RTPH"
    // 协议版本号：当前为版本 1
    constexpr quint8 Version = 1;
    // 载荷类型号：96 表示 H.264 视频流（遵循 RFC 标准动态载荷类型范围 96-127）
    constexpr quint8 PayloadTypeH264 = 96;
    // RTP 协议头大小：36 字节
    // 布局：magic(4) + version(1) + flags(1) + headerSize(2) + payloadType(1) + reserved(1)
    //       + streamId(4) + frameSequence(4) + packetSequence(4)
    //       + fragmentIndex(2) + fragmentCount(2) + sendTimestampUs(8) + payloadSize(2)
    constexpr quint16 HeaderSize = 36;
    // 每个 RTP 分包的最大载荷大小：1200 字节（确保不超过以太网 MTU 1500 减去 IP/UDP 头）
    constexpr int MaximumPayloadSize = 1200;
    // 单帧允许的最大分包数：2048（防止恶意或异常数据导致无限分包）
    constexpr int MaximumFragmentsPerFrame = 2048;
    // 帧结束标记位：值为 0x01，在 flags 字段中置位表示当前分包是一帧的最后一个分包
    constexpr quint8 MarkerFlag = 0x01;
    // 关键帧标记位：值为 0x02，在 flags 字段中置位表示当前帧是 IDR 关键帧
    constexpr quint8 KeyFrameFlag = 0x02;

    // RTP 分包头部结构体：包含帧重组所需的所有元数据
    struct Header
    {
        // 标志位字节（可按位组合 MarkerFlag | KeyFrameFlag）
        quint8 flags = 0;
        // 载荷类型：96 表示 H.264
        quint8 payloadType = PayloadTypeH264;
        // 流 ID：随机生成，用于接收端区分不同的推流会话
        quint32 streamId = 0;
        // 帧序列号：同一帧的所有分包共享此序号，每发送完一帧递增
        quint32 frameSequence = 0;
        // 包序列号：每发送一个 RTP 分包递增，全局唯一递增
        quint32 packetSequence = 0;
        // 当前分包在帧中的索引（从 0 开始，fragmentCount-1 为最后一个分包）
        quint16 fragmentIndex = 0;
        // 当前帧被拆分成的总分包数
        quint16 fragmentCount = 0;
        // 发送时间戳（微秒），用于接收端计算端到端延迟
        quint64 sendTimestampUs = 0;
        // 当前分包的载荷字节数
        quint16 payloadSize = 0;
    };

    // 构造完整的 RTP 数据报（36 字节头 + 载荷），大端字节序
    // @param header RTP 分包头部元数据（所有字段需预先填充）
    // @param payload 指向载荷数据的指针
    // @param payloadSize 载荷数据的字节数
    // @return 序列化后的完整数据报 QByteArray，参数无效时返回空数组
    QByteArray makeDatagram(const Header &header, const char *payload, int payloadSize);

    // 解析接收到的 RTP 数据报，提取头部和载荷
    // @param datagram 接收到的原始数据报字节数组
    // @param header 输出参数：解析后的 RTP 分包头部
    // @param payload 输出参数：提取出的载荷数据
    // @param error 可选输出参数：解析失败时填入错误描述字符串
    // @return true 解析成功，false 解析失败（error 中包含失败原因）
    bool parseDatagram(const QByteArray &datagram, Header *header,
                       QByteArray *payload, QString *error = nullptr);
} // namespace VideoRtp

// 头文件保护宏结束
#endif // RTP_PACKET_H