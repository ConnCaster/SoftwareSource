#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <thread>

#include "monitoring/event_queue/event_queue.hpp"
#include "monitoring/events/monitor_event.hpp"
#include "monitoring/sources/software_source.hpp"

volatile std::sig_atomic_t g_stop = 0;

void OnSignal(int) {
    g_stop = 1;
}

int main() {
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    auto queue = std::make_shared<monitoring::EventQueue>(4096);

    monitoring::SoftwareMonitorConfig config;

    config.db_dirs = {
        "/var/lib/dpkg",
        "/var/lib/dpkg/updates",

        "/var/lib/rpm",
        "/usr/lib/sysimage/rpm"
    };

    config.enable_dpkg_snapshot = true;
    config.enable_rpm_snapshot = true;

    monitoring::SoftwareSource source(queue);
    source.SetMonitorConfig(config);

    const int rc = source.Start();

    if (rc != 0) {
        std::cerr << "source.Start() failed: " << rc << '\n';
        return 1;
    }

    std::cerr << "main: entering software event loop\n";

    while (!g_stop) {
        auto event = queue->TryPop();

        if (event) {
            if (event->src_type != monitoring::SourceType::Software) {
                continue;
            }

            auto software_event =
                std::static_pointer_cast<monitoring::SoftwareEvent>(event);

            std::cout << "event=" << monitoring::GetEventName(software_event->type)
                      << " name=" << software_event->name
                      << " version=" << software_event->version
                      << " old_name=" << software_event->old_name
                      << " old_version=" << software_event->old_version
                      << " new_name=" << software_event->new_name
                      << " new_version=" << software_event->new_version
                      << " pid=" << software_event->pid
                      << " ppid=" << software_event->ppid
                      << std::endl;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    std::cerr << "main: stopping source and queue\n";

    source.Stop();
    queue->Shutdown();

    std::cerr << "main: exiting\n";
    return 0;
}