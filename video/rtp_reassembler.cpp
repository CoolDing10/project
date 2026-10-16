// 包含本模块的头文件声明
#include "rtp_reassembler.h" // 引入 Reassembler、Assembly 和 CompleteFrame 声明。

// VideoRtp 命名空间实现
namespace VideoRtp // 实现自定义 UDP 视频分片的顺序重组逻辑。
{
    // 匿名命名空间：内部辅助函数仅在当前 .cpp 文件可见
    namespace // 内层匿名命名空间隐藏序号比较辅助函数。
    {

        // 判断序列号 left 是否严格小于 right（处理序列号回绕的正确比较）
        // 使用有符号差值判断：当差值 < 0 时 left 在前面
        // @param left 待比较的序列号
        // @param right 参考序列号
        // @return true 表示 left 在 right 之前
        bool sequenceBefore(quint32 left, quint32 right) // left/right 是允许 32 位回绕的帧序号。
        {
            // 将无符号差转为有符号，负数表示 left < right（处理 32 位回绕场景）
            return qint32(left - right) < 0; // 差值解释为有符号数，负值表示 left 在 right 前面。
        }

    } // 匿名命名空间

    // 构造函数：初始化超时阈值和所有计数器为零
    // @param timeout 单帧重组超时时间（毫秒），默认 80ms
    Reassembler::Reassembler(qint64 timeout) // timeout 是单帧从首片开始等待的最大毫秒数。
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
    void Reassembler::reset() // 开始新流时恢复到未收到任何序号的初始状态。
    {
        // 清空所有正在组装的帧
        frames.clear(); // 释放全部未完成帧及其分片 QByteArray。
        // 尚未收到有效帧
        hasExpectedSequence = false; // 下一有效分片将重新建立序号基准。
        // 无挂起的不连续标记
        pendingDiscontinuity = false; // 新流开始前不携带旧流的丢失标记。
        // 所有计数器归零
        lostFrameCount = lostFragmentCount = duplicateFragmentCount = 0; // 同时清零三项累计统计。
    }

