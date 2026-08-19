#!/bin/sh
# boinc_selinux_shmem_policy.sh -- let BOINC read idle_detect's shared memory
# segment on SELinux systems.
#
# Run as root:   sudo boinc_selinux_shmem_policy.sh
# To undo:       sudo boinc_selinux_shmem_policy.sh --remove
#
# WHY THIS IS NEEDED
#
# BOINC 8.2.10 and later read /dev/shm/idle_detect_shmem to get the last-active
# timestamp from event_detect, which is far more reliable than BOINC's own
# legacy idle detection (it works on Wayland, where the legacy X11 path cannot).
#
# BOINC runs confined as boinc_t. The segment is created by event_detect and,
# because event_detect runs as unconfined_service_t, the file inherits the
# generic tmpfs_t type. Stock SELinux policy grants boinc_t only 'getattr' on
# file_type, so BOINC can stat the segment but cannot open or read it:
#
#   avc: denied { read } for comm="boinc" name="idle_detect_shmem" dev="tmpfs"
#     scontext=system_u:system_r:boinc_t:s0
#     tcontext=system_u:object_r:tmpfs_t:s0 tclass=file permissive=0
#
# The denial is dontaudit-suppressed, so 'ausearch -m AVC' shows nothing and the
# only symptom is BOINC logging that it is using legacy idle detection and
# recommending that you install idle_detect -- which you already have. The file
# permissions (0644) are irrelevant and actively misleading here.
#
# SCOPE AND CAVEAT
#
# The rule below grants boinc_t read access to all tmpfs_t files, which is
# broader than ideal. A narrower fix needs a dedicated type for the segment
# (e.g. idle_detect_shmem_t) plus a type transition, which has to be coordinated
# between this project's packaging and the distribution's SELinux policy.
#
# This script is therefore a WORKAROUND, expected to become unnecessary once
# BOINC and/or the distributions ship policy covering this access. Review it
# before running it, and skip it if your security posture does not permit the
# rule above.
#
# It does nothing on systems without SELinux.

set -u

MODULE_NAME="boinc_idle_detect"
MODULE_VERSION="1.0"

if [ "$(id -u)" -ne 0 ]; then
    echo "$0: must be run as root (try: sudo $0)" >&2
    exit 1
fi

# Policy tools live in sbin on some distributions.
PATH="/sbin:/usr/sbin:$PATH"
export PATH

# --- Is SELinux even present? ----------------------------------------
if ! command -v getenforce >/dev/null 2>&1; then
    echo "SELinux tools not found; nothing to do on this system."
    exit 0
fi

SELINUX_STATE="$(getenforce 2>/dev/null || echo Disabled)"
if [ "$SELINUX_STATE" = "Disabled" ]; then
    echo "SELinux is disabled; nothing to do."
    exit 0
fi

# --- Removal ---------------------------------------------------------
if [ "${1:-}" = "--remove" ] || [ "${1:-}" = "-r" ]; then
    if semodule -l 2>/dev/null | grep -qx "$MODULE_NAME"; then
        if semodule -r "$MODULE_NAME"; then
            echo "Removed the '$MODULE_NAME' SELinux module."
            echo "Restart BOINC for the change to take effect:"
            echo "  systemctl restart boinc-client"
            exit 0
        fi
        echo "$0: failed to remove the '$MODULE_NAME' module." >&2
        exit 1
    fi
    echo "The '$MODULE_NAME' module is not installed; nothing to remove."
    exit 0
fi

# --- Already installed? ----------------------------------------------
if semodule -l 2>/dev/null | grep -qx "$MODULE_NAME"; then
    echo "The '$MODULE_NAME' SELinux module is already installed; nothing to do."
    echo "To remove it: $0 --remove"
    exit 0
fi

# --- Tools -----------------------------------------------------------
MISSING=""
for tool in checkmodule semodule_package semodule; do
    command -v "$tool" >/dev/null 2>&1 || MISSING="$MISSING $tool"
done

if [ -n "$MISSING" ]; then
    echo "$0: missing required SELinux tools:$MISSING" >&2
    echo "  openSUSE:      zypper install checkpolicy policycoreutils" >&2
    echo "  Fedora/RHEL:   dnf install checkpolicy policycoreutils" >&2
    echo "  Debian/Ubuntu: apt install checkpolicy policycoreutils" >&2
    exit 1
fi

# --- Build and install -----------------------------------------------
WORK_DIR="$(mktemp -d)" || { echo "$0: mktemp failed." >&2; exit 1; }
trap 'rm -rf "$WORK_DIR"' EXIT INT TERM

cat > "${WORK_DIR}/${MODULE_NAME}.te" <<EOF
module ${MODULE_NAME} ${MODULE_VERSION};

require {
    type boinc_t;
    type tmpfs_t;
    class file { open read };
}

# Allow the BOINC client to read idle_detect's POSIX shared memory segment
# (/dev/shm/idle_detect_shmem), which event_detect creates as tmpfs_t.
allow boinc_t tmpfs_t:file { open read };
EOF

echo "Building the '$MODULE_NAME' SELinux policy module..."
if ! checkmodule -M -m -o "${WORK_DIR}/${MODULE_NAME}.mod" \
                 "${WORK_DIR}/${MODULE_NAME}.te"; then
    echo "$0: checkmodule failed." >&2
    echo "  If it reported an unknown type, this policy may not define boinc_t," >&2
    echo "  which means the BOINC client is not confined here and no rule is needed." >&2
    exit 1
fi

if ! semodule_package -o "${WORK_DIR}/${MODULE_NAME}.pp" \
                      -m "${WORK_DIR}/${MODULE_NAME}.mod"; then
    echo "$0: semodule_package failed." >&2
    exit 1
fi

if ! semodule -i "${WORK_DIR}/${MODULE_NAME}.pp"; then
    echo "$0: semodule -i failed." >&2
    exit 1
fi

echo "Installed the '$MODULE_NAME' SELinux module (SELinux is $SELINUX_STATE)."
echo ""
echo "Restart BOINC so it picks up the segment:"
echo "  systemctl restart boinc-client"
echo ""
echo "To confirm it worked, this should print one line:"
echo "  sudo grep idle_detect_shmem /proc/\$(pgrep -x boinc)/maps"
echo ""
echo "BOINC should also stop logging that it is using legacy idle detection."
echo "To undo this change: $0 --remove"

exit 0
