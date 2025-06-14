#!/bin/sh

# The first argument ($1) is:
#     "pre" or "post"
# The second argument ($2) is:
#     "suspend", "hibernate", "hybrid-sleep", or "suspend-then-hibernate"

SYSTEMCTL="/usr/bin/systemctl"
SERVICE_NAME="xen-watchdog.service"

XEN_WATCHDOG_SLEEP_LOG="${XEN_LOG_DIR}/xen-watchdog-sleep.log"

case "$1" in
pre)
    echo "$(date): Stopping ${SERVICE_NAME} before $2." | tee -a "${XEN_WATCHDOG_SLEEP_LOG}"
    "${SYSTEMCTL}" stop "${SERVICE_NAME}"
    ;;
post)
    echo "$(date): Starting ${SERVICE_NAME} after $2." | tee -a "${XEN_WATCHDOG_SLEEP_LOG}"
    "${SYSTEMCTL}" start "${SERVICE_NAME}"
    ;;
*)
    echo "$(date): Script called with unknown action '$1'. Arguments: '$@'" | tee -a "${XEN_WATCHDOG_SLEEP_LOG}"
    exit 1
    ;;
esac

exit 0
