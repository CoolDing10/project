/*
 * ============================================================================
 * ap3216c.cpp — AP3216C 传感器 sysfs 读取实现
 * ============================================================================
 *
 * 通过 POSIX 文件 I/O 读取 Linux 驱动导出的 sysfs 属性文件。
 * 每 500ms 轮询一次（QTimer），读取三个传感器值。
 *
 * 桌面环境兼容：sysfs 节点不存在时返回 "N/A"，通过 errno 记录。
 */
#include "ap3216c.h" // 引入 Ap3216cSensor 类声明。

#include <cerrno>  // 提供 errno，保存最近一次 POSIX 系统调用错误码。
#include <cstring> // 提供 std::strerror()，把 errno 转换为可读文本。
#include <fcntl.h> // 提供 open() 和 O_RDONLY 打开标志。
#include <QDebug>  // 提供 qWarning() 错误日志。
#include <QTimer>  // 实现每 500ms 一次的传感器轮询。
#include <unistd.h> // 提供 read() 和 close() POSIX 文件操作。

namespace // 匿名命名空间中的路径和常量只在本实现文件可见。
{
    const char AmbientLightPath[] = "/sys/class/misc/ap3216c/als"; // 环境光强度
    const char ProximityPath[] = "/sys/class/misc/ap3216c/ps";     // 接近距离
    const char InfraredPath[] = "/sys/class/misc/ap3216c/ir";      // 红外强度
    const int PollIntervalMs = 500;                                // 轮询周期，单位毫秒。
} // 结束文件内部匿名命名空间。

Ap3216cSensor::Ap3216cSensor(QObject *parent) // parent 是 Qt 父对象，用于自动管理本传感器对象。
    : QObject(parent),                       // 初始化 QObject 基类并加入父对象树。
      pollingTimer(new QTimer(this))          // 创建子定时器，其生命周期跟随当前对象。
{
    pollingTimer->setInterval(PollIntervalMs);                    // 设置每 500ms 触发一次 timeout。
    connect(pollingTimer, &QTimer::timeout,                       // 定时器到期信号作为采样触发源。
            this, &Ap3216cSensor::pollSensorData);                // 在当前对象所在线程执行一次完整采样。
}

/*
 * 使用 POSIX open/read/close 从 sysfs 文件读取最多 31 字节文本。
 * sysfs 文件通常只包含一个数字+换行符，32 字节缓冲区足够。
 * 桌面环境下设备节点不存在会返回 "N/A"。
 */
QString Ap3216cSensor::readTextValue(const char *devicePath) // devicePath 是要读取的 sysfs 节点 C 字符串路径。
{
    const int descriptor = open(devicePath, O_RDONLY); // descriptor 保存只读打开后的文件描述符，失败为 -1。
    if (descriptor < 0)                                // 文件不存在或权限不足时不能继续读取。
    {
        qWarning() << "Unable to open" << devicePath << ':' << std::strerror(errno); // 输出路径和系统错误原因。
        return QStringLiteral("N/A");                                            // 用明确的不可用文本代替无效数值。
    }

    char buffer[32] = {}; // 清零字符数组，最多容纳 31 字节数据和结尾的 '\0'。
    const ssize_t bytesRead = read(descriptor, buffer, sizeof(buffer) - 1); // bytesRead 是实际读取字节数，失败为 -1。
    const int savedErrno = errno; // 在 close() 可能改变 errno 之前保存 read() 对应错误码。
    close(descriptor);            // 本次读取结束后立即释放文件描述符。

    if (bytesRead < 0) // 负返回值表示 read() 失败。
    {
        qWarning() << "Unable to read" << devicePath << ':'             // 输出读取失败的节点路径，
                   << std::strerror(savedErrno);                         // 以及读取时保存的系统错误原因。
        return QStringLiteral("N/A");                                  // 返回不可用占位文本。
    }

    return QString::fromLocal8Bit(buffer, int(bytesRead)).trimmed(); // 按实际长度转为 QString，并移除 sysfs 末尾换行。
}

/*
 * 执行一次完整的传感器轮询——依次读取三个 sysfs 节点。
 * 读取完成后发射 sensorDataChanged() 信号，
 * 由 CameraStreamingPage 响应，更新 UI 并做阈值判断。
 */
void Ap3216cSensor::pollSensorData() // 执行一次三通道采样并通知所有观察者。
{
    ambientLightValue = readTextValue(AmbientLightPath); // 更新环境光缓存。
    proximityValue = readTextValue(ProximityPath);       // 更新接近距离缓存。
    infraredValue = readTextValue(InfraredPath);         // 更新红外强度缓存。
    emit sensorDataChanged();                            // 三项数据一致更新后再通知 UI 读取。
}

QString Ap3216cSensor::ambientLight() const // const getter 不修改对象，返回最近环境光缓存。
{
    return ambientLightValue; // QString 使用隐式共享，返回值通常无需立即复制字符数据。
}

QString Ap3216cSensor::proximity() const // 返回最近一次接近传感器读数。
{
    return proximityValue; // 将缓存值按值返回给调用者。
}

QString Ap3216cSensor::infrared() const // 返回最近一次红外传感器读数。
{
    return infraredValue; // 将缓存值按值返回给调用者。
}

void Ap3216cSensor::setPollingEnabled(bool enabled) // enabled 决定启动还是停止周期采样。
{
    if (enabled) // 开启时既要立即得到数据，也要安排后续周期采样。
    {
        pollSensorData();      // 启动时立即读取一次
        pollingTimer->start(); // 然后每 500ms 自动轮询
    }
    else // 关闭轮询分支。
        pollingTimer->stop(); // 停止后不再产生 timeout，但保留最后一次缓存值。
}
