/**
 * @file src/platform/linux/pairing_notifications.cpp
 * @brief Inline pairing PIN replies on Linux notification servers that support them.
 */
#include "pairing_notifications.h"

#include "src/nvhttp.h"

#include <algorithm>
#include <gio/gio.h>
#include <map>
#include <span>
#include <string>
#include <string_view>

namespace {
  constexpr auto service = "org.freedesktop.Notifications";
  constexpr auto path = "/org/freedesktop/Notifications";
  constexpr int call_timeout_ms = 500;

  /** @brief Pairing identifier, source address, and notification server's receipt. */
  struct notice_t {
    std::string address;  ///< Address reported by the accepted GameStream connection.
    guint32 id;  ///< Notification ID, or zero once the user has dismissed it.
  };

  /** @brief Own the notification receipts for one GameStream server lifetime. */
  class pairing_notifications_t {
  public:
    /**
     * @brief Use the monitor proxy to track the actual notification server owner.
     * @param proxy Notification server proxy, used only on its GLib context.
     */
    explicit pairing_notifications_t(GDBusProxy *proxy):
        proxy {proxy},
        bus {g_dbus_proxy_get_connection(proxy)} {}

    /** @brief Close outstanding notifications when Sunshine exits. */
    ~pairing_notifications_t() {
      for (const auto &[_, notice] : notices) {
        close(notice.id);
      }
    }

    /** @brief Reconcile notifications with unexpired, unapproved GameStream requests. */
    void refresh() {
      g_autofree auto *unique_name = g_dbus_proxy_get_name_owner(proxy);
      if (owner != (unique_name ? unique_name : "")) {
        notices.clear();
        owner = unique_name ? unique_name : "";
        inline_reply = !owner.empty() && supports_inline_reply();
      }

      if (!inline_reply) {
        return;
      }

      const auto pending = nvhttp::get_pending_pairings();
      std::erase_if(notices, [&](const auto &entry) {
        if (std::ranges::find(pending, entry.first, &nvhttp::pending_pairing_t::id) != pending.end()) {
          return false;
        }
        close(entry.second.id);
        return true;
      });

      for (const auto &request : pending) {
        if (!notices.contains(request.id)) {
          notices.emplace(request.id, notice_t {request.address, show(request.address)});
        }
      }
    }

