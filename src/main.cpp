#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/layout/algorithm/TiledAlgorithm.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <unistd.h>

extern "C" double lua_tonumberx(lua_State* state, int index, int* isNumber);

static HANDLE g_pluginHandle = nullptr;

namespace {

constexpr const char* PLUGIN_NAME = "hyprsplitrow";
constexpr const char* TILED_ALGO_NAME = "splitrow";
constexpr const char* PLUGIN_VERSION = "0.1";
constexpr double DEFAULT_TOP_ROW_RATIO = 1.0 / 3.0;
constexpr double MIN_TOP_ROW_RATIO = 0.10;
constexpr double MAX_TOP_ROW_RATIO = 0.90;
constexpr bool PROFILE_FADE_ENABLED = true;
constexpr float PROFILE_FADE_IN_START_ALPHA = 0.0F;
constexpr float PROFILE_FADE_OUT_END_ALPHA = 0.01F;
// Fade-out hides only after reaching near-invisible alpha to avoid a mid-fade blink.


class CSplitRowAlgorithm;

std::vector<CSplitRowAlgorithm*> g_instances;
constexpr int PROFILE_COUNT = 10;

struct STopProfileData {
    std::vector<std::uintptr_t> order;
    std::unordered_map<std::uintptr_t, double> weights;
    std::unordered_map<std::uintptr_t, CBox> lastBoxes;
    std::uintptr_t fullscreenWindowKey = 0;
};

struct STopProfileState {
    int activeProfile = 1;
    std::unordered_set<std::uintptr_t> windowKeys;
    std::unordered_set<std::uintptr_t> hiddenWindowKeys;
    std::unordered_map<std::uintptr_t, int> windowProfiles;
    std::unordered_map<std::uintptr_t, SP<Layout::ITarget>> targets;
    std::unordered_map<int, STopProfileData> profiles;
    bool inactiveProfilesDirty = true;
    std::optional<CBox> inactiveProfilesTopArea;
};

struct STopProfileTransition {
    std::unordered_set<std::uintptr_t> fadingOutKeys;
    std::unordered_set<std::uintptr_t> fadingInKeys;
};

struct SBottomFullscreenState {
    std::uintptr_t windowKey = 0;
    bool forceRestoreSpaceUpdate = false;
};

enum class EFocusedSplitRow {
    Bottom,
    Top,
};

enum class ESpawnIntentSource {
    FocusedWindow,
    TopProfileSwitch,
    WorkspaceSwitch,
};

struct SSpawnIntent {
    EFocusedSplitRow row = EFocusedSplitRow::Bottom;
    ESpawnIntentSource source = ESpawnIntentSource::FocusedWindow;
    std::uintptr_t focusedWindowKeyWhenSourceSet = 0;
};

struct SActiveSplitRowWindow {
    PHLWINDOW window;
    CSplitRowAlgorithm* algorithm = nullptr;
    std::uintptr_t key = 0;
};

STopProfileState g_topState;
STopProfileTransition g_topProfileTransition;
SSpawnIntent g_spawnIntent;
std::unordered_map<std::uintptr_t, double> g_bottomWindowWeights;
std::vector<std::uintptr_t> g_bottomWindowOrder;
std::unordered_set<std::uintptr_t> g_bottomFullscreenWindowKeys;
double g_topRowRatio = DEFAULT_TOP_ROW_RATIO;

Hyprutils::Signal::CHyprSignalListener g_windowCloseListener;
Hyprutils::Signal::CHyprSignalListener g_windowDestroyListener;
Hyprutils::Signal::CHyprSignalListener g_windowActiveListener;
Hyprutils::Signal::CHyprSignalListener g_workspaceActiveListener;


void recalculateAllInstances();
void savePersistentState();
void loadPersistentState();
bool restoreTopStateFromPersistence(const SP<Layout::ITarget>& target);
bool windowIsClosingOrDead(const PHLWINDOW& window);
bool validProfile(int profile);
std::uintptr_t windowKey(const PHLWINDOW& window);
PHLWINDOW windowFromKey(std::uintptr_t key);
bool moveTopWindowInOrder(const PHLWINDOW& window, int delta);
void showTopProfile(int profile);
std::uintptr_t focusCandidateKeyForTopProfile(int profile);
std::vector<std::uintptr_t> liveWindowKeysForTopProfile(int profile);
void resetProfileFadeAlpha(const PHLWINDOW& window);
void setProfileFadeInputBlocked(const PHLWINDOW& window, bool blocked);
void cancelTopProfileTransition();
void startTopProfileFade(int fromProfile, int toProfile);
SDispatchResult toggleActiveFocusedFullscreen();
SDispatchResult resizeActiveWindowByWeight(int delta);
void setRowFullscreenVisualState(const PHLWINDOW& window, bool enabled);
bool clearTopFullscreenForWindow(std::uintptr_t key, bool restoreVisuals);
void clearAllTopFullscreenState(bool restoreVisuals);
SP<Layout::ITarget> topProfileFocusCandidateAfterRemoving(std::uintptr_t key);


bool focusWindowByKey(std::uintptr_t key);
void setTopProfileHiddenState(const PHLWINDOW& window, bool hidden);
void markInactiveTopProfilesDirty();
bool boxesNearlyEqual(const CBox& a, const CBox& b);
void setSpawnIntent(EFocusedSplitRow row, ESpawnIntentSource source);
void updateSpawnIntentFromFocusedWindow(const PHLWINDOW& window);
std::optional<EFocusedSplitRow> rowFromCursorForArea(const CBox& area);
void saveBottomWindowOrder(const std::vector<std::uintptr_t>& order);
void removeBottomWindowFromPersistentState(std::uintptr_t key);
SDispatchResult setTopRowRatio(double ratio);
SDispatchResult releaseActiveWindowFromSplitRowState();
SDispatchResult moveActiveWindowToWorkspace(int workspace);


std::filesystem::path persistentStatePath() {
    if (const char* xdgCache = std::getenv("XDG_CACHE_HOME"); xdgCache && *xdgCache)
        return std::filesystem::path{xdgCache} / "hyprsplitrow" / "state.txt";

    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path{home} / ".cache" / "hyprsplitrow" / "state.txt";

    return std::filesystem::temp_directory_path() / "hyprsplitrow-state.txt";
}

void markInactiveTopProfilesDirty() {
    g_topState.inactiveProfilesDirty = true;
}

bool boxesNearlyEqual(const CBox& a, const CBox& b) {
    constexpr double EPSILON = 0.5;

    return std::abs(a.x - b.x) < EPSILON
        && std::abs(a.y - b.y) < EPSILON
        && std::abs(a.w - b.w) < EPSILON
        && std::abs(a.h - b.h) < EPSILON;
}

std::optional<std::uintptr_t> parseWindowKey(const std::string& value) {
    if (value.empty())
        return std::nullopt;

    try {
        std::size_t parsed = 0;
        const auto key = static_cast<std::uintptr_t>(std::stoull(value, &parsed, 16));

        if (parsed != value.size())
            return std::nullopt;

        return key;
    } catch (...) {
        return std::nullopt;
    }
}

void loadPersistentState() {
    const auto path = persistentStatePath();
    std::ifstream file{path};

    if (!file)
        return;

    int savedPid = 0;
    int activeProfile = g_topState.activeProfile;
    std::unordered_map<int, STopProfileData> loadedTopProfiles;
    std::unordered_map<std::uintptr_t, int> loadedWindowProfiles;
    std::unordered_map<std::uintptr_t, double> loadedBottomWeights;
    std::vector<std::uintptr_t> loadedBottomOrder;
    std::unordered_set<std::uintptr_t> loadedBottomOrderKeys;
    std::unordered_set<std::uintptr_t> loadedBottomFullscreenWindowKeys;

    std::string line;
    while (std::getline(file, line)) {
        std::istringstream stream{line};
        std::string kind;
        stream >> kind;

        if (kind == "version") {
            int version = 0;
            stream >> version;

            if (version != 3)
                return;
        } else if (kind == "hyprland_pid") {
            stream >> savedPid;
        } else if (kind == "active") {
            int profile = 0;
            stream >> profile;

            if (validProfile(profile))
                activeProfile = profile;
        } else if (kind == "top") {
            int profile = 0;
            std::string keyText;
            stream >> profile >> keyText;

            const auto key = parseWindowKey(keyText);
            if (!validProfile(profile) || !key || *key == 0)
                continue;

            if (loadedWindowProfiles.contains(*key))
                continue;

            loadedWindowProfiles[*key] = profile;
            loadedTopProfiles[profile].order.push_back(*key);
        } else if (kind == "top_weight") {
            int profile = 0;
            std::string keyText;
            double weight = 1.0;
            stream >> profile >> keyText >> weight;

            const auto key = parseWindowKey(keyText);
            if (!validProfile(profile) || !key || *key == 0 || !std::isfinite(weight) || weight <= 0.0)
                continue;

            loadedTopProfiles[profile].weights[*key] = std::clamp(weight, 0.2, 10.0);
        } else if (kind == "top_fullscreen") {
            int profile = 0;
            std::string keyText;
            stream >> profile >> keyText;

            const auto key = parseWindowKey(keyText);
            if (!validProfile(profile) || !key || *key == 0)
                continue;

            loadedTopProfiles[profile].fullscreenWindowKey = *key;
        } else if (kind == "bottom_weight") {
            std::string keyText;
            double weight = 1.0;
            stream >> keyText >> weight;

            const auto key = parseWindowKey(keyText);
            if (!key || *key == 0 || !std::isfinite(weight) || weight <= 0.0)
                continue;

            loadedBottomWeights[*key] = std::clamp(weight, 0.2, 10.0);
        } else if (kind == "bottom") {
            std::string keyText;
            stream >> keyText;

            const auto key = parseWindowKey(keyText);
            if (!key || *key == 0 || loadedBottomOrderKeys.contains(*key))
                continue;

            loadedBottomOrder.push_back(*key);
            loadedBottomOrderKeys.insert(*key);
        } else if (kind == "bottom_fullscreen") {
            std::string keyText;
            stream >> keyText;

            const auto key = parseWindowKey(keyText);
            if (!key || *key == 0)
                continue;

            loadedBottomFullscreenWindowKeys.insert(*key);
        }
    }

    // Window pointer addresses are only trusted inside the same Hyprland
    // process. This keeps old state files from a previous Hyprland session
    // from moving unrelated windows after address reuse.
    if (savedPid != 0 && savedPid != getpid())
        return;

    for (auto& [profile, profileData] : loadedTopProfiles) {
        if (profileData.fullscreenWindowKey != 0) {
            const auto profileIt = loadedWindowProfiles.find(profileData.fullscreenWindowKey);
            if (profileIt == loadedWindowProfiles.end() || profileIt->second != profile)
                profileData.fullscreenWindowKey = 0;
        }
    }

    g_topState.activeProfile = activeProfile;
    g_topState.windowProfiles = std::move(loadedWindowProfiles);
    g_topState.profiles = std::move(loadedTopProfiles);
    g_bottomWindowWeights = std::move(loadedBottomWeights);
    g_bottomWindowOrder = std::move(loadedBottomOrder);
    g_bottomFullscreenWindowKeys = std::move(loadedBottomFullscreenWindowKeys);
}

void savePersistentState() {
    const auto path = persistentStatePath();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);

    const auto tempPath = path.string() + ".tmp";
    std::ofstream file{tempPath, std::ios::trunc};
    if (!file)
        return;

    file << "version 3\n";
    file << "hyprland_pid " << getpid() << "\n";
    file << "active " << g_topState.activeProfile << "\n";

