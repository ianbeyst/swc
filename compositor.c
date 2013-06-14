#include <stdlib.h>
#include <stdio.h>
#include <libudev.h>
#include <gbm.h>

#include "compositor.h"
#include "tty.h"
#include "output.h"
#include "surface.h"
#include "event.h"
#include "region.h"
#include "data_device_manager.h"

const char default_seat[] = "seat0";

struct repaint_operation
{
    struct swc_compositor * compositor;
    struct swc_output * output;
};

static void repaint_output(void * data)
{
    struct repaint_operation * operation = data;
    struct swc_compositor * compositor = operation->compositor;

    swc_renderer_repaint_output(&compositor->renderer, operation->output,
                                &compositor->surfaces);

    swc_output_switch_buffer(operation->output);

    free(operation);
}

static void schedule_repaint_for_output(struct swc_compositor * compositor,
                                        struct swc_output * output)
{
    struct wl_event_loop * event_loop;
    struct repaint_operation * operation;

    if (output->repaint_scheduled)
        return;

    operation = malloc(sizeof *operation);
    operation->compositor = compositor;
    operation->output = output;

    event_loop = wl_display_get_event_loop(compositor->display);
    wl_event_loop_add_idle(event_loop, &repaint_output, operation);
    output->repaint_scheduled = true;
}

static bool handle_key(struct swc_keyboard * keyboard, uint32_t time,
                       uint32_t key, uint32_t state)
{
    struct swc_seat * seat;
    struct swc_binding * binding;
    struct swc_compositor * compositor;
    char keysym_name[64];

    seat = wl_container_of(keyboard, seat, keyboard);
    compositor = wl_container_of(seat, compositor, seat);

    if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
    {
        xkb_keysym_t keysym;

        keysym = xkb_state_key_get_one_sym(seat->xkb.state, key + 8);

        wl_array_for_each(binding, &compositor->key_bindings)
        {
            if (binding->value == keysym)
            {
                xkb_mod_mask_t mod_mask;
                uint32_t modifiers = 0;
                mod_mask = xkb_state_serialize_mods(seat->xkb.state,
                                                    XKB_STATE_MODS_EFFECTIVE);
                mod_mask = xkb_state_mod_mask_remove_consumed(seat->xkb.state, key + 8,
                                                              mod_mask);

                if (mod_mask & (1 << seat->xkb.indices.ctrl))
                    modifiers |= SWC_MOD_CTRL;
                if (mod_mask & (1 << seat->xkb.indices.alt))
                    modifiers |= SWC_MOD_ALT;
                if (mod_mask & (1 << seat->xkb.indices.super))
                    modifiers |= SWC_MOD_SUPER;
                if (mod_mask & (1 << seat->xkb.indices.shift))
                    modifiers |= SWC_MOD_SHIFT;

                if (binding->modifiers == SWC_MOD_ANY
                    || binding->modifiers == modifiers)
                {
                    binding->handler(time, keysym, binding->data);
                    printf("\t-> handled\n");
                    return true;
                }
            }
        }
    }

    return false;
}

struct swc_keyboard_handler keyboard_handler = {
    .key = &handle_key,
};



/* XXX: maybe this should go in swc_drm */
static void handle_tty_event(struct wl_listener * listener, void * data)
{
    struct swc_event * event = data;
    struct swc_compositor * compositor;

    compositor = wl_container_of(listener, compositor, tty_listener);

    switch (event->type)
    {
        case SWC_TTY_VT_ENTER:
            swc_drm_set_master(&compositor->drm);
            break;
        case SWC_TTY_VT_LEAVE:
            swc_drm_drop_master(&compositor->drm);
            break;
    }
}

static void handle_surface_event(struct wl_listener * listener, void * data)
{
    struct swc_event * event = data;
    struct swc_surface * surface = event->data;
    struct swc_compositor * compositor = surface->compositor_state.compositor;

    switch (event->type)
    {
        case SWC_SURFACE_ATTACH:
            swc_renderer_attach(&compositor->renderer, &compositor->outputs,
                                surface, surface->state.buffer);
            break;
        case SWC_SURFACE_REPAINT:
        {
            struct swc_output * output;
            wl_list_for_each(output, &compositor->outputs, link)
            {
                if (surface->output_mask & (1 << output->id))
                    schedule_repaint_for_output(compositor, output);
            }
            break;
        }
    }
}

