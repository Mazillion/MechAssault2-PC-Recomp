#include "mission_extras.h"

#include <algorithm>
#include <cstdlib>
#include <set>
#include <unordered_set>

#include "scenario.h"
#include "triggers.h"
#include "util.h"

namespace ma2 {

using namespace world;

const char* kGenPrefix = "[gen] ";
const char* kGenGroup = "[gen] Mission extras";

namespace {

const uint32_t K_TIMER = key_of("MATriggerTimer"), K_LOGIC = key_of("MATriggerLogic"), K_ACTIVATE = key_of("MAActionActivateCinema"),
               K_CINEMA = key_of("MACinema"), K_CTRL = key_of("MAFreeCameraController");

// Next free generated text id (kStrGenerated ..), counting what `strings` already holds.
int next_gen_id(const std::map<int, std::string>& strings) {
    int n = kStrGenerated;
    while (strings.count(n)) n++;
    return n;
}

WObj* by_uid(Wdf& w, uint32_t uid) { return uid ? object_by_uid(w, uid) : nullptr; }

// The trigger that ends the mission's intro cutscene (the last shot's "ends" trigger), 0 if the mission has none.
// The intro is a cutscene a trigger plays at the start: an enabled timer of one second or less (the same test the
// start-state code in scenario.cpp uses for "plays at mission start") holding "Play a camera sequence".
uint32_t intro_exit_trigger(const Wdf& w) {
    WTree t;
    t.build(w);
    for (size_t i = 0; i < w.objs.size(); i++) {
        const WObj& tr = w.objs[i];
        if (tr.cls != K_TIMER || !tr.i32("TriggerEnabled") || tr.f32("TrgTimerStart") > 1.f ||
            std::max(tr.f32("TrgTimerDuration"), tr.f32("TrgTimerDurationSeconds")) > 1.f)
            continue;
        for (int k : t.kids[i]) {
            const WObj& a = w.objs[size_t(k)];
            if (a.cls != K_ACTIVATE) continue;
            int ci = t.find(a.u32("cinemaID"));
            if (ci < 0 || w.objs[size_t(ci)].cls != K_CINEMA) continue;
            std::vector<int> sub;
            t.subtree(ci, sub);
            uint32_t exit = 0;
            for (int m : sub)
                if (w.objs[size_t(m)].cls == K_CTRL) exit = w.objs[size_t(m)].u32("triggerExit");  // the last shot wins
            if (exit && t.find(exit) >= 0) return exit;
        }
    }
    return 0;
}

void set_timer(Wdf& w, uint32_t uid, float seconds, bool enabled) {
    WObj* o = by_uid(w, uid);
    if (!o) return;
    o->set_f32("TrgTimerDurationSeconds", seconds);
    o->set_f32("TrgTimerDuration", seconds / 60.f);
    o->set_f32("TrgTimerStart", 0);
    if (o->find("TriggerEnabled")) o->set_i32("TriggerEnabled", enabled ? 1 : 0);
}

// A UTF-8 string cut to at most `max` bytes without splitting a character.
std::string cut_utf8(const std::string& s, size_t max) {
    if (s.size() <= max) return s;
    size_t n = max;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) n--;
    return s.substr(0, n);
}

}  // namespace

// ----------------------------------------------------------------------------- briefing

std::vector<BriefingPage> briefing_pages(const std::string& title, const std::string& text) {
    constexpr size_t kWidth = 18;  // characters per line, a little under the measured 19-20 so a page never overflows
    constexpr int kLines = 6;
    // Paragraphs: the title, then the text's own lines; blank lines are dropped (the box has six lines in all).
    std::vector<std::string> paras;
    auto add_lines = [&](const std::string& src) {
        for (auto& ln : split(src, '\n')) {
            std::string t = trim(ln);
            if (!t.empty()) paras.push_back(t);
        }
    };
    add_lines(title);
    add_lines(text);
    // Word wrap each paragraph to kWidth: (paragraph, line) pairs.
    struct Line { size_t para; std::string text; };
    std::vector<Line> lines;
    for (size_t p = 0; p < paras.size(); p++) {
        std::string cur;
        for (auto& word : split(paras[p], ' ')) {
            if (word.empty()) continue;
            std::string wd = word;
            while (wd.size() > kWidth) {  // a word longer than a line breaks across lines
                if (!cur.empty()) { lines.push_back({p, cur}); cur.clear(); }
                lines.push_back({p, cut_utf8(wd, kWidth)});
                wd = wd.substr(cut_utf8(wd, kWidth).size());
            }
            if (cur.empty()) cur = wd;
            else if (cur.size() + 1 + wd.size() <= kWidth) cur += " " + wd;
            else { lines.push_back({p, cur}); cur = wd; }
        }
        if (!cur.empty()) lines.push_back({p, cur});
    }
    std::vector<BriefingPage> pages;
    for (size_t i = 0; i < lines.size(); i += kLines) {
        std::string body;
        size_t end = std::min(lines.size(), i + size_t(kLines));
        for (size_t k = i; k < end; k++) {
            if (k > i) body += lines[k].para == lines[k - 1].para ? " " : "\n";
            body += lines[k].text;
        }
        BriefingPage pg;
        pg.text = body;
        pg.seconds = std::min(30.f, 4.f + float(body.size()) / 15.f);
        pages.push_back(std::move(pg));
    }
    return pages;
}

