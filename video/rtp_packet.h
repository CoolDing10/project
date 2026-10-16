// 防止头文件被重复包含的头文件保护宏
#ifndef RTP_PACKET_H // 防止本协议声明被重复包含。
#define RTP_PACKET_H // 定义头文件保护宏。

// 引入 Qt 字节数组类，用于存储序列化后的数据报和载荷数据
#include <QByteArray> // 保存完整 UDP 数据报和解析后的分片载荷。
// 引入 Qt 字符串类，用于错误信息传递
#include <QString> // parseDatagram() 返回可读的校验错误。
// 引入 Qt 全局定义（如 quint8/quint16/quint32/quint64 等固定宽度整数类型）
#include <QtGlobal> // 提供具有固定宽度的 quint8/16/32/64 类型。

// VideoRtp 命名空间：封装自定义 RTP 协议的所有常量和工具函数
namespace VideoRtp // 隔离项目自定义视频分片协议的类型、常量和函数。
{
    // 魔术字："RTPH"（0x52545048），数据报的起始标识，用于接收端校验数据完整性
    constexpr quint32 Magic = 0x52545048; // "RTPH"
    // 协议版本号：当前为版本 1
    constexpr quint8 Version = 1; // 接收端只接受与此值相同的数据报版本。
    // 载荷类型号：96 表示 H.264 视频流（遵循 RFC 标准动态载荷类型范围 96-127）
    constexpr quint8 PayloadTypeH264 = 96; // 动态载荷类型 96 在本项目中固定表示 H.264。
    // RTP 协议头大小：36 字节
    // 布局：magic(4) + version(1) + flags(1) + headerSize(2) + payloadType(1) + reserved(1)
    //       + streamId(4) + frameSequence(4) + packetSequence(4)
    //       + fragmentIndex(2) + fragmentCount(2) + sendTimestampUs(8) + payloadSize(2)
    constexpr quint16 HeaderSize = 36; // 序列化协议头的固定字节数。
    // 每个 RTP 分包的最大载荷大小：1200 字节（确保不超过以太网 MTU 1500 减去 IP/UDP 头）
    constexpr int MaximumPayloadSize = 1200; // 单个 UDP 包中允许的最大视频载荷字节数。
    // 单帧允许的最大分包数：2048（防止恶意或异常数据导致无限分包）
    constexpr int MaximumFragmentsPerFrame = 2048; // 单帧分片上限，限制内存和恶意输入。
    // 帧结束标记位：值为 0x01，在 flags 字段中置位表示当前分包是一帧的最后一个分包
    constexpr quint8 MarkerFlag = 0x01; // flags 第 0 位表示这是当前帧最后一个分片。
    // 关键帧标记位：值为 0x02，在 flags 字段中置位表示当前帧是 IDR 关键帧
    constexpr quint8 KeyFrameFlag = 0x02; // flags 第 1 位表示所属帧为关键帧。

    // RTP 分包头部结构体：包含帧重组所需的所有元数据
    struct Header // 一个 UDP 分片随附的重组、统计和时间元数据。
    {
        // 标志位字节（可按位组合 MarkerFlag | KeyFrameFlag）
        quint8 flags = 0; // MarkerFlag 和 KeyFrameFlag 的按位组合。
        // 载荷类型：96 表示 H.264
        quint8 payloadType = PayloadTypeH264; // 默认按 H.264 动态载荷类型初始化。
        // 流 ID：随机生成，用于接收端区分不同的推流会话
        quint32 streamId = 0; // 一次推流会话内固定，配置变化时重新生成。
        // 帧序列号：同一帧的所有分包共享此序号，每发送完一帧递增
        quint32 frameSequence = 0; // 同帧各分片相同，每完成一帧递增。
        // 包序列号：每发送一个 RTP 分包递增，全局唯一递增
        quint32 packetSequence = 0; // 每个数据报唯一递增，用于包级丢失观察。
        // 当前分包在帧中的索引（从 0 开始，fragmentCount-1 为最后一个分包）
        quint16 fragmentIndex = 0; // 当前载荷在帧内的零起始位置编号。
        // 当前帧被拆分成的总分包数
        quint16 fragmentCount = 0; // 该帧预期收齐的分片总数。
        // 发送时间戳（微秒），用于接收端计算端到端延迟
        quint64 sendTimestampUs = 0; // 发送端 Unix 微秒时间戳。
        // 当前分包的载荷字节数
        quint16 payloadSize = 0; // 当前分片头之后实际有效载荷长度。
    };

    // 构造完整的 RTP 数据报（36 字节头 + 载荷），大端字节序
    // @param header RTP 分包头部元数据（所有字段需预先填充）
    // @param payload 指向载荷数据的指针
    // @param payloadSize 载荷数据的字节数
    // @return 序列化后的完整数据报 QByteArray，参数无效时返回空数组
    QByteArray makeDatagram(const Header &header, const char *payload, int payloadSize); // 序列化 header 和 payloadSize 字节载荷。

    // 解析接收到的 RTP 数据报，提取头部和载荷
    // @param datagram 接收到的原始数据报字节数组
    // @param header 输出参数：解析后的 RTP 分包头部
    // @param payload 输出参数：提取出的载荷数据
    // @param error 可选输出参数：解析失败时填入错误描述字符串
    // @return true 解析成功，false 解析失败（error 中包含失败原因）
    bool parseDatagram(const QByteArray &datagram, Header *header, // datagram 是输入，header 接收解析元数据。
                       QByteArray *payload, QString *error = nullptr); // payload 接收载荷，error 可选接收失败原因。
} // namespace VideoRtp

// 头文件保护宏结束
#endif // RTP_PACKET_H：结束头文件保护。
