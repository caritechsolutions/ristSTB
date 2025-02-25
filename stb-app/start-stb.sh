#!/bin/bash

# Kill any existing X instances
killall Xorg
rm -rf /tmp/.X11-unix
rm -f /tmp/.X0-lock

sleep 2

# Set up environment
export DISPLAY=:0.0
export QT_QPA_PLATFORM=xcb

# Set audio defaults for HDMI
echo 'defaults.pcm.card 1
defaults.pcm.device 0
defaults.ctl.card 1' > /etc/asound.conf

# Start X server in background
X -ac :0 &

# Wait for X to start
sleep 2

# Set access permissions
xhost +

# Start the Qt application
./STB