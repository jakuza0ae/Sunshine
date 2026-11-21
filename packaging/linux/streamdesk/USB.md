# StreamDesk Lightning relay

The host relay keeps GameStream pairing, launch, media and input intact. It opens
TCP connections to an iPad on port 41589 through libusbmuxd. The client listener
uses IPv4 loopback, and the relay only connects to Sunshine's IPv4 loopback.
The host explicitly selects a physical USB device. The ordinary LAN service
continues to serve existing Wi-Fi clients.

The relay needs Python 3.11+, libusbmuxd and usbmuxd running on the Linux host.
An already paired iPad is required. The relay selects the sole physically
attached device unless you supply `--udid`. Run the relay in the foreground with
`~/.local/bin/streamdesk-host usb` once StreamDesk build 7 or later is waiting
in **Connect → USB**. The launcher uses the configured GameStream port.
In a source checkout, you can also run
`python3 packaging/linux/streamdesk/streamdesk-usb-relay.py --port 48189`.
Add `--wait` to wait for the device and retry after disconnect. The optional
systemd user service provides the same behavior while StreamDesk Host is
running. Use `streamdesk-host usb-auto` and `streamdesk-host usb-auto-off`
to enable and disable the service. It is disabled on a fresh install.

## Wire protocol

All integers wider than a byte are big-endian. Every USB socket begins with
four bytes `SDU1`, a one-byte kind and a four-byte channel ID.

| Kind | Channel ID | Bytes after the header |
| ---- | ---------- | ---------------------- |
| 0, session | 0 | Host advertises a two-byte Sunshine base port. The client replies with `SDU1`. |
| 1, TCP | Requested ID | Ordinary bidirectional TCP bytes. EOF closes that direction. |
| 2, UDP | Requested ID | Each direction uses a two-byte length followed by the datagram. Lengths range from 1 to 16384. |

After session acknowledgement, the client requests lanes on the session socket
as a one-byte kind, a four-byte nonzero channel ID and a signed one-byte offset
relative to the base port. The relay opens a fresh USB socket for every request.
TCP offsets are −5 (HTTPS), 0 (HTTP) and 21 (RTSP).
UDP offsets are 9 (video), 10 (control and input) and 11 (audio).
The relay permits up to 24 concurrent lanes and closes them upon loss of the
session socket. Invalid requests end the relay. Each failed data lane is closed
independently. Independent sockets keep video backpressure out of the
application queues for audio and control.

The client binds GameStream listeners only to IPv4 loopback and publishes the
cable route after opening all required listeners. TCP listeners request a new
channel for each local connection. Each UDP listener keeps its local peer
address so replies appear to come from the expected GameStream port. The selected
cable route must stay fixed for the active streaming session. The client's
pairing identity and pinned Sunshine certificate apply to both route choices.
RTSP launch responses from Sunshine use the address of the accepted HTTP
connection, which is loopback in this path.

## Verification

```sh
python3 -W error::ResourceWarning packaging/linux/streamdesk/test-usb-relay.py -v
```

The fixtures cover TCP request fragmentation and half-close, repeated UDP
datagrams, concurrent video and control lanes, invalid requests and teardown.
A separate smoke run with a temporary iPad loopback listener forwarded over
SSH verified that two cable lanes opened through libusbmuxd and carried a
read-only HTTP request to the installed Sunshine instance. That run used an
emulated client. Real-device media decoding and end-to-end latency still need
to be measured with the StreamDesk app.
