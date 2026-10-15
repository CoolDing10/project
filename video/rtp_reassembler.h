// 防止头文件被重复包含的头文件保护宏
#ifndef RTP_REASSEMBLER_H
#define RTP_REASSEMBLER_H

// 引入 RTP 分包协议定义（Header 结构体、常量、标记位）
#include "rtp_packet.h"

// Qt 位数组类，用于高效追踪哪些分包已到达（QBitArray::testBit/setBit）
#include <QBitArray>
// Qt 有序映射容器，按帧序列号排序存储正在组装的帧
#include <QMap>
// Qt 动态数组容器，用于返回完整帧列表
#include <QVector>

// VideoRtp 命名空间：封装 RTP 帧重组逻辑
namespace VideoRtp
{
    // 完整帧结构体：重组完成后产生的帧数据及其元数据
    struct CompleteFrame
    {
        // H.264 访问单元（重组后的完整编码帧数据）
        QByteArray accessUnit;
        // 帧序列号（从 RTP 包头继承）
        quint32 sequence = 0;
        // 发送端时间戳（微秒），用于计算端到端延迟
        quint64 sendTimestampUs = 0;
        // 是否为 IDR 关键帧
        bool keyFrame = false;
        // 此帧之前是否发生过帧不连续（丢帧导致解码器需要重新同步）
        bool discontinuityBefore = false;
        // 帧重组耗时（毫秒）：从首包到齐到最后一包到达的时间
        qint64 reassemblyMilliseconds = 0;
    };

    // RTP 帧重组器：将乱序到达的 RTP 分包按帧序列号重组为完整的 H.264 访问单元
    class Reassembler
    {
    public:
        // 构造函数
        // @param timeoutMilliseconds 单帧超时时间（毫秒），超过此时间未收齐分包则标记为丢失，默认 80ms
        explicit Reassembler(qint64 timeoutMilliseconds = 80);

        // 重置重组器状态（清空所有正在组装的帧和计数器）
        void reset();

        // 推入一个 RTP 分包到重组器
        // @param header RTP 分包头部
        // @param payload RTP 分包载荷数据
        // @param nowMilliseconds 当前时间戳（毫秒），用于超时计算
        // @return 本次推入后新产生的完整帧列表（可能为空）
        QVector<CompleteFrame> push(const Header &header, const QByteArray &payload,
                                    qint64 nowMilliseconds);

        // 手动触发过期检查：将超时未完成的帧强制产出或丢弃
        // @param nowMilliseconds 当前时间戳（毫秒）
        // @return 超时后产生的完整/不完整帧列表
        QVector<CompleteFrame> expire(qint64 nowMilliseconds);

        // 累计丢失帧数（只读访问器）
        // @return 丢失帧总数
        quint64 lostFrames() const { return lostFrameCount; }

        // 累计丢失分包数（只读访问器）
        // @return 丢失分包总数
        quint64 lostFragments() const { return lostFragmentCount; }

        // 累计重复分包数（只读访问器）
        // @return 重复分包总数
        quint64 duplicateFragments() const { return duplicateFragmentCount; }

    private:
        // 正在组装的帧状态（内部结构体）
        struct Assembly
        {
            // 分包数据数组：fragmentIndex 为索引，存储对应分包的载荷数据
            QVector<QByteArray> fragments;
            // 位数组：present[i] 为 true 表示第 i 个分包已到达
            QBitArray present;
            // 已接收的分包数量
            quint16 received = 0;
            // 发送端时间戳（微秒），从第一个到达的分包继承
            quint64 sendTimestampUs = 0;
            // 首包到达时的本地时间（毫秒），用于计算重组耗时和超时判断
            qint64 firstSeenMilliseconds = 0;
            // 是否为关键帧
            bool keyFrame = false;
        };

        // 统一的帧产出函数：检查所有挂起的组装，产出完整帧或清理超时帧
        // @param nowMilliseconds 当前时间戳（毫秒）
        // @return 产出的完整帧列表
        QVector<CompleteFrame> drain(qint64 nowMilliseconds);

        // 挂起的帧组装映射表：键 = 帧序列号，值 = Assembly 组装状态
        QMap<quint32, Assembly> frames;

        // 单帧超时阈值（毫秒）
        qint64 timeoutMilliseconds;

        // 期望的下一个帧序列号（顺序递增）
        quint32 expectedSequence;

        // 是否已初始化 expectedSequence（接收到第一个有效帧后设为 true）
        bool hasExpectedSequence;

        // 下一帧是否需要标记 discontinuityBefore = true（发生过丢帧）
        bool pendingDiscontinuity;

        // ==================== 统计计数器 ====================
        // 累计丢失帧数
        quint64 lostFrameCount;
        // 累计丢失分包数（收集不完整的帧中缺失的分包数）
        quint64 lostFragmentCount;
        // 累计重复分包数（同一分包被收到多次）
        quint64 duplicateFragmentCount;
    };
} // namespace VideoRtp

// 头文件保护宏结束
#endif