cmake_minimum_required(VERSION 3.15)

project(idle_detect VERSION 0.3 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -O2 -fPIE -fstack-protector-strong")

# Source files
set(SOURCES_EVENT_DETECT
  "util.h"
  "tinyformat.h"
  "event_detect.h"
  "util.cpp"
  "event_detect.cpp"
)

# Source files for idle_detect
# Note: The generated Wayland source file will be added later via target_sources()
set(SOURCES_IDLE_DETECT
  "util.h"
  "tinyformat.h"
  "idle_detect.h"
  "util.cpp"
  "idle_detect.cpp"
)

include_directories(".")

# --- Find Packages ---
find_package(PkgConfig REQUIRED)

# Dependencies for event_detect
pkg_check_modules(LIBEVDEV REQUIRED libevdev)

# Dependencies for idle_detect
find_package(X11 REQUIRED)
pkg_check_modules(XSS REQUIRED xscrnsaver)
pkg_check_modules(DBUS REQUIRED dbus-1)
pkg_check_modules(GLIB REQUIRED glib-2.0 gobject-2.0 gio-2.0)

# == Wayland Workaround using pkg-config ==
# Use pkg-config to find wayland-client instead of find_package(Wayland)
pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)

# Find wayland-protocols (assuming this part worked or has its fallback)
# Appears broken on openSUSE
# find_package(WaylandProtocols 1.17 REQUIRED)

# workaround
# Use pkg-config directly to find the data directory for wayland-protocols
find_package(PkgConfig REQUIRED) # Ensure pkg-config is found first
execute_process(
    COMMAND ${PKG_CONFIG_EXECUTABLE} --variable=pkgdatadir wayland-protocols
    OUTPUT_VARIABLE WaylandProtocols_DATADIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _pkg_config_result
    ERROR_QUIET # Don't print errors if pkg-config fails
)

if(NOT _pkg_config_result EQUAL 0 OR NOT WaylandProtocols_DATADIR)
    message(FATAL_ERROR "Failed to get pkgdatadir for wayland-protocols using pkg-config. Is wayland-protocols-devel installed and configured correctly?")
else()
    message(STATUS "Found wayland-protocols data dir via pkg-config: ${WaylandProtocols_DATADIR}")
endif()

# Find the wayland-scanner executable (assuming it's in the PATH)
# If this fails later during build, you might need find_program() or specify full path
find_program(WAYLAND_SCANNER_EXECUTABLE wayland-scanner REQUIRED)
# =========================================

# --- Wayland Protocol Code Generation ---

# Get the directory where wayland-protocols installs XML files
if(NOT WaylandProtocols_DATADIR)
    message(WARNING "WaylandProtocols_DATADIR not set by find_package. Attempting fallback via pkg-config.")
    pkg_check_modules(WAYLAND_PROTOCOLS_PC REQUIRED wayland-protocols)
    set(WaylandProtocols_DATADIR ${WAYLAND_PROTOCOLS_PC_pkgdatadir})
endif()
set(IDLE_NOTIFY_XML "${WaylandProtocols_DATADIR}/staging/ext-idle-notify/ext-idle-notify-v1.xml")

if(NOT EXISTS ${IDLE_NOTIFY_XML})
    message(FATAL_ERROR "Wayland protocol XML file not found: ${IDLE_NOTIFY_XML}. "
                        "Ensure 'wayland-protocols-devel' package is installed and path is correct.")
endif()

