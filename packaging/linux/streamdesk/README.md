# StreamDesk host for KDE Wayland

StreamDesk extends a KDE desktop onto an iPad using a dedicated 1440×1080 virtual output and the Sunshine streaming backend. The build targets Linux. The packaged profile uses a separate user service, settings directory, pairing database and TCP/UDP port family.

## Install a prebuilt package

The x86-64 package was built and tested on CachyOS with glibc 2.44 and OpenSSL 4. Its installer checks the Linux runtime libraries. Unpack and install it in one shell command.

```sh
tar -xzf StreamDesk-Host-linux-x86_64.tar.gz && bash StreamDesk-Host/install.sh
```

The package still uses the machine's runtime libraries and KDE tools. It requires no compilation or root access. A source build is available for machines with different dependencies.

`pack-prebuilt.sh` checks that Sunshine reports the same Git commit as the packaging checkout and that it was built from clean sources. Run `bash packaging/linux/streamdesk/test-prebuilt-revision.sh` to verify revision detection and rejection of mismatched binaries.

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

The installer selects the new release after validating its desktop entry and reloading the user systemd configuration. If applying the update fails, it restores the previous launcher and services.

## Start and connect

```sh
~/.local/bin/streamdesk-host start
~/.local/bin/streamdesk-host doctor
```

Open `https://localhost:48190` on the Linux host to initialize or administer this instance. In StreamDesk on the iPad, add the host as `LAN-IP:48189`. When the iPad shows its PIN, select **Enter PIN** in the KDE notification and enter the four digits. The web panel at `https://localhost:48190/pin` also supports pairing. Then choose **StreamDesk Display**. The built-in client profile initially requests H.264, 1280×960, 30 FPS and 6 Mb/s. The second-display settings also offer 1024×768 and 1440×1080.

New installations leave audio on the computer. To use the same default with a previously installed host, set `stream_audio = disabled` in its StreamDesk Sunshine configuration. The installer preserves existing settings and paired devices.

`Virtual-StreamDesk` can be positioned in KDE Display Configuration. The host service waits for it before starting capture. KWin rejects capture if the named output disappears. The KRfb VNC TCP listener is confined to a separate network namespace. KWin permissions are registered for the specific fork binary.

The command below stops the StreamDesk host and releases its virtual display.

```sh
~/.local/bin/streamdesk-host stop
```

## Diagnose

`journalctl --user -u streamdesk-host.service -u streamdesk-display.service` shows capture, encoder and display setup. Check `Found H.264 encoder`, `Screencasting output name Virtual-StreamDesk` and the reported capture dimensions. GPU, display scale, Wi-Fi quality and device decoder behavior all affect the stream. Use the iPad toolbar **Stats** and the client's [measurement matrix](https://github.com/jakuza0ae/moonlight-ios/blob/ipad-streamdesk/docs/streamdesk/research.md) to assess latency and dropped frames under a repeatable workload.

## Isolated streaming regression

The capture smoke test runs on loopback in its own network, D-Bus and KDE session. It uses a private WirePlumber 0.5 policy session to link the capture node, with no device monitors.

With the headless client and PyGObject available, set `STREAMDESK_TEST_KDE_PAIRING=1` to pair through a simulated notification server. This checks the reply field, rejection of an unrelated notification ID and retry after malformed input before the streaming checks.

```sh
packaging/linux/streamdesk/test-nested-kwin.sh "$HOME/.local/share/streamdesk-host/current"
```

The optional streaming regression builds [Moonlight Embedded](https://github.com/moonlight-stream/moonlight-embedded/tree/33142e58ebd576085a076f4ce24881d20b3c4ef5) at commit `33142e5` with `moonlight-headless-test.patch` applied. The patch only adds a test renderer that writes received H.264 decode units to a temporary file. No graphics backend is required for this client build.

```sh
git -C /path/to/moonlight-embedded apply --unidiff-zero "$PWD/packaging/linux/streamdesk/moonlight-headless-test.patch"
cmake -S /path/to/moonlight-embedded -B /path/to/moonlight-embedded/build \
  -DENABLE_SDL=OFF -DENABLE_FFMPEG=OFF -DENABLE_X11=OFF -DENABLE_CEC=OFF -DENABLE_PULSE=OFF
cmake --build /path/to/moonlight-embedded/build
STREAMDESK_PROBE_CLIENT=/path/to/moonlight-embedded/build/moonlight \
  packaging/linux/streamdesk/test-nested-kwin.sh "$HOME/.local/share/streamdesk-host/current"
```

The test performs pairing, launches **StreamDesk Display** and reads at least 30 decodable H.264 frames at the expected dimensions for each 4:3 profile. It uses `ffprobe` for validation. Input is disabled for the test client. For a content check, also set `STREAMDESK_TEST_ANIMATED=1`. This mode requires GTK 4, GTK4 Layer Shell with its Python bindings, and FFmpeg. A moving test pattern runs on the nested virtual display. The regression requires distinct decoded frames and nonzero host processing samples for every requested mode.

Results cover host capture, encoding and transport over loopback. The iPad decoder and the Wi-Fi link need separate validation.