    std::unordered_set<std::uintptr_t> writtenBottomOrder;
    for (const auto key : g_bottomWindowOrder) {
        if (key == 0 || writtenBottomOrder.contains(key))
            continue;

        if (!windowFromKey(key))
            continue;

        if (g_topState.windowKeys.contains(key))
            continue;

        file << "bottom " << std::hex << key << std::dec << "\n";
        writtenBottomOrder.insert(key);
    }

    for (int profile = 1; profile <= PROFILE_COUNT; ++profile) {
        const auto profileIt = g_topState.profiles.find(profile);
        if (profileIt == g_topState.profiles.end())
            continue;

        const auto& profileData = profileIt->second;
        std::unordered_set<std::uintptr_t> written;

        for (const auto key : profileData.order) {
            if (key == 0 || written.contains(key))
                continue;

            const auto windowProfileIt = g_topState.windowProfiles.find(key);
            if (windowProfileIt == g_topState.windowProfiles.end() || windowProfileIt->second != profile)
                continue;

            file << "top " << profile << " " << std::hex << key << std::dec << "\n";

            const auto weightIt = profileData.weights.find(key);
            if (weightIt != profileData.weights.end() && std::isfinite(weightIt->second) && weightIt->second > 0.0)
                file << "top_weight " << profile << " " << std::hex << key << std::dec << " " << weightIt->second << "\n";

            written.insert(key);
        }

        const auto fullscreenKey = profileData.fullscreenWindowKey;
        if (fullscreenKey != 0 && written.contains(fullscreenKey))
            file << "top_fullscreen " << profile << " " << std::hex << fullscreenKey << std::dec << "\n";
    }

    for (const auto& [key, weight] : g_bottomWindowWeights) {
        if (key == 0 || !std::isfinite(weight) || weight <= 0.0)
            continue;

        if (!windowFromKey(key))
            continue;

        file << "bottom_weight " << std::hex << key << std::dec << " " << weight << "\n";
    }

    for (const auto key : g_bottomFullscreenWindowKeys) {
        if (key == 0)
            continue;

        if (!windowFromKey(key))
            continue;

        if (g_topState.windowKeys.contains(key))
            continue;

        file << "bottom_fullscreen " << std::hex << key << std::dec << "\n";
    }

    file.close();
    if (!file) {
        std::filesystem::remove(tempPath, error);
        return;
    }

    std::filesystem::rename(tempPath, path, error);
    if (error) {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(tempPath, path, error);
    }

    if (error)
        std::filesystem::remove(tempPath, error);
}

bool restoreTopStateFromPersistence(const SP<Layout::ITarget>& target) {
    if (!target || !target->window() || windowIsClosingOrDead(target->window()))
        return false;

    const auto key = windowKey(target->window());
    if (key == 0 || g_topState.windowKeys.contains(key))
        return false;

    const auto profileIt = g_topState.windowProfiles.find(key);
    if (profileIt == g_topState.windowProfiles.end() || !validProfile(profileIt->second))
        return false;

    g_topState.windowKeys.insert(key);
    g_topState.targets[key] = target;

    auto& profileData = g_topState.profiles[profileIt->second];
    auto& order = profileData.order;
    if (std::ranges::find(order, key) == order.end())
        order.push_back(key);

    if (!profileData.weights.contains(key))
        profileData.weights[key] = 1.0;

    target->window()->m_pinned = true;
    markInactiveTopProfilesDirty();
    return true;
}

bool windowIsClosingOrDead(const PHLWINDOW& window) {
    return !window || !window->m_isMapped || window->m_fadingOut || window->m_readyToDelete;
}

bool clearTopWindowState(const PHLWINDOW& window) {
    const auto key = windowKey(window);

    if (key == 0)
        return false;

    bool changed = false;
    changed = g_topState.windowKeys.erase(key) > 0 || changed;
    changed = g_topState.windowProfiles.erase(key) > 0 || changed;
    changed = g_topState.targets.erase(key) > 0 || changed;

    for (auto& [profile, profileData] : g_topState.profiles) {
        const auto oldSize = profileData.order.size();
        std::erase(profileData.order, key);
        changed = profileData.order.size() != oldSize || changed;
        changed = profileData.weights.erase(key) > 0 || changed;
        changed = profileData.lastBoxes.erase(key) > 0 || changed;
        if (profileData.fullscreenWindowKey == key) {
            profileData.fullscreenWindowKey = 0;
            changed = true;
        }
    }

    if (window) {
        g_topProfileTransition.fadingOutKeys.erase(key);
        g_topProfileTransition.fadingInKeys.erase(key);
        resetProfileFadeAlpha(window);
        setProfileFadeInputBlocked(window, false);
        setTopProfileHiddenState(window, false);

        if (window->m_pinned) {
            window->m_pinned = false;
            changed = true;
        }
    }

    if (changed)
        markInactiveTopProfilesDirty();

    return changed;
}


void setTopProfileHiddenState(const PHLWINDOW& window, bool hidden) {
    if (!window)
        return;

    const auto key = windowKey(window);

    if (hidden) {
        if (key != 0 && g_topProfileTransition.fadingOutKeys.contains(key))
            return;

        if (key != 0 && g_topState.hiddenWindowKeys.contains(key))
            return;

        window->setHidden(true);

        if (key != 0)
            g_topState.hiddenWindowKeys.insert(key);

        return;
    }

    if (key != 0 && !g_topState.hiddenWindowKeys.erase(key))
        return;

    window->setHidden(false);
}


void notify(const std::string& text, const CHyprColor& color = CHyprColor{0.2F, 0.8F, 1.0F, 1.0F}) {
    if (g_pluginHandle)
        HyprlandAPI::addNotification(g_pluginHandle, text, color, 2500);
}

std::uintptr_t windowKey(const PHLWINDOW& window) {
    return window ? reinterpret_cast<std::uintptr_t>(window.get()) : 0;
}

PHLWINDOW windowFromKey(std::uintptr_t key) {
    if (key == 0 || !g_pCompositor)
        return nullptr;

    for (const auto& window : g_pCompositor->m_windows) {
        if (windowKey(window) == key)
            return window;
    }

    return nullptr;
}

void refreshWindowDecorations(const PHLWINDOW& window) {
    if (!window)
        return;

    window->m_borderSizeCacheDirty = true;
    window->updateWindowData();
    window->updateWindowDecos();
    window->updateDecorationValues();
}

void setRowFullscreenVisualState(const PHLWINDOW& window, bool enabled) {
    if (!window || !window->m_ruleApplicator)
        return;

    using Desktop::Types::PRIORITY_LAYOUT;

    if (enabled) {
        window->m_ruleApplicator->borderSize().set(0, PRIORITY_LAYOUT);
        window->m_ruleApplicator->rounding().set(0, PRIORITY_LAYOUT);
        window->m_ruleApplicator->decorate().set(false, PRIORITY_LAYOUT);
        window->m_ruleApplicator->noShadow().set(true, PRIORITY_LAYOUT);
    } else {
        window->m_ruleApplicator->borderSize().unset(PRIORITY_LAYOUT);
        window->m_ruleApplicator->rounding().unset(PRIORITY_LAYOUT);
        window->m_ruleApplicator->decorate().unset(PRIORITY_LAYOUT);
        window->m_ruleApplicator->noShadow().unset(PRIORITY_LAYOUT);
    }

    refreshWindowDecorations(window);
}

bool clearTopFullscreenForWindow(std::uintptr_t key, bool restoreVisuals) {
    if (key == 0)
        return false;

    bool changed = false;

    for (auto& [profile, profileData] : g_topState.profiles) {
        if (profileData.fullscreenWindowKey != key)
            continue;

        if (restoreVisuals)
            setRowFullscreenVisualState(windowFromKey(key), false);

        profileData.fullscreenWindowKey = 0;
        changed = true;
    }

    if (changed) {
        markInactiveTopProfilesDirty();
        savePersistentState();
    }

    return changed;
}

void clearAllTopFullscreenState(bool restoreVisuals) {
    for (auto& [profile, profileData] : g_topState.profiles) {
        if (profileData.fullscreenWindowKey == 0)
            continue;

        if (restoreVisuals)
            setRowFullscreenVisualState(windowFromKey(profileData.fullscreenWindowKey), false);

        profileData.fullscreenWindowKey = 0;
    }
}

SP<Layout::ITarget> topProfileFocusCandidateAfterRemoving(std::uintptr_t key) {
    if (key == 0 || !g_topState.windowKeys.contains(key))
        return nullptr;

    const auto profileIt = g_topState.windowProfiles.find(key);
    if (profileIt == g_topState.windowProfiles.end() || !validProfile(profileIt->second))
        return nullptr;

    const auto profileDataIt = g_topState.profiles.find(profileIt->second);
    if (profileDataIt == g_topState.profiles.end())
        return nullptr;

    const auto& order = profileDataIt->second.order;
    const auto closedIt = std::ranges::find(order, key);
    if (closedIt == order.end())
        return nullptr;

    auto usableTarget = [](std::uintptr_t candidateKey) -> SP<Layout::ITarget> {
        if (candidateKey == 0)
            return nullptr;

        const auto targetIt = g_topState.targets.find(candidateKey);
        if (targetIt == g_topState.targets.end() || !targetIt->second || !targetIt->second->window())
            return nullptr;

        if (windowIsClosingOrDead(targetIt->second->window()))
            return nullptr;

        return targetIt->second;
    };

    // Prefer the left neighbour. If the closing window is leftmost, use the
    // window that will shift into its slot. Only consider windows in the same
    // top profile, so focus does not escape to the bottom row while siblings
    // remain in the active profile.
    for (auto it = closedIt; it != order.begin();) {
        --it;
        if (auto target = usableTarget(*it))
            return target;
    }

    for (auto it = std::next(closedIt); it != order.end(); ++it) {
        if (auto target = usableTarget(*it))
            return target;
    }

    return nullptr;
}

bool focusWindowByKey(std::uintptr_t key) {
    const auto window = windowFromKey(key);

    if (!window || windowIsClosingOrDead(window))
        return false;

    setTopProfileHiddenState(window, false);

    // Keep this inside Hyprland instead of shelling out to hyprctl. Hyprland
    // 0.55+ exposes focus through Desktop::focusState(); the old compositor
    // helper is not available to plugins through the hyprpm headers, and the
    // hyprctl dispatcher syntax changed with the Lua config migration.
    Desktop::focusState()->fullWindowFocus(window, Desktop::FOCUS_REASON_UNMAP_WINDOW_TILING);

    updateSpawnIntentFromFocusedWindow(window);
    return true;
}

bool moveKeyOneSlot(std::vector<std::uintptr_t>& order, std::uintptr_t key, int delta) {
    if (delta == 0)
        return false;

    const auto it = std::ranges::find(order, key);

    if (it == order.end())
        return false;

    if (delta < 0) {
        if (it == order.begin())
            return false;

        std::swap(*it, *std::prev(it));
        return true;
    }

    const auto next = std::next(it);

    if (next == order.end())
        return false;

    std::swap(*it, *next);
    return true;
}

template <typename IsUsable>
std::optional<std::uintptr_t> neighbourKeyAfterRemoving(
    const std::vector<std::uintptr_t>& order,
    std::uintptr_t key,
    IsUsable usable
) {
    if (key == 0)
        return std::nullopt;

    const auto removedIt = std::ranges::find(order, key);
    if (removedIt == order.end())
        return std::nullopt;

    for (auto it = removedIt; it != order.begin();) {
        --it;
        if (*it != key && usable(*it))
            return *it;
    }

    for (auto it = std::next(removedIt); it != order.end(); ++it) {
        if (*it != key && usable(*it))
            return *it;
    }

    return std::nullopt;
}

