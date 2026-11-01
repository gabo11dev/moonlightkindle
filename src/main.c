/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <client.h>
#include <errors.h>
#include <openssl/rand.h>
#include <curl/curl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include "video.h"
#include "geometry.h"
#include "eink.h"

/* Only the GTK thread touches widgets. One joinable worker owns the protocol.
 * A mutex serializes input against connection teardown; frames use a separate
 * single-slot mailbox so a slow e-ink refresh cannot accumulate video latency. */
static GtkWidget *window, *host_entry, *pin_entry, *apps_combo, *controls, *status_label;
static GtkWidget *canvas, *fps_spin, *bitrate_spin, *res_combo, *actions, *stop_button;
static GtkWidget *text_entry, *osk, *typing_target;
static GtkWidget *header, *text_bar, *bottom_bar, *stream_menu_button, *root_box;
static int stream_controls = 1, menu_release_pending;
static GdkPixbuf *picture;
static pthread_t worker;
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t stop_cond = PTHREAD_COND_INITIALIZER;
static int busy, done, connected, starting, stop_requested, closing, is_kindle, smoke, preview;
static volatile sig_atomic_t signal_quit;
static char message[1024] = "Enter the PC address. Pair, then load apps.";
static char *key_dir, *settings_path;
static int app_ids[512], app_count, result_count;
static char *result_names[512];
static int result_ids[512];
static int refresh_pending, frame_w, frame_h;
static enum { JOB_PAIR, JOB_LIST, JOB_STREAM } job_type;
typedef struct { int type, app_id, width, height, fps, bitrate; char *host; char pin[5]; } Job;
extern void gs_destroy(PSERVER_DATA server);

