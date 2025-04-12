#!/bin/bash

# Installation script for event_detect and idle_detect services
# Run this script with sudo: sudo ./install.sh

# Exit immediately if a command exits with a non-zero status.
set -e

INSTALL_PREFIX="/usr/local"

# Allow overriding via --prefix= argument
for arg in "$@"; do
  case $arg in
    --prefix=*)
    INSTALL_PREFIX="${arg#*=}"
    shift # Remove --prefix=... from processing
    ;;
  esac
done

# --- Configuration ---
BUILD_DIR="build/cmake"
SYSTEM_CONFIG_DIR="/etc"
USER_SERVICE_DIR_TEMPLATE=".config/systemd/user" # Relative to user home
USER_CONFIG_DIR_TEMPLATE=".config"             # Relative to user home
SERVICE_USER="event_detect"
SERVICE_GROUP="event_detect"
# Explicitly set compilers if needed (otherwise CMake might pick defaults)
# Adjust these paths if necessary!
CXX_COMPILER="/usr/bin/g++-14"
C_COMPILER="/usr/bin/gcc-14"
CMAKE_CMD="cmake ../../ -DCMAKE_INSTALL_PREFIX=${INSTALL_PREFIX} -DCMAKE_CXX_COMPILER=${CXX_COMPILER} -DCMAKE_C_COMPILER=${C_COMPILER}"
# --- End Configuration ---

# --- Check for sudo ---
if [ "$(id -u)" -ne 0 ]; then
  echo "ERROR: Please run this script with sudo."
  exit 1
fi

# --- Check for invoking user (needed for user service install) ---
if [ -z "$SUDO_USER" ]; then
    echo "ERROR: \$SUDO_USER is not set. Please run using 'sudo -E ./install.sh' or ensure SUDO_USER is passed."
    echo "       Cannot determine target user for user service installation."
    # Or attempt to get user from logname/who? More complex.
    exit 1
fi
TARGET_USER_HOME=$(getent passwd "$SUDO_USER" | cut -d: -f6)
if [ ! -d "$TARGET_USER_HOME" ]; then
    echo "ERROR: Could not determine home directory for user '$SUDO_USER'."
    exit 1
fi
USER_SERVICE_DIR="${TARGET_USER_HOME}/${USER_SERVICE_DIR_TEMPLATE}"
USER_CONFIG_DIR="${TARGET_USER_HOME}/${USER_CONFIG_DIR_TEMPLATE}"

echo "--- Starting Installation for user $SUDO_USER ---"

# --- Remind about Dependencies ---
echo "INFO: Ensuring necessary build tools (cmake, make/ninja, C++/C compiler) and library dependencies are installed..."
echo "      (See README.md for dependency package names for your distribution)."
# Optionally add package install commands here later, commented out or conditional

# --- Build Step ---
echo "INFO: Configuring project with CMake..."
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
if $CMAKE_CMD ; then
    echo "INFO: CMake configuration successful."
else
    echo "ERROR: CMake configuration failed."
    exit 1
fi

echo "INFO: Building project..."
# Use number of processors for parallel build, requires 'nproc' command
if cmake --build . -j$(nproc) ; then
    echo "INFO: Build successful."
else
    echo "ERROR: Build failed."
    exit 1
fi
cd ../.. # Go back to project root

# --- Install System Files ---
echo "INFO: Installing system files (binaries, system service, system config)..."
# This runs 'install(TARGETS...)' and 'install(FILES...)' from CMakeLists.txt
# Install step uses the prefix configured during the cmake step implicitly
if cmake --install "$BUILD_DIR" ; then
    echo "INFO: System files installed successfully."
else
    echo "ERROR: System file installation failed (check permissions or CMakeLists.txt install rules)."
    exit 1
fi

# --- Systemd Reload (System) ---
echo "INFO: Reloading systemd manager configuration..."
systemctl daemon-reload

# --- User/Group Management ---
echo "INFO: Setting up user '$SERVICE_USER' and group '$SERVICE_GROUP'..."
if ! getent group "$SERVICE_GROUP" > /dev/null 2>&1; then
    echo "INFO: Creating system group '$SERVICE_GROUP'."
    groupadd --system "$SERVICE_GROUP"
