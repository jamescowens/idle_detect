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

# --- Check for invoking user ---
# SUDO_USER is needed to run the build step as the non-root user, avoiding
# root-owned files in the build directory.
if [ -z "$SUDO_USER" ]; then
    echo "ERROR: \$SUDO_USER is not set. Please run this script via sudo, not directly as root."
    echo "       Example: sudo ./install.sh"
    exit 1
fi

echo "--- Starting System-Level Installation ---"
echo "INFO: Install prefix set to: ${INSTALL_PREFIX}"

# --- Remind about Dependencies ---
echo "INFO: Ensuring necessary build tools (cmake, make/ninja, C++/C compiler) and library dependencies are installed..."
echo "      (See README.md for dependency package names for your distribution)."

# --- Build Step ---
# Run cmake configure and build as the invoking user (not root) to avoid
# leaving root-owned files in the build directory that break subsequent
# non-root builds.
echo "INFO: Ensuring clean build directory: $BUILD_DIR"
rm -rf "$BUILD_DIR"
sudo -u "$SUDO_USER" mkdir -p "$BUILD_DIR"

echo "INFO: Configuring project with CMake..."
echo "INFO: Running CMake: cmake -S . -B ${BUILD_DIR} ${CMAKE_CONFIGURE_ARGS}"

# Execute cmake configure command as the invoking user
if ! sudo -u "$SUDO_USER" cmake -S . -B "$BUILD_DIR" ${CMAKE_CONFIGURE_ARGS} ; then
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
# Build as the invoking user
if sudo -u "$SUDO_USER" cmake --build "$BUILD_DIR" -j$(nproc) ; then
    echo "INFO: Build successful."
else
    BUILD_EC=$?
    echo "ERROR: Build failed with exit code $BUILD_EC."
    exit 1
fi

# --- Install System Files ---
echo "INFO: Installing system files (binaries, system service, system config) to prefix '${INSTALL_PREFIX}'..."

# 'cmake --install' rewrites ${SYSTEM_CONFIG_DIR}/event_detect.conf unconditionally,
# which silently discards local settings on an upgrade -- monitor_ttys, for one, which
# an admin may deliberately have turned off. The Debian package marks that file as a
# conffile so dpkg preserves it; do the same here. Keep the admin's file and leave the
# new default beside it as .new.
SYSTEM_CONFIG_FILE="${SYSTEM_CONFIG_DIR}/event_detect.conf"
PRESERVED_SYSTEM_CONFIG=""
if [ -f "$SYSTEM_CONFIG_FILE" ]; then
    PRESERVED_SYSTEM_CONFIG="$(mktemp)"
    cp -a "$SYSTEM_CONFIG_FILE" "$PRESERVED_SYSTEM_CONFIG"
fi

# Install FROM the build directory using the prefix set during configure
if cmake --install "$BUILD_DIR" ; then
   echo "INFO: System files installed successfully."
else
   echo "ERROR: System file installation failed."
   [ -n "$PRESERVED_SYSTEM_CONFIG" ] && rm -f "$PRESERVED_SYSTEM_CONFIG"
   exit 1
fi

if [ -n "$PRESERVED_SYSTEM_CONFIG" ]; then
    if cmp -s "$PRESERVED_SYSTEM_CONFIG" "$SYSTEM_CONFIG_FILE"; then
        rm -f "$PRESERVED_SYSTEM_CONFIG"
    else
        cp -a "$SYSTEM_CONFIG_FILE" "${SYSTEM_CONFIG_FILE}.new"
        cp -a "$PRESERVED_SYSTEM_CONFIG" "$SYSTEM_CONFIG_FILE"
        rm -f "$PRESERVED_SYSTEM_CONFIG"
        echo "INFO: Kept the existing ${SYSTEM_CONFIG_FILE} rather than overwriting it."
        echo "INFO: The new default was written to ${SYSTEM_CONFIG_FILE}.new; diff the two"
        echo "      if you want to pick up newly added settings."
    fi
fi

