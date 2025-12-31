/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 *  Copyright © 2012 Igalia S.L.
 *
 *  This file is part of Pafari.
 *
 *  Pafari is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Pafari is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Pafari.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"
#include "ephy-about-handler.h"

#include "ephy-embed-shell.h"
#include "ephy-embed-prefs.h"
#include "ephy-embed-utils.h"
#include "ephy-file-helpers.h"
#include "ephy-flatpak-utils.h"
#include "ephy-history-service.h"
#include "ephy-history-types.h"
#include "ephy-output-encoding.h"
#include "ephy-prefs.h"
#include "ephy-settings.h"
#include "ephy-smaps.h"
#include "ephy-snapshot-service.h"
#include "ephy-web-app-utils.h"

/* Forward declarations for accessing EphyShell from embed layer */
/* Note: We can't include ephy-shell.h due to layering, but we can access the functions
 * via dlsym or by using the fact that EphyShell is a subclass of EphyEmbedShell */
typedef struct _EphyShell EphyShell;
typedef struct _EphyBookmarksManager EphyBookmarksManager;
typedef struct _EphyBookmark EphyBookmark;

/* Function pointer types for accessing bookmarks functions */
/* Note: GSequence and GSequenceIter are already defined in GLib */
typedef EphyBookmarksManager* (*EphyShellGetBookmarksManagerFunc) (EphyShell *shell);
typedef GSequence* (*EphyBookmarksManagerGetBookmarksWithTagFunc) (EphyBookmarksManager *self, const char *tag);
typedef const char* (*EphyBookmarkGetUrlFunc) (EphyBookmark *bookmark);
typedef const char* (*EphyBookmarkGetTitleFunc) (EphyBookmark *bookmark);
typedef GSequenceIter* (*GSequenceGetBeginIterFunc) (GSequence *seq);
typedef gboolean (*GSequenceIterIsEndFunc) (GSequenceIter *iter);
typedef GSequenceIter* (*GSequenceIterNextFunc) (GSequenceIter *iter);
typedef gpointer (*GSequenceGetFunc) (GSequenceIter *iter);
typedef void (*GSequenceFreeFunc) (GSequence *seq);

#include <gio/gio.h>
#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include <dlfcn.h>

struct _EphyAboutHandler {
  GObject parent_instance;

  EphySMaps *smaps;
};

G_DEFINE_FINAL_TYPE (EphyAboutHandler, ephy_about_handler, G_TYPE_OBJECT)


#define EPHY_ABOUT_OVERVIEW_MAX_ITEMS 9

#define EPHY_PAGE_TEMPLATE_ABOUT_CSS        "ephy-resource:///org/gnome/epiphany/page-templates/about.css"

static void
ephy_about_handler_finalize (GObject *object)
{
  EphyAboutHandler *handler = EPHY_ABOUT_HANDLER (object);

  g_clear_object (&handler->smaps);

  G_OBJECT_CLASS (ephy_about_handler_parent_class)->finalize (object);
}

static void
ephy_about_handler_init (EphyAboutHandler *handler)
{
}

static void
ephy_about_handler_class_init (EphyAboutHandlerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = ephy_about_handler_finalize;
}

static EphySMaps *
ephy_about_handler_get_smaps (EphyAboutHandler *handler)
{
  if (!handler->smaps)
    handler->smaps = ephy_smaps_new ();

  return handler->smaps;
}

static void
ephy_about_handler_finish_request (WebKitURISchemeRequest *request,
                                   gchar                  *data,
                                   gssize                  data_length)
{
  GInputStream *stream;

  data_length = data_length != -1 ? data_length : (gssize)strlen (data);
  stream = g_memory_input_stream_new_from_data (data, data_length, g_free);
  webkit_uri_scheme_request_finish (request, stream, data_length, "text/html");
  g_object_unref (stream);
}

static void
handle_memory_finished_cb (EphyAboutHandler       *handler,
                           GAsyncResult           *result,
                           WebKitURISchemeRequest *request)
{
  GString *data_str;
  gsize data_length;
  char *memory;

  data_str = g_string_new ("<html>");

  memory = g_task_propagate_pointer (G_TASK (result), NULL);
  if (memory) {
    g_string_append_printf (data_str, "<head><title>%s</title>"
                            "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\" />"
                            "<link href=\""EPHY_PAGE_TEMPLATE_ABOUT_CSS "\" rel=\"stylesheet\" type=\"text/css\">"
                            "</head><body>"
                            "<div id='memory'>",
                            _("Memory usage"));

    g_string_append_printf (data_str, "<h1>%s</h1>", _("Memory usage"));
    g_string_append (data_str, memory);
    g_free (memory);

    g_string_append (data_str, "</div>");
  }

  g_string_append (data_str, "</html>");

  data_length = data_str->len;
  ephy_about_handler_finish_request (request, g_string_free (data_str, FALSE), data_length);
  g_object_unref (request);
}

static void
handle_memory_sync (GTask        *task,
                    gpointer      source_object,
                    gpointer      task_data,
                    GCancellable *cancellable)
{
  EphyAboutHandler *handler = EPHY_ABOUT_HANDLER (source_object);

  g_task_return_pointer (task,
                         ephy_smaps_to_html (ephy_about_handler_get_smaps (handler)),
                         g_free);
}

static gboolean
ephy_about_handler_handle_memory (EphyAboutHandler       *handler,
                                  WebKitURISchemeRequest *request)
{
  GTask *task;

  task = g_task_new (handler, NULL,
                     (GAsyncReadyCallback)handle_memory_finished_cb,
                     g_object_ref (request));
  g_task_run_in_thread (task, handle_memory_sync);
  g_object_unref (task);

  return TRUE;
}

