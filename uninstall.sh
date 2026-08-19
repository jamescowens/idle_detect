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

# --- Remove the Retired Wrapper Script ---
# The CMake uninstall script above removes it when it exists, using the exact bin
# directory it was configured with. This is the backstop for the case that script
# does not exist -- a build directory that was cleaned or never kept -- since the
# wrapper is precisely the file that survives when nothing else does: it is absent
# from any manifest a current build produces.
#
# The prefix is read from the build directory's CMake cache when there is one, and
# otherwise defaults to the prefix install.sh uses. Only this exact filename is
# removed, and a failure is a warning: an inert leftover must not fail an
# uninstall.
WRAPPER_PREFIX=""
if [ -f "${BUILD_DIR}/CMakeCache.txt" ]; then
    WRAPPER_PREFIX=$(grep -m1 '^CMAKE_INSTALL_PREFIX:PATH=' "${BUILD_DIR}/CMakeCache.txt" | cut -d= -f2- || true)
fi
if [ -z "$WRAPPER_PREFIX" ]; then
    WRAPPER_PREFIX="/usr/local"
fi

STALE_WRAPPER="${WRAPPER_PREFIX}/bin/idle_detect_wrapper.sh"
if [ -e "$STALE_WRAPPER" ]; then
    echo "INFO: Found retired wrapper script: ${STALE_WRAPPER}"
    if rm -f "$STALE_WRAPPER"; then
        echo "INFO: Removed ${STALE_WRAPPER}."
    else
        echo "WARN: Could not remove ${STALE_WRAPPER}. It is unused; remove it manually."
    fi
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
