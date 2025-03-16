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
      signal="SIGINT"
      ;;
    SIGTERM)
      terminate_subshells "$1"
      signal="SIGTERM"
      ;;
    SIGHUP)
      terminate_subshells "$1"
      signal="SIGHUP"
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

enumerate_event_device_ids() {
  event_device_id_array=()

  for n in /sys/class/input/event*; do
    device="$(cat $n/device/name)"

    for pointing_device in "${dev_input_devices[@]}"; do
      if [ "$pointing_device" = "$device" ]; then
        event_device_id_array["${#event_device_id_array[@]}"]="$(basename $n)"

        log "input device" "$device" "selected at /dev/input/$(basename $n)"
      fi
    done
  done
}

check_event_device_ids_changed() {
  event_device_id_count="$(find /dev/input/event* | wc -l)"

  if ((event_device_id_count_init == 0)); then
    event_device_id_count_prev="$event_device_id_count"

    event_device_id_count_init=1

    return 0
  else
    if ((event_device_id_count!=event_device_id_count_prev)); then
      event_device_id_count_prev="$event_device_id_count"

      return 1
    else
      return 0
    fi
  fi
}

check_evemu_record_exists() {
  if ! check_executable "evemu-record"; then
    log "ERROR: evemu-record not found but is required to do mouse movement monitoring directly from input event devices."
    exit 1
  fi
}

mkdir_event_detect() {
  if [[ ! -d "/run/event_detect" ]]; then
    mkdir "/run/event_detect"
  fi
}

initiate_event_activity_recorders() {
  event_device_monitors_pid_array=()

  for event_id in "${event_device_id_array[@]}"; do
    debug_log "/dev/input/$event_id"

    evemu-record "/dev/input/$event_id" 2> /dev/null | { count=0; while IFS= read -r line; do ((count++)); echo "$count" > "/run/event_detect/"$event_id"_count.dat"; done; } & subshell_pid=$!

    event_device_monitors_pid_array["${#event_device_monitors_pid_array[@]}"]="$subshell_pid"

  done

  debug_log "event activity recorder pids:" "${event_device_monitors_pid_array[@]}"
}

# Wait for all subshells to finish - this blocks.
wait_for_event_activity_recorders() {
  wait "${event_device_monitors_pid_array[@]}"
}

# Clean up dat files
clean_up_event_dat_files() {
  for event_id in "${event_device_id_array[@]}"; do
      debug_log "removing " "$event_count_files_path/$event_id""_count.dat"

      rm "/run/event_detect/$event_id""_count.dat"
  done
}

# Trap SIGINT (Ctrl+C), SIGTERM, and SIGHUP
trap 'handle_signals SIGINT' SIGINT
trap 'handle_signals SIGTERM' SIGTERM
trap 'handle_signals SIGHUP' SIGHUP

signal=""
event_device_id_count=0;
monitor_pid=0

main_script_pid="$BASHPID"

log "event_detect.sh PID:" "$main_script_pid"

check_evemu_record_exists

mkdir_event_detect

(
  event_device_id_count_prev=0;
  event_device_id_count_init=0;

  while true; do
    check_event_device_ids_changed

    result="$?"

    if ((result != 0)); then
      kill -s SIGHUP "$main_script_pid"
    fi

    sleep 1
  done
) & monitor_pid=$!

log "event id monitor PID =" "$monitor_pid"

while true; do
  if [[ "$signal" = "SIGINT" ]] || [[ "$signal" = "SIGTERM" ]]; then
    log "killing event id monitor PID" "$monitor_pid"

    kill -s SIGTERM "$monitor_pid" > /dev/null 2>&1

    exit 0
  else
    signal=""
  fi

  enumerate_event_device_ids

  initiate_event_activity_recorders

  # This blocks until a trapped signal is received
  wait_for_event_activity_recorders

  clean_up_event_dat_files
done