std::string briefing_description(const std::string& title, const std::string& text) {
    auto flat = [](const std::string& s) {  // whitespace runs (line breaks too) become one space
        std::string o;
        bool gap = false;
        for (unsigned char c : s) {
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { gap = !o.empty(); continue; }
            if (gap) o += ' ';
            gap = false;
            o += char(c);
        }
        return o;
    };
    std::string t = flat(title), b = flat(text);
    std::string d = t.empty() ? b : b.empty() ? t : t + ": " + b;
    if (d.size() > 240) d = cut_utf8(d, 240) + "...";
    return d;
}

std::string speaker_line(const std::string& speaker, const std::string& text) {
    std::string sp = trim(speaker);
    return sp.empty() ? text : sp + ": " + text;
}

// ----------------------------------------------------------------------------- cutscene safety

namespace {

const uint32_t K_ACT_AI = key_of("MAActionAI"), K_ESET = key_of("MoeEntitySet"), K_SPAWNER = key_of("MASpawner"),
               K_GSET = key_of("MoeGroupSet"), K_SHOTCTRL = key_of("MAFreeCameraController"), K_CINE = key_of("MACinema"),
               K_PLAY = key_of("MAActionActivateCinema"), K_PATHC = key_of("MAPath"), K_CAMPROP = key_of("MACameraProperty"),
               K_TIMER_ = key_of("MATriggerTimer");

constexpr float kHoldAgainSeconds = 0.5f;

bool is_gen(const WObj& o) { return o.description().compare(0, std::string(kGenPrefix).size(), kGenPrefix) == 0; }

// A unit the AI drives: it has an "AI starts on" flag. (A unit without AI is not a fighter.)
bool is_fighter(const WObj& o) {
    static const uint32_t units[] = {key_of("MAMech"), key_of("MATank"), key_of("MAVehicle"), key_of("MAFlyer"), key_of("MATurret"), key_of("MAPerson")};
    return std::find(std::begin(units), std::end(units), o.cls) != std::end(units) && o.find("m_bAIEnabled");
}

// The cinema's shots (its camera controllers), in play order.
std::vector<uint32_t> shots_of_cinema(const Wdf& w, const WTree& t, uint32_t cin) {
    std::vector<uint32_t> out;
    int ci = t.find(cin);
    if (ci < 0) return out;
    std::vector<int> sub;
    t.subtree(ci, sub);
    for (int k : sub)
        if (w.objs[size_t(k)].cls == K_SHOTCTRL) out.push_back(w.objs[size_t(k)].uid());
    return out;
}

// How long a shot runs: its length setting, else its longest eye / focus path.
float shot_seconds(const Wdf& w, const WTree& t, uint32_t ctrl) {
    const WObj* c = object_by_uid(w, ctrl);
    if (!c) return 0;
    if (c->f32("CameraDuration") > 0) return c->f32("CameraDuration");
    float best = 0;
    std::vector<int> sub;
    if (int ci = t.find(ctrl); ci >= 0) t.subtree(ci, sub);
    for (int k : sub) {
        const WObj& p = w.objs[size_t(k)];
        if (p.cls != K_CAMPROP) continue;
        for (const char* key : {"eyeUID", "focusUID"})
            if (const WObj* po = p.find(key) ? object_by_uid(w, p.u32(key)) : nullptr; po && po->cls == K_PATHC) best = std::max(best, po->f32("duration"));
    }
    return best;
}

std::vector<uint32_t> set_members_of(const WObj& s) {
    std::vector<uint32_t> out;
    int n = s.i32("MoeEntitySetCount");
    for (int k = 0; k < n; k++) out.push_back(s.u32("MoeEntitySetMember" + std::to_string(k)));
    return out;
}

// The triggers that switch a unit's AI on: an "AI on" action naming it (or a set holding it), or its spawner's enable trigger.
std::vector<uint32_t> wakers_of(const Wdf& w, uint32_t unit) {
    std::vector<uint32_t> out;
    auto add = [&](uint32_t t) { if (t && std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t); };
    for (auto& a : w.objs) {
        if (a.cls != K_ACT_AI || !a.i32("EnableAI") || is_gen(a)) continue;
        bool hit = a.u32("ActionEntity") == unit;
        if (!hit)
            if (const WObj* st = a.u32("AISet") ? object_by_uid(w, a.u32("AISet")) : nullptr; st && st->cls == K_ESET)
                for (uint32_t m : set_members_of(*st)) hit |= m == unit;
        if (hit) add(a.parent());
    }
    if (const WObj* u = object_by_uid(w, unit))
        if (const WObj* p = u->parent() ? object_by_uid(w, u->parent()) : nullptr; p && p->cls == K_SPAWNER) add(p->u32("TriggerEnableID"));
    std::sort(out.begin(), out.end());
    return out;
}

float timer_seconds(const WObj& t) {
    float s = t.f32("TrgTimerDurationSeconds");
    return s > 0 ? s : t.f32("TrgTimerDuration") * 60.f;
}

// The triggers that play the cinema (`players`), and where it starts when a timer plays it: that timer's delay; -1 when unknown.
float cinema_start_seconds(const Wdf& w, uint32_t cin, std::vector<uint32_t>& players) {
    float at = -1;
    for (auto& a : w.objs) {
        if (a.cls != K_PLAY || a.u32("cinemaID") != cin || is_gen(a)) continue;
        players.push_back(a.parent());
        if (const WObj* p = object_by_uid(w, a.parent()); p && p->cls == K_TIMER_ && p->i32("TriggerEnabled", 1)) at = timer_seconds(*p);
    }
    return at;
}

}  // namespace

