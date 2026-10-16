/*
 * event_store.h —— SQLite监控事件存储类的接口声明。
 *
 * EventStore负责创建事件数据库、写入MonitoringEvent，
 * 以及按时间顺序查询最近发生的监控事件。
 */
#ifndef EVENT_STORE_H // 防止该头文件在同一编译单元中被重复包含。
#define EVENT_STORE_H // 定义与上方判断配套的头文件保护宏。

#include "monitoring_event.h" // 引入MonitoringEvent事件结构和事件严重等级枚举。

#include <QSqlDatabase> // QSqlDatabase表示本类持有的SQLite数据库连接。
#include <QVector>      // QVector用于保存并返回多条MonitoringEvent记录。

/** 管理监控事件SQLite数据库的存储类；final表示该类不允许被继承。 */
class EventStore final // EventStore封装数据库连接、事件写入和查询操作。
{
public: // 对外提供对象生命周期、初始化、写入和查询接口。
    EventStore();  // 构造存储对象并生成当前实例专用的数据库连接名，但不立即打开数据库。
    ~EventStore(); // 关闭数据库，释放连接句柄并移除当前实例注册的Qt SQL连接。

    /**
     * 创建数据目录，打开SQLite文件，并创建事件表和时间索引。
     * @return 全部关键初始化操作成功时返回true，失败时返回false。
     */
    bool initialize(); // 初始化事件数据库；失败原因可通过lastError()取得。

    /**
     * 将一条监控事件写入数据库。
     * @param event 指向待写入事件的指针，不能为nullptr；成功后其id会被回填为数据库自增主键。
     * @return 写入成功返回true，参数无效、数据库未打开或SQL失败时返回false。
     */
    bool appendEvent(MonitoringEvent *event); // 持久化一条事件并回填event->id。

    /**
     * 查询最近写入的监控事件。
     * @param maximumCount 最多返回的事件数量；小于或等于0时返回空数组。
     * @return 按数据库id从大到小排列的MonitoringEvent数组，即最新事件位于最前面。
     */
    QVector<MonitoringEvent> recentEvents(int maximumCount) const; // const表示查询不会改变对象的逻辑状态，errorText除外。

    /** @return 最近一次数据库操作失败时保存的错误说明；没有错误时为空字符串。 */
    QString lastError() const; // 只读取errorText，不修改对象状态。

    /** @return initialize()确定的SQLite数据库文件完整路径；初始化前可能为空。 */
    QString databasePath() const; // 返回storagePath中保存的edge_monitor.db路径。

private: // 以下成员仅供EventStore内部管理数据库连接和状态。
    QString connectionName; // 当前对象专用的Qt SQL连接名，由this地址的十六进制文本生成，防止多个实例重名。
    QString storagePath; // SQLite数据库文件的完整路径，初始化后指向data/edge_monitor.db。
    mutable QString errorText; // 保存最近一次错误；mutable允许recentEvents()等const函数更新错误信息。
    QSqlDatabase database; // 当前EventStore实例持有的SQLite连接句柄，实际驱动类型为QSQLITE。
};

#endif // EVENT_STORE_H：结束头文件保护范围。
