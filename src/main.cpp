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
constexpr const char* PLUGIN_VERSION = "0.2.0-primary-secondary";
constexpr double DEFAULT_PRIMARY_REGION_RATIO = 1.0 / 3.0;
constexpr double MIN_PRIMARY_REGION_RATIO = 0.10;
constexpr double MAX_PRIMARY_REGION_RATIO = 0.90;
constexpr bool PRIMARY_PROFILE_FADE_ENABLED = true;
constexpr float PRIMARY_PROFILE_FADE_IN_START_ALPHA = 0.0F;
constexpr float PRIMARY_PROFILE_FADE_OUT_END_ALPHA = 0.01F;
// Fade-out hides only after reaching near-invisible alpha to avoid a mid-fade blink.


class CSplitRegionAlgorithm;

std::vector<CSplitRegionAlgorithm*> g_instances;
constexpr int PRIMARY_PROFILE_COUNT = 10;

struct SPrimaryProfileData {
    std::vector<std::uintptr_t> order;
    std::unordered_map<std::uintptr_t, double> weights;
    std::unordered_map<std::uintptr_t, CBox> lastBoxes;
    std::uintptr_t fullscreenWindowKey = 0;
};

struct SPrimaryProfileState {
    int activeProfile = 1;
    std::unordered_set<std::uintptr_t> windowKeys;
    std::unordered_set<std::uintptr_t> hiddenWindowKeys;
    std::unordered_map<std::uintptr_t, int> windowProfiles;
    std::unordered_map<std::uintptr_t, SP<Layout::ITarget>> targets;
    std::unordered_map<int, SPrimaryProfileData> profiles;
    bool inactiveProfilesDirty = true;
    std::optional<CBox> inactiveProfilesPrimaryArea;
};

struct SPrimaryProfileTransition {
    std::unordered_set<std::uintptr_t> fadingOutKeys;
    std::unordered_set<std::uintptr_t> fadingInKeys;
};

struct SSecondaryFullscreenState {
    std::uintptr_t windowKey = 0;
    bool forceRestoreSpaceUpdate = false;
};

enum class EFocusedSplitRegion {
    Secondary,
    Primary,
};

enum class ESpawnIntentSource {
    FocusedWindow,
    PrimaryProfileSwitch,
    WorkspaceSwitch,
};

struct SSpawnIntent {
    EFocusedSplitRegion region = EFocusedSplitRegion::Secondary;
    ESpawnIntentSource source = ESpawnIntentSource::FocusedWindow;
    std::uintptr_t focusedWindowKeyWhenSourceSet = 0;
};

struct SActiveSplitRegionWindow {
    PHLWINDOW window;
    CSplitRegionAlgorithm* algorithm = nullptr;
    std::uintptr_t key = 0;
};

SPrimaryProfileState g_primaryState;
SPrimaryProfileTransition g_primaryProfileTransition;
SSpawnIntent g_spawnIntent;
std::unordered_map<std::uintptr_t, double> g_secondaryWindowWeights;
std::vector<std::uintptr_t> g_secondaryWindowOrder;
std::unordered_set<std::uintptr_t> g_secondaryFullscreenWindowKeys;
double g_primaryRegionRatio = DEFAULT_PRIMARY_REGION_RATIO;

Hyprutils::Signal::CHyprSignalListener g_windowCloseListener;
Hyprutils::Signal::CHyprSignalListener g_windowDestroyListener;
Hyprutils::Signal::CHyprSignalListener g_windowActiveListener;
Hyprutils::Signal::CHyprSignalListener g_workspaceActiveListener;


void recalculateAllInstances();
void savePersistentState();
void loadPersistentState();
bool restorePrimaryStateFromPersistence(const SP<Layout::ITarget>& target);
bool windowIsClosingOrDead(const PHLWINDOW& window);
bool validProfile(int profile);
std::uintptr_t windowKey(const PHLWINDOW& window);
PHLWINDOW windowFromKey(std::uintptr_t key);
bool movePrimaryWindowInOrder(const PHLWINDOW& window, int delta);
void showPrimaryProfile(int profile, bool focusProfileWindow = true);
std::uintptr_t focusCandidateKeyForPrimaryProfile(int profile);
std::vector<std::uintptr_t> liveWindowKeysForPrimaryProfile(int profile);
void resetProfileFadeAlpha(const PHLWINDOW& window);
void setProfileFadeInputBlocked(const PHLWINDOW& window, bool blocked);
void cancelPrimaryProfileTransition();
void startPrimaryProfileFade(int fromPrimaryProfile, int toPrimaryProfile);
SDispatchResult toggleActiveFocusedFullscreen();
SDispatchResult resizeActiveWindowByWeight(int delta);
void setRegionFullscreenVisualState(const PHLWINDOW& window, bool enabled);
bool clearPrimaryFullscreenForWindow(std::uintptr_t key, bool restoreVisuals);
void clearAllPrimaryFullscreenState(bool restoreVisuals);
SP<Layout::ITarget> primaryProfileFocusCandidateAfterRemoving(std::uintptr_t key);


bool focusWindowByKey(std::uintptr_t key);
void setPrimaryProfileHiddenState(const PHLWINDOW& window, bool hidden);
void markInactivePrimaryProfilesDirty();
bool boxesNearlyEqual(const CBox& a, const CBox& b);
void setSpawnIntent(EFocusedSplitRegion region, ESpawnIntentSource source);
void updateSpawnIntentFromFocusedWindow(const PHLWINDOW& window);
std::optional<EFocusedSplitRegion> regionFromCursorForArea(const CBox& area);
void saveSecondaryWindowOrder(const std::vector<std::uintptr_t>& order);
void removeSecondaryWindowFromPersistentState(std::uintptr_t key);
SDispatchResult setSplitRatio(double ratio);
SDispatchResult releaseActiveWindowFromSplitRegionState();
SDispatchResult moveActiveWindowToWorkspace(int workspace);


std::filesystem::path persistentStatePath() {
    if (const char* xdgCache = std::getenv("XDG_CACHE_HOME"); xdgCache && *xdgCache)
        return std::filesystem::path{xdgCache} / "hyprsplitrow-primary-secondary" / "state.txt";

    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path{home} / ".cache" / "hyprsplitrow-primary-secondary" / "state.txt";

    return std::filesystem::temp_directory_path() / "hyprsplitrow-primary-secondary-state.txt";
}

void markInactivePrimaryProfilesDirty() {
    g_primaryState.inactiveProfilesDirty = true;
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
    int activeProfile = g_primaryState.activeProfile;
    std::unordered_map<int, SPrimaryProfileData> loadedPrimaryProfiles;
    std::unordered_map<std::uintptr_t, int> loadedWindowProfiles;
    std::unordered_map<std::uintptr_t, double> loadedSecondaryWeights;
    std::vector<std::uintptr_t> loadedSecondaryOrder;
    std::unordered_set<std::uintptr_t> loadedSecondaryOrderKeys;
    std::unordered_set<std::uintptr_t> loadedSecondaryFullscreenWindowKeys;

    std::string line;
    while (std::getline(file, line)) {
        std::istringstream stream{line};
        std::string kind;
        stream >> kind;

        if (kind == "version") {
            int version = 0;
            stream >> version;

            if (version != 4)
                return;
        } else if (kind == "hyprland_pid") {
            stream >> savedPid;
        } else if (kind == "active_primary_profile") {
            int profile = 0;
            stream >> profile;

            if (validProfile(profile))
                activeProfile = profile;
        } else if (kind == "primary") {
            int profile = 0;
            std::string keyText;
            stream >> profile >> keyText;

            const auto key = parseWindowKey(keyText);
            if (!validProfile(profile) || !key || *key == 0)
                continue;

            if (loadedWindowProfiles.contains(*key))
                continue;

            loadedWindowProfiles[*key] = profile;
            loadedPrimaryProfiles[profile].order.push_back(*key);
        } else if (kind == "primary_weight") {
            int profile = 0;
            std::string keyText;
            double weight = 1.0;
            stream >> profile >> keyText >> weight;

            const auto key = parseWindowKey(keyText);
            if (!validProfile(profile) || !key || *key == 0 || !std::isfinite(weight) || weight <= 0.0)
                continue;

            loadedPrimaryProfiles[profile].weights[*key] = std::clamp(weight, 0.2, 10.0);
        } else if (kind == "primary_fullscreen") {
            int profile = 0;
            std::string keyText;
            stream >> profile >> keyText;

            const auto key = parseWindowKey(keyText);
            if (!validProfile(profile) || !key || *key == 0)
                continue;

            loadedPrimaryProfiles[profile].fullscreenWindowKey = *key;
        } else if (kind == "secondary_weight") {
            std::string keyText;
            double weight = 1.0;
            stream >> keyText >> weight;

            const auto key = parseWindowKey(keyText);
            if (!key || *key == 0 || !std::isfinite(weight) || weight <= 0.0)
                continue;

            loadedSecondaryWeights[*key] = std::clamp(weight, 0.2, 10.0);
        } else if (kind == "secondary") {
            std::string keyText;
            stream >> keyText;

            const auto key = parseWindowKey(keyText);
            if (!key || *key == 0 || loadedSecondaryOrderKeys.contains(*key))
                continue;

            loadedSecondaryOrder.push_back(*key);
            loadedSecondaryOrderKeys.insert(*key);
        } else if (kind == "secondary_fullscreen") {
            std::string keyText;
            stream >> keyText;

            const auto key = parseWindowKey(keyText);
            if (!key || *key == 0)
                continue;

            loadedSecondaryFullscreenWindowKeys.insert(*key);
        }
    }

    // Window pointer addresses are only trusted inside the same Hyprland
    // process. This keeps old state files from a previous Hyprland session
    // from moving unrelated windows after address reuse.
    if (savedPid != 0 && savedPid != getpid())
        return;

    for (auto& [profile, profileData] : loadedPrimaryProfiles) {
        if (profileData.fullscreenWindowKey != 0) {
            const auto profileIt = loadedWindowProfiles.find(profileData.fullscreenWindowKey);
            if (profileIt == loadedWindowProfiles.end() || profileIt->second != profile)
                profileData.fullscreenWindowKey = 0;
        }
    }

    g_primaryState.activeProfile = activeProfile;
    g_primaryState.windowProfiles = std::move(loadedWindowProfiles);
    g_primaryState.profiles = std::move(loadedPrimaryProfiles);
    g_secondaryWindowWeights = std::move(loadedSecondaryWeights);
    g_secondaryWindowOrder = std::move(loadedSecondaryOrder);
    g_secondaryFullscreenWindowKeys = std::move(loadedSecondaryFullscreenWindowKeys);
}