int cutscene_hold(const WObj& c) { return c.find(kHoldProp) ? std::clamp(c.i32(kHoldProp), 0, 2) : 1; }
bool cutscene_skippable(const WObj& c) { return !c.find(kSkipProp) || c.i32(kSkipProp) != 0; }
void set_cutscene_hold(WObj& c, int hold) {
    if (hold == 1) c.erase(key_of(kHoldProp));  // the default is not stored
    else c.set_i32(kHoldProp, hold);
}
void set_cutscene_skippable(WObj& c, bool skippable) {
    if (skippable) c.erase(key_of(kSkipProp));
    else c.set_i32(kSkipProp, 0);
}

std::vector<uint32_t> cutscene_noskip_uids(const Wdf& w) {
    std::vector<uint32_t> out;
    for (auto& o : w.objs)
        if (o.cls == K_CINE && !cutscene_skippable(o) && !is_gen(o)) out.push_back(o.uid());
    return out;
}

std::vector<uint32_t> cutscene_hold_units(const Wdf& w, int hold) {
    std::vector<uint32_t> out;
    if (hold <= 0) return out;
    for (auto& o : w.objs) {
        if (!is_fighter(o) || is_gen(o)) continue;
        int nation = o.i32("Nation");
        if (hold == 1 ? nation == 5 : nation != 0) out.push_back(o.uid());  // 5 = the enemy side, 0 = neutral
    }
    return out;
}

std::vector<std::pair<std::string, uint32_t>> cutscene_hold_notes(const Wdf& w) {
    std::vector<std::pair<std::string, uint32_t>> notes;
    WTree t;
    t.build(w);
    for (auto& c : w.objs) {
        if (c.cls != K_CINE || is_gen(c) || cutscene_hold(c) == 0) continue;
        const std::string nm = c.description().empty() ? std::string("(unnamed)") : c.description();
        std::vector<uint32_t> shots = shots_of_cinema(w, t, c.uid());
        if (shots.empty()) {
            notes.push_back({"Cutscene \"" + nm + "\" holds the fight, but it has no shot, so nothing is held.", c.uid()});
            continue;
        }
        std::vector<uint32_t> players;
        float start = cinema_start_seconds(w, c.uid(), players);
        float len = 0;
        for (uint32_t sh : shots) len += shot_seconds(w, t, sh);
        for (uint32_t u : cutscene_hold_units(w, cutscene_hold(c))) {
            const WObj* uo = object_by_uid(w, u);
            if (!uo) continue;
            for (uint32_t wk : wakers_of(w, u)) {
                const WObj* wo = object_by_uid(w, wk);
                if (!wo) continue;
                bool during = std::find(players.begin(), players.end(), wk) != players.end();
                if (!during && start >= 0 && wo->cls == K_TIMER_ && wo->i32("TriggerEnabled", 1)) {
                    float at = timer_seconds(*wo);
                    during = at >= start && at <= start + len;
                }
                if (during) {
                    notes.push_back({"Cutscene \"" + nm + "\": \"" + uo->description() + "\" is woken during the cutscene by trigger \"" + wo->description() +
                                         "\", so it will fight on camera.", u});
                    break;
                }
            }
        }
    }
    return notes;
}

float intro_seconds(const Wdf& w) {
    WTree t;
    t.build(w);
    for (size_t i = 0; i < w.objs.size(); i++) {
        const WObj& tr = w.objs[i];
        if (tr.cls != K_TIMER_ || !tr.i32("TriggerEnabled", 1) || timer_seconds(tr) > 1.f || is_gen(tr)) continue;
        for (int k : t.kids[i]) {
            const WObj& a = w.objs[size_t(k)];
            if (a.cls != K_PLAY) continue;
            std::vector<uint32_t> shots = shots_of_cinema(w, t, a.u32("cinemaID"));
            float len = 0;
            for (uint32_t sh : shots) len += shot_seconds(w, t, sh);
            if (len > 0) return timer_seconds(tr) + len;
        }
    }
    return 0;
}

