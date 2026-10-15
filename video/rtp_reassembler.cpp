// 包含本模块的头文件声明
#include "rtp_reassembler.h"

// VideoRtp 命名空间实现
namespace VideoRtp
{
    // 匿名命名空间：内部辅助函数仅在当前 .cpp 文件可见
    namespace
    {

        // 判断序列号 left 是否严格小于 right（处理序列号回绕的正确比较）
        // 使用有符号差值判断：当差值 < 0 时 left 在前面
        // @param left 待比较的序列号
        // @param right 参考序列号
        // @return true 表示 left 在 right 之前
        bool sequenceBefore(quint32 left, quint32 right)
        {
            // 将无符号差转为有符号，负数表示 left < right（处理 32 位回绕场景）
            return qint32(left - right) < 0;
        }

    } // 匿名命名空间

    // 构造函数：初始化超时阈值和所有计数器为零
    // @param timeout 单帧重组超时时间（毫秒），默认 80ms
    Reassembler::Reassembler(qint64 timeout)
        : timeoutMilliseconds(timeout), // 设置超时阈值
          expectedSequence(0),          // 期望序列号初始为 0
          hasExpectedSequence(false),   // 尚未收到有效帧
          pendingDiscontinuity(false),  // 无挂起的不连续标记
          lostFrameCount(0),            // 丢失帧数清零
          lostFragmentCount(0),         // 丢失分包数清零
          duplicateFragmentCount(0)     // 重复分包数清零
    {
    }

    // 重置所有状态：清空组装表、计数器归零
    void Reassembler::reset()
    {
        // 清空所有正在组装的帧
        frames.clear();
        // 尚未收到有效帧
        hasExpectedSequence = false;
        // 无挂起的不连续标记
        pendingDiscontinuity = false;
        // 所有计数器归零
        lostFrameCount = lostFragmentCount = duplicateFragmentCount = 0;
    }

    // 推入一个 RTP 分包到重组器
    // @param header RTP 分包头部
    // @param payload 分包载荷数据
    // @param nowMilliseconds 当前时间戳（毫秒）
    // @return 本次推入后新产生的完整帧列表
    QVector<CompleteFrame> Reassembler::push(const Header &header,
                                             const QByteArray &payload,
                                             qint64 nowMilliseconds)
    {
        // 分包数超过上限或载荷为空：直接触发 drain 并返回（无效数据）
        if (header.fragmentCount > MaximumFragmentsPerFrame || payload.isEmpty())
            return drain(nowMilliseconds);
        // 如果尚未初始化期望序列号，以第一个有效分包的帧序列号为基准
        if (!hasExpectedSequence)
        {
            expectedSequence = header.frameSequence;
            hasExpectedSequence = true;
        }
        // 如果分包的帧序列号小于期望值（过时的分包），跳过并触发 drain
        if (sequenceBefore(header.frameSequence, expectedSequence))
            return drain(nowMilliseconds);

        // 查找该帧序列号对应的组装状态
        auto it = frames.find(header.frameSequence);
        if (it == frames.end())
        {
            // 首次收到该帧：创建新的 Assembly
            Assembly assembly;
            // 预分配分包数组（数量 = 总分包数）
            assembly.fragments.resize(header.fragmentCount);
            // 创建位数组追踪每个分包的到达状态
            assembly.present.resize(header.fragmentCount);
            // 记录发送端时间戳
            assembly.sendTimestampUs = header.sendTimestampUs;
            // 记录首包到达的本地时间
            assembly.firstSeenMilliseconds = nowMilliseconds;
            // 记录是否为关键帧
            assembly.keyFrame = (header.flags & KeyFrameFlag) != 0;
            // 插入到帧组装映射表
            it = frames.insert(header.frameSequence, assembly);
        }
        // 获取组装状态的引用
        Assembly &assembly = it.value();
        // 一致性检查：分包数量或发送时间戳与首包不一致（属于不同帧的同序号冲突）
        if (assembly.fragments.size() != header.fragmentCount ||
            assembly.sendTimestampUs != header.sendTimestampUs)
        {
            // 统计当前已收集但未完成的分包为丢失
            lostFragmentCount += quint64(assembly.fragments.size() - assembly.received);
            // 移除冲突帧
            frames.erase(it);
            // 标记不连续
            pendingDiscontinuity = true;
            // 触发 drain
            return drain(nowMilliseconds);
        }
        // 检查该分包是否已收到（重复分包检测）
        if (assembly.present.testBit(header.fragmentIndex))
            ++duplicateFragmentCount; // 统计重复分包
        else
        {
            // 存储分包载荷到对应位置
            assembly.fragments[header.fragmentIndex] = payload;
            // 标记该分包已到达
            assembly.present.setBit(header.fragmentIndex);
            // 已接收分包计数加 1
            ++assembly.received;
        }
        // 触发 drain：检查是否有帧已完成组装或超时
        return drain(nowMilliseconds);
    }

