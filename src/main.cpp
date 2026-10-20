// Input Buffer (mobile) by yrcsi
//
// Idea: when you press (or release, for wave), look a few ticks ahead along
// the predicted path. If the input as-is would hit a hazard but a slightly
// later one would not, hold the input back for that many ticks.
//
// 1 tick = one processCommands call = 1/240 s at default physics, so
// 4 ticks = 1 frame at 60 fps.
//
// NOTE: only the wave has a real trajectory model right now. Other gamemodes
// use constant velocity, so every delay scores the same and inputs pass
// through untouched. See predictDelay() if you want to add more modes.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <cmath>
#include <deque>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace {

// ---- tunables -------------------------------------------------------------
// Fraction of the real hitbox used for the check. Wave and spikes both have
// hitboxes much smaller than their sprites; adjust if it saves too much / too little.
constexpr float kPlayerHitboxScale = 0.35f;
constexpr float kHazardHitboxScale = 0.55f;
// How far (in units) around the path we collect hazards.
constexpr float kPadding = 40.f;

// ---- state ----------------------------------------------------------------
struct Pending {
    bool down;
    int button;
    bool isPlayer1;
    int due;
};

struct State {
    std::deque<Pending> queue;
    int tick = 0;
    bool pressed[2] = {false, false}; // what the game itself currently sees
    float lastX[2] = {0.f, 0.f};
    float lastY[2] = {0.f, 0.f};
    float dx[2] = {0.f, 0.f};
    float dy[2] = {0.f, 0.f};
    float waveSlope[2] = {1.f, 1.f};
    bool replaying = false;

    void reset() {
        queue.clear();
        tick = 0;
        pressed[0] = pressed[1] = false;
        lastX[0] = lastX[1] = lastY[0] = lastY[1] = 0.f;
        dx[0] = dx[1] = dy[0] = dy[1] = 0.f;
        waveSlope[0] = waveSlope[1] = 1.f;
        replaying = false;
    }
};

State g;

struct Limits {
    bool on = false;
    int all = 0;
    int wave = 0;
};

Limits readLimits() {
    Limits l;
    auto mod = Mod::get();
    if (!mod->getSettingValue<bool>("enabled")) return l;
    auto stage = mod->getSettingValue<std::string>("stage");
    l.on = true;
    if (stage == "2/2") { l.all = 2; l.wave = 2; }
    else if (stage == "4/4") { l.all = 4; l.wave = 4; }
    else if (stage == "8/8") { l.all = 8; l.wave = 8; }
    else if (stage == "16/8") { l.all = 16; l.wave = 8; }
    else l.on = false;
    return l;
}

CCRect shrink(CCRect r, float s) {
    float w = r.size.width * s;
    float h = r.size.height * s;
    float cx = r.origin.x + r.size.width / 2.f;
    float cy = r.origin.y + r.size.height / 2.f;
    return CCRect(cx - w / 2.f, cy - h / 2.f, w, h);
}

// ---- prediction -----------------------------------------------------------
struct Probe {
    CCRect rect;
    float dx;
    float dy;       // per-tick vertical movement for non-wave modes
    bool wave;
    bool flipped;   // upside-down gravity
    float slope;    // |dy| / dx for the wave
};

std::vector<CCRect> collectDanger(GJBaseGameLayer* gl, CCRect start, float dx, int horizon, bool waveSolids) {
    std::vector<CCRect> out;
    float minX = start.getMinX() - kPadding;
    float maxX = start.getMaxX() + std::fabs(dx) * horizon + kPadding;

    CCObject* o = nullptr;
    CCARRAY_FOREACH(gl->m_objects, o) {
        auto obj = static_cast<GameObject*>(o);
        if (!obj) continue;
        float x = obj->getPositionX();
        if (x < minX - 60.f || x > maxX + 60.f) continue;

        auto type = obj->m_objectType;
        if (type == GameObjectType::Hazard) {
            out.push_back(shrink(obj->getObjectRect(), kHazardHitboxScale));
        } else if (waveSolids && type == GameObjectType::Solid) {
            out.push_back(obj->getObjectRect());
        }
    }
    return out;
}

// Ticks survived (max = horizon) if the control state is `cur` for the first
// `delay` ticks and `target` afterwards.
int survive(const Probe& pr, const std::vector<CCRect>& danger, bool cur, bool target, int delay, int horizon) {
    CCRect r = pr.rect;
    for (int t = 1; t <= horizon; ++t) {
        bool pressed = (t <= delay) ? cur : target;
        float dy = pr.dy;
        if (pr.wave) {
            float dir = pressed ? 1.f : -1.f;
            if (pr.flipped) dir = -dir;
            dy = dir * pr.slope * std::fabs(pr.dx);
        }
        r.origin.x += pr.dx;
        r.origin.y += dy;
        for (auto const& d : danger) {
            if (r.intersectsRect(d)) return t;
        }
    }
    return horizon;
}