uint32_t intro_cinema(const Wdf& w) {
    WTree t;
    t.build(w);
    for (size_t i = 0; i < w.objs.size(); i++) {
        const WObj& tr = w.objs[i];
        if (tr.cls != K_TIMER_ || !tr.i32("TriggerEnabled", 1) || timer_seconds(tr) > 1.f || is_gen(tr)) continue;
        for (int k : t.kids[i]) {
            const WObj& a = w.objs[size_t(k)];
            if (a.cls != K_PLAY) continue;
            std::vector<uint32_t> shots = shots_of_cinema(w, t, a.u32("cinemaID"));
            float len = 0;
            for (uint32_t sh : shots) len += shot_seconds(w, t, sh);
            if (len > 0) return a.u32("cinemaID");
        }
    }
    return 0;
}

bool briefing_span(const Scenario& s, float& from, float& to, float* intro) {
    if (s.multiplayer || (trim(s.briefing_title).empty() && trim(s.briefing).empty())) return false;
    std::vector<BriefingPage> pages = briefing_pages(s.briefing_title, s.briefing);
    if (pages.empty()) return false;
    if (pages.size() > 10) pages.resize(10);
    const float in = intro_seconds(s.world);
    if (intro) *intro = in;
    float len = 0;
    for (size_t k = 0; k < pages.size(); k++) len += pages[k].seconds + (k ? 0.25f : 0.f);
    from = in;
    to = in + len;
    return true;
}

// ----------------------------------------------------------------------------- export