    // 手动触发过期检查
    // @param nowMilliseconds 当前时间戳（毫秒）
    // @return 超时后产生的帧列表
    QVector<CompleteFrame> Reassembler::expire(qint64 nowMilliseconds)
    {
        // 直接调用 drain 进行超时清理
        return drain(nowMilliseconds);
    }

    // 统一的帧产出函数：产出完整帧、清理超时帧、跳过丢失帧
    // @param nowMilliseconds 当前时间戳（毫秒）
    // @return 产出的完整帧列表
    QVector<CompleteFrame> Reassembler::drain(qint64 nowMilliseconds)
    {
        // 产出结果列表
        QVector<CompleteFrame> completed;
        // 如果尚未初始化期望序列号，直接返回空列表
        if (!hasExpectedSequence)
            return completed;

        // 循环处理从 expectedSequence 开始的帧
        while (!frames.isEmpty())
        {
            // 查找期望序列号对应的帧
            auto current = frames.find(expectedSequence);
            if (current != frames.end())
            {
                // 获取组装状态的引用
                Assembly &assembly = current.value();
                // 如果所有分包已到齐，产出完整帧
                if (assembly.received == assembly.fragments.size())
                {
                    // 构造完整帧结构体
                    CompleteFrame frame;
                    // 设置帧序列号
                    frame.sequence = expectedSequence;
                    // 设置发送端时间戳
                    frame.sendTimestampUs = assembly.sendTimestampUs;
                    // 设置关键帧标志
                    frame.keyFrame = assembly.keyFrame;
                    // 标记是否之前存在不连续
                    frame.discontinuityBefore = pendingDiscontinuity;
                    // 计算重组耗时：当前时间 - 首包到达时间
                    frame.reassemblyMilliseconds =
                        nowMilliseconds - assembly.firstSeenMilliseconds;
                    // 拼接所有分包载荷为完整的 H.264 访问单元
                    for (const QByteArray &fragment : assembly.fragments)
                        frame.accessUnit.append(fragment);
                    // 从映射表中移除已完成的帧
                    frames.erase(current);
                    // 期望序列号递增
                    ++expectedSequence;
                    // 清除不连续标记
                    pendingDiscontinuity = false;
                    // 添加到产出列表
                    completed.append(frame);
                    continue; // 继续处理下一个期望帧
                }
                // 如果该帧已超时（部分分包未到达），强制丢弃
                if (nowMilliseconds - assembly.firstSeenMilliseconds >= timeoutMilliseconds)
                {
                    // 统计缺失的分包数
                    lostFragmentCount += quint64(assembly.fragments.size() - assembly.received);
                    // 统计丢失帧数
                    ++lostFrameCount;
                    // 移除超时帧
                    frames.erase(current);
                    // 期望序列号递增（跳过该帧）
                    ++expectedSequence;
                    // 标记不连续
                    pendingDiscontinuity = true;
                    continue; // 继续处理下一个期望帧
                }
                // 帧未超时且未完成：无法继续，跳出循环等待更多分包
                break;
            }

            // 期望帧在映射表中不存在（可能已在之前被清理或从未收到）
            // 查找映射表中的第一个帧
            auto next = frames.begin();
            // 移除所有序列号小于期望值的残留帧（应已过时）
            while (next != frames.end() && sequenceBefore(next.key(), expectedSequence))
                next = frames.erase(next);
            // 如果映射表已空，或第一个帧的首包时间未超时，无法跳过
            if (next == frames.end() ||
                nowMilliseconds - next.value().firstSeenMilliseconds < timeoutMilliseconds)
                break;
            // 计算跳过的帧数（期望序列号到下一个有效帧之间的差距）
            const quint32 skipped = next.key() - expectedSequence;
            // 统计丢失帧数
            lostFrameCount += skipped;
            // 将期望序列号跳到下一个有效帧
            expectedSequence = next.key();
            // 标记不连续
            pendingDiscontinuity = true;
        }
        return completed;
    }

} // namespace VideoRtp