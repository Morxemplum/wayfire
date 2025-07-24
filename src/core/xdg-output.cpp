#include "wayfire/xdg-output.hpp"

#include <cassert>
#include <wayland-server-protocol.h>

#include <wayland-util.h>
#include <wayland-server-core.h>
#include <wayland-server.h>

#define OUTPUT_MANAGER_VERSION 3
#define OUTPUT_DONE_DEPRECATED_SINCE_VERSION 3
#define OUTPUT_DESCRIPTION_MUTABLE_SINCE_VERSION 3

namespace wf 
{

extern "C" void output_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

extern "C" void output_manager_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

void output_manager_handle_get_xdg_output(struct wl_client *client, struct wl_resource *resource, uint32_t id, struct wl_resource *output_resource) {
    const struct zxdg_output_manager_v1_interface output_impl = {
        .destroy = wf::output_handle_destroy,
    };

    assert(wl_resource_instance_of(resource, &zxdg_output_manager_v1_interface, &xdg_output_manager_t::wl_impl));

    xdg_output_manager_t *self = static_cast<xdg_output_manager_t *> (wl_resource_get_user_data(resource));

    wlr_output_layout *layout = self->layout;
    wlr_output *output = wlr_output_from_resource(output_resource);

    wl_resource *xdg_output_resource = wl_resource_create(client, &zxdg_output_v1_interface, wl_resource_get_version(resource), id);
    if (!xdg_output_resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(xdg_output_resource, &output_impl, NULL, xdg_output_t::handle_resource_destroy);

    if (output == NULL) {
        wl_list_init(wl_resource_get_link(xdg_output_resource));
        return;
    }

    wlr_output_layout_output *layout_output = wlr_output_layout_get(layout, output);
    assert(layout_output);

    xdg_output_t *_xdg_output, *xdg_output = NULL;
    wl_list_for_each(_xdg_output, &self->outputs, link) {
        if (_xdg_output->layout_output == layout_output) {
            xdg_output = _xdg_output;
            break;
        }
    }
    assert(xdg_output);

    wl_list_insert(&xdg_output->resources, wl_resource_get_link(xdg_output_resource));

    // Name and description should only be sent once per output
	uint32_t xdg_version = wl_resource_get_version(xdg_output_resource);
	if (xdg_version >= ZXDG_OUTPUT_V1_NAME_SINCE_VERSION) {
		zxdg_output_v1_send_name(xdg_output_resource, output->name);
	}
	if (xdg_version >= ZXDG_OUTPUT_V1_DESCRIPTION_SINCE_VERSION &&
			output->description != NULL) {
		zxdg_output_v1_send_description(xdg_output_resource,
			output->description);
	}

    xdg_output->send_details(xdg_output_resource);

    uint32_t wl_version = wl_resource_get_version(output_resource);
    if (wl_version >= WL_OUTPUT_DONE_SINCE_VERSION && xdg_version >= OUTPUT_DONE_DEPRECATED_SINCE_VERSION) {
        wl_output_send_done(output_resource);
    }
}

void output_manger_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    xdg_output_manager_t *self = static_cast<xdg_output_manager_t *>(data);
    wl_resource *resource = wl_resource_create(client, &zxdg_output_manager_v1_interface, version, id);

    if (resource == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &xdg_output_manager_t::wl_impl, self, NULL);
}

// XDG_OUTPUT_MANAGER_T

// Constructor
xdg_output_manager_t::xdg_output_manager_t(struct wl_display *display, struct wlr_output_layout *layout) {
    this->layout = layout;
    
    this->global = wl_global_create(display, &zxdg_output_manager_v1_interface, OUTPUT_MANAGER_VERSION, this, wf::output_manager_bind);
    if (!this->global) {
        delete this;
        return;
    }
    
    wl_list_init(&this->outputs);
    wlr_output_layout_output *layout_output;
    wl_list_for_each(layout_output, &layout->outputs, link) {
        this->add_output(layout_output);
    }

    wl_signal_init(&this->events.destroy);

    this->layout_add.notify = handle_layout_add;
    wl_signal_add(&layout->events.add, &this->layout_add);
    this->layout_change.notify = handle_layout_change;
    wl_signal_add(&layout->events.change, &this->layout_change);
    this->layout_destroy.notify = handle_layout_destroy;
    wl_signal_add(&layout->events.destroy, &this->layout_destroy);
    this->display_destroy.notify = handle_display_destroy;
    wl_display_add_destroy_listener(display, &this->display_destroy);

}

void xdg_output_manager_t::handle_layout_add(struct wl_listener *listener, void *data) {
    xdg_output_manager_t *self = wl_container_of(listener, self, layout_add);
    wlr_output_layout_output *layout_output = static_cast<wlr_output_layout_output *>(data);
    self->add_output(layout_output);
}
void xdg_output_manager_t::handle_layout_change(struct wl_listener *listener, void *data) {
    xdg_output_manager_t *self = wl_container_of(listener, self, layout_change);
    self->send_details();
}
void xdg_output_manager_t::handle_layout_destroy(struct wl_listener *listener, void *data) {
    xdg_output_manager_t *self = wl_container_of(listener, self, layout_destroy);
    self->destroy();
}
void xdg_output_manager_t::handle_display_destroy(struct wl_listener *listener, void *data) {
    xdg_output_manager_t *self = wl_container_of(listener, self, display_destroy);
    self->destroy();
}

void xdg_output_manager_t::add_output(struct wlr_output_layout_output *layout_output) {
    xdg_output_t output = xdg_output_t(this, layout_output);
    wl_list_insert(&this->outputs, &output.link);
    output.update();
}

void xdg_output_manager_t::send_details() {
    xdg_output_t *output;
    wl_list_for_each(output, &this->outputs, link) {
        output->update();
    }
}

// manager_destroy()
void xdg_output_manager_t::destroy() {
    xdg_output_t *output, *tmp;
    wl_list_for_each_safe(output, tmp, &this->outputs, link) {
        output->o_destroy();
    }

    wl_signal_emit_mutable(&this->events.destroy, this);
    assert(wl_list_empty(&this->events.destroy.listener_list));
    
    wl_list_remove(&this->display_destroy.link);
    wl_list_remove(&this->layout_add.link);
    wl_list_remove(&this->layout_change.link);
    wl_list_remove(&this->layout_destroy.link);
    delete this;
}


// XDG_OUTPUT_T

// Standalone constructor
xdg_output_t::xdg_output_t(xdg_output_manager_t *manager, wlr_output_layout_output *layout) {
    this->manager = manager;
    this->layout_output = layout;
    wl_list_init(&this->resources);

    this->destroy.notify = handle_output_destroy;
    wl_signal_add(&layout->output->events.destroy, &this->destroy);
    this->description.notify = handle_output_description;
    wl_signal_add(&layout->output->events.description, &this->description);
}

// Constructors that are based on wl_listeners
void xdg_output_t::handle_output_destroy(struct wl_listener *listener, void *data) {
    xdg_output_t *self = wl_container_of(listener, self, destroy);
    self->o_destroy();
}

void xdg_output_t::handle_output_description(struct wl_listener *listener, void *data){
    xdg_output_t *self = wl_container_of(listener, self, description);
    wlr_output *output = self->layout_output->output;

    if (output->description == NULL) {
        return;
    }
    
    struct wl_resource *resource;
    wl_resource_for_each(resource, &self->resources) {
        if (wl_resource_get_version(resource) >= OUTPUT_DESCRIPTION_MUTABLE_SINCE_VERSION) {
            zxdg_output_v1_send_description(resource, output->description);
        }
    }
}

void xdg_output_t::handle_resource_destroy(struct wl_resource *resource) {
    wl_list_remove(wl_resource_get_link(resource));
}

// Methods
void xdg_output_t::send_details(struct wl_resource *resource) {
    // TODO: We may have to tamper with this to send this data
    // differently depending on if this is XWayland
    
    zxdg_output_v1_send_logical_position(resource, this->x, this->y);
    zxdg_output_v1_send_logical_size(resource, this->width, this->height);
    if (wl_resource_get_version(resource) < OUTPUT_DONE_DEPRECATED_SINCE_VERSION) {
        zxdg_output_v1_send_done(resource);
    }
}

void xdg_output_t::update() {
    bool updated = false;
    
    // Check position
    if (this->layout_output->x != this->x || this->layout_output->x != this->y) {
        this->x = this->layout_output->x;
        this->y = this->layout_output->y;
        updated = true;
    }
    
    // Check size
    int width, height;
    wlr_output_effective_resolution(this->layout_output->output, &width, &height);
    if (this->width != width || this->height != height) {
        this->width = width;
        this->height = height;
        updated = true;
    }
    
    if (updated) {
        struct wl_resource *resource;
        wl_resource_for_each(resource, &this->resources) {
            this->send_details(resource);
        }

        wlr_output_schedule_done(this->layout_output->output);
    }
}

void xdg_output_t::o_destroy() {
    wl_resource *resource, *tmp;
    wl_resource_for_each_safe(resource, tmp, &this->resources) {
        wl_list_remove(wl_resource_get_link(resource));
        wl_list_init(wl_resource_get_link(resource));
    }
    wl_list_remove(&this->destroy.link);
    wl_list_remove(&this->description.link);
    wl_list_remove(&this->link);
    delete this;
}

}