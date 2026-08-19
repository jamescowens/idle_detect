#!/bin/bash

# User-level installation script for idle_detect service
# Run this script AS YOUR REGULAR USER after running 'sudo ./install.sh'.

# Exit immediately if a command exits with a non-zero status.
set -e

# --- Configuration ---
# Destination directories (use XDG standards if available, else default)
USER_CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
USER_CONFIG_DEST="${USER_CONFIG_HOME}/idle_detect.conf"
SERVICE_NAME="dc_idle_detection.service"

# --- Find the system-wide default config template ---
DEFAULT_CONFIG_SOURCE=""
# Define potential installation locations to search
SEARCH_PATHS=(
    "/usr/local/share/idle_detect/idle_detect.conf.default"
    "/usr/share/idle_detect/idle_detect.conf.default"
)

# Loop through search paths and find the first one that exists
for path in "${SEARCH_PATHS[@]}"; do
    if [ -f "$path" ]; then
        DEFAULT_CONFIG_SOURCE="$path"
        break # Stop searching once found
    fi
done

# --- Start Setup ---
echo "--- Starting User-Level Setup for idle_detect ---"

# --- Install User Config File (if it doesn't exist) ---
if [ ! -f "$USER_CONFIG_DEST" ]; then
    # Check if we found the source template file
    if [ -z "$DEFAULT_CONFIG_SOURCE" ]; then
        echo "ERROR: Could not find the default config template in any of the following locations:" >&2
        printf "         - %s\n" "${SEARCH_PATHS[@]}" >&2
        echo "       Please run 'sudo ./install.sh' or install the package first." >&2
        exit 1
    fi
    echo "INFO: Copying default user config from $DEFAULT_CONFIG_SOURCE to $USER_CONFIG_DEST"
    # Ensure the target directory exists
    mkdir -p "$(dirname "$USER_CONFIG_DEST")"
    cp "$DEFAULT_CONFIG_SOURCE" "$USER_CONFIG_DEST"
    chmod 600 "$USER_CONFIG_DEST" # Restrict permissions
else
    echo "INFO: User config already exists at $USER_CONFIG_DEST (skipping copy)."
fi

# --- Systemd User Daemon Reload ---
echo "INFO: Reloading systemd user instance..."
systemctl --user daemon-reload

# --- Enable and Start User Service ---
echo "INFO: Enabling and starting user service '$SERVICE_NAME'..."

# Same three hazards as the system service, so handle them the same way.
#
# A drop-in under ~/.config/systemd/user outlives any reinstall and can pin ExecStart at a path
# this install no longer provides; 'systemctl --user cat' does not reveal that an override is in
# effect, only DropInPaths does.
USER_DROP_IN_PATHS="$(systemctl --user show "$SERVICE_NAME" -p DropInPaths --value 2>/dev/null)"
if [ -n "$USER_DROP_IN_PATHS" ]; then
    for drop_in in $USER_DROP_IN_PATHS; do
        [ -r "$drop_in" ] || continue
        grep -qs '^ExecStart=.' "$drop_in" || continue

        pinned_exec="$(grep -hs '^ExecStart=.' "$drop_in" | tail -n 1 | sed 's/^ExecStart=//' | awk '{print $1}')"
        if [ -n "$pinned_exec" ] && [ ! -x "$pinned_exec" ]; then
            echo "WARN: Drop-in '${drop_in}' pins ExecStart to '${pinned_exec}', which is not executable."
            echo "WARN: The user service will fail to start until that drop-in is updated or removed."
        elif [ -n "$pinned_exec" ]; then
            echo "INFO: Drop-in '${drop_in}' pins ExecStart to '${pinned_exec}'."
        fi
    done
fi

# The unit sets StartLimitBurst, so a prior failure can latch it into 'failed' and refuse further
# start requests until cleared.
systemctl --user reset-failed "$SERVICE_NAME" 2>/dev/null || true

# 'enable --now' will not restart an already-running instance, which on an upgrade leaves the old
# binary executing against a deleted inode.
if systemctl --user is-active --quiet "$SERVICE_NAME"; then
    echo "INFO: User service is already running; restarting so the new binary takes effect."
    systemctl --user enable "$SERVICE_NAME" > /dev/null 2>&1 || true
    if systemctl --user restart "$SERVICE_NAME" ; then
        echo "INFO: User service restarted successfully."
    else
        echo "WARN: Failed to restart user service. Check status with:"
        echo "      systemctl --user status $SERVICE_NAME"
        echo "      journalctl --user -u $SERVICE_NAME"
    fi
elif systemctl --user enable --now "$SERVICE_NAME" ; then
    echo "INFO: User service enabled and started successfully."
else
    echo "WARN: Failed to enable/start user service. Check status with:"
    echo "      systemctl --user status $SERVICE_NAME"
    echo "      journalctl --user -u $SERVICE_NAME"
fi

# --- Final Instructions ---
echo ""
echo "--- User-Level Installation Complete ---"
echo "The user service '$SERVICE_NAME' has been installed and started."
echo "You may want to customize your configuration:"
echo "  nano $USER_CONFIG_DEST"
echo "If you change the config, restart the service:"
echo "  systemctl --user restart $SERVICE_NAME"
echo ""

exit 0
