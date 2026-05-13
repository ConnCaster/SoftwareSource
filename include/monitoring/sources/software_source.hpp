#pragma once

#ifndef SOFTWARE_SOURCE_HPP
#define SOFTWARE_SOURCE_HPP

#include <memory>

#include "monitoring/event_queue/i_event_queue.hpp"
#include "monitoring/sources/threaded_source.hpp"
#include "monitoring/sources/monitors/software_monitor/software_monitor.hpp"

namespace monitoring {

    class SoftwareSource : public ThreadedSource {
    public:
        explicit SoftwareSource(std::shared_ptr<IEventQueue> queue);

        SoftwareSource(
            std::shared_ptr<IEventQueue> queue,
            SoftwareMonitorConfig config
        );

        void SetMonitorConfig(const SoftwareMonitorConfig& config);
        SoftwareMonitorConfig GetMonitorConfig() const;

        ~SoftwareSource() override;

    protected:
        void Run() override;

    private:
        std::unique_ptr<SoftwareMonitor> monitor_{};
    };

}  // namespace monitoring

#endif  // SOFTWARE_SOURCE_HPP