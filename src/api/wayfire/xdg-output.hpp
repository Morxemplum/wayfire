#ifndef XDG_OUTPUT_HPP
#define XDG_OUTPUT_HPP

#include "wayfire/util.hpp"
#include <vector>
#include <wayland-server-core.h>
extern "C" {
    #include <wlr/types/wlr_output_layout.h>
    #include <wlr/types/wlr_output_management_v1.h>
    #include "xdg-output-unstable-v1-protocol.h"
}

/** The main reason we are deviating from upstream is because we need to modify
  * the implementation on how it reports scale to XWayland clients. Upstream 
  * refuses to do this.
  * https://gitlab.freedesktop.org/wlroots/wlroots/-/issues/3849
  *
  * Be aware that wlroots considers wlr_xdg_output_v1 and 
  * wlr_xdg_output_manager_v1 unstable, so upgrading wlroots to a new version 
  * may involve having to modify this class to follow new changes. 
 */
namespace wf
{   

    /* TODO: Find a better way to translate these methods into C++. These ones 
     * are a pain to translate because their function values are bound to 
     * interfaces.
     */
    extern "C" void output_handle_destroy(struct wl_client *client, struct wl_resource *resource);
    extern "C" void output_manager_handle_destroy(struct wl_client *client, struct wl_resource *resource);
    void output_manager_handle_get_xdg_output(struct wl_client *client, struct wl_resource *resource, uint32_t id, struct wl_resource *output_resource);
    void output_manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id);

    // Forward declaration needed for xdg_output_manager_t
    class xdg_output_t;

    /* xdg_output_manager_t is our custom implementation of 
     * wlr_xdg_output_manager_v1. Converted from C to C++.
     */
    class xdg_output_manager_t {
        
        public:
            struct wl_global *global;
            // TODO: Replace this with output_layout_t
            struct wlr_output_layout *layout;

            struct std::vector<xdg_output_t*> outputs;

            static constexpr struct zxdg_output_manager_v1_interface wl_impl = {
                .destroy = wf::output_manager_handle_destroy,
                .get_xdg_output = wf::output_manager_handle_get_xdg_output,
            };

            // Constructor
            xdg_output_manager_t(struct wl_display *display, struct wlr_output_layout *layout);

            // Methods
            void add_output(struct wlr_output_layout_output *layout_output);
            void send_details();
            void destroy();

        private:
            struct wl_listener_wrapper on_layout_add;
            struct wl_listener_wrapper on_layout_change;
            struct wl_listener_wrapper on_layout_destroy;
    };

    /* xdg_output_manager_t is our custom implementation of wlr_xdg_output_v1. 
     * Converted from C to C++.
     */
    class xdg_output_t {
        public:
            xdg_output_manager_t *manager;
            struct wl_list resources;
            struct wl_list link;

            // TODO: Replace this with output_layout_output_t
            struct wlr_output_layout_output *layout_output;

            int32_t x, y;
            int32_t width, height;
            
            // Standalone constructor
            xdg_output_t(xdg_output_manager_t *manager, wlr_output_layout_output *layout);
            
            // Class methods
            static void handle_resource_destroy(struct wl_resource *resource);

            // Methods
            void send_details(struct wl_resource *resource);
            void update();
            void destroy();

        private:
            struct wl_listener_wrapper on_destroy;
            struct wl_listener_wrapper set_description;
    };
};

#endif /* end of include guard: XDG_OUTPUT_HPP */