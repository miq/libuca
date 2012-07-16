/* Copyright (C) 2011, 2012 Matthias Vogelgesang <matthias.vogelgesang@kit.edu>
   (Karlsruhe Institute of Technology)

   This library is free software; you can redistribute it and/or modify it
   under the terms of the GNU Lesser General Public License as published by the
   Free Software Foundation; either version 2.1 of the License, or (at your
   option) any later version.

   This library is distributed in the hope that it will be useful, but WITHOUT
   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
   FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more
   details.

   You should have received a copy of the GNU Lesser General Public License along
   with this library; if not, write to the Free Software Foundation, Inc., 51
   Franklin St, Fifth Floor, Boston, MA 02110, USA */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pcilib.h>
#include <errno.h>
#include "uca-camera.h"
#include "uca-ufo-camera.h"

#define PCILIB_SET_ERROR(err, err_type)                 \
    if (err != 0) {                                     \
        g_set_error(error, UCA_UFO_CAMERA_ERROR,        \
                err_type,                               \
                "pcilib: %s", strerror(err));           \
        return;                                         \
    }

#define UCA_UFO_CAMERA_GET_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE((obj), UCA_TYPE_UFO_CAMERA, UcaUfoCameraPrivate))

G_DEFINE_TYPE(UcaUfoCamera, uca_ufo_camera, UCA_TYPE_CAMERA)

static const guint SENSOR_WIDTH = 2048;
static const guint SENSOR_HEIGHT = 1088;
static const gdouble EXPOSURE_TIME_SCALE = 2.69e6;

/**
 * UcaUfoCameraError:
 * @UCA_UFO_CAMERA_ERROR_INIT: Initializing pcilib failed
 * @UCA_UFO_CAMERA_ERROR_START_RECORDING: Starting failed
 * @UCA_UFO_CAMERA_ERROR_STOP_RECORDING: Stopping failed
 * @UCA_UFO_CAMERA_ERROR_TRIGGER: Triggering a frame failed
 * @UCA_UFO_CAMERA_ERROR_NEXT_EVENT: No event happened
 * @UCA_UFO_CAMERA_ERROR_NO_DATA: No data was transmitted
 * @UCA_UFO_CAMERA_ERROR_MAYBE_CORRUPTED: Data is potentially corrupted
 */
GQuark uca_ufo_camera_error_quark()
{
    return g_quark_from_static_string("uca-ufo-camera-error-quark");
}

enum {
    PROP_SENSOR_TEMPERATURE = N_BASE_PROPERTIES,
    PROP_UFO_START,
    N_MAX_PROPERTIES = 512
};

static gint base_overrideables[] = {
    PROP_NAME,
    PROP_SENSOR_WIDTH,
    PROP_SENSOR_HEIGHT,
    PROP_SENSOR_HORIZONTAL_BINNING,
    PROP_SENSOR_VERTICAL_BINNING,
    PROP_SENSOR_MAX_FRAME_RATE,
    PROP_SENSOR_BITDEPTH,
    PROP_EXPOSURE_TIME,
    PROP_ROI_X,
    PROP_ROI_Y,
    PROP_ROI_WIDTH,
    PROP_ROI_HEIGHT,
    PROP_ROI_WIDTH_MULTIPLIER,
    PROP_ROI_HEIGHT_MULTIPLIER,
    PROP_HAS_STREAMING,
    PROP_HAS_CAMRAM_RECORDING,
    PROP_TRIGGER_MODE,
    0,
};

typedef struct _RegisterInfo {
    gchar  *name;
    guint   cached_value; 
} RegisterInfo;

static GParamSpec *ufo_properties[N_MAX_PROPERTIES] = { NULL, };

static guint N_PROPERTIES;
static GHashTable *ufo_property_table; /* maps from prop_id to RegisterInfo* */

struct _UcaUfoCameraPrivate {
    pcilib_t *handle;
    guint     n_bits;
};

static void ignore_messages(const char *format, ...)
{
}

static guint
read_register_value (pcilib_t *handle, const gchar *name)
{
    pcilib_register_value_t reg_value;

    pcilib_read_register(handle, NULL, name, &reg_value);
    return (guint) reg_value;
}

