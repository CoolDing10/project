/*
 * ============================================================================
 * event_store.h — SQLite 事件存储
 * ============================================================================
 *
 * 将 MonitoringEvent 持久化到 SQLite 数据库。
 * 数据库文件路径：<应用程序目录>/data/edge_monitor.db
 *
 * 表结构：
 *   monitoring_event(
 *     id INTEGER PRIMARY KEY AUTOINCREMENT,
 *     device_id TEXT, event_type TEXT, severity INTEGER,
 *     source TEXT, message TEXT,
 *     recording_path TEXT, created_at TEXT, acknowledged INTEGER)
 *
 * 使用独立的 SQLite 连接名（基于 this 指针生成），
 * 避免与其他可能使用 SQLite 的模块冲突。
 */
#ifndef EVENT_STORE_H
#define EVENT_STORE_H

#include "monitoring_event.h"

#include <QSqlDatabase>
#include <QVector>

class EventStore final
{
public:
    EventStore();
    ~EventStore();

    bool initialize();                                             // 创建数据库和表
    bool appendEvent(MonitoringEvent *event);                      // 写入一条事件（自动填充 id）
    QVector<MonitoringEvent> recentEvents(int maximumCount) const; // 查询最近事件
    QString lastError() const;                                     // 最后一次错误信息
    QString databasePath() const;                                  // 数据库文件完整路径

private:
    QString connectionName;    // 独立连接名（基于 this 指针 hex）
    QString storagePath;       // 数据库文件路径
    mutable QString errorText; // 最后的错误消息
    QSqlDatabase database;     // SQLite 数据库实例
};

#endif // EVENT_STORE_H
