// read_shmem_timestamp.cpp
#include <iostream>
#include <string>
#include <atomic>
#include <cstdint>
#include <algorithm>
#include <cstring> // For strerror

// POSIX Shared Memory Headers
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>

// Include the project's utility header
#include "util.h"

const char* SHM_NAME_READER = "/event_detect_last_active";

std::atomic<bool> g_debug = false;

int main(int argc, char* argv[]) {
    // --- Argument Parsing ---
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: " << (argc > 0 ? argv[0] : "read_shmem_timestamp")
        << " <shmem_name> [raw|iso|hr]" << std::endl;
        std::cerr << "  shmem_name: Name of shared memory segment relative to /dev/shmem (e.g., /event_detect_last_active)"
                  << std::endl;
        std::cerr << "  format (optional): 'raw' (default) for int64_t number,"
                  << " 'iso' or 'hr' for human-readable UTC" << std::endl;
        return 1;
    }

    const char* shm_name = argv[1];

    std::string format = "raw";
    bool human_readable = false;

    if (argc == 3) {
        format = argv[2];
        std::transform(format.begin(), format.end(), format.begin(), ::tolower);
        if (format == "iso" || format == "hr") {
            human_readable = true;
        } else if (format != "raw") {
            std::cerr << "Warning: Unknown format '" << argv[2] << "'. Defaulting to 'raw'." << std::endl;
            // revert to format = raw (i.e. human_readable = false)
        }
    }

    // --- Shared Memory Reading (as before) ---
    int shm_fd = -1;
    std::atomic<int64_t>* shm_ptr = nullptr;
    int64_t timestamp = 0;
    bool read_success = false;

    errno = 0;
    shm_fd = shm_open(shm_name, O_RDONLY, 0);
    if (shm_fd == -1) {
        // ... (error handling as before) ...
        return 2;
    }

    errno = 0;
    void* mapped_mem = mmap(nullptr, sizeof(int64_t), PROT_READ, MAP_SHARED, shm_fd, 0);
    close(shm_fd);

    if (mapped_mem == MAP_FAILED) {
        // ... (error handling as before) ...
        return 3;
    }

    shm_ptr = static_cast<std::atomic<int64_t>*>(mapped_mem);
    timestamp = shm_ptr->load(std::memory_order_relaxed);
    read_success = true;

    errno = 0;
    if (munmap(shm_ptr, sizeof(int64_t)) == -1) {
        std::cerr << "Warning: munmap failed for shm '" << shm_name << "': " << strerror(errno) << std::endl;
    }

    // --- Output ---
    if (read_success) {
        if (human_readable) {
            // Call the function from util.h/util.cpp directly
            std::cout << FormatISO8601DateTime(timestamp) << std::endl;
        } else {
            std::cout << timestamp << std::endl;
        }
        return 0; // Success
    } else {
        std::cerr << "Error: Failed to read timestamp from shared memory." << std::endl;
        return 4;
    }
}