static gboolean
ephy_about_handler_handle_about (EphyAboutHandler       *handler,
                                 WebKitURISchemeRequest *request)
{
  char *data;
  char *version;
  g_autofree char *path = NULL;
  GtkIconTheme *icon_theme;
  g_autoptr (GtkIconPaintable) paintable = NULL;

  version = g_strdup_printf (_("Version %s"), VERSION);

  icon_theme = gtk_icon_theme_get_for_display (gdk_display_get_default ());
  paintable = gtk_icon_theme_lookup_icon (icon_theme,
                                          APPLICATION_ID,
                                          NULL,
                                          256,
                                          1,
                                          GTK_TEXT_DIR_LTR,
                                          GTK_ICON_LOOKUP_FORCE_REGULAR);

  if (paintable) {
    g_autoptr (GFile) file = gtk_icon_paintable_get_file (paintable);

    path = g_file_get_path (file);
  }

  data = g_strdup_printf ("<html><head><title>%s</title>"
                          "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\" />"
                          "<link href=\""EPHY_PAGE_TEMPLATE_ABOUT_CSS "\" rel=\"stylesheet\" type=\"text/css\">"
                          "</head><body>"
                          "<div id=\"about-app\">"
                          "<div class=\"dialog\">"
                          "<img id=\"about-icon\" src=\"file://%s\"/>"
                          "<h1 id=\"about-title\">%s</h1>"
                          "<h2 id=\"about-subtitle\">%s</h2>"
                          "<p id=\"about-tagline\">%s</p>"
                          "<table class=\"properties\">"
                          "<tr><td class=\"prop-label\">%s</td><td class=\"prop-value\">%d.%d.%d</td></tr>"
                          "</table>"
                          "</div></div></body></html>",
                          _("About Pafari"),
                          path ? path : "",
#if !TECH_PREVIEW
                          _("Web"),
#else
                          _("Pafari Technology Preview"),
#endif
                          version,
                          _("A simple, clean, beautiful view of the web"),
                          "WebKitGTK", webkit_get_major_version (), webkit_get_minor_version (), webkit_get_micro_version ());
  g_free (version);

  ephy_about_handler_finish_request (request, data, -1);

  return TRUE;
}

static gboolean
ephy_about_handler_handle_epiphany (EphyAboutHandler       *handler,
                                    WebKitURISchemeRequest *request)
{
  char *data;

  data = g_strdup_printf ("<html class=\"epiphany-html\"><head><title>%s</title>"
                          "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\" />"
                          "<link href=\""EPHY_PAGE_TEMPLATE_ABOUT_CSS "\" rel=\"stylesheet\" type=\"text/css\">"
                          "</head><body class=\"epiphany-body\">"
                          "<div id=\"ephytext\">"
                          "« Il semble que la perfection soit atteinte non quand il n'y a plus rien à"
                          " ajouter, mais quand il n'y a plus rien à retrancher. »"
                          "</div>"
                          "<div id=\"from\">"
                          "<!-- Terre des Hommes, III: L'Avion, p. 60 -->"
                          "Antoine de Saint-Exupéry"
                          "</div></body></html>",
                          _("Web"));

  ephy_about_handler_finish_request (request, data, -1);

  return TRUE;
}

