SUMMARY = "Minimal adaptive video image for i.MX6ULL"
LICENSE = "MIT"

inherit core-image features_check

REQUIRED_DISTRO_FEATURES = "systemd"

IMAGE_FEATURES = ""
EXTRA_IMAGE_FEATURES = ""
IMAGE_LINGUAS = ""
NO_RECOMMENDATIONS = "1"

IMAGE_INSTALL = "packagegroup-core-boot adaptive-video v4l-utils"
IMAGE_FSTYPES:append = " tar.gz"

# Fail the build if the uncompressed rootfs exceeds the project requirement.
python do_check_adaptive_rootfs_size() {
    import os
    import bb
    rootfs = d.getVar('IMAGE_ROOTFS')
    total = 0
    for directory, _, files in os.walk(rootfs):
        for name in files:
            path = os.path.join(directory, name)
            if not os.path.islink(path):
                total += os.lstat(path).st_size
    limit = 100 * 1024 * 1024
    bb.note('adaptive-video rootfs payload: %.2f MiB' % (total / 1024.0 / 1024.0))
    if total >= limit:
        bb.fatal('adaptive-video rootfs is %.2f MiB; required size is < 100 MiB' %
                 (total / 1024.0 / 1024.0))
}
addtask check_adaptive_rootfs_size after do_rootfs before do_image
