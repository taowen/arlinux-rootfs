# Static bundle protocol v2

This document defines the persistent, host-independent contract of an
`.zip` distribution bundle. Runtime communication between the
installed guest and the Android host is defined separately in
[RUNTIME-PROTOCOL.md](RUNTIME-PROTOCOL.md).

## Archive

A bundle is a ZIP64 archive whose payload members are stored without ZIP
compression. `manifest.json` is the only top-level metadata source. It contains:

- `protocol`: the integer `2`;
- `distributionId`: a stable `[a-z][a-z0-9-]{0,63}` identifier;
- `name`: the display name;
- `hostPackage`: `io.taowen.arlinux`;
- `compositor`: the default host compositor hint, `anlabwc` or `hyprland`;
- `requiredFiles`: rootfs-relative paths that must exist before installation;
- `libraryDirectories`: rootfs-relative dynamic-library directories;
- `files`: every payload member mapped to its lowercase SHA-256 digest.

The host rejects an unsupported protocol version, unsafe or duplicate member
names, undeclared members, missing required members, and digest mismatches
before modifying an instance. Checksums protect against corruption; they do not
authenticate a publisher. A distribution catalog therefore needs its own trust
and signing policy.

## Required payloads

Protocol 2 requires:

```text
rootfs.tar.zst
rootfs-seed-id
gpu-qualcomm.tar.zst
gpu-qualcomm-id
gpu-generic.tar.zst
gpu-generic-id
guest.properties
profile.json
bionicx/lib/ld-linux-aarch64.so.1
bionicx/lib/libc.so.6
bionicx/lib/libm.so.6
bionicx/lib/ldconfig
bionicx/sudo
```

`rootfs.tar.zst` contains the distribution-owned AArch64 userspace and
`/usr/lib/arlinux/guest/first-boot.sh`. `profile.json` follows launch-profile
schema 3. The two GPU overlays let the host select Qualcomm Turnip or the
generic Android Vulkan/libhybris path without requiring device-specific
distribution builds.

An incompatible archive or manifest change increments the static `protocol`
number. Runtime services can evolve independently according to the negotiation
rules in the runtime protocol.

## Installation and instances

One Android installation may contain multiple instances, including multiple
instances of the same distribution. An instance ID is not a distribution ID.
Persistent data is stored as:

```text
files/instances/<instance-id>/rootfs
files/instances/<instance-id>/home
files/instances/<instance-id>/profiles
files/instances/<instance-id>/instance.json
```

Bundles are cached independently from instance state. Importing a replacement
bundle must be staged and verified before use and must not erase an existing
instance. `instance.json` records the distribution identity, display name,
compositor hint, and digest-named bundle directory used by the instance.

The compatibility runtime currently requires the fixed absolute guest path
`/data/user/0/io.taowen.arlinux/files/rootfs`. Before launch, the host makes
`files/rootfs` refer to the selected instance. Only one instance runs at a time.
Switching instances stops guest processes and the compositor, changes that
alias atomically, clears ephemeral runtime state, and then starts a new session.
The alias must never change while a guest process is alive.

Home, launch profile, and other mutable state are instance-specific. A bundle
must not contain an Android application ID other than the protocol's
`hostPackage`, request Android privileges, or load Android libraries directly.

## First boot

The host runs `/usr/lib/arlinux/guest/first-boot.sh` after installation. The
script must be idempotent. It may exit with status `75` after replacing libc,
the dynamic loader, or another process-runtime package; the host then runs it
again at a fresh guest execution boundary. Any other non-zero status is a
failure, and the host limits restart requests to prevent loops.

Compositor selection is a host setting and defaults to the manifest hint. The
Android application supplies both anlabwc and anhyprland, so a bundle owns
desktop policy but does not package or start another compositor.
