# Moonlight WinKey (experimental build)

Separate webOS application `com.limelight.webos.winkey`. It can be installed alongside ordinary Moonlight and must be paired with the host independently.

This branch experiments with a **non-exclusive**, Meta/Windows-key-only Linux evdev reader. It never calls `EVIOCGRAB`. It retains the original Moonlight SDL keyboard/mouse code, and searches for Bluetooth keyboards reconnected mid-session every 500ms.

Build: push to `feature/meta-key-bluetooth-safe` and download the `webos-snapshot-...` artifact from the `Build (webOS)` GitHub Actions run. Extract the `.ipk` and install it through webOS Dev Manager.

Validation protocol:
1. Test Win, Win+E, Win+R; ordinary Ctrl/Alt/Shift shortcuts.
2. Disconnect and reconnect Bluetooth **keyboard** without leaving stream; verify typing and Windows key.
3. Disconnect and reconnect Bluetooth **mouse** without leaving stream; verify pointer/clicks.
4. Repeat after 1s and after 5s; test Win key release after disconnect.
5. Diagnose logs for `WinKey: observing`, `WinKey: left DOWN`, or `WinKey SDL:`.

Limitations: Requires the TV app to be allowed to read `/dev/input/event*`. Non-exclusive forwarding may not prevent webOS from also handling the key. Key injection originates from evdev worker thread, and should be examined if host input ordering is inconsistent. This is experimental and **not tested on the actual LG TV**.

## Build status

The original main-branch webOS build workflow is used unchanged. This branch is deliberately kept separate so the normal Moonlight app and its Bluetooth SDL event handling are not replaced.
