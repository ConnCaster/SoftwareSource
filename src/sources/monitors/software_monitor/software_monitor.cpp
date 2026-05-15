#include "monitoring/sources/monitors/software_monitor/software_monitor.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/fanotify.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

namespace monitoring {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kBufSize = 16384;

constexpr uint64_t kSoftwareDbWriteMask =
    FAN_MODIFY |
    FAN_CLOSE_WRITE |
    FAN_ATTRIB;

constexpr uint64_t kSoftwareDbNameMask =
    FAN_CREATE |
    FAN_DELETE |
    FAN_MOVED_FROM |
    FAN_MOVED_TO;

constexpr uint64_t kSoftwareDbActionMask =
    kSoftwareDbWriteMask |
    kSoftwareDbNameMask;

bool IsSoftwareDbEvent(uint64_t mask) {
    return (mask & kSoftwareDbActionMask) != 0;
}

std::string NormalizeDirPath(std::string path) {
    if (path.empty()) {
        return {};
    }

    path = fs::path(path).lexically_normal().string();

    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }

    return path;
}

void NormalizeConfig(SoftwareMonitorConfig& config) {
    for (auto& path : config.db_dirs) {
        path = NormalizeDirPath(std::move(path));
    }

    config.db_dirs.erase(
        std::remove_if(
            config.db_dirs.begin(),
            config.db_dirs.end(),
            [](const std::string& path) {
                return path.empty();
            }
        ),
        config.db_dirs.end()
    );

    std::sort(config.db_dirs.begin(), config.db_dirs.end());
    config.db_dirs.erase(
        std::unique(config.db_dirs.begin(), config.db_dirs.end()),
        config.db_dirs.end()
    );
}

std::vector<std::string> Split(const std::string& value, char separator) {
    std::vector<std::string> result;

    std::size_t start = 0;

    for (;;) {
        const std::size_t pos = value.find(separator, start);

        if (pos == std::string::npos) {
            result.push_back(value.substr(start));
            break;
        }

        result.push_back(value.substr(start, pos - start));
        start = pos + 1;
    }

    return result;
}

pid_t GetParentPid(pid_t pid) {
    if (pid <= 0) {
        return -1;
    }

    std::ifstream file("/proc/" + std::to_string(pid) + "/status");

    if (!file.is_open()) {
        return -1;
    }

    std::string line;

    while (std::getline(file, line)) {
        if (line.rfind("PPid:", 0) == 0) {
            std::istringstream iss(line.substr(5));

            pid_t ppid = -1;
            iss >> ppid;

            return ppid;
        }
    }

    return -1;
}

struct CommandResult {
    std::vector<std::string> lines;
    bool ok = false;
};

CommandResult RunCommandLines(const std::string& command) {
    CommandResult result;

    FILE* pipe = popen(command.c_str(), "r");

    if (pipe == nullptr) {
        return result;
    }

    std::array<char, 4096> buffer{};
    std::string pending;

    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        pending += buffer.data();

        for (;;) {
            const std::size_t pos = pending.find('\n');

            if (pos == std::string::npos) {
                break;
            }

            std::string line = pending.substr(0, pos);

            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            if (!line.empty()) {
                result.lines.push_back(std::move(line));
            }

            pending.erase(0, pos + 1);
        }
    }

    if (!pending.empty()) {
        if (!pending.empty() && pending.back() == '\r') {
            pending.pop_back();
        }

        if (!pending.empty()) {
            result.lines.push_back(std::move(pending));
        }
    }

    const int status = pclose(pipe);

    result.ok =
        status != -1 &&
        WIFEXITED(status) &&
        WEXITSTATUS(status) == 0;

    return result;
}

std::string BuildRpmVersion(const std::string& epoch, const std::string& version, const std::string& release) {
    std::string result = version;

    if (!release.empty()) {
        result += "-" + release;
    }

    if (!epoch.empty() && epoch != "0" && epoch != "(none)") {
        result = epoch + ":" + result;
    }

    return result;
}

}  // namespace

SoftwareMonitor::SoftwareMonitor() {
    NormalizeConfig(config_);
}

SoftwareMonitor::SoftwareMonitor(SoftwareMonitorConfig config)
    : config_(std::move(config)) {
    NormalizeConfig(config_);
}

