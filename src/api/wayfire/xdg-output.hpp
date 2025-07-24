#ifndef XDG_OUTPUT_HPP
#define XDG_OUTPUT_HPP

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

    /* xdg_output_manager_t is our custom implementation of 
     * wlr_xdg_output_manager_v1. Converted from C to C++.
     */
    class xdg_output_manager_t {
        
        public:
            struct wl_global *global;
            // TODO: Replace this with output_layout_t
            struct wlr_output_layout *layout;

            struct wl_list outputs;

            struct {
                struct wl_signal destroy;
            } events;

            static constexpr struct zxdg_output_manager_v1_interface wl_impl = {
                .destroy = wf::output_manager_handle_destroy,
                .get_xdg_output = wf::output_manager_handle_get_xdg_output,
            };

            // Constructor
            xdg_output_manager_t(struct wl_display *display, struct wlr_output_layout *layout);

            static void handle_layout_add(struct wl_listener *listener, void *data);
            static void handle_layout_change(struct wl_listener *listener, void *data);
            static void handle_layout_destroy(struct wl_listener *listener, void *data);
            static void handle_display_destroy(struct wl_listener *listener, void *data);

            // The C counterparts use wlr_xdg_output_manager_v1 as the first parameter.
            // That parameter has been stripped here to convert them to methods
            void add_output(struct wlr_output_layout_output *layout_output);
            void send_details();
            void destroy(); // manager_destroy()

        private:
            struct wl_listener display_destroy;
            struct wl_listener layout_add;
            struct wl_listener layout_change;
            struct wl_listener layout_destroy;
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
            
            // Wlroots uses wl_container_of and wl_list_for_each to instantiate
            // wlr_xdg_output_v1. The following class methods below will assist
            // in constructing instances based on the provided listener.
            static void handle_output_destroy(struct wl_listener *listener, void *data);
            static void handle_output_description(struct wl_listener *listener, void *data);
            
            static void handle_resource_destroy(struct wl_resource *resource);

            // The C counterparts use wlr_xdg_output_v1 as the first parameter.
            // That parameter has been stripped here to convert them to methods
            void send_details(struct wl_resource *resource);
            void update();
            void o_destroy();

        private:
            struct wl_listener destroy;
            struct wl_listener description;
    };
};

#endif /* end of include guard: XDG_OUTPUT_HPP */