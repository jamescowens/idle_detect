/*
 * Copyright (C) 2025 James C. Owens
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#ifndef IDLE_DETECT_H
#define IDLE_DETECT_H

#include <cstdint> // For int64_t

#include <util.h>

namespace IdleDetect {

// Global fixed or default config parameter(s).
int DEFAULT_IDLE_THRESHOLD_SECONDS = 0; // This is set in main from config, and config has the default value.
constexpr int DEFAULT_CHECK_INTERVAL_SECONDS = 1;

// Function to get the user's idle time in seconds
int64_t GetIdleTimeSeconds();

} // namespace IdleDetect

//!
//! \brief The IdleDetectConfig class. This specializes the Config class and implements the virtual method ProcessArgs()
//! for idle_detect.
//!
class IdleDetectConfig : public Config
{
    void ProcessArgs() override;
};

#endif // IDLE_DETECT_H
