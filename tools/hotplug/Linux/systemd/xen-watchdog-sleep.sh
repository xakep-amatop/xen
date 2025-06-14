#!/bin/sh

# The first argument ($1) is:
#     "pre" or "post"
# The second argument ($2) is:
#     "suspend", "hibernate", "hybrid-sleep", or "suspend-then-hibernate"

. /etc/xen/scripts/hotplugpath.sh

SERVICE_NAME="xen-watchdog.service"
STATE_FILE="/run/xen-watchdog-sleep-marker"
XEN_WATCHDOG_SLEEP_LOG="${XEN_LOG_DIR}/xen-watchdog-sleep.log"

log_watchdog() {
    echo "$1"
    echo "$(date): $1" >> "${XEN_WATCHDOG_SLEEP_LOG}"
}

# Exit silently if Xen watchdog service is not present
if ! systemctl show "${SERVICE_NAME}" > /dev/null 2>&1; then
    exit 0
fi

case "$1" in
pre)
    if systemctl is-active --quiet "${SERVICE_NAME}"; then
        touch "${STATE_FILE}"
        log_watchdog "Stopping ${SERVICE_NAME} before $2."
        systemctl stop "${SERVICE_NAME}"
    fi
    ;;
post)
    if [ -f "${STATE_FILE}" ]; then
        log_watchdog "Starting ${SERVICE_NAME} after $2."
        systemctl start "${SERVICE_NAME}"
        rm "${STATE_FILE}"
    fi
    ;;
*)
    log_watchdog "Script called with unknown action '$1'. Arguments: '$@'"
    exit 1
    ;;
esac

exit 0
