# OVMX k3s rail — offload heavy builds/tests

**Build/test tooling only.** Per CLAUDE.md Rule 9 the OVMX runtime is the
kernel/QEMU path; k3s here is *only* a place to run `cmake`/`ctest`/
`tests/qemu/run_tests.sh` and QEMU-KVM smokes on cluster hardware instead of
the shared `workshop` host. This is never an OVMX runtime and must not be
presented as one.

## What's here

| File | Purpose |
|------|---------|
| `run-on-rail.sh` | Run a command in the repo at a git-ref, in a pod on k3s-worker; stream logs; exit with the command's exit code. |
| `Dockerfile.rail` | The builder image (ubuntu:24.04 + the tests/qemu toolchain + git + genisoimage). |
| `namespace.yaml` | `ovmx-ci` namespace + ResourceQuota (~6 CPU / 40Gi) + LimitRange. |
| `job-template.yaml` | Job manifest `run-on-rail.sh` renders per run. |
| `kvm-smoke.sh` | Proves `/dev/kvm` passthrough / KVM acceleration in-pod. |

## Usage

```bash
export KUBECONFIG=~/.kube/config          # context: default

# Full build + ctest on cluster hardware:
tools/k3s/run-on-rail.sh main \
  "cmake -B build -DBUILD_TESTS=ON -DBUILD_TOOLS=ON && \
   cmake --build build -j\$(nproc) && cd build && ctest --output-on-failure"

# Prove KVM passthrough:
tools/k3s/run-on-rail.sh main "bash tools/k3s/kvm-smoke.sh"

# Any ref (branch/tag/SHA); --keep leaves the Job for debugging:
tools/k3s/run-on-rail.sh --keep work/my-branch "cmake --build build -j\$(nproc)"
```

The script exits with the command's exit code, so it drops straight into CI or
a shell `&&` chain.

### Flags / env knobs

- `--keep` — don't delete the Job when it finishes.
- `--name NAME` — base name for the Job (default `ovmx-ci`).
- `OVMX_RAIL_IMAGE` — override the builder image ref.
- `OVMX_RAIL_REQ_CPU` / `_REQ_MEM` / `_LIM_CPU` / `_LIM_MEM` — resources.
- `OVMX_RAIL_DEADLINE` — wall cap seconds (default 3600).
- `OVMX_REPO_URL` — clone URL (default `https://github.com/3dl-dev/vms`, public).

## Cluster facts (verified)

- **Registry:** `gpu-rail-registry` (default ns), NodePort **30500** → 5000,
  ClusterIP `10.43.10.176`. Insecure HTTP registry, trusted cluster-wide.
  - **Push from workshop:** via a node NodePort, e.g.
    `192.168.2.44:30500` (k3s-worker) or `192.168.2.43:30500` (k3s-cp).
  - **Pull in-cluster:** the working ref is
    **`gpu-rail-registry.default.svc.cluster.local:5000/ovmx-builder:latest`**
    (see "Registry notes" below for why this one and not the NodePort).
- **Target node:** `k3s-worker` (192.168.2.44, 8 CPU / 53 GB, KVM-capable bare
  metal). The Job pins `nodeSelector kubernetes.io/hostname=k3s-worker`. It
  never targets k3s-cp (small), k3s-mini (NotReady), or the GPU node.
- **KVM:** `/dev/kvm` is passed through with a privileged pod + hostPath
  `/dev/kvm` (`type: CharDevice`). Confirmed working — `kvm-smoke.sh` boots a
  real kernel under strict `-accel kvm` and asserts the guest logs
  `Hypervisor detected: KVM`.

## Building / pushing the builder image (one-time bootstrap)

This local `docker build` on workshop is the only local-heavy step; it's
acceptable as a one-time bootstrap.

```bash
# from the repo root on workshop:
docker build -f tools/k3s/Dockerfile.rail -t 192.168.2.44:30500/ovmx-builder:latest .
docker push 192.168.2.44:30500/ovmx-builder:latest
```

### Registry notes (the parts that fight you)

- The registry serves **HTTP** (insecure). Workshop's Docker already pushes to
  it (other tenants do), and the k3s nodes' containerd already trusts it (they
  pull other tenants' images from it), so **no per-node TLS/insecure config was
  needed** for either push or pull.
- **Push tag vs. pull ref differ on purpose.** You push to a *NodePort*
  (`192.168.2.44:30500`) because that's reachable from workshop. Pods pull with
  the *in-cluster* Service DNS name
  (`gpu-rail-registry.default.svc.cluster.local:5000`), which resolves to the
  Service ClusterIP `10.43.10.176:5000`. Both names address the same registry
  storage, so an image pushed at the NodePort tag is pullable at the in-cluster
  tag — they are the same blob under two names. If DNS ever misbehaves, the
  ClusterIP form `10.43.10.176:5000/ovmx-builder:latest` works identically
  (set `OVMX_RAIL_IMAGE`).
- `imagePullPolicy: Always` — the tag is `:latest` and mutable; always re-pull
  so a rebuilt builder image is picked up.

## How exit codes / streaming work

The pod clones the repo at the ref, decodes the caller's command (passed
base64 so arbitrary shell never touches the YAML), and runs it. The command's
exit status is the container's exit status. `run-on-rail.sh` streams
`kubectl logs -f`, waits for the Job to reach complete/failed, reads the
container's `terminated.exitCode`, and re-exits with it. A failed command sets
`backoffLimit: 0` so it is never retried — the first exit code is the verdict.
