/* SPDX-License-Identifier: GPL-3.0-or-later
 * Run with a 1448x1072 X display. No host or remote input is sent. */
#define eink_refresh test_eink_refresh
#define eink_clear test_eink_clear
#define main moonlight_application_main
#include "../src/main.c"
#undef main
#undef eink_refresh
#undef eink_clear
#include <assert.h>
static int stage, full_refreshes, clears, fail_clear, verify_painted;
int test_eink_clear(void) {
    clears++;
    if (fail_clear) return -1;
    /* Simulate eips overwriting framebuffer pixels without a GTK expose. */
    GtkAllocation a; gtk_widget_get_allocation(canvas, &a);
    gdk_draw_rectangle(gtk_widget_get_window(canvas), canvas->style->white_gc, TRUE, 0, 0, a.width, a.height);
    gdk_display_sync(gdk_display_get_default());
    return 0;
}
int test_eink_refresh(int full) {
    if (verify_painted) {
        /* The refresh must follow GTK's pending paint, even without video. */
        GdkRegion *pending = gdk_window_get_update_area(gtk_widget_get_window(canvas));
        assert(!pending); verify_painted = 0;
    }
    full_refreshes += !!full; return 0;
}
static GtkWidget *find_button(GtkWidget *container, const char *label) {
    GList *children = gtk_container_get_children(GTK_CONTAINER(container));
    GtkWidget *found = NULL;
    for (GList *p = children; p; p = p->next)
        if (GTK_IS_BUTTON(p->data) && !strcmp(gtk_button_get_label(GTK_BUTTON(p->data)), label)) found = p->data;
    g_list_free(children); return found;
}
static gboolean verify_ui(gpointer data) {
    GtkAllocation screen, area;
    gtk_widget_get_allocation(window, &screen); gtk_widget_get_allocation(canvas, &area);
    switch (stage++) {
    case 0:
        assert(screen.width == 1448 && screen.height == 1072);
        assert(gtk_widget_get_visible(controls));
        assert(!strcmp(gtk_label_get_text(GTK_LABEL(header)), "Moonlight"));
        pthread_mutex_lock(&state_lock); connected = 1; pthread_mutex_unlock(&state_lock);
        break;
    case 1: {
        assert(!gtk_widget_get_visible(bottom_bar) && !gtk_widget_get_visible(controls));
        assert(area.width == screen.width && area.height == screen.height);
        GdkEventButton event = {0}; event.type = GDK_BUTTON_PRESS; event.button = 1;
        event.x = area.width - 2; event.y = 2;
        assert(mouse_button(canvas, &event, NULL)); assert(stream_controls && menu_release_pending);
        event.type = GDK_BUTTON_RELEASE;
        assert(mouse_button(canvas, &event, NULL)); assert(!menu_release_pending && mouse_buttons == 0);
        break;
    }
    case 2: {
        assert(gtk_widget_get_visible(bottom_bar) && gtk_widget_get_visible(stream_menu_button));
        assert(area.height < screen.height);
        GtkWidget *refresh = find_button(bottom_bar, "Refresh screen");
        assert(refresh && gtk_widget_get_sensitive(refresh));
        if (picture) g_object_unref(picture);
        picture = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 16, 16);
        assert(picture); gdk_pixbuf_fill(picture, 0x000000ff); frame_w = frame_h = 16;
        int before = full_refreshes, before_clear = clears; verify_painted = 1;
        gtk_button_clicked(GTK_BUTTON(refresh)); assert(full_refresh_pending);
        tick(NULL);
        assert(clears == before_clear + 1 && full_refreshes == before && !full_refresh_pending && !verify_painted);
        GtkAllocation visible; gtk_widget_get_allocation(canvas, &visible);
        GdkPixbuf *sample = gdk_pixbuf_get_from_drawable(NULL, gtk_widget_get_window(canvas),
            gdk_colormap_get_system(), visible.width / 2, visible.height / 2, 0, 0, 1, 1);
        assert(sample && gdk_pixbuf_get_pixels(sample)[0] < 32);
        g_object_unref(sample);
        fail_clear = 1;
        gtk_button_clicked(GTK_BUTTON(refresh)); tick(NULL);
        assert(clears == before_clear + 2 && full_refreshes == before + 1 && !full_refresh_pending);
        fail_clear = 0;
        hide_stream_controls(NULL, NULL); break;
    }
    case 3:
        assert(!gtk_widget_get_visible(bottom_bar) && area.height == screen.height);
        pthread_mutex_lock(&state_lock); connected = 0; pthread_mutex_unlock(&state_lock);
        break;
    case 4: {
        assert(gtk_widget_get_visible(controls) && gtk_widget_get_visible(bottom_bar));
        assert(!gtk_widget_get_visible(stream_menu_button));
        assert(gtk_adjustment_get_lower(gtk_spin_button_get_adjustment(GTK_SPIN_BUTTON(fps_spin))) == 1);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(fps_spin), 1);
        gtk_combo_box_set_active(GTK_COMBO_BOX(res_combo), 3);
        assert(gtk_widget_get_visible(custom_row));
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(width_spin), 1448);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(height_spin), 1072);
        int w, h;
        assert(selected_resolution(&w, &h) && w == 1448 && h == 1072);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(width_spin), 801);
        assert(!selected_resolution(&w, &h));
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(width_spin), 1448);
        save_settings();
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(fps_spin), 10);
        gtk_combo_box_set_active(GTK_COMBO_BOX(res_combo), 0);
        assert(!gtk_widget_get_visible(custom_row));
        assert(selected_resolution(&w, &h) && w == 640 && h == 360);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(width_spin), 800);
        load_settings();
        assert(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(fps_spin)) == 1);
        assert(gtk_combo_box_get_active(GTK_COMBO_BOX(res_combo)) == 3);
        assert(gtk_widget_get_visible(custom_row));
        assert(selected_resolution(&w, &h) && w == 1448 && h == 1072);
        closing = 1;
        puts("Moonlight label, eips clear/repaint, refresh fallback, 1 FPS and custom resolution persistence: OK");
        return FALSE;
    }
    }
    return TRUE;
}
int main(void) {
    g_timeout_add(700, verify_ui, NULL);
    char *args[] = {"ui-test", "--preview-kindle", NULL};
    return moonlight_application_main(2, args);
}
