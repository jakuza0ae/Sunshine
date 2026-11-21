# StreamDesk host for KDE Wayland

StreamDesk extends a KDE desktop onto an iPad using a dedicated 1440×1080 virtual output and the Sunshine streaming backend. The build targets Linux. The packaged profile uses a separate user service, settings directory, pairing database and TCP/UDP port family.

## Install a prebuilt package

The Arch/CachyOS x86-64 package has a userspace installer. Unpack and install it in one shell command.

```sh
tar -xzf StreamDesk-Host-linux-x86_64.tar.gz && bash StreamDesk-Host/install.sh
```

The package still uses the machine's runtime libraries and KDE tools. It requires no compilation or root access. A source build is available for machines with different dependencies.

## Install from source

Arch/CachyOS with a C++23 development environment, CMake, Ninja and Node/npm is the primary target. The build requires the usual Sunshine development libraries and KDE's `krfb-virtualmonitor`, `kscreen-doctor`, `bubblewrap` and `jq`. A hardware H.264 encoder and write access to `/dev/uinput` are recommended for remote mouse and keyboard.

From this checkout, install with one command.

```sh
bash packaging/linux/streamdesk/install.sh
```

For a fresh checkout use the following command.

```sh
git clone --depth 1 --branch linux-streamdesk https://github.com/jakuza0ae/Sunshine.git streamdesk-sunshine && bash streamdesk-sunshine/packaging/linux/streamdesk/install.sh
```

The installer compiles and stages a revision under `${XDG_DATA_HOME:-$HOME/.local/share}/streamdesk-host/releases`. Reinstalling the same revision reuses the binary and preserves configuration and pairing. Installation leaves existing sessions running. It does not enable any service at login.

## Start and connect

```sh
~/.local/bin/streamdesk-host start
~/.local/bin/streamdesk-host doctor
```

Open `https://localhost:48190` on the Linux host to initialize or administer this instance. In StreamDesk on the iPad, add the host as `LAN-IP:48189`, pair and choose **StreamDesk Display**. The built-in client profile initially requests H.264, 1280×960, 30 FPS and 6 Mb/s. The second-display settings also offer 1024×768 and 1440×1080.

`Virtual-StreamDesk` can be positioned in KDE Display Configuration. The host service waits for it before starting capture. KWin rejects capture if the named output disappears. The KRfb VNC TCP listener is confined to a separate network namespace. KWin permissions are registered for the specific fork binary.

The command below stops the StreamDesk host and releases its virtual display.

```sh
~/.local/bin/streamdesk-host stop
```

## Diagnose

`journalctl --user -u streamdesk-host.service -u streamdesk-display.service` shows capture, encoder and display setup. Check `Found H.264 encoder`, `Screencasting output name Virtual-StreamDesk` and the reported capture dimensions. GPU, display scale, Wi-Fi quality and device decoder behavior all affect the stream. Use the iPad toolbar **Stats** and the client's [measurement matrix](https://github.com/jakuza0ae/moonlight-ios/blob/ipad-streamdesk/docs/streamdesk/research.md) to assess latency and dropped frames under a repeatable workload.