SoftwareMonitor::~SoftwareMonitor() {
    Shutdown();
}

bool SoftwareMonitor::SetConfig(SoftwareMonitorConfig config) {
    if (fan_fd_ >= 0) {
        std::cerr << "SoftwareMonitor::SetConfig() must be called before Init()"
                  << std::endl;
        return false;
    }

    NormalizeConfig(config);

    if (config.db_dirs.empty()) {
        std::cerr << "SoftwareMonitor config has no db directories"
                  << std::endl;
        return false;
    }

    if (!config.enable_dpkg_snapshot && !config.enable_rpm_snapshot) {
        std::cerr << "SoftwareMonitor config has no enabled snapshot backend"
                  << std::endl;
        return false;
    }

    config_ = std::move(config);
    return true;
}

int SoftwareMonitor::Init() {
    Shutdown();

    NormalizeConfig(config_);

    if (config_.db_dirs.empty()) {
        std::cerr << "SoftwareMonitor: no package db directories configured"
                  << std::endl;
        return EINVAL;
    }

    if (!config_.enable_dpkg_snapshot && !config_.enable_rpm_snapshot) {
        std::cerr << "SoftwareMonitor: no snapshot backend enabled"
                  << std::endl;
        return EINVAL;
    }

    constexpr unsigned int kBaseInitFlags =
        FAN_CLASS_NOTIF |
        FAN_CLOEXEC |
        FAN_NONBLOCK;

    report_name_mode_ = true;

    fan_fd_ = fanotify_init(
        kBaseInitFlags | FAN_REPORT_DIR_FID | FAN_REPORT_NAME,
        O_RDONLY | O_CLOEXEC | O_LARGEFILE
    );

    if (fan_fd_ < 0 && errno == EINVAL) {
        report_name_mode_ = false;

        fan_fd_ = fanotify_init(
            kBaseInitFlags,
            O_RDONLY | O_CLOEXEC | O_LARGEFILE
        );
    }


    if (fan_fd_ < 0) {
        const int saved_errno = errno;

        std::cerr << "fanotify_init() failed: "
                  << std::strerror(saved_errno)
                  << std::endl;

        return saved_errno;
    }

    int first_error = 0;

    for (const auto& db_dir : config_.db_dirs) {
        const int rc = MarkDbDirectory(db_dir);

        if (rc != 0 && first_error == 0) {
            first_error = rc;
        }
    }

    if (watched_roots_.empty()) {
        std::cerr << "SoftwareMonitor: no existing package db directories were marked"
                  << std::endl;

        Shutdown();
        return first_error != 0 ? first_error : ENOENT;
    }

    auto initial_snapshot = ReadInstalledSnapshot();

    if (!initial_snapshot.ok) {
        std::cerr << "SoftwareMonitor: failed to read initial software snapshot"
                  << std::endl;

        Shutdown();
        return ENOENT;
    }

    snapshot_ = std::move(initial_snapshot.packages);
    snapshot_initialized_ = true;

    return 0;
}

void SoftwareMonitor::Shutdown() {
    if (fan_fd_ >= 0) {
        close(fan_fd_);
        fan_fd_ = -1;
    }

    for (auto& root : watched_roots_) {
        if (root.fd >= 0) {
            close(root.fd);
            root.fd = -1;
        }
    }

    watched_roots_.clear();

    snapshot_.clear();
    snapshot_initialized_ = false;
}

int SoftwareMonitor::MarkDbDirectory(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);

    if (fd < 0) {
        const int saved_errno = errno;

        if (saved_errno == ENOENT || saved_errno == ENOTDIR || saved_errno == EACCES) {
            return 0;
        }

        std::cerr << "open(" << path << ") failed: "
                  << std::strerror(saved_errno)
                  << std::endl;

        return saved_errno;
    }

    uint64_t mark_mask = kSoftwareDbWriteMask | FAN_EVENT_ON_CHILD;

    if (report_name_mode_) {
        mark_mask |= kSoftwareDbNameMask | FAN_ONDIR;
    }

    if (fanotify_mark(
            fan_fd_,
            FAN_MARK_ADD | FAN_MARK_ONLYDIR,
            mark_mask,
            AT_FDCWD,
            path.c_str()
        ) < 0) {
        const int saved_errno = errno;

        std::cerr << "fanotify_mark(" << path << ") failed: "
                  << std::strerror(saved_errno)
                  << " errno=" << saved_errno
                  << std::endl;

        close(fd);
        return saved_errno;
    }

    watched_roots_.push_back({path, fd});

    std::cout << "software db directory marked successfully: "
              << path
              << std::endl;

    return 0;
}

