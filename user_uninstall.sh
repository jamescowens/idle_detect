#!/bin/bash

# User-level uninstallation script for idle_detect service
# Run this script AS YOUR REGULAR USER before running 'sudo ./uninstall.sh'.

# --- Configuration ---
USER_CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
USER_CONFIG_DEST="${USER_CONFIG_HOME}/idle_detect.conf"
SERVICE_NAME="dc_idle_detection.service"
# --- End Configuration ---

echo "--- Starting User-Level Uninstallation ---"

# --- Disable and Stop User Service ---
echo "INFO: Disabling and stopping user service '$SERVICE_NAME'..."
# Use || true to ignore errors if service doesn't exist or isn't active/enabled
systemctl --user disable --now "$SERVICE_NAME" || true

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

# --- Retired Wrapper Script ---
# idle_detect_wrapper.sh was installed by releases up to 0.9.1.0 and is no longer
# used: the user service ExecStart runs the binary directly. It lives in the
# install prefix's bin directory, which is normally root-owned, so this script
# usually can only report it -- 'sudo ./uninstall.sh' is what removes it there.
# A prefix inside the user's home is the case this can actually clean up, and it
# is also the case sudo would have trouble with.
#
# Only this exact filename is considered, and a removal that fails is reported
# rather than treated as an error, since the file is inert either way.
for wrapper_bin_dir in "$HOME/.local/bin" "/usr/local/bin" "/usr/bin"; do
    STALE_WRAPPER="${wrapper_bin_dir}/idle_detect_wrapper.sh"

    if [ ! -e "$STALE_WRAPPER" ]; then
        continue
    fi

    if rm -f "$STALE_WRAPPER" 2>/dev/null; then
        echo "INFO: Removed retired wrapper script: $STALE_WRAPPER"
    else
        echo "INFO: A retired wrapper script remains at $STALE_WRAPPER."
        echo "      It is unused; 'sudo ./uninstall.sh' or a reinstall removes it."
    fi
done

# --- Systemd User Daemon Reload ---
echo "INFO: Reloading systemd user instance..."
systemctl --user daemon-reload

echo ""
echo "--- User-Level Uninstallation Complete ---"
echo "User service stopped/disabled and files removed (config removal based on prompt)."
echo "Now run 'sudo ./uninstall.sh' to remove system components."
echo ""

exit 0
