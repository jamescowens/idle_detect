#!/bin/bash

# User-level installation script for idle_detect service
# Run this script AS YOUR REGULAR USER after running 'sudo ./install.sh'.

# Exit immediately if a command exits with a non-zero status.
set -e

# --- Configuration ---
# Source files (expected in current directory where script is run)
USER_CONFIG_SOURCE="idle_detect.conf"
USER_SERVICE_SOURCE="dc_idle_detection.service"

# Destination directories (use XDG standards if available, else default)
USER_CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
USER_SYSTEMD_DIR="${USER_CONFIG_HOME}/systemd/user"
USER_CONFIG_DEST="${USER_CONFIG_HOME}/idle_detect.conf"
USER_SERVICE_DEST="${USER_SYSTEMD_DIR}/dc_idle_detection.service"
# --- End Configuration ---

echo "--- Starting User-Level Installation ---"

# --- Check if source files exist ---
if [ ! -f "$USER_CONFIG_SOURCE" ]; then
    echo "ERROR: Source file not found: $USER_CONFIG_SOURCE"
    echo "       Please run this script from the project's root directory."
    exit 1
fi
if [ ! -f "$USER_SERVICE_SOURCE" ]; then
    echo "ERROR: Source file not found: $USER_SERVICE_SOURCE"
    echo "       Please run this script from the project's root directory."
    exit 1
fi

# --- Create Directories ---
echo "INFO: Ensuring user directories exist..."
mkdir -p "$USER_SYSTEMD_DIR" # Create ~/.config/systemd/user if needed

# --- Install User Config File (if it doesn't exist) ---
if [ ! -f "$USER_CONFIG_DEST" ]; then
    echo "INFO: Copying default user config to $USER_CONFIG_DEST"
    cp "$USER_CONFIG_SOURCE" "$USER_CONFIG_DEST"
    chmod 600 "$USER_CONFIG_DEST" # Restrict permissions
else
    echo "INFO: User config already exists at $USER_CONFIG_DEST (skipping copy)."
fi

# --- Install User Service File ---
echo "INFO: Copying user service file to $USER_SERVICE_DEST"
cp "$USER_SERVICE_SOURCE" "$USER_SERVICE_DEST"
chmod 644 "$USER_SERVICE_DEST" # Standard service file permissions

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