void SoftwareMonitor::PollOnce(std::vector<SoftwareRawEvent>& changes, int timeout_ms) {
    if (fan_fd_ < 0) {
        std::cerr << "fanotify descriptor is not initialized" << std::endl;
        return;
    }

    pollfd poll_fd{};
    poll_fd.fd = fan_fd_;
    poll_fd.events = POLLIN;

    const int poll_rc = poll(&poll_fd, 1, timeout_ms);

    if (poll_rc < 0) {
        const int saved_errno = errno;

        if (saved_errno == EINTR) {
            return;
        }

        std::cerr << "poll() failed: "
                  << std::strerror(saved_errno)
                  << std::endl;

        return;
    }

    if (poll_rc == 0) {
        return;
    }

    if (poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        std::cerr << "fanotify fd error, revents=0x"
                  << std::hex << poll_fd.revents
                  << std::dec << std::endl;
        return;
    }

    if ((poll_fd.revents & POLLIN) == 0) {
        return;
    }

    const FanotifyTrigger trigger = DrainFanotifyEvents();

    if (!trigger.triggered) {
        return;
    }

    auto current_snapshot = ReadInstalledSnapshot();

    if (!current_snapshot.ok) {
        std::cerr << "SoftwareMonitor: failed to read software snapshot after db change"
                  << std::endl;
        return;
    }

    const std::size_t old_size = changes.size();

    if (snapshot_initialized_) {
        DiffSnapshots(snapshot_, current_snapshot.packages, changes);
    } else {
        snapshot_initialized_ = true;
    }

    for (std::size_t i = old_size; i < changes.size(); ++i) {
        changes[i].pid = trigger.pid;
        changes[i].ppid = trigger.ppid;
    }

    snapshot_ = std::move(current_snapshot.packages);
}

SoftwareMonitor::FanotifyTrigger SoftwareMonitor::DrainFanotifyEvents() {
    FanotifyTrigger trigger;

    alignas(fanotify_event_metadata) char buffer[kBufSize];

    for (;;) {
        const ssize_t len = read(fan_fd_, buffer, sizeof(buffer));

        if (len < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                return trigger;
            }

            const int saved_errno = errno;

            std::cerr << "fanotify read error: "
                      << std::strerror(saved_errno)
                      << std::endl;

            return trigger;
        }

        if (len == 0) {
            return trigger;
        }

        auto* metadata = reinterpret_cast<fanotify_event_metadata*>(buffer);
        ssize_t remain = len;

        while (FAN_EVENT_OK(metadata, remain)) {
            if (metadata->vers != FANOTIFY_METADATA_VERSION) {
                std::cerr << "fanotify metadata version mismatch" << std::endl;
                return trigger;
            }

            const int event_fd = metadata->fd;

            if ((metadata->mask & FAN_Q_OVERFLOW) != 0) {
                std::cerr << "fanotify software db queue overflow" << std::endl;

                trigger.triggered = true;
                trigger.pid = metadata->pid;
                trigger.ppid = GetParentPid(metadata->pid);

                if (event_fd >= 0) {
                    close(event_fd);
                }

                metadata = FAN_EVENT_NEXT(metadata, remain);
                continue;
            }

            if (IsSoftwareDbEvent(metadata->mask)) {
                trigger.triggered = true;
                trigger.pid = metadata->pid;
                trigger.ppid = GetParentPid(metadata->pid);
            }

            if (event_fd >= 0) {
                close(event_fd);
            }

            metadata = FAN_EVENT_NEXT(metadata, remain);
        }
    }
}

