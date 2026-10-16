/*
 * event_store.cpp — SQLite 事件存储实现
 *
 * 初始化流程：
 *   创建 data 目录 → 建立 QSQLITE 连接 → 打开 edge_monitor.db
 *   → 创建 monitoring_event 表 → 创建时间索引。
 *
 * 写入时使用 SQL 占位符和 addBindValue()，避免手工拼接文本造成转义问题。
 * 读取时按 id 倒序查询，因此数组第一项始终是最近写入的事件。
 */
#include "event_store.h" // 引入 EventStore 类和 MonitoringEvent 数据结构声明。

#include <QCoreApplication> // 获取可执行程序所在目录 applicationDirPath()。
#include <QDebug>           // 输出 qInfo() 和 qWarning() 运行日志。
#include <QDir>             // 创建 data 目录并安全拼接文件路径。
#include <QSqlError>        // 从数据库或 SQL 查询对象中取得错误文本。
#include <QSqlQuery>        // 准备、绑定并执行 SQLite SQL 语句。
#include <QVariant>         // 接收 QSqlQuery::value() 和 lastInsertId() 的通用值。

/**
 * 为当前对象创建唯一连接名。
 *
 * QStringLiteral("edge_events_%1") 是连接名模板；
 * quintptr(this) 把对象地址转换成可打印整数；
 * arg(..., 0, 16) 使用自动字段宽度和十六进制格式替换 %1。
 * 不同 EventStore 实例地址不同，因此不会误用彼此的 Qt SQL 连接。
 */
EventStore::EventStore()
    : connectionName(QStringLiteral("edge_events_%1") // 初始化唯一连接名的固定前缀。
                         .arg(quintptr(this), 0, 16))  // 把当前对象地址追加为十六进制后缀。
{
} // 构造时不打开数据库，实际 I/O 延迟到 initialize()。

/**
 * 释放数据库连接。
 *
 * removeDatabase() 前必须先销毁/清空所有引用该连接的 QSqlDatabase 句柄，
 * 所以先 close()，再把成员赋成默认对象，最后从全局连接表移除连接名。
 */
EventStore::~EventStore()
{
    if (database.isValid())       // 只有已经通过 addDatabase() 创建的句柄才需要关闭。
        database.close();         // 关闭 SQLite 文件并结束该连接上的数据库操作。
    database = QSqlDatabase();    // 清空成员句柄，解除它对 connectionName 连接的引用。
    QSqlDatabase::removeDatabase(connectionName); // 从 Qt 全局连接表删除本对象的连接。
}

/**
 * 初始化事件数据库、数据表和索引。
 *
 * @return true 表示目录、数据库和数据表均可使用；
 *         false 表示关键步骤失败，详细原因保存在 errorText。
 */