    // 推入一个 RTP 分包到重组器
    // @param header RTP 分包头部
    // @param payload 分包载荷数据
    // @param nowMilliseconds 当前时间戳（毫秒）
    // @return 本次推入后新产生的完整帧列表
    QVector<CompleteFrame> Reassembler::push(const Header &header, // header 含帧号和分片位置。
                                             const QByteArray &payload, // payload 是当前分片的 H.264 字节。
                                             qint64 nowMilliseconds) // 当前本地毫秒时间用于超时。
    {
        // 分包数超过上限或载荷为空：直接触发 drain 并返回（无效数据）
        if (header.fragmentCount > MaximumFragmentsPerFrame || payload.isEmpty()) // 防御绕过解析器的异常输入。
            return drain(nowMilliseconds); // 不保存无效分片，但仍推进已有帧的超时处理。
        // 如果尚未初始化期望序列号，以第一个有效分包的帧序列号为基准
        if (!hasExpectedSequence) // 第一个有效分片建立输出顺序起点。
        {
            expectedSequence = header.frameSequence; // 从该分片所属帧开始等待。
            hasExpectedSequence = true;              // 后续启用过时分片判断。
        }
        // 如果分包的帧序列号小于期望值（过时的分包），跳过并触发 drain
        if (sequenceBefore(header.frameSequence, expectedSequence)) // 已经判定处理过或跳过的旧帧分片。
            return drain(nowMilliseconds); // 丢弃旧包并继续检查当前挂起帧。

        // 查找该帧序列号对应的组装状态
        auto it = frames.find(header.frameSequence); // it 指向该帧现有 Assembly 或 end()。
        if (it == frames.end())                      // 首次见到此帧序号时创建状态。
        {
            // 首次收到该帧：创建新的 Assembly
            Assembly assembly; // 栈上建立初始组装对象，随后复制进 QMap。
            // 预分配分包数组（数量 = 总分包数）
            assembly.fragments.resize(header.fragmentCount); // 为每个分片索引预留一个 QByteArray 槽。
            // 创建位数组追踪每个分包的到达状态
            assembly.present.resize(header.fragmentCount); // 位数组初始全 false，表示尚未收到。
            // 记录发送端时间戳
            assembly.sendTimestampUs = header.sendTimestampUs; // 保存用于一致性检查和端到端延迟。
            // 记录首包到达的本地时间
            assembly.firstSeenMilliseconds = nowMilliseconds; // 超时从第一片实际到达时开始计算。
            // 记录是否为关键帧
            assembly.keyFrame = (header.flags & KeyFrameFlag) != 0; // 解析当前帧关键帧属性。
            // 插入到帧组装映射表
            it = frames.insert(header.frameSequence, assembly); // 插入映射并取得可修改迭代器。
        }
        // 获取组装状态的引用
        Assembly &assembly = it.value(); // 引用 map 内对象，后续修改直接保留。
        // 一致性检查：分包数量或发送时间戳与首包不一致（属于不同帧的同序号冲突）
        if (assembly.fragments.size() != header.fragmentCount || // 同帧序号声明的总片数必须一致；并且
            assembly.sendTimestampUs != header.sendTimestampUs)  // 发送时间戳也必须一致。
        {
            // 统计当前已收集但未完成的分包为丢失
            lostFragmentCount += quint64(assembly.fragments.size() - assembly.received); // 把旧 Assembly 未到片计入丢失。
            // 移除冲突帧
            frames.erase(it); // 删除元数据冲突的整帧，防止混合不同内容。
            // 标记不连续
            pendingDiscontinuity = true; // 通知下一完整帧前存在断层。
            // 触发 drain
            return drain(nowMilliseconds); // 立即尝试推进后续序号或超时。
        }
        // 检查该分包是否已收到（重复分包检测）
        if (assembly.present.testBit(header.fragmentIndex)) // 对应索引位已经为 true 表示重复到达。
            ++duplicateFragmentCount; // 统计重复分包
        else // 分包序号不连续时，累计缺失的分片数量。
        {
            // 存储分包载荷到对应位置
            assembly.fragments[header.fragmentIndex] = payload; // 将载荷放到准确顺序槽位。
            // 标记该分包已到达
            assembly.present.setBit(header.fragmentIndex); // 标记此槽位有效。
            // 已接收分包计数加 1
            ++assembly.received; // 仅首次收到该索引时增加计数。
        }
        // 触发 drain：检查是否有帧已完成组装或超时
        return drain(nowMilliseconds); // 检查期望帧是否已收齐或已有帧超时。
    }

    // 手动触发过期检查
    // @param nowMilliseconds 当前时间戳（毫秒）
    // @return 超时后产生的帧列表
    QVector<CompleteFrame> Reassembler::expire(qint64 nowMilliseconds) // nowMilliseconds 是定时器触发时的本地时间。
    {
        // 直接调用 drain 进行超时清理
        return drain(nowMilliseconds); // 不加入新分片，仅运行相同产出与清理状态机。
    }

