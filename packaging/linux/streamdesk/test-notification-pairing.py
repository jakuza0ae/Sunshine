"""Provide an isolated notification server for the GameStream pairing regression."""

from pathlib import Path
import sys

from gi.repository import Gio, GLib


NAME = "org.freedesktop.Notifications"
PATH = "/org/freedesktop/Notifications"
INTERFACE = Gio.DBusNodeInfo.new_for_xml(f"""<node><interface name="{NAME}">
  <method name="GetCapabilities"><arg type="as" direction="out"/></method>
  <method name="Notify">
    <arg type="s" direction="in"/><arg type="u" direction="in"/>
    <arg type="s" direction="in"/><arg type="s" direction="in"/>
    <arg type="s" direction="in"/><arg type="as" direction="in"/>
    <arg type="a{{sv}}" direction="in"/><arg type="i" direction="in"/>
    <arg type="u" direction="out"/>
  </method>
  <method name="CloseNotification"><arg type="u" direction="in"/></method>
  <signal name="NotificationReplied"><arg type="u"/><arg type="s"/></signal>
</interface></node>""").interfaces[0]


def main(scratch):
    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    loop = GLib.MainLoop()
    receipts = []
    closed = set()
    failures = []

    def call(connection, sender, _path, _interface, method, args, reply):
        try:
            if method == "GetCapabilities":
                reply.return_value(GLib.Variant("(as)", (["actions", "inline-reply"],)))
            elif method == "Notify":
                app, replaced, _, title, message, actions, hints, timeout = args.unpack()
                assert app == "StreamDesk" and replaced == 0 and timeout == 0
                assert title == "StreamDesk pairing" and "127.0.0.1" in message
                assert actions == ["inline-reply", "Enter PIN"]
                assert hints["transient"] is True
                assert hints["x-kde-reply-placeholder-text"] == "4 digits"
                assert hints["x-kde-reply-submit-button-text"] == "Pair"
                assert len(receipts) < 2
                receipt = 123 + len(receipts)
                receipts.append(receipt)
                reply.return_value(GLib.Variant("(u)", (receipt,)))

                def submit():
                    if len(receipts) == 1:
                        connection.emit_signal(sender, PATH, NAME, "NotificationReplied",
                                               GLib.Variant("(us)", (999, "4321")))
                        pin = "bad input"
                    else:
                        pin = "4321"
                    connection.emit_signal(sender, PATH, NAME, "NotificationReplied",
                                           GLib.Variant("(us)", (receipt, pin)))
                    return GLib.SOURCE_REMOVE

                GLib.timeout_add(100, submit)
            elif method == "CloseNotification":
                closed.add(args.unpack()[0])
                reply.return_value(None)
                if len(receipts) == 2 and closed == set(receipts):
                    (scratch / "notification-accepted").touch()
        except AssertionError as error:
            failures.append(error)
            reply.return_dbus_error(f"{NAME}.Regression", "Unexpected notification or reply")
            loop.quit()

    bus.register_object(PATH, INTERFACE, call, None, None)
    Gio.bus_own_name_on_connection(bus, NAME, Gio.BusNameOwnerFlags.DO_NOT_QUEUE,
                                  lambda *_: (scratch / "notification-ready").touch(),
                                  lambda *_: loop.quit())
    loop.run()
    if failures:
        raise failures[0]


if __name__ == "__main__":
    main(Path(sys.argv[1]))
