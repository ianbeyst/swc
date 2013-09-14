#include "seat.h"

#include "evdev_device.h"
#include "util.h"
#include "binding.h"
#include "event.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct evdev_device_entry
{
    struct swc_evdev_device device;
    struct wl_listener event_listener;
    struct swc_seat * seat;
    struct wl_list link;
};

static void handle_key(struct swc_seat * seat, uint32_t time, uint32_t key,
                       uint32_t state)
{
    swc_keyboard_handle_key(&seat->keyboard, time, key, state);
}

static void handle_button(struct swc_seat * seat, uint32_t time,
                          uint32_t button, uint32_t state)
{
    swc_pointer_handle_button(&seat->pointer, time, button, state);
}

static void handle_relative_motion(struct swc_seat * seat, uint32_t time,
                                   wl_fixed_t dx, wl_fixed_t dy)
{
    swc_pointer_handle_relative_motion(&seat->pointer, time, dx, dy);
}

static void handle_axis_motion(struct swc_seat * seat, uint32_t time,
                               enum wl_pointer_axis axis, wl_fixed_t amount)
{
    swc_pointer_handle_axis(&seat->pointer, time, axis, amount);
}

static void handle_evdev_event(struct wl_listener * listener, void * data)
{
    struct evdev_device_entry * entry;
    struct swc_event * event = data;
    struct swc_evdev_device_event_data * evdev_data = event->data;

    entry = swc_container_of(listener, typeof(*entry), event_listener);

    switch (event->type)
    {
        case SWC_EVDEV_DEVICE_EVENT_KEY:
            handle_key(entry->seat, evdev_data->time, evdev_data->key.key,
                       evdev_data->key.state);
            break;
        case SWC_EVDEV_DEVICE_EVENT_BUTTON:
            handle_button(entry->seat, evdev_data->time,
                          evdev_data->button.button, evdev_data->button.state);
            break;
        case SWC_EVDEV_DEVICE_EVENT_RELATIVE_MOTION:
            handle_relative_motion(entry->seat, evdev_data->time,
                                   evdev_data->relative_motion.dx,
                                   evdev_data->relative_motion.dy);
            break;
        case SWC_EVDEV_DEVICE_EVENT_ABSOLUTE_MOTION:
            break;
        case SWC_EVDEV_DEVICE_EVENT_AXIS_MOTION:
            handle_axis_motion(entry->seat, evdev_data->time,
                               evdev_data->axis_motion.axis,
                               evdev_data->axis_motion.amount);
            break;
    }
}

static void handle_keyboard_focus_event(struct wl_listener * listener,
                                        void * data)
{
    struct swc_seat * seat = swc_container_of
        (listener, typeof(*seat), keyboard_focus_listener);
    struct swc_event * event = data;
    struct swc_input_focus_event_data * event_data = event->data;

    switch (event->type)
    {
        case SWC_INPUT_FOCUS_EVENT_CHANGED:
            if (event_data->new)
            {
                struct wl_client * client
                    = wl_resource_get_client(event_data->new->resource);

                /* Offer the selection to the new focus. */
                swc_data_device_offer_selection(&seat->data_device, client);
            }
            break;
    }
}

static void handle_data_device_event(struct wl_listener * listener, void * data)
{
    struct swc_seat * seat = swc_container_of
        (listener, typeof(*seat), data_device_listener);
    struct swc_event * event = data;

    switch (event->type)
    {
        case SWC_DATA_DEVICE_EVENT_SELECTION_CHANGED:
            if (seat->keyboard.focus.resource)
            {
                struct wl_client * client
                    = wl_resource_get_client(seat->keyboard.focus.resource);
                swc_data_device_offer_selection(&seat->data_device, client);
            }
            break;
    }
}

/* Wayland Seat Interface */
static void get_pointer(struct wl_client * client, struct wl_resource * resource,
                        uint32_t id)
{
    struct swc_seat * seat = wl_resource_get_user_data(resource);
    struct swc_pointer * pointer = &seat->pointer;

    swc_pointer_bind(pointer, client, id);
}

static void get_keyboard(struct wl_client * client, struct wl_resource * resource,
                         uint32_t id)
{
    struct wl_resource * client_resource;
    struct swc_seat * seat = wl_resource_get_user_data(resource);
    struct swc_keyboard * keyboard = &seat->keyboard;

    client_resource = swc_keyboard_bind(keyboard, client, id);
}

static void get_touch(struct wl_client * client, struct wl_resource * resource,
               uint32_t id)
{
}

struct wl_seat_interface seat_implementation = {
    .get_pointer = &get_pointer,
    .get_keyboard = &get_keyboard,
    .get_touch = &get_touch
};

