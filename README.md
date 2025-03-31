# idle_detect

This is an effort to provide a compact C++/script library to detect user activity on a workstation for the purpose of running commands when the idle/active state changes. It is motivated primarily to solve the idle detection issues with BOINC and other Linux based DC programs. There are two major components:

1. A system-wide C++ application, event_detect, to be run as a systemd service that detects pointer activity and tty/pty activity and reports the last active time of the user. This application currently writes the last active time to a file /run/event_detect/last_active_time.dat, but it is envisioned for there to be an option to use a shared memory segment will also be offered. (The script version of event_detect is now deprecated.)

2. A user level application, idle_detect, currently in script form, to be run as an automatically started application, or systemd user level service, to use the last active time reported by event_detect in conjunction with other GUI session tools available, such as xprintidle, etc. to be able to properly detect "idle inhibit" at the GUI session level. This will be replaced by a C++ application in the near future.

The detection of "idle time" on a Linux workstation is not straightforward. The Linux kernel has a well-developed and standardized mechanism for monitoring activity at the device input level via the /dev/input/event* devices, and a reliable way of determining which of those are pointing devices. The tty/pty monitoring is not as clean, but can be done fairly reliably by monitoring the atime for the /dev/pty/\* and dev/tty* devices. The gap is at the GUI session (window manager or compositor level). Monitoring the user activity at the lower level of the input devices and pty/tty devices is generic and works across every GUI session type and also if there is no GUI session at all. The problem is that there are some activities in the GUI where the user is not apparently active via the lower level activity measurement, but they are *considered* active by the GUI session. The most common example is watching a movie. the XSS module in X allows an application to essentially inhibit the idle detection while it is running. This was originally to prevent the lock screen or screensaver to activate during a movie. This is only implemented at the GUI session level.

To make matters worse, to be able to use this capability means that you have to use the X xprintidle (if via script) or the XSS module interface that xprintidle uses (if using C++), or in Wayland, the compositor equivalent. X is standardized, which makes this fairly easy. Wayland is not. While there is a standard way for an application to indicate in Wayland to inhibit "idle", there is NO standardized idle detection capability at the window manager level in Wayland. Each compositor implements its own interface for this, and unfortunately, each currently available compositor for the different windowing environments, such as KDE or Gnome does it differently. This is unfortunate. This is where the remaining gap is:

The combination of event_detect and idle_detect currently reliably detects idle time for user activity, *including* idle inhibit, for X sessions and pty/tty sessions. It currently reliably detects idle time for Wayland sessions, but NOT idle inhibit. This means that if one is watching a movie in Wayland, after the idle time trigger is reached, the system will be considered idle. The lift to close this remaining gap is considerable in terms of both programming effort and the maintenance required afterwards to keep up with all of the GUI window managers, since this is still evolving.

This should be considered alpha code.

Development work is occurring on the development branch, and releases will be on master.

This code is licensed under the MIT license.

### Current dependencies:
 - libevdev library and development (header) files
 - libXss-devel for idle inhibit detection (idle detect C++ version)
 - xprintidle for X session idle inhibit detection (idle_detect.sh)

### Installation procedure

1. git clone https://github.com/jamescowens/idle_detect.git and change into the idle_detect directory.

2. Make sure you have installed the package for libevdev and for xprintidle (if you intend to detect idle inhibit in X).

3. Compile the event_detect C++ application with cmake:
    - cd ./build/cmake
    - cmake ../../ (note you may need the -DCMAKE_CXX_COMPILER=\<c++ compiler path\> option to point to a C++-17 compliant compiler)
    - cmake build .

4. Install executables and system-wide service with sudo cmake --install .

5. Load the new service into systemd with sudo systemctl daemon-reload.

6. Modify event_detect.conf to your liking with sudo nano /etc/event_detect.conf.

7. Install user level script and user level service with cmake --build . --target install_user_service.

8. Modify idle_detect.conf to your liking with nano ~/.config/idle_detect.conf

9. Load the new user level service into systemd with systemctl --user daemon-reload

10. sudo systemctl enable --now dc_event_detection. This enables and starts the pointing device and pts/tty event detection service.

11. systemctl --user enable --now dc_idle_detection. This enables and starts the user mode component as a user level service.

12. Check the status of both the services: sudo systemctl status dc_event_detection, and systemctl --user status dc_idle_detection, and make sure they are successfully running.

13. Copy the dc_pause and dc_unpause scripts to /usr/local/bin and modify to your liking (they are currently set up to control BOINC in its default installation). This will be put in the cmake install shortly:
    - sudo cp dc_pause /usr/local/bin
    - sudo cp dc_unpause /usr/local/bin
    - sudo nano /usr/local/bin/dc_pause
    - sudo nano /usr/local/bin/dc_unpause

14. The user level idle_detection script detects changes to the idle_detect.conf file and automatically applies the changes; however if you change the event_detect.conf file for the system event detection service, then you currently need to restart the system level dc_event_detection service via sudo systemctl restart dc_event_detection.

### This is alpha level code and is currently subject to rapid change.
