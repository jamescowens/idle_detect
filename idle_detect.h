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

// Configuration parameters (can be made configurable later)
constexpr int DEFAULT_IDLE_THRESHOLD_SECONDS = 30;
constexpr int DEFAULT_CHECK_INTERVAL_SECONDS = 5;

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
