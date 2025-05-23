#!/bin/sh

# Wrapper script to wait for necessary session environment variables
# before executing the actual idle_detect binary.

# Configuration
MAX_WAIT_SECONDS=30 # Total time to wait (e.g., 30 seconds)
WAIT_INTERVAL=2   # Time between checks (e.g., 2 seconds)
CONFIG_FILE_ARG="$1" # Assume config file is the first argument

# Function to log messages (optional, adjust as needed)
log_message() {
    echo "idle_detect_wrapper: $1" >&2 # Log to stderr
}

# Wait loop
SECONDS_WAITED=0
ENV_READY=0
while [ $SECONDS_WAITED -lt $MAX_WAIT_SECONDS ]; do
    log_message "Checking environment (waited ${SECONDS_WAITED}s)..."

    # Check for D-Bus Address using systemctl show-environment
    DBUS_ADDR=$(systemctl --user show-environment | grep '^DBUS_SESSION_BUS_ADDRESS=')

    # Check for either DISPLAY or WAYLAND_DISPLAY using systemctl show-environment
    DISP_VAR=$(systemctl --user show-environment | grep -q -E '^(DISPLAY=|WAYLAND_DISPLAY=)' && echo "found")

    # Check if both conditions are met
    if [ -n "$DBUS_ADDR" ] && [ -n "$DISP_VAR" ]; then
        log_message "Found DBUS_SESSION_BUS_ADDRESS and DISPLAY/WAYLAND_DISPLAY."
        log_message "DBUS_ADDR = $DBUS_ADDR, DISPLAY = $DISPLAY, WAYLAND_DISPLAY = $WAYLAND_DISPLAY"
        ENV_READY=1
        break
    fi

    # Log specific missing variables for debugging
    if [ -z "$DBUS_ADDR" ]; then log_message " DBUS_SESSION_BUS_ADDRESS missing"; fi
    if [ -z "$DISP_VAR" ]; then log_message " DISPLAY/WAYLAND_DISPLAY missing"; fi

    sleep $WAIT_INTERVAL
    SECONDS_WAITED=$((SECONDS_WAITED + WAIT_INTERVAL))
done

# Check if loop timed out
if [ "$ENV_READY" -eq 0 ]; then
    log_message "ERROR: Timed out after ${SECONDS_WAITED}s waiting for session environment variables. Exiting."
    exit 1
fi

# Environment seems ready, execute the main program
log_message "Environment ready. Executing /usr/local/bin/idle_detect..."
# Use exec to replace this script process with the idle_detect process
# Pass the original config file argument
exec /usr/local/bin/idle_detect "$CONFIG_FILE_ARG"

# Exit with error if exec fails for some reason (shouldn't happen)
log_message "ERROR: Failed to exec /usr/local/bin/idle_detect. Exiting."
exit 1
