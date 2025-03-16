# idle_detect

This is an effort to provide a compact script/C++ library to detect user activity on a workstation for the purpose of running commands when the idle/active state changes. Currently only the script form is implemented. It is motivated primarily to solve the idle detection issues with BOINC and other Linux based DC programs. It is not intended to be as sophisticated in control or logging as https://github.com/Drag-NDrop/IdleRunner for example.

This should be considered an early alpha.

Development work is occurring on the development branch, and releases will be on master.

Conceptually the idea is to handle idle detection for both GUI (X or Wayland sessions) and tty activity. The script version currently handles X and tty activity. A service file is included which can be added to the
systemd user level (i.e. accessed via systemctl --user), although it still does not behave correctly on session login. The check granularity is currently at 1 second intervals, which I believe is sufficient to balance
the responsiveness of the idle check with CPU usage for the check itself.

This code is licensed under the MIT license.

### Current dependencies:
 - xprintidle and/or xinput for X session idle detection
 - w for tty idle detection
 - evemu-record for Wayland (or if desired, low level X session) idle detection
