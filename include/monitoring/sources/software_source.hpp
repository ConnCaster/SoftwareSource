#pragma once

#ifndef SOFTWARE_SOURCE_HPP
#define SOFTWARE_SOURCE_HPP

#include <memory>

#include "monitoring/event_queue/i_event_queue.hpp"
#include "monitoring/sources/threaded_source.hpp"
#include "monitoring/sources/monitors/software_monitor/software_monitor.hpp"

namespace monitoring {

    /**
     * @brief Источник событий изменения системных пакетов Linux.
     *
     * SoftwareSource запускает SoftwareMonitor в отдельном рабочем потоке,
     * получает от него низкоуровневые изменения SoftwareRawEvent и преобразует их
     * в события мониторинга SoftwareEvent.
     *
     * Монитор отслеживает изменения баз данных пакетных менеджеров через fanotify.
     * После fanotify-триггера он перечитывает список установленных пакетов,
     * сравнивает новый snapshot с предыдущим и определяет установки, удаления
     * и обновления пакетов.
     *
     * Сформированные SoftwareEvent помещаются в общую очередь событий мониторинга.
     */
    class SoftwareSource : public ThreadedSource {
    public:
        SoftwareSource(std::shared_ptr<IEventQueue> queue);
        SoftwareSource(std::shared_ptr<IEventQueue> queue, SoftwareMonitorConfig config);
        ~SoftwareSource() override;

        /**
         * @brief Устанавливает конфигурацию внутреннего SoftwareMonitor.
         *
         * @param config Новая конфигурация монитора системных пакетов.
         */
        void SetMonitorConfig(const SoftwareMonitorConfig& config);

    protected:
        void Run() override;

    private:
        std::unique_ptr<SoftwareMonitor> monitor_{};
    };

}  // namespace monitoring

#endif  // SOFTWARE_SOURCE_HPP