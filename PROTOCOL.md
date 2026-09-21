# Arlinux distribution protocols

The Android host and a Linux distribution meet at two independent contracts:

- [Static bundle protocol](STATIC-PROTOCOL.md) defines archive contents,
  verification, installation, instances, and first boot.
- [Android–Linux runtime protocol](RUNTIME-PROTOCOL.md) defines the live
  session and its display, input, graphics, input-method, audio, accessibility,
  and lifecycle interfaces.

Keeping these contracts separate allows a bundle to be inspected and built
without Android while runtime services evolve through their own negotiated
interfaces. Distribution authors should read both documents.
