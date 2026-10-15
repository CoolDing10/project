// 包含本模块的头文件声明
#include "rtp_packet.h"

// 引入 Qt 数据流类，用于大端字节序的二进制序列化/反序列化
#include <QDataStream>

// VideoRtp 命名空间实现
namespace VideoRtp
{
    // 构造完整的 RTP 数据报：36 字节大端帧头 + 可变长度载荷
    // @param header RTP 分包头部（所有字段需预先填充）
    // @param payload 指向载荷数据的原始指针
    // @param payloadSize 载荷数据的字节数
    // @return 序列化后的完整数据报 QByteArray，参数无效时返回空数组
    QByteArray makeDatagram(const Header &header, const char *payload, int payloadSize)
    {
        // 参数校验：载荷指针非空、大小在有效范围内
        if (!payload || payloadSize <= 0 || payloadSize > MaximumPayloadSize)
            return QByteArray();
        // 创建结果字节数组
        QByteArray datagram;
        // 预分配内存：帧头 36 字节 + 载荷大小，避免多次扩容
        datagram.reserve(HeaderSize + payloadSize);
        // 创建数据流对象，绑定到 datagram，写入模式
        QDataStream stream(&datagram, QIODevice::WriteOnly);
        // 设置为大端字节序（网络字节序标准）
        stream.setByteOrder(QDataStream::BigEndian);
        // 按序写入帧头字段：
        // 魔术字(4) + 版本(1) + 标志位(1) + 头大小(2)
        // + 载荷类型(1) + 保留字节(1)
        // + 流ID(4) + 帧序号(4) + 包序号(4)
        // + 分包索引(2) + 总分包数(2) + 发送时间戳(8) + 载荷大小(2)
        stream << Magic << Version << header.flags << HeaderSize
               << header.payloadType << quint8(0) // quint8(0) 为保留字节
               << header.streamId << header.frameSequence << header.packetSequence
               << header.fragmentIndex << header.fragmentCount
               << header.sendTimestampUs << quint16(payloadSize);
        // 将载荷数据追加到帧头之后
        datagram.append(payload, payloadSize);
        // 返回完整的数据报
        return datagram;
    }

    // 解析接收到的 RTP 数据报：反序列化帧头并校验合法性
    // @param datagram 接收到的原始数据报字节数组
    // @param header 输出参数：解析后的 RTP 分包头部
    // @param payload 输出参数：提取出的载荷数据
    // @param error 可选输出参数：解析失败时填入错误描述字符串
    // @return true 解析成功，false 解析失败
    bool parseDatagram(const QByteArray &datagram, Header *header,
                       QByteArray *payload, QString *error)
    {
        // 本地 Lambda 函数：统一的失败处理，填充错误信息并返回 false
        auto fail = [error](const QString &message)
        {
            if (error)            // 如果调用方提供了 error 参数
                *error = message; // 填入错误描述字符串
            return false;         // 返回失败
        };
        // 输出参数空指针检查
        if (!header || !payload)
            return fail(QStringLiteral("输出参数为空"));
        // 数据报长度检查：必须至少包含完整的帧头
        if (datagram.size() < HeaderSize)
            return fail(QStringLiteral("RTP 数据报短于协议头"));

        // 创建数据流，绑定到接收数据报，读取模式
        QDataStream stream(datagram);
        // 设置为大端字节序
        stream.setByteOrder(QDataStream::BigEndian);
        // 帧头字段变量（读取前初始化为零）
        quint32 magic = 0;
        quint8 version = 0;
        quint16 headerSize = 0;
        quint8 reserved = 0;
        // 用于接收解析结果的临时头部结构体
        Header parsed;
        // 按序读取帧头字段：
        // 魔术字 → 版本 → 标志位 → 头大小 → 载荷类型 → 保留字节
        // → 流ID → 帧序号 → 包序号 → 分包索引 → 总分包数 → 发送时间戳 → 载荷大小
        stream >> magic >> version >> parsed.flags >> headerSize >> parsed.payloadType >> reserved >> parsed.streamId >> parsed.frameSequence >> parsed.packetSequence >> parsed.fragmentIndex >> parsed.fragmentCount >> parsed.sendTimestampUs >> parsed.payloadSize;

        // 校验数据流状态和帧头基础字段：流未损坏、魔术字匹配、版本匹配、头大小匹配
        if (stream.status() != QDataStream::Ok || magic != Magic ||
            version != Version || headerSize != HeaderSize)
            return fail(QStringLiteral("RTP 协议头无效"));
        // 校验载荷类型：必须是 H.264 (96)
        if (parsed.payloadType != PayloadTypeH264)
            return fail(QStringLiteral("不支持的 RTP 载荷类型"));
        // 校验载荷大小：非零、不超过最大限制、数据报总长度 = 头 + 载荷大小
        if (parsed.payloadSize == 0 || parsed.payloadSize > MaximumPayloadSize ||
            datagram.size() != HeaderSize + parsed.payloadSize)
            return fail(QStringLiteral("RTP 载荷长度无效"));
        // 校验分包编号：总分包数非零且在限制内、分包索引在有效范围 [0, fragmentCount-1]
        if (parsed.fragmentCount == 0 ||
            parsed.fragmentCount > MaximumFragmentsPerFrame ||
            parsed.fragmentIndex >= parsed.fragmentCount)
            return fail(QStringLiteral("RTP 分片编号无效"));
        // 校验 Marker 标志一致性：Marker 标记必须与"是最后一个分包"一致
        const bool marker = (parsed.flags & MarkerFlag) != 0;
        if (marker != (parsed.fragmentIndex + 1 == parsed.fragmentCount))
            return fail(QStringLiteral("RTP Marker 与末分片不一致"));

        // 所有校验通过：将解析结果写入输出参数
        *header = parsed;
        // 从数据报中提取载荷部分（帧头之后的数据）
        *payload = datagram.mid(HeaderSize, parsed.payloadSize);
        return true;
    }
} // namespace VideoRtp