void saveBottomWindowOrder(const std::vector<std::uintptr_t>& order) {
    if (order.empty())
        return;

    std::unordered_set<std::uintptr_t> orderKeys;
    for (const auto key : order) {
        if (key != 0)
            orderKeys.insert(key);
    }

    if (orderKeys.empty())
        return;

    std::erase_if(g_bottomWindowOrder, [&](std::uintptr_t key) {
        return key == 0 || orderKeys.contains(key) || g_topState.windowKeys.contains(key);
    });

    std::unordered_set<std::uintptr_t> written;
    std::vector<std::uintptr_t> merged;
    merged.reserve(order.size() + g_bottomWindowOrder.size());

    for (const auto key : order) {
        if (key == 0 || written.contains(key) || g_topState.windowKeys.contains(key))
            continue;

        merged.push_back(key);
        written.insert(key);
    }

    for (const auto key : g_bottomWindowOrder) {
        if (key == 0 || written.contains(key) || g_topState.windowKeys.contains(key))
            continue;

        merged.push_back(key);
        written.insert(key);
    }

    g_bottomWindowOrder = std::move(merged);
    savePersistentState();
}

void removeBottomWindowFromPersistentState(std::uintptr_t key) {
    if (key == 0)
        return;

    bool changed = false;
    const auto oldOrderSize = g_bottomWindowOrder.size();
    std::erase(g_bottomWindowOrder, key);
    changed = changed || oldOrderSize != g_bottomWindowOrder.size();

    changed = g_bottomWindowWeights.erase(key) > 0 || changed;
    changed = g_bottomFullscreenWindowKeys.erase(key) > 0 || changed;

    if (changed)
        savePersistentState();
}

bool validProfile(int profile) {
    return profile >= 1 && profile <= PROFILE_COUNT;
}

bool moveTopWindowInOrder(const PHLWINDOW& window, int delta) {
    const auto key = windowKey(window);

    if (key == 0 || !g_topState.windowKeys.contains(key))
        return false;

    const auto profileIt = g_topState.windowProfiles.find(key);
    if (profileIt == g_topState.windowProfiles.end())
        return false;

    auto& profileData = g_topState.profiles[profileIt->second];
    if (profileData.fullscreenWindowKey != 0)
        return false;

    const bool moved = moveKeyOneSlot(profileData.order, key, delta);
    if (moved) {
        markInactiveTopProfilesDirty();
        savePersistentState();
    }

    return moved;
}

std::uintptr_t focusCandidateKeyForTopProfile(int profile) {
    if (!validProfile(profile))
        return 0;

    const auto profileIt = g_topState.profiles.find(profile);
    if (profileIt == g_topState.profiles.end())
        return 0;

    const auto& profileData = profileIt->second;

    auto usableKey = [](std::uintptr_t key) -> std::uintptr_t {
        if (key == 0)
            return 0;

        const auto targetIt = g_topState.targets.find(key);
        if (targetIt == g_topState.targets.end() || !targetIt->second || !targetIt->second->window())
            return 0;

        if (windowIsClosingOrDead(targetIt->second->window()))
            return 0;

        return key;
    };

    if (const auto fullscreenKey = usableKey(profileData.fullscreenWindowKey); fullscreenKey != 0)
        return fullscreenKey;

    for (const auto key : profileData.order) {
        if (const auto candidateKey = usableKey(key); candidateKey != 0)
            return candidateKey;
    }

    return 0;
}

std::vector<std::uintptr_t> liveWindowKeysForTopProfile(int profile) {
    std::vector<std::uintptr_t> keys;

    if (!validProfile(profile))
        return keys;

    const auto profileIt = g_topState.profiles.find(profile);
    if (profileIt == g_topState.profiles.end())
        return keys;

    const auto addIfUsable = [&](std::uintptr_t key) {
        if (key == 0 || std::ranges::find(keys, key) != keys.end())
            return;

        const auto targetIt = g_topState.targets.find(key);
        if (targetIt == g_topState.targets.end() || !targetIt->second || !targetIt->second->window())
            return;

        if (windowIsClosingOrDead(targetIt->second->window()))
            return;

        keys.push_back(key);
    };

    addIfUsable(profileIt->second.fullscreenWindowKey);

    for (const auto key : profileIt->second.order)
        addIfUsable(key);

    return keys;
}

void resetProfileFadeAlpha(const PHLWINDOW& window) {
    if (!window)
        return;

    window->alpha(Desktop::View::WINDOW_ALPHA_LAYOUT)->setCallbackOnEnd(nullptr);
    window->alpha(Desktop::View::WINDOW_ALPHA_LAYOUT)->setValueAndWarp(1.F);
}

void setProfileFadeInputBlocked(const PHLWINDOW& window, bool blocked) {
    if (!window)
        return;

    window->setInputBlocked(Desktop::View::INPUT_BLOCK_GROUP_INACTIVE, blocked);
}

void cancelTopProfileTransition() {
    for (const auto key : g_topProfileTransition.fadingOutKeys) {
        const auto window = windowFromKey(key);
        if (!window)
            continue;

        resetProfileFadeAlpha(window);
        setProfileFadeInputBlocked(window, false);
    }

    for (const auto key : g_topProfileTransition.fadingInKeys) {
        const auto window = windowFromKey(key);
        if (!window)
            continue;

        resetProfileFadeAlpha(window);
        setProfileFadeInputBlocked(window, false);
    }

    g_topProfileTransition.fadingOutKeys.clear();
    g_topProfileTransition.fadingInKeys.clear();
}

void startTopProfileFade(int fromProfile, int toProfile) {
    if (!PROFILE_FADE_ENABLED || fromProfile == toProfile)
        return;

    cancelTopProfileTransition();

    for (const auto key : liveWindowKeysForTopProfile(toProfile)) {
        const auto window = windowFromKey(key);
        if (!window)
            continue;

        g_topProfileTransition.fadingInKeys.insert(key);
        window->alpha(Desktop::View::WINDOW_ALPHA_LAYOUT)->setCallbackOnEnd(nullptr);
        window->alpha(Desktop::View::WINDOW_ALPHA_LAYOUT)->setValueAndWarp(PROFILE_FADE_IN_START_ALPHA);
        setTopProfileHiddenState(window, false);
        *window->alpha(Desktop::View::WINDOW_ALPHA_LAYOUT) = 1.F;
        window->alpha(Desktop::View::WINDOW_ALPHA_LAYOUT)->setCallbackOnEnd([key](auto) {
            const auto window = windowFromKey(key);
            if (window)
                resetProfileFadeAlpha(window);

            g_topProfileTransition.fadingInKeys.erase(key);
        });
    }

    for (const auto key : liveWindowKeysForTopProfile(fromProfile)) {
        const auto window = windowFromKey(key);
        if (!window)
            continue;

        g_topProfileTransition.fadingOutKeys.insert(key);
        setProfileFadeInputBlocked(window, true);
        window->alpha(Desktop::View::WINDOW_ALPHA_LAYOUT)->setCallbackOnEnd(nullptr);
        window->alpha(Desktop::View::WINDOW_ALPHA_LAYOUT)->setValueAndWarp(1.F);
        *window->alpha(Desktop::View::WINDOW_ALPHA_LAYOUT) = PROFILE_FADE_OUT_END_ALPHA;
        window->alpha(Desktop::View::WINDOW_ALPHA_LAYOUT)->setCallbackOnEnd([key](auto) {
            const auto window = windowFromKey(key);
            if (!window)
                return;

            resetProfileFadeAlpha(window);
            setProfileFadeInputBlocked(window, false);
            g_topProfileTransition.fadingOutKeys.erase(key);

            const auto profileIt = g_topState.windowProfiles.find(key);
            if (profileIt != g_topState.windowProfiles.end() && profileIt->second != g_topState.activeProfile)
                setTopProfileHiddenState(window, true);
        });
    }
}

void showTopProfile(int profile) {
    if (!validProfile(profile))
        return;

    setSpawnIntent(EFocusedSplitRow::Top, ESpawnIntentSource::TopProfileSwitch);

    const auto fromProfile = g_topState.activeProfile;
    const auto focusCandidateKey = focusCandidateKeyForTopProfile(profile);

    if (fromProfile != profile) {
        startTopProfileFade(fromProfile, profile);
        g_topState.activeProfile = profile;
        markInactiveTopProfilesDirty();
        savePersistentState();
        recalculateAllInstances();
    }

    if (focusCandidateKey != 0)
        focusWindowByKey(focusCandidateKey);
}

PHLWINDOW activeWindow() {
    if (!g_pCompositor)
        return nullptr;

    for (const auto& window : g_pCompositor->m_windows) {
        if (window && window->m_isMapped && g_pCompositor->isWindowActive(window))
            return window;
    }

    return nullptr;
}

void setSpawnIntent(EFocusedSplitRow row, ESpawnIntentSource source) {
    g_spawnIntent.row = row;
    g_spawnIntent.source = source;

    const auto focused = activeWindow();
    g_spawnIntent.focusedWindowKeyWhenSourceSet = focused ? windowKey(focused) : 0;
}

void updateSpawnIntentFromFocusedWindow(const PHLWINDOW& window) {
    if (!window)
        return;

    const auto key = windowKey(window);
    if (key == 0)
        return;

    g_spawnIntent.row = g_topState.windowKeys.contains(key) ? EFocusedSplitRow::Top : EFocusedSplitRow::Bottom;
    g_spawnIntent.source = ESpawnIntentSource::FocusedWindow;
    g_spawnIntent.focusedWindowKeyWhenSourceSet = key;
}



std::optional<EFocusedSplitRow> rowFromCursorForArea(const CBox& area) {
    if (!g_pInputManager || area.w <= 0.0 || area.h <= 0.0)
        return std::nullopt;

    const auto cursor = g_pInputManager->getMouseCoordsInternal();

    if (cursor.x < area.x || cursor.x >= area.x + area.w || cursor.y < area.y || cursor.y >= area.y + area.h)
        return std::nullopt;

    const double topHeight = std::floor(area.h * g_topRowRatio);
    return cursor.y < area.y + topHeight ? EFocusedSplitRow::Top : EFocusedSplitRow::Bottom;
}

SDispatchResult setTopRowRatio(double ratio) {
    if (!std::isfinite(ratio))
        return {.success = false, .error = "splitrow: invalid top row ratio"};

    if (ratio > 1.0)
        ratio /= 100.0;

    const double clamped = std::clamp(ratio, MIN_TOP_ROW_RATIO, MAX_TOP_ROW_RATIO);

    if (std::abs(g_topRowRatio - clamped) < 0.0001)
        return {.success = true, .error = ""};

    g_topRowRatio = clamped;
    markInactiveTopProfilesDirty();
    recalculateAllInstances();
    return {.success = true, .error = ""};
}


class CSplitRowAlgorithm final : public Layout::ITiledAlgorithm {
  public:
    CSplitRowAlgorithm() {
        g_instances.push_back(this);
    }

    ~CSplitRowAlgorithm() override {
        clearBottomFullscreenState(true);
        std::erase(g_instances, this);
    }

    std::optional<std::string> layoutName() const override {
        return std::string{TILED_ALGO_NAME};
    }