static void
handle_applications_finished_cb (EphyAboutHandler       *handler,
                                 GAsyncResult           *result,
                                 WebKitURISchemeRequest *request)
{
  WebKitWebView *view;
  GString *data_str;
  gsize data_length;
  GList *applications, *p;

  view = webkit_uri_scheme_request_get_web_view (request);
  ephy_web_view_register_message_handler (EPHY_WEB_VIEW (view), EPHY_WEB_VIEW_ABOUT_APPS_MESSAGE_HANDLER, EPHY_WEB_VIEW_REGISTER_MESSAGE_HANDLER_FOR_CURRENT_PAGE);

  data_str = g_string_new (NULL);
  applications = g_task_propagate_pointer (G_TASK (result), NULL);

  if (g_list_length (applications) > 0) {
    g_string_append_printf (data_str, "<html><head><title>%s</title>"
                            "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\" />"
                            "<link href=\""EPHY_PAGE_TEMPLATE_ABOUT_CSS "\" rel=\"stylesheet\" type=\"text/css\">"
                            "<script>"
                            "  function launchWebApp(appID, appName) {"
                            "    window.webkit.messageHandlers.aboutApps.postMessage({action: 'launch', app: appID, name: appName, page: %" G_GUINT64_FORMAT "});"
                            "  }"
                            "  function deleteWebApp(appID, appName) {"
                            "    window.webkit.messageHandlers.aboutApps.postMessage({action: 'remove', app: appID, name: appName, page: %" G_GUINT64_FORMAT "});"
                            "  }"
                            "</script>"
                            "</head><div id=\"applications\"><body class=\"applications-body\"><h1>%s</h1>"
                            "<p>%s</p>",
                            _("Apps"),
                            webkit_web_view_get_page_id (view),
                            webkit_web_view_get_page_id (view),
                            _("Apps"),
                            _("List of installed web apps"));

    g_string_append (data_str, "<table>");

    for (p = applications; p; p = p->next) {
      EphyWebApplication *app = (EphyWebApplication *)p->data;
      const char *icon_path = NULL;
      g_autofree char *encoded_icon_path = NULL;
      g_autofree char *encoded_name = NULL;
      g_autofree char *encoded_url = NULL;
      g_autoptr (GDate) date = NULL;
      char install_date[128];

      if (ephy_web_application_is_system (app))
        continue;

      date = g_date_new ();
      g_date_set_time_t (date, (time_t)app->install_date_uint64);
      g_date_strftime (install_date, 127, "%x", date);

      /* In the sandbox we don't have access to the host side icon file */
      if (ephy_is_running_inside_sandbox ())
        icon_path = app->tmp_icon_path;
      else
        icon_path = app->icon_path;

      if (!icon_path) {
        g_warning ("Failed to get icon path for app %s", app->id);
        continue;
      }

      /* Most of these fields are at least semi-trusted. The app ID was chosen
       * by ephy so it's safe. The icon URL could be changed by the user to
       * something else after web app creation, though, so better not fully
       * trust it. Then the app name and the main URL could contain anything
       * at all, so those need to be encoded for sure. Install date should be
       * fine because it's constructed by Pafari.
       */
      encoded_icon_path = ephy_encode_for_html_attribute (icon_path);
      encoded_name = ephy_encode_for_html_entity (app->name);
      encoded_url = ephy_encode_for_html_entity (app->url);
      g_string_append_printf (data_str,
                              "<tbody><tr id =\"%s\">"
                              "<td class=\"icon\"><img width=64 height=64 src=\"file://%s\"></img></td>"
                              "<td class=\"data\"><div class=\"appname\">%s</div><div class=\"appurl\">%s</div></td>"
                              "<td class=\"input\"><input type=\"button\" value=\"%s\" "
                              "onclick=\"const appRow = this.closest('tr'); launchWebApp(appRow.id, appRow.querySelector('.appname').innerText);\" "
                              "class=\"suggested-action\"></td>  "
                              "<td class=\"input\"><input type=\"button\" value=\"%s\" "
                              "onclick=\"const appRow = this.closest('tr'); deleteWebApp(appRow.id, appRow.querySelector('.appname').innerText);\" "
                              "class=\"destructive-action\"></td>"
                              "<td class=\"date\">%s <br /> %s</td></tr></tbody>",
                              app->id, encoded_icon_path, encoded_name, encoded_url, _("Launch"), _("Delete"),
                              /* Note for translators: this refers to the installation date. */
                              _("Installed on:"), install_date);
    }

    g_string_append (data_str, "</table></div></body></html>");
  } else {
    GtkIconTheme *icon_theme;
    g_autoptr (GtkIconPaintable) paintable = NULL;
    g_autofree char *path = NULL;

    g_string_append_printf (data_str, "<html><head><title>%s</title>"
                            "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\" />"
                            "<link href=\""EPHY_PAGE_TEMPLATE_ABOUT_CSS "\" rel=\"stylesheet\" type=\"text/css\">"
                            "</head><body class=\"applications-body\">",
                            _("Apps"));

    icon_theme = gtk_icon_theme_get_for_display (gdk_display_get_default ());
    paintable = gtk_icon_theme_lookup_icon (icon_theme,
                                            "application-x-addon-symbolic",
                                            NULL,
                                            128,
                                            1,
                                            GTK_TEXT_DIR_LTR,
                                            0);

    if (paintable) {
      g_autoptr (GFile) file = gtk_icon_paintable_get_file (paintable);

      path = g_file_get_path (file);
    }

    g_string_append_printf (data_str,
                            "  <div id=\"overview\" class=\"overview-empty\">\n"
                            "    <img src=\"file://%s\"/>\n"
                            "    <div><h1>%s</h1></div>\n"
                            "    <div><p>%s</p></div>\n"
                            "  </div>\n"
                            "</body></html>\n",
                            path ? path : "",
                            /* Displayed when opening applications without any installed web apps. */
                            _("Apps"), _("You can add your favorite website by clicking <b>Install as Web App…</b> within the page menu."));
  }

  ephy_web_application_free_application_list (applications);

  data_length = data_str->len;
  ephy_about_handler_finish_request (request, g_string_free (data_str, FALSE), data_length);
  g_object_unref (request);
}

static void
handle_applications_sync (GTask        *task,
                          gpointer      source_object,
                          gpointer      task_data,
                          GCancellable *cancellable)
{
  g_task_return_pointer (task,
                         ephy_web_application_get_application_list (),
                         (GDestroyNotify)ephy_web_application_free_application_list);
}

static gboolean
ephy_about_handler_handle_applications (EphyAboutHandler       *handler,
                                        WebKitURISchemeRequest *request)
{
  GTask *task;

  task = g_task_new (handler, NULL,
                     (GAsyncReadyCallback)handle_applications_finished_cb,
                     g_object_ref (request));
  g_task_run_in_thread (task, handle_applications_sync);
  g_object_unref (task);

  return TRUE;
}

typedef struct {
  WebKitURISchemeRequest *request;
  GList *urls;
  gboolean success;
  guint itp_count;
  GHashTable *tracker_table;
  GHashTable *website_table;
} OverviewData;

static void
itp_summary_ready (GObject      *source_object,
                   GAsyncResult *res,
                   gpointer      user_data);

static void
history_service_query_urls_cb (EphyHistoryService     *history,
                               gboolean                success,
                               GList                  *urls,
                               WebKitURISchemeRequest *request)
{
  OverviewData *data;
  WebKitWebsiteDataManager *data_manager;
  WebKitNetworkSession *network_session;
  EphyEmbedShell *shell;

  data = g_new0 (OverviewData, 1);
  data->request = g_object_ref (request);
  data->urls = urls ? ephy_history_url_list_copy (urls) : NULL;
  data->success = success;
  data->itp_count = 0;
  data->tracker_table = NULL;
  data->website_table = NULL;

  shell = ephy_embed_shell_get_default ();
  network_session = ephy_embed_shell_get_network_session (shell);
  data_manager = webkit_network_session_get_website_data_manager (network_session);

  webkit_website_data_manager_get_itp_summary (data_manager, NULL, itp_summary_ready, data);
}

