#include <glib/gprintf.h>
#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "uca.h"


typedef struct {
    gboolean running;
    gboolean store;
    
    guchar *buffer, *pixels;
    GdkPixbuf *pixbuf;
    GtkWidget *image;

    GtkStatusbar *statusbar;
    guint statusbar_context_id;

    int timestamp;
    int width;
    int height;
    int pixel_size;
    struct uca_camera *cam;
    struct uca *u;
    float scale;
} ThreadData;


typedef struct {
    ThreadData *thread_data;
    GtkTreeStore *tree_store;
} ValueCellData;


enum {
    COLUMN_NAME = 0,
    COLUMN_VALUE,
    COLUMN_UNIT,
    COLUMN_UCA_ID,
    NUM_COLUMNS
};


void convert_8bit_to_rgb(guchar *output, guchar *input, int width, int height)
{
    for (int i = 0, j = 0; i < width*height; i++) {
        output[j++] = input[i];
        output[j++] = input[i]; 
        output[j++] = input[i];
    }
}

void convert_16bit_to_rgb(guchar *output, guchar *input, int width, int height, float scale)
{
    uint16_t *in = (uint16_t *) input;
    for (int i = 0, j = 0; i < width*height; i++) {
        guchar val = (uint8_t) ((in[i]/scale)*256.0f);
        output[j++] = val;
        output[j++] = val;
        output[j++] = val;
    }
}

void reallocate_buffers(ThreadData *td, int width, int height)
{
    const size_t num_bytes = width * height * td->pixel_size;

    g_object_unref(td->pixbuf);
    g_free(td->buffer);

    td->pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, width, height);
    td->buffer = (guchar *) g_malloc(num_bytes);
    td->width  = width;
    td->height = height;
    td->pixels = gdk_pixbuf_get_pixels(td->pixbuf);
    gtk_image_set_from_pixbuf(GTK_IMAGE(td->image), td->pixbuf);
    memset(td->buffer, 0, num_bytes);

    if (uca_cam_alloc(td->cam, 20) != UCA_NO_ERROR)
        g_print("Couldn't allocate buffer for 20 frames\n");
}

void *grab_thread(void *args)
{
    ThreadData *data = (ThreadData *) args;
    struct uca_camera *cam = data->cam;
    char filename[FILENAME_MAX] = {0,};
    int counter = 0;

    while (data->running) {
        uca_cam_grab(cam, (char *) data->buffer, NULL);
        if (data->store) {
            snprintf(filename, FILENAME_MAX, "frame-%i-%08i.raw", data->timestamp, counter++);
            FILE *fp = fopen(filename, "wb");
            fwrite(data->buffer, data->width*data->height, data->pixel_size, fp);
            fclose(fp);
        }

        if (data->pixel_size == 1)
            convert_8bit_to_rgb(data->pixels, data->buffer, data->width, data->height);
        else if (data->pixel_size == 2)
            convert_16bit_to_rgb(data->pixels, data->buffer, data->width, data->height, data->scale);

        gdk_threads_enter();
        gdk_flush();
        gtk_image_clear(GTK_IMAGE(data->image));
        gtk_image_set_from_pixbuf(GTK_IMAGE(data->image), data->pixbuf);
        gtk_widget_queue_draw_area(data->image, 0, 0, data->width, data->height);
        gdk_threads_leave();
    }
    return NULL;
}

gboolean on_delete_event(GtkWidget *widget, GdkEvent *event, gpointer data)
{
    return FALSE;
}

void on_destroy(GtkWidget *widget, gpointer data)
{
    ThreadData *td = (ThreadData *) data;
    td->running = FALSE;
    uca_destroy(td->u);
    gtk_main_quit();
}

void on_adjustment_scale_value_changed(GtkAdjustment* adjustment, gpointer user_data)
{
    ThreadData *data = (ThreadData *) user_data;
    data->scale = gtk_adjustment_get_value(adjustment);
}

