# OpenCode integration

After wireless ADB setup, use `phone.open_opencode(serial)` instead of the
generic `phone.open_desktop(serial)` to return to the current OpenCode prompt.
The OpenCode shell plugin preserves accessibility with `NO_AT_BRIDGE=0`.
