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
    
    this->outputs = {};
    wlr_output_layout_output *layout_output;
    wl_list_for_each(layout_output, &layout->outputs, link) {
        this->add_output(layout_output);
    }

    this->on_layout_add.set_callback([&] (void *data) {
        wlr_output_layout_output *layout_output = static_cast<wlr_output_layout_output *>(data);
        this->add_output(layout_output);
    });
    this->on_layout_add.connect(&layout->events.add);
    this->on_layout_change.set_callback(this->send_details());
    this->on_layout_change.connect(&layout->events.change);
    this->on_layout_destroy.set_callback(this->destroy());
    this->on_layout_destroy.connect(&layout->events.destroy);
    // this->on_display_destroy.set_callback(this->destroy());
    // wl_display_add_destroy_listener(display, &this->on_display_destroy->listener);
}

void xdg_output_manager_t::add_output(struct wlr_output_layout_output *layout_output) {
    xdg_output_t output = xdg_output_t(this, layout_output);
    this->outputs.add(output);
    output.update();
}

void xdg_output_manager_t::send_details() {
    for (xdg_output_t *output : this->outputs) {
        output->update();
    }
}

// manager_destroy()
void xdg_output_manager_t::destroy() {
    for (auto i = this->outputs.begin(); i != this->outputs.end(); ) {
        this->outputs[i]->destroy();
        this->outputs.erase(i);
    }
    
    // wl_list_remove(&this->on_display_destroy->listener.link);
    this->on_layout_add.disconnect();
    this->on_layout_change.disconnect();
    this->on_layout_destroy.disconnect();
    delete this;
}


// XDG_OUTPUT_T

// Standalone constructor
xdg_output_t::xdg_output_t(xdg_output_manager_t *manager, wlr_output_layout_output *layout) {
    this->manager = manager;
    this->layout_output = layout;
    wl_list_init(&this->resources);

    this->on_destroy.set_callback(this->destroy());
    this->on_destroy.connect(&layout->output->events.destroy);
    this->set_description.set_callback([&] (void *data) {
        wlr_output *output = this->layout_output->output;

        if (output->description == NULL) {
            return;
        }

        struct wl_resource *resource;
        wl_resource_for_each(resource, &this->resources) {
            if (wl_resource_get_version(resource) >= OUTPUT_DESCRIPTION_MUTABLE_SINCE_VERSION) {
                zxdg_output_v1_send_description(resource, output->description);
            }
        }
    });
    this->set_description.connect(&layout->output->events.description);
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

void xdg_output_t::destroy() {
    wl_resource *resource, *tmp;
    wl_resource_for_each_safe(resource, tmp, &this->resources) {
        wl_list_remove(wl_resource_get_link(resource));
        wl_list_init(wl_resource_get_link(resource));
    }
    this->on_destroy.disconnect();
    this->set_description.disconnect();
    wl_list_remove(&this->link);
    delete this;
}

}