void on_toolbutton_run_clicked(GtkWidget *widget, gpointer args)
{
    ThreadData *data = (ThreadData *) args;
    if (data->running)
        return;
    
    GError *error = NULL;
    data->running = TRUE;
    uca_cam_start_recording(data->cam);
    if (!g_thread_create(grab_thread, data, FALSE, &error)) {
        g_printerr("Failed to create thread: %s\n", error->message);
        uca_destroy(data->u);
    }
}

void on_toolbutton_stop_clicked(GtkWidget *widget, gpointer args)
{
    ThreadData *data = (ThreadData *) args;
    data->running = FALSE;
    data->store = FALSE;
    uca_cam_stop_recording(data->cam);
}

void on_toolbutton_record_clicked(GtkWidget *widget, gpointer args)
{
    ThreadData *data = (ThreadData *) args;
    data->timestamp = (int) time(0);
    data->store = TRUE;
    GError *error = NULL;
    
    gtk_statusbar_push(data->statusbar, data->statusbar_context_id, "Recording...");
    
    if (data->running != TRUE) {
        data->running = TRUE;
        uca_cam_start_recording(data->cam);
        if (!g_thread_create(grab_thread, data, FALSE, &error)) {
            g_printerr("Failed to create thread: %s\n", error->message);
            uca_destroy(data->u);
        }
    }
}

void on_valuecell_edited(GtkCellRendererText *renderer, gchar *path, gchar *new_text, gpointer data)
{
    ValueCellData *value_data = (ValueCellData *) data;

    if (value_data->thread_data->running)
        return;

    GtkTreeModel *tree_model = GTK_TREE_MODEL(value_data->tree_store);
    GtkTreePath *tree_path = gtk_tree_path_new_from_string(path);
    GtkTreeIter iter;

    if (gtk_tree_model_get_iter(tree_model, &iter, tree_path)) {
        struct uca_camera *cam = value_data->thread_data->cam;
        uint32_t prop_id;
        gtk_tree_model_get(tree_model, &iter, COLUMN_UCA_ID, &prop_id, -1);

        /* TODO: extensive value checking */
        uint32_t val = (uint32_t) g_ascii_strtoull(new_text, NULL, 10);
        uca_cam_set_property(cam, prop_id, &val);
        if ((prop_id == UCA_PROP_WIDTH) || (prop_id == UCA_PROP_HEIGHT)) {
            uint32_t width, height;
            uca_cam_get_property(cam, UCA_PROP_WIDTH, &width, 0);
            uca_cam_get_property(cam, UCA_PROP_HEIGHT, &height, 0);
            reallocate_buffers(value_data->thread_data, width, height);
        }

        gtk_tree_store_set(value_data->tree_store, &iter, COLUMN_VALUE, new_text, -1);
    }
}

static void get_first_level_root(GtkTreeStore *store, GtkTreeIter *iter, gchar *group)
{
    GtkTreeIter root;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &root)) {
        gchar *str;
        gtk_tree_model_get(GTK_TREE_MODEL(store), &root, 0, &str, -1);
        if (g_strcmp0(group, str) == 0) {
            *iter = root;
            return;
        }

        /* Iterate through all groups */
        while (gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &root)) {
            gtk_tree_model_get(GTK_TREE_MODEL(store), &root, 0, &str, -1);
            if (g_strcmp0(group, str) == 0) {
                *iter = root;
                g_free(str);
                return;
            }
        }

        /* Not found, append the group */
        g_free(str);
    }

    /* Tree is empty or group is not found */
    gtk_tree_store_append(store, iter, NULL);
    gtk_tree_store_set(store, iter, 0, group, -1);
}

