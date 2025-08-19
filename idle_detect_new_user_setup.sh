#!/bin/sh

# This script is run via XDG Autostart on user login to ensure
# the idle_detect user service is enabled and running.

SERVICE_NAME="dc_idle_detection.service"

# Check if the service is already active to avoid unnecessary work on every login
if ! systemctl --user is-active --quiet "$SERVICE_NAME"; then
    # If not active, run enable --now. This will start it for the current session
    # and ensure it starts on subsequent logins. It's safe to run multiple times.
    echo "First time setup or service not running: Enabling and starting $SERVICE_NAME for user." >&2
    systemctl --user enable --now "$SERVICE_NAME"
fi

exit 0
