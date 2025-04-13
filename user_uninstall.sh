#!/bin/bash

# User-level uninstallation script for idle_detect service
# Run this script AS YOUR REGULAR USER before running 'sudo ./uninstall.sh'.

# --- Configuration ---
USER_CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
USER_SYSTEMD_DIR="${USER_CONFIG_HOME}/systemd/user"
USER_CONFIG_DEST="${USER_CONFIG_HOME}/idle_detect.conf"
USER_SERVICE_DEST="${USER_SYSTEMD_DIR}/dc_idle_detection.service"
SERVICE_NAME="dc_idle_detection.service"
# --- End Configuration ---

echo "--- Starting User-Level Uninstallation ---"

# --- Disable and Stop User Service ---
echo "INFO: Disabling and stopping user service '$SERVICE_NAME'..."
# Use || true to ignore errors if service doesn't exist or isn't active/enabled
systemctl --user disable --now "$SERVICE_NAME" || true

# --- Remove User Service File ---
echo "INFO: Removing user service file: $USER_SERVICE_DEST"
rm -f "$USER_SERVICE_DEST"

# --- Optionally Remove User Config File ---
if [ -f "$USER_CONFIG_DEST" ]; then
    read -p "Remove user configuration file ($USER_CONFIG_DEST)? [y/N] " response
    case "$response" in
        [yY][eE][sS]|[yY])
            echo "INFO: Removing user configuration file: $USER_CONFIG_DEST"
            rm -f "$USER_CONFIG_DEST"
            ;;
        *)
            echo "INFO: Skipping removal of user configuration file."
            ;;
    esac
fi

# --- Systemd User Daemon Reload ---
echo "INFO: Reloading systemd user instance..."
systemctl --user daemon-reload

echo ""
echo "--- User-Level Uninstallation Complete ---"
echo "User service stopped/disabled and files removed (config removal based on prompt)."
echo "Now run 'sudo ./uninstall.sh' to remove system components."
echo ""

exit 0
