#include "Config.h"
#include "DscChart.h"
#include "DscProbe.h"
#include "ChartAwareness.h"
#include "HapticEngine.h"
#include "InputHaptics.h"
#include "JudgementHaptics.h"
#ifdef _WIN32
#include "MenuHaptics.h"
#endif
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <cstring>
#include <cstdlib>

struct Peaks { float all=0,left=0,right=0; };

static void require(bool condition, const char* message) {
    if (!condition) {
		std::cerr << "SMOKE FAIL: " << message << std::endl;
		std::exit(1);
	}
}
static Peaks renderPeaks(HapticEngine& e, uint32_t frames=480) {
    std::vector<float> out(static_cast<size_t>(frames)*2);
    e.RenderBlock(out.data(),frames,48000);
    Peaks p{};
    for(uint32_t i=0;i<frames;i++){
        p.left=std::max(p.left,std::abs(out[i*2]));
        p.right=std::max(p.right,std::abs(out[i*2+1]));
        p.all=std::max({p.all,p.left,p.right});
    }
    return p;
}

static uint8_t neutralDpad(){ return 8; }

static void pushU32(std::vector<uint8_t>& out, uint32_t v) {
    const size_t p=out.size(); out.resize(p+4); std::memcpy(out.data()+p,&v,4);
}
static void pushCmd(std::vector<uint8_t>& out, uint32_t op, std::initializer_list<int32_t> params) {
    pushU32(out,op); for(auto v:params) pushU32(out,static_cast<uint32_t>(v));
}
static void pushTarget(std::vector<uint8_t>& out, int type) {
    pushCmd(out,0x06,{type,0,0,0,0,0,0});
}

