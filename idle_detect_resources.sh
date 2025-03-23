#!/bin/bash

version() {
  echo "idle_detect/event_detect alpha version 11 - 20250322."
}

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

# Arg 1 is data to write
# Arg 2 is the file to write to (which also is the lockfile)
# Arg 3 is the timeout
atomic_file_write() {
  local arg_size="${#@}"

  if ((arg_size != 3)); then
    log "ERROR: atomic_file_write takes three arguments."
    return 1
  fi

  (
    exec 9> "$2"

    if flock -w "$3" -x 9; then
      # Critical section: write data to the locked file
      echo "$1" > "$2"
      return 0
    else
      log "ERROR: Failed to take a lock on $2 by timeout of $3 seconds."
      return 1
    fi
    # Lock is automatically released when fd 9 is closed
  )
}

# Arg 1 is the file to read from (which also is the lockfile)
# Arg 2 is the timeout
atomic_file_read() {
  local arg_size="${#@}"

  if ((arg_size != 2)); then
    log "ERROR: atomic_file_write takes three arguments."
    return 1
  fi

  (
    exec 9< "$1"

    if flock -w "$2" -s 9; then
      # Critical section: read data from the locked file
      cat "$1"
      return 0
    else
      log "ERROR: Failed to take a lock on $1 by timeout of $2 seconds."
      return 1
    fi
    # Lock is automatically released when fd 9 is closed
  )
}