SoftwareMonitor::SnapshotReadResult SoftwareMonitor::ReadInstalledSnapshot() const {
    SnapshotReadResult result;

    if (config_.enable_dpkg_snapshot) {
        const auto command_result = RunCommandLines(config_.dpkg_query_command);

        if (command_result.ok) {
            result.ok = true;

            for (const auto& line : command_result.lines) {
                const auto columns = Split(line, '\t');

                if (columns.size() < 3) {
                    continue;
                }

                const std::string& name = columns[0];
                const std::string& version = columns[1];
                const std::string& status = columns[2];

                /*
                 * "ii " означает:
                 * desired action = install,
                 * current status = installed.
                 */
                if (status.size() < 2 || status[0] != 'i' || status[1] != 'i') {
                    continue;
                }

                if (name.empty() || version.empty()) {
                    continue;
                }

                SoftwarePackageInfo info;
                info.name = name;
                info.version = version;

                result.packages[MakePackageKey(info)] = std::move(info);
            }
        }
    }

    if (config_.enable_rpm_snapshot) {
        const auto command_result = RunCommandLines(config_.rpm_query_command);

        if (command_result.ok) {
            result.ok = true;

            for (const auto& line : command_result.lines) {
                const auto columns = Split(line, '\t');

                if (columns.size() < 4) {
                    continue;
                }

                const std::string& name = columns[0];
                const std::string& epoch = columns[1];
                const std::string& rpm_version = columns[2];
                const std::string& release = columns[3];

                if (name.empty() || rpm_version.empty()) {
                    continue;
                }

                SoftwarePackageInfo info;
                info.name = name;
                info.version = BuildRpmVersion(epoch, rpm_version, release);

                result.packages[MakePackageKey(info)] = std::move(info);
            }
        }
    }

    return result;
}

void SoftwareMonitor::DiffSnapshots(const Snapshot& old_snapshot, const Snapshot& new_snapshot, std::vector<SoftwareRawEvent>& changes) const {
    std::vector<SoftwarePackageInfo> removed;
    std::vector<SoftwarePackageInfo> added;

    for (const auto& [key, old_info] : old_snapshot) {
        if (new_snapshot.find(key) == new_snapshot.end()) {
            removed.push_back(old_info);
        }
    }

    for (const auto& [key, new_info] : new_snapshot) {
        if (old_snapshot.find(key) == old_snapshot.end()) {
            added.push_back(new_info);
        }
    }

    std::vector<bool> removed_used(removed.size(), false);
    std::vector<bool> added_used(added.size(), false);

    /*
     * Если исчезла версия A и появилась версия B того же имени,
     * считаем это обновлением.
     */
    for (std::size_t i = 0; i < removed.size(); ++i) {
        for (std::size_t j = 0; j < added.size(); ++j) {
            if (added_used[j]) {
                continue;
            }

            if (MakeIdentityKey(removed[i]) != MakeIdentityKey(added[j])) {
                continue;
            }

            SoftwareRawEvent event;
            event.kind = SoftwareChangeKind::Updated;

            event.package_name = added[j].name;
            event.package_version = added[j].version;

            event.old_name = removed[i].name;
            event.old_version = removed[i].version;

            event.new_name = added[j].name;
            event.new_version = added[j].version;

            changes.push_back(std::move(event));

            removed_used[i] = true;
            added_used[j] = true;
            break;
        }
    }

    for (std::size_t i = 0; i < added.size(); ++i) {
        if (added_used[i]) {
            continue;
        }

        SoftwareRawEvent event;
        event.kind = SoftwareChangeKind::Installed;

        event.package_name = added[i].name;
        event.package_version = added[i].version;

        event.new_name = added[i].name;
        event.new_version = added[i].version;

        changes.push_back(std::move(event));
    }

    for (std::size_t i = 0; i < removed.size(); ++i) {
        if (removed_used[i]) {
            continue;
        }

        SoftwareRawEvent event;
        event.kind = SoftwareChangeKind::Removed;

        event.package_name = removed[i].name;
        event.package_version = removed[i].version;

        event.old_name = removed[i].name;
        event.old_version = removed[i].version;

        changes.push_back(std::move(event));
    }
}

std::string SoftwareMonitor::MakeIdentityKey(const SoftwarePackageInfo& info) {
    return info.name;
}

std::string SoftwareMonitor::MakePackageKey(const SoftwarePackageInfo& info) {
    return info.name + '\t' + info.version;
}

}  // namespace monitoring