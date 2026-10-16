// 包含本模块的头文件声明
#include "rtp_packet.h" // 引入自定义视频分片头、常量和函数声明。

// 引入 Qt 数据流类，用于大端字节序的二进制序列化/反序列化
#include <QDataStream> // 以大端顺序序列化和反序列化固定宽度整数。

// VideoRtp 命名空间实现
namespace VideoRtp // 实现项目自定义 RTP 风格数据报协议。
{
    // 构造完整的 RTP 数据报：36 字节大端帧头 + 可变长度载荷
    // @param header RTP 分包头部（所有字段需预先填充）
    // @param payload 指向载荷数据的原始指针
    // @param payloadSize 载荷数据的字节数
    // @return 序列化后的完整数据报 QByteArray，参数无效时返回空数组
    QByteArray makeDatagram(const Header &header, const char *payload, int payloadSize) // header 是元数据，payload/size 是分片内容。
    {
        // 参数校验：载荷指针非空、大小在有效范围内
        if (!payload || payloadSize <= 0 || payloadSize > MaximumPayloadSize) // 指针为空、长度非正或超过 MTU 安全值时拒绝。
            return QByteArray(); // 返回空数组表示调用参数无效。
        // 创建结果字节数组
        QByteArray datagram; // 保存最终的 36 字节头和载荷。
        // 预分配内存：帧头 36 字节 + 载荷大小，避免多次扩容
        datagram.reserve(HeaderSize + payloadSize); // 一次预留准确容量，避免追加时反复分配。
        // 创建数据流对象，绑定到 datagram，写入模式
        QDataStream stream(&datagram, QIODevice::WriteOnly); // 将整数按顺序写入 datagram。
        // 设置为大端字节序（网络字节序标准）
        stream.setByteOrder(QDataStream::BigEndian); // 统一使用网络字节序，独立于两端 CPU 字节序。
        // 按序写入帧头字段：
        // 魔术字(4) + 版本(1) + 标志位(1) + 头大小(2)
        // + 载荷类型(1) + 保留字节(1)
        // + 流ID(4) + 帧序号(4) + 包序号(4)
        // + 分包索引(2) + 总分包数(2) + 发送时间戳(8) + 载荷大小(2)
        stream << Magic << Version << header.flags << HeaderSize // 写入识别字、版本、标志和固定头长度，
               << header.payloadType << quint8(0)                // 写载荷类型及值为 0 的保留字节，
               << header.streamId << header.frameSequence << header.packetSequence // 写会话、帧、包序号，
               << header.fragmentIndex << header.fragmentCount   // 写当前分片位置与总数，
               << header.sendTimestampUs << quint16(payloadSize); // 写时间戳和当前载荷长度。
        // 将载荷数据追加到帧头之后
        datagram.append(payload, payloadSize); // 在协议头之后复制当前分片载荷。
        // 返回完整的数据报
        return datagram; // 返回可直接交给 QUdpSocket 的完整数据报。
    }

