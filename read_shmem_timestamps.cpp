/*
 * Copyright (C) 2025 James C. Owens
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#include <string>
#include <cstdint>
#include <cstring>   // For strerror
#include <iostream>  // Keep for final std::cout output

// POSIX Shared Memory Headers
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>

// Include the project's utility header
#include "util.h"

// Define constants for the shared memory layout
const char* DEFAULT_SHMEM_NAME = "/idle_detect_shmem";
const size_t SHMEM_SIZE = sizeof(int64_t[2]); // Size of the array

int main(int argc, char* argv[]) {
    // --- Argument Parsing ---
    if (argc < 2 || argc > 3) {
        log("%s: Usage: %s <shmem_name> [raw|iso|hr]",
                  (argc > 0 ? argv[0] : "read_shmem_timestamps"));
        log("%s: shmem_name: Name of shared memory segment (e.g., /idle_detect_shmem)");
        log("%s: format (optional): 'raw' (default), 'iso' or 'hr' for human-readable UTC");
        return 1;
    }

    const char* shm_name = argv[1];
    std::string format_arg = "raw";
    bool human_readable = false;

    if (argc == 3) {
        format_arg = argv[2];
        std::string format_lower = ToLower(format_arg);

        if (format_lower == "iso" || format_lower == "hr") {
            human_readable = true;
        } else if (format_lower != "raw") {
            log("WARN: %s: Unknown format '%s'. Defaulting to 'raw'.", __func__, argv[2]);
            // format remains "raw", human_readable remains false
        }
    }

    // --- Read Shared Memory ---
    int shm_fd = -1;
    void* mapped_mem = MAP_FAILED;
    int64_t* shm_ptr = nullptr;
    int64_t update_time = -1;
    int64_t last_active_time = -1;
    bool read_success = false;

    errno = 0;
    shm_fd = shm_open(shm_name, O_RDONLY, 0);
    if (shm_fd == -1) {
        error_log("%s: shm_open(RO) failed for '%s': %s (%d)", __func__, shm_name, strerror(errno), errno);
        return 2;
    }

    errno = 0;
    mapped_mem = mmap(nullptr, SHMEM_SIZE, PROT_READ, MAP_SHARED, shm_fd, 0);
    close(shm_fd); // Close FD immediately

    if (mapped_mem == MAP_FAILED) {
        error_log("ERROR: %s: mmap failed for shm '%s': %s (%d)",
                  __func__, shm_name, strerror(errno), errno);
        // No fd to close here
        return 3;
    }

    // --- Access Data ---
    shm_ptr = static_cast<int64_t*>(mapped_mem);
    update_time = shm_ptr[0];
    last_active_time = shm_ptr[1];
    read_success = true;

    // Unmap memory
    errno = 0;
    if (munmap(mapped_mem, SHMEM_SIZE) == -1) {
        log("WARN: %s: munmap failed for shm '%s': %s (%d)",
            __func__, shm_name, strerror(errno), errno);
        // Continue as we already have the value
    }
    mapped_mem = nullptr;
    shm_ptr = nullptr;

    // --- Unmap ---
    errno = 0;
    if (munmap(mapped_mem, SHMEM_SIZE) == -1) {
        log("WARN: %s: munmap failed for shm '%s': %s (%d)", __func__, shm_name, strerror(errno), errno);
        // Continue, we already read the data
    }

    // --- Output ---
    if (read_success) {
        if (human_readable) {
            std::string fmt_update = FormatISO8601DateTime(update_time);
            std::string fmt_last_active = FormatISO8601DateTime(last_active_time);
            if (fmt_update.empty() || fmt_last_active.empty()) {
                error_log("%s: Failed to format one or both timestamps (%lld, %lld)",
                          __func__,
                          (long long)update_time, (long long)last_active_time);
                return 5;
            }
            // Output space-separated ISO strings to standard output
            std::cout << fmt_update << " " << fmt_last_active << std::endl;
        } else {
            // Output space-separated raw epoch seconds to standard output
            std::cout << update_time << " " << last_active_time << std::endl;
        }
        return 0; // Success
    } else {
        error_log("%s: Failed to read data from shared memory '%s'.", __func__, shm_name);
        return 4;
    }
}
