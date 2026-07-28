#include <sprout/config.h>
#include <sprout/log/log.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace {

pvz2_config_t g_config{};

constexpr char kDefaultSoName[]  = "libPVZ2.so";
constexpr char kDefaultObbName[] = "main.147.com.ea.game.pvz2_row.obb";

constexpr char kDefaultIni[] =
    "; Sprout configuration.\n"
    ";\n"
    "; This file is OPTIONAL and was created with every switch off. With no\n"
    "; config.ini present the behaviour is identical.\n"
    "\n"
    "[paths]\n"
    "; so  = lib/libPVZ2.so\n"
    "; obb = lib/main.147.com.ea.game.pvz2_row.obb\n"
    "\n"
    "[log]\n"
    "verbose = 0\n"
    "trace = 0\n"
    "pc_sample = 0\n"
    "input = 0\n"
    "; channel masks (1 = enabled, 0 = disabled):\n"
    "; chan_dex = 1\n"
    "; chan_jit = 1\n"
    "; chan_gfx = 1\n"
    "; chan_audio = 1\n"
    "; chan_input = 1\n"
    "; chan_engine = 1\n"
    "; chan_vfs = 1\n"
    "; chan_iap = 1\n"
    "; chan_version = 1\n"
    "\n"
    "[runtime]\n"
    "no_page_table = 0\n"
    "heap_quarantine = 0\n"
    "\n"
    "[gl]\n"
    "debug_clear = 0\n"
    "no_viewport_fix = 0\n"
    "flat_fragment = 0\n"
    "strict = 0\n"
    "diagnostics = 0\n"
    "\n"
    "[graphics]\n"
    "quality = high\n"
    "shadows = high\n"
    "render_scale = 1.0\n"
    "\n"
    "[video]\n"
    "mode = auto\n"
    "; width = 0\n"
    "; height = 0\n"
    "fullscreen = 0\n"
    "vsync = 1\n"
    "fps_limit = 60\n"
    "\n"
    "[game]\n"
    "; user_locale = en_US\n"
    "emulate_iap = 1\n"
    "persist_saves = 1\n"
    "network = none\n";

std::string trim(const std::string &s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

int parse_bool(const std::string &v) {
    const std::string t = lower(trim(v));
    if (t == "1" || t == "true" || t == "yes" || t == "on")  return 1;
    return 0;
}

void set_path(char (&dst)[512], const std::string &v) {
    std::snprintf(dst, sizeof(dst), "%s", v.c_str());
}

std::string base_with_sep(const char *base_dir) {
    std::string base = (base_dir != nullptr) ? base_dir : "";
    if (!base.empty() && base.back() != '/' && base.back() != '\\') base.push_back('/');
    return base;
}

std::string default_lib_path(const char *base_dir, const char *name) {
    return base_with_sep(base_dir) + "lib/" + name;
}

bool is_absolute(const char *p) {
    if (p == nullptr || p[0] == '\0') return false;
    if (p[0] == '/' || p[0] == '\\') return true;
    return std::isalpha((unsigned char)p[0]) && p[1] == ':';
}

void resolve_relative(char (&dst)[512], const char *base_dir) {
    if (dst[0] == '\0' || is_absolute(dst)) return;
    set_path(dst, base_with_sep(base_dir) + dst);
}

void apply(const std::string &section, const std::string &key, const std::string &val) {
    if (section == "log") {
        if (key == "verbose")            g_config.verbose = parse_bool(val);
        else if (key == "trace")         g_config.trace = parse_bool(val);
        else if (key == "pc_sample")     g_config.pc_sample = parse_bool(val);
        else if (key == "input")         g_config.input = parse_bool(val);
        else if (key == "chan_dex")      g_config.log_channels[LOG_CHAN_DEX]     = parse_bool(val);
        else if (key == "chan_jit")      g_config.log_channels[LOG_CHAN_JIT]     = parse_bool(val);
        else if (key == "chan_gfx")      g_config.log_channels[LOG_CHAN_GFX]     = parse_bool(val);
        else if (key == "chan_audio")    g_config.log_channels[LOG_CHAN_AUDIO]   = parse_bool(val);
        else if (key == "chan_input")    g_config.log_channels[LOG_CHAN_INPUT]   = parse_bool(val);
        else if (key == "chan_engine")   g_config.log_channels[LOG_CHAN_ENGINE]  = parse_bool(val);
        else if (key == "chan_vfs")      g_config.log_channels[LOG_CHAN_VFS]     = parse_bool(val);
        else if (key == "chan_iap")      g_config.log_channels[LOG_CHAN_IAP]     = parse_bool(val);
        else if (key == "chan_version")  g_config.log_channels[LOG_CHAN_VERSION] = parse_bool(val);
    } else if (section == "runtime") {
        if (key == "no_page_table")      g_config.no_page_table = parse_bool(val);
        else if (key == "heap_quarantine")
            g_config.heap_quarantine = (unsigned)std::strtoul(val.c_str(), nullptr, 0);
    } else if (section == "gl") {
        if (key == "debug_clear")        g_config.gl_debug_clear = parse_bool(val);
        else if (key == "no_viewport_fix") g_config.gl_no_viewport_fix = parse_bool(val);
        else if (key == "flat_fragment") g_config.gl_flat_fragment = parse_bool(val);
        else if (key == "strict")        g_config.gl_strict = parse_bool(val);
        else if (key == "diagnostics")   g_config.gl_diagnostics = parse_bool(val);
    } else if (section == "paths") {
        if (key == "so" && !trim(val).empty())  set_path(g_config.so_path, trim(val));
        else if (key == "obb" && !trim(val).empty()) set_path(g_config.obb_path, trim(val));
    } else if (section == "game") {
        if (key == "user_locale" && !trim(val).empty())
            std::snprintf(g_config.user_locale, sizeof(g_config.user_locale), "%s", trim(val).c_str());
        else if (key == "emulate_iap")  g_config.emulate_iap = parse_bool(val);
        else if (key == "persist_saves") g_config.persist_saves = parse_bool(val);
        else if (key == "network") {
            const std::string t = lower(trim(val));
            if (t == "wifi" || t == "on" || t == "1" || t == "true" || t == "yes")
                g_config.network_status = 2;
            else if (t == "mobile" || t == "cell")
                g_config.network_status = 1;
            else if (t == "none" || t == "off" || t == "0" || t == "false" || t == "no")
                g_config.network_status = 0;
        }
    } else if (section == "graphics") {
        if (key == "quality" && !trim(val).empty())
            std::snprintf(g_config.graphics_quality, sizeof(g_config.graphics_quality), "%s", lower(trim(val)).c_str());
        else if (key == "shadows" && !trim(val).empty())
            std::snprintf(g_config.graphics_shadows, sizeof(g_config.graphics_shadows), "%s", lower(trim(val)).c_str());
        else if (key == "render_scale" && !trim(val).empty())
            std::snprintf(g_config.render_scale, sizeof(g_config.render_scale), "%s", lower(trim(val)).c_str());
    } else if (section == "video") {
        if (key == "mode" && !trim(val).empty())
            std::snprintf(g_config.video_mode, sizeof(g_config.video_mode), "%s", lower(trim(val)).c_str());
        else if (key == "width")      g_config.video_width = (int)std::strtol(val.c_str(), nullptr, 0);
        else if (key == "height")     g_config.video_height = (int)std::strtol(val.c_str(), nullptr, 0);
        else if (key == "fullscreen") g_config.video_fullscreen = parse_bool(val);
        else if (key == "vsync")     g_config.vsync = parse_bool(val);
        else if (key == "fps_limit" || key == "fpslimit") {
            const std::string t = trim(val);
            char *end = nullptr;
            const long v = std::strtol(t.c_str(), &end, 10);
            if (end != t.c_str()) g_config.fps_limit = v > 0 ? (int)v : 0;
        }
    }
}

void parse(FILE *f) {
    std::string section;
    char line[1024];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        std::string s = trim(line);
        if (s.empty() || s[0] == ';' || s[0] == '#') continue;
        if (s.front() == '[' && s.back() == ']') {
            section = lower(trim(s.substr(1, s.size() - 2)));
            continue;
        }
        const std::size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        apply(section, lower(trim(s.substr(0, eq))), s.substr(eq + 1));
    }
}