    std::optional<EFocusedSplitRow> rowFromCursorForCurrentSpace() const {
        const auto parent = m_parent.lock();

        if (!parent)
            return std::nullopt;

        const auto space = parent->space();

        if (!space)
            return std::nullopt;

        CBox cursorArea = space->workArea(false);
        if (const auto workspace = space->workspace(); workspace && workspace->m_monitor)
            cursorArea = workspace->m_monitor->logicalBoxMinusReserved();

        return rowFromCursorForArea(cursorArea);
    }

    void newTarget(SP<Layout::ITarget> target) override {
        // newTarget() must not infer intent from Hyprland's active window.
        // On blank workspace switches the active window can still be a sticky
        // top-row window from the previous workspace. Spawn placement should
        // apply the latest explicit intent recorded by real events. When the
        // last intent came from focus, mouse position can refine the target
        // row at spawn time, which helps empty workspaces feel like real row
        // regions without continuously tracking pointer motion.
        auto spawnRow = g_spawnIntent.row;
        const auto focusedInsertKey = g_spawnIntent.source == ESpawnIntentSource::FocusedWindow
            ? g_spawnIntent.focusedWindowKeyWhenSourceSet
            : 0;

        if (m_spawnFollowsFocusReady && g_spawnIntent.source == ESpawnIntentSource::FocusedWindow) {
            if (const auto cursorRow = rowFromCursorForCurrentSpace())
                spawnRow = *cursorRow;
        }

        const bool shouldSpawnIntoTopProfile = m_spawnFollowsFocusReady
            && spawnRow == EFocusedSplitRow::Top;

        const bool restoredTopState = addTarget(target);

        if (!restoredTopState && shouldSpawnIntoTopProfile && target && target->window()) {
            setWindowTopProfile(target->window(), g_topState.activeProfile, focusedInsertKey);
            return;
        }

        if (!restoredTopState && target && target->window())
            insertBottomTargetAfterKey(target, focusedInsertKey);

        recalculate(Layout::RECALCULATE_REASON_UNKNOWN);
    }

    void movedTarget(SP<Layout::ITarget> target, std::optional<Vector2D> focalPoint = std::nullopt) override {
        (void)focalPoint;
        addTarget(target);
        recalculate(Layout::RECALCULATE_REASON_UNKNOWN);
    }

    void removeTarget(SP<Layout::ITarget> target) override {
        if (target && target->window()) {
            const auto key = windowKey(target->window());
            const bool closingOrDead = windowIsClosingOrDead(target->window());

            if (key != 0 && key == m_bottomFullscreen.windowKey && closingOrDead)
                clearBottomFullscreenState(false);

            // removeTarget is also called when a pinned top-row window is
            // detached from a workspace during workspace changes. In that
            // case the window is still mapped and must remain in the sticky
            // top-row state. Only clear the global top-row state when the
            // window is actually no longer mapped.
            if (key != 0 && g_topState.windowKeys.contains(key)) {
                if (closingOrDead) {
                    if (clearTopWindowState(target->window()))
                        savePersistentState();
                }
            }
        }

        if (target && target->window()) {
            const auto key = windowKey(target->window());
            if (key != 0 && !g_topState.windowKeys.contains(key)) {
                m_bottomOrderBeforeLastRemove = bottomWindowKeysInOrder();
                removeBottomWindowFromPersistentState(key);
            }
        }

        std::erase_if(m_targets, [&](const auto& other) { return other == target; });
        saveBottomWindowOrder(bottomWindowKeysInOrder());
        recalculateAllInstances();
    }

    void resizeTarget(const Vector2D& delta, SP<Layout::ITarget> target, Layout::eRectCorner corner = Layout::CORNER_NONE) override {
        (void)corner;

        if (!target || !target->window() || std::abs(delta.x) < 1.0)
            return;

        resizeWindowInRow(target->window(), delta.x > 0.0 ? 1 : -1);
    }

    void recalculate(Layout::eRecalculateReason reason = Layout::RECALCULATE_REASON_UNKNOWN) override {
        (void)reason;
        placeTargetsInRows();
    }

    SP<Layout::ITarget> getNextCandidate(SP<Layout::ITarget> old) override {
        if (old && old->window()) {
            const auto key = windowKey(old->window());

            if (auto topCandidate = topProfileFocusCandidateAfterRemoving(key))
                return topCandidate;

            if (auto bottomCandidate = bottomFocusCandidateAfterRemoving(key))
                return bottomCandidate;
        }

        if (m_targets.empty())
            return nullptr;

        for (const auto& target : m_targets) {
            if (target && target != old)
                return target;
        }

        return m_targets.front();
    }

    void swapTargets(SP<Layout::ITarget> a, SP<Layout::ITarget> b) override {
        if (!a || !b || a == b)
            return;

        auto ia = std::ranges::find(m_targets, a);
        auto ib = std::ranges::find(m_targets, b);

        if (ia == m_targets.end() || ib == m_targets.end())
            return;

        std::swap(*ia, *ib);
        saveBottomWindowOrder(bottomWindowKeysInOrder());
        recalculate(Layout::RECALCULATE_REASON_UNKNOWN);
    }

    void moveTargetInDirection(SP<Layout::ITarget> target, Math::eDirection direction, bool silent) override {
        (void)silent;

        if (!target || !target->window())
            return;

        int delta = 0;

        if (direction == Math::DIRECTION_LEFT)
            delta = -1;
        else if (direction == Math::DIRECTION_RIGHT)
            delta = 1;
        else
            return;

        const auto key = windowKey(target->window());

        if (key != 0 && g_topState.windowKeys.contains(key)) {
            if (moveTopWindowInOrder(target->window(), delta)) {
                recalculateAllInstances();
            }

            return;
        }

        moveWindowInBottomRow(target->window(), delta);
    }

    bool setWindowTopProfile(const PHLWINDOW& window, int profile, std::uintptr_t insertAfterKey = 0) {
        if (!window || !validProfile(profile))
            return false;

        const auto target = targetForWindow(window);
        if (!target)
            return false;

        const auto key = windowKey(window);
        if (key == 0)
            return false;

        if (m_bottomFullscreen.windowKey == key)
            clearBottomFullscreenState(true);

        clearTopFullscreenForWindow(key, true);

        const bool wasTop = g_topState.windowKeys.contains(key);
        const int oldProfile = g_topState.windowProfiles.contains(key) ? g_topState.windowProfiles[key] : 0;

        if (wasTop && oldProfile != profile)
            std::erase(g_topState.profiles[oldProfile].order, key);

        g_topState.windowKeys.insert(key);
        g_topState.windowProfiles[key] = profile;
        g_topState.targets[key] = target;
        g_bottomWindowWeights.erase(key);
        g_bottomFullscreenWindowKeys.erase(key);
        std::erase(g_bottomWindowOrder, key);

        auto& profileData = g_topState.profiles[profile];
        auto& order = profileData.order;
        if (std::ranges::find(order, key) == order.end()) {
            const auto insertAfterIt = std::ranges::find(order, insertAfterKey);
            if (insertAfterKey != 0 && insertAfterIt != order.end())
                order.insert(std::next(insertAfterIt), key);
            else
                order.push_back(key);
        }

        if (!profileData.weights.contains(key))
            profileData.weights[key] = 1.0;

        window->m_pinned = true;
        markInactiveTopProfilesDirty();

        savePersistentState();
        recalculateAllInstances();
        return true;
    }

    bool setWindowTopState(const PHLWINDOW& window, bool top) {
        if (top)
            return setWindowTopProfile(window, g_topState.activeProfile);

        if (!window)
            return false;

        const auto key = windowKey(window);
        if (key == 0)
            return false;

        clearTopFullscreenForWindow(key, true);

        g_topState.windowKeys.erase(key);
        g_topState.windowProfiles.erase(key);
        g_topState.targets.erase(key);
        for (auto& [profile, profileData] : g_topState.profiles) {
            std::erase(profileData.order, key);
            profileData.weights.erase(key);
            profileData.lastBoxes.erase(key);
        }
        if (!g_bottomWindowWeights.contains(key))
            g_bottomWindowWeights[key] = 1.0;

        saveBottomWindowOrder(bottomWindowKeysInOrder());

        setTopProfileHiddenState(window, false);
        window->m_pinned = false;
        markInactiveTopProfilesDirty();

        savePersistentState();
        recalculateAllInstances();
        return true;
    }

    bool toggleWindowTopState(const PHLWINDOW& window) {
        if (!window)
            return false;

        const auto key = windowKey(window);
        if (key == 0)
            return false;

        const bool currentlyTop = g_topState.windowKeys.contains(key);
        return setWindowTopState(window, !currentlyTop);
    }

    bool toggleBottomFullscreen(const PHLWINDOW& window) {
        if (!window)
            return false;

        const auto key = windowKey(window);
        if (key == 0 || g_topState.windowKeys.contains(key))
            return false;

        if (!targetForWindow(window))
            return false;

        if (m_bottomFullscreen.windowKey == key) {
            clearBottomFullscreenState(true);
        } else {
            clearBottomFullscreenState(true);
            m_bottomFullscreen.windowKey = key;
            g_bottomFullscreenWindowKeys.insert(key);
            setRowFullscreenVisualState(window, true);
        }

        savePersistentState();
        recalculateAllInstances();
        return true;
    }

    bool toggleTopFullscreen(const PHLWINDOW& window) {
        if (!window)
            return false;

        const auto key = windowKey(window);
        if (key == 0 || !g_topState.windowKeys.contains(key))
            return false;

        const auto profileIt = g_topState.windowProfiles.find(key);
        if (profileIt == g_topState.windowProfiles.end() || !validProfile(profileIt->second))
            return false;

        auto& profileData = g_topState.profiles[profileIt->second];

        if (profileData.fullscreenWindowKey == key) {
            setRowFullscreenVisualState(window, false);
            profileData.fullscreenWindowKey = 0;
        } else {
            clearTopFullscreenForWindow(profileData.fullscreenWindowKey, true);
            profileData.fullscreenWindowKey = key;
            setRowFullscreenVisualState(window, true);
        }

        markInactiveTopProfilesDirty();
        savePersistentState();
        recalculateAllInstances();
        return true;
    }

    bool moveWindowInBottomRow(const PHLWINDOW& window, int delta) {
        if (!window || delta == 0)
            return false;

        if (m_bottomFullscreen.windowKey != 0)
            return false;

        const auto key = windowKey(window);

        if (key == 0 || g_topState.windowKeys.contains(key))
            return false;

        std::vector<std::size_t> bottomPositions;

        for (std::size_t i = 0; i < m_targets.size(); ++i) {
            const auto& target = m_targets[i];
            const auto targetKey = windowKey(target ? target->window() : nullptr);

            if (targetKey != 0 && !g_topState.windowKeys.contains(targetKey))
                bottomPositions.push_back(i);
        }

        for (std::size_t rowIndex = 0; rowIndex < bottomPositions.size(); ++rowIndex) {
            const auto actualIndex = bottomPositions[rowIndex];
            const auto& target = m_targets[actualIndex];

            if (!target || target->window() != window)
                continue;

            if (delta < 0) {
                if (rowIndex == 0)
                    return false;

                std::swap(m_targets[actualIndex], m_targets[bottomPositions[rowIndex - 1]]);
                saveBottomWindowOrder(bottomWindowKeysInOrder());
                recalculate(Layout::RECALCULATE_REASON_UNKNOWN);
                return true;
            }

            if (rowIndex + 1 >= bottomPositions.size())
                return false;

            std::swap(m_targets[actualIndex], m_targets[bottomPositions[rowIndex + 1]]);
            saveBottomWindowOrder(bottomWindowKeysInOrder());
            recalculate(Layout::RECALCULATE_REASON_UNKNOWN);
            return true;
        }

        return false;
    }

