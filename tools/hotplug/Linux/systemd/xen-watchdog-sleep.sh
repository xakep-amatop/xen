#!/bin/sh

# The first argument ($1) is:
#     "pre" or "post"
# The second argument ($2) is:
#     "suspend", "hibernate", "hybrid-sleep", or "suspend-then-hibernate"

. /etc/xen/scripts/hotplugpath.sh

SERVICE_NAME="xen-watchdog.service"
STATE_FILE="/run/xen-watchdog-sleep-marker"
XEN_WATCHDOG_SLEEP_LOG="${XEN_LOG_DIR}/xen-watchdog-sleep.log"

# Exit silently if Xen watchdog service is not present
if ! systemctl show "${SERVICE_NAME}" > /dev/null 2>&1; then
    exit 0
fi

case "$1" in
pre)
    if systemctl is-active --quiet "${SERVICE_NAME}"; then
        touch "${STATE_FILE}"
        echo "$(date): Stopping ${SERVICE_NAME} before $2." | tee -a "${XEN_WATCHDOG_SLEEP_LOG}"
        systemctl stop "${SERVICE_NAME}"
    fi
    ;;
post)
    if [ -f "${STATE_FILE}" ]; then
        echo "$(date): Starting ${SERVICE_NAME} after $2." | tee -a "${XEN_WATCHDOG_SLEEP_LOG}"
        systemctl start "${SERVICE_NAME}"
        rm "${STATE_FILE}"
    fi
    ;;
*)
    echo "$(date): Script called with unknown action '$1'. Arguments: '$@'" | tee -a "${XEN_WATCHDOG_SLEEP_LOG}"
    exit 1
    ;;
esac

exit 0
