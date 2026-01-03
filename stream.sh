#!/bin/bash
#
# MJPEG Stream Transmitter for Lilka Desktop Monitor
# Uses GStreamer to capture screen and stream MJPEG over TCP
#
# Usage: ./stream.sh <ESP32_IP> [PORT] [FPS] [QUALITY]
#

set -e

# Default settings
IP="${1:-192.168.1.100}"
PORT="${2:-8090}"
FPS="${3:-15}"
QUALITY="${4:-50}"

# Display dimensions for Lilka v2
WIDTH=280
HEIGHT=240

if [ -z "$1" ]; then
    echo "=== MJPEG Stream Transmitter for Lilka ==="
    echo ""
    echo "Usage: $0 <ESP32_IP> [PORT] [FPS] [QUALITY]"
    echo ""
    echo "Arguments:"
    echo "  ESP32_IP   - IP address of the Lilka device (required)"
    echo "  PORT       - TCP port (default: 8090)"
    echo "  FPS        - Frames per second (default: 15)"
    echo "  QUALITY    - JPEG quality 1-100 (default: 50)"
    echo ""
    echo "Examples:"
    echo "  $0 192.168.1.100"
    echo "  $0 192.168.1.100 8090 20 60"
    echo ""
    echo "GStreamer plugins required:"
    echo "  Linux:  gstreamer1.0-plugins-good (ximagesrc)"
    echo "  macOS:  gstreamer1.0-plugins-bad (avfvideosrc)"
    exit 1
fi

echo "=== MJPEG Stream Transmitter ==="
echo "Target: $IP:$PORT"
echo "Resolution: ${WIDTH}x${HEIGHT}"
echo "FPS: $FPS, Quality: $QUALITY"
echo ""

# Detect platform and set appropriate screen capture element
if [[ "$OSTYPE" == "linux-gnu"* ]]; then
    # Linux - try different capture methods
    if gst-inspect-1.0 ximagesrc &>/dev/null; then
        CAPTURE="ximagesrc use-damage=false show-pointer=true ! videoconvert"
        echo "Using X11 capture (ximagesrc)"
    elif gst-inspect-1.0 pipewiresrc &>/dev/null; then
        CAPTURE="pipewiresrc ! videoconvert"
        echo "Using PipeWire capture"
    else
        echo "ERROR: No suitable screen capture plugin found"
        echo "Install: sudo apt install gstreamer1.0-plugins-good"
        exit 1
    fi
elif [[ "$OSTYPE" == "darwin"* ]]; then
    # macOS - use avfvideosrc
    if gst-inspect-1.0 avfvideosrc &>/dev/null; then
        CAPTURE="avfvideosrc capture-screen=true ! videoconvert"
        echo "Using AVFoundation capture (macOS)"
    else
        echo "ERROR: avfvideosrc not found"
        echo "Install: brew install gstreamer gst-plugins-bad"
        exit 1
    fi
else
    echo "ERROR: Unsupported platform: $OSTYPE"
    exit 1
fi

echo "Starting stream... (Ctrl+C to stop)"
echo ""

# GStreamer pipeline:
# 1. Capture screen
# 2. Add queue for buffering
# 3. Scale to 280x240 with Lanczos filter for quality
# 4. Convert to I420 (required for jpegenc baseline)
# 5. Limit framerate
# 6. Encode as baseline JPEG (not progressive, compatible with TJpgDec)
# 7. Add queue before network sink
# 8. Send over TCP

exec gst-launch-1.0 -e \
    $CAPTURE \
    ! queue max-size-buffers=2 leaky=downstream \
    ! videoscale method=lanczos \
    ! "video/x-raw,width=$WIDTH,height=$HEIGHT" \
    ! videorate \
    ! "video/x-raw,framerate=$FPS/1" \
    ! videoconvert \
    ! "video/x-raw,format=I420" \
    ! jpegenc quality=$QUALITY idct-method=ifast \
    ! queue max-size-buffers=2 leaky=downstream \
    ! tcpclientsink host=$IP port=$PORT