static void set_message(const char *fmt, ...) {
    va_list ap; pthread_mutex_lock(&state_lock);
    va_start(ap, fmt); vsnprintf(message, sizeof(message), fmt, ap); va_end(ap);
    pthread_mutex_unlock(&state_lock);
}
static void log_message(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}
static void stage_failed(int stage, int code) { set_message("%s failed (%d)", LiGetStageName(stage), code); }
static void stage_start(int stage) { set_message("Connecting: %s", LiGetStageName(stage)); }
static void terminated(int code) {
    pthread_mutex_lock(&state_lock);
    connected = 0; stop_requested = 1;
    snprintf(message, sizeof(message), "The host ended the stream (%d)", code);
    pthread_cond_broadcast(&stop_cond); pthread_mutex_unlock(&state_lock);
}
static void request_stop(void) {
    pthread_mutex_lock(&state_lock);
    stop_requested = 1;
    if (starting) LiInterruptConnection();
    pthread_cond_broadcast(&stop_cond);
    pthread_mutex_unlock(&state_lock);
}
static int was_stopped(void) {
    pthread_mutex_lock(&state_lock); int stopped = stop_requested; pthread_mutex_unlock(&state_lock); return stopped;
}
static void free_apps(PAPP_LIST list) {
    while (list) { PAPP_LIST next = list->next; free(list->name); free(list); list = next; }
}
static void *run_job(void *arg) {
    Job *job = arg;
    SERVER_DATA server = {0}; PAPP_LIST list = NULL;
    int ret = gs_init(&server, job->host, 47989, key_dir, 0, true);
    if (ret != GS_OK) { set_message("Connection failed (%d): %s", ret, gs_error ? gs_error : "check the address, Sunshine, and Wi-Fi network"); goto finish; }
    if (was_stopped()) goto finish;
    if (job->type == JOB_PAIR) {
        if (server.paired) set_message("This Kindle is already paired. Load apps.");
        else {
            ret = gs_pair(&server, job->pin);
            if (ret == GS_OK) set_message("Paired. Now select Load apps.");
            else {
                fprintf(stderr, "Pairing failed (%d): %s\n", ret, gs_error ? gs_error : "no details");
                set_message("Pairing error (%d): %s", ret, gs_error ? gs_error : "PIN rejected");
            }
        }
        goto finish;
    }
    if (!server.paired) { set_message("Pair this Kindle with the PC first."); goto finish; }
    ret = gs_applist(&server, &list);
    if (ret != GS_OK) { set_message("Could not load apps (%d): %s", ret, gs_error ? gs_error : "no details"); goto finish; }
    if (job->type == JOB_LIST) {
        int n = 0;
        for (PAPP_LIST p = list; p && n < 512; p = p->next) {
            result_names[n] = g_strdup(p->name); result_ids[n++] = p->id;
        }
        pthread_mutex_lock(&state_lock); result_count = n; pthread_mutex_unlock(&state_lock);
        set_message(n ? "Select an app and choose Start." : "No apps are configured in Sunshine.");
        goto finish;
    }
    /* Do not accept a stale app ID after changing hosts. */
    int found = 0;
    for (PAPP_LIST p = list; p; p = p->next) if (p->id == job->app_id) found = 1;
    if (!found) { set_message("This app is no longer available. Reload the app list."); goto finish; }
    if (was_stopped()) goto finish;
    STREAM_CONFIGURATION config;
    LiInitializeStreamConfiguration(&config);
    config.width = job->width; config.height = job->height; config.fps = job->fps;
    config.bitrate = job->bitrate; config.packetSize = 1024;
    config.streamingRemotely = STREAM_CFG_LOCAL;
    config.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    config.supportedVideoFormats = VIDEO_FORMAT_H264;
    if (RAND_bytes((unsigned char *)config.remoteInputAesKey, sizeof(config.remoteInputAesKey)) != 1 ||
        RAND_bytes((unsigned char *)config.remoteInputAesIv, sizeof(config.remoteInputAesIv)) != 1) {
        set_message("Could not generate session keys."); goto finish;
    }
    ret = gs_start_app(&server, &config, job->app_id, false, true, 0);
    if (ret != GS_OK) { set_message("Could not launch the app (%d): %s", ret, gs_error ? gs_error : "check the resolution and H.264 encoder in Sunshine"); goto finish; }
    if (was_stopped()) goto finish;
    CONNECTION_LISTENER_CALLBACKS callbacks;
    LiInitializeConnectionCallbacks(&callbacks);
    callbacks.stageStarting = stage_start; callbacks.stageFailed = stage_failed;
    callbacks.connectionTerminated = terminated; callbacks.logMessage = log_message;
    pthread_mutex_lock(&state_lock); starting = 1; pthread_mutex_unlock(&state_lock);
    ret = LiStartConnection(&server.serverInfo, &config, &callbacks, &kindle_decoder, NULL, NULL, 0, NULL, 0);
    pthread_mutex_lock(&state_lock); starting = 0;
    if (ret == 0 && !stop_requested) {
        connected = 1;
        snprintf(message, sizeof(message), "Streaming H.264 · %dx%d · %d fps. Tap to click.", job->width, job->height, job->fps);
        while (!stop_requested) pthread_cond_wait(&stop_cond, &state_lock);
    } else if (ret != 0 && !stop_requested) {
        snprintf(message, sizeof(message), "Could not start streaming (%d). See moonlight.log.", ret);
    }
    connected = 0; pthread_mutex_unlock(&state_lock);
    /* LiStartConnection cleans up its own failures. */
    if (ret == 0) LiStopConnection();
finish:
    free_apps(list); gs_destroy(&server); g_free(job->host); free(job);
    pthread_mutex_lock(&state_lock); done = 1; pthread_mutex_unlock(&state_lock);
    return NULL;
}
static void save_settings(void) {
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_string(kf, "stream", "host", gtk_entry_get_text(GTK_ENTRY(host_entry)));
    g_key_file_set_integer(kf, "stream", "fps", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(fps_spin)));
    g_key_file_set_integer(kf, "stream", "bitrate", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(bitrate_spin)));
    g_key_file_set_integer(kf, "stream", "resolution", gtk_combo_box_get_active(GTK_COMBO_BOX(res_combo)));
    gsize length; gchar *data = g_key_file_to_data(kf, &length, NULL);
    g_file_set_contents(settings_path, data, length, NULL); g_free(data); g_key_file_free(kf);
}
static void load_settings(void) {
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, settings_path, G_KEY_FILE_NONE, NULL)) {
        char *host = g_key_file_get_string(kf, "stream", "host", NULL);
        if (host) gtk_entry_set_text(GTK_ENTRY(host_entry), host);
        g_free(host);
        int fps = g_key_file_get_integer(kf, "stream", "fps", NULL);
        int rate = g_key_file_get_integer(kf, "stream", "bitrate", NULL);
        int res = g_key_file_get_integer(kf, "stream", "resolution", NULL);
        if (fps >= 5 && fps <= 30) gtk_spin_button_set_value(GTK_SPIN_BUTTON(fps_spin), fps);
        if (rate >= 250 && rate <= 10000) gtk_spin_button_set_value(GTK_SPIN_BUTTON(bitrate_spin), rate);
        if (res >= 0 && res <= 2) gtk_combo_box_set_active(GTK_COMBO_BOX(res_combo), res);
    }
    g_key_file_free(kf);
}
static void start_job(GtkWidget *button, gpointer data) {
    int type = GPOINTER_TO_INT(data);
    if (busy) return;
    const char *host = gtk_entry_get_text(GTK_ENTRY(host_entry));
    if (!host[0] || strpbrk(host, " /?#@\t\r\n")) { set_message("Enter the host address without http:// or a port."); return; }
    int index = gtk_combo_box_get_active(GTK_COMBO_BOX(apps_combo));
    if (type == JOB_STREAM && (index < 0 || index >= app_count)) { set_message("Load the apps and select one."); return; }
    Job *job = calloc(1, sizeof(*job));
    if (!job) return;
    job->type = type; job->host = g_strdup(host);
    job->app_id = type == JOB_STREAM ? app_ids[index] : 0;
    job->fps = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(fps_spin));
    job->bitrate = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(bitrate_spin));
    int res = gtk_combo_box_get_active(GTK_COMBO_BOX(res_combo));
    const int widths[] = {640, 800, 960}, heights[] = {360, 450, 540};
    job->width = widths[res]; job->height = heights[res];
    if (type == JOB_PAIR) {
        unsigned int n;
        if (RAND_bytes((unsigned char *)&n, sizeof(n)) != 1) { g_free(job->host); free(job); return; }
        snprintf(job->pin, sizeof(job->pin), "%04u", n % 10000);
        gtk_entry_set_text(GTK_ENTRY(pin_entry), job->pin);
        set_message("Enter PIN %s on the Sunshine PIN page on your PC.", job->pin);
    } else set_message("Connecting to %s…", host);
    save_settings(); video_discard_frame();
    if (picture) { g_object_unref(picture); picture = NULL; }
    job_type = type;
    pthread_mutex_lock(&state_lock); busy = 1; done = 0; connected = 0; stop_requested = 0; pthread_mutex_unlock(&state_lock);
    gtk_widget_set_sensitive(controls, FALSE); gtk_widget_set_sensitive(actions, FALSE);
    gtk_widget_set_sensitive(stop_button, TRUE);
    gtk_widget_set_sensitive(text_entry, FALSE);
    int err = pthread_create(&worker, NULL, run_job, job);
    if (err) {
        pthread_mutex_lock(&state_lock); busy = 0; pthread_mutex_unlock(&state_lock);
        gtk_widget_set_sensitive(controls, TRUE); gtk_widget_set_sensitive(actions, TRUE);
        gtk_widget_set_sensitive(stop_button, FALSE);
        g_free(job->host); free(job); set_message("Could not create the connection thread (%d)", err);
    }
}
static gboolean close_window(GtkWidget *widget, GdkEvent *event, gpointer data) {
    closing = 1; request_stop();
    return TRUE; /* Keep GTK alive while the worker shuts down. */
}
static void stop_clicked(GtkWidget *widget, gpointer data) { request_stop(); }
static void quit_clicked(GtkWidget *widget, gpointer data) { close_window(widget, NULL, NULL); }
static void pixbuf_free(guchar *pixels, gpointer data) { free(pixels); }
static Viewport current_view(void) {
    GtkAllocation a; gtk_widget_get_allocation(canvas, &a);
    return viewport_fit(a.width, a.height, frame_w, frame_h);
}
static gboolean draw(GtkWidget *widget, GdkEventExpose *event, gpointer data) {
    GtkAllocation a; gtk_widget_get_allocation(widget, &a);
    gdk_draw_rectangle(gtk_widget_get_window(widget), picture ? widget->style->black_gc : widget->style->white_gc, TRUE, 0, 0, a.width, a.height);
    if (picture) {
        Viewport v = current_view();
        GdkPixbuf *scaled = gdk_pixbuf_scale_simple(picture, v.width, v.height, GDK_INTERP_NEAREST);
        if (scaled) {
            gdk_draw_pixbuf(gtk_widget_get_window(widget), widget->style->fg_gc[GTK_STATE_NORMAL], scaled,
                            0, 0, v.x, v.y, -1, -1, GDK_RGB_DITHER_NONE, 0, 0);
            g_object_unref(scaled);
        }
    }
    refresh_pending = 1; return TRUE;
}
static int mouse_position(double x, double y) {
    Viewport v = current_view(); int px, py;
    if (!picture || !viewport_point(v, x, y, &px, &py)) return 0;
    LiSendMousePositionEvent(px, py, v.width, v.height); return 1;
}
static unsigned int mouse_buttons;
static gboolean mouse_motion(GtkWidget *widget, GdkEventMotion *event, gpointer data) {
    pthread_mutex_lock(&state_lock);
    if (connected) mouse_position(event->x, event->y);
    pthread_mutex_unlock(&state_lock); return TRUE;
}
static gboolean mouse_button(GtkWidget *widget, GdkEventButton *event, gpointer data) {
    if (event->type != GDK_BUTTON_PRESS && event->type != GDK_BUTTON_RELEASE) return TRUE;
    int b = event->button == 1 ? BUTTON_LEFT : event->button == 2 ? BUTTON_MIDDLE : event->button == 3 ? BUTTON_RIGHT : 0;
    if (!b) return FALSE;
    if (event->button == 1 && event->type == GDK_BUTTON_RELEASE && menu_release_pending) {
        menu_release_pending = 0; return TRUE;
    }
    GtkAllocation allocation; gtk_widget_get_allocation(canvas, &allocation);
    pthread_mutex_lock(&state_lock); int live = connected; pthread_mutex_unlock(&state_lock);
    if (live && !stream_controls && event->button == 1 && event->type == GDK_BUTTON_PRESS &&
        event->x >= allocation.width - 64 && event->y < 64) {
        stream_controls = 1; menu_release_pending = 1; refresh_pending = 1; return TRUE;
    }
    gtk_widget_grab_focus(canvas);
    pthread_mutex_lock(&state_lock);
    if (connected) {
        int inside = mouse_position(event->x, event->y);
        if (event->type == GDK_BUTTON_PRESS && inside) {
            mouse_buttons |= 1U << b; LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, b);
        } else if (event->type == GDK_BUTTON_RELEASE && (mouse_buttons & (1U << b))) {
            mouse_buttons &= ~(1U << b); LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, b);
        }
    }
    pthread_mutex_unlock(&state_lock); return TRUE;
}
static int virtual_key(guint key) {
    if (key >= GDK_a && key <= GDK_z) return key - GDK_a + 'A';
    if ((key >= GDK_A && key <= GDK_Z) || (key >= GDK_0 && key <= GDK_9)) return key;
    if (key >= GDK_F1 && key <= GDK_F12) return 0x70 + key - GDK_F1;
    switch (key) {
    case GDK_Return: return 0x0d; case GDK_BackSpace: return 0x08; case GDK_Tab: return 0x09;
    case GDK_Escape: return 0x1b; case GDK_space: return 0x20;
    case GDK_Left: return 0x25; case GDK_Up: return 0x26; case GDK_Right: return 0x27; case GDK_Down: return 0x28;
    case GDK_Home: return 0x24; case GDK_End: return 0x23; case GDK_Delete: return 0x2e;
    case GDK_Page_Up: return 0x21; case GDK_Page_Down: return 0x22;
    case GDK_Shift_L: return 0xa0; case GDK_Shift_R: return 0xa1;
    case GDK_Control_L: return 0xa2; case GDK_Control_R: return 0xa3;
    case GDK_Alt_L: return 0xa4; case GDK_Alt_R: return 0xa5;
    default: return 0;
    }
}
static unsigned char held_keys[256];
static void release_input(void) {
    pthread_mutex_lock(&state_lock);
    if (connected) {
        for (int i = 0; i < 256; i++) if (held_keys[i]) LiSendKeyboardEvent(0x8000 | i, KEY_ACTION_UP, 0);
        for (int b = BUTTON_LEFT; b <= BUTTON_RIGHT; b++) if (mouse_buttons & (1U << b)) LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, b);
    }
    memset(held_keys, 0, sizeof(held_keys)); mouse_buttons = 0;
    pthread_mutex_unlock(&state_lock);
}
static gboolean focus_out(GtkWidget *widget, GdkEventFocus *event, gpointer data) { release_input(); return FALSE; }
static gboolean key_event(GtkWidget *widget, GdkEventKey *event, gpointer data) {
    if (event->keyval == GDK_Escape && event->type == GDK_KEY_PRESS) { request_stop(); return TRUE; }
    int vk = virtual_key(event->keyval);
    if (!vk) return FALSE;
    int modifiers = ((event->state & GDK_SHIFT_MASK) ? MODIFIER_SHIFT : 0) |
                    ((event->state & GDK_CONTROL_MASK) ? MODIFIER_CTRL : 0) |
                    ((event->state & GDK_MOD1_MASK) ? MODIFIER_ALT : 0);
    pthread_mutex_lock(&state_lock);
    if (connected) {
        int down = event->type == GDK_KEY_PRESS;
        held_keys[vk] = down;
        LiSendKeyboardEvent(0x8000 | vk, down ? KEY_ACTION_DOWN : KEY_ACTION_UP, modifiers);
    }
    pthread_mutex_unlock(&state_lock); return TRUE;
}
static void send_text(GtkWidget *widget, gpointer data) {
    pthread_mutex_lock(&state_lock);
    if (connected) {
        const char *text = gtk_entry_get_text(GTK_ENTRY(text_entry));
        LiSendUtf8TextEvent(text, strlen(text));
    }
    pthread_mutex_unlock(&state_lock);
    gtk_entry_set_text(GTK_ENTRY(text_entry), "");
}
static void special_key(GtkWidget *widget, gpointer data) {
    int key = GPOINTER_TO_INT(data);
    pthread_mutex_lock(&state_lock);
    if (connected) {
        LiSendKeyboardEvent(0x8000 | key, KEY_ACTION_DOWN, 0);
        LiSendKeyboardEvent(0x8000 | key, KEY_ACTION_UP, 0);
    }
    pthread_mutex_unlock(&state_lock);
}
static gboolean entry_focused(GtkWidget *widget, GdkEventFocus *event, gpointer data) {
    typing_target = widget; return FALSE;
}
static void osk_key(GtkWidget *widget, gpointer data) {
    if (!typing_target || !GTK_IS_ENTRY(typing_target) || !gtk_widget_is_sensitive(typing_target)) return;
    const char *key = data;
    GtkEditable *entry = GTK_EDITABLE(typing_target);
    int pos = gtk_editable_get_position(entry), a, b;
    if (gtk_editable_get_selection_bounds(entry, &a, &b)) { gtk_editable_delete_text(entry, a, b); pos = a; }
    if (!strcmp(key, "Backspace")) { if (pos > 0) { gtk_editable_delete_text(entry, pos-1, pos); pos--; } }
    else { const char *text = !strcmp(key, "Space") ? " " : key; gtk_editable_insert_text(entry, text, strlen(text), &pos); }
    gtk_editable_set_position(entry, pos); refresh_pending = 1;
}
static void toggle_keyboard(GtkWidget *widget, gpointer data) {
    if (gtk_widget_get_visible(osk)) { gtk_widget_hide(osk); gtk_widget_hide(text_bar); }
    else {
        gtk_widget_show_all(osk);
        pthread_mutex_lock(&state_lock); int live = connected; pthread_mutex_unlock(&state_lock);
        if (live) { gtk_widget_show_all(text_bar); typing_target = text_entry; }
    }
    refresh_pending = 1;
}
static void hide_stream_controls(GtkWidget *widget, gpointer data) {
    release_input(); stream_controls = 0;
    gtk_widget_hide(osk); gtk_widget_hide(text_bar); refresh_pending = 1;
}
static void host_changed(GtkWidget *widget, gpointer data) {
    while (app_count) { gtk_combo_box_remove_text(GTK_COMBO_BOX(apps_combo), --app_count); }
}
static gboolean tick(gpointer data) {
    if (signal_quit) { closing = 1; request_stop(); }
    char msg[sizeof(message)]; int finished, live;
    pthread_mutex_lock(&state_lock);
    memcpy(msg, message, sizeof(msg)); finished = done; live = connected;
    pthread_mutex_unlock(&state_lock);
    if (strcmp(gtk_label_get_text(GTK_LABEL(status_label)), msg)) {
        gtk_label_set_text(GTK_LABEL(status_label), msg); refresh_pending = 1;
    }
    if (finished) {
        pthread_join(worker, NULL);
        pthread_mutex_lock(&state_lock); busy = 0; done = 0; pthread_mutex_unlock(&state_lock);
        release_input();
        gtk_widget_set_sensitive(controls, TRUE); gtk_widget_set_sensitive(actions, TRUE);
        gtk_widget_set_sensitive(stop_button, FALSE);
        if (job_type == JOB_LIST) {
            while (app_count) gtk_combo_box_remove_text(GTK_COMBO_BOX(apps_combo), --app_count);
            for (int i = 0; i < result_count; i++) {
                gtk_combo_box_append_text(GTK_COMBO_BOX(apps_combo), result_names[i]);
                app_ids[app_count++] = result_ids[i]; g_free(result_names[i]); result_names[i] = NULL;
            }
            result_count = 0;
            if (app_count) gtk_combo_box_set_active(GTK_COMBO_BOX(apps_combo), 0);
        }
        refresh_pending = 1;
    }
    static int previously_live;
    if (live != previously_live) {
        stream_controls = !live;
        gtk_widget_hide(osk); gtk_widget_hide(text_bar);
        previously_live = live;
        refresh_pending = 1;
    }
    gtk_widget_set_sensitive(text_entry, live);
    if (live) { gtk_widget_hide(controls); gtk_widget_hide(actions); gtk_widget_show(stream_menu_button); }
    else { gtk_widget_show(controls); gtk_widget_show(actions); gtk_widget_hide(stream_menu_button); }
    if (live && !stream_controls) {
        gtk_widget_hide(header); gtk_widget_hide(status_label); gtk_widget_hide(bottom_bar);
        gtk_widget_hide(text_bar); gtk_widget_hide(osk);
        gtk_container_set_border_width(GTK_CONTAINER(window), 0);
        gtk_box_set_spacing(GTK_BOX(root_box), 0);
    } else {
        gtk_widget_show(header); gtk_widget_show(status_label); gtk_widget_show(bottom_bar);
        gtk_container_set_border_width(GTK_CONTAINER(window), 4);
        gtk_box_set_spacing(GTK_BOX(root_box), 2);
    }
    if (closing && !busy) { save_settings(); gtk_main_quit(); return FALSE; }
    int w, h; uint8_t *pixels = video_take_frame(&w, &h);
    if (pixels) {
        if (picture) g_object_unref(picture);
        picture = gdk_pixbuf_new_from_data(pixels, GDK_COLORSPACE_RGB, FALSE, 8, w, h, w * 3, pixbuf_free, NULL);
        frame_w = w; frame_h = h;
        gtk_widget_queue_draw(canvas);
    }
    /* Flush X rendering before asking the EPDC to display its framebuffer. */
    if (refresh_pending) {
        gdk_display_sync(gdk_display_get_default());
        if (eink_refresh(0) < 0) fprintf(stderr, "FBInk refresh failed\n");
        refresh_pending = 0;
    }
    return TRUE;
}
static void signal_handler(int sig) { signal_quit = 1; }
static gboolean smoke_done(gpointer data) { closing = 1; return FALSE; }
static GtkWidget *button(GtkWidget *box, const char *label, GCallback fn, gpointer arg) {
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_box_pack_start(GTK_BOX(box), b, TRUE, TRUE, 2);
    if (is_kindle) gtk_widget_set_size_request(b, -1, 46);
    g_signal_connect(b, "clicked", fn, arg); return b;
}
static void row(GtkWidget *box, const char *label, GtkWidget *entry) {
    if (is_kindle) gtk_widget_set_size_request(entry, -1, 40);
    GtkWidget *h = gtk_hbox_new(FALSE, 8);
    gtk_box_pack_start(GTK_BOX(h), gtk_label_new(label), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(h), entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), h, TRUE, TRUE, 2);
}
/* The Kindle window manager uses its own orientation/chrome title fields in
 * addition to EWMH fullscreen. Request rotation after our window is managed. */
