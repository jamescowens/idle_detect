cmake_minimum_required(VERSION 3.15)

# Read the content of the updated release.h
file(READ release.h RELEASE_H_CONTENT)

# Extract version components using regex on the #define lines
string(REGEX MATCH "#define[ \t]+IDLE_DETECT_VERSION_MAJOR[ \t]+([0-9]+)"
             _dummy_match "${RELEASE_H_CONTENT}")
if(NOT CMAKE_MATCH_COUNT EQUAL 1)
    message(FATAL_ERROR "Could not extract IDLE_DETECT_VERSION_MAJOR from release.h")
endif()
set(PROJECT_VERSION_MAJOR "${CMAKE_MATCH_1}")

string(REGEX MATCH "#define[ \t]+IDLE_DETECT_VERSION_MINOR[ \t]+([0-9]+)"
             _dummy_match "${RELEASE_H_CONTENT}")
if(NOT CMAKE_MATCH_COUNT EQUAL 1)
    message(FATAL_ERROR "Could not extract IDLE_DETECT_VERSION_MINOR from release.h")
endif()
set(PROJECT_VERSION_MINOR "${CMAKE_MATCH_1}")

string(REGEX MATCH "#define[ \t]+IDLE_DETECT_VERSION_PATCH[ \t]+([0-9]+)"
             _dummy_match "${RELEASE_H_CONTENT}")
if(NOT CMAKE_MATCH_COUNT EQUAL 1)
    message(FATAL_ERROR "Could not extract IDLE_DETECT_VERSION_PATCH from release.h")
endif()
set(PROJECT_VERSION_PATCH "${CMAKE_MATCH_1}")

set(PROJECT_VERSION_TWEAK "0") # Default if not found
string(REGEX MATCH "#define[ \t]+IDLE_DETECT_VERSION_TWEAK[ \t]+([0-9]+)"
             _dummy_match "${RELEASE_H_CONTENT}")
if(CMAKE_MATCH_COUNT EQUAL 1)
    set(PROJECT_VERSION_TWEAK "${CMAKE_MATCH_1}")
endif()

# Construct the full version string for the project command
set(PROJECT_VERSION_FROM_HEADER "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}.${PROJECT_VERSION_TWEAK}")
message(STATUS "Found project version in release.h: ${PROJECT_VERSION_FROM_HEADER}")

