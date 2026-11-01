# Steam Bionic compatibility adapter

`python3 /usr/lib/arlinux/steam_bionic.py register STEAM_DIRECTORY` registers
`arlinux-wine-bionic` under `compatibilitytools.d`. Steam startup also registers
it directly, without a Proton depot or patch pack. Bionic Wine is the ARLinux
default for games with Windows builds, including games that also offer native
Linux builds. The global `CompatToolMapping/0` entry uses priority `250`;
priority `75` is only a fallback and allows native compatibility rules to win.
Explicit per-game third-party and native Linux choices are preserved.
Previous ARLinux Proton selections migrate to Bionic. Restart Steam to rescan tools.

Steam invokes `/run %verb%`; the guest adapter accepts `run` and
`waitforexitandrun`, uses the single instance-level Wine prefix
`~/.local/share/arlinux/windows-bionic/prefix` (containing `pfx`), preserves executable
arguments, and returns the native job exit status. `getcompatpath` and
`getnativepath` translate the Android host Z: drive. The runtime is automatically
prepared using the current `windows-downloads.tsv` pin and shared verified
immutable download cache. Android's WindowsRuntimeHost launches Wine outside
any guest seccomp filter and selects matching native DXVK/graphics libraries.
Only the explicitly listed game identities and WebView environment variables
cross the host boundary; guest glibc loader and graphics paths do not.

`STEAM_COMPAT_DATA_PATH` remains Steam's per-game metadata directory, not the
Wine prefix. Before handing control to Valve, Steam startup prepares the Bionic
runtime, initializes this prefix and prepares the instance-wide WebView2 runtime.
On Play, `windows_webview.prepare()` reuses that runtime and
`steamworks.session()` supplies the Steamworks bridge. Discovery
helpers without a game identity run without a Steamworks session. These paths
share the Windows launcher's Wine prefix. The adapter does not inject engine
arguments or create title-specific compatibility selections. User selections
and launch arguments are retained.
`runtime/tools/windows_webview.py` owns one instance-wide WebView2 policy,
shared with Windows Launcher and built-in Windows apps. The fixed runtime and
browser arguments are identical for all hosts. Android's prefix initialization
owns the Windows 10 version identity, 144-DPI initial setting and ClearType;
neither app installers nor WebView2 preparation change the Windows version.
Both registry views use the documented `*` policy for all WebView2 apps; see
[Microsoft's loader override contract](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/webview2-idl#createcorewebview2environmentwithoptions).
These settings do not prove GPU rendering or provide a Chromium sandbox under Wine.
The first application launch prepares this common dependency even for an app
that does not use WebView2; later launches reuse the verified installed copy.
The patched Steam Proton adapter and its core build/install chain were removed.
Implementation does not establish compatibility for every game: acceptance
requires a fresh instance, hardware acceleration and actual Play/gameplay.

For device validation, delete and reinstall the selected test Linux instance,
use a fresh prefix, and record exact APK, bundle and runtime hashes. Reusing a
verified immutable runtime outside the instance is permitted; prefix or home
restoration is not. Authentication-only test preparation remains explicit and
private, outside application startup.
