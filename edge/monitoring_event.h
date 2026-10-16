/*
 * monitoring_event.h — 边缘监控事件的公共数据模型
 *
 * CameraStreamingPage 生成该结构，EventStore 将它保存到 SQLite，
 * 同时页面会把同一份数据转换为 UI 列表项和 MQTT JSON。
 * 当前实际事件类型只有 visual_motion（画面变化）。
 */
#ifndef MONITORING_EVENT_H // 防止本头文件在同一编译单元中被重复展开。
#define MONITORING_EVENT_H // 定义头文件保护宏。

#include <QDateTime> // QDateTime 保存事件发生时间，并可转换为 ISO 8601 字符串。
#include <QString>   // QString 保存设备编号、事件类型、消息和录像路径。

/** 事件严重程度；枚举值会以整数形式写入 SQLite 和 MQTT。 */
enum class MonitoringEventSeverity
{
    Information = 0, // 普通信息，不代表故障或危险。
    Warning = 1,     // 警告事件；当前画面变化事件使用该级别。
    Critical = 2     // 严重事件，为以后扩展更高等级告警保留。
};

/** 一条完整监控事件；字段与数据库 monitoring_event 表一一对应。 */
struct MonitoringEvent
{
    qint64 id = -1; // SQLite 自增主键；-1 表示对象还没有成功写入数据库。

    QString deviceId; // 产生事件的设备编号，例如 imx6ull-001。

    QString eventType; // 稳定的机器可读事件类型；当前值为 visual_motion。

    MonitoringEventSeverity severity = MonitoringEventSeverity::Information; // 事件等级，默认是普通信息。

    QString source; // 事件来源模块；当前画面变化检测使用 camera。

    QString message; // 面向用户的事件说明，例如变化像素所占百分比。

    QString recordingPath; // 关联的 H.264 事件录像路径；未录像时为空。

    QDateTime createdAt; // 事件发生时间；数据库中保存为带毫秒的 ISO 8601 文本。

    bool acknowledged = false; // 是否已确认；当前 UI 尚未使用，默认未确认。
};

#endif // MONITORING_EVENT_H：结束头文件保护。