static void
itp_summary_ready (GObject      *source_object,
                   GAsyncResult *res,
                   gpointer      user_data)
{
  OverviewData *data = (OverviewData *)user_data;
  WebKitWebsiteDataManager *manager = WEBKIT_WEBSITE_DATA_MANAGER (source_object);
  g_autolist (WebKitITPThirdParty) summary = NULL;
  g_autoptr (GError) error = NULL;
  WebKitWebView *view;
  EphySnapshotService *snapshot_service;
  EphyEmbedShell *shell;
  GString *data_str;
  gsize data_length;
  char *lang;
  GList *l;
  guint list_length;
  g_autofree char *privacy_description = NULL;

  summary = webkit_website_data_manager_get_itp_summary_finish (manager, res, &error);
  if (error) {
    g_warning ("Could not fetch ITP summary: %s", error->message);
    data->itp_count = 0;
    data->tracker_table = NULL;
    data->website_table = NULL;
  } else {
    GHashTable *tracker_table;
    GHashTable *website_table;
    GList *tp_list;
    
    data->itp_count = g_list_length (summary);
    
    /* Create website and tracker tables similar to privacy report */
    tracker_table = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
    website_table = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
    
    for (tp_list = summary; tp_list && tp_list->data; tp_list = tp_list->next) {
      WebKitITPThirdParty *tp = (WebKitITPThirdParty *)(tp_list->data);
      GList *fp_list;
      
      for (fp_list = webkit_itp_third_party_get_first_parties (tp); fp_list && fp_list->data; fp_list = fp_list->next) {
        WebKitITPFirstParty *fp = (WebKitITPFirstParty *)(fp_list->data);
        
        if (!webkit_itp_first_party_get_website_data_access_allowed (fp)) {
          const char *fp_domain = webkit_itp_first_party_get_domain (fp);
          const char *tp_domain = webkit_itp_third_party_get_domain (tp);
          GPtrArray *websites = NULL;
          GPtrArray *trackers = NULL;
          
          /* Websites */
          if (g_hash_table_lookup_extended (website_table, fp_domain, NULL, (gpointer *)&websites)) {
            g_ptr_array_add (websites, g_strdup (tp_domain));
          } else {
            websites = g_ptr_array_new_with_free_func (g_free);
            g_ptr_array_add (websites, g_strdup (tp_domain));
            g_hash_table_insert (website_table, g_strdup (fp_domain), websites);
          }
          
          /* Tracker */
          if (g_hash_table_lookup_extended (tracker_table, tp_domain, NULL, (gpointer *)&trackers)) {
            g_ptr_array_add (trackers, g_strdup (fp_domain));
          } else {
            trackers = g_ptr_array_new_with_free_func (g_free);
            g_ptr_array_add (trackers, g_strdup (fp_domain));
            g_hash_table_insert (tracker_table, g_strdup (tp_domain), trackers);
          }
        }
      }
    }
    
    data->tracker_table = tracker_table;
    data->website_table = website_table;
  }

  view = webkit_uri_scheme_request_get_web_view (data->request);
  ephy_web_view_register_message_handler (EPHY_WEB_VIEW (view), EPHY_WEB_VIEW_PRIVACY_REPORT_MESSAGE_HANDLER, EPHY_WEB_VIEW_REGISTER_MESSAGE_HANDLER_FOR_CURRENT_PAGE);

  snapshot_service = ephy_snapshot_service_get_default ();
  shell = ephy_embed_shell_get_default ();

  data_str = g_string_new (NULL);

  lang = g_strdup (pango_language_to_string (gtk_get_default_language ()));
  g_strdelimit (lang, "_-@", '\0');

  g_string_append_printf (data_str,
                          "<html xml:lang=\"%s\" lang=\"%s\" dir=\"%s\">\n"
                          "<head>\n"
                          "  <title>%s</title>\n"
                          "  <meta http-equiv=\"content-type\" content=\"text/html; charset=utf-8\" />\n"
                          "  <meta name=\"viewport\" content=\"width=device-width\">"
                          "  <link href=\""EPHY_PAGE_TEMPLATE_ABOUT_CSS "\" rel=\"stylesheet\" type=\"text/css\">\n"
                          "  <script>\n"
                          "    document.addEventListener('DOMContentLoaded', function() {\n"
                          "      document.addEventListener('click', function(e) {\n"
                          "        if (e.target && e.target.classList.contains('overview-close-button')) {\n"
                          "          e.preventDefault();\n"
                          "          e.stopPropagation();\n"
                          "          var item = e.target.closest('.overview-item');\n"
                          "          if (item && item.href && window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.overview) {\n"
                          "            window.webkit.messageHandlers.overview.postMessage(item.href);\n"
                          "            item.classList.add('overview-removed');\n"
                          "            setTimeout(function() { if (item.parentNode) item.parentNode.removeChild(item); }, 250);\n"
                          "          }\n"
                          "          return false;\n"
                          "        }\n"
                          "      }, true);\n"
                          "    });\n"
                          "  </script>\n"
                          "</head>\n"
                          "<body>\n",
                          lang, lang,
                          ((gtk_widget_get_default_direction () == GTK_TEXT_DIR_RTL) ? "rtl" : "ltr"),
                          _(NEW_TAB_PAGE_TITLE));
  g_free (lang);

  list_length = g_list_length (data->urls);

  /* Generate privacy description and details */
  {
    guint tracker_count = 0;
    guint website_count = 0;
    g_autofree char *trackers_html = NULL;
    g_autofree char *websites_html = NULL;
    GString *trackers_str = NULL;
    GString *websites_str = NULL;
    GHashTableIter iter;
    gpointer key, value;
    guint i;
    
    if (data->itp_count > 0 && data->tracker_table && data->website_table) {
      tracker_count = g_hash_table_size (data->tracker_table);
      website_count = g_hash_table_size (data->website_table);
      
      privacy_description = g_strdup_printf (ngettext ("Pafari prevented %u tracker from profiling you", "Pafari prevented %u trackers from profiling you", data->itp_count), data->itp_count);
      
      /* Generate trackers list (max 5) */
      trackers_str = g_string_new (NULL);
      i = 0;
      g_hash_table_iter_init (&iter, data->tracker_table);
      while (g_hash_table_iter_next (&iter, &key, &value) && i < 5) {
        const char *tracker_domain = (const char *)key;
        g_autofree char *escaped = g_markup_escape_text (tracker_domain, -1);
        if (i > 0)
          g_string_append (trackers_str, ", ");
        g_string_append (trackers_str, escaped);
        i++;
      }
      if (tracker_count > 5) {
        g_autofree char *more = g_strdup_printf (_(" and %u more"), tracker_count - 5);
        g_string_append (trackers_str, more);
      }
      trackers_html = g_string_free (trackers_str, FALSE);
      
      /* Generate websites list (max 5) */
      websites_str = g_string_new (NULL);
      i = 0;
      g_hash_table_iter_init (&iter, data->website_table);
      while (g_hash_table_iter_next (&iter, &key, &value) && i < 5) {
        const char *website_domain = (const char *)key;
        g_autofree char *escaped = g_markup_escape_text (website_domain, -1);
        if (i > 0)
          g_string_append (websites_str, ", ");
        g_string_append (websites_str, escaped);
        i++;
      }
      if (website_count > 5) {
        g_autofree char *more = g_strdup_printf (_(" and %u more"), website_count - 5);
        g_string_append (websites_str, more);
      }
      websites_html = g_string_free (websites_str, FALSE);
    } else {
      privacy_description = g_strdup (_("No trackers blocked yet"));
      trackers_html = g_strdup ("");
      websites_html = g_strdup ("");
    }
    
    /* Generate detail HTML for trackers and websites */
    {
      g_autofree char *trackers_detail_html = NULL;
      g_autofree char *websites_detail_html = NULL;
    
    if (trackers_html && trackers_html[0]) {
      g_autofree char *trackers_label = g_markup_escape_text (_("Trackers"), -1);
      trackers_detail_html = g_strdup_printf ("<p class=\"privacy-report-detail\"><strong>%s:</strong> %s</p>", trackers_label, trackers_html);
    } else {
      trackers_detail_html = g_strdup ("");
    }
    
    if (websites_html && websites_html[0]) {
      g_autofree char *websites_label = g_markup_escape_text (_("Websites"), -1);
      websites_detail_html = g_strdup_printf ("<p class=\"privacy-report-detail\"><strong>%s:</strong> %s</p>", websites_label, websites_html);
    } else {
      websites_detail_html = g_strdup ("");
    }

  /* Generate new start page with Favorites, Privacy Report, and Bookmarks */
  {
    g_autofree char *favorites_title = NULL;
    g_autofree char *privacy_report_title = NULL;
    g_autofree char *privacy_description_escaped = NULL;
    g_autofree char *show_more_text = NULL;
    g_autofree char *bookmarks_title = NULL;
    g_autofree char *most_visited_title = NULL;
    char *bookmarks_html = NULL;
    char *favorites_html = NULL;

    favorites_title = g_markup_escape_text (_("Favorites"), -1);
    privacy_report_title = g_markup_escape_text (_("Privacy Report"), -1);
    privacy_description_escaped = g_markup_escape_text (privacy_description, -1);
    show_more_text = g_markup_escape_text (_("Show more"), -1);
    bookmarks_title = g_markup_escape_text (_("Bookmarks"), -1);
    most_visited_title = g_markup_escape_text (_("Most Visited"), -1);
    
    /* Generate favorites HTML */
    {
      GString *favorites_str = NULL;
      const char *favorites_data[][2] = {
        { "https://pearos.xyz", "pearOS" },
        { "https://google.com", "Google" },
        { "https://youtube.com", "Youtube" },
        { "https://mail.google.com", "Gmail" }
      };
      int i;
      
      favorites_str = g_string_new (NULL);
      
      for (i = 0; i < 4; i++) {
        g_autofree char *encoded_url = NULL;
        g_autofree char *encoded_title = NULL;
        g_autofree char *escaped_title = NULL;
        
        encoded_url = ephy_encode_for_html_attribute (favorites_data[i][0]);
        encoded_title = ephy_encode_for_html_attribute (favorites_data[i][1]);
        escaped_title = ephy_encode_for_html_entity (favorites_data[i][1]);
        
        g_string_append_printf (favorites_str,
                                "<a class=\"overview-item\" title=\"%s\" href=\"%s\">"
                                "  <iframe class=\"overview-thumbnail\" src=\"%s\" loading=\"lazy\" sandbox=\"allow-same-origin allow-scripts\"></iframe>"
                                "  <span class=\"overview-title\">%s</span>"
                                "</a>",
                                encoded_title, encoded_url, encoded_url, escaped_title);
      }
      
      favorites_html = g_string_free (favorites_str, FALSE);
    }
    
    /* Generate bookmarks HTML */
    {
      GString *bookmarks_str = NULL;
      EphyEmbedShell *embed_shell;
      EphyShell *shell;
      EphyBookmarksManager *bookmarks_manager;
      GSequence *bookmarks;
      GSequenceIter *iter;
      void *handle;
      EphyShellGetBookmarksManagerFunc get_bookmarks_manager;
      EphyBookmarksManagerGetBookmarksWithTagFunc get_bookmarks_with_tag;
      EphyBookmarkGetUrlFunc bookmark_get_url;
      EphyBookmarkGetTitleFunc bookmark_get_title;
      GSequenceGetBeginIterFunc sequence_get_begin_iter;
      GSequenceIterIsEndFunc sequence_iter_is_end;
      GSequenceIterNextFunc sequence_iter_next;
      GSequenceGetFunc sequence_get;
      GSequenceFreeFunc sequence_free;
      
      bookmarks_str = g_string_new (NULL);
      embed_shell = ephy_embed_shell_get_default ();
      
      /* Try to cast to EphyShell - it's a subclass of EphyEmbedShell */
      shell = (EphyShell *)embed_shell;
      
      /* Load function pointers using dlsym */
      handle = dlopen (NULL, RTLD_LAZY);
      if (handle) {
        get_bookmarks_manager = (EphyShellGetBookmarksManagerFunc) dlsym (handle, "ephy_shell_get_bookmarks_manager");
        get_bookmarks_with_tag = (EphyBookmarksManagerGetBookmarksWithTagFunc) dlsym (handle, "ephy_bookmarks_manager_get_bookmarks_with_tag");
        bookmark_get_url = (EphyBookmarkGetUrlFunc) dlsym (handle, "ephy_bookmark_get_url");
        bookmark_get_title = (EphyBookmarkGetTitleFunc) dlsym (handle, "ephy_bookmark_get_title");
        sequence_get_begin_iter = (GSequenceGetBeginIterFunc) dlsym (handle, "g_sequence_get_begin_iter");
        sequence_iter_is_end = (GSequenceIterIsEndFunc) dlsym (handle, "g_sequence_iter_is_end");
        sequence_iter_next = (GSequenceIterNextFunc) dlsym (handle, "g_sequence_iter_next");
        sequence_get = (GSequenceGetFunc) dlsym (handle, "g_sequence_get");
        sequence_free = (GSequenceFreeFunc) dlsym (handle, "g_sequence_free");
        
        if (get_bookmarks_manager && get_bookmarks_with_tag && bookmark_get_url && bookmark_get_title &&
            sequence_get_begin_iter && sequence_iter_is_end && sequence_iter_next && sequence_get && sequence_free) {
          bookmarks_manager = get_bookmarks_manager (shell);
          if (bookmarks_manager) {
            bookmarks = get_bookmarks_with_tag (bookmarks_manager, NULL);
            
            for (iter = sequence_get_begin_iter (bookmarks);
                 !sequence_iter_is_end (iter);
                 iter = sequence_iter_next (iter)) {
              EphyBookmark *bookmark = (EphyBookmark *)sequence_get (iter);
              const char *url = bookmark_get_url (bookmark);
              const char *title = bookmark_get_title (bookmark);
              g_autofree char *encoded_url = NULL;
              g_autofree char *encoded_title = NULL;
              g_autofree char *escaped_title = NULL;
              
              if (!url || !title)
                continue;
              
              encoded_url = ephy_encode_for_html_attribute (url);
              encoded_title = ephy_encode_for_html_attribute (title);
              escaped_title = ephy_encode_for_html_entity (title);
              
              g_string_append_printf (bookmarks_str,
                                      "<a class=\"overview-item\" title=\"%s\" href=\"%s\">"
                                      "  <span class=\"overview-thumbnail\"></span>"
                                      "  <span class=\"overview-title\">%s</span>"
                                      "</a>",
                                      encoded_title, encoded_url, escaped_title);
            }
            
            sequence_free (bookmarks);
          }
        }
        
        dlclose (handle);
      }
      
      bookmarks_html = g_string_free (bookmarks_str, FALSE);
    }

    g_string_append_printf (data_str,
                            "<div id=\"overview\" class=\"start-page\">\n"
                            "  <div class=\"start-page-section\">\n"
                            "    <h2 class=\"start-page-title\">%s</h2>\n"
                            "    <div id=\"favorites-grid\" class=\"bookmarks-grid\">\n"
                            "%s"
                            "    </div>\n"
                            "  </div>\n"
                            "  <div class=\"start-page-section\">\n"
                            "    <h2 class=\"start-page-title\">%s</h2>\n"
                            "    <div class=\"privacy-report-card\">\n"
                            "      <div class=\"privacy-report-header\">\n"
                            "        <svg class=\"privacy-report-shield\" width=\"64\" height=\"64\" viewBox=\"0 0 16 16\" xmlns=\"http://www.w3.org/2000/svg\"><g fill=\"currentColor\"><path d=\"m 0 2.316406 v 5.507813 c 0 2.214843 1.183594 4.257812 3.109375 5.355469 l 4.890625 2.796874 l 4.890625 -2.796874 c 1.925781 -1.097657 3.109375 -3.140626 3.109375 -5.355469 v -5.507813 l -8 -2.285156 z m 14.726562 1.71875 l -0.726562 -0.964844 v 4.753907 c 0 1.492187 -0.804688 2.878906 -2.101562 3.617187 l -4.394532 2.511719 h 0.992188 l -4.394532 -2.511719 c -1.296874 -0.738281 -2.101562 -2.125 -2.101562 -3.617187 v -4.753907 l -0.726562 0.964844 l 7 -2 h -0.546876 z m 0 0\"/><path d=\"m 5.46875 7.78125 l 2 2 c 0.292969 0.292969 0.769531 0.292969 1.0625 0 l 3 -3 c 0.292969 -0.292969 0.292969 -0.769531 0 -1.0625 s -0.769531 -0.292969 -1.0625 0 l -3 3 h 1.0625 l -2 -2 c -0.292969 -0.292969 -0.769531 -0.292969 -1.0625 0 s -0.292969 0.769531 0 1.0625 z m 0 0\"/></g></svg>\n"
                            "        <div class=\"privacy-report-stats\">\n"
                            "          <p class=\"privacy-report-main-text\">%s</p>\n"
                            "%s%s"
                            "        </div>\n"
                            "      </div>\n"
                            "      <a href=\"#\" onclick=\"window.webkit.messageHandlers.privacyReport.postMessage({}); return false;\" class=\"privacy-report-show-more\">%s</a>\n"
                            "    </div>\n"
                            "    <div class=\"privacy-promotions-grid\">\n"
                            "      <a class=\"overview-item\" title=\"Get NordVPN\" href=\"https://go.nordvpn.net/aff_c?offer_id=15&aff_id=136731&url_id=902\">\n"
                            "        <div class=\"overview-thumbnail\" style=\"background: white; display: flex; align-items: center; justify-content: center; padding: 12px;\">\n"
                            "          <img src=\"https://ic.nordcdn.com/v1/https://sb.nordcdn.com/m/1431cb1f1a5ca2c9/original/nordvpn-default.svg\" alt=\"NordVPN\" style=\"max-width: 100%%; max-height: 100%%; object-fit: contain;\">\n"
                            "        </div>\n"
                            "        <span class=\"overview-title\">Get NordVPN</span>\n"
                            "      </a>\n"
                            "      <a class=\"overview-item\" title=\"Get NordPass\" href=\"https://go.nordpass.io/aff_c?offer_id=488&aff_id=136731&url_id=9356\">\n"
                            "        <div class=\"overview-thumbnail\" style=\"background: white; display: flex; align-items: center; justify-content: center; padding: 12px;\">\n"
                            "          <img src=\"https://sb.nordcdn.com/transform/06a0b074-f89f-41d6-90e9-e70fc533c948/nordpass-vertical-logo?format=webp&quality=80&io=transform%%3Afill%%2Cwidth%%3A360\" alt=\"NordPass\" style=\"max-width: 100%%; max-height: 100%%; object-fit: contain;\">\n"
                            "        </div>\n"
                            "        <span class=\"overview-title\">Get NordPass</span>\n"
                            "      </a>\n"
                            "    </div>\n"
                            "  </div>\n"
                            "  <div class=\"start-page-section\">\n"
                            "    <h2 class=\"start-page-title\">%s</h2>\n"
                            "    <div id=\"bookmarks-grid\" class=\"bookmarks-grid\">\n"
                            "%s"
                            "    </div>\n"
                            "  </div>\n",
                            favorites_title, favorites_html ? favorites_html : "",
                            privacy_report_title, privacy_description_escaped, 
                            trackers_detail_html ? trackers_detail_html : "",
                            websites_detail_html ? websites_detail_html : "",
                            show_more_text, bookmarks_title,
                            bookmarks_html ? bookmarks_html : "");

      /* Free HTML strings - they are now copied into data_str */
      g_free (bookmarks_html);
      g_free (favorites_html);
      bookmarks_html = NULL;
      favorites_html = NULL;

      if (list_length > 0 && data->success) {
        /* Also show most visited if available */
        g_string_append_printf (data_str,
                                "  <div class=\"start-page-section\">\n"
                                "    <h2 class=\"start-page-title\">%s</h2>\n"
                                "    <div id=\"most-visited-grid\" class=\"bookmarks-grid\">\n",
                                most_visited_title);
      } else {
        g_string_append (data_str, "</div>\n</body></html>\n");
        data_length = data_str->len;
        ephy_about_handler_finish_request (data->request, g_string_free (data_str, FALSE), data_length);
        g_object_unref (data->request);
        ephy_history_url_list_free (data->urls);
        if (data->tracker_table)
          g_hash_table_unref (data->tracker_table);
        if (data->website_table)
          g_hash_table_unref (data->website_table);
        g_free (data);
        return;
      }
    }
    }
  }

  for (l = data->urls; l; l = g_list_next (l)) {
    EphyHistoryURL *url = (EphyHistoryURL *)l->data;
    const char *snapshot;
    g_autofree char *thumbnail_style = NULL;
    g_autofree char *entity_encoded_title = NULL;
    g_autofree char *attribute_encoded_title = NULL;
    g_autofree char *encoded_url = NULL;

    snapshot = ephy_snapshot_service_lookup_cached_snapshot_path (snapshot_service, url->url);
    if (snapshot)
      thumbnail_style = g_strdup_printf (" style=\"background: url(file://%s) no-repeat; background-size: 100%%;\"", snapshot);
    else
      ephy_embed_shell_schedule_thumbnail_update (shell, url);

    /* Title and URL are controlled by web content and could be malicious. */
    entity_encoded_title = ephy_encode_for_html_entity (url->title);
    attribute_encoded_title = ephy_encode_for_html_attribute (url->title);
    encoded_url = ephy_encode_for_html_attribute (url->url);
    g_string_append_printf (data_str,
                            "<a class=\"overview-item\" title=\"%s\" href=\"%s\">"
                            "  <div class=\"overview-close-button\" title=\"%s\"></div>"
                            "  <span class=\"overview-thumbnail\"%s></span>"
                            "  <span class=\"overview-title\">%s</span>"
                            "</a>",
                            attribute_encoded_title, encoded_url, _("Remove from overview"),
                            thumbnail_style ? thumbnail_style : "",
                            entity_encoded_title);
  }

  g_string_append (data_str,
                   "    </div>\n"
                   "  </div>\n"
                   "</div>\n"
                   "</body></html>\n");

  data_length = data_str->len;
  ephy_about_handler_finish_request (data->request, g_string_free (data_str, FALSE), data_length);
  g_object_unref (data->request);
  ephy_history_url_list_free (data->urls);
  if (data->tracker_table)
    g_hash_table_unref (data->tracker_table);
  if (data->website_table)
    g_hash_table_unref (data->website_table);
  g_free (data);
}

