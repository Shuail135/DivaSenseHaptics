#include "Config.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <cstdint>

namespace {
std::string trim(std::string s) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    return s;
}
bool toBool(const std::string& s, bool fallback) {
    auto v = lower(trim(s));
    if (v=="1" || v=="true" || v=="yes" || v=="on") return true;
    if (v=="0" || v=="false" || v=="no" || v=="off") return false;
    return fallback;
}
int toInt(const std::string& s, int fallback) {
    try { return std::stoi(trim(s)); } catch (...) { return fallback; }
}
float toFloat(const std::string& s, float fallback) {
    try { return std::stof(trim(s)); } catch (...) { return fallback; }
}
std::wstring widen(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}
uint8_t noteMask(const std::string& raw, uint8_t fallback) {
    auto v=lower(trim(raw));
    if(v.empty()) return fallback;
    // Numeric masks are also accepted (square=1,cross=2,circle=4,triangle=8).
    try {
        size_t pos=0; int n=std::stoi(v,&pos,0);
        if(pos==v.size()) return static_cast<uint8_t>(n)&0x0f;
    } catch (...) {}
    for(char& c:v) if(c=='|'||c==','||c=='/') c='+';
    uint8_t out=0;
    std::stringstream ss(v); std::string tok;
    while(std::getline(ss,tok,'+')) {
        tok=trim(tok);
        if(tok=="none"||tok=="off"||tok=="0") continue;
        if(tok=="square"||tok=="sq") out|=0x01;
        else if(tok=="cross"||tok=="x") out|=0x02;
        else if(tok=="circle"||tok=="o") out|=0x04;
        else if(tok=="triangle"||tok=="tri") out|=0x08;
        else return fallback;
    }
    return out;
}
}

