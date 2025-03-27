#
# Copyright (C) 2025 James C. Owens
#
# This code is licensed under the MIT license. See LICENSE.md in the repository.
#

TARGET = event_detect

# QMAKE_CC = /usr/bin/gcc-14
# QMAKE_CXX = /usr/bin/g++-14

QMAKE_CXXFLAGS += -std=c++17

# QT = core gui widgets

HEADERS = \
    event_detect.h \
    tinyformat.h \
    util.h

SOURCES = \
   event_detect.cpp \
   util.cpp

INCLUDEPATH = /usr/include/libevdev-1.0

LIBS += -levdev

#DEFINES =

DISTFILES += \
  README.md \
  dc_event_detection.service \
  event_detect.conf \
  event_detect.sh \
  dc_idle_detection.service \
  idle_detect.conf \
  idle_detect.sh \
  idle_detect_resources.sh \
  dc_pause \
  dc_unpause

