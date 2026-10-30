# Container test environment

The container provides the Linux userspace tools for Zephyr `native_sim` tests:
Ubuntu 24.04, Python 3.12, west, Twister dependencies and GCC with 32-bit support.
It targets Linux x86_64 and does not require a Zephyr SDK installation on the host.

## Run

Install Docker Engine and ensure your user can run Docker commands. Keep the
repository named `ZephyrECU` inside a dedicated workspace directory:

```text
workspace/
  ZephyrECU/
```

From the repository, run:

```sh
./tools/run_docker.sh
```

The script builds or reuses cached layers of `zephyrecu-ci`, mounts the parent
workspace at `/workspace`, and runs `tools/run_ci.sh`. It also works when invoked
by absolute path from another directory. The first run in a new workspace uses
`west init` and `west update` to download the dependencies alongside `ZephyrECU`.
Allow network access and disk space for the Zephyr checkout and modules.
An existing workspace is checked against `west-frozen.yml`; updates are explicit.

```sh
./tools/run_docker.sh --update          # Resolve the checked-in west manifest
./tools/run_docker.sh --unit-only       # Codegen + Twister; no CAN setup or NET_ADMIN
./tools/run_docker.sh --timing strict   # Also enforce host UDS latency limits
./tools/run_docker.sh --help
```

The default run checks DBC generation and its host codec tests, executes every
native_sim Twister suite found below `tests/`, the raw CAN and can_comm integration
testers, the UDS integration tester, and the application smoke test. A failure
stops the run and returns a nonzero exit status. The smoke
test checks the application readiness log as well as process termination.

UDS timing defaults to `report`: functional assertions remain mandatory, while
host latency and worker poll gaps are reported. The deterministic pacing
regression in ztest always runs. Use `--timing strict` when testing host latency
under controlled load.

## Virtual CAN and permissions

The integration tests need vcan and kernel CAN ISO-TP support on the Docker host.
On a Linux host with these features available as modules, load them if needed:

```sh
sudo modprobe vcan
sudo modprobe can-isotp
```

The launcher grants `NET_ADMIN` to create and bring up `vcan0` inside the
container's own network namespace. It does not use host networking. An existing
host `vcan0` is independent and need not be up. Container removal also removes
the container interface. Separate runs have separate CAN networks.

After network setup, the entrypoint switches to the invoking user's UID/GID.
Build files remain editable by that user on the host. `--unit-only` skips CAN
setup and does not add `NET_ADMIN`.

The container shares the host kernel: installing Ubuntu packages in this image
cannot supply a missing host CAN kernel module. On Docker Desktop the relevant
kernel belongs to its Linux VM. The setup has been validated on Linux x86_64;
other host architectures and VM kernels require separate validation.

## Results and native execution

Docker writes to the workspace's `build/docker/`, separate from native builds
whose CMake caches contain different absolute paths. Results include:

- `twister/`: Twister reports and test logs.
- `logs/`: build and runtime logs, the resolved west manifest, CAN interface
  details, image ID, tool versions and Python package versions.
- `can_if_socketcan/`, `can_comm_socketcan/`, `diag_uds_socketcan/`, `native_sim/`: integration and
  application build directories.

The workspace is bind-mounted, so local source edits are tested immediately.
The container is removed after the run; source, dependencies and build outputs
stay on the host. Later runs replace stage logs and Twister rotates its output.
Copy results elsewhere before another run if you need to retain them.

With an already configured native environment and an active Python venv, the
same orchestrator can run without Docker:

```sh
./tools/run_ci.sh
```

This uses `build/ci/` by default; set `CI_BUILD_DIR` to change it. Native execution
expects an already configured host `vcan0` for integration tests. See [Testing](testing.md#virtual-test-environment)
for native package and interface setup.

## Dependency pinning

`Dockerfile.ci` pins the Ubuntu base by digest. `tools/requirements-ci.lock` pins
west and all resolved Python dependencies for Python 3.12/Linux amd64.
`west-frozen.yml` records the expected source revisions, and every run checks it
against the actual workspace. The image contains tools; west source checkouts
are kept outside it in the mounted workspace.

The launcher always invokes a cached Docker build to keep the image consistent
with the Dockerfile, lock file and entrypoint. The build context excludes the
application tree and build artifacts through `.dockerignore`. To use a different
local image tag, set `ZEPHYRECU_CI_IMAGE` before launching.

APT repositories are not snapshotted, so a later rebuild without cached layers
can select newer system packages. To share an identical userspace environment,
distribute the already built image by registry digest or Docker save/load and
record its image ID. The launcher records the local image ID in the run logs.
Identical userspace does not guarantee identical scheduling or kernel behavior.

Update the Python lock deliberately when changing Zephyr or testing dependency
updates. Resolve into a fresh Python 3.12 environment from the three pinned
Zephyr requirement files, `tools/requirements-codegen.txt` and west, then review
the resulting versions.
For example, from the repository after the image and west workspace exist:

```sh
docker run --rm \
  --mount "type=bind,src=$(realpath ..),dst=/workspace,readonly" \
  zephyrecu-ci bash -c '
    python3 -m venv /tmp/resolve &&
    /tmp/resolve/bin/pip install west \
      -r /workspace/zephyr/scripts/requirements-base.txt \
      -r /workspace/zephyr/scripts/requirements-build-test.txt \
      -r /workspace/zephyr/scripts/requirements-run-test.txt \
      -r /workspace/ZephyrECU/tools/requirements-codegen.txt >&2 &&
    /tmp/resolve/bin/pip check >&2 &&
    /tmp/resolve/bin/pip freeze
  ' > /tmp/requirements-ci.lock
```

Review `/tmp/requirements-ci.lock` before replacing `tools/requirements-ci.lock`,
then rerun the full container tests. This command deliberately resolves current
compatible Python versions; normal test runs install only the checked-in lock.