int main(){
    // Music + event mixer.
    ModConfig cfg;
    cfg.audio.musicGain=0.5f;
    HapticEngine e;e.Configure(cfg);
    std::vector<float> audio(480*2);
    for(size_t i=0;i<480;i++){
        float s=std::sin(float(i)*6.2831853f*120.0f/48000.0f)*0.5f;
        audio[i*2]=audio[i*2+1]=s;
    }
    e.PushAudioFloatStereo(audio.data(),480,48000);
    e.Trigger(HapticEvent::Multi4,1.0f);
    auto mixed=renderPeaks(e);
    require(mixed.all>0.05f&&mixed.all<=1.0f, "music + gameplay event mix did not produce a valid haptic peak");

    // Actual multi-note mask changes the spatial texture.
    ModConfig fxCfg;fxCfg.audio.enabled=false;fxCfg.audio.masterGain=0.55f;
    HapticEngine multi2Engine;multi2Engine.Configure(fxCfg);multi2Engine.Trigger(HapticEvent::Multi2,1.0f); 
    auto multi2Peak=renderPeaks(multi2Engine,256);
    require(multi2Peak.all>0.001f , "multi2 waveform silent");

    // A normal USB/XInput-synthesized Cross press must create a gameplay haptic.
    ModConfig buttonCfg=fxCfg;
    buttonCfg.input.multiWindowMs=0;
    HapticEngine buttonEngine;buttonEngine.Configure(buttonCfg);
    JudgementHaptics buttonJudge(buttonEngine,buttonCfg);
    buttonJudge.SetHookAvailable(true);buttonJudge.OnGamePoll();
    InputHaptics buttonInput(buttonEngine,buttonCfg,&buttonJudge);
    uint8_t buttonReport[11]{};
    buttonReport[0]=1;buttonReport[1]=buttonReport[2]=buttonReport[3]=buttonReport[4]=128;buttonReport[8]=neutralDpad();
    buttonInput.OnUsbReport(buttonReport,sizeof(buttonReport));
    buttonReport[8]=static_cast<uint8_t>(neutralDpad()|0x20); // Cross / XInput A
    buttonInput.OnUsbReport(buttonReport,sizeof(buttonReport));
    buttonInput.OnUsbReport(buttonReport,sizeof(buttonReport));
    require(renderPeaks(buttonEngine).all<0.001f,"raw input bypassed judgement");
    buttonJudge.OnJudgement(JudgementHaptics::Grade::Cool,false,false,false,false,false,false,1);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));buttonJudge.Tick();
    auto pbutton=renderPeaks(buttonEngine,256);
    require(pbutton.all>0.02f, "single Cross button report did not produce a haptic");

    // DIVA shoulder macro can become a multi-note instead of a slide.
    ModConfig macroCfg=fxCfg;
    macroCfg.input.multiWindowMs=0;
    macroCfg.input.l1MacroMask=0x09; // square+triangle
    macroCfg.input.shouldersAsSlides=true; // macro has priority for L1 only
    HapticEngine macroEngine;macroEngine.Configure(macroCfg);
    JudgementHaptics macroJudge(macroEngine,macroCfg);
    macroJudge.SetHookAvailable(true);macroJudge.OnGamePoll();
    InputHaptics macroInput(macroEngine,macroCfg,&macroJudge);
    uint8_t report[64]{};
    report[0]=1;report[1]=report[2]=report[3]=report[4]=128;report[8]=neutralDpad();
    macroInput.OnUsbReport(report,sizeof(report));
    report[9]=0x01; // L1
    macroInput.OnUsbReport(report,sizeof(report));
    macroInput.OnUsbReport(report,sizeof(report)); // flush zero-ms pending window
    macroJudge.OnJudgement(JudgementHaptics::Grade::Cool,false,false,false,false,false,false,2);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));macroJudge.Tick();
    auto pm=renderPeaks(macroEngine,256);
    require(pm.all>0.02f, "shoulder macro did not produce a multi-note haptic");

    // L1+L2 with no macro is detected as a double-left slide and should be left-heavy.
    ModConfig slideCfg=fxCfg;slideCfg.input.slideWindowMs=0;
    HapticEngine slideEngine;slideEngine.Configure(slideCfg);
    JudgementHaptics slideJudge(slideEngine,slideCfg);
    slideJudge.SetHookAvailable(true);slideJudge.OnGamePoll();
    InputHaptics slideInput(slideEngine,slideCfg,&slideJudge);
    std::fill(std::begin(report),std::end(report),uint8_t{0});
    report[0]=1;report[1]=report[2]=report[3]=report[4]=128;report[8]=neutralDpad();
    slideInput.OnUsbReport(report,sizeof(report));
    report[9]=0x05; // L1 + L2
    slideInput.OnUsbReport(report,sizeof(report));
    slideInput.OnUsbReport(report,sizeof(report));
    slideJudge.OnJudgement(JudgementHaptics::Grade::Cool,true,false,false,false,false,false,1);
    auto ps=renderPeaks(slideEngine,256);
    require(ps.left>ps.right*1.3f, "double-left slide was not sufficiently left-biased");


    // During gameplay the main body waits for DIVA's confirmation, then uses the grade.
    ModConfig judgeCfg=fxCfg;
    judgeCfg.judgement.enabled=true;
    // Raw presses must never bypass judgement confirmation.
    judgeCfg.judgement.overlayGain=0.42f;
    HapticEngine judgeEngine;judgeEngine.Configure(judgeCfg);
    JudgementHaptics judge(judgeEngine,judgeCfg);
    judge.Submit(HapticEvent::Cross,0x02,false);
    require(renderPeaks(judgeEngine,128).all<0.001f, "missing hook allowed raw gameplay haptics");
    judge.SetHookAvailable(true);
    judge.OnGamePoll();
    judge.Submit(HapticEvent::Cross,0x02,false);
    auto beforeConfirm=renderPeaks(judgeEngine,128);
    require(beforeConfirm.all<0.001f, "gameplay note fired its full haptic before DIVA judgement confirmation");
    judge.OnJudgement(JudgementHaptics::Grade::Cool,false,false,false,false,false,false,1); // COOL
    std::this_thread::sleep_for(std::chrono::milliseconds(2)); judge.Tick();
    auto coolConfirmed=renderPeaks(judgeEngine,256);
    require(coolConfirmed.all>0.02f, "confirmed COOL did not produce haptic feedback");

    ModConfig disabledCfg=judgeCfg;
    disabledCfg.judgement.enabled=false;
    HapticEngine disabledEngine;disabledEngine.Configure(disabledCfg);
    JudgementHaptics disabledJudge(disabledEngine,disabledCfg);
    disabledJudge.Submit(HapticEvent::Cross,0x02,false);
    disabledJudge.OnJudgement(JudgementHaptics::Grade::Cool,false,false,false,false,false,false,1);
    disabledJudge.Tick();
    require(renderPeaks(disabledEngine,256).all<0.001f, "disabled judgements allowed gameplay note feedback");


    // Game-resolved input must work without a raw HID/XInput candidate. This is what
    // lets Mega Mix+ keybinds, Steam remaps, keyboard keys and in-game macros follow
    // the game automatically instead of requiring haptics.ini mappings.
    ModConfig gameMapCfg=fxCfg;
    gameMapCfg.judgement.enabled=true;
    gameMapCfg.judgement.overlayGain=0.0f;
    gameMapCfg.audio.masterGain=0.42f;
    HapticEngine gameSingleEngine;gameSingleEngine.Configure(gameMapCfg);
    JudgementHaptics gameSingle(gameSingleEngine,gameMapCfg);
    gameSingle.SetHookAvailable(true);
    gameSingle.OnJudgement(JudgementHaptics::Grade::Cool,false,false,false,false,false,false,1);
    std::this_thread::sleep_for(std::chrono::milliseconds(2)); gameSingle.Tick();
    auto gameSinglePeak=renderPeaks(gameSingleEngine,512);
    require(gameSinglePeak.all>0.01f, "game-resolved note without raw controller input produced no haptic");

    HapticEngine game2Engine;game2Engine.Configure(gameMapCfg);
    JudgementHaptics game2(game2Engine,gameMapCfg);game2.SetHookAvailable(true);
    game2.OnJudgement(JudgementHaptics::Grade::Cool,false,false,false,false,false,false,2);
    std::this_thread::sleep_for(std::chrono::milliseconds(2)); game2.Tick();
    auto game2Peak=renderPeaks(game2Engine,512);
    HapticEngine game3Engine;game3Engine.Configure(gameMapCfg);
    JudgementHaptics game3(game3Engine,gameMapCfg);game3.SetHookAvailable(true);
    game3.OnJudgement(JudgementHaptics::Grade::Cool,false,false,false,false,false,false,3);
    std::this_thread::sleep_for(std::chrono::milliseconds(2)); game3.Tick();
    auto game3Peak=renderPeaks(game3Engine,512);
    HapticEngine game4Engine;game4Engine.Configure(gameMapCfg);
    JudgementHaptics game4(game4Engine,gameMapCfg);game4.SetHookAvailable(true);
    game4.OnJudgement(JudgementHaptics::Grade::Cool,false,false,false,false,false,false,4);
    std::this_thread::sleep_for(std::chrono::milliseconds(2)); game4.Tick();
    auto game4Peak=renderPeaks(game4Engine,512);
    require(game2Peak.all>gameSinglePeak.all, "2-note confirmation was not stronger than a single note");
    require(game3Peak.all>gameSinglePeak.all, "3-note confirmation was not stronger than a 2-note confirmation");
    require(game4Peak.all>gameSinglePeak.all, "4-note confirmation was not stronger than a 3-note confirmation");

    // Full report -> judgement -> PCM path, with either thread arriving first.
    for(uint8_t mask : {uint8_t{0x07},uint8_t{0x0b},uint8_t{0x0d},uint8_t{0x0e},uint8_t{0x0f}}) {
		const int reported = (mask == 0x0f) ? 4 : 3;
        for(bool macro : {false,true}) for(bool judgementFirst : {false,true}) {
            ModConfig chordCfg=gameMapCfg;
            if(macro) chordCfg.input.l1MacroMask=mask;
            HapticEngine chordEngine;chordEngine.Configure(chordCfg);
            JudgementHaptics chordJudge(chordEngine,chordCfg);
            chordJudge.SetHookAvailable(true);chordJudge.OnGamePoll();
            InputHaptics chordInput(chordEngine,chordCfg,&chordJudge);
            uint8_t chordReport[11]{1,128,128,128,128,0,0,0,8,0,0};
            chordInput.OnUsbReport(chordReport,sizeof(chordReport));
            if(judgementFirst) chordJudge.OnJudgement(JudgementHaptics::Grade::Cool,false,false,false,false,false,false,reported);
            if(macro) chordReport[9]=1;
            else chordReport[8]=static_cast<uint8_t>(8 | (mask<<4));
            chordInput.OnUsbReport(chordReport,sizeof(chordReport));
            if(!judgementFirst) {
                require(renderPeaks(chordEngine).all<0.001f,"unconfirmed chord produced haptics");
                chordJudge.OnJudgement(JudgementHaptics::Grade::Cool,false,false,false,false,false,false,reported);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));chordJudge.Tick();
            const auto peaks=renderPeaks(chordEngine,16000);
            require(peaks.left>0.1f && peaks.right>0.1f,"confirmed physical/macro chord was silent");
            chordJudge.Tick();
            require(renderPeaks(chordEngine).all<0.001f,"confirmed chord replayed on next frame");
        }
    }

    HapticEngine missEngine;missEngine.Configure(gameMapCfg);
    JudgementHaptics missJudge(missEngine,gameMapCfg);missJudge.SetHookAvailable(true);
    missJudge.OnJudgement(JudgementHaptics::Grade::Worst,false,false,false,false,false,false,4);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));missJudge.Tick();
    require(renderPeaks(missEngine).all<0.001f,"all-miss chord incorrectly played successful-note feedback");

    // A partial physical mask must not downgrade the game's three/four-note result,
    // either during judgement resolution or when the waveform is synthesized.
    for(int count=3;count<=4;++count) {
        HapticEngine chordEngine;chordEngine.Configure(gameMapCfg);
        JudgementHaptics chord(chordEngine,gameMapCfg);chord.SetHookAvailable(true);
        chord.UpdatePhysicalFace(0x03);
        chord.OnJudgement(JudgementHaptics::Grade::Cool,false,false,false,false,false,false,count);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));chord.Tick();
        HapticEngine expected;expected.Configure(gameMapCfg);
        expected.Trigger(count==3 ? HapticEvent::Multi3 : HapticEvent::Multi4);
        std::vector<float> actualSamples(1024),expectedSamples(1024);
        chordEngine.RenderBlock(actualSamples.data(),512,48000);
        expected.RenderBlock(expectedSamples.data(),512,48000);
        require(actualSamples==expectedSamples,"partial physical mask downgraded a confirmed chord");
    }

    

