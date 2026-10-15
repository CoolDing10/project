# ============================================================
# pc_receiver.pro — PC 端接收程序构建配置
# ============================================================
#
# 这是一个独立于主项目的 Qt 应用，运行在 Linux 桌面 x86_64 环境。
# 功能：RTP/UDP 接收 H.264 视频帧 + MQTT 远程管理设备。
#
# 依赖：
#   - Qt 5.12.9 (core gui widgets network)
#   - QtMqtt 5.12.9 (x86 预编译库)
#
# 构建：在 pc_receiver 目录下执行 qmake pc_receiver.pro && make
# ============================================================

QT += core gui widgets network

CONFIG += c++11

isEmpty(FFMPEG_ROOT) {
    CONFIG += link_pkgconfig
    PKGCONFIG += libavcodec libavutil libswscale
} else {
    INCLUDEPATH += $$FFMPEG_ROOT/include
    LIBS += -L$$FFMPEG_ROOT/lib -lavcodec -lavutil -lswscale
    QMAKE_RPATHDIR += $$FFMPEG_ROOT/lib
}

TEMPLATE = app
TARGET = imx6ull_pc_receiver

SOURCES += \
    archive_browser.cpp \
    h264_decoder.cpp \
    main.cpp \
    receiver_window.cpp \
    ../video/rtp_packet.cpp \
    ../video/rtp_reassembler.cpp

HEADERS += \
    archive_browser.h \
    h264_decoder.h \
    receiver_window.h \
    ../video/rtp_packet.h \
    ../video/rtp_reassembler.h

INCLUDEPATH += $$PWD/../video

# QtMqtt 5.12.9 x86 预编译库
MQTT_ROOT = $$PWD/../lib/x86_qtmqtt
INCLUDEPATH += $$MQTT_ROOT/src
DEPENDPATH += $$MQTT_ROOT/src
LIBS += $$MQTT_ROOT/lib/libQt5Mqtt.so.5.12.9
QMAKE_RPATHDIR += $$MQTT_ROOT/lib
