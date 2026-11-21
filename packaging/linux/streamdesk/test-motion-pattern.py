import gi
gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
gi.require_version("Gtk4LayerShell", "1.0")
from gi.repository import Gdk, GLib, Gtk, Gtk4LayerShell as Shell

def start(app):
    monitors = Gdk.Display.get_default().get_monitors()
    output = next(monitors[i] for i in range(monitors.get_n_items())
                  if monitors[i].get_connector() == "Virtual-StreamDesk")
    window = Gtk.ApplicationWindow(application=app)
    Shell.init_for_window(window)
    Shell.set_monitor(window, output)
    Shell.set_layer(window, Shell.Layer.OVERLAY)
    for edge in (Shell.Edge.TOP, Shell.Edge.LEFT, Shell.Edge.BOTTOM, Shell.Edge.RIGHT):
        Shell.set_anchor(window, edge, True)
    area = Gtk.DrawingArea()
    window.set_child(area)
    phase = 0

    def draw(_, ctx, width, height):
        nonlocal phase
        phase += 1
        if phase == 1:
            assert (width, height) == (1440, 1080), (width, height)
            print("Pattern ready", Shell.get_monitor(window).get_connector(), flush=True)
        ctx.set_source_rgb(.09, .10, .12)
        ctx.paint()
        for row in range(16):
            for col in range(10):
                shade = 45 + ((phase + row * 19 + col * 7) % 52)
                ctx.set_source_rgb(shade/255, 2*shade/255, (180-shade)/255)
                ctx.rectangle(col*145+10, row*65+8, 135, 54)
                ctx.fill()
                ctx.set_source_rgb(1, 1, 1)
                ctx.move_to(col*145+22, row*65+30)
                ctx.show_text(f"{row:02d} . {(phase+col)%1200:04d}")

    area.set_draw_func(draw)
    GLib.timeout_add(33, lambda: (area.queue_draw(), True)[1])
    window.present()

app = Gtk.Application(application_id="org.streamdesk.NestedPattern")
app.connect("activate", start)
app.run()