    // 统一的帧产出函数：产出完整帧、清理超时帧、跳过丢失帧
    // @param nowMilliseconds 当前时间戳（毫秒）
    // @return 产出的完整帧列表
    QVector<CompleteFrame> Reassembler::drain(qint64 nowMilliseconds) // 按序产出完成帧并判定超时丢失。
    {
        // 产出结果列表
        QVector<CompleteFrame> completed; // 收集本次调用新产出的零到多帧。
        // 如果尚未初始化期望序列号，直接返回空列表
        if (!hasExpectedSequence) // 尚无序列基准时无法判断任何帧顺序。
            return completed;     // 返回空列表。

        // 循环处理从 expectedSequence 开始的帧
        while (!frames.isEmpty()) // 只要挂起表中还有帧，就尝试从 expectedSequence 推进。
        {
            // 查找期望序列号对应的帧
            auto current = frames.find(expectedSequence); // 查找当前唯一允许首先处理的帧。
            if (current != frames.end())                   // 期望帧至少收到过一个分片。
            {
                // 获取组装状态的引用
                Assembly &assembly = current.value(); // 引用期望帧的组装状态。
                // 如果所有分包已到齐，产出完整帧
                if (assembly.received == assembly.fragments.size()) // 已收数量等于总槽位数表示完整。
                {
                    // 构造完整帧结构体
                    CompleteFrame frame; // 创建将返回给解码层的结果结构。
                    // 设置帧序列号
                    frame.sequence = expectedSequence; // 保存当前有序帧号。
                    // 设置发送端时间戳
                    frame.sendTimestampUs = assembly.sendTimestampUs; // 传递发送端时间戳。
                    // 设置关键帧标志
                    frame.keyFrame = assembly.keyFrame; // 传递关键帧属性。
                    // 标记是否之前存在不连续
                    frame.discontinuityBefore = pendingDiscontinuity; // 告诉解码层是否需先 reset。
                    // 计算重组耗时：当前时间 - 首包到达时间
                    frame.reassemblyMilliseconds = // 保存从首片到本次产出的耗时，
                        nowMilliseconds - assembly.firstSeenMilliseconds; // 单位毫秒。
                    // 拼接所有分包载荷为完整的 H.264 访问单元
                    for (const QByteArray &fragment : assembly.fragments) // 按索引从 0 到末尾遍历载荷，
                        frame.accessUnit.append(fragment); // 依次拼接成原始 H.264 访问单元。
                    // 从映射表中移除已完成的帧
                    frames.erase(current); // 完整帧已转移到结果，不再占用挂起内存。
                    // 期望序列号递增
                    ++expectedSequence; // 下一循环处理紧接着的帧，溢出时自然回绕。
                    // 清除不连续标记
                    pendingDiscontinuity = false; // 断层信息已经附加到本帧，只消费一次。
                    // 添加到产出列表
                    completed.append(frame); // 将完整帧加入返回列表。
                    continue; // 继续处理下一个期望帧
                }
                // 如果该帧已超时（部分分包未到达），强制丢弃
                if (nowMilliseconds - assembly.firstSeenMilliseconds >= timeoutMilliseconds) // 未完成期望帧已达到等待上限。
                {
                    // 统计缺失的分包数
                    lostFragmentCount += quint64(assembly.fragments.size() - assembly.received); // 累加该帧缺少的槽位数。
                    // 统计丢失帧数
                    ++lostFrameCount; // 整个访问单元无法解码，丢帧数加一。
                    // 移除超时帧
                    frames.erase(current); // 释放已收到但不完整的分片。
                    // 期望序列号递增（跳过该帧）
                    ++expectedSequence; // 跳过丢失帧继续等待下一帧。
                    // 标记不连续
                    pendingDiscontinuity = true; // 下一完整帧前需要重置解码器参考状态。
                    continue; // 继续处理下一个期望帧
                }
                // 帧未超时且未完成：无法继续，跳出循环等待更多分包
                break; // 期望帧仍在期限内，只能等待后续分片。
            }

            // 期望帧在映射表中不存在（可能已在之前被清理或从未收到）
            // 查找映射表中的第一个帧
            auto next = frames.begin(); // QMap 首项是当前存储的最小普通序号。
            // 移除所有序列号小于期望值的残留帧（应已过时）
            while (next != frames.end() && sequenceBefore(next.key(), expectedSequence)) // 清除迟到的旧帧状态。
                next = frames.erase(next); // erase 返回下一个有效迭代器。
            // 如果映射表已空，或第一个帧的首包时间未超时，无法跳过
            if (next == frames.end() || // 没有更后面的帧可证明期望帧缺失；或者
                nowMilliseconds - next.value().firstSeenMilliseconds < timeoutMilliseconds) // 后续帧也还年轻时，
                break; // 继续等待期望帧到达，暂不判丢。
            // 计算跳过的帧数（期望序列号到下一个有效帧之间的差距）
            const quint32 skipped = next.key() - expectedSequence; // 计算缺失序号区间内的帧数。
            // 统计丢失帧数
            lostFrameCount += skipped; // 把完全没收到任何分片的帧计入丢帧统计。
            // 将期望序列号跳到下一个有效帧
            expectedSequence = next.key(); // 跳到已经存在且等待已久的下一帧。
            // 标记不连续
            pendingDiscontinuity = true; // 下一产出帧必须携带序列断层标志。
        }
        return completed; // 返回本次按顺序产出的全部帧。
    }

} // namespace VideoRtp