static gboolean
ephy_about_handler_handle_newtab (EphyAboutHandler       *handler,
                                  WebKitURISchemeRequest *request)
{
  char *data;

  data = g_strdup_printf ("<html><head><title>%s</title>"
                          "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\" />"
                          "</head><body style=\"color-scheme: light dark;\">"
                          "</body></html>",
                          _(NEW_TAB_PAGE_TITLE));

  ephy_about_handler_finish_request (request, data, -1);

  return TRUE;
}

EphyHistoryQuery *
ephy_history_query_new_for_overview (void)
{
  EphyHistoryQuery *query;

  query = ephy_history_query_new ();
  query->sort_type = EPHY_HISTORY_SORT_MOST_VISITED;
  query->limit = EPHY_ABOUT_OVERVIEW_MAX_ITEMS;
  query->ignore_hidden = TRUE;
  query->ignore_local = TRUE;

  return query;
}

static gboolean
ephy_about_handler_handle_html_overview (EphyAboutHandler       *handler,
                                         WebKitURISchemeRequest *request)
{
  EphyHistoryService *history;
  EphyHistoryQuery *query;

  history = ephy_embed_shell_get_global_history_service (ephy_embed_shell_get_default ());
  query = ephy_history_query_new_for_overview ();
  ephy_history_service_query_urls (history, query, NULL,
                                   (EphyHistoryJobCallback)history_service_query_urls_cb,
                                   g_object_ref (request));
  ephy_history_query_free (query);

  return TRUE;
}

