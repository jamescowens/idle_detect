#!/bin/bash

# (c) Copyright 2025 James C. Owens

config_file="$1"

source "$(dirname "$0")/idle_detect_resources.sh"

source_config

sleep "$startup_delay"

# Function to handle signals
handle_signals() {
  case "$1" in
    SIGINT)
      terminate_subshells "$1"
      ;;
    SIGTERM)
      terminate_subshells "$1"
      ;;
    *)
      log "WARNING: Unknown signal received."
      ;;
  esac
}

# Function to terminate subshells
terminate_subshells() {
  log "$1 detected. Ensuring subshells are terminated..."

  for pid in "${event_device_monitors_pid_array[@]}"; do
    debug_log "killing PID" "$pid"
    kill -s SIGTERM "$pid" > /dev/null 2>&1
  done
}

event_device_id_array=()
event_device_monitors_pid_array=()

if ! check_executable "evemu-record"; then
  log "ERROR: evemu-record not found but is required to do mouse movement monitoring directly from input event devices."
  exit 1
fi

# Trap SIGINT (Ctrl+C) and SIGTERM
trap 'handle_signals SIGINT' SIGINT
trap 'handle_signals SIGTERM' SIGTERM

for n in /sys/class/input/event*; do
  device="$(cat $n/device/name)"

  for pointing_device in "${dev_input_devices[@]}"; do
    if [ "$pointing_device" = "$device" ]; then
      event_device_id_array["${#event_device_id_array[@]}"]="$(basename $n)"

      log "input device" "$device" "selected at /dev/input/$(basename $n)"
    fi
  done
done

if [[ ! -d "/run/event_detect" ]]; then
  mkdir "/run/event_detect"
fi

for event_id in "${event_device_id_array[@]}"; do
  debug_log "/dev/input/$event_id"

  evemu-record "/dev/input/$event_id" | { count=0; while IFS= read -r line; do ((count++)); echo "$count" > "/run/event_detect/"$event_id"_count.dat"; done; } & subshell_pid=$!

  event_device_monitors_pid_array["${#event_device_monitors_pid_array[@]}"]="$subshell_pid"

done

debug_log "${event_device_monitors_pid_array[@]}"

# Wait for all subshells to finish (or Ctrl+C)
for pid in "${event_device_monitors_pid_array[@]}"; do
  wait "$pid"
done

# Clean up dat files before exit
for event_id in "${event_device_id_array[@]}"; do
    debug_log "removing " "$event_count_files_path/$event_id""_count.dat"

    rm "/run/event_detect/$event_id""_count.dat"
done