void seed_default(const char *ini_path) {
    if (FILE *f = std::fopen(ini_path, "wb")) {
        std::fwrite(kDefaultIni, 1, std::strlen(kDefaultIni), f);
        std::fclose(f);
    }
}

}  // namespace

extern "C" void pvz2_config_load(const char *ini_path, const char *base_dir) {
    g_config = pvz2_config_t{};

    /* All log channels enabled by default */
    for (int i = 0; i < LOG_CHAN_COUNT; i++)
        g_config.log_channels[i] = 1;

    g_config.emulate_iap = 1;
    g_config.persist_saves = 1;
    g_config.fps_limit = 60;
    g_config.vsync = 1;
    g_config.network_status = 0;
    std::snprintf(g_config.graphics_quality, sizeof(g_config.graphics_quality), "high");
    std::snprintf(g_config.graphics_shadows, sizeof(g_config.graphics_shadows), "high");
    std::snprintf(g_config.render_scale, sizeof(g_config.render_scale), "1.0");

    if (ini_path != nullptr) {
        FILE *f = std::fopen(ini_path, "rb");
        if (f == nullptr) {
            seed_default(ini_path);
            f = std::fopen(ini_path, "rb");
        }
        if (f != nullptr) {
            parse(f);
            std::fclose(f);
        }
    }

    /* Push per-channel toggles into the logging system */
    for (int i = 0; i < LOG_CHAN_COUNT; i++)
        log_set_channel_enabled(i, g_config.log_channels[i]);

    if (g_config.so_path[0] == '\0')
        set_path(g_config.so_path, default_lib_path(base_dir, kDefaultSoName));
    else
        resolve_relative(g_config.so_path, base_dir);

    /* Auto-detect obb file */
    if (g_config.obb_path[0] == '\0') {
        bool found = false;
        std::string lib = base_with_sep(base_dir) + "lib";
        std::error_code ec;
        for (auto &e : std::filesystem::directory_iterator(lib, ec)) {
            if (e.path().extension() == ".obb") {
                set_path(g_config.obb_path, e.path().string());
                found = true;
                break;
            }
        }
        if (!found) {
            set_path(g_config.obb_path, default_lib_path(base_dir, kDefaultObbName));
        }
    } else {
        resolve_relative(g_config.obb_path, base_dir);
    }

    if (g_config.user_locale[0] == '\0')
        std::snprintf(g_config.user_locale, sizeof(g_config.user_locale), "en_US");

    if (g_config.save_dir[0] == '\0')
        set_path(g_config.save_dir, base_with_sep(base_dir) + "save");
    else
        resolve_relative(g_config.save_dir, base_dir);

    if (g_config.video_mode[0] == '\0')
        std::snprintf(g_config.video_mode, sizeof(g_config.video_mode), "auto");
}

extern "C" const pvz2_config_t *pvz2_config(void) { return &g_config; }
