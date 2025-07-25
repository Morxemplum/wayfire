#include "xdg-toplevel.hpp"
#include "wayfire/core.hpp"
#include <memory>
#include <wayfire/txn/transaction-manager.hpp>
#include <wlr/util/edges.h>
#include "wayfire/geometry.hpp"
#include "wayfire/scene-render.hpp"
#include "wayfire/toplevel-view.hpp"
#include "wayfire/toplevel.hpp"
#include "wayfire/txn/transaction-object.hpp"
#include "../view-impl.hpp"
#include "xdg-shell-protocol.h"

wf::xdg_toplevel_t::xdg_toplevel_t(wlr_xdg_toplevel *toplevel,
    std::shared_ptr<wf::scene::wlr_surface_node_t> main_surface)
{
    this->toplevel     = toplevel;
    this->main_surface = main_surface;

    on_surface_commit.set_callback([&] (void*) { handle_surface_commit(); });
    on_surface_commit.connect(&toplevel->base->surface->events.commit);

    on_toplevel_destroy.set_callback([&] (void*)
    {
        this->toplevel = NULL;
        on_toplevel_destroy.disconnect();
        on_surface_commit.disconnect();
        emit_ready();
    });
    on_toplevel_destroy.connect(&toplevel->base->events.destroy);
}

void wf::xdg_toplevel_t::request_native_size()
{
    if (toplevel && toplevel->base->initialized)
    {
        // This will trigger a client-driven transaction
        wlr_xdg_toplevel_set_size(toplevel, 0, 0);
    }
}

void wf::xdg_toplevel_t::commit()
{
    this->pending_ready = true;
    _committed = _pending;
    LOGC(TXNI, this, ": committing toplevel state mapped=", _pending.mapped,
        " geometry=", _pending.geometry, " tiled=", _pending.tiled_edges, " fs=", _pending.fullscreen,
        " margins=", _pending.margins.left, ",", _pending.margins.right, ",",
        _pending.margins.top, ",", _pending.margins.bottom);

    if (!this->toplevel || (_current.mapped && !_pending.mapped) || !toplevel->base->initialized)
    {
        // No longer mapped => we can do whatever
        emit_ready();
        return;
    }

    auto configure_serial = configure_surface_with_state(_pending, _current);
    if (configure_serial.has_value())
    {
        // Send frame done to let the client know it update its state as fast as possible.
        this->target_configure = *configure_serial;
        main_surface->send_frame_done(true);
    } else
    {
        emit_ready();
    }
}

std::optional<uint32_t> wf::xdg_toplevel_t::configure_surface_with_state(
    const wf::toplevel_state_t& desired_state, const wf::toplevel_state_t& base_state)
{
    wf::dimensions_t current_size =
        shrink_dimensions_by_margins(wf::dimensions(base_state.geometry), base_state.margins);
    if (desired_state.mapped && !base_state.mapped)
    {
        // We are trying to map the toplevel => check whether we should wait until it sets the proper
        // geometry, or whether we are 'only' mapping without resizing.
        current_size = get_current_wlr_toplevel_size();
    }

    const wf::dimensions_t desired_size =
        wf::shrink_dimensions_by_margins(wf::dimensions(desired_state.geometry), desired_state.margins);
    std::optional<uint32_t> configure_serial;

    if ((current_size != desired_size) && (desired_state.geometry.width > 0) &&
        (desired_state.geometry.height > 0))
    {
        const int configure_width  = std::max(1, desired_size.width);
        const int configure_height = std::max(1, desired_size.height);
        configure_serial = wlr_xdg_toplevel_set_size(this->toplevel, configure_width, configure_height);
    }

    if (base_state.tiled_edges != desired_state.tiled_edges)
    {
        wlr_xdg_toplevel_set_tiled(this->toplevel, desired_state.tiled_edges);
        auto version = wl_resource_get_version(toplevel->resource);
        if (version >= XDG_TOPLEVEL_STATE_TILED_LEFT_SINCE_VERSION)
        {
            configure_serial =
                wlr_xdg_toplevel_set_maximized(this->toplevel,
                    (desired_state.tiled_edges == TILED_EDGES_ALL));
        } else
        {
            configure_serial = wlr_xdg_toplevel_set_maximized(this->toplevel, !!desired_state.tiled_edges);
        }
    }

    if (base_state.fullscreen != desired_state.fullscreen)
    {
        configure_serial = wlr_xdg_toplevel_set_fullscreen(toplevel, desired_state.fullscreen);
    }

    return configure_serial;
}

