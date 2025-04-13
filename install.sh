#!/bin/bash

# Installation script for event_detect and idle_detect services
# Usage: sudo ./install.sh [--prefix=PREFIX] [--cxx-compiler=COMPILER_PATH]
#   PREFIX defaults to /usr/local
#   COMPILER_PATH defaults to CMake's auto-detection

# Exit immediately if a command exits with a non-zero status.
set -e

# --- Configuration & Argument Parsing ---
INSTALL_PREFIX="/usr/local" # Default prefix for manual install
USER_CXX_COMPILER=""      # Default: let CMake find/use its default C++ compiler

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

BUILD_DIR="build/cmake" # Relative to project root where script is run
SYSTEM_CONFIG_DIR="/etc"
# User directories handled by user_install.sh
SERVICE_USER="event_detect"
SERVICE_GROUP="event_detect"

# --- Construct CMake Configure Arguments ---
# Arguments to pass to the configuration step
CMAKE_CONFIGURE_ARGS="-DCMAKE_INSTALL_PREFIX=${INSTALL_PREFIX}"
if [ -n "$USER_CXX_COMPILER" ]; then
    if [ ! -x "$USER_CXX_COMPILER" ]; then
        echo "ERROR: Specified C++ compiler not found or not executable: ${USER_CXX_COMPILER}"
        exit 1
    fi
    echo "INFO: Using specified C++ compiler: ${USER_CXX_COMPILER}"
    CMAKE_CONFIGURE_ARGS="${CMAKE_CONFIGURE_ARGS} -DCMAKE_CXX_COMPILER=${USER_CXX_COMPILER}"
else
    echo "INFO: Using default C++ compiler found by CMake (ensure it's C++17 compliant!)."
fi
# Removed C compiler flag

# --- Check for sudo ---
if [ "$(id -u)" -ne 0 ]; then
  echo "ERROR: Please run this script with sudo."
  exit 1
fi

# --- Check for invoking user (still needed for user/group creation ownership checks if refined later) ---
if [ -z "$SUDO_USER" ]; then
    # This case might occur if run directly as root, not via sudo from a user session.
    # For user/group creation, this is okay, but user service steps were removed anyway.
    echo "WARNING: \$SUDO_USER is not set (maybe run directly as root?)."
    # We don't strictly need SUDO_USER anymore in *this* script after removing user steps.
fi
# TARGET_USER_HOME determination not needed in this script anymore.

echo "--- Starting System-Level Installation ---"
echo "INFO: Install prefix set to: ${INSTALL_PREFIX}"

# --- Remind about Dependencies ---
echo "INFO: Ensuring necessary build tools (cmake, make/ninja, C++/C compiler) and library dependencies are installed..."
echo "      (See README.md for dependency package names for your distribution)."

# --- Build Step ---
echo "INFO: Ensuring clean build directory: $BUILD_DIR"
rm -rf "$BUILD_DIR" # Clean first! (Run as root via sudo)
mkdir -p "$BUILD_DIR" # Create fresh

echo "INFO: Configuring project with CMake..."
echo "INFO: Running CMake: cmake -S . -B ${BUILD_DIR} ${CMAKE_CONFIGURE_ARGS}"

# Execute cmake configure command from project root, specifying Source and Build dirs
# Run directly within the sudo context
if ! cmake -S . -B "$BUILD_DIR" ${CMAKE_CONFIGURE_ARGS} ; then
    echo "ERROR: CMake configuration command failed with exit code $?."
    exit 1
fi

# Explicitly check if essential build files were generated inside BUILD_DIR
if [ ! -f "${BUILD_DIR}/Makefile" ] && [ ! -f "${BUILD_DIR}/build.ninja" ]; then
    echo "ERROR: CMake configuration finished but no Makefile or build.ninja found in ${BUILD_DIR}!"
    echo "       Check the full CMake output above for potential errors or warnings."
    ls -la "$BUILD_DIR" # Show directory contents
    exit 1
else
     echo "INFO: CMake configuration successful (build files found)."
fi

echo "INFO: Building project in ${BUILD_DIR}..."
# Build using the specified build directory, no need to cd
if cmake --build "$BUILD_DIR" -j$(nproc) ; then
    echo "INFO: Build successful."
else
    BUILD_EC=$?
    echo "ERROR: Build failed with exit code $BUILD_EC."
    exit 1
fi
# No need to cd back

# --- Install System Files ---
echo "INFO: Installing system files (binaries, system service, system config) to prefix '${INSTALL_PREFIX}'..."
# Install FROM the build directory using the prefix set during configure
if cmake --install "$BUILD_DIR" ; then
   echo "INFO: System files installed successfully."
else
   echo "ERROR: System file installation failed."
   exit 1
fi

# --- Systemd Reload (System) ---
echo "INFO: Reloading systemd manager configuration..."
systemctl daemon-reload

# --- User/Group Management ---
# (Keep this section unchanged from your version)
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
usermod -aG input "$SERVICE_USER" || echo "WARN: Failed or not needed to add $SERVICE_USER to input group."
usermod -aG tty "$SERVICE_USER" || echo "WARN: Failed or not needed to add $SERVICE_USER to tty group."

# --- Enable and Start System Service ---
# (Keep this section unchanged from your version)
echo "INFO: Enabling and starting system service 'dc_event_detection.service'..."
systemctl enable --now dc_event_detection.service

# --- Final Instructions ---
# (Keep this section unchanged from your version)
echo ""
echo "--- System-Level Installation Complete ---"
echo "*** IMPORTANT: Now run './user_install.sh' as the regular user ***"
echo "    (e.g., exit sudo session, then run './user_install.sh')"
echo ""
echo "You may want to customize configuration files:"
echo "  System config: sudo nano ${SYSTEM_CONFIG_DIR}/event_detect.conf"
echo "  Helper scripts (installed in ${INSTALL_PREFIX}/bin): dc_pause, dc_unpause"
echo ""

exit 0
