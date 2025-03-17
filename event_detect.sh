#!/bin/bash

# (c) Copyright 2025 James C. Owens

config_file="$1"

source "$(dirname "$0")/idle_detect_resources.sh"

source_config

log "$(version)"

sleep "$startup_delay"

# Function to handle signals
handle_signals() {
  debug_log "handle_signals called"

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
      sleep 1
      ;;
    *)
      log "WARNING: Unknown signal received."
      ;;
  esac
}

terminate_subshells() {
  debug_log "terminate_subshells called"
  log "$1 detected. Ensuring subshells are terminated..."

  for pid in "${event_device_monitors_pid_array[@]}"; do
    debug_log "killing PID" "$pid"

    kill -s SIGTERM --timeout 500 SIGKILL "$pid" > /dev/null 2>&1

    result="$?"
    debug_log "kill pid $pid status = $result"
  done

  for event_id in "${event_device_id_array[@]}"; do
    evemu_pid_file="$event_count_files_path/${event_id}_evemu_pid.dat"

    if [[ -f "$evemu_pid_file" ]]; then
        evemu_pid=$(<"$evemu_pid_file")
        if kill -s SIGTERM --timeout 500 SIGKILL "$evemu_pid" > /dev/null 2>&1; then
            debug_log "evemu-record $evemu_pid terminated."
            # rm "$evemu_pid_file" #remove the file when done.
        else
            debug_log "evemu-record $evemu_pid not found."
        fi
    else
        debug_log "evemu pid file not found"
    fi
  done

  sleep 1
}

enumerate_event_device_ids() {
  debug_log "enumerate_event_device_ids called"

  event_device_id_array=()

  for n in /sys/class/input/event*; do
    device="$(<$n/device/name)"

    for pointing_device in "${dev_input_devices[@]}"; do
      if [ "$pointing_device" = "$device" ]; then
        event_device_id_array["${#event_device_id_array[@]}"]="$(basename $n)"

        log "input device" "$device" "selected at /dev/input/$(basename $n)"
      fi
    done
  done
}

check_event_device_ids_changed() {
  debug_log "check_event_device_ids_changed called"

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
  debug_log "check_evemu_record_exists called"

  if ! check_executable "evemu-record"; then
    log "ERROR: evemu-record not found but is required to do mouse movement monitoring directly from input event devices."
    exit 1
  fi
}

mkdir_event_detect() {
  debug_log "mkdir_event_detect called"

  if [[ ! -d "$event_count_files_path" ]]; then
    mkdir "$event_count_files_path"
  fi
}

initiate_event_activity_recorders() {
  debug_log "initiate_event_activity_recorders called"

  event_device_monitors_pid_array=()

  for event_id in "${event_device_id_array[@]}"; do
    debug_log "/dev/input/$event_id"

    local signal_command=""
    local read_result=""
    local fifo_result=""

    fifo="$event_count_files_path/${event_id}_signal.dat"
    echo "none" > "$fifo"

    (
      terminate() {
        debug_log "subshell received SIGTERM. Writing signal to $fifo"
        echo "SIGTERM" > "$fifo"
      }

      trap 'terminate' SIGTERM

      evemu-record "/dev/input/$event_id" 2> /dev/null | {
        count=0

        debug_log "fifo = $fifo"

        # Capture evemu-record PID
        evemu_pid=$!

        echo "$evemu_pid" > "$event_count_files_path/${event_id}_evemu_pid.dat" #store the evemu pid
        debug_log "evemu_pid = $evemu_pid"

        # This complexity is required to ensure that IFS= read -r -t 1 line does not block
        # forever on waiting for a stream from evemu-record. If the device is not active
        # or has even been deleted by device removal, then it would never unblock. Here
        # a time based unblocking (-t 1) is used to ensure that a signal passed in via the
        # $fifo signal file is actually processed, causing the while loop to exit.
        # The terminate_subshells() terminates the $evemu_pid to ensure that evemu_pid does not
        # become an orphan. Note that the braced code is itself a subshell, and that has a
        # different process id.
        while true; do
          IFS= read -r -t 1 line
          read_result="$?"

          if ((read_result == 0)); then
            ((count++))
            echo "$count" > "$event_count_files_path/${event_id}_count.dat"
          fi

          signal_command=$(<"$fifo")

          if [[ "$signal_command" = "SIGTERM" ]]; then
            debug_log "Recieved SIGTERM: breaking from piped stream to ${event_id}_count.dat"
            break
          fi
        done
      } & evemu_parent_pid=$!

      log "evemu_parent_pid = $evemu_parent_pid"

      wait "$evemu_parent_pid"  2>&1 # wait for it to finish.
    ) & subshell_pid=$!

    event_device_monitors_pid_array["${#event_device_monitors_pid_array[@]}"]="$subshell_pid"

  done

  log "event activity recorder pids:" "${event_device_monitors_pid_array[@]}"
}

# Wait for all subshells to finish - this blocks.
wait_for_event_activity_recorders() {
  debug_log "wait_for_event_activity_recorders called"

  wait "${event_device_monitors_pid_array[@]}"
}

# Clean up dat files
clean_up_event_dat_files() {
  debug_log "clean_up_event_dat_files called"

  for event_id in "${event_device_id_array[@]}"; do
      debug_log "removing " "$event_count_files_path/$event_id""_count.dat"

      rm "$event_count_files_path/${event_id}_count.dat"
      rm "$event_count_files_path/${event_id}_evemu_pid.dat"
      rm "$event_count_files_path/${event_id}_signal.dat"
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

# This is a parallet subshell that implements the event device id
# monitoring. It simply checks the count of devices in /dev/input/event*
# and if it changes, sends a SIGHUP to the parent shell. This in
# turn will be caught by the parent shell handle_signals() which
# in turn will call terminate_subshells(). The terminate subshells
# will cause the wait_for_event_activity_recorders to unblock.
(
  event_device_id_count_prev=0;
  event_device_id_count_init=0;

  while true; do
    check_event_device_ids_changed

    result="$?"

    if ((result != 0)); then
      debug_log "event device id count changed: sending SIGHUP to parent shell"
      kill -s SIGHUP "$main_script_pid"
    fi

    sleep 1
  done

) & monitor_pid=$!

log "event id monitor PID =" "$monitor_pid"

while true; do
  # This runs if the wait_for_event_activity_recorders is unblocked
  # below with a SIGINT or SIGTERM and does a final cleanup and
  # then script exit.
  if [[ "$signal" = "SIGINT" ]] || [[ "$signal" = "SIGTERM" ]]; then
    log "killing event id monitor PID" "$monitor_pid"

    kill -s SIGTERM "$monitor_pid" > /dev/null 2>&1

    clean_up_event_dat_files

    exit 0
  else
    signal=""
  fi

  enumerate_event_device_ids

  initiate_event_activity_recorders

  # This blocks until a trapped signal is received
  wait_for_event_activity_recorders

  # If the above is unblocked after subshell termination with
  # a SIGHUP, then clean_up_event_files is executed in
  # preparation for the while loop next pass.
  if [[ "$signal" = "SIGHUP" ]]; then
    clean_up_event_dat_files
  fi
done