static gboolean
ephy_about_handler_handle_incognito (EphyAboutHandler       *handler,
                                     WebKitURISchemeRequest *request)
{
  char *data;

  if (ephy_embed_shell_get_mode (ephy_embed_shell_get_default ()) != EPHY_EMBED_SHELL_MODE_INCOGNITO)
    return FALSE;

  data = g_strdup_printf ("<html>\n"
                          "<div dir=\"%s\">\n"
                          "<head>\n"
                          "<title>%s</title>\n"
                          "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\" />"
                          "<link href=\""EPHY_PAGE_TEMPLATE_ABOUT_CSS "\" rel=\"stylesheet\" type=\"text/css\">\n"
                          "</head>\n"
                          "<body class=\"incognito-body\">\n"
                          "  <img class=\"incognito-body-image\" src=\"ephy-resource:///org/gnome/epiphany/page-icons/private-mode.svg\">\n" \
                          "  <br/>\n"
                          "  <h1>%s</h1>\n"
                          "  <p>%s</p>\n"
                          "  <p><strong>%s</strong> %s</p>\n"
                          "</body>\n"
                          "</div>\n"
                          "</html>\n",
                          gtk_widget_get_default_direction () == GTK_TEXT_DIR_RTL ? "rtl" : "ltr",
                          _("Private Browsing"),
                          _("Private Browsing"),
                          _("You are currently browsing incognito. Pages viewed in this "
                            "mode will not show up in your browsing history and all stored "
                            "information will be cleared when you close the window. Files you "
                            "download will be kept."),
                          _("Incognito mode hides your activity only from people using this "
                            "computer."),
                          _("It will not hide your activity from your employer if you are at "
                            "work. Your internet service provider, your government, other "
                            "governments, the websites that you visit, and advertisers on "
                            "these websites may still be tracking you."));

  ephy_about_handler_finish_request (request, data, -1);

  return TRUE;
}

