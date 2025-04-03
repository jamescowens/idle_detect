#!/bin/bash

# (c) Copyright 2025 James C. Owens

# This code is licensed under the MIT license. See LICENSE.md in the repository.

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


# Function to check idle time and determine active state.
check_idle() {
  debug_log "check_idle"

  local current_time=0
  local last_active_time=0
  local idle_milliseconds=0
  local idle_seconds_event_detect=0
  local idle_seconds_xprintidle=0
  local last_active_time_by_xprintidle=0

  local using_event_detect=0
  local using_xprintidle=0
  local updating_event_detect=0

  current_time="$(timestamp)"

  # Use xprintidle if it is present and use_xprintidle=1 is specified to allow detection of idle inhibit.
  if ((use_xprintidle == 1)) && ([[ "$display_type" = "X11" ]] || [[ "$display_type" = "X11_XAUTHORITY" ]] || [[ "$display_type" = "X11_LIKELY" ]]); then
    if check_executable "xprintidle" 0; then
      using_xprintidle=1
      idle_milliseconds=$(xprintidle)

      idle_seconds_xprintidle=$(echo "scale=0; $idle_milliseconds / 1000" | bc)

      if ((update_event_detect=1)) && [[ -p "$event_count_files_path/event_registration_pipe" ]]; then
        updating_event_detect=1

        last_active_time_by_xprintidle=$((current_time - idle_seconds_xprintidle))

        debug_log "INFO: sending $last_active_time_by_xprintidle:USER_ACTIVE to $event_count_files_path/event_registration_pipe"

        echo "$last_active_time_by_xprintidle:USER_ACTIVE" > "$event_count_files_path/event_registration_pipe"
      fi

    else
      log "ERROR: use_xprintidle specified but xprintidle cannot be found. Is the package for xprintidle installed?"
      using_xprintidle=0
      return 2
    fi
  fi

  # If use_event_detect then use event_detect service last_active_time.dat output.
  if ((use_event_detect == 1)) || [[ "$display_type" = "WAYLAND" ]] || [[ "$display_type" = "WAYLAND_XDG" ]]; then

    if [[ -f "$event_count_files_path/last_active_time.dat" ]]; then
      using_event_detect=1

      last_active_time=$(<"$event_count_files_path/last_active_time.dat")

      idle_seconds_event_detect=$((current_time - last_active_time))

     else
      log "ERROR: use_event_detect specified but last_active_time.dat is not found. Is the dc_event_detection service running?"
      using_event_detect=0
      return 1
    fi
  fi

  if ((using_event_detect == 0)) && ((using_xprintidle == 0)); then
    log "ERROR: both use_event_detect and use_xprintidle are set to 0 or both are not available and this means there is nothing to do."
    return 2
  fi

  if ((using_event_detect)); then
    debug_log "INFO: idle_seconds_event_detect = $idle_seconds_event_detect"
  fi

  if ((using_xprintidle)); then
    debug_log "INFO idle_seconds_xprintidle = $idle_seconds_xprintidle"
  fi

  if ((using_event_detect == 0)); then
    idle_seconds="$idle_seconds_xprintidle"
  elif ((using_xprintidle == 0)); then
    idle_seconds="$idle_seconds_event_detect"
  else
    idle_seconds=$(min "$idle_seconds_event_detect" "$idle_seconds_xprintidle")
  fi

  result="$?"

  if ((result!=0)); then
    log "ERROR: idle_seconds calculation failed"
  fi

  debug_log "INFO: idle_seconds = $idle_seconds"

  if ((idle_seconds >= inactivity_time_trigger)); then
    active_state=0
  else
    active_state=1
  fi

  return 0
}

# This is the main execution below.

source_config

log "$(version)"

sleep "$initial_sleep"

active_state=0
last_active_state=0
process_source_config=0
display_type=""

while true; do
  # Skip the source config on the first pass, since it was just done before entering the loop.
  if ((process_source_config == 1)); then
    source_config
  else
    process_source_config=1
  fi

  display_type="$(check_gui_session_type)"

  debug_log "INFO: display_type = $display_type"

  check_idle

  if [[ $? -eq 2 ]]; then
    exit 1
  fi

  if ((execute_dc_control_scripts=1)) && ((active_state != last_active_state)); then
    if ((active_state == 0)); then
      eval "$idle_command"
    else
      eval "$active_command"
    fi
  fi

  last_active_state="$active_state"

  sleep 1
done