void savePersistentState() {
    const auto path = persistentStatePath();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);

    const auto tempPath = path.string() + ".tmp";
    std::ofstream file{tempPath, std::ios::trunc};
    if (!file)
        return;

    file << "version 4\n";
    file << "hyprland_pid " << getpid() << "\n";
    file << "active_primary_profile " << g_primaryState.activeProfile << "\n";

    std::unordered_set<std::uintptr_t> writtenSecondaryOrder;
    for (const auto key : g_secondaryWindowOrder) {
        if (key == 0 || writtenSecondaryOrder.contains(key))
            continue;

        if (!windowFromKey(key))
            continue;

        if (g_primaryState.windowKeys.contains(key))
            continue;

        file << "secondary " << std::hex << key << std::dec << "\n";
        writtenSecondaryOrder.insert(key);
    }

    for (int profile = 1; profile <= PRIMARY_PROFILE_COUNT; ++profile) {
        const auto profileIt = g_primaryState.profiles.find(profile);
        if (profileIt == g_primaryState.profiles.end())
            continue;

        const auto& profileData = profileIt->second;
        std::unordered_set<std::uintptr_t> written;

        for (const auto key : profileData.order) {
            if (key == 0 || written.contains(key))
                continue;

            const auto windowProfileIt = g_primaryState.windowProfiles.find(key);
            if (windowProfileIt == g_primaryState.windowProfiles.end() || windowProfileIt->second != profile)
                continue;

            file << "primary " << profile << " " << std::hex << key << std::dec << "\n";

            const auto weightIt = profileData.weights.find(key);
            if (weightIt != profileData.weights.end() && std::isfinite(weightIt->second) && weightIt->second > 0.0)
                file << "primary_weight " << profile << " " << std::hex << key << std::dec << " " << weightIt->second << "\n";

            written.insert(key);
        }

        const auto fullscreenKey = profileData.fullscreenWindowKey;
        if (fullscreenKey != 0 && written.contains(fullscreenKey))
            file << "primary_fullscreen " << profile << " " << std::hex << fullscreenKey << std::dec << "\n";
    }

    for (const auto& [key, weight] : g_secondaryWindowWeights) {
        if (key == 0 || !std::isfinite(weight) || weight <= 0.0)
            continue;

        if (!windowFromKey(key))
            continue;

        file << "secondary_weight " << std::hex << key << std::dec << " " << weight << "\n";
    }

    for (const auto key : g_secondaryFullscreenWindowKeys) {
        if (key == 0)
            continue;

        if (!windowFromKey(key))
            continue;

        if (g_primaryState.windowKeys.contains(key))
            continue;

        file << "secondary_fullscreen " << std::hex << key << std::dec << "\n";
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

bool restorePrimaryStateFromPersistence(const SP<Layout::ITarget>& target) {
    if (!target || !target->window() || windowIsClosingOrDead(target->window()))
        return false;

    const auto key = windowKey(target->window());
    if (key == 0 || g_primaryState.windowKeys.contains(key))
        return false;

    const auto profileIt = g_primaryState.windowProfiles.find(key);
    if (profileIt == g_primaryState.windowProfiles.end() || !validProfile(profileIt->second))
        return false;

    g_primaryState.windowKeys.insert(key);
    g_primaryState.targets[key] = target;

    auto& profileData = g_primaryState.profiles[profileIt->second];
    auto& order = profileData.order;
    if (std::ranges::find(order, key) == order.end())
        order.push_back(key);

    if (!profileData.weights.contains(key))
        profileData.weights[key] = 1.0;

    target->window()->m_pinned = true;
    markInactivePrimaryProfilesDirty();
    return true;
}

bool windowIsClosingOrDead(const PHLWINDOW& window) {
    return !window || !window->m_isMapped || window->m_fadingOut || window->m_readyToDelete;
}

bool clearPrimaryWindowState(const PHLWINDOW& window) {
    const auto key = windowKey(window);

    if (key == 0)
        return false;

    bool changed = false;
    changed = g_primaryState.windowKeys.erase(key) > 0 || changed;
    changed = g_primaryState.windowProfiles.erase(key) > 0 || changed;
    changed = g_primaryState.targets.erase(key) > 0 || changed;

    for (auto& [profile, profileData] : g_primaryState.profiles) {
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
        g_primaryProfileTransition.fadingOutKeys.erase(key);
        g_primaryProfileTransition.fadingInKeys.erase(key);
        resetProfileFadeAlpha(window);
        setProfileFadeInputBlocked(window, false);
        setPrimaryProfileHiddenState(window, false);

        if (window->m_pinned) {
            window->m_pinned = false;
            changed = true;
        }
    }

    if (changed)
        markInactivePrimaryProfilesDirty();

    return changed;
}


void setPrimaryProfileHiddenState(const PHLWINDOW& window, bool hidden) {
    if (!window)
        return;

    const auto key = windowKey(window);

    if (hidden) {
        if (key != 0 && g_primaryProfileTransition.fadingOutKeys.contains(key))
            return;

        if (key != 0 && g_primaryState.hiddenWindowKeys.contains(key))
            return;

        window->setHidden(true);

        if (key != 0)
            g_primaryState.hiddenWindowKeys.insert(key);

        return;
    }

    if (key != 0 && !g_primaryState.hiddenWindowKeys.erase(key))
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

void setRegionFullscreenVisualState(const PHLWINDOW& window, bool enabled) {
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

bool clearPrimaryFullscreenForWindow(std::uintptr_t key, bool restoreVisuals) {
    if (key == 0)
        return false;

    bool changed = false;

    for (auto& [profile, profileData] : g_primaryState.profiles) {
        if (profileData.fullscreenWindowKey != key)
            continue;

        if (restoreVisuals)
            setRegionFullscreenVisualState(windowFromKey(key), false);

        profileData.fullscreenWindowKey = 0;
        changed = true;
    }

    if (changed) {
        markInactivePrimaryProfilesDirty();
        savePersistentState();
    }

    return changed;
}

void clearAllPrimaryFullscreenState(bool restoreVisuals) {
    for (auto& [profile, profileData] : g_primaryState.profiles) {
        if (profileData.fullscreenWindowKey == 0)
            continue;

        if (restoreVisuals)
            setRegionFullscreenVisualState(windowFromKey(profileData.fullscreenWindowKey), false);

        profileData.fullscreenWindowKey = 0;
    }
}

SP<Layout::ITarget> primaryProfileFocusCandidateAfterRemoving(std::uintptr_t key) {
    if (key == 0 || !g_primaryState.windowKeys.contains(key))
        return nullptr;

    const auto profileIt = g_primaryState.windowProfiles.find(key);
    if (profileIt == g_primaryState.windowProfiles.end() || !validProfile(profileIt->second))
        return nullptr;

    const auto profileDataIt = g_primaryState.profiles.find(profileIt->second);
    if (profileDataIt == g_primaryState.profiles.end())
        return nullptr;

    const auto& order = profileDataIt->second.order;
    const auto closedIt = std::ranges::find(order, key);
    if (closedIt == order.end())
        return nullptr;

    auto usableTarget = [](std::uintptr_t candidateKey) -> SP<Layout::ITarget> {
        if (candidateKey == 0)
            return nullptr;

        const auto targetIt = g_primaryState.targets.find(candidateKey);
        if (targetIt == g_primaryState.targets.end() || !targetIt->second || !targetIt->second->window())
            return nullptr;

        if (windowIsClosingOrDead(targetIt->second->window()))
            return nullptr;

        return targetIt->second;
    };

    // Prefer the left neighbour. If the closing window is leftmost, use the
    // window that will shift into its slot. Only consider windows in the same
    // primary profile, so focus does not escape to the secondary region while siblings
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

    setPrimaryProfileHiddenState(window, false);

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

void saveSecondaryWindowOrder(const std::vector<std::uintptr_t>& order) {
    if (order.empty())
        return;

    std::unordered_set<std::uintptr_t> orderKeys;
    for (const auto key : order) {
        if (key != 0)
            orderKeys.insert(key);
    }

    if (orderKeys.empty())
        return;

    std::erase_if(g_secondaryWindowOrder, [&](std::uintptr_t key) {
        return key == 0 || orderKeys.contains(key) || g_primaryState.windowKeys.contains(key);
    });

    std::unordered_set<std::uintptr_t> written;
    std::vector<std::uintptr_t> merged;
    merged.reserve(order.size() + g_secondaryWindowOrder.size());

    for (const auto key : order) {
        if (key == 0 || written.contains(key) || g_primaryState.windowKeys.contains(key))
            continue;

        merged.push_back(key);
        written.insert(key);
    }

    for (const auto key : g_secondaryWindowOrder) {
        if (key == 0 || written.contains(key) || g_primaryState.windowKeys.contains(key))
            continue;

        merged.push_back(key);
        written.insert(key);
    }

    g_secondaryWindowOrder = std::move(merged);
    savePersistentState();
}

void removeSecondaryWindowFromPersistentState(std::uintptr_t key) {
    if (key == 0)
        return;

    bool changed = false;
    const auto oldOrderSize = g_secondaryWindowOrder.size();
    std::erase(g_secondaryWindowOrder, key);
    changed = changed || oldOrderSize != g_secondaryWindowOrder.size();

    changed = g_secondaryWindowWeights.erase(key) > 0 || changed;
    changed = g_secondaryFullscreenWindowKeys.erase(key) > 0 || changed;

    if (changed)
        savePersistentState();
}

bool validProfile(int profile) {
    return profile >= 1 && profile <= PRIMARY_PROFILE_COUNT;
}

bool movePrimaryWindowInOrder(const PHLWINDOW& window, int delta) {
    const auto key = windowKey(window);

    if (key == 0 || !g_primaryState.windowKeys.contains(key))
        return false;

    const auto profileIt = g_primaryState.windowProfiles.find(key);
    if (profileIt == g_primaryState.windowProfiles.end())
        return false;

    auto& profileData = g_primaryState.profiles[profileIt->second];
    if (profileData.fullscreenWindowKey != 0)
        return false;

    const bool moved = moveKeyOneSlot(profileData.order, key, delta);
    if (moved) {
        markInactivePrimaryProfilesDirty();
        savePersistentState();
    }

    return moved;
}

std::uintptr_t focusCandidateKeyForPrimaryProfile(int profile) {
    if (!validProfile(profile))
        return 0;

    const auto profileIt = g_primaryState.profiles.find(profile);
    if (profileIt == g_primaryState.profiles.end())
        return 0;

    const auto& profileData = profileIt->second;

    auto usableKey = [](std::uintptr_t key) -> std::uintptr_t {
        if (key == 0)
            return 0;

        const auto targetIt = g_primaryState.targets.find(key);
        if (targetIt == g_primaryState.targets.end() || !targetIt->second || !targetIt->second->window())
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

std::vector<std::uintptr_t> liveWindowKeysForPrimaryProfile(int profile) {
    std::vector<std::uintptr_t> keys;

    if (!validProfile(profile))
        return keys;

    const auto profileIt = g_primaryState.profiles.find(profile);
    if (profileIt == g_primaryState.profiles.end())
        return keys;

    const auto addIfUsable = [&](std::uintptr_t key) {
        if (key == 0 || std::ranges::find(keys, key) != keys.end())
            return;

        const auto targetIt = g_primaryState.targets.find(key);
        if (targetIt == g_primaryState.targets.end() || !targetIt->second || !targetIt->second->window())
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

void cancelPrimaryProfileTransition() {
    for (const auto key : g_primaryProfileTransition.fadingOutKeys) {
        const auto window = windowFromKey(key);
        if (!window)
            continue;

        resetProfileFadeAlpha(window);
        setProfileFadeInputBlocked(window, false);
    }

    for (const auto key : g_primaryProfileTransition.fadingInKeys) {
        const auto window = windowFromKey(key);
        if (!window)
            continue;

        resetProfileFadeAlpha(window);
        setProfileFadeInputBlocked(window, false);
    }

    g_primaryProfileTransition.fadingOutKeys.clear();
    g_primaryProfileTransition.fadingInKeys.clear();
}

void startPrimaryProfileFade(int fromPrimaryProfile, int toPrimaryProfile) {
    if (!PRIMARY_PROFILE_FADE_ENABLED || fromPrimaryProfile == toPrimaryProfile)
        return;

    cancelPrimaryProfileTransition();

    for (const auto key : liveWindowKeysForPrimaryProfile(toPrimaryProfile)) {
        const auto window = windowFromKey(key);
        if (!window)
            continue;

        g_primaryProfileTransition.fadingInKeys.insert(key);
        window->alpha(Desktop::View::WINDOW_ALPHA_LAYOUT)->setCallbackOnEnd(nullptr);
        window->alpha(Desktop::View::WINDOW_ALPHA_LAYOUT)->setValueAndWarp(PRIMARY_PROFILE_FADE_IN_START_ALPHA);
        setPrimaryProfileHiddenState(window, false);
        *window->alpha(Desktop::View::WINDOW_ALPHA_LAYOUT) = 1.F;
        window->alpha(Desktop::View::WINDOW_ALPHA_LAYOUT)->setCallbackOnEnd([key](auto) {
            const auto window = windowFromKey(key);
            if (window)
                resetProfileFadeAlpha(window);

            g_primaryProfileTransition.fadingInKeys.erase(key);
        });
    }

    for (const auto key : liveWindowKeysForPrimaryProfile(fromPrimaryProfile)) {
        const auto window = windowFromKey(key);
        if (!window)
            continue;

        g_primaryProfileTransition.fadingOutKeys.insert(key);
        setProfileFadeInputBlocked(window, true);
        window->alpha(Desktop::View::WINDOW_ALPHA_LAYOUT)->setCallbackOnEnd(nullptr);
        window->alpha(Desktop::View::WINDOW_ALPHA_LAYOUT)->setValueAndWarp(1.F);
        *window->alpha(Desktop::View::WINDOW_ALPHA_LAYOUT) = PRIMARY_PROFILE_FADE_OUT_END_ALPHA;
        window->alpha(Desktop::View::WINDOW_ALPHA_LAYOUT)->setCallbackOnEnd([key](auto) {
            const auto window = windowFromKey(key);
            if (!window)
                return;

            resetProfileFadeAlpha(window);
            setProfileFadeInputBlocked(window, false);
            g_primaryProfileTransition.fadingOutKeys.erase(key);

            const auto profileIt = g_primaryState.windowProfiles.find(key);
            if (profileIt != g_primaryState.windowProfiles.end() && profileIt->second != g_primaryState.activeProfile)
                setPrimaryProfileHiddenState(window, true);
        });
    }
}

void showPrimaryProfile(int profile, bool focusProfileWindow) {
    if (!validProfile(profile))
        return;

    setSpawnIntent(EFocusedSplitRegion::Primary, ESpawnIntentSource::PrimaryProfileSwitch);

    const auto fromPrimaryProfile = g_primaryState.activeProfile;
    const auto focusCandidateKey = focusProfileWindow ? focusCandidateKeyForPrimaryProfile(profile) : 0;

    if (fromPrimaryProfile != profile) {
        startPrimaryProfileFade(fromPrimaryProfile, profile);
        g_primaryState.activeProfile = profile;
        markInactivePrimaryProfilesDirty();
        savePersistentState();
        recalculateAllInstances();
    }

    if (focusCandidateKey != 0)
        focusWindowByKey(focusCandidateKey);
}

void revealPrimaryProfileForFocusedWindow(const PHLWINDOW& window) {
    if (!window)
        return;

    const auto key = windowKey(window);
    if (key == 0)
        return;

    const auto profileIt = g_primaryState.windowProfiles.find(key);
    if (profileIt == g_primaryState.windowProfiles.end() || !validProfile(profileIt->second))
        return;

    if (profileIt->second != g_primaryState.activeProfile)
        showPrimaryProfile(profileIt->second, false);
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

void setSpawnIntent(EFocusedSplitRegion region, ESpawnIntentSource source) {
    g_spawnIntent.region = region;
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

    g_spawnIntent.region = g_primaryState.windowKeys.contains(key) ? EFocusedSplitRegion::Primary : EFocusedSplitRegion::Secondary;
    g_spawnIntent.source = ESpawnIntentSource::FocusedWindow;
    g_spawnIntent.focusedWindowKeyWhenSourceSet = key;
}



std::optional<EFocusedSplitRegion> regionFromCursorForArea(const CBox& area) {
    if (!g_pInputManager || area.w <= 0.0 || area.h <= 0.0)
        return std::nullopt;

    const auto cursor = g_pInputManager->getMouseCoordsInternal();

    if (cursor.x < area.x || cursor.x >= area.x + area.w || cursor.y < area.y || cursor.y >= area.y + area.h)
        return std::nullopt;

    const double primaryHeight = std::floor(area.h * g_primaryRegionRatio);
    return cursor.y < area.y + primaryHeight ? EFocusedSplitRegion::Primary : EFocusedSplitRegion::Secondary;
}

SDispatchResult setSplitRatio(double ratio) {
    if (!std::isfinite(ratio))
        return {.success = false, .error = "splitrow: invalid primary region ratio"};

    if (ratio > 1.0)
        ratio /= 100.0;

    const double clamped = std::clamp(ratio, MIN_PRIMARY_REGION_RATIO, MAX_PRIMARY_REGION_RATIO);

    if (std::abs(g_primaryRegionRatio - clamped) < 0.0001)
        return {.success = true, .error = ""};

    g_primaryRegionRatio = clamped;
    markInactivePrimaryProfilesDirty();
    recalculateAllInstances();
    return {.success = true, .error = ""};
}


class CSplitRegionAlgorithm final : public Layout::ITiledAlgorithm {
  public:
    CSplitRegionAlgorithm() {
        g_instances.push_back(this);
    }

    ~CSplitRegionAlgorithm() override {
        clearSecondaryFullscreenState(true);
        std::erase(g_instances, this);
    }

    std::optional<std::string> layoutName() const override {
        return std::string{TILED_ALGO_NAME};
    }

    std::optional<EFocusedSplitRegion> regionFromCursorForCurrentSpace() const {
        const auto parent = m_parent.lock();

        if (!parent)
            return std::nullopt;

        const auto space = parent->space();

        if (!space)
            return std::nullopt;

        CBox cursorArea = space->workArea(false);
        if (const auto workspace = space->workspace(); workspace && workspace->m_monitor)
            cursorArea = workspace->m_monitor->logicalBoxMinusReserved();

        return regionFromCursorForArea(cursorArea);
    }

    void newTarget(SP<Layout::ITarget> target) override {
        // newTarget() must not infer intent from Hyprland's active window.
        // On blank workspace switches the active window can still be a sticky
        // primary-region window from the previous workspace. Spawn placement should
        // apply the latest explicit intent recorded by real events. When the
        // last intent came from focus, mouse position can refine the target
        // region at spawn time, which helps empty workspaces feel like real region
        // regions without continuously tracking pointer motion.
        auto spawnRegion = g_spawnIntent.region;
        const auto focusedInsertKey = g_spawnIntent.source == ESpawnIntentSource::FocusedWindow
            ? g_spawnIntent.focusedWindowKeyWhenSourceSet
            : 0;

        if (m_spawnFollowsFocusReady && g_spawnIntent.source == ESpawnIntentSource::FocusedWindow) {
            if (const auto cursorRegion = regionFromCursorForCurrentSpace())
                spawnRegion = *cursorRegion;
        }

        const bool shouldSpawnIntoPrimaryProfile = m_spawnFollowsFocusReady
            && spawnRegion == EFocusedSplitRegion::Primary;

        const bool restoredPrimaryState = addTarget(target);

        if (!restoredPrimaryState && shouldSpawnIntoPrimaryProfile && target && target->window()) {
            setWindowPrimaryProfile(target->window(), g_primaryState.activeProfile, focusedInsertKey);
            return;
        }

        if (!restoredPrimaryState && target && target->window())
            insertSecondaryTargetAfterKey(target, focusedInsertKey);

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

            if (key != 0 && key == m_secondaryFullscreen.windowKey && closingOrDead)
                clearSecondaryFullscreenState(false);

            // removeTarget is also called when a pinned primary-region window is
            // detached from a workspace during workspace changes. In that
            // case the window is still mapped and must remain in the sticky
            // primary-region state. Only clear the global primary-region state when the
            // window is actually no longer mapped.
            if (key != 0 && g_primaryState.windowKeys.contains(key)) {
                if (closingOrDead) {
                    if (clearPrimaryWindowState(target->window()))
                        savePersistentState();
                }
            }
        }

        if (target && target->window()) {
            const auto key = windowKey(target->window());
            if (key != 0 && !g_primaryState.windowKeys.contains(key)) {
                m_secondaryOrderBeforeLastRemove = secondaryWindowKeysInOrder();
                removeSecondaryWindowFromPersistentState(key);
            }
        }

        std::erase_if(m_targets, [&](const auto& other) { return other == target; });
        saveSecondaryWindowOrder(secondaryWindowKeysInOrder());
        recalculateAllInstances();
    }

    void resizeTarget(const Vector2D& delta, SP<Layout::ITarget> target, Layout::eRectCorner corner = Layout::CORNER_NONE) override {
        (void)corner;

        if (!target || !target->window() || std::abs(delta.x) < 1.0)
            return;

        resizeWindowInRegion(target->window(), delta.x > 0.0 ? 1 : -1);
    }

    void recalculate(Layout::eRecalculateReason reason = Layout::RECALCULATE_REASON_UNKNOWN) override {
        (void)reason;
        placeTargetsInRegions();
    }

    SP<Layout::ITarget> getNextCandidate(SP<Layout::ITarget> old) override {
        if (old && old->window()) {
            const auto key = windowKey(old->window());

            if (auto primaryCandidate = primaryProfileFocusCandidateAfterRemoving(key))
                return primaryCandidate;

            if (auto secondaryCandidate = secondaryFocusCandidateAfterRemoving(key))
                return secondaryCandidate;
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
        saveSecondaryWindowOrder(secondaryWindowKeysInOrder());
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

        if (key != 0 && g_primaryState.windowKeys.contains(key)) {
            if (movePrimaryWindowInOrder(target->window(), delta)) {
                recalculateAllInstances();
            }

            return;
        }

        moveWindowInSecondaryRegion(target->window(), delta);
    }

    bool setWindowPrimaryProfile(const PHLWINDOW& window, int profile, std::uintptr_t insertAfterKey = 0) {
        if (!window || !validProfile(profile))
            return false;

        const auto target = targetForWindow(window);
        if (!target)
            return false;

        const auto key = windowKey(window);
        if (key == 0)
            return false;

        if (m_secondaryFullscreen.windowKey == key)
            clearSecondaryFullscreenState(true);

        clearPrimaryFullscreenForWindow(key, true);

        const bool wasPrimary = g_primaryState.windowKeys.contains(key);
        const int oldProfile = g_primaryState.windowProfiles.contains(key) ? g_primaryState.windowProfiles[key] : 0;

        if (wasPrimary && oldProfile != profile)
            std::erase(g_primaryState.profiles[oldProfile].order, key);

        g_primaryState.windowKeys.insert(key);
        g_primaryState.windowProfiles[key] = profile;
        g_primaryState.targets[key] = target;
        g_secondaryWindowWeights.erase(key);
        g_secondaryFullscreenWindowKeys.erase(key);
        std::erase(g_secondaryWindowOrder, key);

        auto& profileData = g_primaryState.profiles[profile];
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
        markInactivePrimaryProfilesDirty();

        savePersistentState();
        recalculateAllInstances();
        return true;
    }

    bool setWindowPrimaryState(const PHLWINDOW& window, bool inPrimaryRegion) {
        if (inPrimaryRegion)
            return setWindowPrimaryProfile(window, g_primaryState.activeProfile);

        if (!window)
            return false;

        const auto key = windowKey(window);
        if (key == 0)
            return false;

        clearPrimaryFullscreenForWindow(key, true);

        g_primaryState.windowKeys.erase(key);
        g_primaryState.windowProfiles.erase(key);
        g_primaryState.targets.erase(key);
        for (auto& [profile, profileData] : g_primaryState.profiles) {
            std::erase(profileData.order, key);
            profileData.weights.erase(key);
            profileData.lastBoxes.erase(key);
        }
        if (!g_secondaryWindowWeights.contains(key))
            g_secondaryWindowWeights[key] = 1.0;

        saveSecondaryWindowOrder(secondaryWindowKeysInOrder());

        setPrimaryProfileHiddenState(window, false);
        window->m_pinned = false;
        markInactivePrimaryProfilesDirty();

        savePersistentState();
        recalculateAllInstances();
        return true;
    }

    bool toggleWindowPrimaryState(const PHLWINDOW& window) {
        if (!window)
            return false;

        const auto key = windowKey(window);
        if (key == 0)
            return false;

        const bool currentlyPrimary = g_primaryState.windowKeys.contains(key);
        return setWindowPrimaryState(window, !currentlyPrimary);
    }

    bool toggleSecondaryFullscreen(const PHLWINDOW& window) {
        if (!window)
            return false;

        const auto key = windowKey(window);
        if (key == 0 || g_primaryState.windowKeys.contains(key))
            return false;

        if (!targetForWindow(window))
            return false;

        if (m_secondaryFullscreen.windowKey == key) {
            clearSecondaryFullscreenState(true);
        } else {
            clearSecondaryFullscreenState(true);
            m_secondaryFullscreen.windowKey = key;
            g_secondaryFullscreenWindowKeys.insert(key);
            setRegionFullscreenVisualState(window, true);
        }

        savePersistentState();
        recalculateAllInstances();
        return true;
    }

    bool togglePrimaryFullscreen(const PHLWINDOW& window) {
        if (!window)
            return false;

        const auto key = windowKey(window);
        if (key == 0 || !g_primaryState.windowKeys.contains(key))
            return false;

        const auto profileIt = g_primaryState.windowProfiles.find(key);
        if (profileIt == g_primaryState.windowProfiles.end() || !validProfile(profileIt->second))
            return false;

        auto& profileData = g_primaryState.profiles[profileIt->second];

        if (profileData.fullscreenWindowKey == key) {
            setRegionFullscreenVisualState(window, false);
            profileData.fullscreenWindowKey = 0;
        } else {
            clearPrimaryFullscreenForWindow(profileData.fullscreenWindowKey, true);
            profileData.fullscreenWindowKey = key;
            setRegionFullscreenVisualState(window, true);
        }

        markInactivePrimaryProfilesDirty();
        savePersistentState();
        recalculateAllInstances();
        return true;
    }

    bool moveWindowInSecondaryRegion(const PHLWINDOW& window, int delta) {
        if (!window || delta == 0)
            return false;

        if (m_secondaryFullscreen.windowKey != 0)
            return false;

        const auto key = windowKey(window);

        if (key == 0 || g_primaryState.windowKeys.contains(key))
            return false;

        std::vector<std::size_t> secondaryPositions;

        for (std::size_t i = 0; i < m_targets.size(); ++i) {
            const auto& target = m_targets[i];
            const auto targetKey = windowKey(target ? target->window() : nullptr);

            if (targetKey != 0 && !g_primaryState.windowKeys.contains(targetKey))
                secondaryPositions.push_back(i);
        }

        for (std::size_t regionIndex = 0; regionIndex < secondaryPositions.size(); ++regionIndex) {
            const auto actualIndex = secondaryPositions[regionIndex];
            const auto& target = m_targets[actualIndex];

            if (!target || target->window() != window)
                continue;

            if (delta < 0) {
                if (regionIndex == 0)
                    return false;

                std::swap(m_targets[actualIndex], m_targets[secondaryPositions[regionIndex - 1]]);
                saveSecondaryWindowOrder(secondaryWindowKeysInOrder());
                recalculate(Layout::RECALCULATE_REASON_UNKNOWN);
                return true;
            }

            if (regionIndex + 1 >= secondaryPositions.size())
                return false;

            std::swap(m_targets[actualIndex], m_targets[secondaryPositions[regionIndex + 1]]);
            saveSecondaryWindowOrder(secondaryWindowKeysInOrder());
            recalculate(Layout::RECALCULATE_REASON_UNKNOWN);
            return true;
        }

        return false;
    }

    bool resizeWindowInRegion(const PHLWINDOW& window, int delta) {
        if (!window || delta == 0)
            return false;

        const auto key = windowKey(window);
        if (key == 0)
            return false;

        if (g_primaryState.windowKeys.contains(key)) {
            const auto profileIt = g_primaryState.windowProfiles.find(key);
            if (profileIt == g_primaryState.windowProfiles.end())
                return false;

            auto& profileData = g_primaryState.profiles[profileIt->second];
            if (profileData.fullscreenWindowKey != 0)
                return false;

            const bool initializedWeights = ensureWeightsForOrder(profileData.order, profileData.weights);
            const bool changed = resizeKeyInOrder(profileData.order, key, delta, profileData.weights);
            if (changed || initializedWeights) {
                markInactivePrimaryProfilesDirty();
                savePersistentState();
                recalculateAllInstances();
            }

            return changed;
        }

        if (m_secondaryFullscreen.windowKey != 0)
            return false;

        const auto secondaryKeys = secondaryWindowKeysInOrder();
        const bool initializedWeights = ensureWeightsForOrder(secondaryKeys, g_secondaryWindowWeights);
        const bool changed = resizeKeyInOrder(secondaryKeys, key, delta, g_secondaryWindowWeights);
        if (changed || initializedWeights) {
            savePersistentState();
            recalculate(Layout::RECALCULATE_REASON_UNKNOWN);
        }

        return changed;
    }

    bool containsWindow(const PHLWINDOW& window) const {
        return targetForWindow(window) != nullptr;
    }

    bool hasSecondaryFullscreen() const {
        return m_secondaryFullscreen.windowKey != 0;
    }

    bool clearSecondaryFullscreenForWindow(std::uintptr_t key, bool restoreVisuals) {
        if (key == 0 || key != m_secondaryFullscreen.windowKey)
            return false;

        clearSecondaryFullscreenState(restoreVisuals);
        return true;
    }

    SP<Layout::ITarget> secondaryFocusCandidateAfterRemoving(std::uintptr_t key) const {
        if (key == 0 || g_primaryState.windowKeys.contains(key))
            return nullptr;

        const auto targetForKey = [&](std::uintptr_t candidateKey) -> SP<Layout::ITarget> {
            if (candidateKey == 0 || candidateKey == key || g_primaryState.windowKeys.contains(candidateKey))
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

        if (auto target = candidateFromOrder(secondaryWindowKeysInOrder()))
            return target;

        if (auto target = candidateFromOrder(m_secondaryOrderBeforeLastRemove))
            return target;

        return nullptr;
    }

    void restoreSecondaryFullscreenFromBind() {
        clearSecondaryFullscreenState(true);
        savePersistentState();
        m_secondaryFullscreen.forceRestoreSpaceUpdate = true;
        recalculateAllInstances();
    }

    void finishRecalculate() {
        m_secondaryFullscreen.forceRestoreSpaceUpdate = false;
    }

    void clearSecondaryFullscreenOnExit() {
        clearSecondaryFullscreenState(true);
    }

  private:
    std::vector<SP<Layout::ITarget>> m_targets;
    std::vector<std::uintptr_t> m_secondaryOrderBeforeLastRemove;
    SSecondaryFullscreenState m_secondaryFullscreen;
    bool m_spawnFollowsFocusReady = false;

    std::vector<std::uintptr_t> secondaryWindowKeysInOrder() const {
        std::vector<std::uintptr_t> keys;

        for (const auto& target : m_targets) {
            const auto key = windowKey(target ? target->window() : nullptr);
            if (key != 0 && !g_primaryState.windowKeys.contains(key))
                keys.push_back(key);
        }

        return keys;
    }

    void applyPersistentSecondaryOrder() {
        if (g_secondaryWindowOrder.empty() || m_targets.size() < 2)
            return;

        std::stable_sort(m_targets.begin(), m_targets.end(), [](const auto& left, const auto& right) {
            const auto leftKey = windowKey(left ? left->window() : nullptr);
            const auto rightKey = windowKey(right ? right->window() : nullptr);
            const bool leftIsSecondary = leftKey != 0 && !g_primaryState.windowKeys.contains(leftKey);
            const bool rightIsSecondary = rightKey != 0 && !g_primaryState.windowKeys.contains(rightKey);

            if (leftIsSecondary != rightIsSecondary)
                return !leftIsSecondary;

            if (!leftIsSecondary || !rightIsSecondary)
                return false;

            const auto leftIt = std::ranges::find(g_secondaryWindowOrder, leftKey);
            const auto rightIt = std::ranges::find(g_secondaryWindowOrder, rightKey);
            const bool leftKnown = leftIt != g_secondaryWindowOrder.end();
            const bool rightKnown = rightIt != g_secondaryWindowOrder.end();

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

    void clearSecondaryFullscreenState(bool restoreVisuals) {
        if (restoreVisuals)
            setRegionFullscreenVisualState(windowFromKey(m_secondaryFullscreen.windowKey), false);

        g_secondaryFullscreenWindowKeys.erase(m_secondaryFullscreen.windowKey);
        m_secondaryFullscreen.windowKey = 0;
    }


    bool addTarget(const SP<Layout::ITarget>& target) {
        if (!target)
            return false;

        if (std::ranges::find(m_targets, target) == m_targets.end())
            m_targets.push_back(target);

        const bool restoredPrimary = restorePrimaryStateFromPersistence(target);
        if (!restoredPrimary) {
            const auto key = windowKey(target->window());
            if (key != 0) {
                if (!g_secondaryWindowWeights.contains(key))
                    g_secondaryWindowWeights[key] = 1.0;

                if (g_secondaryFullscreenWindowKeys.contains(key)) {
                    m_secondaryFullscreen.windowKey = key;
                    setRegionFullscreenVisualState(target->window(), true);
                }
            }
        }

        applyPersistentSecondaryOrder();
        return restoredPrimary;
    }

    void insertSecondaryTargetAfterKey(const SP<Layout::ITarget>& target, std::uintptr_t insertAfterKey) {
        if (!target || insertAfterKey == 0)
            return;

        const auto key = windowKey(target->window());
        if (key == 0 || g_primaryState.windowKeys.contains(key))
            return;

        const auto focusedIt = std::ranges::find_if(m_targets, [&](const auto& other) {
            const auto otherKey = windowKey(other ? other->window() : nullptr);
            return otherKey == insertAfterKey && !g_primaryState.windowKeys.contains(otherKey);
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
            return otherKey == insertAfterKey && !g_primaryState.windowKeys.contains(otherKey);
        });

        if (focusedAgainIt == m_targets.end()) {
            m_targets.push_back(movedTarget);
            return;
        }

        m_targets.insert(std::next(focusedAgainIt), movedTarget);
        saveSecondaryWindowOrder(secondaryWindowKeysInOrder());
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

    void pruneDeadPrimaryTargets() {
        std::vector<std::uintptr_t> staleKeys;

        for (const auto& [key, target] : g_primaryState.targets) {
            if (!target || windowIsClosingOrDead(target->window()))
                staleKeys.push_back(key);
        }

        for (const auto key : staleKeys) {
            g_primaryState.windowKeys.erase(key);
            g_primaryState.windowProfiles.erase(key);
            g_primaryState.targets.erase(key);

            for (auto& [profile, profileData] : g_primaryState.profiles) {
                std::erase(profileData.order, key);
                profileData.weights.erase(key);
                profileData.lastBoxes.erase(key);
                if (profileData.fullscreenWindowKey == key)
                    profileData.fullscreenWindowKey = 0;
            }
        }

        if (!staleKeys.empty()) {
            for (const auto key : staleKeys)
                g_primaryState.hiddenWindowKeys.erase(key);
            markInactivePrimaryProfilesDirty();
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

    static std::vector<SPlacedTarget> calculateColumnRegionBoxes(
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

    static void placeColumnRegion(
        const std::vector<SP<Layout::ITarget>>& targets,
        const CBox& area,
        const std::unordered_map<std::uintptr_t, double>& weights,
        std::unordered_map<std::uintptr_t, CBox>* savedBoxes = nullptr
    ) {
        const auto placements = calculateColumnRegionBoxes(targets, area, weights);

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

    void placeTargetsInRegions() {
        const auto parent = m_parent.lock();

        if (!parent)
            return;

        const auto space = parent->space();

        if (!space)
            return;

        pruneDeadPrimaryTargets();

        // Do not drop targets just because their current geometry is outside
        // this space. Secondary fullscreen collapses non-focused secondary targets;
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

        const double primaryHeight = std::floor(workArea.h * g_primaryRegionRatio);
        const CBox primaryArea{
            workArea.x,
            workArea.y,
            workArea.w,
            primaryHeight,
        };
        const CBox secondaryArea{
            workArea.x,
            workArea.y + primaryHeight,
            workArea.w,
            workArea.h - primaryHeight,
        };

        const double fullscreenPrimaryHeight = std::floor(fullscreenWorkArea.h * g_primaryRegionRatio);
        const CBox fullscreenPrimaryArea{
            fullscreenWorkArea.x,
            fullscreenWorkArea.y,
            fullscreenWorkArea.w,
            fullscreenPrimaryHeight,
        };
        const CBox fullscreenSecondaryArea{
            fullscreenWorkArea.x,
            fullscreenWorkArea.y + fullscreenPrimaryHeight,
            fullscreenWorkArea.w,
            fullscreenWorkArea.h - fullscreenPrimaryHeight,
        };

        std::vector<SP<Layout::ITarget>> primaryTargets;
        std::vector<SP<Layout::ITarget>> secondaryTargets;

        auto& activeOrder = g_primaryState.profiles[g_primaryState.activeProfile].order;

        for (const auto key : activeOrder) {
            const auto it = g_primaryState.targets.find(key);

            if (it == g_primaryState.targets.end())
                continue;

            const auto& target = it->second;

            if (target && target->window() && !windowIsClosingOrDead(target->window()))
                primaryTargets.push_back(target);
        }

        for (const auto& target : m_targets) {
            if (!target)
                continue;

            const auto key = windowKey(target->window());

            if (key == 0 || !g_primaryState.windowKeys.contains(key))
                secondaryTargets.push_back(target);
        }

        auto& activeProfileData = g_primaryState.profiles[g_primaryState.activeProfile];
        if (activeProfileData.fullscreenWindowKey != 0) {
            auto fullscreenIt = std::ranges::find_if(primaryTargets, [&](const auto& target) {
                return target && windowKey(target->window()) == activeProfileData.fullscreenWindowKey;
            });

            if (fullscreenIt != primaryTargets.end()) {
                for (const auto& target : primaryTargets) {
                    if (!target || !target->window())
                        continue;

                    if (target == *fullscreenIt)
                        setPrimaryProfileHiddenState(target->window(), false);
                    else
                        setPrimaryProfileHiddenState(target->window(), true);
                }

                if ((*fullscreenIt)->window())
                    setRegionFullscreenVisualState((*fullscreenIt)->window(), true);

                placeSingleTargetNoGaps(*fullscreenIt, fullscreenPrimaryArea);
                activeProfileData.lastBoxes[activeProfileData.fullscreenWindowKey] = fullscreenPrimaryArea;
            } else {
                setRegionFullscreenVisualState(windowFromKey(activeProfileData.fullscreenWindowKey), false);
                activeProfileData.fullscreenWindowKey = 0;
                markInactivePrimaryProfilesDirty();
                savePersistentState();

                for (const auto& target : primaryTargets) {
                    if (target && target->window())
                        setPrimaryProfileHiddenState(target->window(), false);
                }

                placeColumnRegion(primaryTargets, primaryArea, activeProfileData.weights, &activeProfileData.lastBoxes);
            }
        } else {
            for (const auto& target : primaryTargets) {
                if (target && target->window())
                    setPrimaryProfileHiddenState(target->window(), false);
            }

            placeColumnRegion(primaryTargets, primaryArea, activeProfileData.weights, &activeProfileData.lastBoxes);
        }

        // Inactive primary profiles are hidden rather than parked off-screen.
        // Off-screen tiling makes Hyprland apply different edge/gap handling,
        // which changes Chromium/Electron client buffers by a few pixels.
        // Keeping hidden profiles in their real primary-region geometry avoids that
        // resize path entirely.

        // Secondary fullscreen is different: keep the other secondary targets inside
        // the current layout space with a tiny geometry. Parking them far
        // off-screen can make Hyprland detach them from this layout space, so
        // restoring fullscreen leaves only the fullscreen target until another
        // pointer drag forces the layout target list to update.
        const CBox collapsedSecondaryArea{
            secondaryArea.x + std::max(0.0, secondaryArea.w - 1.0),
            secondaryArea.y + std::max(0.0, secondaryArea.h - 1.0),
            1.0,
            1.0,
        };

        if (m_secondaryFullscreen.windowKey != 0) {
            auto fullscreenIt = std::ranges::find_if(secondaryTargets, [this](const auto& target) {
                return target && windowKey(target->window()) == m_secondaryFullscreen.windowKey;
            });

            if (fullscreenIt != secondaryTargets.end()) {
                // Secondary fullscreen is a real secondary-region layout mode: only the
                // fullscreen target owns the visible secondary region. Other secondary
                // targets remain managed and keep their region/order state, but
                // are collapsed until fullscreen is restored. This avoids
                // hover-focus leaking through to tiled windows underneath.
                placeSingleTargetNoGaps(*fullscreenIt, fullscreenSecondaryArea);

                for (const auto& target : secondaryTargets) {
                    if (!target || target == *fullscreenIt)
                        continue;

                    target->setPositionGlobal(collapsedSecondaryArea);
                    target->damageEntire();
                    target->warpPositionSize();
                }
            } else {
                clearSecondaryFullscreenState(true);
                placeColumnRegion(secondaryTargets, secondaryArea, g_secondaryWindowWeights);
            }
        } else {
            placeColumnRegion(secondaryTargets, secondaryArea, g_secondaryWindowWeights);

            if (m_secondaryFullscreen.forceRestoreSpaceUpdate) {
                for (const auto& target : secondaryTargets) {
                    if (!target)
                        continue;

                    target->onUpdateSpace();
                    target->damageEntire();
                    target->warpPositionSize();
                }
            }
        }

        // Keep inactive primary profiles hidden, but avoid relaying them out on
        // every recalculation. They only need fresh geometry when the primary area,
        // profile membership, order, or resize weights change.
        const bool inactivePrimaryAreaChanged = !g_primaryState.inactiveProfilesPrimaryArea
            || !boxesNearlyEqual(*g_primaryState.inactiveProfilesPrimaryArea, primaryArea);
        const bool relayoutInactiveProfiles = g_primaryState.inactiveProfilesDirty || inactivePrimaryAreaChanged;

        for (int profile = 1; profile <= PRIMARY_PROFILE_COUNT; ++profile) {
            if (profile == g_primaryState.activeProfile)
                continue;

            const auto profileIt = g_primaryState.profiles.find(profile);
            if (profileIt == g_primaryState.profiles.end())
                continue;

            auto& profileData = profileIt->second;
            std::vector<SP<Layout::ITarget>> hiddenPrimaryTargets;
            bool missingSavedBox = false;

            for (const auto key : profileData.order) {
                const auto targetIt = g_primaryState.targets.find(key);
                if (targetIt == g_primaryState.targets.end())
                    continue;

                const auto& target = targetIt->second;
                if (!target || !target->window() || windowIsClosingOrDead(target->window()))
                    continue;

                hiddenPrimaryTargets.push_back(target);

                const auto savedBoxIt = profileData.lastBoxes.find(key);
                if (savedBoxIt == profileData.lastBoxes.end() || savedBoxIt->second.w <= 0.0 || savedBoxIt->second.h <= 0.0)
                    missingSavedBox = true;
            }

            if (relayoutInactiveProfiles || missingSavedBox) {
                if (profileData.fullscreenWindowKey != 0) {
                    auto fullscreenIt = std::ranges::find_if(hiddenPrimaryTargets, [&](const auto& target) {
                        return target && windowKey(target->window()) == profileData.fullscreenWindowKey;
                    });

                    if (fullscreenIt != hiddenPrimaryTargets.end()) {
                        if ((*fullscreenIt)->window())
                            setRegionFullscreenVisualState((*fullscreenIt)->window(), true);

                        placeSingleTargetNoGaps(*fullscreenIt, fullscreenPrimaryArea);
                        profileData.lastBoxes[profileData.fullscreenWindowKey] = fullscreenPrimaryArea;
                    } else {
                        setRegionFullscreenVisualState(windowFromKey(profileData.fullscreenWindowKey), false);
                        profileData.fullscreenWindowKey = 0;
                        savePersistentState();
                    }
                }

                const auto placements = calculateColumnRegionBoxes(hiddenPrimaryTargets, primaryArea, profileData.weights);

                for (const auto& placement : placements) {
                    const auto key = windowKey(placement.target ? placement.target->window() : nullptr);

                    if (key != 0 && key != profileData.fullscreenWindowKey)
                        profileData.lastBoxes[key] = placement.box;

                    if (key != profileData.fullscreenWindowKey)
                        applyPlacement(placement.target, placement.box);

                    if (placement.target && placement.target->window())
                        setPrimaryProfileHiddenState(placement.target->window(), true);
                }
            } else {
                for (const auto& target : hiddenPrimaryTargets) {
                    if (target && target->window())
                        setPrimaryProfileHiddenState(target->window(), true);
                }
            }
        }

        g_primaryState.inactiveProfilesDirty = false;
        g_primaryState.inactiveProfilesPrimaryArea = primaryArea;

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

    std::uintptr_t primaryFocusFallbackKey = 0;
    if (g_primaryState.windowKeys.contains(key)) {
        if (const auto fallbackTarget = primaryProfileFocusCandidateAfterRemoving(key); fallbackTarget && fallbackTarget->window())
            primaryFocusFallbackKey = windowKey(fallbackTarget->window());
    }

    bool changed = false;
    const auto oldSecondaryOrderSize = g_secondaryWindowOrder.size();
    std::erase(g_secondaryWindowOrder, key);
    changed = oldSecondaryOrderSize != g_secondaryWindowOrder.size() || changed;
    changed = g_secondaryWindowWeights.erase(key) > 0 || changed;
    changed = g_secondaryFullscreenWindowKeys.erase(key) > 0 || changed;

    for (auto* instance : g_instances) {
        if (instance && instance->clearSecondaryFullscreenForWindow(key, false))
            changed = true;
    }

    if (g_primaryState.windowKeys.contains(key))
        changed = clearPrimaryWindowState(window) || changed;

    if (changed) {
        savePersistentState();
        recalculateAllInstances();

        if (primaryFocusFallbackKey != 0)
            focusWindowByKey(primaryFocusFallbackKey);
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
        // sticky primary window. That is not a new user focus choice and must not
        // override the workspace.active event's secondary-region spawn intent.
        if (reason == Desktop::FOCUS_REASON_WORKSPACE_CHANGE)
            return;

        revealPrimaryProfileForFocusedWindow(window);
        updateSpawnIntentFromFocusedWindow(window);
    });

    g_workspaceActiveListener = Event::bus()->m_events.workspace.active.listen([](auto) {
        setSpawnIntent(EFocusedSplitRegion::Secondary, ESpawnIntentSource::WorkspaceSwitch);
    });

    return g_windowCloseListener && g_windowDestroyListener && g_windowActiveListener && g_workspaceActiveListener;
}

void unregisterEventListeners() {
    g_windowCloseListener.reset();
    g_windowDestroyListener.reset();
    g_windowActiveListener.reset();
    g_workspaceActiveListener.reset();
}

CSplitRegionAlgorithm* algorithmForWindow(const PHLWINDOW& window) {
    if (!window)
        return nullptr;

    for (auto* instance : g_instances) {
        if (instance && instance->containsWindow(window))
            return instance;
    }

    return nullptr;
}

SDispatchResult activeSplitRegionWindow(SActiveSplitRegionWindow& active) {
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

SDispatchResult setActiveWindowPrimaryProfile(int profile) {
    if (!validProfile(profile))
        return {.success = false, .error = "splitrow: invalid primary profile"};

    SActiveSplitRegionWindow active;
    if (const auto result = activeSplitRegionWindow(active); !result.success)
        return result;

    if (!active.algorithm->setWindowPrimaryProfile(active.window, profile))
        return {.success = false, .error = "splitrow: failed to update active window primary profile"};

    return {.success = true, .error = ""};
}


SDispatchResult setActiveWindowPrimaryState(bool inPrimaryRegion) {
    SActiveSplitRegionWindow active;
    if (const auto result = activeSplitRegionWindow(active); !result.success)
        return result;

    if (!active.algorithm->setWindowPrimaryState(active.window, inPrimaryRegion))
        return {.success = false, .error = "splitrow: failed to update active window region"};

    return {.success = true, .error = ""};
}


SDispatchResult showPrimaryProfileResult(int profile) {
    if (!validProfile(profile))
        return {.success = false, .error = "splitrow: invalid primary profile"};

    showPrimaryProfile(profile);
    return {.success = true, .error = ""};
}

SDispatchResult toggleActiveWindowPrimaryState() {
    SActiveSplitRegionWindow active;
    if (const auto result = activeSplitRegionWindow(active); !result.success)
        return result;

    if (!active.algorithm->toggleWindowPrimaryState(active.window))
        return {.success = false, .error = "splitrow: failed to toggle active window region"};

    return {.success = true, .error = ""};
}


SDispatchResult toggleActiveFocusedFullscreen() {
    SActiveSplitRegionWindow active;
    if (const auto result = activeSplitRegionWindow(active); !result.success)
        return result;

    if (active.key != 0 && g_primaryState.windowKeys.contains(active.key)) {
        if (!active.algorithm->togglePrimaryFullscreen(active.window))
            return {.success = false, .error = "splitrow: failed to toggle primary fullscreen"};

        return {.success = true, .error = ""};
    }

    if (active.algorithm->hasSecondaryFullscreen()) {
        active.algorithm->restoreSecondaryFullscreenFromBind();
        return {.success = true, .error = ""};
    }

    if (!active.algorithm->toggleSecondaryFullscreen(active.window))
        return {.success = false, .error = "splitrow: failed to toggle secondary fullscreen"};

    return {.success = true, .error = ""};
}


SDispatchResult resizeActiveWindowByWeight(int delta) {
    SActiveSplitRegionWindow active;
    if (const auto result = activeSplitRegionWindow(active); !result.success)
        return result;

    if (!active.algorithm->resizeWindowInRegion(active.window, delta))
        return {.success = false, .error = "splitrow: active region cannot be resized"};

    return {.success = true, .error = ""};
}


SDispatchResult moveActiveWindowHorizontally(int delta) {
    SActiveSplitRegionWindow active;
    if (const auto result = activeSplitRegionWindow(active); !result.success)
        return result;

    if (active.key != 0 && g_primaryState.windowKeys.contains(active.key)) {
        if (!movePrimaryWindowInOrder(active.window, delta))
            return {.success = true, .error = ""};

        recalculateAllInstances();
        return {.success = true, .error = ""};
    }

    active.algorithm->moveWindowInSecondaryRegion(active.window, delta);
    return {.success = true, .error = ""};
}


SDispatchResult releaseActiveWindowFromSplitRegionState() {
    SActiveSplitRegionWindow active;
    if (const auto result = activeSplitRegionWindow(active); !result.success)
        return result;

    if (active.key == 0 || !g_primaryState.windowKeys.contains(active.key))
        return {.success = true, .error = ""};

    // Native Hyprland workspace move dispatchers do not know about splitrow
    // primary profiles. Primary-profile windows are pinned so inactive profiles can
    // survive workspace switches, which means a normal movetoworkspace bind can
    // leave the focused primary-profile window stuck in splitrow state. Release it
    // first, then let the user's normal workspace dispatcher move it.
    if (!clearPrimaryWindowState(active.window))
        return {.success = true, .error = ""};

    if (!g_secondaryWindowWeights.contains(active.key))
        g_secondaryWindowWeights[active.key] = 1.0;

    savePersistentState();
    recalculateAllInstances();
    return {.success = true, .error = ""};
}


SDispatchResult moveActiveWindowToWorkspace(int workspace) {
    if (workspace <= 0)
        return {.success = false, .error = "splitrow: invalid workspace"};

    // Release primary-profile state before using Hyprland's own workspace move
    // dispatcher. This keeps workspace move compatibility internal to the plugin.
    if (const auto releaseResult = releaseActiveWindowFromSplitRegionState(); !releaseResult.success)
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


int luaMovePrimary(lua_State*) {
    const auto result = setActiveWindowPrimaryState(true);

    if (!result.success && !result.error.empty())
        notify(result.error, CHyprColor{1.0F, 0.35F, 0.2F, 1.0F});

    return 0;
}

int luaMoveSecondary(lua_State*) {
    const auto result = setActiveWindowPrimaryState(false);

    if (!result.success && !result.error.empty())
        notify(result.error, CHyprColor{1.0F, 0.35F, 0.2F, 1.0F});

    return 0;
}

int luaToggleRegion(lua_State*) {
    const auto result = toggleActiveWindowPrimaryState();

    if (!result.success && !result.error.empty())
        notify(result.error, CHyprColor{1.0F, 0.35F, 0.2F, 1.0F});

    return 0;
}

int luaReleaseActive(lua_State*) {
    const auto result = releaseActiveWindowFromSplitRegionState();

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


int luaSetSplitRatio(lua_State* state) {
    if (!state) {
        notify("splitrow: setsplitratio expects a number", CHyprColor{1.0F, 0.35F, 0.2F, 1.0F});
        return 0;
    }

    int isNumber = 0;
    const auto ratio = lua_tonumberx(state, 1, &isNumber);

    if (!isNumber) {
        notify("splitrow: setsplitratio expects a number", CHyprColor{1.0F, 0.35F, 0.2F, 1.0F});
        return 0;
    }

    const auto result = setSplitRatio(ratio);

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
        notify("splitrow: invalid primary profile", CHyprColor{1.0F, 0.35F, 0.2F, 1.0F});
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

int luaShowPrimaryProfile(lua_State* state) {
    const auto profile = luaProfileArgument(state, "showprimaryprofile");
    if (!profile)
        return 0;

    const auto result = showPrimaryProfileResult(*profile);
    if (!result.success && !result.error.empty())
        notify(result.error, CHyprColor{1.0F, 0.35F, 0.2F, 1.0F});
    return 0;
}

int luaSendToPrimaryProfile(lua_State* state) {
    const auto profile = luaProfileArgument(state, "sendprimaryprofile");
    if (!profile)
        return 0;

    const auto result = setActiveWindowPrimaryProfile(*profile);
    if (!result.success && !result.error.empty())
        notify(result.error, CHyprColor{1.0F, 0.35F, 0.2F, 1.0F});
    return 0;
}

bool registerLuaFunctions() {
    bool ok = true;

    auto add = [&](const std::string& name, PLUGIN_LUA_FN fn) {
        ok = HyprlandAPI::addLuaFunction(g_pluginHandle, "splitrow", name, fn) && ok;
    };

    add("moveprimary", luaMovePrimary);
    add("movesecondary", luaMoveSecondary);
    add("toggleregion", luaToggleRegion);
    add("releaseactive", luaReleaseActive);
    add("movetoworkspace", luaMoveToWorkspace);
    add("togglefocusedfullscreen", luaToggleFocusedFullscreen);
    add("moveleft", luaMoveLeft);
    add("moveright", luaMoveRight);
    add("shrinkfocused", luaShrinkFocused);
    add("growfocused", luaGrowFocused);
    add("setsplitratio", luaSetSplitRatio);
    add("showprimaryprofile", luaShowPrimaryProfile);
    add("sendprimaryprofile", luaSendToPrimaryProfile);

    return ok;
}


void unregisterLuaFunctions() {
    if (!g_pluginHandle)
        return;

    auto remove = [&](const std::string& name) {
        HyprlandAPI::removeLuaFunction(g_pluginHandle, "splitrow", name);
    };

    remove("moveprimary");
    remove("movesecondary");
    remove("toggleregion");
    remove("releaseactive");
    remove("movetoworkspace");
    remove("togglefocusedfullscreen");
    remove("moveleft");
    remove("moveright");
    remove("shrinkfocused");
    remove("growfocused");
    remove("setsplitratio");
    remove("showprimaryprofile");
    remove("sendprimaryprofile");
}


bool registerAlgorithms() {
    return HyprlandAPI::addTiledAlgo(
        g_pluginHandle,
        TILED_ALGO_NAME,
        &typeid(CSplitRegionAlgorithm),
        []() -> UP<Layout::ITiledAlgorithm> {
            return makeUnique<CSplitRegionAlgorithm>();
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
        "Two-region split layout for Hyprland with primary profiles, focused region fullscreen, focus-following spawns, and configurable split ratio.",
        "Sarah Mac Carthy + ChatGPT",
        PLUGIN_VERSION
    };
}

APICALL EXPORT void PLUGIN_EXIT() {
    savePersistentState();
    cancelPrimaryProfileTransition();
    unregisterEventListeners();
    unregisterLuaFunctions();
    unregisterAlgorithms();
    clearAllPrimaryFullscreenState(true);
    for (const auto& [key, target] : g_primaryState.targets) {
        if (target && target->window()) {
            setPrimaryProfileHiddenState(target->window(), false);
            target->window()->m_pinned = false;
        }
    }

    for (auto* instance : g_instances) {
        if (instance)
            instance->clearSecondaryFullscreenOnExit();
    }

    g_instances.clear();
    g_primaryState.windowKeys.clear();
    g_primaryState.windowProfiles.clear();
    g_primaryState.targets.clear();
    g_primaryState.profiles.clear();
    g_primaryState.hiddenWindowKeys.clear();
    g_primaryProfileTransition.fadingOutKeys.clear();
    g_primaryProfileTransition.fadingInKeys.clear();
    g_primaryState.activeProfile = 1;
    g_primaryState.inactiveProfilesDirty = true;
    g_primaryState.inactiveProfilesPrimaryArea.reset();
    notify("hyprsplitrow unloaded");
    g_pluginHandle = nullptr;
}