static void handle_drm_event(struct wl_listener * listener, void * data)
{
    struct swc_event * event = data;
    struct swc_compositor * compositor;

    compositor = wl_container_of(listener, compositor, drm_listener);

    switch (event->type)
    {
        case SWC_DRM_PAGE_FLIP:
        {
            struct swc_output * output = event->data;

            output->repaint_scheduled = false;
            output->front_buffer ^= 1;

            break;
        }
    }
}

static void handle_surface_destroy(struct wl_listener * listener, void * data)
{
    struct wl_resource * resource = data;
    struct swc_surface * surface = resource->data;

    wl_list_remove(&surface->link);

    free(surface);
}

static void handle_terminate(uint32_t time, uint32_t value, void * data)
{
    struct wl_display * display = data;
    printf("handling terminate\n");
    wl_display_terminate(display);
}

static void handle_switch_vt(uint32_t time, uint32_t value, void * data)
{
    struct swc_tty * tty = data;
    uint8_t vt = value - XKB_KEY_XF86Switch_VT_1 + 1;
    printf("handle switch vt%u\n", vt);
    if (vt != tty->vt)
        swc_tty_switch_vt(tty, vt);
}

static void create_surface(struct wl_client * client,
                           struct wl_resource * resource, uint32_t id)
{
    printf("compositor_create_surface\n");
    struct swc_compositor * compositor = resource->data;
    struct swc_surface * surface;
    struct swc_output * output;

    surface = malloc(sizeof *surface);

    if (!surface)
    {
        wl_resource_post_no_memory(resource);
        return;
    }

    output = wl_container_of(compositor->outputs.next, output, link);

    /* Initialize compositor state */
    surface->compositor_state = (struct swc_compositor_surface_state) {
        .compositor = compositor,
        .event_listener = (struct wl_listener) {
            .notify = &handle_surface_event
        },
        .destroy_listener = {
            .notify = &handle_surface_destroy
        }
    };

    swc_surface_initialize(surface, client, id);
    wl_signal_add(&surface->event_signal,
                  &surface->compositor_state.event_listener);
    wl_resource_add_destroy_listener(surface->resource,
                  &surface->compositor_state.destroy_listener);

    wl_list_insert(&compositor->surfaces, &surface->link);
    surface->output_mask |= 1 << output->id;
}

static void create_region(struct wl_client * client,
                          struct wl_resource * resource, uint32_t id)
{
    struct swc_region * region;

    region = malloc(sizeof *region);

    if (!region)
    {
        wl_resource_post_no_memory(resource);
        return;
    }

    swc_region_initialize(region, client, id);
}

struct wl_compositor_interface compositor_implementation = {
    .create_surface = &create_surface,
    .create_region = &create_region
};

static void bind_compositor(struct wl_client * client, void * data,
                            uint32_t version, uint32_t id)
{
    struct swc_compositor * compositor = data;

    wl_client_add_object(client, &wl_compositor_interface,
                         &compositor_implementation, id, compositor);
}

bool swc_compositor_initialize(struct swc_compositor * compositor,
                               struct wl_display * display)
{
    struct wl_event_loop * event_loop;
    struct udev_device * drm_device;
    struct wl_list * outputs;
    xkb_keysym_t keysym;

    compositor->display = display;
    compositor->tty_listener.notify = &handle_tty_event;
    compositor->drm_listener.notify = &handle_drm_event;

    compositor->udev = udev_new();

    if (compositor->udev == NULL)
    {
        printf("could not initialize udev context\n");
        goto error_base;
    }

    event_loop = wl_display_get_event_loop(display);

    if (!swc_tty_initialize(&compositor->tty, event_loop, 2))
    {
        printf("could not initialize tty\n");
        goto error_udev;
    }

    wl_signal_add(&compositor->tty.event_signal, &compositor->tty_listener);

    /* TODO: configurable seat */
    if (!swc_seat_initialize(&compositor->seat, compositor->udev,
                             default_seat))
    {
        printf("could not initialize seat\n");
        goto error_tty;
    }

    swc_seat_add_event_sources(&compositor->seat, event_loop);
    compositor->seat.keyboard.handler = &keyboard_handler;