void wf::xdg_toplevel_t::apply()
{
    xdg_toplevel_applied_state_signal event_applied;
    event_applied.old_state = current();

    // Damage the main surface before applying the new state. This ensures that the old position of the view
    // is damaged.
    if (main_surface->parent())
    {
        wf::scene::damage_node(main_surface->parent(), main_surface->parent()->get_bounding_box());
    }

    if (!toplevel)
    {
        // If toplevel does no longer exist, we can't change the size anymore.
        _committed.geometry.width  = _current.geometry.width;
        _committed.geometry.height = _current.geometry.height;
        if (_current.mapped == false)
        {
            // Avoid mapping if the view was already destroyed.
            _committed.mapped = false;
        }
    }

    this->_current = committed();
    const bool is_pending = wf::get_core().tx_manager->is_object_pending(shared_from_this());
    if (!is_pending)
    {
        // Adjust for potential moves due to gravity
        _pending = committed();
    }

    apply_pending_state();
    emit(&event_applied);

    // Damage the new position.
    if (main_surface->parent())
    {
        wf::scene::damage_node(main_surface->parent(), main_surface->parent()->get_bounding_box());
    }
}

void wf::xdg_toplevel_t::handle_surface_commit()
{
    pending_state.merge_state(toplevel->base->surface);
    if (toplevel->base->initial_commit)
    {
        wf::toplevel_state_t empty_state{};
        configure_surface_with_state(_committed, empty_state);
        wlr_xdg_surface_schedule_configure(toplevel->base);
        return;
    }

    const bool is_committed = wf::get_core().tx_manager->is_object_committed(shared_from_this());
    if (is_committed)
    {
        // TODO: handle overflow?
        if (this->toplevel->base->current.configure_serial < this->target_configure)
        {
            // Desired state not reached => wait for the desired state to be reached. In the meantime, send a
            // frame done so that the client can redraw faster.
            main_surface->send_frame_done(true);
            return;
        }

        const wf::dimensions_t real_size =
            expand_dimensions_by_margins(get_current_wlr_toplevel_size(), _committed.margins);
        wf::adjust_geometry_for_gravity(_committed, real_size);
        emit_ready();
        return;
    }

    const bool is_pending = wf::get_core().tx_manager->is_object_pending(shared_from_this());
    if (is_pending)
    {
        return;
    }

    auto toplevel_size =
        expand_dimensions_by_margins(get_current_wlr_toplevel_size(), _current.margins);
    if ((toplevel_size == wf::dimensions(current().geometry)) || !current().mapped)
    {
        if (toplevel)
        {
            if (this->wm_offset != wf::origin(toplevel->base->geometry))
            {
                // Trigger reppositioning in the view implementation
                this->wm_offset = wf::origin(toplevel->base->geometry);
                xdg_toplevel_applied_state_signal event_applied;
                event_applied.old_state = current();
                this->emit(&event_applied);
            }
        }

        // Size did not change, there are no transactions going on - apply the new texture directly
        apply_pending_state();
        return;
    }

    adjust_geometry_for_gravity(_pending, toplevel_size);
    LOGC(VIEWS, "Client-initiated resize to geometry ", pending().geometry);
    auto tx = wf::txn::transaction_t::create();
    tx->add_object(shared_from_this());
    wf::get_core().tx_manager->schedule_transaction(std::move(tx));
}

wf::geometry_t wf::xdg_toplevel_t::calculate_base_geometry()
{
    auto geometry = current().geometry;
    geometry.x     = geometry.x - wm_offset.x + _current.margins.left;
    geometry.y     = geometry.y - wm_offset.y + _current.margins.top;
    geometry.width = main_surface->get_bounding_box().width;
    geometry.height = main_surface->get_bounding_box().height;
    return geometry;
}

void wf::xdg_toplevel_t::apply_pending_state()
{
    if (toplevel)
    {
        pending_state.merge_state(toplevel->base->surface);
    }

    main_surface->apply_state(std::move(pending_state));

    if (toplevel)
    {
        this->wm_offset = wf::origin(toplevel->base->geometry);
    }
}

void wf::xdg_toplevel_t::emit_ready()
{
    if (pending_ready)
    {
        pending_ready = false;
        emit_object_ready(this);
    }
}

wf::dimensions_t wf::xdg_toplevel_t::get_current_wlr_toplevel_size()
{
    // Size did change => Start a new transaction to change the size.
    return wf::dimensions(toplevel->base->geometry);
}

wf::dimensions_t wf::xdg_toplevel_t::get_min_size()
{
    if (toplevel)
    {
        return wf::dimensions_t{toplevel->current.min_width, toplevel->current.min_height};
    }

    return {0, 0};
}

wf::dimensions_t wf::xdg_toplevel_t::get_max_size()
{
    if (toplevel)
    {
        return wf::dimensions_t{toplevel->current.max_width, toplevel->current.max_height};
    }

    return {0, 0};
}
