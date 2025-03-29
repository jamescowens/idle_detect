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

# Install target (optional)
#install(TARGETS idle_detect DESTINATION bin)