static int event_callback(pcilib_event_id_t event_id, pcilib_event_info_t *info, void *user)
{
    UcaCamera *camera = UCA_CAMERA(user);
    UcaUfoCameraPrivate *priv = UCA_UFO_CAMERA_GET_PRIVATE(camera);
    size_t error = 0;

    void *buffer = pcilib_get_data(priv->handle, event_id, PCILIB_EVENT_DATA, &error);

    if (buffer == NULL) {
        pcilib_trigger(priv->handle, PCILIB_EVENT0, 0, NULL);
        return PCILIB_STREAMING_CONTINUE;
    }

    camera->grab_func(buffer, camera->user_data);
    pcilib_return_data(priv->handle, event_id, PCILIB_EVENT_DATA, buffer);
    pcilib_trigger(priv->handle, PCILIB_EVENT0, 0, NULL);

    return PCILIB_STREAMING_CONTINUE;
}

UcaUfoCamera *uca_ufo_camera_new(GError **error)
{
    pcilib_model_t model = PCILIB_MODEL_DETECT;
    pcilib_model_description_t *model_description;
    pcilib_t *handle = pcilib_open("/dev/fpga0", model);
    guint prop = PROP_UFO_START;
    guint bit_mode;

    if (handle == NULL) {
        g_set_error(error, UCA_UFO_CAMERA_ERROR, UCA_UFO_CAMERA_ERROR_INIT,
                "Initializing pcilib failed");
        return NULL;
    }

    pcilib_set_error_handler(&ignore_messages, &ignore_messages);

    /* Generate properties from model description */
    model_description = pcilib_get_model_description(handle);
    ufo_property_table = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);

    for (guint i = 0; model_description->registers[i].name != NULL; i++) {
        GParamFlags flags;
        RegisterInfo *reg_info;
        gchar *prop_name;
        pcilib_register_description_t *reg;
        pcilib_register_value_t value;
        
        reg = &model_description->registers[i];

        switch (reg->mode) {
            case PCILIB_REGISTER_R:
                flags = G_PARAM_READABLE;
                break;
            case PCILIB_REGISTER_W:
            case PCILIB_REGISTER_W1C:
                flags = G_PARAM_WRITABLE;
                break;
            case PCILIB_REGISTER_RW:
            case PCILIB_REGISTER_RW1C:
                flags = G_PARAM_READWRITE;
                break;
        }

        pcilib_read_register (handle, NULL, reg->name, &value);
        reg_info = g_new0 (RegisterInfo, 1);
        reg_info->name = g_strdup (reg->name);
        reg_info->cached_value = (guint32) value;

        g_hash_table_insert (ufo_property_table, GINT_TO_POINTER (prop), reg_info);
        prop_name = g_strdup_printf ("ufo-%s", reg->name);

        ufo_properties[prop++] = g_param_spec_uint (
                prop_name, reg->description, reg->description,
                0, G_MAXUINT, reg->defvalue, flags);

        g_free (prop_name);
    }

    N_PROPERTIES = prop;

    UcaUfoCamera *camera = g_object_new(UCA_TYPE_UFO_CAMERA, NULL);
    UcaUfoCameraPrivate *priv = UCA_UFO_CAMERA_GET_PRIVATE(camera);

    bit_mode = read_register_value (handle, "bit_mode");
    priv->n_bits = bit_mode == 0 ? 12 : 10;
    priv->handle = handle;

    return camera;
}

static void uca_ufo_camera_start_recording(UcaCamera *camera, GError **error)
{
    g_return_if_fail(UCA_IS_UFO_CAMERA(camera));
    UcaUfoCameraPrivate *priv = UCA_UFO_CAMERA_GET_PRIVATE(camera);
    int err = pcilib_start(priv->handle, PCILIB_EVENT_DATA, PCILIB_EVENT_FLAGS_DEFAULT);
    PCILIB_SET_ERROR(err, UCA_UFO_CAMERA_ERROR_START_RECORDING);

    gboolean transfer_async = FALSE;
    g_object_get(G_OBJECT(camera),
            "transfer-asynchronously", &transfer_async,
            NULL);

    if (transfer_async) {
        pcilib_trigger(priv->handle, PCILIB_EVENT0, 0, NULL);
        pcilib_stream(priv->handle, &event_callback, camera);
    }
}

