#!/bin/bash

config_file="$1"

timestamp() {
  date --utc +"%s"
}

hr_timestamp() {
  date --utc +"%m/%d/%Y %H:%M:%S.%N"
}

# Source the config file
source_config() {
  current_mtime=$(stat -c %Y "$config_file")

  if [ "$previous_mtime" != "$current_mtime" ]; then
    source "$config_file"
    debug_log "source_config"
    previous_mtime="$current_mtime"
  fi
}

log() {
  string=$(hr_timestamp)

  for arg in "$@"; do
    string="$string $arg"
  done

  echo "$string"
}

debug_log() {
  if ((debug == 1)); then
    string=""

    for arg in "$@"; do
      string="$string $arg"
    done

    log "$string"
  fi
}

# Function to check for executable existence
check_executable() {
  local executable="$1"

  if command -v "$executable" &> /dev/null; then
    debug_log "Executable '$executable' found."
    return 0 # Success
  else
    log "ERROR: Executable '$executable' not found."
    return 1 # Failure
  fi
}


check_gui_session_type() {
  # Check the DISPLAY environment variable
  if [[ -n "$DISPLAY" ]]; then
    # DISPLAY is set, indicating a graphical session

    # Check for Wayland
    if [[ -n "$WAYLAND_DISPLAY" ]]; then
      echo "WAYLAND"
    elif [[ -n "$XDG_SESSION_TYPE" && "$XDG_SESSION_TYPE" == "wayland" ]]; then
      echo "WAYLAND_XDG"
    # Check for X11
    elif [[ -n "$WINDOWID" ]]; then
      echo "X11"
    elif [[ -n "$XAUTHORITY" ]]; then
      echo "X11_XAUTHORITY"
    else
      #If DISPLAY is set, but no other environment variable indicates wayland or X11, it is likely X11.
      echo "X11_LIKELY"
    fi

  else
    # DISPLAY is not set, indicating a text-based session
    echo "TTY"
  fi
}

