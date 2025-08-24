#!/bin/bash

# Uninstallation script for event_detect and idle_detect services
# Run this script with sudo: sudo ./uninstall.sh

# Exit immediately if a command exits with a non-zero status.
set -e

# --- Configuration ---
BUILD_DIR="build/cmake" # Assumes build dir exists and contains uninstall.cmake
SERVICE_USER="event_detect"
SERVICE_GROUP="event_detect"
# --- End Configuration ---

# --- Check for sudo ---
if [ "$(id -u)" -ne 0 ]; then
  echo "ERROR: Please run this script with sudo."
  exit 1
fi

echo "--- Starting System-Level Uninstallation ---"
echo "*** IMPORTANT: Run './user_uninstall.sh' as the regular user FIRST ***"
read -p "Have you run './user_uninstall.sh' as the regular user already? [y/N] " response
case "$response" in
    [yY][eE][sS]|[yY])
        echo "Proceeding..."
        ;;
    *)
        echo "Please run './user_uninstall.sh' as the user first. Aborting."
        exit 1
        ;;
esac


# --- Disable and Stop System Service ---
echo "INFO: Disabling and stopping system service 'dc_event_detection.service'..."
if systemctl is-active --quiet dc_event_detection.service; then
    systemctl disable --now dc_event_detection.service
else
    echo "INFO: System service not active."
    systemctl disable dc_event_detection.service || true # Try disabling even if not active
fi

# --- Run CMake Uninstall Script ---
# This removes files installed by 'cmake --install' based on install_manifest.txt
UNINSTALL_SCRIPT="${BUILD_DIR}/uninstall.cmake"
if [ -f "$UNINSTALL_SCRIPT" ]; then
    echo "INFO: Running CMake uninstall script ($UNINSTALL_SCRIPT)..."
    # This executes the manifest reading logic
    cmake -P "$UNINSTALL_SCRIPT"
else
    echo "WARNING: CMake uninstall script not found at $UNINSTALL_SCRIPT."
    echo "         System files installed by 'cmake --install' might need manual removal."
fi

# --- Remove Runtime Directory ---
RUNTIME_DIR="/run/event_detect"
if [ -d "$RUNTIME_DIR" ]; then
    echo "INFO: Removing runtime directory $RUNTIME_DIR..."
    rm -rf "$RUNTIME_DIR"
fi

# --- Systemd Reload ---
echo "INFO: Reloading systemd manager configuration..."
systemctl daemon-reload

# --- User/Group Removal (Optional) ---
echo ""
read -p "Do you want to remove the system user '${SERVICE_USER}' and group '${SERVICE_GROUP}'? (This is for a complete purge) [y/N] " purge_response
case "$purge_response" in
    [yY][eE][sS]|[yY])
        echo "INFO: Removing user '${SERVICE_USER}' and group '${SERVICE_GROUP}'..."
        userdel "$SERVICE_USER" || echo "WARN: Could not remove user '${SERVICE_USER}' (might not exist)."
        groupdel "$SERVICE_GROUP" || echo "WARN: Could not remove group '${SERVICE_GROUP}' (might not exist or user still belongs to it)."
        ;;
    *)
        echo "INFO: Skipping removal of user and group."
        echo "      To remove them manually later, run:"
        echo "      sudo userdel $SERVICE_USER"
        echo "      sudo groupdel $SERVICE_GROUP"
        ;;
esac

echo ""
echo "--- Uninstallation Complete ---"
echo ""

exit 0