else
    echo "INFO: System group '$SERVICE_GROUP' already exists."
fi

if ! id -u "$SERVICE_USER" > /dev/null 2>&1; then
    echo "INFO: Creating system user '$SERVICE_USER'."
    useradd --system -g "$SERVICE_GROUP" -d / -s /sbin/nologin "$SERVICE_USER"
else
    echo "INFO: System user '$SERVICE_USER' already exists."
fi

echo "INFO: Adding user '$SERVICE_USER' to 'input' and 'tty' groups..."
# Add checks if needed, usermod might print warnings if already a member
usermod -aG input "$SERVICE_USER" || echo "WARN: Failed or not needed to add $SERVICE_USER to input group."
usermod -aG tty "$SERVICE_USER" || echo "WARN: Failed or not needed to add $SERVICE_USER to tty group."

# --- Install User Files ---
echo "INFO: Installing user configuration and service files for user '$SUDO_USER'..."
# Create target directories if they don't exist, owned by the user
echo "INFO: Creating user directories (if needed)..."
mkdir -p "$USER_SERVICE_DIR"
chown "$SUDO_USER":"$(id -gn $SUDO_USER)" "$USER_CONFIG_DIR" || true # Allow failure if dir exists with diff owner?
chown "$SUDO_USER":"$(id -gn $SUDO_USER)" "$USER_CONFIG_DIR/systemd" || true
chown "$SUDO_USER":"$(id -gn $SUDO_USER)" "$USER_SERVICE_DIR" || true

echo "INFO: Copying user files..."
# Copy user service file
cp ./dc_idle_detection.service "${USER_SERVICE_DIR}/dc_idle_detection.service"
chown "$SUDO_USER":"$(id -gn $SUDO_USER)" "${USER_SERVICE_DIR}/dc_idle_detection.service"
# Copy default user config file (don't overwrite if exists?)
if [ ! -f "${USER_CONFIG_DIR}/idle_detect.conf" ]; then
    cp ./idle_detect.conf "${USER_CONFIG_DIR}/idle_detect.conf"
    chown "$SUDO_USER":"$(id -gn $SUDO_USER)" "${USER_CONFIG_DIR}/idle_detect.conf"
    echo "INFO: Copied default config to ${USER_CONFIG_DIR}/idle_detect.conf"
else
    echo "INFO: User config ${USER_CONFIG_DIR}/idle_detect.conf already exists, not overwriting."
fi

# --- Systemd Reload (User) ---
echo "INFO: Reloading systemd user manager configuration for user '$SUDO_USER'..."
# Run as the target user
sudo -u "$SUDO_USER" systemctl --user daemon-reload

# --- Enable and Start Services ---
echo "INFO: Enabling and starting system service 'dc_event_detection.service'..."
systemctl enable --now dc_event_detection.service

echo "INFO: Enabling and starting user service 'dc_idle_detection.service' for user '$SUDO_USER'..."
# Run as the target user
sudo -u "$SUDO_USER" systemctl --user enable --now dc_idle_detection.service

# --- Final Instructions ---
echo ""
echo "--- Installation Complete ---"
echo "Please check the status of the services:"
echo "  sudo systemctl status dc_event_detection.service"
echo "  systemctl --user status dc_idle_detection.service (run as user '$SUDO_USER')"
echo ""
echo "You may want to customize configuration files:"
echo "  System config: sudo nano ${SYSTEM_CONFIG_DIR}/event_detect.conf"
echo "  User config:   nano ${USER_CONFIG_DIR}/idle_detect.conf (run as user '$SUDO_USER')"
echo "  Helper scripts (installed in ${INSTALL_PREFIX}/bin): dc_pause, dc_unpause"
echo ""
echo "Remember to restart services after changing config files:"
echo "  sudo systemctl restart dc_event_detection.service"
echo "  systemctl --user restart dc_idle_detection.service (run as user '$SUDO_USER')"
echo ""

exit 0
