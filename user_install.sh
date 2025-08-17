#!/bin/bash

# User-level installation script for idle_detect service
# Run this script AS YOUR REGULAR USER after running 'sudo ./install.sh'.

# Exit immediately if a command exits with a non-zero status.
set -e

# --- Configuration ---
# Destination directories (use XDG standards if available, else default)
USER_CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
USER_SYSTEMD_DIR="${USER_CONFIG_HOME}/systemd/user"
USER_CONFIG_DEST="${USER_CONFIG_HOME}/idle_detect.conf"
USER_SERVICE_DEST="${USER_SYSTEMD_DIR}/dc_idle_detection.service"
# --- End Configuration ---

echo "--- Starting User-Level Setup for idle_detect ---"

# --- Install User Config File (if it doesn't exist) ---
if [ ! -f "$USER_CONFIG_DEST" ]; then
    echo "INFO: Copying default user config to $USER_CONFIG_DEST"
    cp "$USER_CONFIG_SOURCE" "$USER_CONFIG_DEST"
    chmod 600 "$USER_CONFIG_DEST" # Restrict permissions
else
    echo "INFO: User config already exists at $USER_CONFIG_DEST (skipping copy)."
fi

# --- Systemd User Daemon Reload ---
echo "INFO: Reloading systemd user instance..."
systemctl --user daemon-reload

# --- Enable and Start User Service ---
echo "INFO: Enabling and starting user service 'dc_idle_detection.service'..."
if systemctl --user enable --now dc_idle_detection.service ; then
    echo "INFO: User service enabled and started successfully."
else
    echo "WARN: Failed to enable/start user service. Check status with:"
    echo "      systemctl --user status dc_idle_detection.service"
    echo "      journalctl --user -u dc_idle_detection.service"
fi

# --- Final Instructions ---
echo ""
echo "--- User-Level Installation Complete ---"
echo "The user service 'dc_idle_detection.service' has been installed and started."
echo "You may want to customize your configuration:"
echo "  nano $USER_CONFIG_DEST"
echo "If you change the config, restart the service:"
echo "  systemctl --user restart dc_idle_detection.service"
echo ""

exit 0
