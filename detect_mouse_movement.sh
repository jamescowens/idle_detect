#!/bin/bash

# Version 4 - 20250308.

config_file="$1"

timestamp() {
  date --utc +"%s"
}

# Source the config file
source_config() {
  debug_echo "source_config"
  current_mtime=$(stat -c %Y "$config_file")

  if [ "$previous_mtime" != "$current_mtime" ]; then
    source "$config_file"
    previous_mtime="$current_mtime"
  fi
}

debug_echo() {
  if ((debug == 1)); then
    echo "$1" "$2"
  fi
}

# Get the ID of the mouse device(s). Initialize the sparse location arrays with zeroes for the valid mouse pointers.
find_mouse_ids() {
  readarray -t pointer_array < <(xinput list | grep -i "slave.*pointer")

  pointer_array_size=${#pointer_array[@]}

  if ((pointer_array_size!=previous_pointer_array_size)); then
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

        debug_echo "mouse_id =" $mouse_id

        mouse_id_array[${#mouse_id_array[@]}]=$mouse_id

        old_x[$mouse_id]=0
        old_y[$mouse_id]=0

        current_x[$mouse_id]=0
        current_y[$mouse_id]=0
      fi
    done

    previous_pointer_array_size=$pointer_array_size
  fi

  if ((mouse_found == 0)); then
    echo "Error: Mouse device not found."
    exit 1
  fi
}

# Function to check idle time for all logged-in users
check_all_ttys_idle() {
  debug_echo "check_all_ttys_idle"

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
      continue
    fi

    if ((idle_seconds_rnd >= inactivity_time_trigger)); then
      active_state=$((0 | active_state))
    else
      active_state=$((1 | active_state))
    fi
  done
}

# Function to check idle time for X session pointers
check_all_pointers_idle() {
  debug_echo "check_all_pointers_idle"

  find_mouse_ids

  for mouse_id in "${mouse_id_array[@]}"; do
    current_x[$mouse_id]=$(xinput query-state $mouse_id | grep "valuator\[0\]")
    current_y[$mouse_id]=$(xinput query-state $mouse_id | grep "valuator\[1\]")

    # Check if mouse has moved
    if [ "${current_x[$mouse_id]}" != "${old_x[$mouse_id]}" ] || [ "${current_y[$mouse_id]}" != "${old_y[$mouse_id]}" ]; then
      last_active_time=$(timestamp)
      debug_echo "last_active_time =" "$last_active_time"

      active_state=1

      old_x[$mouse_id]="${current_x[$mouse_id]}"
      old_y[$mouse_id]="${current_y[$mouse_id]}"

    fi
  done

  current_time=$(timestamp)

  if ((current_time - last_active_time >= inactivity_time_trigger)); then
    debug_echo "set active_state=0"
    active_state=0
  fi
}

# This will be overridden by the config.
debug=1

source_config

sleep "$initial_sleep"

previous_pointer_array_size=0
active_state=0
last_active_state=0
last_active_time=0
process_source_config=0

while true; do

  # Skip the source config on the first pass, since it was just done before entering the loop.
  if ((process_source_config == 1)); then
    source_config
  else
    process_source_config=1
  fi

  if ((monitor_pointers == 1)); then
    check_all_pointers_idle
  fi

  if ((monitor_ttys == 1)); then
    check_all_ttys_idle
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

