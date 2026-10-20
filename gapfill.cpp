/*
 * groupbar-gapfill -- let Hyprland's group tab bar (Super+G) fill the layout gap
 * above its window instead of leaving a strip of wallpaper between the tab and
 * the window above.
 *
 * How it works: the groupbar decoration tells the decoration positioner how
 * much space it wants reserved at the top of the window. That reserved height
 * moves the client surface down and anchors the tab at the tile top. We hook
 * CHyprGroupBarDecoration::getPositioningInfo() and shave the top reservation
 * by the layout gap, so the positioner starts the client surface that much
 * higher -- and the tab, which is anchored to the surface, with it. The tab
 * then overdraws the gap and its top edge lands on the window above.
 *
 * The tab keeps its configured height and its title stays centred. Window
 * geometry is otherwise unchanged: the window grows by the gap at the top. The
 * decoration extents still cover the overdraw, so damage tracking is fine.
 *
 * Windows at the top edge of the workspace are left alone, so the outer gap
 * (general:gaps_out) is preserved where there is no window to fill up to.
 */

#include <plugins/PluginAPI.hpp>

#include <algorithm>
#include <charconv>
#include <string>
#include <vector>

#define private public
#include <render/decorations/IHyprWindowDecoration.hpp>
#undef private

#include <desktop/view/Window.hpp>
#include <event/EventBus.hpp>
#include <layout/target/Target.hpp>
#include <output/Monitor.hpp>
#include <render/decorations/DecorationPositioner.hpp>

static HANDLE              PHANDLE                        = nullptr;
static CFunctionHook*      g_pGetPositioningInfoHook       = nullptr;
static CHyprSignalListener g_configReloadedListener;

/* How far the tab should reach up: the gap between two stacked windows. */
static double g_shaveWanted = 10.0;
/* Keeps the outer gap at the top edge of the workspace intact. */
static double g_gapsOutTop = 10.0;

using origGetPositioningInfo = SDecorationPositioningInfo (*)(void*);

static origGetPositioningInfo g_pOrigGetPositioningInfo = nullptr;

/* How far this particular groupbar may reach up without eating gaps_out. */
static double shaveFor(void* self) {
    auto* const DECO = rc<IHyprWindowDecoration*>(self);

    if (!DECO)
        return g_shaveWanted;

    const auto WINDOW = DECO->m_window.lock();

    if (!WINDOW)
        return g_shaveWanted;

    const auto TARGET = WINDOW->layoutTarget();
    const auto MON    = WINDOW->m_monitor.lock();

    if (!TARGET || !MON)
        return g_shaveWanted;

    const double LIMIT = MON->logicalBoxMinusReserved().y + g_gapsOutTop;
    const double TOP   = TARGET->position().y;

    return std::clamp(TOP - LIMIT, 0.0, g_shaveWanted);
}

static SDecorationPositioningInfo hkGetPositioningInfo(void* self) {
    SDecorationPositioningInfo info = g_pOrigGetPositioningInfo(self);

    if (info.desiredExtents.topLeft.y <= 0.0)
        return info;

    info.desiredExtents.topLeft.y = std::max(0.0, info.desiredExtents.topLeft.y - shaveFor(self));

    return info;
}

/* Parse the four values out of e.g. "css gap data: 5 5 5 5". */
static std::vector<double> readCssGap(const std::string& option) {
    const std::string out = HyprlandAPI::invokeHyprctlCommand("getoption", option);

    std::vector<double> nums;
    const char*         cur = out.c_str();
    const char*         end = cur + out.size();

    while (cur < end) {
        while (cur < end && (*cur < '0' || *cur > '9'))
            ++cur;
        if (cur >= end)
            break;
        double     v         = 0;
        const auto [PTR, EC] = std::from_chars(cur, end, v);
        if (EC != std::errc())
            break;
        nums.push_back(v);
        cur = PTR;
    }

    return nums;
}

static void refreshGaps() {
    const auto GAPSIN  = readCssGap("general:gaps_in");
    const auto GAPSOUT = readCssGap("general:gaps_out");

    /* The gap between two stacked windows is one top plus one bottom gap. */
    g_shaveWanted = GAPSIN.size() == 4 ? GAPSIN[0] + GAPSIN[2] : 10.0;
    g_gapsOutTop  = GAPSOUT.size() == 4 ? GAPSOUT[0] : 10.0;
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    refreshGaps();
    g_configReloadedListener = Event::bus()->m_events.config.reloaded.listen([] { refreshGaps(); });

    const auto MATCHES = HyprlandAPI::findFunctionsByName(PHANDLE, "getPositioningInfo");

    for (const auto& m : MATCHES) {
        if (!m.demangled.starts_with("CHyprGroupBarDecoration::getPositioningInfo"))
            continue;

        g_pGetPositioningInfoHook = HyprlandAPI::createFunctionHook(PHANDLE, m.address, (void*)&hkGetPositioningInfo);
        break;
    }

    if (!g_pGetPositioningInfoHook || !g_pGetPositioningInfoHook->hook())
        return {"groupbar-gapfill", "FAILED to hook CHyprGroupBarDecoration::getPositioningInfo", "Gareth Hubball (londospark)", "0.3"};

    g_pOrigGetPositioningInfo = rc<origGetPositioningInfo>(g_pGetPositioningInfoHook->m_original);

    return {"groupbar-gapfill", "Group tab bars overdraw the layout gap above their window", "Gareth Hubball (londospark)", "0.3"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_configReloadedListener.reset();

    if (g_pGetPositioningInfoHook)
        HyprlandAPI::removeFunctionHook(PHANDLE, g_pGetPositioningInfoHook);

    g_pGetPositioningInfoHook = nullptr;
    g_pOrigGetPositioningInfo = nullptr;
}
