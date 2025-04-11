/*
 * Copyright (C) 2025 James C. Owens
 *
 * This code is licensed under the MIT license. See LICENSE.md in the repository.
 */

#include <string>
#include <atomic>
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

int main(int argc, char* argv[]) {
    // --- Argument Parsing ---
    if (argc < 2 || argc > 3) {
        error_log("%s: Usage: %s <shmem_name> [raw|iso|hr]",
                  __func__, (argc > 0 ? argv[0] : "read_shmem_timestamp"));
        error_log("%s: shmem_name: Name of shared memory segment (e.g., /event_detect_last_active)", __func__);
        error_log("%s: format (optional): 'raw' (default), 'iso' or 'hr' for human-readable UTC", __func__);
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
            // Use log for non-fatal warnings (stdout)
            log("WARN: %s: Unknown format '%s'. Defaulting to 'raw'.", __func__, argv[2]);
            // format remains "raw", human_readable remains false
        }
    }

    // --- Shared Memory Reading ---
    int shm_fd = -1;
    // Map as void* first
    void* mapped_mem = MAP_FAILED;
    // Pointer to the specific atomic type in shared memory
    std::atomic<int64_t>* shm_atomic_ptr = nullptr;

    int64_t timestamp = 0;
    bool read_success = false;

    errno = 0; // Clear errno before system call
    shm_fd = shm_open(shm_name, O_RDONLY, 0);
    if (shm_fd == -1) {
        error_log("%s: shm_open failed for name '%s': %s (%d)",
                  __func__, shm_name, strerror(errno), errno);
        return 2;
    }

    errno = 0;
    // Map exactly the size of the atomic type
    mapped_mem = mmap(nullptr, sizeof(std::atomic<int64_t>), PROT_READ, MAP_SHARED, shm_fd, 0);

    // File descriptor can be closed immediately after mmap
    close(shm_fd);
    shm_fd = -1; // Mark as closed

    if (mapped_mem == MAP_FAILED) {
        error_log("ERROR: %s: mmap failed for shm '%s': %s (%d)",
                  __func__, shm_name, strerror(errno), errno);
        // No fd to close here
        return 3;
    }

    // Cast mapped memory and load the timestamp atomically
    shm_atomic_ptr = static_cast<std::atomic<int64_t>*>(mapped_mem);
    timestamp = shm_atomic_ptr->load(std::memory_order_relaxed);
    read_success = true; // Value loaded successfully

    // Unmap memory
    errno = 0;
    if (munmap(mapped_mem, sizeof(std::atomic<int64_t>)) == -1) {
        log("WARN: %s: munmap failed for shm '%s': %s (%d)",
            __func__, shm_name, strerror(errno), errno);
        // Continue as we already have the value
    }
    mapped_mem = nullptr;
    shm_atomic_ptr = nullptr;

    // --- Output ---
    if (read_success) {
        if (human_readable) {
            std::string formatted_time = FormatISO8601DateTime(timestamp);

            if (formatted_time.empty()) {
                error_log("ERROR: %s: Failed to format timestamp %lld", __func__, (long long)timestamp);
                return 5; // Formatting error exit code
            }

            // Keep final success output on std::cout for scripting
            std::cout << formatted_time << std::endl;
        } else {

            // Keep final success output on std::cout for scripting
            std::cout << timestamp << std::endl;
        }
        return 0; // Success exit code
    } else {
        // Should ideally not be reached if errors above exit, but included as a fallback
        error_log("ERROR: %s: Failed to read timestamp from shared memory (unexpected state).", __func__);
        return 4;
    }
}
