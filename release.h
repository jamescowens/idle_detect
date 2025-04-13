/*
 * Copyright (C) 2025 James C. Owens
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#ifndef RELEASE_H
#define RELEASE_H

#include <string>

// release.h (Alternative)
#define IDLE_DETECT_VERSION_MAJOR 0
#define IDLE_DETECT_VERSION_MINOR 7
#define IDLE_DETECT_VERSION_PATCH 6
#define IDLE_DETECT_VERSION_TWEAK 0

#define ID__STRINGIFY(x) #x
#define ID_STRINGIFY(x) ID__STRINGIFY(x)

#define IDLE_DETECT_VERSION_STRING \
ID_STRINGIFY(IDLE_DETECT_VERSION_MAJOR) "." \
    ID_STRINGIFY(IDLE_DETECT_VERSION_MINOR) "." \
    ID_STRINGIFY(IDLE_DETECT_VERSION_PATCH) "." \
    ID_STRINGIFY(IDLE_DETECT_VERSION_TWEAK)

const std::string g_version_datetime = "20250413";

const std::string g_version = std::string("version ") + std::string(IDLE_DETECT_VERSION_STRING) + " - " + g_version_datetime;

#endif // RELEASE_H
