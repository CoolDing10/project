HEADERS += \
    $$PWD/ap3216c.h \
    $$PWD/icm20608.h \
    $$PWD/capture_thread.h \
    $$PWD/tcp_frame_sender.h \
    $$PWD/../video/rtp_packet.h \
    $$PWD/../video/rtp_reassembler.h



SOURCES += \
    $$PWD/ap3216c.cpp \
    $$PWD/icm20608.cpp \
    $$PWD/capture_thread.cpp \
    $$PWD/tcp_frame_sender.cpp \
    $$PWD/../video/rtp_packet.cpp \
    $$PWD/../video/rtp_reassembler.cpp

INCLUDEPATH += $$PWD/../video


DISTFILES +=

# 学习版关闭 LED/蜂鸣器告警执行器，原构建条目保留在文件末尾。
# 恢复时把下面两项分别移回 HEADERS 和 SOURCES 列表：
# $$PWD/alarm_actuator.h
# $$PWD/alarm_actuator.cpp
