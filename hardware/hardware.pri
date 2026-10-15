HEADERS += \
    $$PWD/ap3216c.h \
    $$PWD/icm20608.h \
    $$PWD/capture_thread.h \
    $$PWD/video_pipeline_config.h \
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
