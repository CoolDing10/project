/*
 * ============================================================================
 * event_store.cpp — SQLite 事件存储实现
 * ============================================================================
 *
 * 使用 QSqlDatabase + QSQLITE 驱动，直接操作 SQLite 文件。
 * 数据库文件路径：<应用目录>/data/edge_monitor.db
 *
 * 表 monitoring_event 字段说明：
 *   id              — 自增主键
 *   device_id       — 设备标识（如 "imx6ull-001"）
 *   event_type      — 事件类型（当前为 "visual_motion"）
 *   severity        — 0=信息, 1=警告, 2=严重
 *   source          — 来源（如 "camera", "ap3216c", "icm20608", "operator"）
 *   message         — 事件描述
 *   recording_path  — 关联录像文件路径
 *   created_at      — ISO 8601 格式时间戳（含毫秒）
 *   acknowledged    — 是否已确认（保留字段）
 */
#include "event_store.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

EventStore::EventStore()
    /*
     * 每个 EventStore 实例使用独立的 SQLite 连接名。
     * 基于 this 指针的十六进制表示生成唯一标识，
     * 避免与项目中其他可能使用 SQLite 的模块冲突。
     */
    : connectionName(QStringLiteral("edge_events_%1")
                         .arg(quintptr(this), 0, 16))
{
}

EventStore::~EventStore()
{
    if (database.isValid())
        database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
}

bool EventStore::initialize()
{
    // 确保 data 目录存在
    const QString dataDirectoryPath =
        QDir(QCoreApplication::applicationDirPath()).filePath("data");
    if (!QDir().mkpath(dataDirectoryPath))
    {
        errorText = QStringLiteral("无法创建事件数据目录：%1")
                        .arg(dataDirectoryPath);
        qWarning() << errorText;
        return false;
    }

    // 打开 SQLite 数据库
    storagePath = QDir(dataDirectoryPath).filePath("edge_monitor.db");
    database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                         connectionName);
    database.setDatabaseName(storagePath);
    if (!database.open())
    {
        errorText = QStringLiteral("打开事件数据库失败：%1")
                        .arg(database.lastError().text());
        qWarning() << errorText;
        return false;
    }

    QSqlQuery query(database);

    // 建表（IF NOT EXISTS 保证幂等性）
    const QString createTableSql = QStringLiteral(
        "CREATE TABLE IF NOT EXISTS monitoring_event ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "device_id TEXT NOT NULL, "
        "event_type TEXT NOT NULL, "
        "severity INTEGER NOT NULL, "
        "source TEXT NOT NULL, "
        "message TEXT NOT NULL, "
        "recording_path TEXT, "
        "created_at TEXT NOT NULL, "
        "acknowledged INTEGER NOT NULL DEFAULT 0)");
    if (!query.exec(createTableSql))
    {
        errorText = QStringLiteral("创建事件数据表失败：%1")
                        .arg(query.lastError().text());
        qWarning() << errorText;
        return false;
    }

    // 创建时间索引（加速按时间倒序查询）
    if (!query.exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_monitoring_event_created_at "
            "ON monitoring_event(created_at DESC)")))
    {
        qWarning() << "创建事件时间索引失败："
                   << query.lastError().text();
    }

    errorText.clear();
    qInfo() << "边缘监控事件数据库已就绪：" << storagePath;
    return true;
}

bool EventStore::appendEvent(MonitoringEvent *event)
{
    if (!event || !database.isOpen())
    {
        errorText = QStringLiteral("事件数据库未打开");
        return false;
    }

    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "INSERT INTO monitoring_event ("
        "device_id, event_type, severity, source, message, "
        "recording_path, created_at, acknowledged) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(event->deviceId);
    query.addBindValue(event->eventType);
    query.addBindValue(int(event->severity));
    query.addBindValue(event->source);
    query.addBindValue(event->message);
    query.addBindValue(event->recordingPath);
    query.addBindValue(event->createdAt.toString(Qt::ISODateWithMs));
    query.addBindValue(event->acknowledged ? 1 : 0);

    if (!query.exec())
    {
        errorText = QStringLiteral("写入监控事件失败：%1")
                        .arg(query.lastError().text());
        qWarning() << errorText;
        return false;
    }

    // 回填自增 ID（调用方可通过 event->id 获取）
    event->id = query.lastInsertId().toLongLong();
    errorText.clear();
    qInfo() << "监控事件已入库：ID=" << event->id
            << "类型=" << event->eventType
            << "消息=" << event->message;
    return true;
}

QVector<MonitoringEvent> EventStore::recentEvents(int maximumCount) const
{
    QVector<MonitoringEvent> events;
    if (!database.isOpen() || maximumCount <= 0)
        return events;

    // 按 id 倒序查询，取最新的事件
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "SELECT id, device_id, event_type, severity, source, message, "
        "recording_path, created_at, acknowledged "
        "FROM monitoring_event ORDER BY id DESC LIMIT ?"));
    query.addBindValue(maximumCount);
    if (!query.exec())
    {
        errorText = QStringLiteral("读取监控事件失败：%1")
                        .arg(query.lastError().text());
        qWarning() << errorText;
        return events;
    }

    while (query.next())
    {
        MonitoringEvent event;
        event.id = query.value(0).toLongLong();
        event.deviceId = query.value(1).toString();
        event.eventType = query.value(2).toString();
        event.severity = MonitoringEventSeverity(query.value(3).toInt());
        event.source = query.value(4).toString();
        event.message = query.value(5).toString();
        event.recordingPath = query.value(6).toString();
        event.createdAt = QDateTime::fromString(query.value(7).toString(),
                                                Qt::ISODateWithMs);
        event.acknowledged = query.value(8).toBool();
        events.append(event);
    }
    return events;
}

QString EventStore::lastError() const
{
    return errorText;
}

QString EventStore::databasePath() const
{
    return storagePath;
}