static void uca_ufo_camera_stop_recording(UcaCamera *camera, GError **error)
{
    g_return_if_fail(UCA_IS_UFO_CAMERA(camera));
    UcaUfoCameraPrivate *priv = UCA_UFO_CAMERA_GET_PRIVATE(camera);
    int err = pcilib_stop(priv->handle, PCILIB_EVENT_FLAGS_DEFAULT);
    PCILIB_SET_ERROR(err, UCA_UFO_CAMERA_ERROR_START_RECORDING);
}

static void uca_ufo_camera_start_readout(UcaCamera *camera, GError **error)
{
    g_return_if_fail(UCA_IS_UFO_CAMERA(camera));
    g_set_error(error, UCA_CAMERA_ERROR, UCA_CAMERA_ERROR_NOT_IMPLEMENTED,
            "Ufo camera does not support recording to internal memory");
}

static void uca_ufo_camera_grab(UcaCamera *camera, gpointer *data, GError **error)
{
    g_return_if_fail(UCA_IS_UFO_CAMERA(camera));
    UcaUfoCameraPrivate *priv = UCA_UFO_CAMERA_GET_PRIVATE(camera);
    const gsize size = SENSOR_WIDTH * SENSOR_HEIGHT * sizeof(guint16);
    pcilib_event_id_t event_id;
    pcilib_event_info_t event_info;
    size_t err = 0;

    err = pcilib_trigger(priv->handle, PCILIB_EVENT0, 0, NULL);
    PCILIB_SET_ERROR(err, UCA_UFO_CAMERA_ERROR_TRIGGER);

    err = pcilib_get_next_event(priv->handle, PCILIB_TIMEOUT_INFINITE, &event_id, sizeof(pcilib_event_info_t), &event_info);
    PCILIB_SET_ERROR(err, UCA_UFO_CAMERA_ERROR_NEXT_EVENT);

    if (*data == NULL)
        *data = g_malloc0(SENSOR_WIDTH * SENSOR_HEIGHT * sizeof(guint16));

    gpointer src = pcilib_get_data(priv->handle, event_id, PCILIB_EVENT_DATA, &err);

    if (src == NULL)
        PCILIB_SET_ERROR(err, UCA_UFO_CAMERA_ERROR_NO_DATA);

    /*
     * Apparently, we checked that err equals total size in previous version.
     * This is problematic because errno is positive and size could be equal
     * even when an error condition is met, e.g. with a very small ROI. However,
     * we don't know if src will always be NULL when an error occured.
     */
    /* assert(err == size); */

    memcpy(*data, src, size);

    /*
     * Another problem here. What does this help us? At this point we have
     * already overwritten the original buffer but can only know here if the
     * data is corrupted.
     */
    err = pcilib_return_data(priv->handle, event_id, PCILIB_EVENT_DATA, data);
    PCILIB_SET_ERROR(err, UCA_UFO_CAMERA_ERROR_MAYBE_CORRUPTED);
}

static void uca_ufo_camera_set_property(GObject *object, guint property_id, const GValue *value, GParamSpec *pspec)
{
    UcaUfoCameraPrivate *priv = UCA_UFO_CAMERA_GET_PRIVATE(object);

    switch (property_id) {
        case PROP_EXPOSURE_TIME:
            {
                pcilib_register_value_t reg_value = (pcilib_register_value_t) EXPOSURE_TIME_SCALE * g_value_get_double(value);
                pcilib_write_register(priv->handle, NULL, "exp_time", reg_value);
            }
            break;
        case PROP_ROI_X:
        case PROP_ROI_Y:
        case PROP_ROI_WIDTH:
        case PROP_ROI_HEIGHT:
            g_debug("ROI feature not implemented yet");
            break;

        default:
            {
                RegisterInfo *reg_info = g_hash_table_lookup (ufo_property_table, GINT_TO_POINTER (property_id));

                if (reg_info != NULL) {
                    pcilib_register_value_t reg_value;
                    
                    reg_value = g_value_get_uint (value);
                    pcilib_write_register(priv->handle, NULL, reg_info->name, reg_value);
                    pcilib_read_register (priv->handle, NULL, reg_info->name, &reg_value);
                    reg_info->cached_value = (guint) reg_value;
                }
                else
                    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
            }
            return;
    }
}