    bool resizeWindowInRow(const PHLWINDOW& window, int delta) {
        if (!window || delta == 0)
            return false;

        const auto key = windowKey(window);
        if (key == 0)
            return false;

        if (g_topState.windowKeys.contains(key)) {
            const auto profileIt = g_topState.windowProfiles.find(key);
            if (profileIt == g_topState.windowProfiles.end())
                return false;

            auto& profileData = g_topState.profiles[profileIt->second];
            if (profileData.fullscreenWindowKey != 0)
                return false;

            const bool initializedWeights = ensureWeightsForOrder(profileData.order, profileData.weights);
            const bool changed = resizeKeyInOrder(profileData.order, key, delta, profileData.weights);
            if (changed || initializedWeights) {
                markInactiveTopProfilesDirty();
                savePersistentState();
                recalculateAllInstances();
            }

            return changed;
        }

        if (m_bottomFullscreen.windowKey != 0)
            return false;

        const auto bottomKeys = bottomWindowKeysInOrder();
        const bool initializedWeights = ensureWeightsForOrder(bottomKeys, g_bottomWindowWeights);
        const bool changed = resizeKeyInOrder(bottomKeys, key, delta, g_bottomWindowWeights);
        if (changed || initializedWeights) {
            savePersistentState();
            recalculate(Layout::RECALCULATE_REASON_UNKNOWN);
        }

        return changed;
    }

    bool containsWindow(const PHLWINDOW& window) const {
        return targetForWindow(window) != nullptr;
    }

    bool hasBottomFullscreen() const {
        return m_bottomFullscreen.windowKey != 0;
    }

    bool clearBottomFullscreenForWindow(std::uintptr_t key, bool restoreVisuals) {
        if (key == 0 || key != m_bottomFullscreen.windowKey)
            return false;

        clearBottomFullscreenState(restoreVisuals);
        return true;
    }

    SP<Layout::ITarget> bottomFocusCandidateAfterRemoving(std::uintptr_t key) const {
        if (key == 0 || g_topState.windowKeys.contains(key))
            return nullptr;

        const auto targetForKey = [&](std::uintptr_t candidateKey) -> SP<Layout::ITarget> {
            if (candidateKey == 0 || candidateKey == key || g_topState.windowKeys.contains(candidateKey))
                return nullptr;

            for (const auto& target : m_targets) {
                if (!target || !target->window())
                    continue;

                if (windowKey(target->window()) != candidateKey)
                    continue;

                if (windowIsClosingOrDead(target->window()))
                    return nullptr;

                return target;
            }

            return nullptr;
        };

        const auto candidateFromOrder = [&](const std::vector<std::uintptr_t>& order) -> SP<Layout::ITarget> {
            const auto candidateKey = neighbourKeyAfterRemoving(
                order,
                key,
                [&](std::uintptr_t candidateKey) { return targetForKey(candidateKey) != nullptr; }
            );

            return candidateKey ? targetForKey(*candidateKey) : nullptr;
        };

        if (auto target = candidateFromOrder(bottomWindowKeysInOrder()))
            return target;

        if (auto target = candidateFromOrder(m_bottomOrderBeforeLastRemove))
            return target;

        return nullptr;
    }

    void restoreBottomFullscreenFromBind() {
        clearBottomFullscreenState(true);
        savePersistentState();
        m_bottomFullscreen.forceRestoreSpaceUpdate = true;
        recalculateAllInstances();
    }

    void finishRecalculate() {
        m_bottomFullscreen.forceRestoreSpaceUpdate = false;
    }

    void clearBottomFullscreenOnExit() {
        clearBottomFullscreenState(true);
    }

  private:
    std::vector<SP<Layout::ITarget>> m_targets;
    std::vector<std::uintptr_t> m_bottomOrderBeforeLastRemove;
    SBottomFullscreenState m_bottomFullscreen;
    bool m_spawnFollowsFocusReady = false;

    std::vector<std::uintptr_t> bottomWindowKeysInOrder() const {
        std::vector<std::uintptr_t> keys;

        for (const auto& target : m_targets) {
            const auto key = windowKey(target ? target->window() : nullptr);
            if (key != 0 && !g_topState.windowKeys.contains(key))
                keys.push_back(key);
        }

        return keys;
    }

    void applyPersistentBottomOrder() {
        if (g_bottomWindowOrder.empty() || m_targets.size() < 2)
            return;

        std::stable_sort(m_targets.begin(), m_targets.end(), [](const auto& left, const auto& right) {
            const auto leftKey = windowKey(left ? left->window() : nullptr);
            const auto rightKey = windowKey(right ? right->window() : nullptr);
            const bool leftIsBottom = leftKey != 0 && !g_topState.windowKeys.contains(leftKey);
            const bool rightIsBottom = rightKey != 0 && !g_topState.windowKeys.contains(rightKey);

            if (leftIsBottom != rightIsBottom)
                return !leftIsBottom;

            if (!leftIsBottom || !rightIsBottom)
                return false;

            const auto leftIt = std::ranges::find(g_bottomWindowOrder, leftKey);
            const auto rightIt = std::ranges::find(g_bottomWindowOrder, rightKey);
            const bool leftKnown = leftIt != g_bottomWindowOrder.end();
            const bool rightKnown = rightIt != g_bottomWindowOrder.end();

            if (leftKnown && rightKnown)
                return leftIt < rightIt;

            if (leftKnown != rightKnown)
                return leftKnown;

            return false;
        });
    }

    static bool ensureWeightsForOrder(
        const std::vector<std::uintptr_t>& order,
        std::unordered_map<std::uintptr_t, double>& weights
    ) {
        bool changed = false;

        for (const auto key : order) {
            if (key == 0)
                continue;

            const auto it = weights.find(key);
            if (it == weights.end() || !std::isfinite(it->second) || it->second <= 0.0) {
                weights[key] = 1.0;
                changed = true;
            }
        }

        return changed;
    }

    static bool resizeKeyInOrder(
        const std::vector<std::uintptr_t>& order,
        std::uintptr_t key,
        int delta,
        std::unordered_map<std::uintptr_t, double>& weights
    ) {
        if (delta == 0 || order.size() < 2 || key == 0)
            return false;

        const auto it = std::ranges::find(order, key);
        if (it == order.end())
            return false;

        const auto index = static_cast<std::size_t>(std::distance(order.begin(), it));
        const bool hasLeft = index > 0;
        const bool hasRight = index + 1 < order.size();

        if (!hasLeft && !hasRight)
            return false;

        constexpr double STEP = 0.04;
        constexpr double EDGE_STEP = STEP / 2.0;
        constexpr double MIN_WEIGHT = 0.20;
        constexpr double EPSILON = 0.001;

        auto focusedWeight = sanitizedWeight(weights, key);

        const auto canTakeFrom = [&](std::uintptr_t neighbourKey) -> double {
            if (neighbourKey == 0 || neighbourKey == key)
                return 0.0;

            const double neighbourWeight = sanitizedWeight(weights, neighbourKey);
            return std::max(0.0, neighbourWeight - MIN_WEIGHT);
        };

        const auto giveToNeighbour = [&](std::uintptr_t neighbourKey, double amount) {
            if (neighbourKey == 0 || neighbourKey == key || amount <= 0.0)
                return;

            weights[neighbourKey] = sanitizedWeight(weights, neighbourKey) + amount;
        };

        if (delta > 0) {
            const auto leftKey = hasLeft ? order[index - 1] : 0;
            const auto rightKey = hasRight ? order[index + 1] : 0;

            double remaining = STEP;
            double taken = 0.0;

            if (hasLeft && hasRight) {
                const double leftAmount = std::min(STEP / 2.0, canTakeFrom(leftKey));
                const double rightAmount = std::min(STEP / 2.0, canTakeFrom(rightKey));

                if (leftAmount > 0.0) {
                    weights[leftKey] = sanitizedWeight(weights, leftKey) - leftAmount;
                    taken += leftAmount;
                    remaining -= leftAmount;
                }

                if (rightAmount > 0.0) {
                    weights[rightKey] = sanitizedWeight(weights, rightKey) - rightAmount;
                    taken += rightAmount;
                    remaining -= rightAmount;
                }

                if (remaining > EPSILON) {
                    const double extraLeft = std::min(remaining, canTakeFrom(leftKey));
                    if (extraLeft > 0.0) {
                        weights[leftKey] = sanitizedWeight(weights, leftKey) - extraLeft;
                        taken += extraLeft;
                        remaining -= extraLeft;
                    }
                }

                if (remaining > EPSILON) {
                    const double extraRight = std::min(remaining, canTakeFrom(rightKey));
                    if (extraRight > 0.0) {
                        weights[rightKey] = sanitizedWeight(weights, rightKey) - extraRight;
                        taken += extraRight;
                    }
                }
            } else {
                const auto neighbourKey = hasLeft ? leftKey : rightKey;
                const double amount = std::min(EDGE_STEP, canTakeFrom(neighbourKey));

                if (amount > 0.0) {
                    weights[neighbourKey] = sanitizedWeight(weights, neighbourKey) - amount;
                    taken += amount;
                }
            }

            if (taken <= EPSILON)
                return false;

            focusedWeight += taken;
            weights[key] = focusedWeight;
            return true;
        }

        if (focusedWeight <= MIN_WEIGHT + EPSILON)
            return false;

        const double amount = std::min(hasLeft && hasRight ? STEP : EDGE_STEP, focusedWeight - MIN_WEIGHT);
        if (amount <= EPSILON)
            return false;

        focusedWeight -= amount;
        weights[key] = focusedWeight;

        if (hasLeft && hasRight) {
            giveToNeighbour(order[index - 1], amount / 2.0);
            giveToNeighbour(order[index + 1], amount / 2.0);
        } else {
            giveToNeighbour(hasLeft ? order[index - 1] : order[index + 1], amount);
        }

        return true;
    }

    void clearBottomFullscreenState(bool restoreVisuals) {
        if (restoreVisuals)
            setRowFullscreenVisualState(windowFromKey(m_bottomFullscreen.windowKey), false);

        g_bottomFullscreenWindowKeys.erase(m_bottomFullscreen.windowKey);
        m_bottomFullscreen.windowKey = 0;
    }


    bool addTarget(const SP<Layout::ITarget>& target) {
        if (!target)
            return false;

        if (std::ranges::find(m_targets, target) == m_targets.end())
            m_targets.push_back(target);

        const bool restoredTop = restoreTopStateFromPersistence(target);
        if (!restoredTop) {
            const auto key = windowKey(target->window());
            if (key != 0) {
                if (!g_bottomWindowWeights.contains(key))
                    g_bottomWindowWeights[key] = 1.0;

                if (g_bottomFullscreenWindowKeys.contains(key)) {
                    m_bottomFullscreen.windowKey = key;
                    setRowFullscreenVisualState(target->window(), true);
                }
            }
        }

        applyPersistentBottomOrder();
        return restoredTop;
    }

