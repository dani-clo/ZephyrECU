#!/usr/bin/env bash
set -euo pipefail

ci_repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
ci_workspace="$(dirname -- "$ci_repo_dir")"
ci_image="${ZEPHYRECU_CI_IMAGE:-zephyrecu-ci}"

if [[ "$(basename -- "$ci_repo_dir")" != ZephyrECU ]]; then
    echo 'The west T2 manifest repository must be named ZephyrECU inside its workspace.' >&2
    exit 2
fi

if [[ "${1:-}" == --help ]]; then
    exec bash "$ci_repo_dir/tools/run_ci.sh" --help
fi

ci_setup_vcan=1
for ci_arg in "$@"; do
    if [[ "$ci_arg" == --unit-only ]]; then
        ci_setup_vcan=0
    fi
done

# Cached rebuilds keep the entrypoint and locked dependencies in sync with source.
docker build -f "$ci_repo_dir/Dockerfile.ci" -t "$ci_image" "$ci_repo_dir"
ci_image_id="$(docker image inspect "$ci_image" --format '{{.Id}}')"
ci_docker_args=(run --rm --init
    --mount "type=bind,src=$ci_workspace,dst=/workspace"
    --workdir /workspace
    --env "ZEPHYRECU_UID=$(id -u)"
    --env "ZEPHYRECU_GID=$(id -g)"
    --env "ZEPHYRECU_SETUP_VCAN=$ci_setup_vcan"
    --env "ZEPHYRECU_IMAGE_ID=$ci_image_id"
    --env CI_BUILD_DIR=/workspace/build/docker)
if (( ci_setup_vcan )); then
    ci_docker_args+=(--cap-add NET_ADMIN)
fi
exec docker "${ci_docker_args[@]}" "$ci_image" \
    /workspace/ZephyrECU/tools/run_ci.sh "$@"
