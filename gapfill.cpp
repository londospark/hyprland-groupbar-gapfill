/*
 * groupbar-gapfill -- let Hyprland's group tab bar (Super+G) fill the layout gap
 * above its window instead of leaving a strip of wallpaper between the tab and
 * the window above.
 *
 * How it works: the group bar decoration reserves space at the top of the window
 * for the tab strip, and the layout uses that reserved area to decide where the
 * window box starts. We hook CWindow::getFullWindowReservedArea() and shave the
 * top reservation by the layout gap (gaps_in top + bottom), so the layout places
 * the window that much higher on its tile. The tab is positioned relative to the
 * window box, so it moves up with it, overdraws the gap, and its top edge lands
 * on the window above.
 *
 * The decoration itself is left alone: same declared height, same box, same
 * input extents. That is why the whole tab stays clickable, including the part
 * that reaches up into the gap.
 *
 * Windows at the top edge of the workspace are clamped, so the outer gap
 * (general:gaps_out) is preserved where there is no window to fill up to.
 */

#include <plugins/PluginAPI.hpp>

#include <algorithm>
#include <charconv>
#include <string>
#include <vector>

#include <desktop/view/Window.hpp>
#include <event/EventBus.hpp>
#include <layout/LayoutManager.hpp>
#include <layout/target/Target.hpp>
#include <output/Monitor.hpp>
#include <render/decorations/DecorationPositioner.hpp>
#include <render/decorations/IHyprWindowDecoration.hpp>
#include <state/MonitorState.hpp>

static HANDLE              PHANDLE           = nullptr;
static CFunctionHook*      g_pReservedHook   = nullptr;
static CHyprSignalListener g_configReloadedListener;

/* How far the tab should reach up: the gap between two stacked windows. */
static double g_shaveWanted = 10.0;
/* Keeps the outer gap at the top edge of the workspace intact. */
static double g_gapsOutTop = 10.0;

using origGetFullWindowReservedArea = SBoxExtents (*)(void*);

static origGetFullWindowReservedArea g_pOrigGetFullWindowReservedArea = nullptr;

/* The reserved area is only consulted on a layout pass. */
static void recalcAllMonitors() {
    for (const auto& MON : State::monitorState()->monitors()) {
        if (MON)
            g_layoutManager->recalculateMonitor(MON);
    }
}

/* How far this particular window may reach up without eating gaps_out. */
static double shaveFor(Desktop::View::CWindow* window) {
    if (!window)
        return 0.0;

    const auto DECO = window->getDecorationByType(DECORATION_GROUPBAR);

    /* No tab bar, or a tab bar that is currently hidden: nothing to fill. */
    if (!DECO || DECO->getPositioningInfo().desiredExtents.topLeft.y <= 0.0)
        return 0.0;

    const auto TARGET = window->layoutTarget();
    const auto MON    = window->m_monitor.lock();

    if (!TARGET || !MON)
        return 0.0;

    const double LIMIT = MON->logicalBoxMinusReserved().y + g_gapsOutTop;
    const double TOP   = TARGET->position().y;

    return std::clamp(TOP - LIMIT, 0.0, g_shaveWanted);
}

static SBoxExtents hkGetFullWindowReservedArea(void* self) {
    SBoxExtents extents = g_pOrigGetFullWindowReservedArea(self);

    if (extents.topLeft.y <= 0.0)
        return extents;

    extents.topLeft.y = std::max(0.0, extents.topLeft.y - shaveFor(rc<Desktop::View::CWindow*>(self)));

    return extents;
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
    g_configReloadedListener = Event::bus()->m_events.config.reloaded.listen([] {
        const double OLDGAP  = g_shaveWanted;
        const double OLDOUT  = g_gapsOutTop;

        refreshGaps();

        if (OLDGAP != g_shaveWanted || OLDOUT != g_gapsOutTop)
            recalcAllMonitors();
    });

    const auto MATCHES = HyprlandAPI::findFunctionsByName(PHANDLE, "getFullWindowReservedArea");

    for (const auto& m : MATCHES) {
        if (!m.demangled.ends_with("CWindow::getFullWindowReservedArea()"))
            continue;

        g_pReservedHook = HyprlandAPI::createFunctionHook(PHANDLE, m.address, (void*)&hkGetFullWindowReservedArea);
        break;
    }

    if (!g_pReservedHook || !g_pReservedHook->hook())
        return {"groupbar-gapfill", "FAILED to hook CWindow::getFullWindowReservedArea", "Gareth Hubball (londospark)", "0.4"};

    g_pOrigGetFullWindowReservedArea = rc<origGetFullWindowReservedArea>(g_pReservedHook->m_original);

    /* Windows only move on a layout pass, so ask for one now. */
    recalcAllMonitors();

    return {"groupbar-gapfill", "Group tab bars overdraw the layout gap above their window", "Gareth Hubball (londospark)", "0.4"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_configReloadedListener.reset();

    if (g_pReservedHook)
        HyprlandAPI::removeFunctionHook(PHANDLE, g_pReservedHook);

    g_pReservedHook                  = nullptr;
    g_pOrigGetFullWindowReservedArea = nullptr;

    /* Put the windows back where they were. */
    recalcAllMonitors();
}
