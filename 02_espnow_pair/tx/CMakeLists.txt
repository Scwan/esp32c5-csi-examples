cmake_minimum_required(VERSION 3.16)

# Deliberately no EXTRA_COMPONENT_DIRS, unlike every other example here.
#
# This project uses nothing from components/ - it only transmits, and never
# looks at CSI. Pointing at that directory anyway would make ESP-IDF discover
# and BUILD csi_c5, because component discovery is by directory and has nothing
# to do with what main/ lists in REQUIRES. csi_c5.h then correctly refuses to
# compile without CONFIG_ESP_WIFI_CSI_ENABLED, which this board has no reason
# to turn on and pay RAM for.

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(csi_espnow_tx)
