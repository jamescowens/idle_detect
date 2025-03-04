#!/bin/bash

# Version 3 - 20250304.

timestamp() {
  date --utc +"%s"
}

config_file="$1"

source "$config_file"

previous_mtime=$(stat -c %Y "$config_file")

sleep "$initial_sleep"

pointer_name_array=($pointer_names)

mouse_found=0
previous_pointer_array_size=0

# Get the ID of the mouse device(s). Initialize the location arrays with zeroes for the valid mouse pointers.
find_mouse_ids() {
  if [ -z "$pointer_names" ]; then
    readarray -t pointer_array < <(xinput list | grep -i "slave.*pointer")

    pointer_array_size=${#pointer_array[@]}

    if ((pointer_array_size!=previous_pointer_array_size)); then
      mouse_id_array=()
      old_x=()
      old_y=()
      current_x=()
      current_y=()

      search_term="id="

      for pointer in "${pointer_array[@]}"; do

        mouse_id=$(echo "$pointer" | sed 's/.*id=\([0-9]*\).*/\1/'  | head -n 1)

        if [ ! -z "$mouse_id" ]; then
          mouse_found=1

          echo "mouse_id =" $mouse_id

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
    previous_pointer_array_size=0

    mouse_id_array=()
    old_x=()
    old_y=()
    current_x=()
    current_y=()

    for pointer_name in "${pointer_name_array[@]}"; do
      mouse_id=$(xinput list | grep "slave.*pointer" | grep -i "$pointer_name" | sed 's/.*id=\([0-9]*\).*/\1/' | head -n 1)

      if [ ! -z "$mouse_id" ]; then
        mouse_found=1

        echo "mouse_id =" $mouse_id

        mouse_id_array[${#mouse_id_array[@]}]=$mouse_id

        old_x[$mouse_id]=0
        old_y[$mouse_id]=0

        current_x[$mouse_id]=0
        current_y[$mouse_id]=0
      fi
    done
  fi
}

find_mouse_ids

if ((mouse_found==0)); then
  echo "Error: Mouse device not found."
  exit 1
fi

last_active_state=0
last_active_time=0
active_state=0

while true; do
  current_mtime=$(stat -c %Y "$config_file")

  if [ -z "$pointer_names" ]; then
    echo "pointer_names_empty"

    # Re-source the config file each traversal to detect any change to pointer_naames variable and/or added devices.
    source "$config_file"
    previous_mtime="$current_mtime"
    find_mouse_ids
  else
    echo "pointer_names_defined"
  fi


  # Re-source the config file if it has been updated.
  if [ "$current_mtime" -ne "$previous_mtime" ]; then
    source "$config_file"
    previous_mtime="$current_mtime"
    find_mouse_ids
  fi

  for mouse_id in "${mouse_id_array[@]}"; do
    current_x[$mouse_id]=$(xinput query-state $mouse_id | grep "valuator\[0\]")
    current_y[$mouse_id]=$(xinput query-state $mouse_id | grep "valuator\[1\]")

    # Check if mouse has moved
    if [ "${current_x[$mouse_id]}" != "${old_x[$mouse_id]}" ] || [ "${current_y[$mouse_id]}" != "${old_y[$mouse_id]}" ]; then
      last_active_time=$(timestamp)

      active_state=1

      old_x[$mouse_id]="${current_x[$mouse_id]}"
      old_y[$mouse_id]="${current_y[$mouse_id]}"

    else
      current_time=$(timestamp)

      if ((current_time - last_active_time >= inactivity_time_trigger)); then
        active_state=0
      fi
    fi
  done

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

