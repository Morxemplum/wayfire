#include "wayfire/debug.hpp"
#include "wayfire/signal-definitions.hpp"
#include <string>
#include <wayfire/config/file.hpp>
#include <wayfire/config-backend.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/core.hpp>
#include <wayfire/util.hpp> // Added for wl_timer

#include <cstring>
#include <sys/inotify.h>
#include <filesystem>
#include <unistd.h>

#define INOT_BUF_SIZE (sizeof(inotify_event) + NAME_MAX + 1)

static std::string config_dir, config_file;
wf::config::config_manager_t *cfg_manager;
static int handle_config_updated(int fd, uint32_t mask, void *data);

static int wd_cfg_dir, wd_cfg_file;

static void add_watch(int fd)
{
    wd_cfg_dir  = inotify_add_watch(fd, config_dir.c_str(), IN_CREATE | IN_MOVED_TO);
    wd_cfg_file = inotify_add_watch(fd, config_file.c_str(), IN_CLOSE_WRITE);
}

static void reload_config()
{
    wf::config::load_configuration_options_from_file(*cfg_manager, config_file);
}

static const char *CONFIG_FILE_ENV = "WAYFIRE_CONFIG_FILE";

namespace wf
{
class dynamic_ini_config_t : public wf::config_backend_t
{
  private:
    struct wl_event_source *inotify_evtsrc = nullptr;
    int inotify_fd = -1;
    wf::wl_timer<false> reload_timer;
    wf::option_wrapper_t<int> config_reload_delay;

  public:
    /**
     * Schedules a configuration reload after a delay.
     * If a reload is already scheduled, it will be reset.
     */
    void schedule_config_reload()
    {
        uint32_t delay_ms = config_reload_delay;
        LOGD("Scheduling configuration file reload in ", delay_ms, "ms");

        reload_timer.set_timeout(delay_ms, [this] ()
        {
            this->do_reload_config();
        });
    }

    void init(wl_display *display, config::config_manager_t& config,
        const std::string& cfg_file) override
    {
        cfg_manager = &config;

        config_file = choose_cfg_file(cfg_file);
        std::filesystem::path path = std::filesystem::absolute(config_file);
        config_dir = path.parent_path();
        LOGI("Using config file: ", config_file.c_str());
        setenv(CONFIG_FILE_ENV, config_file.c_str(), 1);

        config = wf::config::build_configuration(
            get_xml_dirs(), SYSCONFDIR "/wayfire/defaults.ini", config_file);

        // Load option after building the config, as the option is not present before that.
        config_reload_delay.load_option("workarounds/config_reload_delay");
        if (check_auto_reload_option())
        {
            inotify_fd = inotify_init1(IN_CLOEXEC);
            add_watch(inotify_fd);

            inotify_evtsrc = wl_event_loop_add_fd(wl_display_get_event_loop(display),
                inotify_fd, WL_EVENT_READABLE, handle_config_updated, this);
        }
    }

    std::string choose_cfg_file(const std::string& cmdline_cfg_file)
    {
        std::string env_cfg_file = nonull(getenv(CONFIG_FILE_ENV));
        if (!cmdline_cfg_file.empty())
        {
            if ((env_cfg_file != nonull(NULL)) &&
                (cmdline_cfg_file != env_cfg_file))
            {
                LOGW("Wayfire config file specified in the environment is ",
                    "overridden by the command line arguments!");
            }

            return cmdline_cfg_file;
        }

        if (env_cfg_file != nonull(NULL))
        {
            return env_cfg_file;
        }

        std::string env_cfg_home = getenv("XDG_CONFIG_HOME") ?:
            (std::string(nonull(getenv("HOME"))) + "/.config");

        std::string vendored_cfg_file = env_cfg_home + "/wayfire/wayfire.ini";
        if (std::filesystem::exists(vendored_cfg_file))
        {
            return vendored_cfg_file;
        }

        return env_cfg_home + "/wayfire.ini";
    }

    bool check_auto_reload_option()
    {
        wf::option_wrapper_t<bool> auto_reload_config{"workarounds/auto_reload_config"};

        if (auto_reload_config)
        {
            return true;
        } else if (inotify_evtsrc != nullptr)
        {
            wl_event_source_remove(inotify_evtsrc);
            inotify_evtsrc = nullptr;
            close(inotify_fd);
            inotify_fd = -1;
            reload_timer.disconnect();
        }

        return false;
    }

    /**
     * Performs the actual configuration reload and emits the signal.
     * This is called by the wl_timer after the delay.
     */
    void do_reload_config()
    {
        LOGD("Reloading configuration file now!");
        reload_config();
        wf::reload_config_signal ev;
        wf::get_core().emit(&ev);
        check_auto_reload_option(); // Re-check auto-reload option after config has been reloaded
    }
};
}

static int handle_config_updated(int fd, uint32_t mask, void *data)
{
    if ((mask & WL_EVENT_READABLE) == 0)
    {
        return 0;
    }

    char buf[INOT_BUF_SIZE] __attribute__((aligned(alignof(inotify_event))));

    bool should_reload = false;
    inotify_event *event;

    // Reading from the inotify FD is guaranteed to not read partial events.
    // From inotify(7):
    // Each successful read(2) returns a buffer containing
    // one or more [..] structures
    auto len = read(fd, buf, INOT_BUF_SIZE);
    if (len < 0)
    {
        return 0;
    }

    const auto cfg_file_basename = std::filesystem::path(config_file).filename().string();

    for (char *ptr = buf;
         ptr < (buf + len);
         ptr += sizeof(inotify_event) + event->len)
    {
        event = reinterpret_cast<inotify_event*>(ptr);
        // We reload in two cases:
        //
        // 1. The config file itself was modified, or...
        should_reload |= event->wd == wd_cfg_file;
        // 2. The config file was moved nto or created inside the parent directory.
        if (event->len > 0)
        {
            // This is UB unless event->len > 0.
            auto name_matches = cfg_file_basename == event->name;

            if (name_matches)
            {
                inotify_rm_watch(fd, wd_cfg_file);
                wd_cfg_file =
                    inotify_add_watch(fd, (config_dir + "/" + cfg_file_basename).c_str(), IN_CLOSE_WRITE);
            }

            should_reload |= name_matches;
        }
    }

    if (should_reload)
    {
        LOGD("Detected configuration file change.");
        auto self = reinterpret_cast<wf::dynamic_ini_config_t*>(data);
        self->schedule_config_reload();
    }

    return 0;
}

DECLARE_WAYFIRE_CONFIG_BACKEND(wf::dynamic_ini_config_t);
