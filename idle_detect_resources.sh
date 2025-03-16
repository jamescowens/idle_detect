#!/bin/bash

timestamp() {
  date --utc +"%s"
}

hr_timestamp() {
  date --utc +"%m/%d/%Y %H:%M:%S.%N"
}

min() {
  first="$1"
  second="$2"

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
