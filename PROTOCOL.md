# Rootfs bundle protocol v1

The `.arlinux-rootfs` file is a ZIP64 archive with uncompressed members.
`manifest.json` contains `protocol: 1`, a stable lowercase `distributionId`,
display `name`, `hostPackage: io.taowen.arlinux`, a compositor *hint*
(`anlabwc` or `hyprland`), relative `requiredFiles` and `libraryDirectories`,
and a SHA-256 map of all payload members. The loader rejects unknown protocol
versions, unsafe paths, duplicate ZIP names, missing members and hash mismatches
before modifying an instance. No APK package ID belongs to a distribution.

Required members: `rootfs.tar.zst`, `gpu-qualcomm.tar.zst`, `xterm.json`,
`bionicx/lib/{ld-linux-aarch64.so.1,libc.so.6,libm.so.6,ldconfig,libbionicx-runtime.so}`
and `bionicx/sudo`. The launcher/profile schema remains version 3.
`rootfs.tar.zst` supplies `/usr/lib/arlinux/guest/first-boot.sh` and the
distribution package database; Android supplies the display, input, audio,
AT-SPI transport, sockets and guest execution boundary. Future incompatible
changes increment `protocol`.

`first-boot.sh` may exit with status 75 after replacing libc, the dynamic
loader, or other process-runtime packages. The host must start it again at a
fresh guest execution boundary. Other non-zero statuses are failures; the host
limits restart requests to prevent a broken distribution from looping.

One Android installation holds multiple instances. An instance ID is distinct
from `distributionId`. Its persistent state lives in
`files/instances/<instance-id>/{rootfs,home}` and a recorded bundle digest;
creating another instance of the same distribution creates new state. The
glibc patch currently requires the fixed absolute path
`/data/user/0/io.taowen.arlinux/files/rootfs`. The host makes `files/rootfs`
an alias to the selected instance before launching the guest. Only one
instance runs at a time: switching stops guest processes and the compositor,
changes the alias atomically, clears ephemeral sockets and starts a fresh
session. Never switch the alias while a guest process is alive. Profile and
home must also be instance-specific; cache bundles separately from instance
state. Bundle replacement must stage and verify first, never wipe an existing
instance merely because a new bundle was imported.

Compositor choice is a host runtime setting, defaulting to the bundle hint;
the APK must package both anlabwc and anhyprland. A Linux distribution cannot
load Android libraries, change the host's package name, or request Android
privileges through this protocol. Versioned checksums provide corruption
protection, not publisher authentication; external distribution sources need
a separate trusted signing policy before automatic download/update.
