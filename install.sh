#!/bin/bash

# Installation script for event_detect and idle_detect services
# Usage: sudo ./install.sh [--prefix=PREFIX] [--cxx-compiler=COMPILER_PATH]
#   PREFIX defaults to /usr/local
#   COMPILER_PATH defaults to CMake's auto-detection

# Exit immediately if a command exits with a non-zero status.
set -e

# --- Configuration & Argument Parsing ---
INSTALL_PREFIX="/usr/local"
USER_CXX_COMPILER="" # Default: let CMake find/use its default C++ compiler

# Parse arguments (--prefix and --cxx-compiler)
for arg in "$@"; do
  case $arg in
    --prefix=*)
    INSTALL_PREFIX="${arg#*=}"
    shift # Remove from list
    ;;
    --cxx-compiler=*)
    USER_CXX_COMPILER="${arg#*=}"
    shift # Remove from list
    ;;
    *)
    # Handle other arguments or ignore them
    ;;
  esac
done

BUILD_DIR="build/cmake"
SYSTEM_CONFIG_DIR="/etc"
USER_SERVICE_DIR_TEMPLATE=".config/systemd/user" # Relative to user home
USER_CONFIG_DIR_TEMPLATE=".config"               # Relative to user home
SERVICE_USER="event_detect"
SERVICE_GROUP="event_detect"

# --- Construct CMake Command ---
# Base command with install prefix
CMAKE_BASE_CMD="cmake ../../ -DCMAKE_INSTALL_PREFIX=${INSTALL_PREFIX_ARG}"
# Add C++ compiler flag ONLY if user specified one
CMAKE_CONFIGURE_FLAGS=""
if [ -n "$USER_CXX_COMPILER" ]; then
    if [ ! -x "$USER_CXX_COMPILER" ]; then
        echo "ERROR: Specified C++ compiler not found or not executable: ${USER_CXX_COMPILER}"
        exit 1
    fi
    echo "INFO: Using specified C++ compiler: ${USER_CXX_COMPILER}"
    CMAKE_CONFIGURE_FLAGS="-DCMAKE_CXX_COMPILER=${USER_CXX_COMPILER}"
else
    echo "INFO: Using default C++ compiler found by CMake (ensure it's C++17 compliant!)."
fi

CMAKE_FULL_CMD="${CMAKE_BASE_CMD} ${CMAKE_CONFIGURE_FLAGS}"

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

# --- Enable and Start Services ---
echo "INFO: Enabling and starting system service 'dc_event_detection.service'..."
systemctl enable --now dc_event_detection.service

# --- Final Instructions ---
echo ""
echo "--- System-Level Installation Complete ---"
echo "*** IMPORTANT: Now run './user_install.sh' as the regular user ***"
echo "    (e.g., exit sudo session, then run './user_install.sh')"
echo ""
echo "You may want to customize configuration files:"
echo "  System config: sudo nano ${SYSTEM_CONFIG_DIR}/event_detect.conf"
echo "  Helper scripts (installed in ${INSTALL_PREFIX_ARG}/bin): dc_pause, dc_unpause"
echo ""

exit 0
