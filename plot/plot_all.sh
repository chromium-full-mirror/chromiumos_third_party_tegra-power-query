#!/bin/bash

#
# Example of how to draw a couple of windows displaying all of the captured
# data in real-time.
#
# This uses the "driveGnuPlotStreams.pl" available from
# http://users.softlab.ntua.gr/~ttsiod/gnuplotStreaming.html
#
# At least on my computer, this isn't particularly fast; limiting it to a
# subset of the data is much faster.
#


../tegra-power-query -s | ./driveGnuPlotStreams.pl \
    16 \
    2 \
    500 500 \
    0 0.3 \
    0 16 \
    500x500+0+30 \
    500x500+510+30 \
    "BAT Voltage" \
    "BAT Current" \
    "SYS_SM1 Voltage" \
    "SYS_SM1 Current" \
    "SYS_SM2 Voltage" \
    "SYS_SM2 Current" \
    "CHRG Voltage" \
    "CHRG Current" \
    "DDR2 Voltage" \
    "DDR2 Current" \
    "SYS_SM0 Voltage" \
    "SYS_SM0 Current" \
    "BL Voltage" \
    "BL Current" \
    "PNL Voltage" \
    "PNL Current" \
    1 0 1 0 1 0 1 0 1 0 1 0 1 0 1 0