    void insertBottomTargetAfterKey(const SP<Layout::ITarget>& target, std::uintptr_t insertAfterKey) {
        if (!target || insertAfterKey == 0)
            return;

        const auto key = windowKey(target->window());
        if (key == 0 || g_topState.windowKeys.contains(key))
            return;

        const auto focusedIt = std::ranges::find_if(m_targets, [&](const auto& other) {
            const auto otherKey = windowKey(other ? other->window() : nullptr);
            return otherKey == insertAfterKey && !g_topState.windowKeys.contains(otherKey);
        });

        if (focusedIt == m_targets.end())
            return;

        const auto targetIt = std::ranges::find(m_targets, target);
        if (targetIt == m_targets.end())
            return;

        auto movedTarget = *targetIt;
        m_targets.erase(targetIt);

        const auto focusedAgainIt = std::ranges::find_if(m_targets, [&](const auto& other) {
            const auto otherKey = windowKey(other ? other->window() : nullptr);
            return otherKey == insertAfterKey && !g_topState.windowKeys.contains(otherKey);
        });

        if (focusedAgainIt == m_targets.end()) {
            m_targets.push_back(movedTarget);
            return;
        }

        m_targets.insert(std::next(focusedAgainIt), movedTarget);
        saveBottomWindowOrder(bottomWindowKeysInOrder());
    }

    SP<Layout::ITarget> targetForWindow(const PHLWINDOW& window) const {
        if (!window)
            return nullptr;

        for (const auto& target : m_targets) {
            if (target && target->window() == window)
                return target;
        }

        return nullptr;
    }

    void pruneDeadTopTargets() {
        std::vector<std::uintptr_t> staleKeys;

        for (const auto& [key, target] : g_topState.targets) {
            if (!target || windowIsClosingOrDead(target->window()))
                staleKeys.push_back(key);
        }

        for (const auto key : staleKeys) {
            g_topState.windowKeys.erase(key);
            g_topState.windowProfiles.erase(key);
            g_topState.targets.erase(key);

            for (auto& [profile, profileData] : g_topState.profiles) {
                std::erase(profileData.order, key);
                profileData.weights.erase(key);
                profileData.lastBoxes.erase(key);
                if (profileData.fullscreenWindowKey == key)
                    profileData.fullscreenWindowKey = 0;
            }
        }

        if (!staleKeys.empty()) {
            for (const auto key : staleKeys)
                g_topState.hiddenWindowKeys.erase(key);
            markInactiveTopProfilesDirty();
            savePersistentState();
        }
    }

    static double sanitizedWeight(const std::unordered_map<std::uintptr_t, double>& weights, std::uintptr_t key) {
        const auto it = weights.find(key);
        if (it == weights.end() || !std::isfinite(it->second) || it->second <= 0.0)
            return 1.0;

        return std::clamp(it->second, 0.2, 10.0);
    }

    struct SPlacedTarget {
        SP<Layout::ITarget> target;
        CBox box;
    };

    static std::vector<SPlacedTarget> calculateColumnRowBoxes(
        const std::vector<SP<Layout::ITarget>>& targets,
        const CBox& area,
        const std::unordered_map<std::uintptr_t, double>& weights
    ) {
        std::vector<SPlacedTarget> placements;
        const std::size_t count = targets.size();

        if (count == 0 || area.w <= 0.0 || area.h <= 0.0)
            return placements;

        double totalWeight = 0.0;
        for (const auto& target : targets)
            totalWeight += sanitizedWeight(weights, windowKey(target ? target->window() : nullptr));

        if (totalWeight <= 0.0)
            totalWeight = static_cast<double>(count);

        double x = area.x;
        double remainingWidth = area.w;
        double remainingWeight = totalWeight;

        for (std::size_t i = 0; i < count; ++i) {
            const auto& target = targets[i];

            if (!target)
                continue;

            double width = remainingWidth;
            const double weight = sanitizedWeight(weights, windowKey(target->window()));

            if (i + 1 < count && remainingWeight > 0.0)
                width = std::floor(remainingWidth * (weight / remainingWeight));

            placements.push_back({target, CBox{x, area.y, width, area.h}});

            x += width;
            remainingWidth -= width;
            remainingWeight -= weight;
        }

        return placements;
    }

    static void applyPlacement(const SP<Layout::ITarget>& target, const CBox& box) {
        if (!target)
            return;

        target->setPositionGlobal(box);
        target->damageEntire();
        target->warpPositionSize();
    }

    static void placeColumnRow(
        const std::vector<SP<Layout::ITarget>>& targets,
        const CBox& area,
        const std::unordered_map<std::uintptr_t, double>& weights,
        std::unordered_map<std::uintptr_t, CBox>* savedBoxes = nullptr
    ) {
        const auto placements = calculateColumnRowBoxes(targets, area, weights);

        for (const auto& placement : placements) {
            const auto key = windowKey(placement.target ? placement.target->window() : nullptr);

            if (savedBoxes && key != 0)
                (*savedBoxes)[key] = placement.box;

            applyPlacement(placement.target, placement.box);
        }
    }

    static void placeSingleTargetNoGaps(const SP<Layout::ITarget>& target, const CBox& area) {
        if (!target || area.w <= 0.0 || area.h <= 0.0)
            return;

        const Layout::STargetBox box{
            .logicalBox = area,
            .visualBox = area,
        };

        target->setPositionGlobal(box);
        target->damageEntire();
        target->warpPositionSize();
    }

    void placeTargetsInRows() {
        const auto parent = m_parent.lock();

        if (!parent)
            return;

        const auto space = parent->space();

        if (!space)
            return;

        pruneDeadTopTargets();

        // Do not drop targets just because their current geometry is outside
        // this space. Bottom fullscreen collapses non-focused bottom targets;
        // while collapsed, target->space() may no longer match the active
        // layout space. If we erase them here, restore has no targets left to
        // tile. Hyprland calls removeTarget() for real workspace/layout
        // removal, so here we only prune invalid or closing targets.
        std::erase_if(m_targets, [&](const auto& target) {
            return !target
                || !target->window()
                || windowIsClosingOrDead(target->window());
        });

        const CBox workArea = space->workArea(false);

        CBox fullscreenWorkArea = workArea;
        if (const auto workspace = space->workspace(); workspace && workspace->m_monitor)
            fullscreenWorkArea = workspace->m_monitor->logicalBoxMinusReserved();

        const double topHeight = std::floor(workArea.h * g_topRowRatio);
        const CBox topArea{
            workArea.x,
            workArea.y,
            workArea.w,
            topHeight,
        };
        const CBox bottomArea{
            workArea.x,
            workArea.y + topHeight,
            workArea.w,
            workArea.h - topHeight,
        };

        const double fullscreenTopHeight = std::floor(fullscreenWorkArea.h * g_topRowRatio);
        const CBox fullscreenTopArea{
            fullscreenWorkArea.x,
            fullscreenWorkArea.y,
            fullscreenWorkArea.w,
            fullscreenTopHeight,
        };
        const CBox fullscreenBottomArea{
            fullscreenWorkArea.x,
            fullscreenWorkArea.y + fullscreenTopHeight,
            fullscreenWorkArea.w,
            fullscreenWorkArea.h - fullscreenTopHeight,
        };

        std::vector<SP<Layout::ITarget>> topTargets;
        std::vector<SP<Layout::ITarget>> bottomTargets;

        auto& activeOrder = g_topState.profiles[g_topState.activeProfile].order;

        for (const auto key : activeOrder) {
            const auto it = g_topState.targets.find(key);

            if (it == g_topState.targets.end())
                continue;

            const auto& target = it->second;

            if (target && target->window() && !windowIsClosingOrDead(target->window()))
                topTargets.push_back(target);
        }

        for (const auto& target : m_targets) {
            if (!target)
                continue;

            const auto key = windowKey(target->window());

            if (key == 0 || !g_topState.windowKeys.contains(key))
                bottomTargets.push_back(target);
        }

        auto& activeProfileData = g_topState.profiles[g_topState.activeProfile];
        if (activeProfileData.fullscreenWindowKey != 0) {
            auto fullscreenIt = std::ranges::find_if(topTargets, [&](const auto& target) {
                return target && windowKey(target->window()) == activeProfileData.fullscreenWindowKey;
            });

            if (fullscreenIt != topTargets.end()) {
                for (const auto& target : topTargets) {
                    if (!target || !target->window())
                        continue;

                    if (target == *fullscreenIt)
                        setTopProfileHiddenState(target->window(), false);
                    else
                        setTopProfileHiddenState(target->window(), true);
                }

                if ((*fullscreenIt)->window())
                    setRowFullscreenVisualState((*fullscreenIt)->window(), true);

                placeSingleTargetNoGaps(*fullscreenIt, fullscreenTopArea);
                activeProfileData.lastBoxes[activeProfileData.fullscreenWindowKey] = fullscreenTopArea;
            } else {
                setRowFullscreenVisualState(windowFromKey(activeProfileData.fullscreenWindowKey), false);
                activeProfileData.fullscreenWindowKey = 0;
                markInactiveTopProfilesDirty();
                savePersistentState();

                for (const auto& target : topTargets) {
                    if (target && target->window())
                        setTopProfileHiddenState(target->window(), false);
                }

                placeColumnRow(topTargets, topArea, activeProfileData.weights, &activeProfileData.lastBoxes);
            }
        } else {
            for (const auto& target : topTargets) {
                if (target && target->window())
                    setTopProfileHiddenState(target->window(), false);
            }

            placeColumnRow(topTargets, topArea, activeProfileData.weights, &activeProfileData.lastBoxes);
        }

        // Inactive top profiles are hidden rather than parked off-screen.
        // Off-screen tiling makes Hyprland apply different edge/gap handling,
        // which changes Chromium/Electron client buffers by a few pixels.
        // Keeping hidden profiles in their real top-row geometry avoids that
        // resize path entirely.

        // Bottom fullscreen is different: keep the other bottom targets inside
        // the current layout space with a tiny geometry. Parking them far
        // off-screen can make Hyprland detach them from this layout space, so
        // restoring fullscreen leaves only the fullscreen target until another
        // pointer drag forces the layout target list to update.
        const CBox collapsedBottomArea{
            bottomArea.x + std::max(0.0, bottomArea.w - 1.0),
            bottomArea.y + std::max(0.0, bottomArea.h - 1.0),
            1.0,
            1.0,
        };

        if (m_bottomFullscreen.windowKey != 0) {
            auto fullscreenIt = std::ranges::find_if(bottomTargets, [this](const auto& target) {
                return target && windowKey(target->window()) == m_bottomFullscreen.windowKey;
            });

            if (fullscreenIt != bottomTargets.end()) {
                // Bottom fullscreen is a real bottom-row layout mode: only the
                // fullscreen target owns the visible bottom row. Other bottom
                // targets remain managed and keep their row/order state, but
                // are collapsed until fullscreen is restored. This avoids
                // hover-focus leaking through to tiled windows underneath.
                placeSingleTargetNoGaps(*fullscreenIt, fullscreenBottomArea);

                for (const auto& target : bottomTargets) {
                    if (!target || target == *fullscreenIt)
                        continue;

                    target->setPositionGlobal(collapsedBottomArea);
                    target->damageEntire();
                    target->warpPositionSize();
                }
            } else {
                clearBottomFullscreenState(true);
                placeColumnRow(bottomTargets, bottomArea, g_bottomWindowWeights);
            }
        } else {
            placeColumnRow(bottomTargets, bottomArea, g_bottomWindowWeights);

            if (m_bottomFullscreen.forceRestoreSpaceUpdate) {
                for (const auto& target : bottomTargets) {
                    if (!target)
                        continue;

                    target->onUpdateSpace();
                    target->damageEntire();
                    target->warpPositionSize();
                }
            }
        }

        // Keep inactive top profiles hidden, but avoid relaying them out on
        // every recalculation. They only need fresh geometry when the top area,
        // profile membership, order, or resize weights change.
        const bool inactiveTopAreaChanged = !g_topState.inactiveProfilesTopArea
            || !boxesNearlyEqual(*g_topState.inactiveProfilesTopArea, topArea);
        const bool relayoutInactiveProfiles = g_topState.inactiveProfilesDirty || inactiveTopAreaChanged;

        for (int profile = 1; profile <= PROFILE_COUNT; ++profile) {
            if (profile == g_topState.activeProfile)
                continue;

            const auto profileIt = g_topState.profiles.find(profile);
            if (profileIt == g_topState.profiles.end())
                continue;

            auto& profileData = profileIt->second;
            std::vector<SP<Layout::ITarget>> hiddenTopTargets;
            bool missingSavedBox = false;

            for (const auto key : profileData.order) {
                const auto targetIt = g_topState.targets.find(key);
                if (targetIt == g_topState.targets.end())
                    continue;

                const auto& target = targetIt->second;
                if (!target || !target->window() || windowIsClosingOrDead(target->window()))
                    continue;

                hiddenTopTargets.push_back(target);

                const auto savedBoxIt = profileData.lastBoxes.find(key);
                if (savedBoxIt == profileData.lastBoxes.end() || savedBoxIt->second.w <= 0.0 || savedBoxIt->second.h <= 0.0)
                    missingSavedBox = true;
            }

            if (relayoutInactiveProfiles || missingSavedBox) {
                if (profileData.fullscreenWindowKey != 0) {
                    auto fullscreenIt = std::ranges::find_if(hiddenTopTargets, [&](const auto& target) {
                        return target && windowKey(target->window()) == profileData.fullscreenWindowKey;
                    });

                    if (fullscreenIt != hiddenTopTargets.end()) {
                        if ((*fullscreenIt)->window())
                            setRowFullscreenVisualState((*fullscreenIt)->window(), true);

                        placeSingleTargetNoGaps(*fullscreenIt, fullscreenTopArea);
                        profileData.lastBoxes[profileData.fullscreenWindowKey] = fullscreenTopArea;
                    } else {
                        setRowFullscreenVisualState(windowFromKey(profileData.fullscreenWindowKey), false);
                        profileData.fullscreenWindowKey = 0;
                        savePersistentState();
                    }
                }

                const auto placements = calculateColumnRowBoxes(hiddenTopTargets, topArea, profileData.weights);

                for (const auto& placement : placements) {
                    const auto key = windowKey(placement.target ? placement.target->window() : nullptr);

                    if (key != 0 && key != profileData.fullscreenWindowKey)
                        profileData.lastBoxes[key] = placement.box;

                    if (key != profileData.fullscreenWindowKey)
                        applyPlacement(placement.target, placement.box);

                    if (placement.target && placement.target->window())
                        setTopProfileHiddenState(placement.target->window(), true);
                }
            } else {
                for (const auto& target : hiddenTopTargets) {
                    if (target && target->window())
                        setTopProfileHiddenState(target->window(), true);
                }
            }
        }

        g_topState.inactiveProfilesDirty = false;
        g_topState.inactiveProfilesTopArea = topArea;

        m_spawnFollowsFocusReady = true;
    }
};

