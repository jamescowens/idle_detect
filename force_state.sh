#!/bin/bash

# Script to send control commands to the idle_detect user service pipe.

debug=0

# --- Try to source resources ---
resource_file="idle_detect_resources.sh"
sourced=0

# Get the directory the script *itself* resides in, handling symlinks
script_path=$(readlink -f "$0")
script_dir=$(dirname "$script_path")

# 1. Try relative path first (for running from build/source dir)
if [ -f "${script_dir}/${resource_file}" ]; then
    source "${script_dir}/${resource_file}"
    sourced=1
    # Use printf for stderr logging before resources might be sourced
    # printf "DEBUG: Sourced resources from script directory: %s\n" "${script_dir}" >&2
fi

# 2. If not found, check standard installation paths
if [ "$sourced" -eq 0 ]; then
    # Define potential installation locations (adjust if necessary)
    # Assumes package name used in path is 'idle_detect'
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
            source "${dir}/${resource_file}"
            sourced=1
            printf "DEBUG: Sourced resources from install directory: %s\n" "${dir}" >&2
            break # Stop searching once found
        fi
    done
fi

# 3. Check if sourcing succeeded, exit if not
if [ "$sourced" -eq 0 ]; then
    echo "ERROR: Cannot find required resource file: ${resource_file}" >&2
    echo "       Searched relative path (${script_dir}) and standard locations:" >&2
    printf "         - %s\n" "${search_paths[@]}" >&2
    exit 1
fi

# --- End sourcing ---

idle_detect_control_pipe="$XDG_RUNTIME_DIR/idle_detect_control_pipe"
debug_log "idle_detect_control_pipe = $idle_detect_control_pipe"

current_time="$(timestamp)"

user_force_active() {
        debug_log "INFO: sending $current_time:USER_FORCE_ACTIVE to $idle_detect_control_pipe"

        echo "$current_time:USER_FORCE_ACTIVE" > "$idle_detect_control_pipe"
}

user_force_idle() {
        debug_log "INFO: sending $current_time:USER_FORCE_IDLE to $idle_detect_control_pipe"

        echo "$current_time:USER_FORCE_IDLE" > "$idle_detect_control_pipe"
}

user_unforce() {
        debug_log "INFO: sending $current_time:USER_UNFORCE to $idle_detect_control_pipee"

        echo "$current_time:USER_UNFORCE" > "$idle_detect_control_pipe"
}

# --- Argument Parsing ---
if [ $# -eq 0 ]; then
     echo "Usage: $(basename "$0") <command>"
     echo "Commands: force_active, force_idle, unforce | normal"
     exit 1
fi

processed=0
# Process only the first argument as the command
case "$1" in
    force_active)
        user_force_active
        processed=1
        ;;
    force_idle)
        user_force_idle
        processed=1
        ;;
    unforce | normal) # Allow synonyms
        user_unforce
        processed=1
        ;;
    *)
        # Unknown argument
        error_log "Unknown command '%s'. Use force_active, force_idle, or unforce/normal." "$1"
        exit 1
        ;;
esac

# This check might be redundant if the case statement handles all possibilities or exits
if [ "$processed" -eq 0 ]; then
    error_log "No valid command processed."
    exit 1
fi
log "Command sent successfully to $idle_detect_control_pipe."
exit 0
