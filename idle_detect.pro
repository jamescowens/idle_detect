# Created by and for Qt Creator This file was created for editing the project sources only.
# You may attempt to use it for building too, by modifying this file here.

#TARGET = idle_detect

QT = core gui widgets

HEADERS =

SOURCES = \
   $$PWD/boinc_idle_detect.conf \
   $$PWD/dc_idle_detection.service \
   $$PWD/dc_pause \
   $$PWD/dc_unpause \
   $$PWD/detect_mouse_movement.sh

INCLUDEPATH =

#DEFINES = 

DISTFILES += \
  README.md \
  dc_event_detection.service \
  event_detect.conf \
  event_detect.sh \
  idle_detect.conf \
  idle_detect.sh \
  idle_detect_resources.sh