# Get the ID of the mouse device(s). Initialize the sparse location arrays with zeroes for the valid mouse pointers.
find_mouse_ids() {
  display_type="$(check_gui_session_type)"

  debug_log "display_type =" "$display_type"

  if [[ "$display_type" -eq "X11" ]] || [[ "$display_type" -eq "X11_XAUTHORITY" ]] || [[ "$display_type" -eq "X11_LIKELY" ]]; then
    if check_executable "xinput"; then

      readarray -t pointer_array < <(xinput list | grep -i "slave.*pointer")

      pointer_array_size=${#pointer_array[@]}

      # This theoretically could fail if someone were to remove and then add a device within
      # one second so that the device count remains the same, but the mouse_id shifts; however,
      # this is highly improbable, and not worth the extra CPU expense to run this without
      # condition.
      if ((pointer_array_size != previous_pointer_array_size)); then
        mouse_found=0

        mouse_id_array=()
        old_x=()
        old_y=()
        current_x=()
        current_y=()

        for pointer in "${pointer_array[@]}"; do

          mouse_id=$(echo "$pointer" | sed 's/.*id=\([0-9]*\).*/\1/'  | head -n 1)

          if [[ ! -z "$mouse_id" ]]; then
            mouse_found=1

            debug_log "mouse_id =" $mouse_id

            # Note these are sparse arrays.
            mouse_id_array[${#mouse_id_array[@]}]=$mouse_id

            old_x[$mouse_id]=0
            old_y[$mouse_id]=0

            current_x[$mouse_id]=0
            current_y[$mouse_id]=0
          fi
        done

        previous_pointer_array_size=$pointer_array_size
      fi
    else
      # xinput missing for X session is a hard failure.
      return 2
    fi
  fi

  if ((mouse_found == 0)); then
    return 1
  else
    return 0
  fi
}

# Function to check idle time for all logged-in users
check_all_ttys_idle() {
  debug_log "check_all_ttys_idle"

  local status=0

  if check_executable "w"; then

    readarray -t idle_seconds_array < <(w -h | awk '{
      idle = $5;
      seconds = 0;

      if (idle ~ /^[0-9.]+s$/) {
        gsub(/s/,"",idle);
        seconds = idle;
      } else if (idle ~ /^[0-9]+:[0-9]+$/) {
        split(idle, a, ":");
        seconds = a[1] * 60 + a[2];
      } else if (idle ~ /^[0-9]+m$/) {
        gsub(/m/,"",idle);
        seconds = idle * 60;
      } else if (idle ~ /^[0-9]+days$/) {
        gsub(/days/,"",idle);
        seconds = idle * 24 * 60 * 60;
      } else if (idle ~ /[0-9]+:[0-9]+:[0-9]+/) {
        split(idle, a, ":");
        seconds = a[1] * 3600 + a[2] * 60 + a[3];
      } else if (idle ~ /[0-9]+:[0-9]+m$/) {
        gsub(/m/,"",idle);
        split(idle, a, ":");
        seconds = a[1] * 3600 + a[2] * 60 + a[3];
      } else if (idle ~ /[0-9]+:[0-9]+/ && idle !~ /:/){
        #In my particular case, when uptime was 1-23:59:00, for example,
        #the IDLE would print the hours and minutes without :
        seconds = idle * 3600; #assuming no : means only hours
      } else {
        # Handle other cases or print an error
        seconds = -1;  # Indicate an unhandled format
      }

      print seconds;
    }'
    )

    for idle_seconds in ""${idle_seconds_array[@]}""; do
      idle_seconds_rnd=$(echo "scale=0; $idle_seconds / 1" | bc)

      if [[ -z "$idle_seconds_rnd" ]] || [[ "$idle_seconds_rnd" -eq -1 ]]; then
        # User might have just logged out, or other edge case. Skip.
        log "WARNING: tty activity detection encountered an unknown time format or other unknown error."

        status=1

        continue
      fi

      # Note that this accumulates the active state as an or condition on each tty, which is also "ored"
      # from the active_state determined by the pointer check if enabled.
      if ((idle_seconds_rnd >= inactivity_time_trigger)); then
        active_state=$((0 | active_state))
      else
        active_state=$((1 | active_state))
      fi
    done
  else
    status=2 # Failure to find the executable is a hard error if monitor_ttys is 1.
  fi

  return $status
}

# Function to check idle time for X session pointers
check_all_pointers_idle() {
  debug_log "check_all_pointers_idle"

  # Use xprintidle if it is present.
  if check_executable "xprintidle"; then
    idle_milliseconds=$(xprintidle)

    idle_seconds_rnd=$(echo "scale=0; $idle_milliseconds / 1000" | bc)

    if ((idle_seconds_rnd >= inactivity_time_trigger)); then
      active_state=0
    else
      active_state=1
    fi
  else
    find_mouse_ids

    local result=$?

    if [[ $result -eq 2 ]]; then
      return 2
    elif [[ $result -eq 1 ]]; then
      return 1
    fi

    for mouse_id in "${mouse_id_array[@]}"; do
      # Note this contains more text than just the coordinates, but we don't care, because
      # we are simply doing a comparison to see if anything changed and it isn't worth
      # the extra CPU time to extract the coordinate from the string.
      current_x[$mouse_id]=$(xinput query-state $mouse_id | grep "valuator\[0\]")
      current_y[$mouse_id]=$(xinput query-state $mouse_id | grep "valuator\[1\]")

      # Check if mouse has moved
      if [ "${current_x[$mouse_id]}" != "${old_x[$mouse_id]}" ] || [ "${current_y[$mouse_id]}" != "${old_y[$mouse_id]}" ]; then
        last_active_time=$(timestamp)
        debug_log "last_active_time =" "$last_active_time"

        active_state=1

        old_x[$mouse_id]="${current_x[$mouse_id]}"
        old_y[$mouse_id]="${current_y[$mouse_id]}"

      fi
    done

    current_time=$(timestamp)

    if ((current_time - last_active_time >= inactivity_time_trigger)); then
      debug_log "set active_state=0"
      active_state=0
    fi
  fi

  return 0
}

# This is the main execution below.

source_config

log "idle_detect.sh version 7 - 202503012."

sleep "$initial_sleep"

previous_pointer_array_size=0
active_state=0
last_active_state=0
last_active_time=0
process_source_config=0
mouse_found=0

while true; do
  # Skip the source config on the first pass, since it was just done before entering the loop.
  if ((process_source_config == 1)); then
    source_config
  else
    process_source_config=1
  fi

  if ((monitor_pointers == 1)); then
    # Check all pointers idle is a no-op and will return status of 1 if no mouse was found.
    # It will return 2 and cause a script exit with failure if the xinput executable is not
    # found (and monitor_pointers is set to 1).
    check_all_pointers_idle

    if [[ $? -eq 2 ]]; then
      exit 1
    fi
  fi

  if ((monitor_ttys == 1)); then
    # Monitor pointers acts as the reset for the active_state for each loop
    # if both are enabled. If monitor_pointers is false, then the reset is here
    if ((monitor_pointers == 0)); then
      active_state=0
    fi

    check_all_ttys_idle

    if [[ $? -eq 2 ]]; then
      exit 1
    fi
  fi

  if ((active_state != last_active_state)); then
    if ((active_state == 0)); then
      eval "$idle_command"
    else
      eval "$active_command"
    fi
  fi

  last_active_state="$active_state"

  sleep 1
done

