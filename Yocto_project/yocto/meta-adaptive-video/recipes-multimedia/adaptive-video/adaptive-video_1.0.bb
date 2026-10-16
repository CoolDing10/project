SUMMARY = "Low-latency adaptive H.264 video sender for i.MX6ULL"
DESCRIPTION = "V4L2 capture, FFmpeg H.264 encoding, TCP feedback and PID/MPC control"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

# Development layout: this layer lives inside the source tree. For a release,
# replace externalsrc with an immutable git SRC_URI and SRCREV.
inherit cmake pkgconfig systemd useradd externalsrc
EXTERNALSRC = "${THISDIR}/../../../.."
EXTERNALSRC_BUILD = "${WORKDIR}/build"

DEPENDS = "ffmpeg"

SYSTEMD_SERVICE:${PN} = "adaptive-video.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

USERADD_PACKAGES = "${PN}"
GROUPADD_PARAM:${PN} = "--system adaptive-video"
USERADD_PARAM:${PN} = "--system --gid adaptive-video --home /nonexistent --no-create-home \
                       --shell /sbin/nologin --groups video adaptive-video"

EXTRA_OECMAKE = "-DBUILD_VIDEO_APPS=ON -DBUILD_SENDER=ON \
                 -DBUILD_RECEIVER=OFF -DBUILD_TOOLS=OFF -DBUILD_TESTING=OFF \
                 -DREQUIRE_VIDEO_APPS=ON"

do_install:append() {
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${S}/config/adaptive-video.service \
        ${D}${systemd_system_unitdir}/adaptive-video.service
    install -d ${D}${sysconfdir}/default
    install -m 0644 ${S}/config/adaptive-video.default \
        ${D}${sysconfdir}/default/adaptive-video
}

FILES:${PN} += "${systemd_system_unitdir}/adaptive-video.service \
                ${sysconfdir}/default/adaptive-video"