static void bind_seat(struct wl_client * client, void * data, uint32_t version,
                      uint32_t id)
{
    struct swc_seat * seat = data;
    struct wl_resource * resource;

    if (version >= 2)
        version = 2;

    resource = wl_resource_create(client, &wl_seat_interface, version, id);
    wl_resource_set_implementation(resource, &seat_implementation, seat,
                                   &swc_remove_resource);
    wl_list_insert(&seat->resources, wl_resource_get_link(resource));

    if (version >= 2)
        wl_seat_send_name(resource, seat->name);

    wl_seat_send_capabilities(resource, seat->capabilities);
}

static void update_capabilities(struct swc_seat * seat)
{
    struct wl_resource * resource;

    wl_list_for_each(resource, &seat->resources, link)
        wl_seat_send_capabilities(resource, seat->capabilities);
}

static void add_device(struct swc_seat * seat, struct udev_device * udev_device)
{
    const char * device_seat;
    const char * device_path;
    struct evdev_device_entry * entry;

    device_seat = udev_device_get_property_value(udev_device, "ID_SEAT");

    /* If the ID_SEAT property is not set, the device belongs to seat0. */
    if (!device_seat)
        device_seat = "seat0";

    if (strcmp(device_seat, seat->name) != 0)
        return;

    entry = malloc(sizeof *entry);

    if (!entry)
    {
        printf("could not allocate evdev device\n");
        return;
    }

    entry->seat = seat;
    entry->event_listener.notify = &handle_evdev_event;

    if (!swc_evdev_device_initialize(&entry->device, udev_device))
    {
        free(entry);
        return;
    }

    wl_signal_add(&entry->device.event_signal, &entry->event_listener);

    if (~seat->capabilities & entry->device.capabilities)
    {
        seat->capabilities |= entry->device.capabilities;
        update_capabilities(seat);
    }

    wl_list_insert(&seat->devices, &entry->link);
}

bool swc_seat_initialize(struct swc_seat * seat, struct udev * udev,
                         const char * seat_name)
{
    seat->name = strdup(seat_name);
    seat->capabilities = 0;
    seat->keyboard_focus_listener.notify = &handle_keyboard_focus_event;
    seat->data_device_listener.notify = &handle_data_device_event;

    if (!swc_data_device_initialize(&seat->data_device))
    {
        printf("could not initialize data device\n");
        goto error_name;
    }

    if (!swc_keyboard_initialize(&seat->keyboard))
    {
        printf("could not initialize keyboard\n");
        goto error_data_device;
    }

    wl_signal_add(&seat->keyboard.focus.event_signal,
                  &seat->keyboard_focus_listener);

    if (!swc_pointer_initialize(&seat->pointer))
    {
        printf("could not initialize pointer\n");
        goto error_keyboard;
    }

    wl_signal_add(&seat->data_device.event_signal, &seat->data_device_listener);

    wl_list_init(&seat->resources);
    wl_signal_init(&seat->destroy_signal);
    wl_list_init(&seat->devices);
    swc_seat_add_devices(seat, udev);

    return true;

  error_keyboard:
    swc_keyboard_finish(&seat->keyboard);
  error_data_device:
    swc_data_device_finish(&seat->data_device);
  error_name:
    free(seat->name);
  error_base:
    return false;
}

void swc_seat_finish(struct swc_seat * seat)
{
    struct evdev_device_entry * entry, * tmp;

    wl_signal_emit(&seat->destroy_signal, seat);

    swc_pointer_finish(&seat->pointer);
    swc_keyboard_finish(&seat->keyboard);

    free(seat->name);

    wl_list_for_each_safe(entry, tmp, &seat->devices, link)
    {
        swc_evdev_device_finish(&entry->device);
        free(entry);
    }
}

void swc_seat_add_globals(struct swc_seat * seat, struct wl_display * display)
{
    wl_global_create(display, &wl_seat_interface, 2, seat, &bind_seat);
}

void swc_seat_add_event_sources(struct swc_seat * seat,
                                struct wl_event_loop * event_loop)
{
    struct evdev_device_entry * entry;

    wl_list_for_each(entry, &seat->devices, link)
    {
        swc_evdev_device_add_event_sources(&entry->device, event_loop);
    }
}

void swc_seat_add_devices(struct swc_seat * seat, struct udev * udev)
{
    struct udev_enumerate * enumerate;
    struct udev_list_entry * entry;
    const char * path;
    struct udev_device * device;

    enumerate = udev_enumerate_new(udev);
    udev_enumerate_add_match_subsystem(enumerate, "input");
    udev_enumerate_add_match_sysname(enumerate, "event[0-9]*");

    udev_enumerate_scan_devices(enumerate);

    udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(enumerate))
    {
        path = udev_list_entry_get_name(entry);
        device = udev_device_new_from_syspath(udev, path);
        add_device(seat, device);
        udev_device_unref(device);
    }

    udev_enumerate_unref(enumerate);
}
