/*
 * ============================================================================
 * monitoring_event.h — 监控事件数据模型
 * ============================================================================
 *
 * 定义监控事件的数据结构和严重程度枚举。
 * 被 EventStore(SQLite)、MQTT 推送、事件列表 UI 三者共用。
 *
 * 事件类型示例：
 *   visual_motion    — 画面变化检测
 *   face_detected    — 人脸检测
 *   proximity_alert  — 接近传感器超阈值
 *   vibration_alert  — 异常震动
 *   temperature_alert— 板载温度过高
 *   manual_snapshot  — 用户手动抓拍
 */
#ifndef MONITORING_EVENT_H
#define MONITORING_EVENT_H

#include <QDateTime>
#include <QString>

/*
 * 监控事件严重程度：
 *   Information — 信息（手动抓拍等，不触发告警）
 *   Warning     — 警告（运动/人脸/接近/震动，触发 LED+蜂鸣器）
 *   Critical    — 严重（温度过高，触发 LED+蜂鸣器）
 */
enum class MonitoringEventSeverity
{
    Information = 0,
    Warning = 1,
    Critical = 2
};

/*
 * 一条监控事件的完整记录。
 * 存储在 SQLite 数据库 monitoring_event 表中。
 */
struct MonitoringEvent
{
    qint64 id = -1;                                                          // 数据库自增 ID（-1 表示尚未入库）
    QString deviceId;                                                        // 设备标识（如 "imx6ull-001"）
    QString eventType;                                                       // 事件类型（visual_motion / face_detected 等）
    MonitoringEventSeverity severity = MonitoringEventSeverity::Information; // 严重程度
    QString source;                                                          // 事件来源（camera / ap3216c / icm20608 / operator）
    QString message;                                                         // 人类可读的描述文字
    QString snapshotPath;                                                    // 事件抓拍图片路径（可能为空）
    QString recordingPath;                                                   // 事件录像文件路径（可能为空）
    QDateTime createdAt;                                                     // 事件发生时间
    bool acknowledged = false;                                               // 是否已确认（UI 未使用，保留用于未来扩展）
};

#endif // MONITORING_EVENT_H