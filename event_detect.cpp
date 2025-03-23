#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <unistd.h>
#include <sys/wait.h>
#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>

//std::string event_count_files_path = "/path/to/event_count_files";
int sig;

std::vector<std::pair<std::string, std::thread>> event_activity_recorder_threads;
std::vector<std::string> event_device_id_array;
std::pair<std::string, std::thread> monitor_thread;
std::atomic<bool> interrupt_recorders = 0;
std::atomic<bool> interrupt_monitor = 0;

std::mutex mtx_recorder_threads;
std::mutex mtx_monitor_thread;
std::condition_variable cv_recorders;
std::condition_variable cv_monitor;

std::atomic<int64_t> event_count = 0;

class event_monitor
{

};

void log(const std::string& message) {
    std::stringstream out;

    out << message << std::endl;

    std::cout << out.str();
}

void debug_log(const std::string& message) {
    log(message);
}

void error_log(const std::string& message) {
    std::stringstream out;

    out << message << std::endl;

    std::cerr << out.str();
}

void terminate_event_activity_recorder_threads() {
    for (auto& t : event_activity_recorder_threads) {
        if (t.second.joinable()) {
            pthread_kill(t.second.native_handle(), SIGTERM);
        }
    }
}


void handle_signals(int signum) {
    debug_log("handle_signals called");

    switch (signum) {
    case SIGINT:
        debug_log("SIGINT received.");
        interrupt_recorders = true;
        cv_recorders.notify_all();
        break;
    case SIGTERM:
        debug_log("SIGTERM received");
        interrupt_recorders = true;
        cv_recorders.notify_all();
        break;
    case SIGHUP:
        debug_log("SIGHUP received");
        interrupt_recorders = true;
        cv_recorders.notify_all();
        break;
    default:
        log("WARNING: Unknown signal received.");
        break;
    }

    sig = signum;
}

void enumerate_event_device_ids() {
    debug_log("enumerate_event_device_ids called");

    event_device_id_array.clear();

    // Placeholder for actual device enumeration logic
    event_device_id_array.push_back("event0");
    event_device_id_array.push_back("event1");

    if (event_device_id_array.empty()) {
        log("ERROR: No pointing devices identified to monitor. Exiting.");
        exit(1);
    }
}

void event_activity_recorder(const std::string& event_id) {
    debug_log("/dev/input/" + event_id);

    //std::ofstream fifo(event_count_files_path + "/" + event_id + "_signal.dat");
    //fifo << "none";
    //fifo.close();

    while (true) {
        // Simulate evemu-record

        std::unique_lock<std::mutex> lock(mtx_recorder_threads);
        cv_recorders.wait_for(lock, std::chrono::seconds(1), []{ return interrupt_recorders.load(); });

        if (interrupt_recorders) {
            break;
        }

        std::stringstream message;

        message << "event_activity_recorder for "
                << event_id;

        log(message.str());
    }
}

void initiate_event_activity_recorders() {
    debug_log("initiate_event_activity_recorders called");

    interrupt_recorders = false;

    for (const std::string& event_id : event_device_id_array) {

        std::thread activity_thread(event_activity_recorder, event_id);

        event_activity_recorder_threads.emplace_back(std::make_pair(event_id, std::move(activity_thread)));
    }

    debug_log("event activity recorder threads started");
    for (const auto& t : event_activity_recorder_threads) {
        std::cout << t.second.get_id() << " ";
    }

    std::cout << std::endl;
}

void event_activity_monitor() {
    while (true) {
        std::unique_lock<std::mutex> lock(mtx_monitor_thread);
        cv_monitor.wait_for(lock, std::chrono::seconds(1), []{ return interrupt_monitor.load(); });

        if (interrupt_monitor) {
            break;
        }

        std::stringstream message;

        message << "event_activity_monitor";

        log(message.str());
    }
}

void initiate_event_activity_monitor() {
    debug_log("initiate_event_activity_monitor called");

    interrupt_monitor = false;

    std::thread monitor(event_activity_monitor);

    monitor_thread = std::make_pair("monitor", std::move(monitor));
}

void clean_up_event_dat_files() {
    debug_log("clean_up_event_dat_files called");

    //for (const std::string& event_id : event_device_id_array) {
    //    std::remove((event_count_files_path + "/" + event_id + "_count.dat").c_str());
    //    std::remove((event_count_files_path + "/" + event_id + "_evemu_pid.dat").c_str());
    //    std::remove((event_count_files_path + "/" + event_id + "_signal.dat").c_str());
    //}
    //std::remove((event_count_files_path + "/last_active_time.dat").c_str());
}

int main() {
    // To avoid signal races, block signals in the main thread
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGHUP);

    if (pthread_sigmask(SIG_BLOCK, &mask, nullptr) != 0) {
        perror("pthread_sigmask");
        return 1;
    }

    // Register signal handler
    struct sigaction sa;
    sa.sa_handler = handle_signals;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, nullptr) == -1) {
        perror("sigaction");
        return 1;
    }

    log("event_detect C++ program started");

    //std::ofstream last_active_time_file(event_count_files_path + "/last_active_time.dat");
    //last_active_time_file << "0";
    //last_active_time_file.close();

    initiate_event_activity_monitor();

    while (true) {
        if (sig == SIGINT || sig == SIGTERM) {
            log("killing event id monitor thread");
            interrupt_monitor = true;
            cv_monitor.notify_all();

            if (monitor_thread.second.joinable()) {
                monitor_thread.second.join();
            }

            clean_up_event_dat_files();

            return 0;
        }

        enumerate_event_device_ids();
        initiate_event_activity_recorders();

        // Wait for signal
        int sig;
        if (sigwait(&mask, &sig) == 0) {
            handle_signals(sig);
        }

        // Wait for all threads to finish
        log("joining event activity worker threads");
        for (auto& t : event_activity_recorder_threads) {
            if (t.second.joinable()) {
                t.second.join();
            }
        }

        event_activity_recorder_threads.clear();

        if (sig == SIGHUP) {
            clean_up_event_dat_files();
        }
    }

    return 0;
}