static gboolean enter_fullscreen(gpointer data) {
    if (!preview) {
        gchar *args[] = {"lipc-set-prop", "com.lab126.winmgr", "orientationLock", "R", NULL};
        GError *error = NULL;
        if (!g_spawn_async(NULL, args, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &error)) {
            fprintf(stderr, "Could not request landscape orientation: %s\n", error->message);
            g_error_free(error);
        }
    }
    gtk_window_fullscreen(GTK_WINDOW(window));
    gtk_window_resize(GTK_WINDOW(window), gdk_screen_width(), gdk_screen_height());
    refresh_pending = 1;
    return FALSE;
}
static gboolean window_mapped(GtkWidget *widget, GdkEvent *event, gpointer data) {
    static int requested;
    if (is_kindle && !requested) { requested = 1; g_timeout_add(250, enter_fullscreen, NULL); }
    return FALSE;
}
static void screen_resized(GdkScreen *screen, gpointer data) {
    if (!is_kindle) return;
    gtk_window_fullscreen(GTK_WINDOW(window));
    gtk_window_resize(GTK_WINDOW(window), gdk_screen_get_width(screen), gdk_screen_get_height(screen));
    if (!preview && eink_reinit() < 0) fprintf(stderr, "FBInk: could not update orientation\n");
    gtk_widget_queue_draw(canvas); refresh_pending = 1;
}
int main(int argc, char **argv) {
    umask(0077);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--kindle")) is_kindle = 1;
        else if (!strcmp(argv[i], "--preview-kindle")) { is_kindle = 1; preview = 1; }
        else if (!strcmp(argv[i], "--smoke-test")) smoke = 1;
        else if (!strcmp(argv[i], "--help")) {
            puts("moonlight-kindle [--kindle] [--preview-kindle] [--smoke-test]\nGTK2 + H.264 Moonlight client. --kindle requires libfbink.so.1.\nSet MOONLIGHT_DATA_DIR for private pairing keys and settings."); return 0;
        } else { fprintf(stderr, "Unknown option: %s\n", argv[i]); return 2; }
    }
    if (!gtk_init_check(NULL, NULL)) { fprintf(stderr, "Cannot open DISPLAY. This app requires GTK2/X11.\n"); return 1; }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return 1;
    const char *dir = getenv("MOONLIGHT_DATA_DIR");
    char *base = dir ? g_strdup(dir) : g_build_filename(g_get_user_config_dir(), "moonlight-kindle", NULL);
    key_dir = g_build_filename(base, "keys", NULL); settings_path = g_build_filename(base, "settings.ini", NULL);
    if (g_mkdir_with_parents(key_dir, 0700) < 0) { perror("Data directory"); return 1; }
    g_free(base);
    if (eink_init(is_kindle && !preview) < 0) { fprintf(stderr, "Could not initialize FBInk on the Kindle.\n"); return 1; }
    signal(SIGINT, signal_handler); signal(SIGTERM, signal_handler);
    /* Keep compact pixel sizing independent of the device's Xft DPI. */
    if (is_kindle) g_object_set(gtk_settings_get_default(), "gtk-xft-dpi", 96 * 1024, NULL);
    gtk_rc_parse_string(is_kindle ?
        "style \"moonlight\" { font_name=\"DejaVu Sans 14\" bg[NORMAL]=\"#ffffff\" fg[NORMAL]=\"#000000\" base[NORMAL]=\"#ffffff\" text[NORMAL]=\"#000000\" bg[PRELIGHT]=\"#eeeeee\" fg[PRELIGHT]=\"#000000\" } class \"*\" style \"moonlight\"" :
        "style \"moonlight\" { font_name=\"Sans 11\" bg[NORMAL]=\"#ffffff\" fg[NORMAL]=\"#000000\" base[NORMAL]=\"#ffffff\" text[NORMAL]=\"#000000\" } class \"*\" style \"moonlight\"");
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), is_kindle ? "L:A_N:application_PC:N_O:R_ID:org.moonlight.kindle" : "Moonlight for Kindle · GTK2");
    gtk_window_set_default_size(GTK_WINDOW(window), is_kindle ? 1448 : 1000, is_kindle ? 1072 : 700);
    gtk_container_set_border_width(GTK_CONTAINER(window), 4);
    if (is_kindle) { gtk_window_set_decorated(GTK_WINDOW(window), FALSE); gtk_window_fullscreen(GTK_WINDOW(window)); }
    GtkWidget *box = gtk_vbox_new(FALSE, 2); root_box = box; gtk_container_add(GTK_CONTAINER(window), box);
    header = gtk_label_new("Moonlight for Kindle");
    gtk_box_pack_start(GTK_BOX(box), header, FALSE, FALSE, 0);
    controls = gtk_vbox_new(FALSE, 1); gtk_box_pack_start(GTK_BOX(box), controls, FALSE, FALSE, 0);
    GtkWidget *host_row = gtk_hbox_new(FALSE, 8);
    gtk_box_pack_start(GTK_BOX(controls), host_row, FALSE, FALSE, 0);
    host_entry = gtk_entry_new(); gtk_entry_set_max_length(GTK_ENTRY(host_entry), 253); row(host_row, "PC address", host_entry);
    pin_entry = gtk_entry_new(); gtk_entry_set_width_chars(GTK_ENTRY(pin_entry), 5);
    gtk_editable_set_editable(GTK_EDITABLE(pin_entry), FALSE);
    gtk_box_pack_start(GTK_BOX(host_row), gtk_label_new("PIN"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(host_row), pin_entry, FALSE, FALSE, 0);
    GtkWidget *stream_row = gtk_hbox_new(FALSE, 8);
    gtk_box_pack_start(GTK_BOX(controls), stream_row, FALSE, FALSE, 0);
    apps_combo = gtk_combo_box_new_text(); row(stream_row, "App", apps_combo);
    res_combo = gtk_combo_box_new_text();
    gtk_combo_box_append_text(GTK_COMBO_BOX(res_combo), "640 × 360"); gtk_combo_box_append_text(GTK_COMBO_BOX(res_combo), "800 × 450");
    gtk_combo_box_append_text(GTK_COMBO_BOX(res_combo), "960 × 540"); gtk_combo_box_set_active(GTK_COMBO_BOX(res_combo), 0); row(stream_row, "Resolution", res_combo);
    GtkWidget *rates = gtk_hbox_new(FALSE, 6);
    gtk_box_pack_start(GTK_BOX(stream_row), rates, FALSE, FALSE, 0);
    fps_spin = gtk_spin_button_new_with_range(5, 30, 1); gtk_spin_button_set_value(GTK_SPIN_BUTTON(fps_spin), 10); gtk_entry_set_width_chars(GTK_ENTRY(fps_spin), 2);
    bitrate_spin = gtk_spin_button_new_with_range(250, 10000, 250); gtk_spin_button_set_value(GTK_SPIN_BUTTON(bitrate_spin), 1000); gtk_entry_set_width_chars(GTK_ENTRY(bitrate_spin), 5);
    row(rates, "FPS", fps_spin); row(rates, "kbps", bitrate_spin);
    actions = gtk_hbox_new(FALSE, 4); gtk_box_pack_start(GTK_BOX(box), actions, FALSE, FALSE, 0);
    button(actions, "Pair", G_CALLBACK(start_job), GINT_TO_POINTER(JOB_PAIR));
    button(actions, "Load apps", G_CALLBACK(start_job), GINT_TO_POINTER(JOB_LIST));
    button(actions, "Start", G_CALLBACK(start_job), GINT_TO_POINTER(JOB_STREAM));
    status_label = gtk_label_new(message); gtk_label_set_line_wrap(GTK_LABEL(status_label), TRUE);
    gtk_box_pack_start(GTK_BOX(box), status_label, FALSE, FALSE, 3);
    canvas = gtk_drawing_area_new(); gtk_widget_set_size_request(canvas, 320, 180); gtk_widget_set_can_focus(canvas, TRUE);
    gtk_widget_add_events(canvas, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK | GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK | GDK_FOCUS_CHANGE_MASK);
    gtk_box_pack_start(GTK_BOX(box), canvas, TRUE, TRUE, 0);
    GtkWidget *keyboard = gtk_hbox_new(FALSE, 3); text_bar = keyboard; gtk_box_pack_start(GTK_BOX(box), keyboard, FALSE, FALSE, 0);
    text_entry = gtk_entry_new(); gtk_entry_set_max_length(GTK_ENTRY(text_entry), 1000);
    gtk_box_pack_start(GTK_BOX(keyboard), text_entry, TRUE, TRUE, 0);
    button(keyboard, "Send text", G_CALLBACK(send_text), NULL);
    button(keyboard, "Enter", G_CALLBACK(special_key), GINT_TO_POINTER(0x0d));
    button(keyboard, "← Backspace", G_CALLBACK(special_key), GINT_TO_POINTER(0x08));
    g_signal_connect(text_entry, "activate", G_CALLBACK(send_text), NULL);
    osk = gtk_vbox_new(FALSE, 2);
    gtk_box_pack_start(GTK_BOX(box), osk, FALSE, FALSE, 0);
    const char *rows[] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm.-:"};
    for (int r = 0; r < 4; r++) {
        GtkWidget *keys = gtk_hbox_new(TRUE, 1); gtk_box_pack_start(GTK_BOX(osk), keys, FALSE, FALSE, 0);
        for (const char *c = rows[r]; *c; c++) {
            char key[2] = {*c, 0}; char *value = g_strdup(key);
            GtkWidget *b = button(keys, key, G_CALLBACK(osk_key), value);
            g_object_set_data_full(G_OBJECT(b), "key-value", value, g_free);
            gtk_button_set_focus_on_click(GTK_BUTTON(b), FALSE);
            gtk_widget_set_size_request(b, -1, is_kindle ? 46 : 32);
        }
    }
    GtkWidget *extras = gtk_hbox_new(TRUE, 2); gtk_box_pack_start(GTK_BOX(osk), extras, FALSE, FALSE, 0);
    button(extras, "Space", G_CALLBACK(osk_key), "Space");
    button(extras, "Backspace", G_CALLBACK(osk_key), "Backspace");
    g_signal_connect(host_entry, "focus-in-event", G_CALLBACK(entry_focused), NULL);
    g_signal_connect(text_entry, "focus-in-event", G_CALLBACK(entry_focused), NULL);
    typing_target = host_entry;
    GtkWidget *bottom = gtk_hbox_new(FALSE, 3); bottom_bar = bottom; gtk_box_pack_start(GTK_BOX(box), bottom, FALSE, FALSE, 0);
    button(bottom, "Keyboard", G_CALLBACK(toggle_keyboard), NULL);
    stream_menu_button = button(bottom, "Back to video", G_CALLBACK(hide_stream_controls), NULL);
    stop_button = button(bottom, "Disconnect", G_CALLBACK(stop_clicked), NULL);
    button(bottom, "Exit", G_CALLBACK(quit_clicked), NULL);
    g_signal_connect(window, "map-event", G_CALLBACK(window_mapped), NULL);
    g_signal_connect(gdk_screen_get_default(), "size-changed", G_CALLBACK(screen_resized), NULL);
    g_signal_connect(window, "delete-event", G_CALLBACK(close_window), NULL);
    g_signal_connect(canvas, "expose-event", G_CALLBACK(draw), NULL);
    g_signal_connect(canvas, "motion-notify-event", G_CALLBACK(mouse_motion), NULL);
    g_signal_connect(canvas, "button-press-event", G_CALLBACK(mouse_button), NULL);
    g_signal_connect(canvas, "button-release-event", G_CALLBACK(mouse_button), NULL);
    g_signal_connect(canvas, "key-press-event", G_CALLBACK(key_event), NULL);
    g_signal_connect(canvas, "key-release-event", G_CALLBACK(key_event), NULL);
    g_signal_connect(canvas, "focus-out-event", G_CALLBACK(focus_out), NULL);
    g_signal_connect(host_entry, "changed", G_CALLBACK(host_changed), NULL);
    load_settings(); gtk_widget_show_all(window); gtk_widget_hide(osk); gtk_widget_hide(text_bar); gtk_widget_hide(stream_menu_button);
    gtk_widget_set_sensitive(stop_button, FALSE); gtk_widget_set_sensitive(text_entry, FALSE);
    g_timeout_add(100, tick, NULL);
    if (smoke) g_timeout_add(1500, smoke_done, NULL);
    gtk_main();
    if (picture) g_object_unref(picture);
    video_discard_frame(); eink_refresh(1); eink_close(); curl_global_cleanup();
    g_free(key_dir); g_free(settings_path); return 0;
}