// Returns how many ticks to hold back the input (0 = pass through).
int predictDelay(GJBaseGameLayer* gl, bool isPlayer1, bool target, int limit) {
    int idx = isPlayer1 ? 0 : 1;
    PlayerObject* p = isPlayer1 ? gl->m_player1 : gl->m_player2;
    if (!p || limit <= 0) return 0;
    if (g.dx[idx] <= 0.f) return 0; // no motion info yet

    int horizon = static_cast<int>(Mod::get()->getSettingValue<int64_t>("horizon"));
    if (horizon < limit + 4) horizon = limit + 4;

    Probe pr;
    pr.rect = shrink(p->getObjectRect(), kPlayerHitboxScale);
    pr.dx = g.dx[idx];
    pr.dy = g.dy[idx];
    pr.wave = p->m_isDart;
    pr.flipped = p->m_isUpsideDown;
    pr.slope = g.waveSlope[idx];

    // Only the wave is modeled. Add other gamemodes here (ship, ufo, ...).
    if (!pr.wave) return 0;

    bool cur = g.pressed[idx];
    if (cur == target) return 0;

    auto danger = collectDanger(gl, pr.rect, pr.dx, horizon, true);
    if (danger.empty()) return 0;

    int base = survive(pr, danger, cur, target, 0, horizon);
    if (base >= horizon) return 0; // already safe

    int bestDelay = 0;
    int best = base;
    for (int d = 1; d <= limit; ++d) {
        int s = survive(pr, danger, cur, target, d, horizon);
        if (s > best) {
            best = s;
            bestDelay = d;
            if (best >= horizon) break;
        }
    }

    if (bestDelay > 0 && Mod::get()->getSettingValue<bool>("logging")) {
        log::debug("SAVED +{} ticks ({:.2f} frames @ 60fps) {} (now: {} -> delayed: {}) [limit {}]",
            best - base, (best - base) / 4.f, target ? "press" : "release", base, best, limit);
    }
    return bestDelay;
}

} // namespace

class $modify(IBGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto pl = typeinfo_cast<PlayLayer*>(this);
        int idx = isPlayer1 ? 0 : 1;

        // Replayed (already delayed) event or something we don't touch.
        if (g.replaying || !pl || button != 1) {
            if (button == 1) g.pressed[idx] = down;
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        auto lim = readLimits();
        if (!lim.on) {
            g.queue.clear();
            g.pressed[idx] = down;
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        // Keep order: if something is already waiting, queue behind it with
        // the same due tick so the hold length stays the same.
        if (!g.queue.empty()) {
            g.queue.push_back({down, button, isPlayer1, g.queue.back().due});
            return;
        }

        PlayerObject* p = isPlayer1 ? m_player1 : m_player2;
        bool wave = p && p->m_isDart;
        int limit = wave ? lim.wave : lim.all;

        int d = predictDelay(this, isPlayer1, down, limit);
        if (d <= 0) {
            g.pressed[idx] = down;
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }
        g.queue.push_back({down, button, isPlayer1, g.tick + d});
    }

    void processCommands(float dt) {
        if (typeinfo_cast<PlayLayer*>(this)) {
            ++g.tick;

            for (int i = 0; i < 2; ++i) {
                PlayerObject* p = (i == 0) ? m_player1 : m_player2;
                if (!p) continue;
                float x = p->getPositionX();
                float y = p->getPositionY();
                float dx = x - g.lastX[i];
                float dy = y - g.lastY[i];
                // Ignore respawns / teleports / portals
                if (dx > 0.f && dx < 20.f && std::fabs(dy) < 20.f) {
                    g.dx[i] = dx;
                    g.dy[i] = dy;
                    if (p->m_isDart && std::fabs(dy) > 0.001f) {
                        g.waveSlope[i] = std::fabs(dy) / dx;
                    }
                }
                g.lastX[i] = x;
                g.lastY[i] = y;
            }

            while (!g.queue.empty() && g.queue.front().due <= g.tick) {
                auto e = g.queue.front();
                g.queue.pop_front();
                g.replaying = true;
                this->handleButton(e.down, e.button, e.isPlayer1);
                g.replaying = false;
            }
        }
        GJBaseGameLayer::processCommands(dt);
    }
};

class $modify(IBPlayLayer, PlayLayer) {
    void resetLevel() {
        g.reset();
        PlayLayer::resetLevel();
    }
};