namespace {

// A message with a speaker (CaptionTextID = the speaker's own text) is shown by the game as "Speaker: text",
// because the game never draws the caption. The copy gets a generated body for it; scenario.ini [Generated] remembers the
// author's body id so the next open puts it back.
void add_speaker_bodies(Wdf& w, std::map<int, std::string>& strings, std::vector<std::string>& generated_ini) {
    static const uint32_t K_BROADCAST = key_of("MAActionBroadcast");
    const size_t plen = std::string(kGenPrefix).size();
    for (auto& o : w.objs) {
        if (o.cls != K_BROADCAST || o.description().compare(0, plen, kGenPrefix) == 0) continue;
        uint32_t cap = o.u32("CaptionTextID"), body = o.u32("BodyTextID");
        if (cap < kTextBase || body < kTextBase) continue;  // no speaker, or the game's own words (not readable here)
        auto ci = strings.find(int(cap - kTextBase)), bi = strings.find(int(body - kTextBase));
        if (ci == strings.end() || bi == strings.end() || trim(ci->second).empty()) continue;
        std::string line = speaker_line(ci->second, bi->second);  // before next_gen_id may grow the map
        int id = next_gen_id(strings);
        strings[id] = line;
        o.set_u32("BodyTextID", kTextBase + uint32_t(id));
        generated_ini.push_back("Body" + std::to_string(id) + " = " + std::to_string(int(body - kTextBase)));
    }
}

// "[gen] Briefing": the first page at the start of the mission (after the intro cutscene when one plays then), later pages
// chained by generated timers that each page switches on, so one box is never asked for two texts at once.
void add_briefing(const Scenario& s, Wdf& w, uint32_t group, std::map<int, std::string>& strings, std::vector<std::string>* warnings) {
    if (s.multiplayer || (trim(s.briefing_title).empty() && trim(s.briefing).empty())) return;
    std::vector<BriefingPage> pages = briefing_pages(s.briefing_title, s.briefing);
    if (pages.empty()) return;
    constexpr size_t kMaxPages = 10;
    if (pages.size() > kMaxPages) {
        if (warnings)
            warnings->push_back("The briefing is long: the game shows its first " + std::to_string(kMaxPages) + " pages at the start of the mission (the whole text stays in the Custom Scenarios list).");
        pages.resize(kMaxPages);
    }
    const uint32_t exit = intro_exit_trigger(w);
    // The triggers: the first fires at the start, or when the intro cutscene ends; the others are asleep until the page before them is up.
    std::vector<uint32_t> trig(pages.size(), 0);
    if (exit) {
        uint32_t logic = gen_add_trigger(w, "MATriggerLogic", std::string(kGenPrefix) + "Briefing", group);
        if (logic) {
            if (WObj* o = by_uid(w, logic)) { o->set_i32("LogicType", 0); o->set_u32("Inverse", 0); }
            gen_add_reference(w, logic, exit);
        }
        trig[0] = logic;
    } else {
        trig[0] = gen_add_trigger(w, "MATriggerTimer", std::string(kGenPrefix) + "Briefing", group);
        set_timer(w, trig[0], 0, true);
    }
    for (size_t k = 1; k < pages.size(); k++) {
        trig[k] = gen_add_trigger(w, "MATriggerTimer", std::string(kGenPrefix) + "Briefing page " + std::to_string(k + 1), group);
        set_timer(w, trig[k], pages[k - 1].seconds + 0.25f, false);
    }
    for (size_t k = 0; k < pages.size(); k++) {
        if (!trig[k]) {
            if (warnings) warnings->push_back("The briefing was not added to the mission (the trigger templates are not loaded).");
            return;
        }
        uint32_t b = gen_add_action(w, trig[k], "MAActionBroadcast", std::string(kGenPrefix) + "Briefing text");
        WObj* o = by_uid(w, b);
        if (!o) continue;
        int id = next_gen_id(strings);
        strings[id] = pages[k].text;
        o->set_u32("BodyTextID", kTextBase + uint32_t(id));
        o->set_u32("CaptionTextID", 0);
        o->set_f32("TimeDisplayedFor", pages[k].seconds);
        o->set_i32("PauseGame", 0);  // The game does not pause for a message, and a box with 0 seconds is never drawn
        if (k + 1 < pages.size()) {
            uint32_t a = gen_add_action(w, trig[k], "MAActionAI", std::string(kGenPrefix) + "Briefing next page");
            if (WObj* ao = by_uid(w, a)) { ao->set_u32("ActionEntity", trig[k + 1]); ao->set_u32("AISet", 0); ao->set_i32("EnableAI", 1); }
        }
    }
}

// A new MoeEntitySet of `members` (with the units' back links, as retail ties them) under `parent`.
uint32_t gen_unit_set(Wdf& w, const std::string& name, const std::vector<uint32_t>& members, uint32_t parent, std::set<uint32_t>& fresh) {
    uint32_t u = gen_add_proto(w, "MoeEntitySet", name, parent);
    WObj* set = by_uid(w, u);
    if (!set) return 0;
    int old = set->i32("MoeEntitySetCount");
    for (int k = 0; k < std::max(old, 2); k++) set->erase(key_of("MoeEntitySetMember" + std::to_string(k)));
    for (size_t k = 0; k < members.size(); k++) set->set_u32("MoeEntitySetMember" + std::to_string(k), members[k]);
    set->set_i32("MoeEntitySetCount", int(members.size()));
    for (uint32_t m : members)
        if (WObj* mo = by_uid(w, m)) {
            if (!mo->find("SetCount")) fresh.insert(m);  // the open puts it back as it was (no SetCount at all)
            int n = mo->i32("SetCount");
            mo->set_u32("SetMember" + std::to_string(n), u);
            mo->set_i32("SetCount", n + 1);
        }
    return u;
}

// An "AI on / off" action under `trigger` acting on a set (or, set 0, on the object `entity`).
uint32_t gen_ai_action(Wdf& w, uint32_t trigger, const std::string& name, uint32_t set, uint32_t entity, bool on) {
    uint32_t a = gen_add_action(w, trigger, "MAActionAI", std::string(kGenPrefix) + name);
    if (WObj* o = by_uid(w, a)) { o->set_u32("ActionEntity", entity); o->set_u32("AISet", set); o->set_i32("EnableAI", on ? 1 : 0); }
    return a;
}

uint32_t gen_logic(Wdf& w, const std::string& name, bool any, uint32_t group, const std::vector<uint32_t>& inputs) {
    uint32_t l = gen_add_trigger(w, "MATriggerLogic", std::string(kGenPrefix) + name, group);
    if (WObj* o = by_uid(w, l)) { o->set_i32("LogicType", any ? 1 : 0); o->set_u32("Inverse", 0); }
    for (uint32_t in : inputs) gen_add_reference(w, l, in);
    return l;
}

// "During this cutscene": the units that would fight on camera get their AI switched off when the first shot starts and
// on again when it is over (the last shot ends, or a safety timer of the cutscene's length runs out, whichever comes
// first: a skipped cutscene is caught by the timer at the latest). A unit that starts asleep is switched on only when
// one of its own wake-up triggers has fired.
void add_cutscene_holds(Wdf& w, uint32_t group, std::vector<std::string>& generated_ini, std::vector<std::string>* warnings) {
    std::set<uint32_t> fresh;  // units that had no SetCount before the hold gave them a set link
    std::vector<uint32_t> cins;
    for (auto& o : w.objs)
        if (o.cls == K_CINE && !is_gen(o) && cutscene_hold(o) != 0) cins.push_back(o.uid());
    for (uint32_t cin : cins) {
        WTree t;
        t.build(w);
        const WObj* co = by_uid(w, cin);
        if (!co) continue;
        const std::string cname = co->description().empty() ? std::string("cutscene") : co->description();
        std::vector<uint32_t> shots = shots_of_cinema(w, t, cin);
        std::vector<uint32_t> players;
        cinema_start_seconds(w, cin, players);
        if (shots.empty() || players.empty()) continue;  // nothing plays it, or nothing to play: nothing to hold
        std::vector<uint32_t> units = cutscene_hold_units(w, cutscene_hold(*co));
        if (units.empty()) continue;
        float total = 0;
        for (uint32_t sh : shots) total += shot_seconds(w, t, sh);
        // The shot's start / end triggers (retail ties them to the controller); a shot made without gets a generated one.
        auto trigger_of = [&](uint32_t ctrl, const char* key) -> uint32_t {
            const WObj* c = by_uid(w, ctrl);
            if (!c) return 0;
            uint32_t u = c->u32(key);
            if (u && by_uid(w, u)) return u;
            const bool start = std::string(key) == "triggerInit";
            uint32_t g = gen_add_proto(w, "MATrigger", std::string(kGenPrefix) + cname + (start ? " - starts" : " - ends"), cin);
            if (!g) return 0;
            if (WObj* go = by_uid(w, g)) {
                if (go->find("TriggerEnabled")) go->set_i32("TriggerEnabled", 1);
                if (go->find("TriggerAutoReset")) go->set_i32("TriggerAutoReset", 0);
            }
            if (WObj* cm = by_uid(w, ctrl)) cm->set_u32(key, g);
            return g;
        };
        uint32_t exit = trigger_of(shots.back(), "triggerExit");
        if (!exit) {
            if (warnings) warnings->push_back("Cutscene \"" + cname + "\": the fight is not held (the trigger templates are not loaded).");
            continue;
        }
        // Where the sets go: the level's set group, as retail keeps them.
        uint32_t setparent = group;
        if (int gi = find_class(w, K_GSET); gi >= 0) setparent = w.objs[size_t(gi)].uid();
        const std::string tag = std::string(kGenPrefix) + "Hold " + cname;
        uint32_t hold_set = gen_unit_set(w, tag + " - units", units, setparent, fresh);
        uint32_t safety = gen_add_trigger(w, "MATriggerTimer", std::string(kGenPrefix) + cname + " - safety timer", group);
        if (!hold_set || !safety) continue;
        // Every play holds and releases. The game fires a shot's start / end trigger on every play (forced, past
        // its fired flag), but a Logic or timer fires once unless it re-arms: so the release hangs under the last shot's end
        // trigger itself, and the safety timer repeats (Repeat on, switched on by the start, off by the end or by itself) like
        // the cue timers.
        set_timer(w, safety, total + 0.5f, false);
        if (WObj* so = by_uid(w, safety)) so->set_i32("TriggerAutoReset", 1);
        // Start: AI off for the set, and the safety timer starts counting. Under every trigger that plays the cutscene, not
        // under shot 1's start trigger: in a cutscene of several shots the game does not fire shot 1's start trigger again on a
        // second play (run: the enemy fought through the replay of a 3-shot intro; a 1-shot replay held).
        float early = -1;  // a play at the very start of the mission (a timer of one second or less)
        for (uint32_t p : players) {
            gen_ai_action(w, p, "Hold the fight", hold_set, 0, false);
            gen_ai_action(w, p, "Start the safety timer", 0, safety, true);
            if (const WObj* po = object_by_uid(w, p); po && po->cls == K_TIMER_ && po->i32("TriggerEnabled", 1) && timer_seconds(*po) <= 1.f)
                early = std::max(early, timer_seconds(*po));
        }
        // The units' own AI starts a moment after the mission does and undoes an AI-off given at second 0 (run:
        // with the intro at 0 s the enemy fired during shot 1, with the intro at 3 s it was held). So a play at the start of
        // the mission holds again shortly after it, while shot 1 is still on screen.
        if (early >= 0 && total >= 0.4f) {
            uint32_t again = gen_add_trigger(w, "MATriggerTimer", std::string(kGenPrefix) + cname + " - hold again", group);
            if (again) {
                set_timer(w, again, early + std::min(kHoldAgainSeconds, total * 0.5f), true);
                gen_ai_action(w, again, "Hold the fight again", hold_set, 0, false);
            }
        }
        // Restore: awake units as soon as it is over; a unit that started asleep only when one of its wakers has fired.
        std::vector<uint32_t> awake;
        std::map<std::vector<uint32_t>, std::vector<uint32_t>> by_wakers;
        for (uint32_t u : units) {
            const WObj* uo = by_uid(w, u);
            if (!uo) continue;
            if (uo->i32("m_bAIEnabled")) { awake.push_back(u); continue; }
            std::vector<uint32_t> wk = wakers_of(w, u);
            if (!wk.empty()) by_wakers[wk].push_back(u);
        }
        std::vector<std::pair<std::string, std::pair<uint32_t, uint32_t>>> restore;  // name, (set, entity)
        if (!awake.empty()) {
            uint32_t aset = gen_unit_set(w, tag + " - awake", awake, setparent, fresh);
            restore.push_back({"Carry on", {aset, 0}});
        }
        int n = 0;
        for (auto& [wk, us] : by_wakers) {
            n++;
            // "Woken and over": a check switched on when the cutscene is over (asleep, Repeat on) that fires once one of the
            // unit's wakers has fired, wakes the unit and switches itself off again.
            uint32_t anyw = wk.size() == 1 ? wk.front() : gen_logic(w, cname + " - wakers " + std::to_string(n), true, group, wk);
            uint32_t check = gen_logic(w, cname + " - woken and over " + std::to_string(n), true, group, {anyw});
            if (WObj* co2 = by_uid(w, check)) {
                if (co2->find("TriggerEnabled")) co2->set_i32("TriggerEnabled", 0);
                co2->set_i32("TriggerAutoReset", 1);
            }
            uint32_t wset = gen_unit_set(w, tag + " - woken " + std::to_string(n), us, setparent, fresh);
            gen_ai_action(w, check, "Carry on (woken)", wset, 0, true);
            gen_ai_action(w, check, "Woken check off", 0, check, false);
            restore.push_back({"Woken check on " + std::to_string(n), {0, check}});
        }
        for (uint32_t over : {exit, safety})
            for (auto& [name, se] : restore) gen_ai_action(w, over, name, se.first, se.second, true);
        // The safety timer always runs out (0.5 s after a full play it only repeats the release) and then switches itself off:
        // a timer switched off before it fired kept its old start in the game (run: the next play's start switched it on and
        // it fired at once, releasing the units for the whole replay); one that fired and switched itself off restarts.
        gen_ai_action(w, safety, "Stop the safety timer", 0, safety, false);  // last: it does not repeat
    }
    for (uint32_t u : fresh) generated_ini.push_back("NoSetCount" + std::to_string(u) + " = 1");
}

// The "Lose the mission if the time runs out" clock (a HUD timer named "Countdown...") starts when the intro
// cutscene is over, not while it plays: in the copy it starts asleep and a once-only Logic on the intro's last end trigger
// switches it on (a replay of the intro does not restart it). [Generated] CountdownN gives it back its own state on open.
void add_countdown_after_intro(Wdf& w, uint32_t group, std::vector<std::string>& generated_ini) {
    const uint32_t exit = intro_exit_trigger(w);
    if (!exit) return;
    std::vector<uint32_t> clocks;
    for (auto& o : w.objs)
        if (o.cls == K_TIMER_ && !is_gen(o) && o.description().compare(0, 9, "Countdown") == 0 && o.i32("TrgTimerDisplayOnHUD") != 0 &&
            o.i32("TriggerEnabled", 1) != 0)
            clocks.push_back(o.uid());
    if (clocks.empty()) return;
    uint32_t go = gen_logic(w, "Start the clock after the intro", false, group, {exit});
    if (!go) return;
    for (uint32_t c : clocks) {
        if (WObj* o = by_uid(w, c)) o->set_i32("TriggerEnabled", 0);
        gen_ai_action(w, go, "Start the clock", 0, c, true);
        generated_ini.push_back("Countdown" + std::to_string(c) + " = 1");
    }
}

// A cutscene of two or more shots that plays a second time shows black for its first shots: the game leaves
// the colour filter (fade) steps of every shot but the last in a stuck state after a play (game-proven 11-16: with the
// filters at alpha 0 both plays show every shot; with the template's filters the replay is pure black for shots 1..n-1).
// So when such a cutscene can play more than once (two or more triggers play it, or one that repeats), its colour filter
// steps are made invisible in the copy: hard cuts instead of fades, the same on every play. [Generated] FadeN gives the
// alphas back on open.
void neutral_cutscene_filters(Wdf& w, std::vector<std::string>& generated_ini) {
    WTree t;
    t.build(w);
    for (auto& c : w.objs) {
        if (c.cls != K_CINE || is_gen(c)) continue;
        std::vector<uint32_t> shots = shots_of_cinema(w, t, c.uid());
        if (shots.size() < 2) continue;
        std::vector<uint32_t> players;
        cinema_start_seconds(w, c.uid(), players);
        bool again = players.size() >= 2;
        for (uint32_t p : players)
            if (const WObj* po = object_by_uid(w, p); po && po->i32("TriggerAutoReset") != 0) again = true;
        if (!again) continue;
        for (uint32_t sh : shots) {
            int si = t.find(sh);
            if (si < 0) continue;
            for (int k : t.kids[size_t(si)]) {
                WObj& st = w.objs[size_t(k)];
                if (st.cls != K_CAMPROP || st.i32("type") != 4) continue;
                char line[96];
                snprintf(line, sizeof line, "Fade%u = %g,%g", st.uid(), double(st.f32("startAlpha")), double(st.f32("endAlpha")));
                generated_ini.push_back(line);
                st.set_f32("startAlpha", 0);
                st.set_f32("endAlpha", 0);
            }
        }
    }
}

}  // namespace