# --- Remove the Retired Wrapper Script ---
# idle_detect_wrapper.sh was installed into the bin directory by releases up to
# 0.9.1.0. It is no longer built or installed -- the user service ExecStart runs
# the binary directly -- but nothing removes the copy an earlier release left
# behind, because 'cmake --install' only ever writes files and its manifest lists
# only what the current build produces. Left alone it lingers forever, and an
# operator reading the bin directory reasonably concludes it is still live.
#
# Deliberately narrow: only this exact filename is touched, the removal is
# reported, and a failure is a warning rather than an error. The file is inert
# either way, so nothing about the installation depends on it being gone and an
# install must not fail over it.
#
# ${INSTALL_PREFIX}/bin matches where CMake put it, since the project installs
# binaries to ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR} and this script does
# not override CMAKE_INSTALL_BINDIR from its default of "bin".
STALE_WRAPPER="${INSTALL_PREFIX}/bin/idle_detect_wrapper.sh"
if [ -e "$STALE_WRAPPER" ]; then
    echo "INFO: Found retired wrapper script from a previous installation: ${STALE_WRAPPER}"
    if rm -f "$STALE_WRAPPER"; then
        echo "INFO: Removed ${STALE_WRAPPER} (no longer used; ExecStart runs the binary directly)."
    else
        echo "WARN: Could not remove ${STALE_WRAPPER}. It is unused and inert; remove it manually."
    fi
fi

# --- Systemd Reload (System) ---
echo "INFO: Reloading systemd manager configuration..."
systemctl daemon-reload

# --- Shadowing Unit Detection ---
# systemd resolves units by precedence, and /etc/systemd/system outranks both
# /usr/local/lib/systemd/system and /usr/lib/systemd/system. A unit file left in /etc by an
# older release therefore keeps winning after an upgrade: the service still starts, still points
# at a valid binary, and silently runs the OLD unit configuration. That is worse than failing,
# because nothing looks wrong. Note this is NOT the same as the drop-in case handled further
# down -- a drop-in overrides directives, whereas this replaces the whole unit, and
# 'systemctl cat' shows the winning file without indicating that another was passed over.
INSTALLED_SYSTEM_UNIT="${INSTALL_PREFIX}/lib/systemd/system/dc_event_detection.service"
LOADED_SYSTEM_UNIT="$(systemctl show dc_event_detection.service -p FragmentPath --value 2>/dev/null)"

if [ -n "$LOADED_SYSTEM_UNIT" ] && [ "$LOADED_SYSTEM_UNIT" != "$INSTALLED_SYSTEM_UNIT" ] \
   && [ -f "$LOADED_SYSTEM_UNIT" ]; then
    echo "WARN: systemd is loading '${LOADED_SYSTEM_UNIT}', not the unit just installed at"
    echo "WARN: '${INSTALLED_SYSTEM_UNIT}'. The installed unit is being shadowed."

    # Only remove a file we can positively identify as one this project generated. The markers
    # are the Description line and an ExecStart naming our binary. Anything else -- an
    # admin-authored unit, or one carrying customizations -- is reported and left alone, because
    # deleting an unrecognized file under /etc during an install is not a call this script gets
    # to make.
    if grep -qs '^Description=DC Event Detection Service' "$LOADED_SYSTEM_UNIT" \
       && grep -qsE '^ExecStart=.*/event_detect( |$)' "$LOADED_SYSTEM_UNIT"; then
        SHADOW_BACKUP="${LOADED_SYSTEM_UNIT}.superseded-$(date +%Y%m%d%H%M%S)"

        if cp -a "$LOADED_SYSTEM_UNIT" "$SHADOW_BACKUP" && rm -f "$LOADED_SYSTEM_UNIT"; then
            echo "INFO: It is a unit generated by a previous release of this project."
            echo "INFO: Backed it up to '${SHADOW_BACKUP}' and removed it so the newly installed"
            echo "INFO: unit takes effect. Delete the backup once you are satisfied."
            systemctl daemon-reload
        else
            echo "WARN: Could not move it aside. Remove it manually so the new unit is used:"
            echo "WARN:   sudo rm '${LOADED_SYSTEM_UNIT}' && sudo systemctl daemon-reload"
        fi
    else
        echo "WARN: It does not look like a unit this project generated, so it has been left in"
        echo "WARN: place. If it is stale, remove it manually so the new unit takes effect:"
        echo "WARN:   sudo rm '${LOADED_SYSTEM_UNIT}' && sudo systemctl daemon-reload"
    fi