#ifdef _WIN32
    // Default menu feedback must work without a rendered/stationary screen.
    for(auto action : {MenuAction::Up,MenuAction::Down,MenuAction::Left,MenuAction::Right,MenuAction::Confirm}) {
        HapticEngine menuEngine;menuEngine.Configure(gameMapCfg);
        JudgementHaptics menuState(menuEngine,gameMapCfg);menuState.SetHookAvailable(true);
        MenuHaptics menu(menuEngine,gameMapCfg,&menuState);
        menu.Submit(action);
        require(renderPeaks(menuEngine,16000).all>0.02f,"menu input required a screen response");
        menuState.OnGamePoll();
        menu.Submit(action);
        require(renderPeaks(menuEngine,512).all<0.001f,"menu feedback leaked into gameplay");
    }
#endif

    // The game's exact isSuccessNote flag gets its own additional two-stage effect.
    HapticEngine successOffEngine;successOffEngine.Configure(gameMapCfg);
    JudgementHaptics successOff(successOffEngine,gameMapCfg);successOff.SetHookAvailable(true);
    successOff.OnJudgement(JudgementHaptics::Grade::Cool,false,false,false,false,false,false,1);
    std::this_thread::sleep_for(std::chrono::milliseconds(2)); successOff.Tick();
    auto successOffPeak=renderPeaks(successOffEngine,1024);
    HapticEngine successOnEngine;successOnEngine.Configure(gameMapCfg);
    JudgementHaptics successOn(successOnEngine,gameMapCfg);successOn.SetHookAvailable(true);
    successOn.OnJudgement(JudgementHaptics::Grade::Cool,false,false,false,false,false,true,1);
    std::this_thread::sleep_for(std::chrono::milliseconds(2)); successOn.Tick();
    auto successOnPeak=renderPeaks(successOnEngine,1024);
    require(successOnPeak.all>successOffPeak.all, "Success/Chance note flag did not add stronger feedback");

    // Exposed chain frequency must actually alter the confirmed long-slide texture.
    ModConfig chainSlowCfg=fxCfg;chainSlowCfg.effects.chainFrequencyHz=10.0f;
    ModConfig chainFastCfg=fxCfg;chainFastCfg.effects.chainFrequencyHz=26.0f;
    HapticEngine chainSlow;chainSlow.Configure(chainSlowCfg);chainSlow.SetChainMask(0x02);
    HapticEngine chainFast;chainFast.Configure(chainFastCfg);chainFast.SetChainMask(0x02);
    std::vector<float> chainSlowOut(4800*2),chainFastOut(4800*2);
    chainSlow.RenderBlock(chainSlowOut.data(),4800,48000);
    chainFast.RenderBlock(chainFastOut.data(),4800,48000);
    double chainDifference=0.0;
    for(size_t i=0;i<chainSlowOut.size();++i) chainDifference += std::abs(chainSlowOut[i]-chainFastOut[i]);
    require(chainDifference>1.0, "chain_frequency_hz did not change the chain waveform");

    // In strict mode, if the game hook is unavailable, a gameplay candidate must stay silent.
    HapticEngine strictEngine;strictEngine.Configure(judgeCfg);
    JudgementHaptics strictJudge(strictEngine,judgeCfg);
    strictJudge.SetHookAvailable(false);
    strictJudge.Submit(HapticEvent::Cross,0x02,false);
    auto strictSilent=renderPeaks(strictEngine,128);
    require(strictSilent.all<0.001f, "strict mode emitted a raw gameplay press without a DIVA hook");

    // Outside gameplay, InputHaptics only proposes a menu action; it must not emit
    // a haptic itself. The Windows MenuHaptics layer validates the rendered response
    // before accepting that proposal.
    HapticEngine menuInputEngine;menuInputEngine.Configure(judgeCfg);
    JudgementHaptics menuJudge(menuInputEngine,judgeCfg);
    menuJudge.SetHookAvailable(true); // no game poll => outside gameplay/menu mode
    int menuSubmits=0;
    MenuAction lastMenuAction=MenuAction::Up;
    InputHaptics menuInput(menuInputEngine,judgeCfg,&menuJudge,[&](MenuAction a){++menuSubmits;lastMenuAction=a;});
    uint8_t menuReport[11]{};
    menuReport[0]=1;menuReport[1]=menuReport[2]=menuReport[3]=menuReport[4]=128;menuReport[8]=neutralDpad();
    menuInput.OnUsbReport(menuReport,sizeof(menuReport)); // prime menu mode
    menuReport[8]=4; // D-pad down
    menuInput.OnUsbReport(menuReport,sizeof(menuReport));
    require(menuSubmits==1 && lastMenuAction==MenuAction::Down, "menu Down was not proposed to the validator");
    auto menuUnvalidated=renderPeaks(menuInputEngine,128);
    require(menuUnvalidated.all<0.001f, "unvalidated menu input emitted a haptic directly");

    // Menu navigation and confirmation use deliberately different textures/strengths.
    HapticEngine menuNavEngine;menuNavEngine.Configure(judgeCfg);menuNavEngine.Trigger(HapticEvent::MenuDown);
    HapticEngine menuConfirmEngine;menuConfirmEngine.Configure(judgeCfg);menuConfirmEngine.Trigger(HapticEvent::MenuConfirm);
    auto menuNav=renderPeaks(menuNavEngine,512), menuConfirm=renderPeaks(menuConfirmEngine,512);
    require(menuNav.all>0.01f && menuConfirm.all>0.01f, "menu haptic waveforms are missing");
    require(std::abs(menuNav.all-menuConfirm.all)>0.01f, "menu navigation and confirm haptics are not differentiated");

    // Different grades should produce measurably different confirmation strength/texture.
    HapticEngine sadEngine;sadEngine.Configure(judgeCfg);
    JudgementHaptics sadJudge(sadEngine,judgeCfg);
    sadJudge.SetHookAvailable(true);sadJudge.OnGamePoll();
    sadJudge.Submit(HapticEvent::Cross,0x02,false);
    sadJudge.OnJudgement(JudgementHaptics::Grade::Sad,false,false,false,false,false,false,1); // Bad/SAD
    std::this_thread::sleep_for(std::chrono::milliseconds(2)); sadJudge.Tick();
    auto sadConfirmed=renderPeaks(sadEngine,256);
    require(coolConfirmed.all>sadConfirmed.all, "COOL and SAD confirmation strengths are not differentiated");

    // v0.5.0: parse the actual DSC chart. Simultaneous TARGET commands at the
    // same TIME become one multi-note group, and MODE_SELECT carries exact
    // Challenge Time state rather than relying on rendered black bars.
    std::vector<uint8_t> dsc;
    pushCmd(dsc,0x01,{0});       pushTarget(dsc,2); // Cross, hit 1.0 s
    pushCmd(dsc,0x01,{37000});   pushTarget(dsc,1); // Circle, hit 1.37 s
    pushCmd(dsc,0x01,{93000});   pushTarget(dsc,0); pushTarget(dsc,3); // Triangle+Square, hit 1.93 s
    pushCmd(dsc,0x01,{141000});  pushTarget(dsc,2); // Cross, hit 2.41 s
    pushCmd(dsc,0x1A,{31,1});
    pushCmd(dsc,0x01,{208000});  pushTarget(dsc,1); pushTarget(dsc,2); // Challenge Circle+Cross, hit 3.08 s
    pushCmd(dsc,0x1A,{31,3});
    pushCmd(dsc,0x01,{269000});  pushTarget(dsc,0); // normal, hit 3.69 s
    pushCmd(dsc,0x00,{});
    auto parsed=DscChart::Parse(dsc,L"synthetic.dsc",2,130);
    require(parsed.valid && parsed.groups.size()==6, "DSC parser did not produce the expected note groups");
    require(parsed.groups[2].faceMask==0x09, "DSC simultaneous Triangle+Square TARGETs were not grouped into mask 0x9");
    require(parsed.groups[4].challenge && !parsed.groups[5].challenge, "DSC MODE_SELECT Challenge state was not attached to target groups");

    ChartAwareness awareness;
    awareness.SetChart(parsed);
    const auto base=ChartAwareness::TP{} + std::chrono::duration_cast<ChartAwareness::Clock::duration>(std::chrono::duration<double>(100.0));
    auto at=[&](double chartSec){ return base + std::chrono::duration_cast<ChartAwareness::Clock::duration>(std::chrono::duration<double>(chartSec)); };
    awareness.ObserveJudgement(at(1.0),false,false);
    awareness.ObserveJudgement(at(1.37),false,false);
    awareness.ObserveJudgement(at(1.93),false,false);
    auto lockMatch=awareness.ObserveJudgement(at(2.41),false,false);
    require(lockMatch.matched, "DSC judgement clock did not lock after four confirmed notes");
    auto challengeMatch=awareness.ObserveJudgement(at(3.08),false,false);
    require(challengeMatch.matched && challengeMatch.challengeTransition && challengeMatch.challengeActive,
            "DSC Challenge Time start did not transition on the first challenge target");
    require(challengeMatch.group.faceMask==0x06, "locked DSC matcher did not return the exact 2-note chart mask");
    auto challengeEnd=awareness.ObserveJudgement(at(3.69),false,false);
    require(challengeEnd.matched && challengeEnd.challengeTransition && !challengeEnd.challengeActive,
            "DSC Challenge Time end did not transition back to normal");


    // v0.5.1: raw DSC can be recovered from a larger process-memory blob even
    // when Mega Mix+ never opens an individual .dsc through Win32 CreateFile.
    std::vector<uint8_t> memoryDsc;
    for(int n=0;n<24;++n) {
        pushCmd(memoryDsc,0x01,{n*25000});
        if(n==10){ pushTarget(memoryDsc,0); pushTarget(memoryDsc,3); }
        else pushTarget(memoryDsc,n%4);
    }
    pushCmd(memoryDsc,0x00,{});
    std::vector<uint8_t> memoryBlob(4096,0xCD);
    const size_t embeddedOffset=memoryBlob.size();
    memoryBlob.insert(memoryBlob.end(),memoryDsc.begin(),memoryDsc.end());
    memoryBlob.insert(memoryBlob.end(),8192,0xA5);
    auto probed=FindBestDscCandidate(std::span<const uint8_t>(memoryBlob.data(),memoryBlob.size()));
    require(probed.has_value(), "DSC process-memory probe failed to locate a valid embedded chart");
    require(probed->offset==embeddedOffset, "DSC memory probe did not choose the earliest/full TIME stream");
    require(probed->targetCount==25, "DSC memory probe target count was incorrect");
    std::vector<uint8_t> recovered(memoryBlob.begin()+static_cast<std::ptrdiff_t>(probed->offset),
                                   memoryBlob.begin()+static_cast<std::ptrdiff_t>(probed->offset+probed->length));
    auto recoveredChart=DscChart::Parse(recovered,L"<memory-test>",2,130);
    require(recoveredChart.valid && recoveredChart.groups.size()==24, "recovered memory DSC did not parse into the expected chart groups");
    require(recoveredChart.groups[10].faceMask==0x09, "memory-recovered DSC lost its simultaneous 2-note mask");

    // Challenge Time makes normal gameplay events tactilely stronger/different.
    ModConfig challengeCfg=fxCfg;challengeCfg.effects.challengeNoteMultiplier=1.25f;
    HapticEngine normal;normal.Configure(challengeCfg);normal.Trigger(HapticEvent::Cross);
    HapticEngine challenge;challenge.Configure(challengeCfg);challenge.SetChallengeActive(true);challenge.Trigger(HapticEvent::Cross);
    auto pn=renderPeaks(normal,192), pc=renderPeaks(challenge,192);
    require(pc.all>pn.all, "Challenge Time multiplier did not increase gameplay haptic intensity");

    std::cout<<"core smoke ok peak="<<mixed.all
             <<" multi2="<<multi2Peak.all
             <<" slide-L/R="<<ps.left<<"/"<<ps.right
             <<" game-multi="<<gameSinglePeak.all<<","<<game2Peak.all<<","<<game3Peak.all<<","<<game4Peak.all<<"\n";
    return 0;
}