bool EventStore::initialize()
{
    const QString dataDirectoryPath =                         // 保存数据库目录的完整路径。
        QDir(QCoreApplication::applicationDirPath())          // 以可执行程序所在目录作为根目录。
            .filePath("data");                                // 在根目录下拼接 data 子目录。
    if (!QDir().mkpath(dataDirectoryPath))                    // 递归创建目录；已存在也会返回 true。
    {
        errorText = QStringLiteral("无法创建事件数据目录：%1") // 构造面向用户的错误说明。
                        .arg(dataDirectoryPath);               // 把失败的实际目录替换到 %1。
        qWarning() << errorText;                               // 把错误同时写到开发板终端日志。
        return false;                                         // 没有目录就无法继续创建数据库文件。
    }

    storagePath = QDir(dataDirectoryPath)                      // 使用已经确认存在的 data 目录。
                      .filePath("edge_monitor.db");                 // 得到 SQLite 文件完整路径。
    database = QSqlDatabase::addDatabase(                      // 向 Qt SQL 系统注册一个数据库连接。
        QStringLiteral("QSQLITE"),                             // 指定使用 Qt 的 SQLite 驱动。
        connectionName);                                      // 指定本对象专属的连接名。
    database.setDatabaseName(storagePath);                     // 告诉 SQLite 驱动应打开哪个文件。
    if (!database.open())                                      // 尝试打开；文件不存在时 SQLite 会创建。
    {
        errorText = QStringLiteral("打开事件数据库失败：%1")   // 构造数据库打开失败消息。
                        .arg(database.lastError().text());      // 附加驱动返回的具体错误原因。
        qWarning() << errorText;                               // 输出错误，方便检查驱动或文件权限。
        return false;                                         // 数据库未打开，后续 SQL 不能执行。
    }

    QSqlQuery query(database); // 查询对象绑定到本 EventStore 的数据库连接。

    const QString createTableSql = QStringLiteral(             // 保存完整的建表 SQL 文本。
        "CREATE TABLE IF NOT EXISTS monitoring_event ("        // 表不存在时创建，重复初始化不会报错。
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "                // id 是自动递增的唯一主键。
        "device_id TEXT NOT NULL, "                            // 设备编号不能为空。
        "event_type TEXT NOT NULL, "                           // 事件类型不能为空。
        "severity INTEGER NOT NULL, "                          // 严重等级以 0/1/2 整数保存。
        "source TEXT NOT NULL, "                               // 事件来源模块不能为空。
        "message TEXT NOT NULL, "                              // 面向用户的事件描述不能为空。
        "recording_path TEXT, "                                // 录像路径允许为空，代表没有关联录像。
        "created_at TEXT NOT NULL, "                           // 时间以 ISO 8601 文本保存。
        "acknowledged INTEGER NOT NULL DEFAULT 0)");           // 布尔值用 0/1 保存，默认未确认。
    if (!query.exec(createTableSql))                           // 执行建表语句并检查结果。
    {
        errorText = QStringLiteral("创建事件数据表失败：%1")   // 构造建表失败消息。
                        .arg(query.lastError().text());         // 附加 SQLite 返回的具体错误。
        qWarning() << errorText;                               // 输出错误日志。
        return false;                                         // 没有数据表时事件存储不可用。
    }

    if (!query.exec(QStringLiteral(                            // 执行创建索引语句。
            "CREATE INDEX IF NOT EXISTS idx_monitoring_event_created_at " // 索引不存在时才创建。
            "ON monitoring_event(created_at DESC)")))          // 按时间倒序建立索引。
    {
        qWarning() << "创建事件时间索引失败："                  // 索引失败不影响基本读写，所以只警告。
                   << query.lastError().text();                 // 输出 SQLite 的索引创建错误。
    }

    errorText.clear();                                         // 初始化成功，清除以前保存的错误。
    qInfo() << "边缘监控事件数据库已就绪：" << storagePath;    // 记录实际数据库文件位置。
    return true;                                               // 通知调用者数据库已经可用。
}

/**
 * 写入一条 MonitoringEvent。
 *
 * @param event 待写入事件；不能为 nullptr。写入成功后 id 会被数据库主键回填。
 * @return SQL 执行成功返回 true，否则返回 false。
 */