void recalculateAllInstances() {
    for (auto* instance : g_instances) {
        if (instance)
            instance->recalculate(Layout::RECALCULATE_REASON_UNKNOWN);
    }

    for (auto* instance : g_instances) {
        if (instance)
            instance->finishRecalculate();
    }
}

void clearClosedWindowState(const PHLWINDOW& window) {
    const auto key = windowKey(window);

    if (key == 0)
        return;

    std::uintptr_t topFocusFallbackKey = 0;
    if (g_topState.windowKeys.contains(key)) {
        if (const auto fallbackTarget = topProfileFocusCandidateAfterRemoving(key); fallbackTarget && fallbackTarget->window())
            topFocusFallbackKey = windowKey(fallbackTarget->window());
    }

    bool changed = false;
    const auto oldBottomOrderSize = g_bottomWindowOrder.size();
    std::erase(g_bottomWindowOrder, key);
    changed = oldBottomOrderSize != g_bottomWindowOrder.size() || changed;
    changed = g_bottomWindowWeights.erase(key) > 0 || changed;
    changed = g_bottomFullscreenWindowKeys.erase(key) > 0 || changed;

    for (auto* instance : g_instances) {
        if (instance && instance->clearBottomFullscreenForWindow(key, false))
            changed = true;
    }

    if (g_topState.windowKeys.contains(key))
        changed = clearTopWindowState(window) || changed;

    if (changed) {
        savePersistentState();
        recalculateAllInstances();

        if (topFocusFallbackKey != 0)
            focusWindowByKey(topFocusFallbackKey);
    }
}

void clearClosedWindowState(const PHLWINDOWREF& windowRef) {
    clearClosedWindowState(windowRef.lock());
}

bool registerEventListeners() {
    if (!Event::bus())
        return false;

    g_windowCloseListener = Event::bus()->m_events.window.close.listen([](const PHLWINDOWREF& windowRef) {
        clearClosedWindowState(windowRef);
    });

    g_windowDestroyListener = Event::bus()->m_events.window.destroy.listen([](const PHLWINDOWREF& windowRef) {
        clearClosedWindowState(windowRef);
    });

    g_windowActiveListener = Event::bus()->m_events.window.active.listen([](PHLWINDOW window, Desktop::eFocusReason reason) {
        // Workspace switches can emit a focus event for the previously active
        // sticky top window. That is not a new user focus choice and must not
        // override the workspace.active event's bottom-row spawn intent.
        if (reason == Desktop::FOCUS_REASON_WORKSPACE_CHANGE)
            return;

        updateSpawnIntentFromFocusedWindow(window);
    });

    g_workspaceActiveListener = Event::bus()->m_events.workspace.active.listen([](auto) {
        setSpawnIntent(EFocusedSplitRow::Bottom, ESpawnIntentSource::WorkspaceSwitch);
    });

    return g_windowCloseListener && g_windowDestroyListener && g_windowActiveListener && g_workspaceActiveListener;
}

void unregisterEventListeners() {
    g_windowCloseListener.reset();
    g_windowDestroyListener.reset();
    g_windowActiveListener.reset();
    g_workspaceActiveListener.reset();
}

CSplitRowAlgorithm* algorithmForWindow(const PHLWINDOW& window) {
    if (!window)
        return nullptr;

    for (auto* instance : g_instances) {
        if (instance && instance->containsWindow(window))
            return instance;
    }

    return nullptr;
}

SDispatchResult activeSplitRowWindow(SActiveSplitRowWindow& active) {
    active = {};
    active.window = activeWindow();

    if (!active.window)
        return {.success = false, .error = "splitrow: no active window"};

    updateSpawnIntentFromFocusedWindow(active.window);

    active.algorithm = algorithmForWindow(active.window);
    if (!active.algorithm)
        return {.success = false, .error = "splitrow: active window is not managed by splitrow"};

    active.key = windowKey(active.window);

    return {.success = true, .error = ""};
}

SDispatchResult setActiveWindowTopProfile(int profile) {
    if (!validProfile(profile))
        return {.success = false, .error = "splitrow: invalid top profile"};

    SActiveSplitRowWindow active;
    if (const auto result = activeSplitRowWindow(active); !result.success)
        return result;

    if (!active.algorithm->setWindowTopProfile(active.window, profile))
        return {.success = false, .error = "splitrow: failed to update active window top profile"};

    return {.success = true, .error = ""};
}


SDispatchResult setActiveWindowTopState(bool top) {
    SActiveSplitRowWindow active;
    if (const auto result = activeSplitRowWindow(active); !result.success)
        return result;

    if (!active.algorithm->setWindowTopState(active.window, top))
        return {.success = false, .error = "splitrow: failed to update active window row"};

    return {.success = true, .error = ""};
}


SDispatchResult showTopProfileResult(int profile) {
    if (!validProfile(profile))
        return {.success = false, .error = "splitrow: invalid top profile"};

    showTopProfile(profile);
    return {.success = true, .error = ""};
}

SDispatchResult toggleActiveWindowTopState() {
    SActiveSplitRowWindow active;
    if (const auto result = activeSplitRowWindow(active); !result.success)
        return result;

    if (!active.algorithm->toggleWindowTopState(active.window))
        return {.success = false, .error = "splitrow: failed to toggle active window row"};

    return {.success = true, .error = ""};
}


SDispatchResult toggleActiveFocusedFullscreen() {
    SActiveSplitRowWindow active;
    if (const auto result = activeSplitRowWindow(active); !result.success)
        return result;

    if (active.key != 0 && g_topState.windowKeys.contains(active.key)) {
        if (!active.algorithm->toggleTopFullscreen(active.window))
            return {.success = false, .error = "splitrow: failed to toggle top fullscreen"};

        return {.success = true, .error = ""};
    }

    if (active.algorithm->hasBottomFullscreen()) {
        active.algorithm->restoreBottomFullscreenFromBind();
        return {.success = true, .error = ""};
    }

    if (!active.algorithm->toggleBottomFullscreen(active.window))
        return {.success = false, .error = "splitrow: failed to toggle bottom fullscreen"};

    return {.success = true, .error = ""};
}


SDispatchResult resizeActiveWindowByWeight(int delta) {
    SActiveSplitRowWindow active;
    if (const auto result = activeSplitRowWindow(active); !result.success)
        return result;

    if (!active.algorithm->resizeWindowInRow(active.window, delta))
        return {.success = false, .error = "splitrow: active row cannot be resized"};

    return {.success = true, .error = ""};
}


SDispatchResult moveActiveWindowHorizontally(int delta) {
    SActiveSplitRowWindow active;
    if (const auto result = activeSplitRowWindow(active); !result.success)
        return result;

    if (active.key != 0 && g_topState.windowKeys.contains(active.key)) {
        if (!moveTopWindowInOrder(active.window, delta))
            return {.success = true, .error = ""};

        recalculateAllInstances();
        return {.success = true, .error = ""};
    }

    active.algorithm->moveWindowInBottomRow(active.window, delta);
    return {.success = true, .error = ""};
}


SDispatchResult releaseActiveWindowFromSplitRowState() {
    SActiveSplitRowWindow active;
    if (const auto result = activeSplitRowWindow(active); !result.success)
        return result;

    if (active.key == 0 || !g_topState.windowKeys.contains(active.key))
        return {.success = true, .error = ""};

    // Native Hyprland workspace move dispatchers do not know about splitrow
    // top profiles. Top-profile windows are pinned so inactive profiles can
    // survive workspace switches, which means a normal movetoworkspace bind can
    // leave the focused top-profile window stuck in splitrow state. Release it
    // first, then let the user's normal workspace dispatcher move it.
    if (!clearTopWindowState(active.window))
        return {.success = true, .error = ""};

    if (!g_bottomWindowWeights.contains(active.key))
        g_bottomWindowWeights[active.key] = 1.0;

    savePersistentState();
    recalculateAllInstances();
    return {.success = true, .error = ""};
}


SDispatchResult moveActiveWindowToWorkspace(int workspace) {
    if (workspace <= 0)
        return {.success = false, .error = "splitrow: invalid workspace"};

    // Release top-profile state before using Hyprland's own workspace move
    // dispatcher. This keeps workspace move compatibility internal to the plugin.
    if (const auto releaseResult = releaseActiveWindowFromSplitRowState(); !releaseResult.success)
        return releaseResult;

    if (!g_pKeybindManager)
        return {.success = false, .error = "splitrow: keybind manager unavailable"};

    const auto dispatcherIt = g_pKeybindManager->m_dispatchers.find("movetoworkspacesilent");
    if (dispatcherIt == g_pKeybindManager->m_dispatchers.end())
        return {.success = false, .error = "splitrow: movetoworkspacesilent dispatcher unavailable"};

    const auto result = dispatcherIt->second(std::to_string(workspace));
    if (!result.success)
        return {.success = false, .error = result.error.empty() ? "splitrow: failed to move active window to workspace" : result.error};

    return {.success = true, .error = ""};
}


