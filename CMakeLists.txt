cmake_minimum_required(VERSION 3.16)

# AutoLink — ESP Component Registry / ESP-IDF component definition.
#
# Layout:
#   src/     — implementation .cpp files and internal subdirs
#              (al/link, al/util, al/hal, al/web, al/pingpong).
#              Also holds the flat public shims AutoLink.h and
#              PingPong.h so the Arduino toolchain (which only
#              adds src/ to the include path) finds the public
#              API without modification.
#   include/ — canonical public headers. ESP-IDF resolves
#              #include "AutoLink.h" and #include "PingPong.h"
#              from here when this component is consumed.
#
# Both directories are added to the include path so internal
# relative includes ("al/link/Link.h", "al/util/Log.h", etc.)
# keep working.
#
# AutoLinkWeb.cpp is Arduino-only (WiFi.h, Preferences.h,
# Arduino.h). The host test suite covers AutoLinkWebCore.cpp.
# In an ESP-IDF build, the WiFi dashboard is unavailable; the
# core AutoLink + Link protocol still builds and runs.

# Core sources (compile under both Arduino-as-ESP-IDF-component
# and stock ESP-IDF).
set(AUTOLINK_CORE_SRCS
    "src/AutoLink.cpp"
    "src/al/link/Link.cpp"
    "src/al/link/LinkFrameRx.cpp"
    "src/al/link/LinkReorder.cpp"
    "src/al/link/arq/ArqCache.cpp"
    "src/al/link/arq/LinkArq.cpp"
    "src/al/link/sweep/LinkBaudSweep.cpp"
    "src/al/link/sweep/LinkSweep.cpp"
    "src/al/util/Log.cpp"
    "src/al/util/UtilCobs.cpp"
    "src/al/util/UtilCrc.cpp"
    "src/al/web/AutoLinkWebCore.cpp"
)

# Arduino-only sources. Compiled when this component is consumed
# from inside an Arduino-ESP32 project (Arduino-as-ESP-IDF-
# component), where the Arduino core is available.
if(DEFINED ARDUINO OR DEFINED ARDUINO_AS_ESP_IDF_COMPONENT OR DEFINED CONFIG_ARDUINO)
    list(APPEND AUTOLINK_CORE_SRCS "src/al/web/AutoLinkWeb.cpp")
endif()

idf_component_register(
    SRCS ${AUTOLINK_CORE_SRCS}
    INCLUDE_DIRS
        "src"
        "include"
    REQUIRES
        driver
        esp_timer
        esp_system
        freertos
)

if(DEFINED ARDUINO OR DEFINED ARDUINO_AS_ESP_IDF_COMPONENT OR DEFINED CONFIG_ARDUINO)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC ARDUINO=1)
endif()