bool EventStore::appendEvent(MonitoringEvent *event)
{
    if (!event || !database.isOpen())                          // 同时防止空指针和未初始化数据库。
    {
        errorText = QStringLiteral("事件数据库未打开");        // 保存可由 lastError() 取得的原因。
        return false;                                         // 不构造无效查询，也不解引用空指针。
    }

    QSqlQuery query(database);                                 // 在本对象的 SQLite 连接上创建查询。
    query.prepare(QStringLiteral(                              // 预编译带位置占位符的 INSERT 语句。
        "INSERT INTO monitoring_event ("                      // 指定要写入 monitoring_event 表。
        "device_id, event_type, severity, source, message, "   // 前五个占位符对应事件基本信息。
        "recording_path, created_at, acknowledged) "           // 后三个占位符对应录像、时间和确认状态。
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)"));                  // 八个问号按下面绑定顺序一一对应。
    query.addBindValue(event->deviceId);                       // 第1项：设备编号。
    query.addBindValue(event->eventType);                      // 第2项：事件类型。
    query.addBindValue(int(event->severity));                  // 第3项：强类型枚举显式转换为数据库整数。
    query.addBindValue(event->source);                         // 第4项：事件来源模块。
    query.addBindValue(event->message);                        // 第5项：事件描述。
    query.addBindValue(event->recordingPath);                  // 第6项：关联 H.264 录像路径，可为空。
    query.addBindValue(event->createdAt.toString(              // 第7项：把 QDateTime 转换为文本。
        Qt::ISODateWithMs));                                   // 使用带毫秒和时区信息的 ISO 8601 格式。
    query.addBindValue(event->acknowledged ? 1 : 0);           // 第8项：把 bool 明确转换为 SQLite 的 1/0。

    if (!query.exec())                                         // 执行已经准备并绑定参数的 INSERT。
    {
        errorText = QStringLiteral("写入监控事件失败：%1")     // 构造写入失败消息。
                        .arg(query.lastError().text());         // 附加约束、磁盘或 SQL 错误。
        qWarning() << errorText;                               // 输出终端诊断日志。
        return false;                                         // 告诉调用者事件没有成功持久化。
    }

    event->id = query.lastInsertId().toLongLong();             // 将 SQLite 新生成的自增 id 回填原对象。
    errorText.clear();                                         // 写入成功，清除旧错误。
    qInfo() << "监控事件已入库：ID=" << event->id             // 输出新记录主键。
            << "类型=" << event->eventType                    // 输出事件类型，便于定位。
            << "消息=" << event->message;                     // 输出事件内容，便于现场调试。
    return true;                                               // 通知调用方写入成功。
}

/**
 * 查询最近写入的事件。
 *
 * @param maximumCount 最多返回的记录条数。
 * @return 按 id 从大到小排列的事件数组。
 */
QVector<MonitoringEvent> EventStore::recentEvents(int maximumCount) const
{
    QVector<MonitoringEvent> events;                           // 创建空结果数组，失败时也可直接返回。
    if (!database.isOpen() || maximumCount <= 0)               // 数据库未开或条数无效时无需执行 SQL。
        return events;                                        // 返回空数组表示没有可用结果。

    QSqlQuery query(database);                                 // 在当前 SQLite 连接上创建只读查询。
    query.prepare(QStringLiteral(                              // 预编译带 LIMIT 参数的查询。
        "SELECT id, device_id, event_type, severity, source, message, " // 字段顺序决定下方 value() 索引。
        "recording_path, created_at, acknowledged "            // 继续列出录像、时间和确认状态。
        "FROM monitoring_event ORDER BY id DESC LIMIT ?"));   // 新记录在前，并限制最大数量。
    query.addBindValue(maximumCount);                          // 把调用者要求的最大条数绑定到 LIMIT。
    if (!query.exec())                                         // 执行 SELECT 并检查 SQLite 返回值。
    {
        errorText = QStringLiteral("读取监控事件失败：%1")     // 构造查询失败消息。
                        .arg(query.lastError().text());         // 附加数据库具体错误文本。
        qWarning() << errorText;                               // 输出错误供开发板调试。
        return events;                                        // 查询失败时返回仍为空的数组。
    }

    while (query.next())                                      // 将游标逐行移动到每一条查询结果。
    {
        MonitoringEvent event;                                // 为当前数据库行创建事件对象。
        event.id = query.value(0).toLongLong();                // 第0列：数据库自增主键。
        event.deviceId = query.value(1).toString();            // 第1列：设备编号。
        event.eventType = query.value(2).toString();           // 第2列：事件类型。
        event.severity = MonitoringEventSeverity(              // 第3列：把数据库整数恢复成强类型枚举。
            query.value(3).toInt());                           // QVariant 先转换成普通 int。
        event.source = query.value(4).toString();              // 第4列：事件来源。
        event.message = query.value(5).toString();             // 第5列：事件说明。
        event.recordingPath = query.value(6).toString();       // 第6列：关联录像路径。
        event.createdAt = QDateTime::fromString(               // 第7列：把文本恢复成 QDateTime。
            query.value(7).toString(),                         // 取得数据库中的 ISO 时间字符串。
            Qt::ISODateWithMs);                                // 按写入时相同的带毫秒格式解析。
        event.acknowledged = query.value(8).toBool();          // 第8列：把 0/1 恢复成 bool。
        events.append(event);                                  // 将完整事件追加到返回数组末尾。
    }
    return events;                                             // 返回按新到旧排列的全部查询结果。
}

/** 返回最近一次失败保存的错误文本，不会修改对象状态。 */
QString EventStore::lastError() const
{
    return errorText; // QString 使用隐式共享，按值返回的复制成本很低。
}

/** 返回 initialize() 计算得到的 SQLite 文件路径。 */
QString EventStore::databasePath() const
{
    return storagePath; // 初始化前该字符串为空，初始化后为 edge_monitor.db 完整路径。
}
