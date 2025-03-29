cmake_minimum_required(VERSION 3.15)

project(idle_detect VERSION 0.3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -O2 -fPIE -fstack-protector-strong")

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

# Installation paths (parameterized)
set(INSTALL_BIN_DIR "/usr/local/bin" CACHE PATH "Installation directory for binaries")
set(INSTALL_CONFIG_DIR "/etc" CACHE PATH "Installation directory for configuration files")
set(INSTALL_SYSTEM_SERVICE_DIR "/etc/systemd/system" CACHE PATH "Installation directory for the system level dc_event_detection service")
set(INSTALL_USER_CONFIG_DIR "$ENV{HOME}/.config" CACHE PATH "Installation directory for the user level idle_detect.conf")
set(INSTALL_USER_SERVICE_DIR "$ENV{HOME}/.config/systemd/user" CACHE PATH "Installation directory for the user level dc_idle_detection.service")

# Install the executable and scripts
install(TARGETS event_detect DESTINATION ${INSTALL_BIN_DIR})
install(FILES "idle_detect.sh" "idle_detect_resources.sh" DESTINATION ${INSTALL_BIN_DIR})

# Install configuration file (system-wide)
install(FILES "event_detect.conf" DESTINATION ${INSTALL_CONFIG_DIR})

# Install system service (root ownership)
install(FILES "dc_event_detection.service" DESTINATION ${INSTALL_SYSTEM_SERVICE_DIR})

# Uninstall target
configure_file(uninstall.cmake.in uninstall.cmake @ONLY)
#install(SCRIPT uninstall.cmake) # Remove this

include(${CMAKE_CURRENT_BINARY_DIR}/uninstall.cmake) # Add this line

# Install executables only
add_custom_target(install_executables_only
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_BINARY_DIR}/event_detect" "${INSTALL_BIN_DIR}/event_detect"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/idle_detect.sh" "${INSTALL_BIN_DIR}/idle_detect.sh"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/idle_detect_resources.sh" "${INSTALL_BIN_DIR}/idle_detect_resources.sh"
    COMMENT "installing executables only"
)

# System service install target (root)
add_custom_target(install_system_service
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/dc_event_detection.service" "${INSTALL_SYSTEM_SERVICE_DIR}/dc_event_detection.service"
    COMMENT "Installing system-level service (root)"
)

# User-level service and config install (user)
add_custom_target(install_user_service
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/dc_idle_detection.service" "$ENV{HOME}/.config/systemd/user/dc_idle_detection.service"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/idle_detect.conf" "$ENV{HOME}/.config/idle_detect.conf"
    COMMENT "Installing user-level service (user)"
)
