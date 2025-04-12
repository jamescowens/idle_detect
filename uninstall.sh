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

# --- Check for invoking user (needed for user service removal) ---
if [ -z "$SUDO_USER" ]; then
    echo "WARNING: \$SUDO_USER is not set. Skipping user-specific removal steps."
    echo "         Please run user service removal manually as the target user:"
    echo "         systemctl --user disable --now dc_idle_detection.service"
    echo "         rm -f ~/.config/systemd/user/dc_idle_detection.service ~/.config/idle_detect.conf"
    echo "         systemctl --user daemon-reload"
    TARGET_USER_HOME="" # Mark as unknown
else
    TARGET_USER_HOME=$(getent passwd "$SUDO_USER" | cut -d: -f6)
    if [ ! -d "$TARGET_USER_HOME" ]; then
      echo "ERROR: Could not determine home directory for user '$SUDO_USER'. Cannot perform user uninstall steps."
      exit 1
    fi
    USER_SERVICE_DIR="${TARGET_USER_HOME}/.config/systemd/user"
    USER_CONFIG_DIR="${TARGET_USER_HOME}/.config"
fi


echo "--- Starting Uninstallation ---"

# --- Disable and Stop Services ---
echo "INFO: Disabling and stopping system service 'dc_event_detection.service'..."
if systemctl is-active --quiet dc_event_detection.service; then
    systemctl disable --now dc_event_detection.service
else
    echo "INFO: System service not active."
    systemctl disable dc_event_detection.service || true # Try disabling even if not active
fi

if [ -n "$SUDO_USER" ] && [ -n "$TARGET_USER_HOME" ]; then
    echo "INFO: Disabling and stopping user service 'dc_idle_detection.service' for user '$SUDO_USER'..."
    # Run as the target user
    if sudo -u "$SUDO_USER" systemctl --user is-active --quiet dc_idle_detection.service; then
      sudo -u "$SUDO_USER" systemctl --user disable --now dc_idle_detection.service
    else
      echo "INFO: User service not active for $SUDO_USER."
      # Try disabling anyway
      sudo -u "$SUDO_USER" systemctl --user disable dc_idle_detection.service || true
    fi
else
    echo "WARN: Skipping user service disable/stop due to missing \$SUDO_USER."
fi

# --- Remove User Files ---
if [ -n "$SUDO_USER" ] && [ -n "$TARGET_USER_HOME" ]; then
    echo "INFO: Removing user service and config files for user '$SUDO_USER'..."
    rm -f "${USER_SERVICE_DIR}/dc_idle_detection.service"
    rm -f "${USER_CONFIG_DIR}/idle_detect.conf" # Maybe ask user if they want to keep config?
    # Attempt to remove directories if empty? Probably leave them.
else
    echo "WARN: Skipping user file removal due to missing \$SUDO_USER."
fi

# --- Run CMake Uninstall Script ---
# This removes files installed by 'cmake --install' based on install_manifest.txt
# Requires the build directory to exist and contain uninstall.cmake
UNINSTALL_SCRIPT="${BUILD_DIR}/uninstall.cmake" # BUILD_DIR needs to be set
if [ -f "$UNINSTALL_SCRIPT" ]; then
    echo "INFO: Running CMake uninstall script ($UNINSTALL_SCRIPT)..."
    # This now executes the manifest reading logic
    cmake -P "$UNINSTALL_SCRIPT"
else
    echo "WARNING: CMake uninstall script not found at $UNINSTALL_SCRIPT."
    echo "         System files installed by 'cmake --install' might need manual removal."
fi

# --- Remove Runtime Directory ---
# Systemd might remove this automatically if empty, but explicit remove is safer.
RUNTIME_DIR="/run/event_detect"
if [ -d "$RUNTIME_DIR" ]; then
    echo "INFO: Removing runtime directory $RUNTIME_DIR..."
    rm -rf "$RUNTIME_DIR"
fi

# --- User/Group Removal (Optional - uncomment with caution) ---
# echo "INFO: To remove user/group (use with caution):"
# echo "      sudo userdel $SERVICE_USER"
# echo "      sudo groupdel $SERVICE_GROUP"


# --- Systemd Reload ---
echo "INFO: Reloading systemd manager configuration..."
systemctl daemon-reload
if [ -n "$SUDO_USER" ] && [ -n "$TARGET_USER_HOME" ]; then
    echo "INFO: Reloading systemd user manager configuration for user '$SUDO_USER'..."
    sudo -u "$SUDO_USER" systemctl --user daemon-reload
else
    echo "WARN: Skipping user daemon-reload due to missing \$SUDO_USER."
fi

echo ""
echo "--- Uninstallation Complete ---"
echo "User/group '$SERVICE_USER'/'$SERVICE_GROUP' were NOT removed automatically."
echo ""

exit 0
