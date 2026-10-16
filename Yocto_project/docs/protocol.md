# TCP 协议

所有整数均为网络字节序。TCP 流由 16 字节公共头和可变载荷组成。

| 字段 | 字节 | 说明 |
|---|---:|---|
| magic | 4 | `AVTS` (`0x41565453`) |
| version | 2 | 当前为 1 |
| type | 2 | 1=Video，2=Feedback |
| payload_size | 4 | 最大 8 MiB |
| sequence | 4 | 相应消息类型内递增序号 |

Video 载荷依次包含：采集时间戳 us(8)、宽(2)、高(2)、目标码率(4)、GOP(2)、
FPS(2)、关键帧标志(2)、H.264 长度(4)、Annex-B H.264 access unit。

Feedback 载荷依次包含：报告时间戳 us(8)、统计周期 ms(4)、接收字节(8)、解码帧数
(4)、丢帧数(4)、卡顿事件数(4)、解码延迟 us(4)、端到端延迟 us(4)、抖动 us(4)、
接收缓冲 ms(4)、最后视频序号(4)。

接收方必须处理 TCP 粘包/拆包。非法 magic、版本、类型或长度会触发重新同步；不能
直接把网络数据解释为 C/C++ packed struct。

端到端延迟使用两端 wall clock，因此设备和 PC 应通过 chrony/NTP 同步。抖动只使用
相邻 transit time 的差值，对固定时钟偏移不敏感。

