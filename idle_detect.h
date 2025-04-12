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
// Function to get the user's idle time in seconds
int64_t GetIdleTimeSeconds();

} // namespace IdleDetect

//!
//! \brief The IdleDetectConfig class. This specializes the Config class and implements the virtual method ProcessArgs()
//! for idle_detect.
//!
class IdleDetectConfig : public Config
{
    //!
    //! \brief The is the ProcessArgs() implementation for idle_detect.
    //!
    void ProcessArgs() override;
};

#endif // IDLE_DETECT_H
