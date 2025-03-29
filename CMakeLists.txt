cmake_minimum_required(VERSION 3.15)

project(idle_detect VERSION 0.3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -O2 -fPIE -fstack-protector-strong")

# Find Qt (no Qt modules needed currently)
# find_package(Qt6 COMPONENTS Core Filesystem REQUIRED)

# Source files
set(SOURCES
    "util.h"
    "tinyformat.h"
    "event_detect.h"
    "util.cpp"
    "event_detect.cpp"
)

# Add the current directory to the include search path
include_directories(".")

# Executable target
add_executable(event_detect ${SOURCES})

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBEVDEV REQUIRED libevdev)

if(LIBEVDEV_FOUND)
  include_directories(${LIBEVDEV_INCLUDE_DIRS})
  target_link_libraries(event_detect ${LIBEVDEV_LIBRARIES})
endif()

# Link Qt libraries
#target_link_libraries(idle_detect PRIVATE Qt6::Core Qt6::Filesystem)

# Installation paths (parameterized)
option(INSTALL_BIN_DIR "Installation directory for binaries" "/usr/local/bin")
option(INSTALL_CONFIG_DIR "Installation directory for configuration files" "/etc/idle_detect")
option(INSTALL_SYSTEM_SERVICE_DIR "Installation directory for the system level dc_event_detection service" "/etc/systemd/system")
option(INSTALL_USER_SERVICE_DIR "Installation directory for the user level dc_idle_detection service" "$ENV{HOME}/.config/systemd/user")

# Install the executable and scripts
install(TARGETS event_detect DESTINATION ${INSTALL_BIN_DIR})
install(FILES "idle_detect.sh" "idle_detect_resources.sh" DESTINATION ${INSTALL_BIN_DIR})

# Install configuration file
install(FILES "idle_detect.conf" DESTINATION ${INSTALL_CONFIG_DIR})

# Install system service
install(FILES "dc_event_detection.service" DESTINATION ${INSTALL_SYSTEM_SERVICE_DIR})

# Install user service
install(FILES "dc_idle_detection.service" DESTINATION ${INSTALL_USER_SERVICE_DIR})

# Uninstall target
configure_file(uninstall.cmake.in uninstall.cmake @ONLY)
install(SCRIPT uninstall.cmake)

# System service install target
add_custom_target(install_system_service
    COMMAND ${CMAKE_COMMAND} -E copy "dc_event_detection.service" "${INSTALL_SYSTEM_SERVICE_DIR}/dc_event_detection.service"
    COMMAND systemctl daemon-reload
    COMMAND systemctl enable dc_event_detection.service
    COMMENT "Installing and enabling system-level service"
)

# User-level service install target
add_custom_target(install_user_service
    COMMAND ${CMAKE_COMMAND} -E copy "dc_idle_detection.service" "${INSTALL_USER_SERVICE_DIR}/dc_idle_detection.service"
    COMMAND systemctl --user daemon-reload
    COMMAND systemctl --user enable dc_idle_detection.service
    COMMENT "Installing and enabling user-level service"
)