    // 解析接收到的 RTP 数据报：反序列化帧头并校验合法性
    // @param datagram 接收到的原始数据报字节数组
    // @param header 输出参数：解析后的 RTP 分包头部
    // @param payload 输出参数：提取出的载荷数据
    // @param error 可选输出参数：解析失败时填入错误描述字符串
    // @return true 解析成功，false 解析失败
    bool parseDatagram(const QByteArray &datagram, Header *header, // datagram 是输入；header 接收元数据。
                       QByteArray *payload, QString *error) // payload 接收载荷；error 可选接收失败原因。
    {
        // 本地 Lambda 函数：统一的失败处理，填充错误信息并返回 false
        auto fail = [error](const QString &message) // 捕获 error 指针；message 是本次校验失败说明。
        {
            if (error)            // 如果调用方提供了 error 参数
                *error = message; // 填入错误描述字符串
            return false;         // 返回失败
        };
        // 输出参数空指针检查
        if (!header || !payload) // 两个必需输出指针都必须有效。
            return fail(QStringLiteral("输出参数为空")); // 统一写错误并返回 false。
        // 数据报长度检查：必须至少包含完整的帧头
        if (datagram.size() < HeaderSize) // 连固定 36 字节头都不完整时无法解析。
            return fail(QStringLiteral("RTP 数据报短于协议头")); // 拒绝短包。

        // 创建数据流，绑定到接收数据报，读取模式
        QDataStream stream(datagram); // 创建只读流，从字节数组开头依次取字段。
        // 设置为大端字节序
        stream.setByteOrder(QDataStream::BigEndian); // 与发送端序列化字节序保持一致。
        // 帧头字段变量（读取前初始化为零）
        quint32 magic = 0;     // 接收并校验四字节魔术字。
        quint8 version = 0;    // 接收并校验协议版本。
        quint16 headerSize = 0; // 接收并校验发送端声明的头长度。
        quint8 reserved = 0;   // 消费保留字节，当前不参与业务逻辑。
        // 用于接收解析结果的临时头部结构体
        Header parsed; // 临时保存未完成全部校验的头部，避免污染调用者输出。
        // 按序读取帧头字段：
        // 魔术字 → 版本 → 标志位 → 头大小 → 载荷类型 → 保留字节
        // → 流ID → 帧序号 → 包序号 → 分包索引 → 总分包数 → 发送时间戳 → 载荷大小
        stream >> magic >> version >> parsed.flags >> headerSize >> parsed.payloadType >> reserved >> parsed.streamId >> parsed.frameSequence >> parsed.packetSequence >> parsed.fragmentIndex >> parsed.fragmentCount >> parsed.sendTimestampUs >> parsed.payloadSize; // 严格按发送端字段顺序读取完整头。

        // 校验数据流状态和帧头基础字段：流未损坏、魔术字匹配、版本匹配、头大小匹配
        if (stream.status() != QDataStream::Ok || magic != Magic || // 数据流必须正常且魔术字匹配；
            version != Version || headerSize != HeaderSize)        // 版本和头长度也必须等于本实现常量。
            return fail(QStringLiteral("RTP 协议头无效"));         // 任一基础字段不符即拒绝。
        // 校验载荷类型：必须是 H.264 (96)
        if (parsed.payloadType != PayloadTypeH264) // 本接收器只实现 H.264 动态载荷。
            return fail(QStringLiteral("不支持的 RTP 载荷类型")); // 拒绝其他媒体类型。
        // 校验载荷大小：非零、不超过最大限制、数据报总长度 = 头 + 载荷大小
        if (parsed.payloadSize == 0 || parsed.payloadSize > MaximumPayloadSize || // 声明长度必须在 1～1200；并且
            datagram.size() != HeaderSize + parsed.payloadSize)                  // 实际数据报必须恰好等于头加载荷。
            return fail(QStringLiteral("RTP 载荷长度无效"));                     // 拒绝截断或额外尾随数据。
        // 校验分包编号：总分包数非零且在限制内、分包索引在有效范围 [0, fragmentCount-1]
        if (parsed.fragmentCount == 0 ||                             // 总分片数不能为 0；
            parsed.fragmentCount > MaximumFragmentsPerFrame ||      // 也不能超过内存安全上限；
            parsed.fragmentIndex >= parsed.fragmentCount)            // 当前索引必须落在 [0,count) 内。
            return fail(QStringLiteral("RTP 分片编号无效"));         // 拒绝无法安全索引的头。
        // 校验 Marker 标志一致性：Marker 标记必须与"是最后一个分包"一致
        const bool marker = (parsed.flags & MarkerFlag) != 0; // marker 保存头中是否声明帧结束。
        if (marker != (parsed.fragmentIndex + 1 == parsed.fragmentCount)) // 标志必须且只能出现在最后一个分片。
            return fail(QStringLiteral("RTP Marker 与末分片不一致")); // 拒绝自相矛盾的数据报。

        // 所有校验通过：将解析结果写入输出参数
        *header = parsed; // 所有校验通过后才把临时头提交给调用者。
        // 从数据报中提取载荷部分（帧头之后的数据）
        *payload = datagram.mid(HeaderSize, parsed.payloadSize); // 复制头之后声明长度的载荷字节。
        return true; // 头与载荷输出均有效。
    }
} // namespace VideoRtp