static void find_recursively(GtkTreeStore *store, GtkTreeIter *root, GtkTreeIter *result, gchar **tokens, int depth)
{
    GtkTreeIter iter;
    gchar *str;
    gchar *current_token = tokens[depth];

    if (current_token == NULL) {
        *result = *root;
        return;
    }

    if (!gtk_tree_model_iter_has_child(GTK_TREE_MODEL(store), root)) {
        gtk_tree_store_append(store, &iter, root);
        if (tokens[depth+1] == NULL) {
            *result = iter;
            return;
        }
        else {
            gtk_tree_store_set(store, &iter, 0, current_token, -1);
            find_recursively(store, &iter, result, tokens, depth+1);
        }
    }

    gtk_tree_model_iter_children(GTK_TREE_MODEL(store), &iter, root);
    do {
        gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, 0, &str, -1);
        if (g_strcmp0(current_token, str) == 0) {
            find_recursively(store, &iter, result, tokens, depth+1);
            g_free(str);
            return;
        }
    } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &iter));

    g_free(str);
    gtk_tree_store_append(store, &iter, root);
    gtk_tree_store_set(store, &iter, COLUMN_NAME, current_token, -1);
    find_recursively(store, &iter, result, tokens, depth+1);
}

static void fill_tree_store(GtkTreeStore *tree_store, struct uca_camera *cam)
{
    GtkTreeIter iter, child;
    struct uca_property *property;
    const size_t num_bytes = 256;
    gchar *value_string = g_malloc(num_bytes);
    guint8 value_8;
    guint32 value_32;

    for (int prop_id = 0; prop_id < UCA_PROP_LAST; prop_id++) {
        property = uca_get_full_property(prop_id);
        uint32_t result = UCA_NO_ERROR;
        switch (property->type) {
            case uca_string:
                result = uca_cam_get_property(cam, prop_id, value_string, num_bytes);
                break;

            case uca_uint8t:
                result = uca_cam_get_property(cam, prop_id, &value_8, 0);
                g_sprintf(value_string, "%d", value_8);
                break;

            case uca_uint32t:
                result = uca_cam_get_property(cam, prop_id, &value_32, 0);
                g_sprintf(value_string, "%d", value_32);
                break;
        }

        /* Find first level root */
        gchar **tokens = g_strsplit(property->name, ".", 0);
        get_first_level_root(tree_store, &iter, tokens[0]);
        find_recursively(tree_store, &iter, &child, tokens, 1);

        if (result != UCA_NO_ERROR)
            g_sprintf(value_string, "n/a");

        int count = 0;
        while (tokens[count++] != NULL);

        gtk_tree_store_set(tree_store, &child, 
                COLUMN_NAME, tokens[count-2],
                COLUMN_VALUE, value_string,
                COLUMN_UNIT, uca_unit_map[property->unit],
                COLUMN_UCA_ID, prop_id,
                -1);

        g_strfreev(tokens);
    }

    g_free(value_string);
}

static void value_cell_data_func(GtkTreeViewColumn *column, GtkCellRenderer *cell, GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    uint32_t prop_id;

    gtk_tree_model_get(model, iter, COLUMN_UCA_ID, &prop_id, -1);
    struct uca_property *property = uca_get_full_property(prop_id);
    if (property->access & uca_write) {
        g_object_set(cell, "mode", GTK_CELL_RENDERER_MODE_EDITABLE, NULL);
        g_object_set(GTK_CELL_RENDERER_TEXT(cell), "editable", TRUE, NULL);
        g_object_set(GTK_CELL_RENDERER_TEXT(cell), "style", PANGO_STYLE_NORMAL, NULL);
    }
    else {
        g_object_set(cell, "mode", GTK_CELL_RENDERER_MODE_INERT, NULL);
        g_object_set(GTK_CELL_RENDERER_TEXT(cell), "editable", FALSE, NULL);
        g_object_set(GTK_CELL_RENDERER_TEXT(cell), "style", PANGO_STYLE_ITALIC, NULL);
    }
}

