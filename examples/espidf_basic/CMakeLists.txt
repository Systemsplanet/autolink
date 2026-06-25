cmake_minimum_required(VERSION 3.16)

# examples/espidf_basic — minimal stock-ESP-IDF example that pairs with
# the AutoLink component.
#
# Two build modes:
#
#   (a) Standalone stock-ESP-IDF (default below):
#       idf.py set-target esp32
#       idf.py build
#       Builds only main.c (a raw-UART loopback diagnostic in the same
#       spirit as examples/arduino_basic/arduino_basic.ino). The
#       AutoLink component is NOT consumed in this mode — the C++
#       facade depends on the Arduino-ESP32 core and cannot be built
#       with a pure stock-ESP-IDF project.
#
#   (b) Arduino-as-ESP-IDF-component (advanced):
#       Use the Arduino-ESP32 component as a wrapper, then this
#       example can include the AutoLink C++ facade. See the upstream
#       Arduino-ESP32 docs for that workflow.
#
# Wiring (crossover, same as the Arduino examples):
#   TX (GPIO17)  ──►  RX (GPIO16)  on the peer
#   RX (GPIO16)  ◄──  TX (GPIO17)  on the peer
#   shared GND

project(autolink_espidf_basic)

idf_component_register(
    SRCS "main/main.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer freertos
)
