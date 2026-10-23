#!/usr/bin/env bash
set -euo pipefail

# NET_ADMIN is used only to create an interface inside this container's network.
# Kernel modules belong to the Docker host, never to this Ubuntu filesystem.
if [[ "${ZEPHYRECU_SETUP_VCAN:-0}" == 1 ]]; then
    if ! ip link show dev vcan0 >/dev/null 2>&1; then
        if ! ip link add dev vcan0 type vcan; then
            echo 'Cannot create vcan0. Load vcan on the Docker host and grant this container NET_ADMIN.' >&2
            exit 1
        fi
    fi
    ip link set dev vcan0 up
fi

ci_uid="${ZEPHYRECU_UID:-0}"
ci_gid="${ZEPHYRECU_GID:-0}"
if [[ ! "$ci_uid" =~ ^[0-9]+$ || ! "$ci_gid" =~ ^[0-9]+$ ]]; then
    echo 'ZEPHYRECU_UID and ZEPHYRECU_GID must be numeric.' >&2
    exit 2
fi

# Build as the invoking host user so bind-mounted artifacts remain editable.
ci_home=/tmp/zephyrecu-ci-home
mkdir -p "$ci_home"
chown "$ci_uid:$ci_gid" "$ci_home"
exec setpriv --reuid="$ci_uid" --regid="$ci_gid" --clear-groups \
    env HOME="$ci_home" "$@"
