#include "monitoring/sources/software_source.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

#include "monitoring/events/monitor_event.hpp"

namespace monitoring {
namespace {

constexpr int kPollingTimeoutMs = 250;

EventType ToSoftwareEventType(SoftwareChangeKind kind) {
    switch (kind) {
        case SoftwareChangeKind::Installed:
            return EventType::SoftwarePackageInstall;

        case SoftwareChangeKind::Removed:
            return EventType::SoftwarePackageRemove;

        case SoftwareChangeKind::Updated:
            return EventType::SoftwarePackageUpdate;
    }

    return EventType::Unknown;
}

std::string SelectCurrentName(const SoftwareRawEvent& change) {
    if (!change.package_name.empty()) {
        return change.package_name;
    }

    if (!change.new_name.empty()) {
        return change.new_name;
    }

    return change.old_name;
}

std::string SelectCurrentVersion(const SoftwareRawEvent& change) {
    if (!change.package_version.empty()) {
        return change.package_version;
    }

    if (!change.new_version.empty()) {
        return change.new_version;
    }

    return change.old_version;
}

}  // namespace

SoftwareSource::SoftwareSource(std::shared_ptr<IEventQueue> queue)
    : ThreadedSource(std::move(queue)),
      monitor_(std::make_unique<SoftwareMonitor>()) {
    std::cout << "SoftwareSource: constructed\n";
}

SoftwareSource::SoftwareSource(
    std::shared_ptr<IEventQueue> queue,
    SoftwareMonitorConfig config
)
    : ThreadedSource(std::move(queue)),
      monitor_(std::make_unique<SoftwareMonitor>(std::move(config))) {
    std::cout << "SoftwareSource: constructed with custom config\n";
}

void SoftwareSource::SetMonitorConfig(const SoftwareMonitorConfig& config) {
    if (is_running_.load()) {
        std::cerr << "SoftwareSource: cannot set config while running\n";
        return;
    }

    monitor_->SetConfig(config);
}

SoftwareMonitorConfig SoftwareSource::GetMonitorConfig() const {
    return monitor_->GetConfig();
}

SoftwareSource::~SoftwareSource() {
    Stop();
}

void SoftwareSource::Run() {
    const int init_rc = monitor_->Init();

    if (init_rc != 0) {
        std::cerr << "SoftwareSource: failed to init monitor, errno="
                  << init_rc
                  << std::endl;
        return;
    }

    while (is_running_.load()) {
        std::vector<SoftwareRawEvent> raw_changes;
        monitor_->PollOnce(raw_changes, kPollingTimeoutMs);

        for (auto& change : raw_changes) {
            auto event = std::make_shared<SoftwareEvent>();

            event->timestamp = std::chrono::system_clock::now();
            event->pid = change.pid;
            event->ppid = change.ppid;
            event->result = 0;
            event->type = ToSoftwareEventType(change.kind);

            event->name = SelectCurrentName(change);
            event->version = SelectCurrentVersion(change);

            event->old_name = std::move(change.old_name);
            event->old_version = std::move(change.old_version);

            event->new_name = std::move(change.new_name);
            event->new_version = std::move(change.new_version);

            event->package_manager = std::move(change.package_manager);
            event->architecture = std::move(change.architecture);

            queue_->Push(std::move(event));
        }
    }

    monitor_->Shutdown();
}

}  // namespace monitoring