static void
uca_ufo_camera_get_property(GObject *object, guint property_id, GValue *value, GParamSpec *pspec)
{
    UcaUfoCameraPrivate *priv = UCA_UFO_CAMERA_GET_PRIVATE(object);

    switch (property_id) {
        case PROP_SENSOR_WIDTH: 
            g_value_set_uint(value, SENSOR_WIDTH);
            break;
        case PROP_SENSOR_HEIGHT: 
            g_value_set_uint(value, SENSOR_HEIGHT);
            break;
        case PROP_SENSOR_BITDEPTH:
            g_value_set_uint (value, priv->n_bits);
            break;
        case PROP_SENSOR_HORIZONTAL_BINNING:
            g_value_set_uint(value, 1);
            break;
        case PROP_SENSOR_VERTICAL_BINNING:
            g_value_set_uint(value, 1);
            break;
        case PROP_SENSOR_MAX_FRAME_RATE:
            g_value_set_float(value, 340.0);
            break;
        case PROP_SENSOR_TEMPERATURE:
            g_value_set_double (value, 0.17537 * read_register_value (priv->handle, "cmosis_temperature") - 198.03733);
            break;
        case PROP_EXPOSURE_TIME:
            g_value_set_double (value, read_register_value (priv->handle, "exp_time") / EXPOSURE_TIME_SCALE);
            break;
        case PROP_HAS_STREAMING:
            g_value_set_boolean(value, TRUE);
            break;
        case PROP_HAS_CAMRAM_RECORDING:
            g_value_set_boolean(value, FALSE);
            break;
        case PROP_ROI_X:
            g_value_set_uint(value, 0);
            break;
        case PROP_ROI_Y:
            g_value_set_uint(value, 0);
            break;
        case PROP_ROI_WIDTH:
            g_value_set_uint(value, SENSOR_WIDTH);
            break;
        case PROP_ROI_HEIGHT:
            g_value_set_uint(value, SENSOR_HEIGHT);
            break;
        case PROP_ROI_WIDTH_MULTIPLIER:
            g_value_set_uint(value, 1);
            break;
        case PROP_ROI_HEIGHT_MULTIPLIER:
            g_value_set_uint(value, 1);
            break;
        case PROP_NAME: 
            g_value_set_string(value, "Ufo Camera w/ CMOSIS CMV2000");
            break;
        case PROP_TRIGGER_MODE:
            break;
        default:
            {
                RegisterInfo *reg_info = g_hash_table_lookup (ufo_property_table, GINT_TO_POINTER (property_id));

                if (reg_info != NULL) 
                    g_value_set_uint (value, reg_info->cached_value);
                else
                    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
            }
            break;
    }
}

static void uca_ufo_camera_finalize(GObject *object)
{
    UcaUfoCameraPrivate *priv = UCA_UFO_CAMERA_GET_PRIVATE(object);
    pcilib_close(priv->handle);
    G_OBJECT_CLASS(uca_ufo_camera_parent_class)->finalize(object);
}

static void uca_ufo_camera_class_init(UcaUfoCameraClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    gobject_class->set_property = uca_ufo_camera_set_property;
    gobject_class->get_property = uca_ufo_camera_get_property;
    gobject_class->finalize = uca_ufo_camera_finalize;

    UcaCameraClass *camera_class = UCA_CAMERA_CLASS(klass);
    camera_class->start_recording = uca_ufo_camera_start_recording;
    camera_class->stop_recording = uca_ufo_camera_stop_recording;
    camera_class->start_readout = uca_ufo_camera_start_readout;
    camera_class->grab = uca_ufo_camera_grab;

    for (guint i = 0; base_overrideables[i] != 0; i++)
        g_object_class_override_property(gobject_class, base_overrideables[i], uca_camera_props[base_overrideables[i]]);

    ufo_properties[PROP_SENSOR_TEMPERATURE] =
        g_param_spec_double("sensor-temperature",
            "Temperature of the sensor",
            "Temperature of the sensor in degree Celsius",
            -G_MAXDOUBLE, G_MAXDOUBLE, 0.0,
            G_PARAM_READABLE);

    /*
     * This automatic property installation includes the properties created 
     * dynamically in uca_ufo_camera_new().
     */
    for (guint id = N_BASE_PROPERTIES; id < N_PROPERTIES; id++)
        g_object_class_install_property(gobject_class, id, ufo_properties[id]);

    g_type_class_add_private(klass, sizeof(UcaUfoCameraPrivate));
}

static void uca_ufo_camera_init(UcaUfoCamera *self)
{
    self->priv = UCA_UFO_CAMERA_GET_PRIVATE(self);
}
