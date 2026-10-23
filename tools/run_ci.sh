#!/usr/bin/env bash
set -euo pipefail

ci_unit_only=0
ci_update=0
ci_timing=report
while (( $# )); do
    case "$1" in
        --unit-only) ci_unit_only=1; shift ;;
        --update) ci_update=1; shift ;;
        --timing)
            if (( $# < 2 )) || [[ "$2" != strict && "$2" != report ]]; then
                echo '--timing requires strict or report.' >&2
                exit 2
            fi
            ci_timing="$2"; shift 2 ;;
        --help)
            cat <<'HELP'
Usage: tools/run_ci.sh [--unit-only] [--update] [--timing strict|report]
       tools/run_docker.sh [the same options]

Run every native_sim Twister suite discovered below tests/. By default also run
raw CAN and UDS integration tests and the application smoke test on vcan0.
--unit-only skips SocketCAN, requiring no vcan interface or NET_ADMIN.
--update resolves west dependencies; a fresh workspace always initializes them.
--timing defaults to report for host timing; functional checks always apply.
CI_BUILD_DIR overrides the native build/ci output directory (Docker: build/docker).
Run the Docker launcher from any directory; no host Python/Zephyr setup is needed.
HELP
            exit 0 ;;
        *) printf 'Unknown option: %s\n' "$1" >&2; exit 2 ;;
    esac
done

ci_repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
ci_workspace="$(dirname -- "$ci_repo_dir")"
if [[ "$(basename -- "$ci_repo_dir")" != ZephyrECU ]]; then
    echo 'The west manifest repository must be named ZephyrECU.' >&2
    exit 2
fi
cd "$ci_workspace"
export ZEPHYR_TOOLCHAIN_VARIANT=host
ci_build_dir="${CI_BUILD_DIR:-$ci_workspace/build/ci}"
mkdir -p "$ci_build_dir/logs"
ci_build_dir="$(cd "$ci_build_dir" && pwd -P)"
ci_log_dir="$ci_build_dir/logs"
ci_stage=setup
report_exit() {
    local ci_status=$?
    if (( ci_status != 0 )); then
        printf 'CI failed during %s (exit %s). Logs: %s\n' \
            "$ci_stage" "$ci_status" "$ci_log_dir" >&2
    fi
}
trap report_exit EXIT

run_logged() {
    local ci_log_name="$1"
    shift
    "$@" 2>&1 | tee "$ci_log_dir/$ci_log_name.log"
}

if [[ ! -f .west/config ]]; then
    run_logged west-init west init -l "$ci_repo_dir"
    ci_update=1
fi
if [[ "$(west topdir)" != "$ci_workspace" ||
      "$(realpath "$(west config manifest.path)")" != "$ci_repo_dir" ]]; then
    echo 'This directory belongs to a different west manifest workspace.' >&2
    exit 1
fi
if (( ci_update )); then
    run_logged west-update west update --narrow -o=--depth=1
fi
west manifest --validate
west manifest --freeze > "$ci_log_dir/west-frozen.yml"
if ! diff -u "$ci_repo_dir/west-frozen.yml" "$ci_log_dir/west-frozen.yml"; then
    echo 'Dependency revisions do not match west-frozen.yml. Run with --update or review the manifest.' >&2
    exit 1
fi
{
    printf 'Image: %s\n' "${ZEPHYRECU_IMAGE_ID:-native host}"
    printf 'User: %s:%s\n' "$(id -u)" "$(id -g)"
    uname -a
    python --version
    west --version
    cmake --version
    gcc --version
    python -m pip freeze
} > "$ci_log_dir/environment.txt"
python -m pip check

ci_stage=twister
run_logged twister python "$ci_workspace/zephyr/scripts/twister" \
    -T "$ci_repo_dir/tests" -p native_sim \
    --outdir "$ci_build_dir/twister" --inline-logs

if (( ci_unit_only )); then
    printf 'PASS: all native_sim Twister suites. Results: %s\n' "$ci_build_dir"
    exit 0
fi

ci_stage=socketcan-preflight
python - <<'PY'
import socket
try:
    with socket.socket(socket.PF_CAN, socket.SOCK_RAW, socket.CAN_RAW) as raw:
        raw.bind(("vcan0",))
    with socket.socket(socket.PF_CAN, socket.SOCK_DGRAM, socket.CAN_ISOTP) as tester:
        tester.bind(("vcan0", 0x7e8, 0x7e0))
except OSError as error:
    raise SystemExit(f"SocketCAN preflight failed: {error}. Bring up vcan0 and enable vcan/can-isotp on the host.")
print("SocketCAN and kernel ISO-TP available")
PY
ip -details link show vcan0 > "$ci_log_dir/vcan.txt"

ci_stage=can-integration
run_logged can-build west build -b native_sim -S socketcan-native-sim \
    "$ci_repo_dir/tests/can_if_socketcan" -d "$ci_build_dir/can_if_socketcan"
run_logged can-integration timeout 90s python "$ci_repo_dir/tools/test_can_if_socketcan.py" \
    "$ci_build_dir/can_if_socketcan/zephyr/zephyr.exe" --interface vcan0

ci_stage=uds-integration
run_logged uds-build west build -b native_sim -S socketcan-native-sim \
    "$ci_repo_dir/tests/diag_uds_socketcan" -d "$ci_build_dir/diag_uds_socketcan"
run_logged uds-integration timeout 120s python "$ci_repo_dir/tools/test_uds_socketcan.py" \
    "$ci_build_dir/diag_uds_socketcan/zephyr/zephyr.exe" --interface vcan0 --timing "$ci_timing"

ci_stage=application-smoke
run_logged application-build west build -b native_sim -S socketcan-native-sim \
    "$ci_repo_dir" -d "$ci_build_dir/native_sim"
run_logged application-smoke timeout 15s "$ci_build_dir/native_sim/zephyr/zephyr.exe" \
    --can-if=vcan0 -rt -stop_at=1
grep -Fq 'ZephyrECU CAN and diagnostics ready' "$ci_log_dir/application-smoke.log"
printf 'PASS: all native_sim unit/integration/smoke tests. Results: %s\n' "$ci_build_dir"