fi

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
echo "INFO: Enabling and starting system service 'dc_event_detection.service'..."

# A drop-in under /etc lives outside any package's file list, so it survives an uninstall and then
# silently pins ExecStart at a path this install may no longer provide. 'systemctl cat' shows the
# unit body and gives no hint that a drop-in has overridden it; only 'systemctl show -p DropInPaths'
# reveals them. Check explicitly, because the alternative is the service failing later with
# "Unable to locate executable" and no obvious cause.
DROP_IN_PATHS="$(systemctl show dc_event_detection.service -p DropInPaths --value 2>/dev/null)"
if [ -n "$DROP_IN_PATHS" ]; then
    for drop_in in $DROP_IN_PATHS; do
        [ -r "$drop_in" ] || continue
        grep -qs '^ExecStart=.' "$drop_in" || continue

        pinned_exec="$(grep -hs '^ExecStart=.' "$drop_in" | tail -n 1 | sed 's/^ExecStart=//' | awk '{print $1}')"
        if [ -n "$pinned_exec" ] && [ ! -x "$pinned_exec" ]; then
            echo "WARN: Drop-in '${drop_in}' pins ExecStart to '${pinned_exec}', which is not executable."
            echo "WARN: This install placed the binary at '${INSTALL_PREFIX}/bin/event_detect'."
            echo "WARN: The service will fail to start until that drop-in is updated or removed."
        elif [ -n "$pinned_exec" ]; then
            echo "INFO: Drop-in '${drop_in}' pins ExecStart to '${pinned_exec}'."
        fi
    done
fi

# The unit sets StartLimitBurst, so a previous failed run can leave the unit latched in 'failed'
# with further start requests refused ("Start request repeated too quickly"). Clear that first,
# or a correct install still will not start.
systemctl reset-failed dc_event_detection.service 2>/dev/null || true

# 'enable --now' does NOT restart a service that is already running. On an upgrade that leaves the
# previous binary executing against a now-deleted inode (/proc/<pid>/exe shows "(deleted)"), so the
# new build appears installed but is not the one running. Restart explicitly in that case.
if systemctl is-active --quiet dc_event_detection.service; then
    echo "INFO: Service is already running; restarting so the newly installed binary takes effect."
    systemctl enable dc_event_detection.service > /dev/null 2>&1 || true
    systemctl restart dc_event_detection.service
else
    systemctl enable --now dc_event_detection.service
fi

# --- Final Instructions ---
# (Keep this section unchanged from your version)
echo ""
echo "--- System-Level Installation Complete ---"
echo ""
echo "*** Optional: Now run './user_install.sh' as the regular user ***"
echo "    (e.g., exit sudo session, then run './user_install.sh')"
echo "    This copies the default user idle_detect.conf config file"
echo "    to ~/.config, marks the user service as active, and starts"
echo "    the user service."
echo ""
echo "You may want to customize configuration files:"
echo "  System config: sudo nano ${SYSTEM_CONFIG_DIR}/event_detect.conf"
echo "  Helper scripts (installed in ${INSTALL_PREFIX}/bin): dc_pause, dc_unpause, dc_fah_v8"

# SELinux blocks the BOINC client from reading event_detect's shared memory
# segment, and the denial is dontaudit-suppressed, so the only symptom is BOINC
# quietly using its legacy idle detection. Point the admin at the workaround.
if command -v getenforce >/dev/null 2>&1 && [ "$(getenforce 2>/dev/null)" != "Disabled" ]; then
    echo ""
    echo "  NOTE: SELinux is $(getenforce 2>/dev/null) on this system. BOINC cannot read"
    echo "        idle_detect's shared memory segment under stock policy, and the denial is"
    echo "        suppressed, so BOINC will silently fall back to legacy idle detection."
    echo "        If you run BOINC, review and then run:"
    echo "          sudo ${INSTALL_PREFIX}/bin/boinc_selinux_shmem_policy.sh"
fi
echo ""

exit 0