ModConfig ModConfig::Load(const std::filesystem::path& path) {
    ModConfig c;
    std::ifstream f(path);
    if (!f) return c;

    std::unordered_map<std::string,std::string> kv;
    std::string section, line;
    while (std::getline(f, line)) {
        auto hash = line.find_first_of("#;");
        if (hash != std::string::npos) line.resize(hash);
        line = trim(line);
        if (line.empty()) continue;
        if (line.front()=='[' && line.back()==']') {
            section = lower(trim(line.substr(1, line.size()-2)));
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto key = lower(trim(line.substr(0,eq)));
        auto value = trim(line.substr(eq+1));
        if (value.size()>=2 && value.front()=='"' && value.back()=='"')
            value = value.substr(1,value.size()-2);
        kv[section+"."+key] = value;
    }

    auto get = [&](const char* k)->std::string {
        auto it = kv.find(k); return it==kv.end()?std::string{}:it->second;
    };
    auto b = [&](const char* k, bool d){ auto v=get(k); return v.empty()?d:toBool(v,d); };
    auto i = [&](const char* k, int d){ auto v=get(k); return v.empty()?d:toInt(v,d); };
    auto fl= [&](const char* k, float d){ auto v=get(k); return v.empty()?d:toFloat(v,d); };

    c.audio.enabled = b("audio.enabled", c.audio.enabled);
    c.audio.musicGain = fl("audio.music_gain", c.audio.musicGain);
    c.audio.masterGain = fl("audio.master_gain", c.audio.masterGain);
    c.audio.highPassHz = fl("audio.highpass_hz", c.audio.highPassHz);
    c.audio.lowPassHz = fl("audio.lowpass_hz", c.audio.lowPassHz);
    c.audio.softClipDrive = fl("audio.softclip_drive", c.audio.softClipDrive);
    if (auto v=get("audio.endpoint_contains"); !v.empty()) c.audio.endpointContains = widen(v);
    c.audio.maxBufferedMs = i("audio.max_buffered_ms", c.audio.maxBufferedMs);
    c.audio.targetBufferedMs = i("audio.target_buffered_ms", c.audio.targetBufferedMs);

    c.controller.enabled = b("controller.enabled", c.controller.enabled);
    c.controller.forceAudioHaptics = b("controller.force_audio_haptics", c.controller.forceAudioHaptics);
    c.controller.unmuteIntervalMs = i("controller.unmute_interval_ms", c.controller.unmuteIntervalMs);

    c.input.enabled = b("input.enabled", c.input.enabled);
    c.input.multiWindowMs = i("input.multi_window_ms", c.input.multiWindowMs);
    c.input.holdStartMs = i("input.hold_start_ms", c.input.holdStartMs);
    c.input.slideWindowMs = i("input.slide_window_ms", c.input.slideWindowMs);
    c.input.chainStartMs = i("input.chain_start_ms", c.input.chainStartMs);
    c.input.stickThreshold = i("input.stick_threshold", c.input.stickThreshold);
    c.input.touchSwipePixels = i("input.touch_swipe_pixels", c.input.touchSwipePixels);
    c.input.sticksAsSlides = b("input.sticks_as_slides", c.input.sticksAsSlides);
    c.input.shouldersAsSlides = b("input.shoulders_as_slides", c.input.shouldersAsSlides);
    c.input.touchpadAsSlides = b("input.touchpad_as_slides", c.input.touchpadAsSlides);
    c.input.dpadAsFaceButtons = b("input.dpad_as_face_buttons", c.input.dpadAsFaceButtons);
    if (auto v=get("input.l1_macro"); !v.empty()) c.input.l1MacroMask = noteMask(v,c.input.l1MacroMask);
    if (auto v=get("input.r1_macro"); !v.empty()) c.input.r1MacroMask = noteMask(v,c.input.r1MacroMask);
    if (auto v=get("input.l2_macro"); !v.empty()) c.input.l2MacroMask = noteMask(v,c.input.l2MacroMask);
    if (auto v=get("input.r2_macro"); !v.empty()) c.input.r2MacroMask = noteMask(v,c.input.r2MacroMask);

    c.effects.singleGain = fl("effects.single_gain", c.effects.singleGain);
    c.effects.multiGain = fl("effects.multi_gain", c.effects.multiGain);
    c.effects.multi2Multiplier = fl("effects.multi_2_multiplier", c.effects.multi2Multiplier);
    c.effects.multi3Multiplier = fl("effects.multi_3_multiplier", c.effects.multi3Multiplier);
    c.effects.multi4Multiplier = fl("effects.multi_4_multiplier", c.effects.multi4Multiplier);
    c.effects.slideGain = fl("effects.slide_gain", c.effects.slideGain);
    c.effects.holdGain = fl("effects.hold_gain", c.effects.holdGain);
    c.effects.chainGain = fl("effects.chain_gain", c.effects.chainGain);
    c.effects.chainFrequencyHz = fl("effects.chain_frequency_hz", c.effects.chainFrequencyHz);
    c.effects.successNoteGain = fl("effects.success_note_gain", c.effects.successNoteGain);
    c.effects.challengeGain = fl("effects.challenge_gain", c.effects.challengeGain);
    c.effects.challengeNoteMultiplier = fl("effects.challenge_note_multiplier", c.effects.challengeNoteMultiplier);

    c.judgement.enabled = b("judgement.enabled", c.judgement.enabled);
    c.judgement.coolGain = fl("judgement.cool_gain", c.judgement.coolGain);
    c.judgement.fineGain = fl("judgement.fine_gain", c.judgement.fineGain);
    c.judgement.safeGain = fl("judgement.safe_gain", c.judgement.safeGain);
    c.judgement.sadGain = fl("judgement.sad_gain", c.judgement.sadGain);
    c.judgement.wrongGain = fl("judgement.wrong_gain", c.judgement.wrongGain);
    c.judgement.worstGain = fl("judgement.worst_gain", c.judgement.worstGain);
    c.judgement.overlayGain = fl("judgement.overlay_gain", c.judgement.overlayGain);
    c.judgement.matchWindowMs = i("judgement.match_window_ms", c.judgement.matchWindowMs);
    c.judgement.gameplayPollTimeoutMs = i("judgement.gameplay_poll_timeout_ms", c.judgement.gameplayPollTimeoutMs);
    c.judgement.logEvents = b("judgement.log_events", c.judgement.logEvents);

    c.menu.enabled = b("menu.enabled", c.menu.enabled);
    c.menu.visualValidation = b("menu.visual_validation", c.menu.visualValidation);
    c.menu.navGain = fl("menu.nav_gain", c.menu.navGain);
    c.menu.confirmGain = fl("menu.confirm_gain", c.menu.confirmGain);
    c.menu.validationWindowMs = i("menu.validation_window_ms", c.menu.validationWindowMs);
    c.menu.sampleIntervalMs = i("menu.sample_interval_ms", c.menu.sampleIntervalMs);
    c.menu.settleMs = i("menu.settle_ms", c.menu.settleMs);
    c.menu.settleMaxDelta = fl("menu.settle_max_delta", c.menu.settleMaxDelta);
    c.menu.navMinDelta = fl("menu.nav_min_delta", c.menu.navMinDelta);
    c.menu.confirmMinDelta = fl("menu.confirm_min_delta", c.menu.confirmMinDelta);
    c.menu.motionFactor = fl("menu.motion_factor", c.menu.motionFactor);
    c.menu.motionMargin = fl("menu.motion_margin", c.menu.motionMargin);
    c.menu.maxBaselineMotion = fl("menu.max_baseline_motion", c.menu.maxBaselineMotion);
    if (auto v=get("menu.confirm_button"); !v.empty()) {
        const uint8_t m=noteMask(v,c.menu.confirmMask);
        if(m && (m & static_cast<uint8_t>(m-1u))==0) c.menu.confirmMask=m;
    }
    c.menu.leftRightNavigation = b("menu.left_right_navigation", c.menu.leftRightNavigation);
    c.menu.stickNavigation = b("menu.stick_navigation", c.menu.stickNavigation);
    c.menu.logEvents = b("menu.log_events", c.menu.logEvents);

    c.challenge.enabled = b("challenge.enabled", c.challenge.enabled);

    return c;
}
