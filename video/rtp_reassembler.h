// 防止头文件被重复包含的头文件保护宏
#ifndef RTP_REASSEMBLER_H // 如果尚未包含本声明，才继续编译。
#define RTP_REASSEMBLER_H // 定义保护宏，防止重组器重复声明。

// 引入 RTP 分包协议定义（Header 结构体、常量、标记位）
#include "rtp_packet.h" // 使用 VideoRtp::Header、分片常量和 QByteArray 载荷。

// Qt 位数组类，用于高效追踪哪些分包已到达（QBitArray::testBit/setBit）
#include <QBitArray> // 以一个 bit 记录对应 fragmentIndex 是否已经到达。
// Qt 有序映射容器，按帧序列号排序存储正在组装的帧
#include <QMap> // 按 32 位帧序号有序保存并发组装中的帧。
// Qt 动态数组容器，用于返回完整帧列表
#include <QVector> // 保存分片数组和一次 drain() 产出的完整帧列表。

// VideoRtp 命名空间：封装 RTP 帧重组逻辑
namespace VideoRtp // 与分片协议共用命名空间，表示重组输入就是 Header。
{
    // 完整帧结构体：重组完成后产生的帧数据及其元数据
    struct CompleteFrame // 交给 H.264 解码器的完整访问单元及统计元数据。
    {
        // H.264 访问单元（重组后的完整编码帧数据）
        QByteArray accessUnit; // 按 fragmentIndex 顺序拼接后的完整 H.264 字节。
        // 帧序列号（从 RTP 包头继承）
        quint32 sequence = 0; // 当前帧序号。
        // 发送端时间戳（微秒），用于计算端到端延迟
        quint64 sendTimestampUs = 0; // 发送端记录的微秒时间戳。
        // 是否为 IDR 关键帧
        bool keyFrame = false; // true 表示可用于解码器重新同步的关键帧。
        // 此帧之前是否发生过帧不连续（丢帧导致解码器需要重新同步）
        bool discontinuityBefore = false; // true 表示前面有帧丢失，调用者应 reset 解码器。
        // 帧重组耗时（毫秒）：从首包到齐到最后一包到达的时间
        qint64 reassemblyMilliseconds = 0; // 从该帧首片到完成重组的本地耗时。
    };

    // RTP 帧重组器：将乱序到达的 RTP 分包按帧序列号重组为完整的 H.264 访问单元
    class Reassembler // 负责乱序分片缓存、按序产帧、超时丢弃和丢包统计。
    {
    public: // 接收器使用的构造、输入、过期处理与计数接口。
        // 构造函数
        // @param timeoutMilliseconds 单帧超时时间（毫秒），超过此时间未收齐分包则标记为丢失，默认 80ms
        explicit Reassembler(qint64 timeoutMilliseconds = 80); // timeoutMilliseconds 是单帧等待上限。

        // 重置重组器状态（清空所有正在组装的帧和计数器）
        void reset(); // 清空挂起帧、序列状态、不连续标记和统计计数。

        // 推入一个 RTP 分包到重组器
        // @param header RTP 分包头部
        // @param payload RTP 分包载荷数据
        // @param nowMilliseconds 当前时间戳（毫秒），用于超时计算
        // @return 本次推入后新产生的完整帧列表（可能为空）
        QVector<CompleteFrame> push(const Header &header, const QByteArray &payload, // header/payload 描述新到分片。
                                    qint64 nowMilliseconds); // nowMilliseconds 用于首片时间和超时判断。

        // 手动触发过期检查：将超时未完成的帧强制产出或丢弃
        // @param nowMilliseconds 当前时间戳（毫秒）
        // @return 超时后产生的完整/不完整帧列表
        QVector<CompleteFrame> expire(qint64 nowMilliseconds); // 在没有新包时也推进超时清理。

        // 累计丢失帧数（只读访问器）
        // @return 丢失帧总数
        quint64 lostFrames() const { return lostFrameCount; } // 返回累计丢弃或跳过的帧数。

        // 累计丢失分包数（只读访问器）
        // @return 丢失分包总数
        quint64 lostFragments() const { return lostFragmentCount; } // 返回超时帧中累计缺失分片数。

        // 累计重复分包数（只读访问器）
        // @return 重复分包总数
        quint64 duplicateFragments() const { return duplicateFragmentCount; } // 返回已经出现过的重复分片数。

    private: // 单帧组装状态、统一产出函数以及跨帧状态。
        // 正在组装的帧状态（内部结构体）
        struct Assembly // 对应某一个 frameSequence 的临时分片集合。
        {
            // 分包数据数组：fragmentIndex 为索引，存储对应分包的载荷数据
            QVector<QByteArray> fragments; // 按 fragmentIndex 定位保存每个载荷。
            // 位数组：present[i] 为 true 表示第 i 个分包已到达
            QBitArray present; // true 表示 fragments 对应槽位已经有效。
            // 已接收的分包数量
            quint16 received = 0; // 不重复分片的实际已收数量。
            // 发送端时间戳（微秒），从第一个到达的分包继承
            quint64 sendTimestampUs = 0; // 用于检查同序号分片元数据一致性。
            // 首包到达时的本地时间（毫秒），用于计算重组耗时和超时判断
            qint64 firstSeenMilliseconds = 0; // 本地收到该帧第一片的时间。
            // 是否为关键帧
            bool keyFrame = false; // 从分片头 KeyFrameFlag 保存的帧类型。
        };

        // 统一的帧产出函数：检查所有挂起的组装，产出完整帧或清理超时帧
        // @param nowMilliseconds 当前时间戳（毫秒）
        // @return 产出的完整帧列表
        QVector<CompleteFrame> drain(qint64 nowMilliseconds); // 按 expectedSequence 产出完成帧并清除超时帧。

        // 挂起的帧组装映射表：键 = 帧序列号，值 = Assembly 组装状态
        QMap<quint32, Assembly> frames; // 键为 frameSequence，值为该帧组装状态。

        // 单帧超时阈值（毫秒）
        qint64 timeoutMilliseconds; // 从首片开始计算的最大等待时间。

        // 期望的下一个帧序列号（顺序递增）
        quint32 expectedSequence; // 下一次允许按序产出或判丢的帧序号。

        // 是否已初始化 expectedSequence（接收到第一个有效帧后设为 true）
        bool hasExpectedSequence; // false 表示还没收到用于建立序列基准的首包。

        // 下一帧是否需要标记 discontinuityBefore = true（发生过丢帧）
        bool pendingDiscontinuity; // 记录下一完整帧前发生过丢失。

        // ==================== 统计计数器 ====================
        // 累计丢失帧数
        quint64 lostFrameCount; // 累计缺失、冲突或超时的帧数量。
        // 累计丢失分包数（收集不完整的帧中缺失的分包数）
        quint64 lostFragmentCount; // 累计未能收齐的分片数量。
        // 累计重复分包数（同一分包被收到多次）
        quint64 duplicateFragmentCount; // 累计已存在槽位再次收到的分片数量。
    };
} // namespace VideoRtp

// 头文件保护宏结束
#endif // RTP_REASSEMBLER_H：结束头文件保护。
