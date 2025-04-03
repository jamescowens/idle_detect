# idle_detect

This is an effort to provide a compact C++/script library to detect user activity on a workstation for the purpose of running commands when the idle/active state changes. It is motivated primarily to solve the idle detection issues with BOINC and other Linux based DC programs. I am implementing the following architecture, as a separate service any particular DC architecture. Note that I did a proof of concept in bash shell script, and am currently near completion on the C++ implementation. Event_detect is essentially finished and the idle_detect C++ version is operational and being tested.

Note the C++ is implemented in modern C++-17 compliant code, in an object oriented manner. It also is multithreaded with appropriate locking/critical section control.

1. There is a system level (run as root) service, event_detect, run in a tightly controlled systemd service, dc_event_detection.service, that checks three things for idle time:
- the /dev/input/event devices that are pointing devices. (This catches all phyiscal mouse movement.)
- the pts/tty devices for atime (which provides idle time for terminals/pseudo terminals, including ssh sessions.)
- an input event via a named-pipe from user level detector (more on that below)

The output of the event_detect is a file last_active_time.dat, which is in /run/event_detect. Note that on all reasonably recent Linux distributions, the /run directory is a tmpfs which is in memory only. Using an output file (and for the input events) a named pipe in the same directory is much simpler than using a shared memory (which would require complicated semaphore work to coordinate access), and presents less of a security risk. The ONLY thing written to the last_active_time.dat is an int64 in string format that represents unix epoch time in seconds UTC.

2. There is a user level service, idle_detect, run as a user level systemd service (i.e. systemctl --user ...) that does the following things:
- determines the session type, i.e. X, Wayland, or tty (non GUI)
- for X, checks XSS for idle time. This properly catches idle inhibit on an X session with someone playing a movie if the movie app properly inhibits idle
- for Wayland (this is still in progress) checks ext-idle-notify-v1 interface (for KDE Plasma-wayland and other compositors that use that interface), with a fallback to the dbus Gnome mutter interface for Gnome if necessary to get Wayland session based idle time. This will properly get idle inhibit on Wayland sessions if successful for the running compositor
- for text based session, essentially a no-op
- reports the combined last_active_time from all of the above by writing to the named pipe set up by event_detect a short message in the form of timestamp:USER_ACTIVE, which is received by an asynchronous, non-blocking read thread on the event_detect side, which does validation on the message to prevent a security risk.
- optionally can run dc control scripts itself to control dc programs directly based on idle time.

Since event_detect receives update last_active_times from its own sources AND all user level idle_detect instances running on the machine, and provides an overall last_active_time in last_active_time.dat, the BOINC client or other DC architecture can simply poll the last_idle_time.dat file once a second to determine idleness.

This pretty much will resolve 99% of the issues we have been having with idle detection.

This should be considered late alpha code. It should run stably, but not all corner cases have been tested.

Development work is occurring on the development branch, and releases are on master. I am currently doing periodic pre-release tags (i.e. 0.x) at significant development points.

This code is licensed under the MIT license.

### Current dependencies (both library and development files - for compilation):
 - libevdev
 - libXss
 - dbus-1
 - glib-2.0
 - gobject-2.0
 - gio-2.0
 - wayland-client
 - wayland-protocols

### Installation procedure

1. git clone https://github.com/jamescowens/idle_detect.git and change into the idle_detect directory.

2. Make sure you have installed the package for libevdev.

3. Compile the event_detect C++ application with cmake:
    - cd ./build/cmake
    - cmake ../../ (note you may need the -DCMAKE_CXX_COMPILER=\<c++ compiler path\> -DCMAKE_C_COMPILER=\<c compiler path\> options to point to a C++-17 compliant compiler)
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

13. Modify the dc_pause and dc_unpause scripts to your liking (they are currently set up to control BOINC in its default installation).
- sudo nano /usr/local/bin/dc_pause
- sudo nano /usr/local/bin/dc_unpause

14. If you change the event_detect.conf file or the idle_detect.conf files, then you currently need to restart the appropriate service to pick up the changes. Automatic change detection without restart will be added later.

### This is alpha level code and is currently subject to rapid change.