# Use the extracted version in the project() command
project(idle_detect VERSION ${PROJECT_VERSION_FROM_HEADER} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Recommended: Turn off compiler extensions for stricter standard compliance check
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -fPIE -fstack-protector-strong")

# --- Check for required C++17 features (<filesystem>, <variant>, <atomic>) ---
include(CheckCXXSourceCompiles) # Include the necessary module
message(STATUS "Checking for C++17 <filesystem>, <variant>, <atomic> support...")

# Check if a small code snippet using these features compiles
check_cxx_source_compiles(
"
#include <filesystem>
#include <variant>
#include <atomic>
#include <string>
namespace fs = std::filesystem;
int main() {
    fs::path p = fs::current_path();
    std::variant<int, std::string> v = \"hello\";
    std::atomic<bool> b = true;
    (void)p; (void)v; (void)b;
    return 0;
}
"
CXX17_FEATURES_COMPILES # Variable to store TRUE or FALSE result
)

# Check the result and fail configuration if necessary
if(NOT CXX17_FEATURES_COMPILES)
    # Try to get compiler info if project() didn't already provide it
    if(NOT CMAKE_CXX_COMPILER_ID)
         # This part might not be strictly necessary as project() usually sets these
         execute_process(COMMAND ${CMAKE_CXX_COMPILER} --version OUTPUT_VARIABLE CXX_VERSION_OUTPUT ERROR_QUIET)
         string(REGEX MATCH "([0-9]+\\.[0-9]+(\\.[0-9]+)?)" CMAKE_CXX_COMPILER_VERSION "${CXX_VERSION_OUTPUT}")
         # Add logic to determine ID if needed
    endif()

    message(FATAL_ERROR "Compiler ${CMAKE_CXX_COMPILER} (${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}) "
                        "does not provide required C++17 features (like <filesystem>, <variant>, <atomic> support). "
                        "Please use a C++17 compliant compiler (e.g., GCC 9+, Clang 7+) "
                        "or specify one using -DCMAKE_CXX_COMPILER=/path/to/g++")
else()
     message(STATUS "Checking for C++17 <filesystem>, <variant>, <atomic> support... - Success")
endif()

include(GNUInstallDirs)

# Source files
set(SOURCES_EVENT_DETECT
    "release.h"
    "util.h"
    "tinyformat.h"
    "event_detect.h"
    "util.cpp"
    "event_detect.cpp"
)

# Source files for idle_detect
set(SOURCES_IDLE_DETECT
    "release.h"
    "util.h"
    "tinyformat.h"
    "idle_detect.h"
    "util.cpp"
    "idle_detect.cpp"
    # No generated Wayland sources needed anymore
)

# Source file for read_shmem_timestamp
set(SOURCES_READ_SHMEM_TIMESTAMPS
    "util.cpp"
    "read_shmem_timestamps.cpp"
)

include_directories(".")

# --- Find Packages ---
find_package(PkgConfig REQUIRED)

# Dependencies for event_detect
pkg_check_modules(LIBEVDEV REQUIRED libevdev)

# Dependencies for idle_detect
find_package(X11 REQUIRED)
pkg_check_modules(XSS REQUIRED xscrnsaver) # Find XScreenSaver lib via pkg-config
pkg_check_modules(DBUS REQUIRED dbus-1)
# Find GLib, GObject, GIO together (needed for D-Bus calls)
pkg_check_modules(GLIB REQUIRED glib-2.0 gobject-2.0 gio-2.0)

# --- Wayland protocol generation and wayland-client library are no longer needed ---
# --- as we are using D-Bus or X11 calls only for idle_detect ---

# --- Executable Targets ---
add_executable(event_detect ${SOURCES_EVENT_DETECT})

add_executable(idle_detect ${SOURCES_IDLE_DETECT})

# --- Standalone Shared Memory Reader Utility ---
add_executable(read_shmem_timestamps ${SOURCES_READ_SHMEM_TIMESTAMPS})

# --- Target Properties: event_detect ---
if(LIBEVDEV_FOUND)
    target_include_directories(event_detect PRIVATE ${LIBEVDEV_INCLUDE_DIRS})
    target_link_libraries(event_detect PRIVATE ${LIBEVDEV_LIBRARIES})
endif()

# --- Target Properties: idle_detect ---

# Add include directories (Removed CMAKE_CURRENT_BINARY_DIR and WAYLAND_CLIENT includes)
target_include_directories(idle_detect PRIVATE
    # "." is already added globally via include_directories()
    ${X11_INCLUDE_DIR}
    ${XSS_INCLUDE_DIRS}
    ${DBUS_INCLUDE_DIRS}
    ${GLIB_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(idle_detect PRIVATE
    ${X11_LIBRARIES}
    ${XSS_LIBRARIES}
    ${DBUS_LIBRARIES}
    ${GLIB_LIBRARIES} # Includes glib, gobject, gio
)

# Link against librt if needed for shm_open/shm_unlink (often implicit now)
# Use PUBLIC linking if other targets might link against this one, otherwise PRIVATE
target_link_libraries(read_shmem_timestamps PRIVATE rt)

# Installation paths (parameterized)
set(INSTALL_BIN_DIR "${CMAKE_INSTALL_PREFIX}/bin" CACHE PATH "Installation directory for binaries")
set(INSTALL_CONFIG_DIR "/etc" CACHE PATH "Installation directory for configuration files")
set(INSTALL_SYSTEM_SERVICE_DIR "/etc/systemd/system" CACHE PATH "Installation directory for the system level dc_event_detection service")
set(INSTALL_USER_CONFIG_DIR "$ENV{HOME}/.config" CACHE PATH "Installation directory for the user level idle_detect.conf")
set(INSTALL_USER_SERVICE_DIR "$ENV{HOME}/.config/systemd/user" CACHE PATH "Installation directory for the user level dc_idle_detection.service")

# Install the executable and scripts
install(TARGETS event_detect idle_detect read_shmem_timestamps DESTINATION ${INSTALL_BIN_DIR})
install(FILES "dc_pause" "dc_unpause" "idle_detect_wrapper.sh"
        PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ GROUP_EXECUTE GROUP_READ WORLD_EXECUTE WORLD_READ
        DESTINATION ${INSTALL_BIN_DIR} )

# Install configuration file (system-wide)
install(FILES "event_detect.conf" DESTINATION ${INSTALL_CONFIG_DIR})

# Install system service (root ownership)
install(FILES "dc_event_detection.service" DESTINATION ${INSTALL_SYSTEM_SERVICE_DIR})

# --- Install Pre-Generated Doxygen Documentation ---
# Assumes documentation exists in ./docs/ relative to CMakeLists.txt

# Install Man Pages (if the source directory exists)
set(DOXYGEN_MAN_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/docs/man/man3")
if(EXISTS "${DOXYGEN_MAN_SOURCE_DIR}")
    install(DIRECTORY ${DOXYGEN_MAN_SOURCE_DIR}/ # Copy contents of man3/
            DESTINATION ${CMAKE_INSTALL_MANDIR}/man3/  # Install into <prefix>/share/man/man3/
            COMPONENT Documentation                  # <<< FIX: COMPONENT comes BEFORE FILES_MATCHING
            FILES_MATCHING PATTERN "*.3"             # <<< FIX: FILES_MATCHING comes AFTER COMPONENT
            # OPTIONAL # REMOVED OPTIONAL as per previous step, source should exist
    )
    message(STATUS "Man pages from ${DOXYGEN_MAN_SOURCE_DIR} scheduled for installation to ${CMAKE_INSTALL_MANDIR}/man3")
else()
     message(WARNING "Man page source directory not found: ${DOXYGEN_MAN_SOURCE_DIR}")
endif()

# Install HTML Docs (if the source directory exists)
# This block was likely already correct, but ensure COMPONENT is before any potential PATTERN/REGEX
set(DOXYGEN_HTML_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/docs/html")
if(EXISTS "${DOXYGEN_HTML_SOURCE_DIR}")
    install(DIRECTORY ${DOXYGEN_HTML_SOURCE_DIR}/ # Copy contents of html/
            DESTINATION ${CMAKE_INSTALL_DOCDIR}/html # Install into <prefix>/share/doc/<proj>/html/
            COMPONENT Documentation # Optional
    )
    message(STATUS "HTML docs from ${DOXYGEN_HTML_SOURCE_DIR} scheduled for installation to ${CMAKE_INSTALL_DOCDIR}/html")
else()
    message(WARNING "HTML docs source directory not found: ${DOXYGEN_HTML_SOURCE_DIR}")
endif()

# Uninstall target
configure_file(uninstall.cmake.in uninstall.cmake @ONLY)

# Install executables only
add_custom_target(install_executables_only
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_BINARY_DIR}/event_detect" "${INSTALL_BIN_DIR}/event_detect"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_BINARY_DIR}/idle_detect" "${INSTALL_BIN_DIR}/idle_detect"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/idle_detect_wrapper.sh" "${INSTALL_BIN_DIR}/idle_detect_wrapper.sh"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_BINARY_DIR}/read_shmem_timestamps" "${INSTALL_BIN_DIR}/read_shmem_timestamps"

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


