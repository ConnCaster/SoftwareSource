#!/usr/bin/env bash
set -Eeuo pipefail

PKG="${PKG:-git}"
BACKEND="${BACKEND:-auto}"
WAIT_SECONDS="${WAIT_SECONDS:-2}"
REFRESH_METADATA="${REFRESH_METADATA:-1}"
ALLOW_REMOVE_PREINSTALLED="${ALLOW_REMOVE_PREINSTALLED:-0}"
MONITOR_CMD="${MONITOR_CMD:-}"
MONITOR_LOG="${MONITOR_LOG:-/tmp/software-monitor.log}"

MONITOR_PID=""

usage() {
    cat <<EOF
Usage:
  sudo $0 [options]

Options:
  --package NAME                  Package to test. Default: git
  --backend auto|dpkg|rpm         Package DB backend. Default: auto
  --wait SECONDS                  Wait after each package operation. Default: 2
  --no-refresh                    Do not refresh repository metadata
  --allow-remove-preinstalled     Allow removing package even if it was installed before the test
  --monitor-cmd 'COMMAND'         Start monitor command before package operations
  --monitor-log FILE              Monitor stdout/stderr log file
  -h, --help                      Show help

Examples:
  sudo $0

  sudo $0 --package git

  sudo $0 --backend rpm --package git

  sudo MONITOR_CMD='/opt/app/bin/software_monitor_test' \\
       MONITOR_LOG=/tmp/software-monitor.log \\
       $0 --package git --wait 3

Environment:
  PKG=git
  BACKEND=auto|dpkg|rpm
  WAIT_SECONDS=2
  REFRESH_METADATA=1|0
  ALLOW_REMOVE_PREINSTALLED=1|0
  MONITOR_CMD='/path/to/monitor'
  MONITOR_LOG=/tmp/software-monitor.log
EOF
}

log() {
    printf '[software-monitor-test] %s\n' "$*"
}

warn() {
    printf '[software-monitor-test] WARNING: %s\n' "$*" >&2
}

die() {
    printf '[software-monitor-test] ERROR: %s\n' "$*" >&2
    exit 1
}

have_cmd() {
    command -v "$1" >/dev/null 2>&1
}

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --package)
                [[ $# -ge 2 ]] || die "--package requires value"
                PKG="$2"
                shift 2
                ;;
            --backend)
                [[ $# -ge 2 ]] || die "--backend requires value"
                BACKEND="$2"
                shift 2
                ;;
            --wait)
                [[ $# -ge 2 ]] || die "--wait requires value"
                WAIT_SECONDS="$2"
                shift 2
                ;;
            --no-refresh)
                REFRESH_METADATA=0
                shift
                ;;
            --allow-remove-preinstalled)
                ALLOW_REMOVE_PREINSTALLED=1
                shift
                ;;
            --monitor-cmd)
                [[ $# -ge 2 ]] || die "--monitor-cmd requires value"
                MONITOR_CMD="$2"
                shift 2
                ;;
            --monitor-log)
                [[ $# -ge 2 ]] || die "--monitor-log requires value"
                MONITOR_LOG="$2"
                shift 2
                ;;
            -h|--help)
                usage
                exit 0
                ;;
            *)
                die "unknown argument: $1"
                ;;
        esac
    done
}

require_root() {
    [[ "${EUID}" -eq 0 ]] || die "run as root"
}

detect_backend() {
    if [[ "${BACKEND}" == "dpkg" || "${BACKEND}" == "rpm" ]]; then
        return
    fi

    if have_cmd dpkg-query && [[ -d /var/lib/dpkg ]]; then
        BACKEND="dpkg"
        return
    fi

    if have_cmd rpm && { [[ -d /var/lib/rpm ]] || [[ -d /usr/lib/sysimage/rpm ]]; }; then
        BACKEND="rpm"
        return
    fi

    die "cannot detect backend; use --backend dpkg or --backend rpm"
}

detect_rpm_pm() {
    if have_cmd dnf; then
        echo "dnf"
    elif have_cmd yum; then
        echo "yum"
    elif have_cmd apt-get; then
        echo "apt-get"
    else
        die "rpm backend detected, but no dnf/yum/apt-get found"
    fi
}

dpkg_is_installed() {
    dpkg-query -W -f='${Status}\n' "${PKG}" 2>/dev/null | grep -q '^install ok installed'
}

rpm_is_installed() {
    rpm -q "${PKG}" >/dev/null 2>&1
}

is_installed() {
    case "${BACKEND}" in
        dpkg) dpkg_is_installed ;;
        rpm)  rpm_is_installed ;;
        *)    die "unsupported backend: ${BACKEND}" ;;
    esac
}

dpkg_version() {
    dpkg-query -W -f='${Version}\n' "${PKG}" 2>/dev/null || true
}

rpm_version() {
    rpm -q --qf '%{EPOCH}:%{VERSION}-%{RELEASE}\n' "${PKG}" 2>/dev/null \
        | sed 's/^(none)://' \
        | sed 's/^0://' \
        || true
}

package_version() {
    case "${BACKEND}" in
        dpkg) dpkg_version ;;
        rpm)  rpm_version ;;
        *)    die "unsupported backend: ${BACKEND}" ;;
    esac
}

refresh_metadata() {
    [[ "${REFRESH_METADATA}" == "1" ]] || {
        log "repository metadata refresh skipped"
        return
    }

    log "refreshing repository metadata before monitor start"

    case "${BACKEND}" in
        dpkg)
            have_cmd apt-get || die "apt-get not found"
            apt-get update
            ;;
        rpm)
            local pm
            pm="$(detect_rpm_pm)"

            case "${pm}" in
                dnf)
                    dnf makecache -y
                    ;;
                yum)
                    yum makecache -y
                    ;;
                apt-get)
                    apt-get update
                    ;;
            esac
            ;;
    esac
}