    /**
     * @brief Accept a reply only from the owner that created its exact notification.
     * @param sender D-Bus unique name of the signal sender.
     * @param signal Signal member name.
     * @param parameters Signal arguments, borrowed from the GLib dispatcher.
     */
    void signal(const char *sender, std::string_view signal, GVariant *parameters) {
      if (!sender || sender != owner) {
        return;
      }

      const bool reply = signal == "NotificationReplied" && g_variant_is_of_type(parameters, G_VARIANT_TYPE("(us)"));
      if (!reply && (signal != "NotificationClosed" || !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(uu)")))) {
        return;
      }

      guint32 id {};
      g_variant_get_child(parameters, 0, "u", &id);
      const auto found = std::ranges::find_if(notices, [id](const auto &entry) {
        return id != 0 && entry.second.id == id;
      });
      if (found == notices.end()) {
        return;
      }

      found->second.id = 0;
      if (reply) {
        const gchar *pin = nullptr;
        g_variant_get_child(parameters, 1, "&s", &pin);
        if (nvhttp::is_valid_pairing_pin(pin)) {
          nvhttp::pin(found->first, pin, "Moonlight at " + found->second.address);
        } else {
          // Show a fresh input field for this request at the next refresh.
          notices.erase(found);
        }
        close(id);
      }
    }

  private:
    /** @brief Read the notification server's advertised actions. */
    bool supports_inline_reply() const {
      g_autoptr(GVariant) result = call("GetCapabilities", nullptr, G_VARIANT_TYPE("(as)"));
      if (!result) {
        return false;
      }

      g_autoptr(GVariant) capabilities = g_variant_get_child_value(result, 0);
      gsize count {};
      g_autofree const gchar **values = g_variant_get_strv(capabilities, &count);
      return std::ranges::any_of(std::span(values, count), [](std::string_view value) {
        return value == "inline-reply";
      });
    }

    /** @brief Post the PIN input action and return the ID assigned by Plasma. */
    guint32 show(const std::string &address) const {
      const gchar *actions[] {"inline-reply", "Enter PIN", nullptr};
      GVariantBuilder hints;
      g_variant_builder_init(&hints, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add(&hints, "{sv}", "transient", g_variant_new_boolean(TRUE));
      g_variant_builder_add(&hints, "{sv}", "x-kde-reply-placeholder-text", g_variant_new_string("4 digits"));
      g_variant_builder_add(&hints, "{sv}", "x-kde-reply-submit-button-text", g_variant_new_string("Pair"));
      const auto body = "Moonlight at " + address + " wants to pair. Enter the four digits shown on that device.";
      g_autoptr(GVariant) result = call("Notify", g_variant_new("(susss^asa{sv}i)", "StreamDesk", 0U, "computer", "StreamDesk pairing", body.c_str(), actions, &hints, 0), G_VARIANT_TYPE("(u)"));
      guint32 id {};
      if (result) {
        g_variant_get(result, "(u)", &id);
      }
      return id;
    }

    /** @brief Close a receipt on the service instance that created it. */
    void close(const guint32 id) const {
      if (id && !owner.empty()) {
        g_dbus_connection_call(bus, owner.c_str(), path, service, "CloseNotification", g_variant_new("(u)", id), nullptr, G_DBUS_CALL_FLAGS_NONE, call_timeout_ms, nullptr, nullptr, nullptr);
      }
    }

    /** @brief Call a method on the pinned D-Bus owner with a bounded timeout. */
    GVariant *call(const char *method, GVariant *parameters, const GVariantType *result) const {
      return g_dbus_connection_call_sync(bus, owner.c_str(), path, service, method, parameters, result, G_DBUS_CALL_FLAGS_NONE, call_timeout_ms, nullptr, nullptr);
    }

    GDBusProxy *proxy;  ///< Notification name-owner and signal monitor.
    GDBusConnection *bus;  ///< Session bus borrowed from the monitor.
    std::string owner;  ///< Unique name of the active notification server.
    bool inline_reply = false;  ///< Whether the notification server accepts text replies.
    std::map<std::string, notice_t> notices;  ///< Pending requests indexed by exact pairing ID.
  };
}  // namespace

namespace platf {
  void run_pairing_notifications(const std::stop_token stop) {
    g_autoptr(GMainContext) context = g_main_context_new();
    g_main_context_push_thread_default(context);
    g_autoptr(GDBusProxy) proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION, static_cast<GDBusProxyFlags>(G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START), nullptr, service, path, service, nullptr, nullptr);
    if (proxy) {
      pairing_notifications_t notifications {proxy};
      auto on_signal = +[](GDBusProxy *, gchar *sender, gchar *signal, GVariant *parameters, gpointer data) {
        static_cast<pairing_notifications_t *>(data)->signal(sender, signal, parameters);
      };
      const auto handler = g_signal_connect(proxy, "g-signal", G_CALLBACK(on_signal), &notifications);
      g_autoptr(GSource) timer = g_timeout_source_new_seconds(1);
      g_source_set_callback(timer, +[](gpointer data) -> gboolean {
        static_cast<pairing_notifications_t *>(data)->refresh();
        return G_SOURCE_CONTINUE;
      },
                            &notifications,
                            nullptr);
      g_source_attach(timer, context);
      std::stop_callback wake {stop, [context] {
                                 g_main_context_wakeup(context);
                               }};
      while (!stop.stop_requested()) {
        g_main_context_iteration(context, TRUE);
      }
      g_source_destroy(timer);
      g_signal_handler_disconnect(proxy, handler);
    }
    g_main_context_pop_thread_default(context);
  }
}  // namespace platf