int main(int argc, char *argv[])
{
    struct uca *u = uca_init(NULL);
    if (u == NULL) {
        g_print("Couldn't initialize frame grabber and/or cameras\n");
        return 1;
    }

    int width, height, bits_per_sample;
    struct uca_camera *cam = u->cameras;
    uca_cam_get_property(cam, UCA_PROP_WIDTH, &width, 0);
    uca_cam_get_property(cam, UCA_PROP_HEIGHT, &height, 0);
    uca_cam_get_property(cam, UCA_PROP_BITDEPTH, &bits_per_sample, 0);

    uint32_t mode = UCA_TIMESTAMP_ASCII | UCA_TIMESTAMP_BINARY;
    uca_cam_set_property(cam, UCA_PROP_TIMESTAMP_MODE, &mode);

    g_thread_init(NULL);
    gdk_threads_init();
    gtk_init (&argc, &argv);

    GtkBuilder *builder = gtk_builder_new();
    GError *error = NULL;
    if (!gtk_builder_add_from_file(builder, "control.glade", &error)) {
        g_print("Couldn't load UI file!\n");
        g_print("Message: %s\n", error->message);
        g_free(error);
    }

    GtkWidget *window = GTK_WIDGET(gtk_builder_get_object(builder, "window"));
    GtkWidget *image = GTK_WIDGET(gtk_builder_get_object(builder, "image"));
    GdkPixbuf *pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, width, height);
    gtk_image_set_from_pixbuf(GTK_IMAGE(image), pixbuf);

    GtkTreeStore *tree_store = GTK_TREE_STORE(gtk_builder_get_object(builder, "cameraproperties"));
    GtkTreeViewColumn *value_column = GTK_TREE_VIEW_COLUMN(gtk_builder_get_object(builder, "valuecolumn"));
    GtkCellRendererText *value_renderer = GTK_CELL_RENDERER_TEXT(gtk_builder_get_object(builder, "valuecell"));
    fill_tree_store(tree_store, cam);

    gtk_tree_view_column_set_cell_data_func(value_column, GTK_CELL_RENDERER(value_renderer), value_cell_data_func, NULL, NULL);

    /* start grabbing and thread */
    int pixel_size = bits_per_sample == 8 ? 1 : 2;
    if (uca_cam_alloc(cam, 20) != UCA_NO_ERROR)
        g_print("Couldn't allocate buffer for 20 frames\n");

    ThreadData td;
    td.image  = image;
    td.pixbuf = pixbuf;
    td.buffer = (guchar *) g_malloc(pixel_size * width * height);
    td.pixels = gdk_pixbuf_get_pixels(pixbuf);
    td.width  = width;
    td.height = height;
    td.cam    = cam;
    td.u      = u;
    td.running = FALSE;
    td.pixel_size = pixel_size;
    td.scale = 65535.0f;
    td.statusbar = GTK_STATUSBAR(gtk_builder_get_object(builder, "statusbar"));
    td.statusbar_context_id = gtk_statusbar_get_context_id(td.statusbar, "Recording Information");

    gtk_builder_connect_signals(builder, &td);

    /* Configure value cell */
    ValueCellData value_cell_data;
    value_cell_data.thread_data = &td;
    value_cell_data.tree_store = tree_store;

    g_signal_connect(gtk_builder_get_object(builder, "valuecell"), "edited",
        G_CALLBACK(on_valuecell_edited), &value_cell_data);

    /* Configure scale adjustment */
    GtkAdjustment *adjustment = (GtkAdjustment *) gtk_builder_get_object(builder, "adjustment_scale");
    gtk_adjustment_configure(adjustment, 65535.0, 1.0, 65535.0, 0.5, 10.0, 0.0);
    g_signal_connect(adjustment, "value-changed",
        G_CALLBACK(on_adjustment_scale_value_changed), &td);
    
    gtk_widget_show(image);
    gtk_widget_show(window);

    gdk_threads_enter();
    gtk_main ();
    gdk_threads_leave();
    
    return 0;
}
