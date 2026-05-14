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

enum class SoftwareChangeKind {
    Installed,
    Removed,
    Updated
};

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

struct SoftwareRawEvent {
    /*
     * Общее имя и версия.
     *
     * Для install/update:
     * - package_name == new_name
     * - package_version == new_version
     *
     * Для remove:
     * - package_name == old_name
     * - package_version == old_version
     */
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

class SoftwareMonitor {
public:
    SoftwareMonitor();
    explicit SoftwareMonitor(SoftwareMonitorConfig config);
    ~SoftwareMonitor();

    bool SetConfig(SoftwareMonitorConfig config);
    bool AddDbDirectory(std::string path);

    SoftwareMonitorConfig GetConfig() const;

    int Init();
    void Shutdown();

    void PollOnce(std::vector<SoftwareRawEvent>& changes, int timeout_ms);

private:
    struct WatchedRoot {
        std::string path;
        int fd = -1;
    };

    struct SoftwarePackageInfo {
        std::string name;
        std::string version;
    };

    struct SnapshotReadResult {
        std::map<std::string, SoftwarePackageInfo> packages;
        bool ok = false;
    };

    struct FanotifyTrigger {
        bool triggered = false;
        pid_t pid = 0;
        pid_t ppid = 0;
    };

    using Snapshot = std::map<std::string, SoftwarePackageInfo>;

private:
    int MarkDbDirectory(const std::string& path);

    FanotifyTrigger DrainFanotifyEvents();

    SnapshotReadResult ReadInstalledSnapshot() const;

    void DiffSnapshots(const Snapshot& old_snapshot, const Snapshot& new_snapshot, std::vector<SoftwareRawEvent>& changes) const;

    static std::string MakeIdentityKey(const SoftwarePackageInfo& info);
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