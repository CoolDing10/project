# i.MX6ULL has no H.264 VPU encoder. Enable the small, low-latency libx264
# software encoder used by adaptive_sender.
PACKAGECONFIG:append = " gpl x264"
