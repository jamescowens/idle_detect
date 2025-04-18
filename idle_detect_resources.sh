#!/bin/bash

# (c) Copyright 2025 James C. Owens

# This code is licensed under the MIT license. See LICENSE.md in the repository.

version() {
  echo "idle_detect/event_detect alpha development post version 0.3 - 20250401."
}

timestamp() {
  date --utc +"%s"
}

hr_timestamp() {
  date --utc +"%m/%d/%Y %H:%M:%S.%N"
}

min() {
  local first="$1"
  local second="$2"

  if [[ ! "$first" =~ ^[0-9]+$ ]] || [[ ! "$second" =~ ^[0-9]+$ ]]; then
    echo "Error: Non-numeric input" >&2
    return 1 # Signal an error
  fi

  if ((first < second)); then
    echo "$first"
  else
    echo "$second"
  fi
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
  local soft_error=0

  if [ $# -ge 1 ]; then
    soft_error="$2"
  fi

  if command -v "$executable" &> /dev/null; then
    debug_log "Executable \"$executable\" found."
    return 0 # Success
  else
    if ((soft_error != 1)); then
      log "ERROR: Executable \"$executable\" not found."
    else
      debug_log "Executable \"$executable\" not found."
    fi
    return 1 # Failure
  fi
}