    /* TODO: configurable seat */
    if (!swc_drm_initialize(&compositor->drm, compositor->udev, default_seat))
    {
        printf("could not initialize drm\n");
        goto error_seat;
    }

    wl_signal_add(&compositor->drm.event_signal, &compositor->drm_listener);
    swc_drm_add_event_sources(&compositor->drm, event_loop);

    compositor->gbm = gbm_create_device(compositor->drm.fd);

    if (!compositor->gbm)
    {
        printf("could not create gbm device\n");
        goto error_drm;
    }

    if (!swc_egl_initialize(&compositor->egl, compositor->gbm))
    {
        printf("could not initialize egl\n");
        goto error_gbm;
    }

    if (!swc_egl_bind_display(&compositor->egl, compositor->display))
    {
        printf("could not bind egl display\n");
        swc_egl_finish(&compositor->egl);
        goto error_gbm;
    }

    if (!swc_renderer_initialize(&compositor->renderer, &compositor->drm,
                                 compositor->gbm))
    {
        printf("could not initialize renderer\n");
        goto error_egl;
    }

    outputs = swc_drm_create_outputs(&compositor->drm);

    if (outputs)
    {
        wl_list_init(&compositor->outputs);
        wl_list_insert_list(&compositor->outputs, outputs);
        free(outputs);
    }
    else
    {
        printf("could not create outputs\n");
        goto error_renderer;
    }

    wl_list_init(&compositor->surfaces);
    wl_array_init(&compositor->key_bindings);
    wl_signal_init(&compositor->destroy_signal);

    swc_compositor_add_key_binding(compositor,
        SWC_MOD_CTRL | SWC_MOD_ALT, XKB_KEY_BackSpace, &handle_terminate, display);

    for (keysym = XKB_KEY_XF86Switch_VT_1;
         keysym <= XKB_KEY_XF86Switch_VT_12;
         ++keysym)
    {
        swc_compositor_add_key_binding(compositor, SWC_MOD_ANY, keysym,
                                       &handle_switch_vt, &compositor->tty);
    }


    return true;

  error_renderer:
    swc_renderer_finalize(&compositor->renderer);
  error_egl:
    swc_egl_unbind_display(&compositor->egl, compositor->display);
    swc_egl_finish(&compositor->egl);
  error_gbm:
    gbm_device_destroy(compositor->gbm);
  error_drm:
    swc_drm_finish(&compositor->drm);
  error_seat:
    swc_seat_finish(&compositor->seat);
  error_tty:
    swc_tty_finish(&compositor->tty);
  error_udev:
    udev_unref(compositor->udev);
  error_base:
    free(compositor);
    return false;
}

void swc_compositor_finish(struct swc_compositor * compositor)
{
    struct swc_output * output, * tmp;

    wl_signal_emit(&compositor->destroy_signal, compositor);

    wl_array_release(&compositor->key_bindings);

    wl_list_for_each_safe(output, tmp, &compositor->outputs, link)
    {
        swc_output_finish(output);
        free(output);
    }

    swc_egl_unbind_display(&compositor->egl, compositor->display);
    swc_egl_finish(&compositor->egl);
    gbm_device_destroy(compositor->gbm);
    swc_drm_finish(&compositor->drm);
    swc_seat_finish(&compositor->seat);
    swc_tty_finish(&compositor->tty);
    udev_unref(compositor->udev);
}

void swc_compositor_add_globals(struct swc_compositor * compositor,
                                struct wl_display * display)
{
    struct swc_output * output;

    wl_display_add_global(display, &wl_compositor_interface, compositor,
                          &bind_compositor);

    swc_data_device_manager_add_globals(display);
    swc_seat_add_globals(&compositor->seat, display);

    wl_list_for_each(output, &compositor->outputs, link)
    {
        swc_output_add_globals(output, display);
    }
}

void swc_compositor_add_key_binding(struct swc_compositor * compositor,
                                    uint32_t modifiers, uint32_t value,
                                    swc_binding_handler_t handler, void * data)
{
    struct swc_binding * binding;

    binding = wl_array_add(&compositor->key_bindings, sizeof *binding);
    binding->value = value;
    binding->modifiers = modifiers;
    binding->handler = handler;
    binding->data = data;
}

