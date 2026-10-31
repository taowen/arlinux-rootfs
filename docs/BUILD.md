# Distribution build and release

This repository builds Linux distributions and their native Linux GPU libraries.
It has no Bionic Wine source, build graph, assembly or publishing pipeline.
Application launchers download the independently published runtime when needed. Guest Tools bootstraps only the independently
published Wine installer; Wine owns subsequent component selection, downloads
and prefix installation. The distribution does not maintain a Wine version list.
`runtime/tools/component_store.py` provides the common verified release/candidate
storage used by the bootstrap and installer. The installer host protocol is owned
by the [Wine guide](https://github.com/taowen/wine/blob/arlinux/arlinux/BUILD.md#independent-runtime-producers).

The `third_party/libhybris` submodule follows libhybris's single `arlinux`
integration branch, shared with Wine. Keep the two consumers pinned to the same
commit. Linux glibc and Android Bionic retain independent build modes and
artifacts; see the [component ownership guide](https://github.com/taowen/libhybris/blob/arlinux/arlinux/BUILD.md).

## Wayland input adapter

The `linux-ibus` component builds the checksum-pinned, unmodified upstream IBus
release in `tools/build/ibus-component.py` on the ARM64 runner. All distributions
consume its verified runtime tree as a rootfs dependency. The adapter and its
matching client library live under `/usr/lib/arlinux/ibus`; distribution-owned
IBus daemons, toolkit modules and Python GI bindings remain in place. Source and
output hashes govern its independent cache and component capsule.

The session uses this adapter with the hosted engine and the system keyboard
layout. Older IBus Wayland adapters commit unhandled printable keys as text,
which prevents games from receiving physical movement keys. The upstream adapter
also implements forwarding synthesized key events through input-method-v2.
Do not bypass this by disabling input methods for individual applications.

## ARLinux Guest Tools

First-party application integration is a separate `guest-tools` product, not part
of the Linux image or Bionic Wine. `build.sh guest-tools` packages the Python
launchers, download policies, application entries/icons, hosted IBus service and
OpenCode integration (the `arlinux` helper package, instructions and plugin) into `build/artifacts/guest-tools/guest-tools.zip` with a hashed file/mode manifest
and `guest-tools.json` receipt. It runs in this repository's independent ARM64
workflow; it neither builds GPU libraries nor prepares distribution packages.
The canonical component graph requests it alongside, not as a dependency of,
the selected rootfs. `--product rootfs` returns both references in `products.json`.
Each product has its own content identity, publication, capsule and download cache.
Changes to these application scripts do not invalidate the base rootfs.
The OpenCode launcher rewrites its managed `AGENTS.md` section on every start and
appends `/etc/arlinux/opencode-AGENTS.md` when a distribution provides one.
Shared agent guidance lives in `runtime/tools/guest/agent-AGENTS.md`; OpenCode's
own adapter only describes its integration-specific behavior. Other assistants
can read `arlinux-agent instructions` from their authorized local shell and use
`arlinux-agent python SCRIPT.py` or `arlinux-agent python -c CODE` to join the
existing desktop environment. Grok Bot inherits that same environment and
installs native ADB on first use. Its private skills are managed through its
supported conversation/skill UI: ask it to read the local instructions and save
the ARLinux workflow as a private skill. Do not patch its client, edit its private
databases, bypass local command approval or expose the host socket over TCP.
`phone.open_desktop(serial)` returns to ARLinux after verified ADB setup; restore
the requesting assistant with its saved Tasks window ID. OpenCode retains its
prompt-specific `phone.open_opencode(serial)` adapter.
First-use OpenCode installation also installs native ADB from the distribution
repository (`adb` on Debian, `android-tools` on Arch). Later launches reuse the
installed tools without refreshing package databases. Device debugging,
authorization and wireless pairing remain explicit user operations.
The `arlinux.phone` API provides separate, user-authorized main-screen capture
through the Android host's MediaProjection foreground service. The user must
accept screen sharing while ARLinux is visible, then can open system Settings
while OpenCode observes screenshots in the background. Sharing ends from the
notification, system controls or `phone.stop_capture()`; consent is never cached.
After verifying wireless ADB and ending capture, `phone.open_opencode(serial)`
uses that authorized connection's standard `am start` to return to the host and
focus OpenCode. It does not introduce a notification service or grant ADB access.
The shared right-edge OpenCode voice helper lives in `runtime/tools/guest` and
ships with Guest Tools for every distribution, including Yibu and Classic. It
waits for the actual AT-SPI composer within one bounded startup deadline, not
merely for the Electron document. Keep it out of individual desktop recipes.
It activates the owned OpenCode window through the existing Tasks/compositor
identity before focusing or submitting the prompt, including when a hosted
Android application is in front. Do not replace this with X11 title matching.

Protocol 4 distribution manifests declare `guestToolsApi: 1`. Android installs a
matching independently verified Guest Tools package before first boot, records
its archive hash in the instance metadata, and never changes a running instance.
The Debug device deployment downloads both products separately and reuses an
unchanged verified base capsule. Release APKs embed both exact products as
separate assets. Instance snapshots include the pinned Guest Tools archive.
Pre-split distributions without this API declaration remain self-contained.
Guest Tools also supplies the Debian-only Software Store entry. Its standard
desktop `TryExec` hides it on guests without APT. First use installs Synaptic
from the configured distribution repositories, then launches it using guest
virtual-root privileges; later launches do not reinstall it. This is not a
PackageKit/Flatpak service or a replacement for Spark Store. Arch-family guests
have neither this Debian entry nor the Android-generated Spark Store entry.

Spark's portable Windows packages use `arlinux-spark-wine` from Guest Tools.
Android verifies the catalog download, then the importer reads the package's
static `EXEC_PATH`, imports its application tree under Program Files into the
single instance Bionic Wine prefix, and records its own installation receipt and
desktop entry. These imports are not DPKG installations. Removal deletes the
owned application tree and entry, never the shared Wine prefix. Existing
application directories owned by another installer are rejected rather than
merged. Vendor bottle registries, Linux Wine DLL patches, dynamic launch scripts
and packages requiring a vendor setup program are not supported. Static DLL
overrides apply only to that application's launch.

Guest Tools provides QQ, WeChat and Grok Bot (from Cursor's Bot download page)
entries for ARM64 Debian-family guests.
First launch downloads the official DEB mirrored in the fixed
[`popular-apps` release](https://github.com/taowen/arlinux-rootfs/releases/tag/popular-apps),
verifies the release's `SHA256SUMS`, and installs it and its dependencies with
APT. Subsequent launches skip installer downloads and APT; each application's
own online features still require networking.
WeChat also installs its undeclared NSS dependency. Applications select their
native display backend from the desktop session environment. WeChat uses its
native Qt surface size because its bundled Qt paints incorrectly at HiDPI scale.
The default HTTP/HTTPS handler opens the Android host's WebView browser through
standard `xdg-open` associations, without installing a Linux browser. It requires
a host with the browser endpoint. Vendor menu shortcuts are hidden with
`NoDisplay`, retaining their URL handlers for login callbacks; the on-demand
shortcut remains visible in Apps.
The same release supplies Android Pad Apps as `wps-pad.apk` and `capcut-pad.apks`.
The Android host owns their catalog and private sandbox installation. The latter
is a flat ZIP of original signed APKs, including `base.apk` and all required
splits; never merge or re-sign the APKs. Keep these asset names when replacing
installers. Both launchers read current digests from `SHA256SUMS` rather than
requiring a code update. Preserve every release asset's checksum when updating:

```powershell
$assets = 'qq-arm64.deb', 'wechat-arm64.deb', 'grok-bot-arm64.deb', 'wps-pad.apk', 'capcut-pad.apks'
$sums = $assets | ForEach-Object { "$( (Get-FileHash $_ -Algorithm SHA256).Hash.ToLowerInvariant() )  $_" }
[IO.File]::WriteAllText((Join-Path $PWD 'SHA256SUMS'), ($sums -join "`n") + "`n")
gh release upload popular-apps @assets --clobber --repo taowen/arlinux-rootfs
gh release upload popular-apps SHA256SUMS --clobber --repo taowen/arlinux-rootfs
```

Upload the new installers before their updated checksums. A client racing an
update rejects a mismatched download and can retry; it never skips verification.
The checksum file uses an ordinary release download, avoiding anonymous API
rate limits during installation.

The bundled QQ and WeChat icons come from those official packages. Tencent owns
the applications and icons; their vendor licenses and first-run agreements apply.

Guest Tools supplies one File Manager entry on every distribution. It opens the
user's existing `inode/directory` MIME default without changing that association.
Without a configured default it uses the desktop's paired manager: Dolphin on
Plasma, PCManFM-Qt on LXQt, and Thunar on the other current ARLinux profiles.
Installed managers open offline; a missing fallback installs from the configured
APT or pacman repositories on first click. Plain Arch has no official default;
Thunar is our lightweight fallback, not an Arch recommendation. Apps consolidates
FileManager-category launchers behind this entry; native desktop menus stay intact.

OpenCode Desktop is also an on-demand vendor product, not a build dependency.
`runtime/tools/opencode.py` resolves its current stable ARM64 installer from
`https://opencode.ai/update/api/latest/desktop/opencode` on first installation,
then passes that response's immutable URL and SHA-256 to the ordinary shared
download cache. Do not restore a version-pinned TSV or use GitHub's V1 `latest`
release as the V2 channel. Metadata failure must remain a visible retryable
installation error, not an unverified download or silent downgrade. Existing
installations launch offline without an added update lookup. New installations
prefer an available DeepSeek Flash Free model from the official Zen catalog;
otherwise they retain the upstream free default. Existing user configuration
is not overwritten and model lookup failure does not fail installation.
User-facing downloads consume Android's single `/etc/arlinux-network.json`
policy. Only Chinese app locales enable Steam relaying, GitHub destination hints,
public Release proxy candidates or official OpenCode CDN selection. Missing or
disabled policy uses ordinary direct requests, without relay certificates or
certificate-tool installation. English Steam preparation removes this relay's
previous trust if the user changed language. Debug maintenance has its own
developer-triggered routing and is independent of this user policy.
The common downloader retains TLS verification, HTTPS-only redirects, immutable
payload SHA-256 verification, shared caching and validated Range continuation.
Third-party Release proxies never receive metadata or authenticated API calls.
OpenCode metadata and payloads retain their official host and TLS identity;
Android compares verified DNS addresses and the downloader keeps direct fallback.
The launcher selects OpenCode's upstream portable shell permission scanner via
inline runtime config. Bun's WebAssembly tree-sitter loader can terminate a
syscall-translated Android guest with SIGSYS before a shell command runs. The
portable scanner retains command and directory permission checks. Explicit
caller inline configuration takes precedence; user config files are not rewritten.

The only Linux entry is `./build.sh`. Android APK development uses the Android
repository's `build.ps1`, which obtains this independent distribution product.

For rootfs-only device iteration, the Android `build.ps1 rootfs -FrameworkRef
COMMIT -Serial SERIAL -Instance INSTANCE` entry calls this same coordinator with
`--metadata-only`. It resolves/builds the ordinary graph and writes `products.json`
without downloading product capsules to Windows. The Debug APK downloads the
immutable hashed components directly and imports the exact base `default.zip`
and independent `guest-tools.zip`. The
canonical device procedure, fresh-instance replacement and authentication
preparation are documented in the Android repository's `docs/BUILD.md`; this is
not a second builder or a publication of public latest. APK builds still fetch
the same product normally.

The Android entry's `capture` command selects the independent `vulkan-capture`
stage in `taowen/gfxreconstruct` on the ARM64 standard runner. Its pinned source
is `third_party/gfxreconstruct`; native compilation, verified staging and
`capture.zip` assembly are owned by `tools/build/vulkan-capture-component.py`.
Distributions consume the exact verified capture product as a declared
dependency. The layer, conversion/replay tools and license notices are installed
in `/usr/lib/arlinux/gfxreconstruct`; no capture layer is enabled by default.
The single runtime command `/usr/bin/arlinux-vulkan-capture` selects it only for
its child process. Its archive receipt and per-tool provenance accompany the product.

In the desktop terminal, run `arlinux-vulkan-capture --seconds 30 -- APPLICATION ARGUMENTS`.
It records only that child invocation, defaults to a 512 MiB approximate capture
limit and writes `~/Documents/ARLinux-Captures/TIMESTAMP`. The child is stopped
at the time/size limit; save work first. Captures can contain private data and
are never uploaded automatically. Conversion is explicit (`--convert`), or use
`--lifetimes` for API resource counts. JSON output may be much larger than the
capture. These diagnostics are not performance/compatibility acceptance.

```sh
python tools/build/install-github-workflows.py
./build.sh github --framework-ref COMMIT --product rootfs --output build/products
./build.sh publish-rootfs lxqt TAG build/products/rootfs/default.zip
```

Yibu is supplied only inside the Release APK. Its internal archive remains a
build and Debug import product, with no standalone ZIP publication.
The publisher supports the other distributions only.

Windows calls this same Python publisher through Android's `build.ps1
publish-rootfs -Distribution ID -FrameworkRef COMMIT -Tag VERSION -Archive ZIP`.
It consumes the exact tested producer ZIP and adjacent `.json` receipt, verifies
its hash and normal bundle validation, and uploads without rebuilding. Split
Guest Tools remain APK-owned; these downloads require ARLinux 0.6.4 or newer.

Use Python with zstandard and authenticated gh. Component ownership, ARM64
standard runner labels and dependencies are defined in
the independent `tools/build/components/COMPONENT.json` recipes. Each repository runs an independent
workflow_dispatch and owns its own 10 GB cache. Each recipe declares one fixed
`arlinux-COMPONENT-debug` release for daily development, updated in place.
Public distribution promotion remains explicit and does not create SHA-tagged
development releases. The coordinator queries the debug endpoint directly;
input identities select verified caches, not release tags. Payloads upload before
the completion metadata, and all downloads retain SHA-256 verification.
No WSL compilation is required.
Request identities use source content and dependency identities rather than the
integration commit alone. Actions caches retain stage archives, receipts and Linux
GPU outputs. Root-owned scratch rootfs trees are deliberately excluded: their
private directory permissions prevent the unprivileged Actions cache from saving.
Guest ownership and modes stay inside snapshot.tar; only host cache files change
owner after the privileged package stage.
The unmodified distribution seed is stored separately as a content-addressed
`.tar.zst` and SHA-256 receipt. Hosted caches include these readable archives,
never their root-owned extracted trees. Every reuse verifies the archive before
extracting guest ownership and links. Runtime-only edits can therefore reuse
the base seed even when the assembled Linux-assets stage changes.

Hosted verification used [run 39196689519](https://github.com/taowen/arlinux-yibu/actions/runs/39196689519)
to populate the seed archive, then [run 39197632409](https://github.com/taowen/arlinux-yibu/actions/runs/39197632409)
with a changed Steam Python launcher. Both workflows passed: 623 seconds and
173 seconds overall, with dispatcher steps of 530 and 73 seconds. The second
log explicitly reports `verified archive cache` for the seed and `OK cached
rootfs-packages` for the APT snapshot. It still rebuilt Linux assets and sealed
the changed runtime into a new distribution; this was not a coordinator-only
cache lookup or reuse of an installed instance. Network/runner time is included.
After successful publication, the workflow compacts the pipeline cache to the
current input identity and generation of each completed stage. Older directories
still referenced by the current build remain in place. This prevents restored
historical snapshots from accumulating inside every newly saved Actions cache.
The `arlinux-v2-` cache namespace allows saving compacted entries while retaining
the previous namespace as a verified restore seed. The two-attempt
[Yibu check](https://github.com/taowen/arlinux-yibu/actions/runs/39146117964)
passed in 147 and 95 seconds, with a 9-second build step each time. Its saved
cache decreased from 2,359,330,124 to 620,977,341 bytes, with identical output.
Only matching input/output hashes permit reuse; published component capsules
preserve symlinks and modes. Keep download checksums and archive path safety,
propagate real command failures, and validate APK behavior on a fresh instance.
Local coordinators hold a cross-process lock per content identity until the
component is published. Concurrent invocations share the result instead of
dispatching the same workflow twice; process exit automatically releases locks.
Pinned full commit references do not need an extra GitHub commit-resolution call.

Guest Tools application files do not belong to the base-image or APT identity.
The final sealing recipe does not belong to the APT
snapshot identity. The snapshot tracks its package callback, installer recipes,
bundle packer, distribution and GPU inputs. The desktop stage separately tracks
the sealing and bundle recipes. This prevents compression changes from silently
reusing stale archives or unnecessarily reinstalling packages after the cache
schema has been populated.

The package stage declares its compiled runtime helpers and Arch Mesa installer
as inputs explicitly; application entry points are not classified by filename
extension. Optional capture archives and their installation recipe belong to
Linux assets and final sealing, not APT preparation. System integration files and
capture tools remain input/output-verified there; application files are verified
by the independent Guest Tools product.
Debian package preparation uses deb.debian.org inside the disposable package
rootfs, restoring the distribution's consumer sources before exporting the
snapshot. This does not change the user's repository policy or bypass APT
signature validation. The immutable base seed retains its distribution recipe.
Capture runners install only the native capture dependencies and initialize
GFXReconstruct and its staging-helper repository; GPU compiler jobs do not clone
an unrelated distribution. These preparation paths remain in the same adapter.

Hosted verification on 2026-10-19 used framework
`cae4d3293e4a3b104fe4dc335c11e273f9004e52` for
[the first package rebuild](https://github.com/taowen/arlinux-yibu/actions/runs/39244008771):
208 seconds from dispatch to completion, including 65 seconds of host
preparation, 8 seconds restoring cache, 109 seconds in the product dispatcher
and 10 seconds publishing. The log downloaded 156 MB of Debian packages in
2 seconds from deb.debian.org. The earlier
[Tsinghua-mirror run](https://github.com/taowen/arlinux-yibu/actions/runs/39232216101)
took 1,047 seconds overall, including 946 seconds in the dispatcher and
778 seconds downloading 156 MB. This is an observed network comparison,
not a guaranteed mirror throughput or a compiler speedup. The rebuilt ZIP SHA-256
is `1772dd3b56e8c84fa18978dbfb67175da0a3c186b29d9ff43e5b829523c867a7`;
reading its actual rootfs confirmed that consumer APT sources were restored.

Framework `80540e94103cb79255356859179eb935f4da717a` then changed the
extensionless capture command's documentation and removed the synthetic test
hook, without changing package recipes.
[The changed-runtime build](https://github.com/taowen/arlinux-yibu/actions/runs/39244537290)
passed in 176 seconds overall: host preparation 71 seconds, cache restore
7 seconds, dispatcher 69 seconds and publication 10 seconds. Its log reports
`OK cached rootfs-packages`, reuses the verified seed archive and seals a new
distribution. This demonstrates package-cache reuse, not reuse of an installed
instance. The changed ZIP SHA-256 is
`44edb8119dfd06defe711750af0ea7d091a15f6976f7677c1b1d7d907c96d651`;
reading the archive confirmed the new extensionless command documentation and
unchanged consumer APT sources. The product adapter no longer runs unrelated synthetic unit tests as
part of building a distribution; production input/output validation remains.

The independently dispatched
[capture product](https://github.com/taowen/gfxreconstruct/actions/runs/39243368620)
passed with 43 seconds of host preparation, compared with 80 seconds in
[the earlier run](https://github.com/taowen/gfxreconstruct/actions/runs/39230755443).
Its incremental build took 12 seconds; this is not a cold compiler benchmark.
These checks validate hosted construction and caching only, not fresh-instance
device compatibility. Environment preparation and Linux-assets assembly remain
material costs after eliminating repeated APT preparation.

Final rootfs compression uses Zstandard level 12 with four threads. A Windows
compression-only benchmark of a real 638,197,760-byte tar stream measured level
19 at 126.1 seconds / 156,531,220 bytes and level 12 at 7.6 seconds /
174,929,387 bytes. Both round trips matched the original SHA-256. These are
compression timings under local load, not total hosted workflow timings.
Do not maintain redundant feature/file presence lists or rescan the same rootfs
at multiple packaging layers. Publish only the exact validated bundle.

Linux GPU cross-compilation invokes ccache explicitly in the Meson cross file
and libhybris CC/CXX commands. The hosted test compiles C and C++ twice in an
isolated cache and requires two hits with identical object files. This changes
compiler caching, not Mesa drivers or its X11/Wayland platform selection.
Distribution-only jobs skip deletion of unrelated preinstalled runner SDKs.

Actions-restored stage generations take precedence over older release seeds;
the ordinary pipeline input/output verification still decides whether they can
be reused. This prevents an older seed overwriting restored Mesa outputs and
triggering an unnecessary rebuild and published-output mismatch.
The [Mesa verification](https://github.com/taowen/mesa/actions/runs/39150751054/attempts/2)
passed in 75 and 62 seconds overall, with build steps of 2 and 1 seconds and
identical published output. The [Yibu verification](https://github.com/taowen/arlinux-yibu/actions/runs/39150885091/attempts/2)
passed in 99 and 75 seconds, with a 9-second build step on the repeat.

Capsule imports stream decompression into tar extraction without writing an
intermediate full tar. Hosted tests retain executable/read-only modes, symlinks
and hardlinks across repeated imports and reject corrupt or escaping archives.

Workstation product downloads persist manifests, partial transfers and verified
capsules under shared `github-state/downloads/COMPONENT_ID` directories. A lock
serializes concurrent consumers of the same component. Missing parts download
in parallel (up to four); completed partial files are verified and promoted
without another request. Interruptions retain
verified parts and partial files for resumption; after assembling and checking
the complete capsule, redundant part copies are removed.
Canonical per-commit product caches can also satisfy another integration pin
when the exact component reference and every output hash match. The coordinator
copies those verified files into the isolated output instead of fetching them
again. A changed component still requires its first transfer; this is a build
cache, not reuse of installed instances or arbitrary release ZIPs.

Client-only coordinator and artifact transport code is excluded from distribution
content inputs. Changing retries, transfer caching or download presentation must
not invalidate the Linux image. Actual rootfs generation scripts, runtime files,
GPU inputs and the package-selection function remain declared inputs.

The real Yibu product from hosted run 39179635023 passed the normal archive and
rootfs receipt checks. A different integration pin with identical component
inputs then reused the verified 191,807,424-byte distribution ZIP in 7.4 seconds,
without binary downloads. Updating the transport implementation also resumed
that same hosted Yibu run rather than dispatching another distribution build.
These results verify build/download behavior, not fresh-instance application
acceptance.