void add_mission_extras(const Scenario& s, Wdf& w, std::map<int, std::string>& strings, std::vector<std::string>& generated_ini,
                        std::vector<std::string>* warnings) {
    add_speaker_bodies(w, strings, generated_ini);
    if (!s.multiplayer) neutral_cutscene_filters(w, generated_ini);
    if (s.multiplayer || !trigger_prototypes_ready()) return;
    const size_t before = w.objs.size();
    uint32_t next = next_uid(w);
    uint32_t group = ensure_group(w, root_uid(w), kGenGroup, next);
    add_briefing(s, w, group, strings, warnings);
    add_cutscene_holds(w, group, generated_ini, warnings);
    add_countdown_after_intro(w, group, generated_ini);
    bool used = false;
    for (auto& o : w.objs) used |= o.parent() == group;
    if (!used && w.objs.size() >= before + 1) wedit::remove_subtree(w, group);  // nothing was generated under it: no empty group
}

// ----------------------------------------------------------------------------- open

int strip_mission_extras(Scenario& s, const IniFile& ini) {
    Wdf& w = s.world;
    // Message bodies swapped at export go back to the author's text.
    std::map<uint32_t, uint32_t> back;
    for (auto* e : ini.section("Generated")) {
        if (e->key.compare(0, 4, "Body") != 0) continue;
        long gen = strtol(e->key.c_str() + 4, nullptr, 10), orig = strtol(trim(e->value).c_str(), nullptr, 10);
        if (gen > 0 && orig > 0) back[kTextBase + uint32_t(gen)] = kTextBase + uint32_t(orig);
    }
    if (!back.empty())
        for (auto& o : w.objs)
            if (o.find("BodyTextID"))
                if (auto it = back.find(o.u32("BodyTextID")); it != back.end()) o.set_u32("BodyTextID", it->second);
    // Colour filter steps made invisible in the copy get their alphas back.
    for (auto* e : ini.section("Generated")) {
        if (e->key.compare(0, 4, "Fade") != 0) continue;
        uint32_t uid = uint32_t(strtoul(e->key.c_str() + 4, nullptr, 10));
        float a0 = 0, a1 = 0;
        if (uid && sscanf(e->value.c_str(), " %f , %f", &a0, &a1) == 2)
            if (WObj* st = object_by_uid(w, uid); st && st->cls == key_of("MACameraProperty")) { st->set_f32("startAlpha", a0); st->set_f32("endAlpha", a1); }
    }
    // A countdown started after the intro in the copy is switched on from the start again.
    for (auto* e : ini.section("Generated"))
        if (e->key.compare(0, 9, "Countdown") == 0)
            if (WObj* o = object_by_uid(w, uint32_t(strtoul(e->key.c_str() + 9, nullptr, 10))); o && o->cls == key_of("MATriggerTimer")) o->set_i32("TriggerEnabled", 1);
    int removed = 0;
    const size_t plen = std::string(kGenPrefix).size();
    std::set<uint32_t> no_setcount;  // units that had no SetCount before the hold
    for (auto* e : ini.section("Generated"))
        if (e->key.compare(0, 10, "NoSetCount") == 0) no_setcount.insert(uint32_t(strtoul(e->key.c_str() + 10, nullptr, 10)));
    std::unordered_set<uint32_t> before;
    for (auto& o : w.objs) before.insert(o.uid());
    for (;;) {
        uint32_t victim = 0;
        for (auto& o : w.objs)
            if (o.description().compare(0, plen, kGenPrefix) == 0) { victim = o.uid(); break; }
        if (!victim) break;
        const size_t n = w.objs.size();
        wedit::remove_subtree(w, victim);
        if (w.objs.size() == n) break;  // could not remove (no such uid): never loop
        removed += int(n - w.objs.size());
    }
    if (removed) {
        // Links from the author's own objects to what was removed: a shot's start / end trigger made for the hold, and the
        // units' set back links.
        std::unordered_set<uint32_t> alive;
        for (auto& o : w.objs) alive.insert(o.uid());
        auto gone = [&](uint32_t u) { return u && before.count(u) && !alive.count(u); };
        for (auto& o : w.objs) {
            if (o.cls == K_SHOTCTRL)
                for (const char* key : {"triggerInit", "triggerExit"})
                    if (o.find(key) && gone(o.u32(key))) o.set_u32(key, 0);
            int n = o.find("SetCount") ? o.i32("SetCount") : 0;
            if (n <= 0) continue;
            std::vector<uint32_t> keep;
            for (int k = 0; k < n; k++)
                if (uint32_t m = o.u32("SetMember" + std::to_string(k)); !gone(m)) keep.push_back(m);
            if (int(keep.size()) == n) continue;
            for (int k = 0; k < n; k++) o.erase(key_of("SetMember" + std::to_string(k)));
            for (size_t k = 0; k < keep.size(); k++) o.set_u32("SetMember" + std::to_string(k), keep[k]);
            o.set_i32("SetCount", int(keep.size()));
            if (keep.empty() && no_setcount.count(o.uid())) o.erase(key_of("SetCount"));
        }
    }
    for (auto it = s.strings.lower_bound(kStrGenerated); it != s.strings.end() && it->first < kStrGeneratedEnd;) it = s.strings.erase(it);
    return removed;
}

}  // namespace ma2