install_package() {
    log "INSTALL step: ${PKG}"

    case "${BACKEND}" in
        dpkg)
            apt-get install -y "${PKG}"
            ;;
        rpm)
            local pm
            pm="$(detect_rpm_pm)"

            case "${pm}" in
                dnf)     dnf install -y "${PKG}" ;;
                yum)     yum install -y "${PKG}" ;;
                apt-get) apt-get install -y "${PKG}" ;;
            esac
            ;;
    esac
}

update_package() {
    log "UPDATE step: ${PKG}"

    case "${BACKEND}" in
        dpkg)
            apt-get install -y --only-upgrade "${PKG}" || apt-get install -y "${PKG}"
            ;;
        rpm)
            local pm
            pm="$(detect_rpm_pm)"

            case "${pm}" in
                dnf)     dnf upgrade -y "${PKG}" ;;
                yum)     yum update -y "${PKG}" ;;
                apt-get) apt-get install -y "${PKG}" ;;
            esac
            ;;
    esac
}

remove_package() {
    log "REMOVE step: ${PKG}"

    case "${BACKEND}" in
        dpkg)
            apt-get remove -y "${PKG}"
            ;;
        rpm)
            local pm
            pm="$(detect_rpm_pm)"

            case "${pm}" in
                dnf)     dnf remove -y "${PKG}" ;;
                yum)     yum remove -y "${PKG}" ;;
                apt-get) apt-get remove -y "${PKG}" ;;
            esac
            ;;
    esac
}

print_state() {
    if is_installed; then
        log "package state: installed, version=$(package_version)"
    else
        log "package state: not installed"
    fi
}

sleep_for_monitor() {
    sleep "${WAIT_SECONDS}"
}

start_monitor_if_requested() {
    [[ -n "${MONITOR_CMD}" ]] || return

    log "starting monitor command"
    log "monitor log: ${MONITOR_LOG}"

    if have_cmd setsid; then
        setsid bash -c "${MONITOR_CMD}" >"${MONITOR_LOG}" 2>&1 &
    else
        bash -c "${MONITOR_CMD}" >"${MONITOR_LOG}" 2>&1 &
    fi

    MONITOR_PID="$!"

    sleep 2

    if ! kill -0 "${MONITOR_PID}" >/dev/null 2>&1; then
        die "monitor command exited too early; see ${MONITOR_LOG}"
    fi
}

stop_monitor_if_started() {
    [[ -n "${MONITOR_PID}" ]] || return

    log "stopping monitor"

    if kill -0 "${MONITOR_PID}" >/dev/null 2>&1; then
        kill -INT "-${MONITOR_PID}" >/dev/null 2>&1 || kill -INT "${MONITOR_PID}" >/dev/null 2>&1 || true
        sleep 2
    fi

    if kill -0 "${MONITOR_PID}" >/dev/null 2>&1; then
        kill -TERM "-${MONITOR_PID}" >/dev/null 2>&1 || kill -TERM "${MONITOR_PID}" >/dev/null 2>&1 || true
        sleep 1
    fi
}

cleanup_on_exit() {
    stop_monitor_if_started || true
}

main() {
    parse_args "$@"
    require_root
    detect_backend

    trap cleanup_on_exit EXIT

    log "backend: ${BACKEND}"
    log "package: ${PKG}"

    local was_installed=0
    local version_before=""
    local version_after=""

    if is_installed; then
        was_installed=1
        version_before="$(package_version)"
        log "initial state: installed, version=${version_before}"
    else
        log "initial state: not installed"
    fi

    refresh_metadata

    log "expected monitor behavior:"
    if [[ "${was_installed}" == "0" ]]; then
        log "  install: should produce Installed for ${PKG}"
        log "  update: will produce Updated only if package manager changes version after install"
        log "  remove: should produce Removed for ${PKG}"
    else
        log "  install: skipped, package already installed"
        log "  update: should produce Updated only if repository has newer version"
        if [[ "${ALLOW_REMOVE_PREINSTALLED}" == "1" ]]; then
            log "  remove: allowed, should produce Removed for ${PKG}"
        else
            log "  remove: skipped to avoid deleting preinstalled package"
        fi
    fi

    start_monitor_if_requested

    print_state
    sleep_for_monitor

    if [[ "${was_installed}" == "0" ]]; then
        install_package
        print_state
        sleep_for_monitor
    else
        log "INSTALL step skipped: ${PKG} was installed before test"
    fi

    if is_installed; then
        version_before="$(package_version)"
        update_package
        version_after="$(package_version)"

        if [[ "${version_before}" != "${version_after}" ]]; then
            log "UPDATE result: version changed ${version_before} -> ${version_after}"
            log "monitor should report Updated"
        else
            log "UPDATE result: version did not change"
            log "monitor should not report Updated because snapshot is unchanged"
        fi

        print_state
        sleep_for_monitor
    else
        warn "UPDATE step skipped: ${PKG} is not installed"
    fi

    if [[ "${was_installed}" == "0" || "${ALLOW_REMOVE_PREINSTALLED}" == "1" ]]; then
        if is_installed; then
            remove_package
            print_state
            sleep_for_monitor
        else
            warn "REMOVE step skipped: ${PKG} is already absent"
        fi
    else
        log "REMOVE step skipped: ${PKG} was installed before test"
        log "use --allow-remove-preinstalled if you intentionally want to remove it"
    fi

    log "test sequence completed"
}

main "$@"