#pragma once

#ifndef SOFTWARE_MONITOR_HPP
#define SOFTWARE_MONITOR_HPP

#include <map>
#include <string>
#include <sys/types.h>
#include <vector>

namespace monitoring {

inline constexpr const char* kDefaultDpkgDbRoot = "/var/lib/dpkg";
inline constexpr const char* kDefaultDpkgUpdatesRoot = "/var/lib/dpkg/updates";
inline constexpr const char* kDefaultRpmDbRoot = "/var/lib/rpm";
inline constexpr const char* kDefaultUsrLibRpmDbRoot = "/usr/lib/sysimage/rpm";

/**
 * @brief Тип изменения установленного системного пакета.
 */
enum class SoftwareChangeKind {
    Installed,
    Removed,
    Updated
};

/**
 * @brief Конфигурация монитора изменений системных пакетов.
 */
struct SoftwareMonitorConfig {
    std::vector<std::string> db_dirs{
        kDefaultDpkgDbRoot,
        kDefaultDpkgUpdatesRoot,
        kDefaultRpmDbRoot,
        kDefaultUsrLibRpmDbRoot
    };

    bool enable_dpkg_snapshot = true;
    bool enable_rpm_snapshot = true;

    std::string dpkg_query_command{
        R"(LC_ALL=C dpkg-query -W -f='${binary:Package}\t${Version}\t${db:Status-Abbrev}\n' 2>/dev/null)"
    };

    std::string rpm_query_command{
        R"(LC_ALL=C rpm -qa --qf '%{NAME}\t%{EPOCH}\t%{VERSION}\t%{RELEASE}\n' 2>/dev/null)"
    };
};

/**
 * @brief Низкоуровневое событие изменения системного пакета.
 */
struct SoftwareRawEvent {
    std::string package_name;
    std::string package_version;

    std::string old_name;
    std::string old_version;

    std::string new_name;
    std::string new_version;

    pid_t pid = 0;
    pid_t ppid = 0;

    SoftwareChangeKind kind = SoftwareChangeKind::Installed;
};

/**
 * @brief Монитор изменений системных пакетов Linux.
 *
 * SoftwareMonitor отслеживает изменения файловых баз пакетных менеджеров
 * через fanotify. При изменении одной из отслеживаемых директорий монитор
 * сразу перечитывает список установленных пакетов, строит новый snapshot и
 * сравнивает его с предыдущим snapshot-ом.
 *
 * На основании diff-а формируются SoftwareRawEvent:
 * - SoftwareChangeKind::Installed, если появился новый пакет;
 * - SoftwareChangeKind::Removed, если пакет исчез;
 * - SoftwareChangeKind::Updated, если для того же имени пакета старая версия
 *   исчезла, а новая появилась.
 *
 * По умолчанию монитор рассчитан на dpkg/apt и rpm/yum/dnf/apt-rpm системы.
 *
 * @note fanotify используется только как сигнал изменения пакетной БД.
 *       Конкретные имена и версии изменённых пакетов определяются через
 *       snapshot diff, а не напрямую из fanotify-событий.
 */
class SoftwareMonitor {
public:
    SoftwareMonitor();
    explicit SoftwareMonitor(SoftwareMonitorConfig config);
    ~SoftwareMonitor();
    void Shutdown();

    bool SetConfig(SoftwareMonitorConfig config);

    /**
     * @brief Инициализирует fanotify и строит начальный snapshot пакетов.
     *
     * @return 0 при успешной инициализации.
     * @return Код errno при ошибке.
     */
    int Init();
    /**
     * @brief Выполняет один цикл ожидания и обработки fanotify-событий.
     *
     * @param changes Вектор, в который будут добавлены найденные изменения
     *                системных пакетов.
     * @param timeout_ms Таймаут ожидания fanotify-события в миллисекундах.
     */
    void PollOnce(std::vector<SoftwareRawEvent>& changes, int timeout_ms);

private:
    /**
     * @brief Описание отслеживаемой директории пакетной БД.
     */
    struct WatchedRoot {
        std::string path;
        int fd = -1;
    };
    /**
    * @brief Информация о пакете в snapshot-е.
    */
    struct SoftwarePackageInfo {
        std::string name;
        std::string version;
    };

    /**
    * @brief Результат чтения snapshot-а установленных пакетов.
    */
    struct SnapshotReadResult {
        std::map<std::string, SoftwarePackageInfo> packages;
        bool ok = false;
    };

    /**
     * @brief Результат чтения накопленных fanotify-событий.
     */
    struct FanotifyTrigger {
        bool triggered = false;
        pid_t pid = 0;
        pid_t ppid = 0;
    };

    using Snapshot = std::map<std::string, SoftwarePackageInfo>;

private:
    /**
     * @brief Устанавливает fanotify mark на директорию пакетной БД.
     *
     * @param path Путь к директории пакетной БД.
     *
     * @return 0 при успехе или если директория отсутствует.
     * @return Код errno при ошибке открытия директории или установки mark.
     */
    int MarkDbDirectory(const std::string& path);

    /**
     * @brief Вычитывает все накопленные fanotify-события.
     *
     * @return Информация о наличии релевантного fanotify-триггера.
     */
    FanotifyTrigger DrainFanotifyEvents();

    /**
     * @brief Считывает текущий snapshot установленных пакетов.
     *
     * @return SnapshotReadResult с картой пакетов и признаком успешности.
     */
    SnapshotReadResult ReadInstalledSnapshot() const;

    /**
     * @brief Сравнивает два snapshot-а установленных пакетов.
     *
     * Метод определяет установки, удаления и обновления пакетов:
     * - пакет появился в новом snapshot-е — установка;
     * - пакет исчез из нового snapshot-а — удаление;
     * - исчезла старая версия и появилась новая версия того же имени —
     *   обновление.
     *
     * @param old_snapshot Предыдущий snapshot установленных пакетов.
     * @param new_snapshot Новый snapshot установленных пакетов.
     * @param changes Вектор, в который будут добавлены найденные изменения.
     */
    void DiffSnapshots(const Snapshot& old_snapshot, const Snapshot& new_snapshot, std::vector<SoftwareRawEvent>& changes) const;

    /**
     * @brief Возвращает ключ идентичности пакета.
     *
     * @param info Информация о пакете.
     *
     * @return Строковый ключ идентичности пакета.
     */
    static std::string MakeIdentityKey(const SoftwarePackageInfo& info);
    /**
     * @brief Возвращает ключ конкретной установленной версии пакета.
     *
     * @param info Информация о пакете.
     *
     * @return Строковый ключ пакета с версией.
     */
    static std::string MakePackageKey(const SoftwarePackageInfo& info);

private:
    int fan_fd_{-1};
    bool report_name_mode_{false};

    SoftwareMonitorConfig config_;

    std::vector<WatchedRoot> watched_roots_;

    Snapshot snapshot_;
    bool snapshot_initialized_{false};
};

}  // namespace monitoring

#endif  // SOFTWARE_MONITOR_HPP