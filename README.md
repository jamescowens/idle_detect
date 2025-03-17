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

### Installation procedure

At this stage there are no automated installation scripts. Here are the basic steps to get it working.

1. git clone https://github.com/jamescowens/idle_detect.git and change into the idle_detect directory.

2. Make sure you have installed packages for xprintidle, w, or evemu-record. (The last one is if you want
to do low level direct monitoring of the /dev/input/eventX devices, which works across all GUI's.)

3. sudo cp event_detect.sh /usr/local/bin

4. sudo cp idle_detect.sh /usr/local/bin

5. sudo cp idle_detect_resources.sh /usr/local/bin

6. If you are going to use evemu-record, then sudo cp dc_event_detection.service /etc/systemd/system
and then sudo systemctl daemon-reload

7. sudo cp event_detect.conf /etc

8. Modify event_detect.conf to your liking with sudo nano /etc/event_detect.conf.

9. cp idle_detect.conf ~/.config/idle_detect.conf

10. Modify idle_detect.conf to your liking with nano ~/.config/idle_detect.conf

10. cp dc_idle_detection.service ~/.config/systemd/user

11. systemctl --user daemon-reload

12. If using evemu-record for Wayland or low-level event device monnitoring (via use_dev_input_events=1 or because you have a Wayland GUI session), sudo systemctl enable --now dc_event_detection. This enables and starts the pointing device event detection service.

13. systemctl --user enable --now dc_idle_detection

14. Check the status of both the services: sudo systemctl status dc_event_detection, and systemctl --user status dc_idle_detection

15. The user level idle_detection script detects changes to the idle_detect.conf file and re-sources the changes; however if you change the event_detect.conf file for the system event detection service, then you currently need to restart the system level dc_event_detection service via sudo systemctl restart dc_event_detection.

### This is alpha level code and is currently subject to rapid change.
