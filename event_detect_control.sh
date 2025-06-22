#!/bin/bash

# (c) Copyright 2025 James C. Owens

# This code is licensed under the MIT license. See LICENSE.md in the repository.

# Script to send GLOBAL control commands directly to the event_detect service pipe.
# MUST BE RUN WITH SUDO.

# Exit immediately if a command exits with a non-zero status.
set -e

# --- Configuration ---
# Set debug=1 to enable debug_log messages from the sourced resource file.
# The sourced script checks 'if ((debug == 1))'.
debug=0
# --- End Configuration ---

# --- Try to source resources ---
# (This sourcing logic is correct and remains unchanged)
resource_file="idle_detect_resources.sh"
sourced=0
script_path=$(readlink -f "$0")
script_dir=$(dirname "$script_path")
if [ -f "${script_dir}/${resource_file}" ]; then
    source "${script_dir}/${resource_file}"
    sourced=1
fi
if [ "$sourced" -eq 0 ]; then
    search_paths=(
        "/usr/local/bin"
        "/usr/local/libexec/idle_detect"
        "/usr/local/share/idle_detect"
        "/usr/bin"
        "/usr/share/idle_detect"
        "/usr/libexec/idle_detect"
    )
    for dir in "${search_paths[@]}"; do
        if [ -f "${dir}/${resource_file}" ]; then
            source "${dir}/${resource_file}"; sourced=1; break
        fi
    done
fi
if [ "$sourced" -eq 0 ]; then
    echo "ERROR: Cannot find required resource file: ${resource_file}" >&2
    exit 1
fi
# --- End sourcing ---


# --- Check for sudo/root privileges ---
if [ "$(id -u)" -ne 0 ]; then
  echo "ERROR: This script must be run with sudo to access the system-level pipe." >&2
  exit 1
fi


# --- Main Script Logic ---

# --- Dynamically determine pipe path ---
DEFAULT_EVENT_DETECT_DIR="/run/event_detect"
EVENT_DETECT_DIR="$DEFAULT_EVENT_DETECT_DIR"
CONFIG_FILE="/etc/event_detect.conf"

debug_log "INFO: Checking for config file at $CONFIG_FILE" # Correct log call
if [ -r "$CONFIG_FILE" ]; then
    CONFIG_PATH_LINE=$(grep -E '^[[:space:]]*event_count_files_path[[:space:]]*=' "$CONFIG_FILE" | tail -n 1)
    if [ -n "$CONFIG_PATH_LINE" ]; then
        CONFIG_PATH_VAL=$(echo "$CONFIG_PATH_LINE" | cut -d'=' -f2-)
        CONFIG_PATH_FINAL=$(echo "$CONFIG_PATH_VAL" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' -e 's/^"//' -e 's/"$//' -e "s/^'//" -e "s/'$//")
        if [ -n "$CONFIG_PATH_FINAL" ]; then
            EVENT_DETECT_DIR="$CONFIG_PATH_FINAL"
            log "INFO: Using event_detect path from config: $EVENT_DETECT_DIR" # Correct log call
        else
            log "WARN: Found 'event_count_files_path' in config but value is empty. Using default." # Correct log call
        fi
    else
        log "INFO: 'event_count_files_path' not found in $CONFIG_FILE. Using default." # Correct log call
    fi
else
    log "INFO: Config file $CONFIG_FILE not found or not readable. Using default." # Correct log call
fi

# Construct the full pipe path
EVENT_DETECT_PIPE="${EVENT_DETECT_DIR}/event_registration_pipe"
# --- End Dynamic Path Logic ---


# Check if pipe exists before trying to write to it
if [ ! -p "$EVENT_DETECT_PIPE" ]; then
    # Use standard echo to stderr for errors, since error_log doesn't exist
    echo "ERROR: Control pipe not found: $EVENT_DETECT_PIPE. Is the event_detect service running?" >&2
    exit 1
fi

# Get current time using helper function
current_time=$(timestamp)

# --- Functions to send commands (logging calls fixed) ---
global_force_active() {
        debug_log "INFO: sending ${current_time}:USER_FORCE_ACTIVE to ${EVENT_DETECT_PIPE}"
        echo "${current_time}:USER_FORCE_ACTIVE" > "$EVENT_DETECT_PIPE"
}

global_force_idle() {
        debug_log "INFO: sending ${current_time}:USER_FORCE_IDLE to ${EVENT_DETECT_PIPE}"
        echo "${current_time}:USER_FORCE_IDLE" > "$EVENT_DETECT_PIPE"
}

global_unforce() {
        debug_log "INFO: sending ${current_time}:USER_UNFORCE to ${EVENT_DETECT_PIPE}"
        echo "${current_time}:USER_UNFORCE" > "$EVENT_DETECT_PIPE"
}

# --- Argument Parsing ---
if [ $# -eq 0 ]; then
     echo "Usage: $(basename "$0") <command>"
     echo "Commands: force_active, force_idle, unforce | normal"
     exit 1
fi

# Process only the first argument as the command
case "$1" in
    force_active)
        global_force_active
        ;;
    force_idle)
        global_force_idle
        ;;
    unforce|normal) # Allow synonyms
        global_unforce
        ;;
    *)
        # Unknown argument
        echo "ERROR: Unknown command '$1'. Use force_active, force_idle, or unforce/normal." >&2
        exit 1
        ;;
esac

log "Global control command sent successfully to $EVENT_DETECT_PIPE."
exit 0
