/*
 * ============================================================================
 * alarm_actuator.h — LED + 蜂鸣器告警执行器
 * ============================================================================
 *
 * 通过 sysfs 控制 i.MX6ULL 开发板上的板载 LED 和蜂鸣器。
 * 桌面环境通过 #if defined(__arm__) 条件编译跳过文件操作，打印模拟日志。
 *
 * 控制路径：
 *   LED:    /sys/devices/platform/leds/leds/sys-led/brightness  (1=亮, 0=灭)
 *   Buzzer: /sys/devices/platform/leds/leds/beep/brightness     (1=鸣, 0=停)
 *
 * 使用方式：
 *   - pulse(durationMs): LED 和蜂鸣器同时激活 durationMs 毫秒后自动关闭
 *   - setAlarmActive(bool): 直接控制告警状态（MQTT 远程命令使用）
 *
 * 初始化时会将 LED trigger 写入 "none"，接管 LED 的手动控制权。
 */
#ifndef ALARM_ACTUATOR_H
#define ALARM_ACTUATOR_H

#include <QObject>

class QTimer;

class AlarmActuator final : public QObject
{
    Q_OBJECT

public:
    explicit AlarmActuator(QObject *parent = nullptr);
    ~AlarmActuator() override;

public slots:
    void pulse(int durationMs = 2000); // 脉冲式告警（LED亮+蜂鸣响，定时关闭）
    void setAlarmActive(bool active);  // 设置告警状态（true=开, false=关）

signals:
    void stateChanged(bool active, const QString &message);

private:
    bool writeState(const QString &path, bool active,
                    const QString &deviceName); // 写 "1" 或 "0" 到 sysfs 节点

    QTimer *stopTimer; // 脉冲时长定时器（单次触发）
    bool alarmActive;  // 当前告警状态
};

#endif // ALARM_ACTUATOR_H