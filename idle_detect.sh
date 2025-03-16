#!/bin/bash

# (c) Copyright 2025 James C. Owens

config_file="$1"

source "$(dirname "$0")/idle_detect_resources.sh"

source "/etc/event_detect.conf"

# Function to determine GUI session type
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
  local idle_seconds=0
  local idle_seconds_rnd=0
  local tty_idle_seconds=0
  local first_pass=0

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

    local tty_check_timestamp=$(timestamp)

    for idle_seconds in ""${idle_seconds_array[@]}""; do
      tty_idle_seconds_rnd=$(echo "scale=0; $idle_seconds / 1" | bc)

      if [[ -z "$tty_idle_seconds_rnd" ]] || [[ "$tty_idle_seconds_rnd" -eq -1 ]]; then
        # User might have just logged out, or other edge case. Skip.
        log "WARNING: tty activity detection encountered an unknown time format or other unknown error."

        status=1

        continue
      fi

      # This allows us to initialize the tty_idle_seconds above as a local with initial value 0.
      if ((first_pass == 0)); then
        tty_idle_seconds="$tty_idle_seconds_rnd"
        first_pass=1
      else
        tty_idle_seconds=$(min "$tty_idle_seconds" "$tty_idle_seconds_rnd")
      fi
    done

    # To handle the case of a short term ssh session where someone logs in, executes a command (which effectively
    # puts the idle to zero), and then logs out, where the session is destroyed, we must remember the idle seconds
    # at the last check, and then ensure that the idle seconds for this check is no more than the last check plus
    # the clock time elapsed. Otherwise as soon as the ssh session is destroyed, the idle time will revert to the
    # minimum of the remaining entries, which could meet the idle criteria, even though little time has actually
    # elapsed.
    local tty_idle_seconds_limit=$((last_tty_idle_seconds + tty_check_timestamp - last_tty_check_timestamp))

    tty_idle_seconds=$(min "$tty_idle_seconds" "$tty_idle_seconds_limit")

    # Note that this accumulates the active state as an or condition from the active_state determined by the
    # pointer check if enabled.
    if ((tty_idle_seconds >= inactivity_time_trigger)); then
      active_state=$((0 | active_state))
    else
      active_state=$((1 | active_state))
    fi
  else
    status=2 # Failure to find the executable is a hard error if monitor_ttys is 1.
  fi

  return $status
}

# Function to check idle time for pointers
check_all_pointers_idle() {
  debug_log "check_all_pointers_idle"

  local event_count_dat_count=0

  # If use_dev_input_events=1 then use /dev/input/event*
  if ((use_dev_input_events == 1)) || [[ "$display_type" = "WAYLAND" ]] || [[ "$display_type" = "WAYLAND_XDG" ]]; then
    if ((event_monitor_baselined == 0)); then
      for event_count_dat in "$event_count_files_path"/event*_count.dat; do
        event_count_dat_count=$(cat "$event_count_dat")

        event_count=$((event_count + event_count_dat_count))
        event_count_previous="$event_count"


        event_monitor_baselined=1
      done

      debug_log "event_count baseline =" "$event_count_previous"
    else
      event_count=0

      for event_count_dat in "$event_count_files_path"/event*_count.dat; do
        event_count_dat_count=$(cat "$event_count_dat")

        event_count=$((event_count + event_count_dat_count))
      done
    fi

    debug_log "event_count =" "$event_count"

    current_time=$(timestamp)

    # This will almost assuredly catch a situation where a device is added or subtracted
    # from the idle_detect.conf file
    if ((event_count != event_count_previous)); then
      last_active_time=$(timestamp)
      debug_log "last_active_time =" "$last_active_time"

      active_state=1

      event_count_previous="$event_count"
    elif ((current_time - last_active_time >= inactivity_time_trigger)); then
      debug_log "set active_state=0"
      active_state=0
    fi
  # Use xprintidle if it is present.
  elif check_executable "xprintidle" 1; then
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

log "idle_detect.sh alpha version 9 - 20250315."

sleep "$initial_sleep"

previous_pointer_array_size=0
active_state=0
last_active_state=0
last_active_time=0
last_tty_check_timestamp=0
last_tty_idle_seconds=0
process_source_config=0
mouse_found=0
event_monitor_baselined=0
event_count=0
event_count_previous=0

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

