# Impulse Triggers (VaderHaptics)

A lightweight Windows DLL proxy (`dxgi.dll` / `dinput8.dll`) that enables **4-channel Xbox Impulse Trigger & grip vibration** on the **Flydigi Vader 5 Pro V2** controller in PC games.

It intercepts standard PC game haptics/rumble APIs at runtime using [MinHook](https://github.com/TsudaKageyu/minhook) and forwards all four motor channels (low grip, high grip, left trigger, right trigger) directly to the controller's Flydigi Vendor HID endpoint.

---

## Features

- **4-Channel Haptics:** Supports independent low-frequency grip, high-frequency grip, left impulse trigger, and right impulse trigger vibration (`0–255` per channel).
- **Broad API Coverage:** Hooks multiple Windows/PC gaming input & haptics layers:
  - **Windows.Gaming.Input (WGI)**
  - **GameInput** (`gameinput.dll` & `GameInputRedist.dll` v0, v1, v2, v3)
  - **XInput** (multi-module, including named exports and ordinals)
  - **SDL2 / SDL3** (grip & trigger rumble)
  - **Raw XUSB / Steam** (`kernel32!DeviceIoControl` for Microsoft XUSB `0x8000A010` & Steam extended rumble)
- **Drop-in Proxy DLLs:** Provided as both `dxgi.dll` and `dinput8.dll` so you can use whichever proxy your target game loads cleanly.
- **Radio-Friendly Packet Scheduling:** Rate-limits and deduplicates HID writes for the Flydigi wireless dongle (`VID_37D7&PID_2401`) to avoid saturating radio bandwidth, with keepalive and clean zero-vibration shutdown on exit.
- **Process-Safe:** Uses a per-process mutex (`Local\VaderHaptics_PID_<pid>`) and pins the proxy DLL in memory so hooks are only initialized once even if both DLLs are present or dynamically loaded.

---

## Supported Hardware

- **Controller:** Flydigi Vader 5 Pro V2
- - **Connection:** USB-C Cable (**Recommended**)
- **Connection:** Flydigi Wireless Dongle (`VID_37D7`, `PID_2401`) via Vendor HID interface

---

## Installation

1. Download `dxgi.dll` and/or `dinput8.dll` from the release.
2. Copy **one** of the DLLs (start with `dxgi.dll`; if the game already uses a `dxgi.dll` mod like ReShade or doesn't load it, try `dinput8.dll`) into the game's **executable directory** (the folder containing the game's `.exe` file).
3. Launch the game normally with your Flydigi Vader 5 Pro V2 connected.

> **Note:** You only need one proxy DLL per game. If you ever need to uninstall, simply delete the `dxgi.dll` or `dinput8.dll` you copied into the game folder.

---

## Logging & Troubleshooting

- If logging is enabled, output is written to `vader_haptics.log` in the same directory as the proxy DLL.
- If a game already ships with a custom `dxgi.dll` (e.g., ReShade, DXVK, Special K), use `dinput8.dll` instead to avoid conflicts.
- Make sure the controller is connected and recognized before or shortly after launching the game; the background HID worker thread will automatically open the Flydigi Vendor HID command endpoint.