static void
ephy_about_handler_handle_blank (EphyAboutHandler       *handler,
                                 WebKitURISchemeRequest *request)
{
  ephy_about_handler_finish_request (request, g_strdup ("<html></html>"), -1);
}

EphyAboutHandler *
ephy_about_handler_new (void)
{
  return EPHY_ABOUT_HANDLER (g_object_new (EPHY_TYPE_ABOUT_HANDLER, NULL));
}

void
ephy_about_handler_handle_request (EphyAboutHandler       *handler,
                                   WebKitURISchemeRequest *request)
{
  const char *path;
  gboolean handled = FALSE;

  path = webkit_uri_scheme_request_get_path (request);

  if (!g_strcmp0 (path, "memory"))
    handled = ephy_about_handler_handle_memory (handler, request);
  else if (!g_strcmp0 (path, "epiphany"))
    handled = ephy_about_handler_handle_epiphany (handler, request);
  else if (!g_strcmp0 (path, "applications"))
    handled = ephy_about_handler_handle_applications (handler, request);
  else if (!g_strcmp0 (path, "newtab"))
    handled = ephy_about_handler_handle_newtab (handler, request);
  else if (!g_strcmp0 (path, "overview"))
    handled = ephy_about_handler_handle_html_overview (handler, request);
  else if (!g_strcmp0 (path, "incognito"))
    handled = ephy_about_handler_handle_incognito (handler, request);
  else if (!path || path[0] == '\0' || !g_strcmp0 (path, "Web") || !g_strcmp0 (path, "web"))
    handled = ephy_about_handler_handle_about (handler, request);

  if (!handled)
    ephy_about_handler_handle_blank (handler, request);
}