set(IDLE_NOTIFY_CLIENT_HEADER "${CMAKE_CURRENT_BINARY_DIR}/ext-idle-notify-v1-protocol.h")
set(IDLE_NOTIFY_CLIENT_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/ext-idle-notify-v1-client-protocol.c")

# Use add_custom_command to generate header
add_custom_command(
    OUTPUT ${IDLE_NOTIFY_CLIENT_HEADER}
    # Use variable found by find_program()
    COMMAND ${WAYLAND_SCANNER_EXECUTABLE} client-header ${IDLE_NOTIFY_XML} ${IDLE_NOTIFY_CLIENT_HEADER}
    DEPENDS ${IDLE_NOTIFY_XML} ${WAYLAND_SCANNER_EXECUTABLE}
    COMMENT "Generating Wayland client header ${IDLE_NOTIFY_CLIENT_HEADER}"
)

# Use add_custom_command to generate source
add_custom_command(
    OUTPUT ${IDLE_NOTIFY_CLIENT_SOURCE}
    COMMAND ${WAYLAND_SCANNER_EXECUTABLE} private-code ${IDLE_NOTIFY_XML} ${IDLE_NOTIFY_CLIENT_SOURCE}
    DEPENDS ${IDLE_NOTIFY_XML} ${WAYLAND_SCANNER_EXECUTABLE}
    COMMENT "Generating Wayland client source ${IDLE_NOTIFY_CLIENT_SOURCE}"
)

# Custom target for generation (unchanged)
add_custom_target(WaylandIdleNotifyProtocolGenerator DEPENDS
    ${IDLE_NOTIFY_CLIENT_HEADER}
    ${IDLE_NOTIFY_CLIENT_SOURCE}
)

# --- Executable Targets ---
add_executable(event_detect ${SOURCES_EVENT_DETECT})
add_executable(idle_detect ${SOURCES_IDLE_DETECT})

# --- Target Properties: event_detect ---
# (Unchanged)
if(LIBEVDEV_FOUND)
    target_include_directories(event_detect PRIVATE ${LIBEVDEV_INCLUDE_DIRS})
    target_link_libraries(event_detect PRIVATE ${LIBEVDEV_LIBRARIES})
endif()

# --- Target Properties: idle_detect ---

# Add dependency (unchanged)
add_dependencies(idle_detect WaylandIdleNotifyProtocolGenerator)

# Add generated source (unchanged)
target_sources(idle_detect PRIVATE ${IDLE_NOTIFY_CLIENT_SOURCE})

# Add include directories (use pkg-config variable for Wayland)
target_include_directories(idle_detect PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${X11_INCLUDE_DIR}
    ${XSS_INCLUDE_DIRS}
    ${DBUS_INCLUDE_DIRS}
    ${GLIB_INCLUDE_DIRS}
    ${WAYLAND_CLIENT_INCLUDE_DIRS} # <-- Use variable from pkg_check_modules
)

# Link libraries (use pkg-config variable for Wayland)
target_link_libraries(idle_detect PRIVATE
    ${X11_LIBRARIES}
    ${XSS_LIBRARIES}
    ${DBUS_LIBRARIES}
    ${GLIB_LIBRARIES}
    ${WAYLAND_CLIENT_LIBRARIES} # <-- Use variable from pkg_check_modules
)

# Installation paths (parameterized)
set(INSTALL_BIN_DIR "/usr/local/bin" CACHE PATH "Installation directory for binaries")
set(INSTALL_CONFIG_DIR "/etc" CACHE PATH "Installation directory for configuration files")
set(INSTALL_SYSTEM_SERVICE_DIR "/etc/systemd/system" CACHE PATH "Installation directory for the system level dc_event_detection service")
set(INSTALL_USER_CONFIG_DIR "$ENV{HOME}/.config" CACHE PATH "Installation directory for the user level idle_detect.conf")
set(INSTALL_USER_SERVICE_DIR "$ENV{HOME}/.config/systemd/user" CACHE PATH "Installation directory for the user level dc_idle_detection.service")

# Install the executable and scripts
install(TARGETS event_detect idle_detect DESTINATION ${INSTALL_BIN_DIR})
install(FILES "dc_pause" "dc_unpause"
        PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ GROUP_EXECUTE GROUP_READ WORLD_EXECUTE WORLD_READ
        DESTINATION ${INSTALL_BIN_DIR} )

# Install configuration file (system-wide)
install(FILES "event_detect.conf" DESTINATION ${INSTALL_CONFIG_DIR})

# Install system service (root ownership)
install(FILES "dc_event_detection.service" DESTINATION ${INSTALL_SYSTEM_SERVICE_DIR})

# Uninstall target
configure_file(uninstall.cmake.in uninstall.cmake @ONLY)

include(${CMAKE_CURRENT_BINARY_DIR}/uninstall.cmake) # Add this line

# Install executables only
add_custom_target(install_executables_only
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_BINARY_DIR}/event_detect" "${INSTALL_BIN_DIR}/event_detect"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_BINARY_DIR}/idle_detect" "${INSTALL_BIN_DIR}/idle_detect"

    COMMENT "installing executables only"
)

# Install dc control scripts only
add_custom_target(dc_control_scripts_only
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/dc_pause" "${INSTALL_BIN_DIR}/dc_pause"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/dc_unpause" "${INSTALL_BIN_DIR}/dc_unpause"

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
