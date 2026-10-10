# Standalone WebView2 probe

This local Win32 page separates browser compatibility from Adobe installation,
authentication and networking. Install Microsoft's WebView2 runtime in the test
prefix first. Build on Linux with LLVM-MinGW, curl and unzip:

```sh
bash build.sh
arlinux-windows --prefix "$HOME/webview-test" "$PWD/build/webview-probe.exe"
```

Keep `WebView2Loader.dll` next to the executable. Optional browser arguments are
accepted as one quoted argument. None are required or injected by this host.

The page changes background once per second and reports a JavaScript timer,
`requestAnimationFrame` count, keyboard input, navigation and process failures
to `C:\webview-probe.txt`. Browser data is stored in `C:\webview-probe-data`.
Timer messages alone do not prove rendering works: confirm animation frames
advance, visible pixels change and text can be entered.

The standalone Windows runtime uses DXVK 2.7.1, a Vulkan 1.3-compatible baseline.
On the Redmi Adreno 650 this renders through Turnip, without disabling GPU
acceleration or adding browser arguments. DXVK 3.1 rejects this adapter because
`storageBuffer8BitAccess` is unavailable. Repeated factory creation also exposed
DXVK's failed-singleton-construction bug; the build carries an exception-safety
patch for that separate error. Neither fix fabricates missing Vulkan features.

A diagnostic software-rendering control is:

```sh
PROTON_USE_WINED3D=1 arlinux-windows --prefix "$HOME/webview-test" \
    "$PWD/build/webview-probe.exe" \
    '--disable-gpu --disable-features=RemoveRedirectionBitmap'
```

This is not the product default or proof of hardware acceleration. It separates
the browser/host/input path from DXVK and DirectComposition output failures.