int luaMoveTop(lua_State*) {
    const auto result = setActiveWindowTopState(true);

    if (!result.success && !result.error.empty())
        notify(result.error, CHyprColor{1.0F, 0.35F, 0.2F, 1.0F});

    return 0;
}

int luaMoveBottom(lua_State*) {
    const auto result = setActiveWindowTopState(false);

    if (!result.success && !result.error.empty())
        notify(result.error, CHyprColor{1.0F, 0.35F, 0.2F, 1.0F});

    return 0;
}

int luaToggleRow(lua_State*) {
    const auto result = toggleActiveWindowTopState();

    if (!result.success && !result.error.empty())
        notify(result.error, CHyprColor{1.0F, 0.35F, 0.2F, 1.0F});

    return 0;
}

int luaReleaseActive(lua_State*) {
    const auto result = releaseActiveWindowFromSplitRowState();

    if (!result.success && !result.error.empty())
        notify(result.error, CHyprColor{1.0F, 0.35F, 0.2F, 1.0F});

    return 0;
}

int luaToggleFocusedFullscreen(lua_State*) {
    const auto result = toggleActiveFocusedFullscreen();

    if (!result.success && !result.error.empty())
        notify(result.error, CHyprColor{1.0F, 0.35F, 0.2F, 1.0F});

    return 0;
}


int luaMoveLeft(lua_State*) {
    const auto result = moveActiveWindowHorizontally(-1);

    if (!result.success && !result.error.empty())
        notify(result.error, CHyprColor{1.0F, 0.35F, 0.2F, 1.0F});

    return 0;
}

int luaMoveRight(lua_State*) {
    const auto result = moveActiveWindowHorizontally(1);

    if (!result.success && !result.error.empty())
        notify(result.error, CHyprColor{1.0F, 0.35F, 0.2F, 1.0F});

    return 0;
}

int luaShrinkFocused(lua_State*) {
    const auto result = resizeActiveWindowByWeight(-1);

    if (!result.success && !result.error.empty())
        notify(result.error, CHyprColor{1.0F, 0.35F, 0.2F, 1.0F});

    return 0;
}

int luaGrowFocused(lua_State*) {
    const auto result = resizeActiveWindowByWeight(1);

    if (!result.success && !result.error.empty())
        notify(result.error, CHyprColor{1.0F, 0.35F, 0.2F, 1.0F});

    return 0;
}


int luaSetTopRowRatio(lua_State* state) {
    if (!state) {
        notify("splitrow: settoprowratio expects a number", CHyprColor{1.0F, 0.35F, 0.2F, 1.0F});
        return 0;
    }

    int isNumber = 0;
    const auto ratio = lua_tonumberx(state, 1, &isNumber);

    if (!isNumber) {
        notify("splitrow: settoprowratio expects a number", CHyprColor{1.0F, 0.35F, 0.2F, 1.0F});
        return 0;
    }

    const auto result = setTopRowRatio(ratio);

    if (!result.success && !result.error.empty())
        notify(result.error, CHyprColor{1.0F, 0.35F, 0.2F, 1.0F});

    return 0;
}


std::optional<int> luaPositiveIntegerArgument(lua_State* state, const std::string& commandName, const std::string& valueName) {
    if (!state) {
        notify("splitrow: " + commandName + " expects a " + valueName, CHyprColor{1.0F, 0.35F, 0.2F, 1.0F});
        return std::nullopt;
    }

    int isNumber = 0;
    const auto value = lua_tonumberx(state, 1, &isNumber);

    if (!isNumber || !std::isfinite(value) || std::floor(value) != value || value <= 0.0) {
        notify("splitrow: " + commandName + " expects a positive integer " + valueName, CHyprColor{1.0F, 0.35F, 0.2F, 1.0F});
        return std::nullopt;
    }

    return static_cast<int>(value);
}

std::optional<int> luaProfileArgument(lua_State* state, const std::string& commandName) {
    if (!state) {
        notify("splitrow: " + commandName + " expects a profile number", CHyprColor{1.0F, 0.35F, 0.2F, 1.0F});
        return std::nullopt;
    }

    int isNumber = 0;
    const auto value = lua_tonumberx(state, 1, &isNumber);

    if (!isNumber || !std::isfinite(value) || std::floor(value) != value) {
        notify("splitrow: " + commandName + " expects a profile number", CHyprColor{1.0F, 0.35F, 0.2F, 1.0F});
        return std::nullopt;
    }

    const auto profile = static_cast<int>(value);

    if (!validProfile(profile)) {
        notify("splitrow: invalid top profile", CHyprColor{1.0F, 0.35F, 0.2F, 1.0F});
        return std::nullopt;
    }

    return profile;
}

int luaMoveToWorkspace(lua_State* state) {
    const auto workspace = luaPositiveIntegerArgument(state, "movetoworkspace", "workspace number");
    if (!workspace)
        return 0;

    const auto result = moveActiveWindowToWorkspace(*workspace);
    if (!result.success && !result.error.empty())
        notify(result.error, CHyprColor{1.0F, 0.35F, 0.2F, 1.0F});

    return 0;
}

int luaShowProfile(lua_State* state) {
    const auto profile = luaProfileArgument(state, "showprofile");
    if (!profile)
        return 0;

    const auto result = showTopProfileResult(*profile);
    if (!result.success && !result.error.empty())
        notify(result.error, CHyprColor{1.0F, 0.35F, 0.2F, 1.0F});
    return 0;
}

int luaSendToProfile(lua_State* state) {
    const auto profile = luaProfileArgument(state, "sendtoprofile");
    if (!profile)
        return 0;

    const auto result = setActiveWindowTopProfile(*profile);
    if (!result.success && !result.error.empty())
        notify(result.error, CHyprColor{1.0F, 0.35F, 0.2F, 1.0F});
    return 0;
}

bool registerLuaFunctions() {
    bool ok = true;

    auto add = [&](const std::string& name, PLUGIN_LUA_FN fn) {
        ok = HyprlandAPI::addLuaFunction(g_pluginHandle, "splitrow", name, fn) && ok;
    };

    add("movetop", luaMoveTop);
    add("movebottom", luaMoveBottom);
    add("togglerow", luaToggleRow);
    add("releaseactive", luaReleaseActive);
    add("movetoworkspace", luaMoveToWorkspace);
    add("togglefocusedfullscreen", luaToggleFocusedFullscreen);
    add("moveleft", luaMoveLeft);
    add("moveright", luaMoveRight);
    add("shrinkfocused", luaShrinkFocused);
    add("growfocused", luaGrowFocused);
    add("settoprowratio", luaSetTopRowRatio);
    add("showprofile", luaShowProfile);
    add("sendtoprofile", luaSendToProfile);

    return ok;
}


void unregisterLuaFunctions() {
    if (!g_pluginHandle)
        return;

    auto remove = [&](const std::string& name) {
        HyprlandAPI::removeLuaFunction(g_pluginHandle, "splitrow", name);
    };

    remove("movetop");
    remove("movebottom");
    remove("togglerow");
    remove("releaseactive");
    remove("movetoworkspace");
    remove("togglefocusedfullscreen");
    remove("moveleft");
    remove("moveright");
    remove("shrinkfocused");
    remove("growfocused");
    remove("settoprowratio");
    remove("showprofile");
    remove("sendtoprofile");
}


bool registerAlgorithms() {
    return HyprlandAPI::addTiledAlgo(
        g_pluginHandle,
        TILED_ALGO_NAME,
        &typeid(CSplitRowAlgorithm),
        []() -> UP<Layout::ITiledAlgorithm> {
            return makeUnique<CSplitRowAlgorithm>();
        }
    );
}

void unregisterAlgorithms() {
    if (g_pluginHandle)
        HyprlandAPI::removeAlgo(g_pluginHandle, TILED_ALGO_NAME);
}

} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    g_pluginHandle = handle;

    const std::string runningHash = HyprlandAPI::getHyprlandVersion(handle).hash;
    const std::string headerAbi = __hyprland_api_get_hash();

    const bool hashMatches =
        runningHash == headerAbi
        || (!runningHash.empty() && headerAbi.rfind(runningHash, 0) == 0);

    if (!hashMatches) {
        HyprlandAPI::addNotification(
            handle,
            "hyprsplitrow: header/runtime hash mismatch. Refusing to load.",
            CHyprColor{1.0F, 0.2F, 0.2F, 1.0F},
            6000
        );
        throw std::runtime_error(
            "hyprsplitrow: header/runtime hash mismatch: running="
            + runningHash
            + " header="
            + headerAbi
        );
    }

    loadPersistentState();

    if (!registerAlgorithms()) {
        HyprlandAPI::addNotification(
            handle,
            "hyprsplitrow: failed to register tiled algorithm 'splitrow'",
            CHyprColor{1.0F, 0.2F, 0.2F, 1.0F},
            6000
        );
        throw std::runtime_error("hyprsplitrow: failed to register tiled algorithm");
    }

    if (!registerLuaFunctions()) {
        unregisterAlgorithms();
        HyprlandAPI::addNotification(
            handle,
            "hyprsplitrow: failed to register Lua functions",
            CHyprColor{1.0F, 0.2F, 0.2F, 1.0F},
            6000
        );
        throw std::runtime_error("hyprsplitrow: failed to register Lua functions");
    }

    if (!registerEventListeners()) {
        unregisterLuaFunctions();
        unregisterAlgorithms();
        HyprlandAPI::addNotification(
            handle,
            "hyprsplitrow: failed to register window close listeners",
            CHyprColor{1.0F, 0.2F, 0.2F, 1.0F},
            6000
        );
        throw std::runtime_error("hyprsplitrow: failed to register event listeners");
    }

    notify("hyprsplitrow loaded");

    return {
        PLUGIN_NAME,
        "Two-row split layout for Hyprland with sticky top profiles, focused row fullscreen, focus-following spawns, and configurable row ratio.",
        "Sarah Mac Carthy + ChatGPT",
        PLUGIN_VERSION
    };
}

APICALL EXPORT void PLUGIN_EXIT() {
    savePersistentState();
    cancelTopProfileTransition();
    unregisterEventListeners();
    unregisterLuaFunctions();
    unregisterAlgorithms();
    clearAllTopFullscreenState(true);
    for (const auto& [key, target] : g_topState.targets) {
        if (target && target->window()) {
            setTopProfileHiddenState(target->window(), false);
            target->window()->m_pinned = false;
        }
    }

    for (auto* instance : g_instances) {
        if (instance)
            instance->clearBottomFullscreenOnExit();
    }

    g_instances.clear();
    g_topState.windowKeys.clear();
    g_topState.windowProfiles.clear();
    g_topState.targets.clear();
    g_topState.profiles.clear();
    g_topState.hiddenWindowKeys.clear();
    g_topProfileTransition.fadingOutKeys.clear();
    g_topProfileTransition.fadingInKeys.clear();
    g_topState.activeProfile = 1;
    g_topState.inactiveProfilesDirty = true;
    g_topState.inactiveProfilesTopArea.reset();
    notify("hyprsplitrow unloaded");
    g_pluginHandle = nullptr;
}
