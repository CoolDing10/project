/*
 * ============================================================================
 * alarm_actuator.cpp — LED + 蜂鸣器告警执行器实现
 * ============================================================================
 *
 * 通过 QFile 写入 sysfs brightness 文件来控制 LED 和蜂鸣器。
 * 仅 ARM 平台真正执行 I/O；桌面环境打印模拟日志。
 *
 * LED 控制细节：
 *   写入 "1" → 点亮 /sys/devices/.../sys-led/brightness
 *   写入 "0" → 熄灭
 *   初始化时将 trigger 写为 "none"，接管手动控制权（否则内核可能自动控制 LED）
 *
 * 蜂鸣器控制细节：
 *   写入 "1" → 鸣响 /sys/devices/.../beep/brightness
 *   写入 "0" → 停止
 */
#include "alarm_actuator.h"

#include <QDebug>
#include <QFile>
#include <QTimer>

namespace
{
    const QString LedBrightnessPath = QStringLiteral(
        "/sys/devices/platform/leds/leds/sys-led/brightness");
    const QString LedTriggerPath = QStringLiteral(
        "/sys/class/leds/sys-led/trigger");
    const QString BuzzerBrightnessPath = QStringLiteral(
        "/sys/devices/platform/leds/leds/beep/brightness");
}

AlarmActuator::AlarmActuator(QObject *parent)
    : QObject(parent),
      stopTimer(new QTimer(this)),
      alarmActive(false)
{
    stopTimer->setSingleShot(true);
    connect(stopTimer, &QTimer::timeout,
            this, [this]()
            { setAlarmActive(false); });

#if defined(__arm__)
    // 初始化时将 LED trigger 设为 "none"，避免内核自动控制 LED（心跳等）
    QFile triggerFile(LedTriggerPath);
    if (triggerFile.open(QIODevice::WriteOnly))
    {
        triggerFile.write("none");
        triggerFile.close();
    }
    else
    {
        qWarning() << "禁用 LED 自动触发器失败："
                   << triggerFile.errorString();
    }
#endif
}

AlarmActuator::~AlarmActuator()
{
    // 析构时确保 LED 和蜂鸣器处于关闭状态
    if (alarmActive)
        setAlarmActive(false);
}

void AlarmActuator::pulse(int durationMs)
{
    // 脉冲告警：立即开启，durationMs 毫秒后自动关闭
    setAlarmActive(true);
    stopTimer->start(qMax(500, durationMs));
}

void AlarmActuator::setAlarmActive(bool active)
{
    if (!active)
        stopTimer->stop();

    // 同步控制 LED 和蜂鸣器
    const bool ledUpdated = writeState(LedBrightnessPath, active,
                                       QStringLiteral("LED"));
    const bool buzzerUpdated = writeState(BuzzerBrightnessPath, active,
                                          QStringLiteral("蜂鸣器"));
    alarmActive = active;

    const QString message = QStringLiteral("LED/%1%2")
                                .arg(QStringLiteral("蜂鸣器"),
                                     active ? QStringLiteral("告警已开启")
                                            : QStringLiteral("告警已关闭"));
    if (ledUpdated || buzzerUpdated)
        qInfo() << message;
    emit stateChanged(active, message);
}

bool AlarmActuator::writeState(const QString &path, bool active,
                               const QString &deviceName)
{
#if !defined(__arm__)
    // 桌面环境：打印模拟日志，不执行实际文件操作
    qInfo() << "[桌面模拟]" << deviceName
            << (active ? "开启" : "关闭");
    Q_UNUSED(path)
    return true;
#else
    // ARM 平台：写 "1" 或 "0" 到 sysfs brightness 节点
    QFile deviceFile(path);
    if (!deviceFile.exists())
    {
        qWarning() << deviceName << "控制节点不存在：" << path;
        return false;
    }
    if (!deviceFile.open(QIODevice::WriteOnly))
    {
        qWarning() << "打开" << deviceName << "控制节点失败："
                   << deviceFile.errorString();
        return false;
    }

    const qint64 written = deviceFile.write(active ? "1" : "0");
    deviceFile.close();
    if (written != 1)
    {
        qWarning() << "写入" << deviceName << "状态失败";
        return false;
    }
    return true;
#endif
}