#ifndef __UNIFIED_CAMERA_ACCESS_GRABBER_H
#define __UNIFIED_CAMERA_ACCESS_GRABBER_H

#include <stdbool.h>

/**
 * \file uca-grabber.h
 * \brief Abstract frame grabber model
 */

enum uca_grabber_constants {
    UCA_GRABBER_INVALID = -1,
    /* properties */
    UCA_GRABBER_WIDTH = 0,
    UCA_GRABBER_HEIGHT,
    UCA_GRABBER_WIDTH_MAX,
    UCA_GRABBER_WIDTH_MIN,
    UCA_GRABBER_OFFSET_X,
    UCA_GRABBER_OFFSET_Y,
    UCA_GRABBER_EXPOSURE,
    UCA_GRABBER_FORMAT,
    UCA_GRABBER_TRIGGER_MODE,
    UCA_GRABBER_CAMERALINK_TYPE,

    /* values */
    UCA_FORMAT_GRAY8,
    UCA_FORMAT_GRAY16,

    UCA_CL_8BIT_FULL_8,
    UCA_CL_8BIT_FULL_10,

    UCA_TRIGGER_FREERUN
};

/*
 * --- virtual methods --------------------------------------------------------
 */

/**
 * Camera probing and initialization.
 *
 * \return UCA_ERR_INIT_NOT_FOUND if grabber is not found or could not be initialized
 */
typedef uint32_t (*uca_grabber_init) (struct uca_grabber_t **grabber);

/**
 * Free frame grabber resouces.
 */
typedef uint32_t (*uca_grabber_destroy) (struct uca_grabber_t *grabber);

/**
 * Set a frame grabber property.
 *
 * \param[in] prop Name of the property as defined in uca_grabber_constants
 *
 * \return UCA_ERR_PROP_INVALID if property is not supported on the frame
 *   grabber or UCA_ERR_PROP_VALUE_OUT_OF_RANGE if value cannot be set.
 */
typedef uint32_t (*uca_grabber_set_property) (struct uca_grabber_t *grabber, enum uca_grabber_constants prop, void *data);

/**
 * Get a frame grabber property.
 *
 * \param[in] prop Name of the property as defined in uca_grabber_constants
 * 
 * \return UCA_ERR_PROP_INVALID if property is not supported on the frame grabber 
 */
typedef uint32_t (*uca_grabber_get_property) (struct uca_grabber_t *grabber, enum uca_grabber_constants prop, void *data);

/**
 * Allocate buffers with current width, height and bitdepth.
 *
 * \warning Subsequent changes of width and height might corrupt memory.
 */
typedef uint32_t (*uca_grabber_alloc) (struct uca_grabber_t *grabber, uint32_t pixel_size, uint32_t n_buffers);

/**
 * Begin acquiring frames.
 *
 * \param[in] n_frames Number of frames to acquire, -1 means infinite number
 *
 * \param[in] async Grab asynchronous if true
 */
typedef uint32_t (*uca_grabber_acquire) (struct uca_grabber_t *grabber, int32_t n_frames);

/**
 * Stop acquiring frames.
 */
typedef uint32_t (*uca_grabber_stop_acquire) (struct uca_grabber_t *grabber);

/**
 * Grab a frame.
 *
 * This method is usually called through the camera interface and not directly.
 *
 * \param[in] buffer The pointer of the frame buffer is set here
 *
 * \param[out] frame_number Number of the grabbed frame
 */
typedef uint32_t (*uca_grabber_grab) (struct uca_grabber_t *grabber, void **buffer, uint32_t *frame_number);

/**
 * Function pointer to a grab callback.
 * 
 * Register such a callback function with uca_grabber_register_callback() to
 * receive data as soon as it is delivered.
 *
 * \param[in] image_number Current frame number
 *
 * \param[in] buffer Image data
 */
typedef void (*uca_grabber_grab_callback) (uint32_t image_number, void *buffer);

/**
 * Register callback for given frame grabber. To actually start receiving
 * frames, call uca_grabber_acquire().
 *
 * \param[in] grabber The grabber for which the callback should be installed
 *
 * \param[in] cb Callback function for when a frame arrived
 */
typedef uint32_t (*uca_grabber_register_callback) (struct uca_grabber_t *grabber, uca_grabber_grab_callback cb);


/**
 * Represents a frame grabber abstraction, that concrete frame grabber
 * implementations must implement.
 *
 * A uca_grabber_t structure is never used directly but only via the
 * uca_camera_t interface in order to keep certain duplicated properties in sync
 * (e.g. image dimensions can be set on frame grabber and camera).
 */
struct uca_grabber_t {
    struct uca_grabber_t    *next;

    /* Function pointers to grabber-specific methods */
    uca_grabber_destroy      destroy;
    uca_grabber_set_property set_property;
    uca_grabber_get_property get_property;
    uca_grabber_alloc        alloc;
    uca_grabber_acquire      acquire;
    uca_grabber_stop_acquire stop_acquire;
    uca_grabber_grab         grab;
    uca_grabber_register_callback register_callback;

    /* Private */
    uca_grabber_grab_callback callback;
    bool asynchronous;
    void *user;
};

#endif
