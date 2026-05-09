#include "script.h"

#include "GitInfo.h"
#include "InputConfiguration.h"
#include "ScriptMenuUtils.h"

#include "LanguageManager.h"
#include "ScriptSettings.hpp"

#include "Constants.h"
#include "UpdateChecker.h"
#include "Compatibility.h"

#include "Input/CarControls.hpp"
#include "Input/NativeController.h"
#include "Input/keyboard.h"

#include "Util/UIUtils.h"
#include "Util/Strings.hpp"
#include "Util/GUID.h"
#include "Util/MathExt.h"
#include "Util/Logger.hpp"
#include "Util/ScriptUtils.h"
#include "Util/AddonSpawnerCache.h"
#include "Util/Paths.h"

#include "Memory/MemoryPatcher.hpp"
#include "Memory/VehicleExtensions.hpp"

#include "VehicleConfig.h"
#include "SteeringAnim.h"
#include "BlockableControls.h"

#include "AWD.h"
#include "DrivingAssists.h"
#include "CruiseControl.h"
#include "WheelInput.h"

#include <menu.h>
#include <inc/main.h>
#include <inc/natives.h>

#include <fmt/format.h>

#include <Windows.h>
#include <shellapi.h>
#include <string>
#include <mutex>
#include <filesystem>

using VExt = VehicleExtensions;

extern ReleaseInfo g_releaseInfo;
extern std::mutex g_releaseInfoMutex;

extern bool g_notifyUpdate;
extern std::mutex g_notifyUpdateMutex;

extern bool g_checkUpdateDone;
extern std::mutex g_checkUpdateDoneMutex;

extern NativeMenu::Menu g_menu;
extern CarControls g_controls;
extern ScriptSettings g_settings;

extern std::vector<VehicleConfig> g_vehConfigs;

struct SFont {
    int ID;
    std::string Name;
};

struct STagInfo {
    std::string Tag;
    std::string Info;
};

namespace {
    const std::string escapeKey = "BACKSPACE";
    const std::string skipKey = "RIGHT";
    const std::string modUrl = "https://www.gta5-mods.com/scripts/manual-transmission-ikt";

    const std::vector<std::string> gearboxModes = {
        "Sequential",
        "H-pattern",
        "Automatic"
    };

    const std::vector<SFont> fonts {
        { 0, "Chalet London" },
        { 1, "Sign Painter" },
        { 2, "Slab Serif" },
        { 4, "Chalet Cologne" },
        { 7, "Pricedown" },
    };

    const std::vector<std::string> speedoTypes {
        "off",
        "kph",
        "mph",
        "ms"
    };

    const std::vector<std::string> tcsStrings{
        "Brakes", "Throttle"
    };

    const std::vector<std::string> notifyLevelStrings{
        "Debug",
        "Info",
        "UI",
        "None"
    };

    const std::vector<std::string> camAttachPoints {
        "Player head",
        "Vehicle (1)",
        "Vehicle (2)"
    };

    const std::vector<std::string> ffbCurveTypes{
        "Boosted",
        "Gamma/Linear",
    };

    const std::vector<std::string> PitchModeNames {
        "Horizon",
        "Vehicle",
        "Dynamic"
    };

    std::vector<std::string> diDevicesInfo{ "Press Enter to refresh." };

    std::string MenuSubtitleConfig() {
        std::string cfgName = "Base";
        if (g_settings.ConfigActive())
            cfgName = g_settings().Name;
        return fmt::format("CFG: [{}]", cfgName);
    }

    std::string FormatDeviceGuidName(GUID guid) {
        std::string deviceNameCfg = g_settings.GUIDToDeviceName(guid);
        DirectInputDeviceInfo* deviceEntry = g_controls.GetWheel().GetDeviceInfo(guid);
        int index = g_settings.GUIDToDeviceIndex(guid);
        if (deviceEntry) {
            return fmt::format("[{}] {}", index, deviceNameCfg);
        }
        else {
            return fmt::format("[{}] {} (Disconnected)", index, deviceNameCfg);
        }
    }

    void incVal(float& value, float max, float step) {
        if (value + step > max) {
            value = max;
            return;
        }
        value += step;
    }

    void decVal(float& value, float min, float step) {
        if (value - step < min) {
            value = min;
            return;
        }
        value -= step;
    }
}

///////////////////////////////////////////////////////////////////////////////
//                             Menu stuff
///////////////////////////////////////////////////////////////////////////////
void onMenuInit() {
    g_menu.ReadSettings();
}

void saveAllSettings() {
    g_settings.SaveGeneral();
    g_settings.SaveController(&g_controls);
    g_settings.SaveWheel();
}

void onMenuClose() {
    saveAllSettings();
    loadConfigs();
}

void update_mainmenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("menu.title"));
    g_menu.Subtitle(fmt::format("~b~{}{}", Constants::DisplayVersion, GIT_DIFF));

    if (Paths::GetModPathChanged()) {
        g_menu.Option(lang.Tr("main.modpathwarning"), NativeMenu::solidRed,
            { lang.Tr("main.modpathwarning.desc1"),
              lang.Tr("main.modpathwarning.desc2"),
              fmt::format("{} {}", lang.Tr("main.modpathwarning.oldpath"), Paths::GetInitialModPath()),
              fmt::format("{} {}", lang.Tr("main.modpathwarning.newpath"), Paths::GetModPath()) });
    }

    if (g_settings.Error()) {
        g_menu.Option(lang.Tr("main.settingserror"), NativeMenu::solidRed,
            { lang.Tr("main.settingserror.desc1"),
              lang.Tr("main.settingserror.desc2"),
              lang.Tr("main.settingserror.desc3") });
    }

    if (logger.Error()) {
        g_menu.Option(lang.Tr("main.loggingerror"), NativeMenu::solidRed,
            { lang.Tr("main.loggingerror.desc1"),
              lang.Tr("main.loggingerror.desc2"),
              lang.Tr("main.loggingerror.desc3") });
    }

    if (MemoryPatcher::Error) {
        g_menu.Option(lang.Tr("main.patcherror"), NativeMenu::solidRed, 
            { lang.Tr("main.patcherror.desc1"),
              lang.Tr("main.patcherror.desc2"), 
              lang.Tr("main.patcherror.desc3") });
    }

    {
        std::unique_lock releaseInfoLock(g_releaseInfoMutex, std::try_to_lock);
        std::unique_lock notifyUpdateLock(g_notifyUpdateMutex, std::try_to_lock);
        if (notifyUpdateLock.owns_lock() && releaseInfoLock.owns_lock()) {
            if (g_notifyUpdate) {
                std::vector<std::string> bodyLines =
                    NativeMenu::split(g_releaseInfo.Body, '\n');

                std::vector<std::string> extra = {
                    fmt::format(lang.Tr("main.update.newversion"), g_releaseInfo.Version.c_str()),
                    fmt::format(lang.Tr("main.update.releasedate"), g_releaseInfo.TimestampPublished.c_str()),
                    lang.Tr("main.update.changelog")
                };

                for (const auto& line : bodyLines) {
                    extra.push_back(line);
                }

                if (g_menu.OptionPlus(lang.Tr("main.update.available"), extra, nullptr, [] {
                    g_settings.Update.IgnoredVersion = g_releaseInfo.Version;
                    g_notifyUpdate = false;
                    saveAllSettings();
                    }, nullptr, lang.Tr("main.update.info"),
                    { lang.Tr("main.update.desc1"),
                        lang.Tr("main.update.desc2") })) {
                    WAIT(20);
                    PAD::SET_CONTROL_VALUE_NEXT_FRAME(0, ControlFrontendPause, 1.0f);
                    ShellExecuteA(0, 0, modUrl.c_str(), 0, 0, SW_SHOW);
                }
            }
        }
    }

    bool tempEnableRead = g_settings.MTOptions.Enable;
    if (g_menu.BoolOption(lang.Tr("main.enablemt"), tempEnableRead,
        { lang.Tr("main.enablemt.desc") })) {
        toggleManual(!g_settings.MTOptions.Enable);
    }

    int tempShiftMode = static_cast<int>(g_settings().MTOptions.ShiftMode);

    std::vector<std::string> detailsTemp {
        lang.Tr("main.gearbox.desc")
    };

    if (g_settings.ConfigActive()) {
        detailsTemp.push_back(fmt::format("CFG: [{}]", g_settings().Name));
    }

    g_menu.StringArray(lang.Tr("main.gearbox"), gearboxModes, tempShiftMode,
        detailsTemp);

    if (tempShiftMode != static_cast<int>(g_settings().MTOptions.ShiftMode)) {
        setShiftMode(static_cast<EShiftMode>(tempShiftMode));
    }

    g_menu.MenuOption(lang.Tr("menu.settings"), "settingsmenu", { lang.Tr("menu.settings.desc") });
    g_menu.MenuOption(lang.Tr("menu.controls"), "controlsmenu", { lang.Tr("menu.controls.desc") });
    g_menu.MenuOption(lang.Tr("menu.driveassist"), "driveassistmenu", { lang.Tr("menu.driveassist.desc") });
    g_menu.MenuOption(lang.Tr("menu.gameassist"), "gameassistmenu", { lang.Tr("menu.gameassist.desc") });
    g_menu.MenuOption(lang.Tr("menu.misc"), "miscoptionsmenu", { lang.Tr("menu.misc.desc") });
    g_menu.MenuOption(lang.Tr("menu.hud"), "hudmenu", { lang.Tr("menu.hud.desc") });
    g_menu.MenuOption(lang.Tr("menu.dev"), "devoptionsmenu", { lang.Tr("menu.dev.desc") });

    if (g_settings.Debug.DisableInputDetect) {
        int activeIndex = g_controls.PrevInput;
        std::vector<std::string> inputNames {
            lang.Tr("main.input.keyboard"), lang.Tr("main.input.controller"), lang.Tr("main.input.wheel")
        };
        if (g_menu.StringArray(lang.Tr("main.activeinput"), inputNames, activeIndex, { lang.Tr("main.activeinput.desc_manual") })) {
            g_controls.PrevInput = static_cast<CarControls::InputDevices>(activeIndex);
        }
    }
    else {
        int activeIndex = 0;
        std::string activeInputName;
        switch (g_controls.PrevInput) {
        case CarControls::Keyboard:
            activeInputName = lang.Tr("main.input.keyboard");
            break;
        case CarControls::Controller:
            activeInputName = lang.Tr("main.input.controller");
            break;
        case CarControls::Wheel:
            activeInputName = lang.Tr("main.input.wheel");
            break;
        }
        g_menu.StringArray(lang.Tr("main.activeinput"), { activeInputName }, activeIndex, 
            { lang.Tr("main.activeinput.desc_auto") });
    }
}

void update_settingsmenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("menu.title"));
    g_menu.Subtitle(lang.Tr("settings.subtitle"));

    g_menu.MenuOption(lang.Tr("settings.features"), "featuresmenu", { lang.Tr("settings.features.desc") });
    g_menu.MenuOption(lang.Tr("settings.finetune"), "finetuneoptionsmenu", { lang.Tr("settings.finetune.desc") });
    g_menu.MenuOption(lang.Tr("settings.shifting"), "shiftingoptionsmenu", { lang.Tr("settings.shifting.desc") });
    g_menu.MenuOption(lang.Tr("settings.auto_finetune"), "finetuneautooptionsmenu", { lang.Tr("settings.auto_finetune.desc") });
    g_menu.MenuOption(lang.Tr("settings.vehconfig"), "vehconfigmenu", { lang.Tr("settings.vehconfig.desc1"), lang.Tr("settings.vehconfig.desc2"), lang.Tr("settings.vehconfig.desc3") });
}

void update_featuresmenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("features.title"));

    if (g_settings.ConfigActive())
        g_menu.Subtitle(fmt::format(lang.Tr("features.subtitle.partial"), g_settings().Name));
    else
        g_menu.Subtitle(MenuSubtitleConfig());

    g_menu.BoolOption(lang.Tr("features.engdamage"), g_settings.MTOptions.EngDamage, { lang.Tr("features.engdamage.desc") });
    g_menu.BoolOption(lang.Tr("features.engstallh"), g_settings.MTOptions.EngStallH, { lang.Tr("features.engstallh.desc") });
    g_menu.BoolOption(lang.Tr("features.engstalls"), g_settings.MTOptions.EngStallS, { lang.Tr("features.engstalls.desc") });
    g_menu.BoolOption(lang.Tr("features.engbrake"), g_settings.MTOptions.EngBrake, { lang.Tr("features.engbrake.desc") });
    g_menu.BoolOption(lang.Tr("features.englock"), g_settings.MTOptions.EngLock, { lang.Tr("features.englock.desc") });
    g_menu.BoolOption(lang.Tr("features.finalgearrpm"), g_settings.MTOptions.FinalGearRPMLimit, { lang.Tr("features.finalgearrpm.desc") });
    g_menu.BoolOption(lang.Tr("features.speedlimiter"), g_settings().MTOptions.SpeedLimiter.Enable, { lang.Tr("features.speedlimiter.desc") });
    g_menu.MenuOption(lang.Tr("features.speedlimiter.settings"), "speedlimitersettingsmenu");
    g_menu.BoolOption(lang.Tr("features.clutchshifth"), g_settings().MTOptions.ClutchShiftH, { lang.Tr("features.clutchshifth.desc") });
    g_menu.BoolOption(lang.Tr("features.clutchshifts"), g_settings().MTOptions.ClutchShiftS, { lang.Tr("features.clutchshifts.desc") });
    g_menu.BoolOption(lang.Tr("features.clutchcreep"), g_settings().MTOptions.ClutchCreep, { lang.Tr("features.clutchcreep.desc") });
}

void update_finetuneoptionsmenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("finetune.title"));
    g_menu.Subtitle(MenuSubtitleConfig());

    g_menu.FloatOption(lang.Tr("finetune.clutchbite"), g_settings().MTParams.ClutchThreshold, 0.0f, 1.0f, 0.05f,
        { lang.Tr("finetune.clutchbite.desc") });
    g_menu.FloatOption(lang.Tr("finetune.stallrpm"), g_settings().MTParams.StallingRPM, 0.0f, 0.2f, 0.01f,
        { lang.Tr("finetune.stallrpm.desc1"),
          lang.Tr("finetune.stallrpm.desc2"),
          lang.Tr("finetune.stallrpm.desc3")});
    g_menu.FloatOption(lang.Tr("finetune.stallrate"), g_settings().MTParams.StallingRate, 0.0f, 10.0f, 0.05f,
        { lang.Tr("finetune.stallrate.desc") });
    g_menu.FloatOption(lang.Tr("finetune.stallslip"), g_settings().MTParams.StallingSlip, 0.0f, 1.0f, 0.05f,
        { lang.Tr("finetune.stallslip.desc") });

    g_menu.FloatOption(lang.Tr("finetune.rpmdamage"), g_settings().MTParams.RPMDamage, 0.0f, 10.0f, 0.05f,
        { lang.Tr("finetune.rpmdamage.desc") });
    g_menu.FloatOption(lang.Tr("finetune.misshiftdamage"), g_settings().MTParams.MisshiftDamage, 0, 100, 5,
        { lang.Tr("finetune.misshiftdamage.desc") });
    g_menu.FloatOption(lang.Tr("finetune.engbrakethresh"), g_settings().MTParams.EngBrakeThreshold, 0.0f, 1.0f, 0.05f,
        { lang.Tr("finetune.engbrakethresh.desc") });
    g_menu.FloatOption(lang.Tr("finetune.engbrakepower"), g_settings().MTParams.EngBrakePower, 0.0f, 5.0f, 0.05f,
        { lang.Tr("finetune.engbrakepower.desc") });

    // Clutch creep params
    g_menu.FloatOption(lang.Tr("finetune.creepidlerpm"), g_settings().MTParams.CreepIdleRPM, 0.0f, 0.5f, 0.01f,
        { lang.Tr("finetune.creepidlerpm.desc1"),
          lang.Tr("finetune.creepidlerpm.desc2")});
    g_menu.FloatOption(lang.Tr("finetune.creepthrottle"), g_settings().MTParams.CreepIdleThrottle, 0.0f, 1.0f, 0.01f,
        { lang.Tr("finetune.creepthrottle.desc") });
}

void update_shiftingoptionsmenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("shifting.title"));
    g_menu.Subtitle(MenuSubtitleConfig());

    g_menu.BoolOption(lang.Tr("shifting.upshiftcut"), g_settings().ShiftOptions.UpshiftCut,
        { lang.Tr("shifting.upshiftcut.desc1"),
            lang.Tr("shifting.upshiftcut.desc2")});
    g_menu.BoolOption(lang.Tr("shifting.downshiftblip"), g_settings().ShiftOptions.DownshiftBlip,
        { lang.Tr("shifting.downshiftblip.desc1"),
            lang.Tr("shifting.downshiftblip.desc2") });
    g_menu.BoolOption(lang.Tr("shifting.downshiftprot"), g_settings().ShiftOptions.DownshiftProtect,
        { lang.Tr("shifting.downshiftprot.desc1"),
            lang.Tr("shifting.downshiftprot.desc2") });
    g_menu.FloatOption(lang.Tr("shifting.clutchratemult"), g_settings().ShiftOptions.ClutchRateMult, 0.05f, 20.0f, 0.05f,
        { lang.Tr("shifting.clutchratemult.desc1"),
            lang.Tr("shifting.clutchratemult.desc2") });

    g_menu.FloatOption(lang.Tr("shifting.rpmtolerance"), g_settings().ShiftOptions.RPMTolerance, 0.0f, 1.0f, 0.05f,
        { lang.Tr("shifting.rpmtolerance.desc1"),
            lang.Tr("shifting.rpmtolerance.desc2"),
            fmt::format(lang.Tr("shifting.rpmtolerance.desc3"), g_settings().MTOptions.ClutchShiftH ? "~g~en" : "~r~dis")
        });
}

void update_finetuneautooptionsmenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("auto_finetune.title"));
    g_menu.Subtitle(MenuSubtitleConfig());

    g_menu.FloatOption(lang.Tr("auto_finetune.upshiftload"), g_settings().AutoParams.UpshiftLoad, 0.01f, 0.20f, 0.01f,
        { lang.Tr("auto_finetune.upshiftload.desc")});
    g_menu.FloatOption(lang.Tr("auto_finetune.upshifttimeout"), g_settings().AutoParams.UpshiftTimeoutMult, 0.00f, 10.00f, 0.05f,
        { lang.Tr("auto_finetune.upshifttimeout.desc1"),
          lang.Tr("auto_finetune.upshifttimeout.desc2") });
    g_menu.FloatOption(lang.Tr("auto_finetune.downshiftload"), g_settings().AutoParams.DownshiftLoad, 0.30f, 1.00f, 0.01f,
        { lang.Tr("auto_finetune.downshiftload.desc") });
    g_menu.FloatOption(lang.Tr("auto_finetune.downshifttimeout"), g_settings().AutoParams.DownshiftTimeoutMult, 0.05f, 10.00f, 0.05f,
        { lang.Tr("auto_finetune.downshifttimeout.desc1"),
          lang.Tr("auto_finetune.downshifttimeout.desc2") });
    g_menu.FloatOption(lang.Tr("auto_finetune.nextgearminrpm"), g_settings().AutoParams.NextGearMinRPM, 0.20f, 0.50f, 0.01f, 
        { lang.Tr("auto_finetune.nextgearminrpm.desc") });
    g_menu.FloatOption(lang.Tr("auto_finetune.currgearminrpm"), g_settings().AutoParams.CurrGearMinRPM, 0.20f, 0.50f, 0.01f, 
        { lang.Tr("auto_finetune.currgearminrpm.desc") });
    g_menu.FloatOption(lang.Tr("auto_finetune.economyrate"), g_settings().AutoParams.EcoRate, 0.01f, 0.50f, 0.01f,
        { lang.Tr("auto_finetune.economyrate.desc1"),
          lang.Tr("auto_finetune.economyrate.desc2") });
    g_menu.BoolOption(lang.Tr("auto_finetune.useatcu"), g_settings().AutoParams.UsingATCU,
        { lang.Tr("auto_finetune.useatcu.desc1"),
          lang.Tr("auto_finetune.useatcu.desc2") });
}

void update_vehconfigmenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("vehconfig.title"));
    g_menu.Subtitle(MenuSubtitleConfig());

    if (g_menu.Option(lang.Tr("vehconfig.create"), 
        { lang.Tr("vehconfig.create.desc1"),
          lang.Tr("vehconfig.create.desc2"),
          lang.Tr("vehconfig.create.desc3") })) {
        SaveVehicleConfig();
    }

    if (g_menu.Option(lang.Tr("vehconfig.reload"), 
        { lang.Tr("vehconfig.reload.desc") })) {
        loadConfigs();
    }

    if (g_settings.ConfigActive()) {
        bool sel = false;
        g_menu.OptionPlus(fmt::format(lang.Tr("vehconfig.active"), g_settings().Name), {}, &sel, nullptr, nullptr, "",
            { lang.Tr("vehconfig.active.desc1"),
              lang.Tr("vehconfig.active.desc2") });
        if (sel) {
            auto extras = FormatVehicleConfig(g_settings(), gearboxModes);
            g_menu.OptionPlusPlus(extras, lang.Tr("vehconfig.overview"));
        }
    }
    else {
        g_menu.Option(lang.Tr("vehconfig.noactive"));
    }

    g_menu.BoolOption(lang.Tr("vehconfig.savefull"), g_settings.Misc.SaveFullConfig,
        { lang.Tr("vehconfig.savefull.desc1"),
            lang.Tr("vehconfig.savefull.desc2") });

    for (const auto& vehConfig : g_vehConfigs) {
        bool sel = false;
        std::vector<std::string> descr = {
            lang.Tr("vehconfig.config.desc1"),
            lang.Tr("vehconfig.config.desc2")
        };
        g_menu.OptionPlus(vehConfig.Name, {}, &sel, nullptr, nullptr, "", descr);
        if (sel) {
            auto extras = FormatVehicleConfig(vehConfig, gearboxModes);
            g_menu.OptionPlusPlus(extras, lang.Tr("vehconfig.overview"));
        }
    }

    if (g_vehConfigs.empty()) {
        g_menu.OptionPlus(lang.Tr("vehconfig.notfound"), {}, nullptr, nullptr, nullptr, lang.Tr("vehconfig.instructions"), {
            lang.Tr("vehconfig.notfound.desc1"),
            lang.Tr("vehconfig.notfound.desc2"),
            lang.Tr("vehconfig.notfound.desc3")
            });
    }
}


void update_controlsmenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("controls.title"));
    g_menu.Subtitle("");

    g_menu.MenuOption(lang.Tr("controls.controller"), "controllermenu",
        { lang.Tr("controls.controller.desc") });

    g_menu.MenuOption(lang.Tr("controls.keyboard"), "keyboardmenu",
        { lang.Tr("controls.keyboard.desc") });

    g_menu.MenuOption(lang.Tr("controls.wheel"), "wheelmenu",
        { lang.Tr("controls.wheel.desc") });

    g_menu.MenuOption(lang.Tr("controls.steerassist"), "steeringassistmenu",
        { lang.Tr("controls.steerassist.desc") });

    g_menu.MenuOption(lang.Tr("controls.vehspecific"), "controlsvehconfmenu",
        { lang.Tr("controls.vehspecific.desc") });
}

void update_controlsvehconfmenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("controlsveh.title"));
    g_menu.Subtitle(MenuSubtitleConfig());

    g_menu.BoolOption(lang.Tr("controlsveh.esoverride"), g_settings().Steering.CustomSteering.UseCustomLock,
        { lang.Tr("controlsveh.es"),
          lang.Tr("controlsveh.esoverride.desc") });

    g_menu.FloatOptionCb(lang.Tr("controlsveh.esrotation"), g_settings().Steering.CustomSteering.SoftLock, 180.0f, 2880.0f, 30.0f, GetKbEntryFloat,
        { lang.Tr("controlsveh.es"),
          lang.Tr("controlsveh.esrotation.desc") });

    g_menu.FloatOptionCb(lang.Tr("controlsveh.esmult"), g_settings().Steering.CustomSteering.SteeringMult, 0.01f, 2.0f, 0.01f, GetKbEntryFloat,
        { lang.Tr("controlsveh.es"),
          lang.Tr("controlsveh.esmult.desc") });

    g_menu.FloatOptionCb(lang.Tr("controlsveh.esreduction"), g_settings().Steering.CustomSteering.SteeringReduction, 0.0f, 2.0f, 0.01f, GetKbEntryFloat,
        { lang.Tr("controlsveh.es"),
          lang.Tr("controlsveh.esreduction.desc1"),
          lang.Tr("controlsveh.esreduction.desc2"),
          lang.Tr("controlsveh.esreduction.desc3"),
          lang.Tr("controlsveh.esreduction.desc4")});

    g_menu.FloatOptionCb(lang.Tr("controlsveh.ffbsatmult"), g_settings().Steering.Wheel.SATMult, 0.05f, 10.0f, 0.05f, GetKbEntryFloat,
        { lang.Tr("controlsveh.wheel"),
          lang.Tr("controlsveh.ffbsatmult.desc1"),
          lang.Tr("controlsveh.ffbsatmult.desc2") });

    bool showDynamicFfbCurveBox = false;
    if (g_menu.OptionPlus(fmt::format(lang.Tr("controlsveh.ffbcurve"), g_settings().Steering.Wheel.CurveMult), {}, &showDynamicFfbCurveBox,
        [=] { return incVal(g_settings().Steering.Wheel.CurveMult, 5.00f, 0.01f); },
        [=] { return decVal(g_settings().Steering.Wheel.CurveMult, 0.01f, 0.01f); },
        lang.Tr("controlsveh.responsecurve"), {
            lang.Tr("controlsveh.wheel"),
            lang.Tr("controlsveh.ffbcurve.desc1"),
            lang.Tr("controlsveh.ffbcurve.desc2") })) {
        float val = g_settings.Wheel.FFB.ResponseCurve;
        if (GetKbEntryFloat(val)) {
            g_settings.Wheel.FFB.ResponseCurve = val;
        }
    }

    if (showDynamicFfbCurveBox) {
        auto extras = ShowDynamicFfbCurve(abs(WheelInput::GetFFBConstantForce()),
            g_settings.Wheel.FFB.ResponseCurve * g_settings().Steering.Wheel.CurveMult,
            g_settings.Wheel.FFB.FFBProfile);
        g_menu.OptionPlusPlus(extras, lang.Tr("controlsveh.responsecurve"));
    }

    g_menu.FloatOptionCb(lang.Tr("controlsveh.softlock"), g_settings().Steering.Wheel.SoftLock, 180.0f, 2880.0f, 30.0f, GetKbEntryFloat,
        { lang.Tr("controlsveh.softlock.desc") });

    g_menu.FloatOptionCb(lang.Tr("controlsveh.steermult"), g_settings().Steering.Wheel.SteeringMult, 0.1f, 2.0f, 0.01f, GetKbEntryFloat,
        { lang.Tr("controlsveh.steermult.desc") });
}

void update_controllermenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("controller.title"));
    g_menu.Subtitle("");

    g_menu.BoolOption(lang.Tr("controller.nativeinput"), g_settings.Controller.Native.Enable,
        { lang.Tr("controller.nativeinput.desc") });

    if (g_settings.Controller.Native.Enable) {
        g_menu.MenuOption(lang.Tr("controller.bindingsnative"), "controllerbindingsnativemenu",
            { lang.Tr("controller.bindings.desc") });
    }
    else {
        g_menu.MenuOption(lang.Tr("controller.bindingsxinput"), "controllerbindingsxinputmenu",
            { lang.Tr("controller.bindings.desc") });
    }

    g_menu.BoolOption(lang.Tr("controller.enginetoggle"), g_settings.Controller.ToggleEngine,
        { lang.Tr("controller.enginetoggle.desc1"),
            lang.Tr("controller.enginetoggle.desc2") });

    g_menu.IntOption(lang.Tr("controller.longpress"), g_settings.Controller.HoldTimeMs, 100, 5000, 50,
        { lang.Tr("controller.longpress.desc") });

    g_menu.IntOption(lang.Tr("controller.maxtap"), g_settings.Controller.MaxTapTimeMs, 50, 1000, 10,
        { lang.Tr("controller.maxtap.desc") });

    g_menu.FloatOption(lang.Tr("controller.triggervalue"), g_settings.Controller.TriggerValue, 0.25, 1.0, 0.05,
        { lang.Tr("controller.triggervalue.desc") });

    g_menu.BoolOption(lang.Tr("controller.blockcar"), g_settings.Controller.BlockCarControls,
        { lang.Tr("controller.blockcar.desc1"),
            lang.Tr("controller.blockcar.desc2") });

    g_menu.BoolOption(lang.Tr("controller.blockhpattern"), g_settings.Controller.BlockHShift,
        { lang.Tr("controller.blockhpattern.desc") });

    g_menu.BoolOption(lang.Tr("controller.ignoreshifts"), g_settings.Controller.IgnoreShiftsUI,
        { lang.Tr("controller.ignoreshifts.desc") });

    if (!g_settings.Controller.Native.Enable) {
        g_menu.BoolOption(lang.Tr("controller.customdeadzone"), g_settings.Controller.CustomDeadzone,
            { lang.Tr("controller.customdeadzone.desc") });

        if (g_settings.Controller.CustomDeadzone) {
            float ljVal = static_cast<float>(g_settings.Controller.DeadzoneLeftThumb);
            if (g_menu.FloatOptionCb(lang.Tr("controller.deadzoneleft"), ljVal, 0.0f, 32767.0f, 1.0f, GetKbEntryFloat,
                { lang.Tr("controller.deadzoneleft.desc"),
                  fmt::format("{} {}", lang.Tr("controller.deadzone.default"), XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) })) {
                g_settings.Controller.DeadzoneLeftThumb = static_cast<int>(ljVal);
            }

            float rjVal = static_cast<float>(g_settings.Controller.DeadzoneRightThumb);
            if (g_menu.FloatOptionCb(lang.Tr("controller.deadzoneright"), rjVal, 0.0f, 32767.0f, 1.0f, GetKbEntryFloat,
                { lang.Tr("controller.deadzoneright.desc"),
                  fmt::format("{} {}", lang.Tr("controller.deadzone.default"), XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE) })) {
                g_settings.Controller.DeadzoneRightThumb = static_cast<int>(rjVal);
            }
        }
    }
}

void update_controllerbindingsnativemenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("controller.bindings.title"));
    g_menu.Subtitle(lang.Tr("controller.bindings.nativemode"));

    std::vector<std::string> blockableControlsHelp;
    const auto& blockableControls = BlockableControls::GetList();
    blockableControlsHelp.reserve(blockableControls.size());
    for (const auto& control : blockableControls) {
        blockableControlsHelp.emplace_back(control.Text);
    }

    int oldIndexUp = BlockableControls::GetIndex(g_controls.ControlNativeBlocks[static_cast<int>(CarControls::LegacyControlType::ShiftUp)]);
    if (g_menu.StringArray(lang.Tr("controller.bindings.shiftupblocks"), blockableControlsHelp, oldIndexUp)) {
        g_controls.ControlNativeBlocks[static_cast<int>(CarControls::LegacyControlType::ShiftUp)] = blockableControls[oldIndexUp].Control;
    }

    int oldIndexDown = BlockableControls::GetIndex(g_controls.ControlNativeBlocks[static_cast<int>(CarControls::LegacyControlType::ShiftDown)]);
    if (g_menu.StringArray(lang.Tr("controller.bindings.shiftdownblocks"), blockableControlsHelp, oldIndexDown)) {
        g_controls.ControlNativeBlocks[static_cast<int>(CarControls::LegacyControlType::ShiftDown)] = blockableControls[oldIndexDown].Control;
    }

    int oldIndexClutch = BlockableControls::GetIndex(g_controls.ControlNativeBlocks[static_cast<int>(CarControls::LegacyControlType::Clutch)]);
    if (g_menu.StringArray(lang.Tr("controller.bindings.clutchblocks"), blockableControlsHelp, oldIndexClutch)) {
        g_controls.ControlNativeBlocks[static_cast<int>(CarControls::LegacyControlType::Clutch)] = blockableControls[oldIndexClutch].Control;
    }

    std::vector<std::string> controllerInfo = {
        lang.Tr("controller.bindings.pressrightclear"),
        lang.Tr("controller.bindings.pressreturnconfig"),
        "",
    };

    for (const auto& input : g_controls.LegacyControls) {
        controllerInfo.back() = input.Description;
        controllerInfo.push_back(fmt::format(lang.Tr("controller.bindings.assignedto"), NativeController::GetControlName(input.Control), input.Control));

        if (g_menu.OptionPlus(fmt::format(lang.Tr("controller.bindings.assign"), input.Name), controllerInfo, nullptr, std::bind(clearLControllerButton, input.ConfigTag), nullptr, lang.Tr("controller.bindings.currentsetting"))) {
            WAIT(500);
            bool result = configLControllerButton(input.ConfigTag);
            if (!result)
                UI::Notify(WARN, fmt::format(lang.Tr("controller.bindings.cancelled"), input.Name));
            WAIT(500);
        }
        controllerInfo.pop_back();
    }
}

void update_controllerbindingsxinputmenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("controller.bindings.title"));
    g_menu.Subtitle(lang.Tr("controller.bindings.xinputmode"));

    std::vector<std::string> blockableControlsHelp;
    const auto& blockableControls = BlockableControls::GetList();
    blockableControlsHelp.reserve(blockableControls.size());
    for (const auto& control : blockableControls) {
        blockableControlsHelp.emplace_back(control.Text);
    }

    int oldIndexUp = BlockableControls::GetIndex(g_controls.ControlXboxBlocks[static_cast<int>(CarControls::ControllerControlType::ShiftUp)]);
    if (g_menu.StringArray(lang.Tr("controller.bindings.shiftupblocks"), blockableControlsHelp, oldIndexUp)) {
        g_controls.ControlXboxBlocks[static_cast<int>(CarControls::ControllerControlType::ShiftUp)] = blockableControls[oldIndexUp].Control;
    }

    int oldIndexDown = BlockableControls::GetIndex(g_controls.ControlXboxBlocks[static_cast<int>(CarControls::ControllerControlType::ShiftDown)]);
    if (g_menu.StringArray(lang.Tr("controller.bindings.shiftdownblocks"), blockableControlsHelp, oldIndexDown)) {
        g_controls.ControlXboxBlocks[static_cast<int>(CarControls::ControllerControlType::ShiftDown)] = blockableControls[oldIndexDown].Control;
    }

    int oldIndexClutch = BlockableControls::GetIndex(g_controls.ControlXboxBlocks[static_cast<int>(CarControls::ControllerControlType::Clutch)]);
    if (g_menu.StringArray(lang.Tr("controller.bindings.clutchblocks"), blockableControlsHelp, oldIndexClutch)) {
        g_controls.ControlXboxBlocks[static_cast<int>(CarControls::ControllerControlType::Clutch)] = blockableControls[oldIndexClutch].Control;
    }

    std::vector<std::string> controllerInfo = {
        lang.Tr("controller.bindings.pressrightclear"),
        lang.Tr("controller.bindings.pressreturnconfig"),
        "",
    };

    for (const auto& input : g_controls.ControlXbox) {
        controllerInfo.back() = input.Description;
        controllerInfo.push_back(fmt::format(lang.Tr("controller.bindings.assignedtoxbox"), input.Control));
        if (g_menu.OptionPlus(fmt::format(lang.Tr("controller.bindings.assign"), input.Name), controllerInfo, nullptr, std::bind(clearControllerButton, input.ConfigTag), nullptr, lang.Tr("controller.bindings.currentsetting"))) {
            WAIT(500);
            bool result = configControllerButton(input.ConfigTag);
            if (!result)
                UI::Notify(WARN, fmt::format(lang.Tr("controller.bindings.cancelled"), input.Name));
            WAIT(500);
        }
        controllerInfo.pop_back();
    }
}

void update_keyboardmenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("keyboard.title"));
    g_menu.Subtitle("");

    std::vector<std::string> keyboardInfo = {
        lang.Tr("keyboard.pressrightclear"),
        lang.Tr("keyboard.activeinputs"),
    };

    for (uint32_t i = 0; i < static_cast<uint32_t>(CarControls::KeyboardControlType::SIZEOF_KeyboardControlType); ++i) {
        const auto& input = g_controls.KBControl[i];

        if (g_controls.IsKeyPressed(input.Control))
            keyboardInfo.emplace_back(input.Name);
    }
    if (keyboardInfo.size() == 2)
        keyboardInfo.emplace_back(lang.Tr("keyboard.none"));

    for (const auto& input : g_controls.KBControl) {
        keyboardInfo.push_back(fmt::format(lang.Tr("keyboard.assignedto"), GetNameFromKey(input.Control)));
        if (g_menu.OptionPlus(fmt::format(lang.Tr("keyboard.assign"), input.Name), keyboardInfo, nullptr, std::bind(clearKeyboardKey, input.ConfigTag), nullptr, lang.Tr("keyboard.currentsetting"))) {
            WAIT(500);
            bool result = configKeyboardKey(input.ConfigTag);
            UI::Notify(WARN, result ?
                fmt::format(lang.Tr("keyboard.saved"), input.Name) :
                fmt::format(lang.Tr("keyboard.cancelled"), input.Name));
            WAIT(500);
        }
        keyboardInfo.pop_back();
    }
}

void update_wheelmenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("wheelmenu.title"));
    auto wheelGuid = g_controls.WheelAxes[static_cast<int>(CarControls::WheelAxisType::Steer)].Guid;
    g_menu.Subtitle(FormatDeviceGuidName(wheelGuid));

    if (!g_controls.FreeDevices.empty()) {
        for (const auto& device : g_controls.FreeDevices) {
            if (g_menu.Option(device.name, NativeMenu::solidGreen,
                { lang.Tr("wheelmenu.devicefound"),
                  lang.Tr("wheelmenu.devicefound.desc") })) {
                saveAllSettings();
                g_settings.SteeringAppendDevice(device.guid, device.name);
                g_settings.Read(&g_controls);
                g_controls.CheckGUIDs(g_settings.Wheel.InputDevices.RegisteredGUIDs);
            }
        }
    }

    if (g_menu.BoolOption(lang.Tr("wheelmenu.enable"), g_settings.Wheel.Options.Enable,
        { lang.Tr("wheelmenu.enable.desc") })) {
        saveAllSettings();
        g_settings.Read(&g_controls);
        initWheel();
    }

    if (g_menu.BoolOption(lang.Tr("wheelmenu.logiled"), g_settings.Wheel.Options.LogiLEDs,
        { lang.Tr("wheelmenu.logiled.desc1"),
            lang.Tr("wheelmenu.logiled.desc2") })) {
        g_settings.SaveWheel();
    }

    g_menu.MenuOption(lang.Tr("wheelmenu.analogsetup"), "axesmenu",
        { lang.Tr("wheelmenu.analogsetup.desc") });

    g_menu.MenuOption(lang.Tr("wheelmenu.ffboptions"), "forcefeedbackmenu",
        { lang.Tr("wheelmenu.ffboptions.desc") });

    g_menu.MenuOption(lang.Tr("wheelmenu.softlockoptions"), "anglemenu",
        { lang.Tr("wheelmenu.softlockoptions.desc") });

    g_menu.MenuOption(lang.Tr("wheelmenu.buttonsetup"), "buttonsmenu",
        { lang.Tr("wheelmenu.buttonsetup.desc") });

    std::vector<std::string> hpatInfo = {
        lang.Tr("wheelmenu.hpattern.clear"),
        lang.Tr("wheelmenu.hpattern.activegear")
    };
    if (g_controls.ButtonIn(CarControls::WheelControlType::HR)) hpatInfo.emplace_back(lang.Tr("wheelmenu.hpattern.reverse"));
    for (uint8_t gear = 1; gear < VExt::GearsAvailable(); ++gear) {
        // H1 == 1
        if (g_controls.ButtonIn(static_cast<CarControls::WheelControlType>(gear))) 
            hpatInfo.emplace_back(fmt::format(lang.Tr("wheelmenu.hpattern.gear"), gear));
    }

    if (g_menu.OptionPlus(lang.Tr("wheelmenu.hpattern.setup"), hpatInfo, nullptr, std::bind(clearHShifter), nullptr, lang.Tr("wheelmenu.hpattern.inputvalues"),
        { lang.Tr("wheelmenu.hpattern.setup.desc") })) {
        bool result = configHPattern();
        UI::Notify(WARN, result ? lang.Tr("wheelmenu.hpattern.saved") : lang.Tr("wheelmenu.hpattern.cancelled"));
    }

    g_menu.BoolOption(lang.Tr("wheelmenu.shifterauto"), g_settings.Wheel.Options.UseShifterForAuto,
        { lang.Tr("wheelmenu.shifterauto.desc") });

    std::vector<std::string> hAutoInfo = {
        lang.Tr("wheelmenu.hpattern.autoclear"),
        lang.Tr("wheelmenu.hpattern.activegear")
    };
    if (g_controls.ButtonIn(CarControls::WheelControlType::APark)) hAutoInfo.emplace_back(lang.Tr("wheelmenu.hpattern.autopark"));
    if (g_controls.ButtonIn(CarControls::WheelControlType::AReverse)) hAutoInfo.emplace_back(lang.Tr("wheelmenu.hpattern.autoreverse"));
    if (g_controls.ButtonIn(CarControls::WheelControlType::ANeutral)) hAutoInfo.emplace_back(lang.Tr("wheelmenu.hpattern.autoneutral"));
    if (g_controls.ButtonIn(CarControls::WheelControlType::ADrive)) hAutoInfo.emplace_back(lang.Tr("wheelmenu.hpattern.autodrive"));

    if (g_menu.OptionPlus(lang.Tr("wheelmenu.hpattern.autosetup"), hAutoInfo, nullptr, [] { clearASelect(); }, nullptr, lang.Tr("wheelmenu.hpattern.inputvalues"),
        { lang.Tr("wheelmenu.hpattern.autosetup.desc") })) {
        bool result = configASelect();
        UI::Notify(WARN, result ? lang.Tr("wheelmenu.hpattern.autosaved") : lang.Tr("wheelmenu.hpattern.autocancelled"));
    }

    g_menu.BoolOption(lang.Tr("wheelmenu.kbhpattern"), g_settings.Wheel.Options.HPatternKeyboard,
        { lang.Tr("wheelmenu.kbhpattern.desc") });
}

void update_anglemenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("anglemenu.title"));
    g_menu.Subtitle("");
    float minLock = 180.0f;
    if (g_menu.FloatOption(lang.Tr("anglemenu.physicaldeg"), g_settings.Wheel.Steering.AngleMax, minLock, 2880.0f, 30.0f,
        { lang.Tr("anglemenu.physicaldeg.desc1"),
            lang.Tr("anglemenu.physicaldeg.desc2") })) {
        if (g_settings.Wheel.Steering.AngleCar > g_settings.Wheel.Steering.AngleMax) { g_settings.Wheel.Steering.AngleCar = g_settings.Wheel.Steering.AngleMax; }
        if (g_settings.Wheel.Steering.AngleBike > g_settings.Wheel.Steering.AngleMax) { g_settings.Wheel.Steering.AngleBike = g_settings.Wheel.Steering.AngleMax; }
        if (g_settings.Wheel.Steering.AngleBoat > g_settings.Wheel.Steering.AngleMax) { g_settings.Wheel.Steering.AngleBoat = g_settings.Wheel.Steering.AngleMax; }
    }

    g_menu.FloatOption(lang.Tr("anglemenu.carsoftlock"), g_settings.Wheel.Steering.AngleCar, minLock, g_settings.Wheel.Steering.AngleMax, 30.0,
        { lang.Tr("anglemenu.carsoftlock.desc1"),
            lang.Tr("anglemenu.carsoftlock.desc2") });

    g_menu.FloatOption(lang.Tr("anglemenu.bikesoftlock"), g_settings.Wheel.Steering.AngleBike, minLock, g_settings.Wheel.Steering.AngleMax, 30.0,
        { lang.Tr("anglemenu.bikesoftlock.desc1"),
            lang.Tr("anglemenu.bikesoftlock.desc2") });

    g_menu.FloatOption(lang.Tr("anglemenu.boatsoftlock"), g_settings.Wheel.Steering.AngleBoat, minLock, g_settings.Wheel.Steering.AngleMax, 30.0,
        { lang.Tr("anglemenu.boatsoftlock.desc1"),
            lang.Tr("anglemenu.boatsoftlock.desc2") });
}

void update_axesmenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("axesmenu.title"));
    g_menu.Subtitle("");
    g_controls.UpdateValues(CarControls::Wheel, true);
    std::vector<std::string> info = {
        lang.Tr("axesmenu.clearaxis"),
        fmt::format("Steer:\t\t{:.3f}", g_controls.SteerVal),
        fmt::format("Throttle:\t\t{:.3f}", g_controls.ThrottleVal),
        fmt::format("Brake:\t\t{:.3f}", g_controls.BrakeVal),
        fmt::format("Clutch:\t\t{:.3f}", g_controls.ClutchVal),
        fmt::format("Handbrake:\t{:.3f}", g_controls.HandbrakeVal),
    };

    for (const auto& input : g_controls.WheelAxes) {
        if (input.ConfigTag.empty())
            continue;

        // FFB handled in the wheel case of configAxis
        if (input.ConfigTag == "FFB")
            continue;

        bool selected = false;

        if (g_menu.OptionPlus(
            fmt::format(lang.Tr("axesmenu.configure"), input.Name),
            {},
            &selected,
            [&input] { return clearAxis(input.ConfigTag); }, 
            nullptr, 
            lang.Tr("axesmenu.inputvalues"))) {
            bool result = configAxis(input.ConfigTag);
            UI::Notify(WARN, result ? 
                fmt::format(lang.Tr("axesmenu.saved"), input.Name) : 
                fmt::format(lang.Tr("axesmenu.cancelled"), input.Name));
            if (result)
                initWheel();
        }

        if (selected) {
            if (!input.Control.empty() && input.Guid != GUID_NULL) {
                info.push_back(fmt::format(lang.Tr("axesmenu.assignedto"), input.Name));
                info.push_back(fmt::format("{} {}", lang.Tr("axesmenu.device"), FormatDeviceGuidName(input.Guid)));
                info.push_back(fmt::format("{} {}", lang.Tr("axesmenu.axis"), input.Control));
            }
            else {
                info.push_back(fmt::format(lang.Tr("axesmenu.unassigned"), input.Name));
            }

            g_menu.OptionPlusPlus(info, lang.Tr("axesmenu.inputvalues"));
        }
    }

    g_menu.FloatOption(lang.Tr("axesmenu.steerdeadzone"), g_settings.Wheel.Steering.DeadZone, 0.0f, 0.5f, 0.01f,
        { lang.Tr("axesmenu.steerdeadzone.desc") });

    g_menu.FloatOption(lang.Tr("axesmenu.steerdeadzoneoffset"), g_settings.Wheel.Steering.DeadZoneOffset, -0.5f, 0.5f, 0.01f,
        { lang.Tr("axesmenu.steerdeadzoneoffset.desc") });

    g_menu.FloatOption(lang.Tr("axesmenu.throttlead"), g_settings.Wheel.Throttle.AntiDeadZone, 0.0f, 1.0f, 0.01f,
        { lang.Tr("axesmenu.antideadzone.desc") });

    g_menu.FloatOption(lang.Tr("axesmenu.brakead"), g_settings.Wheel.Brake.AntiDeadZone, 0.0f, 1.0f, 0.01f,
        { lang.Tr("axesmenu.antideadzone.desc") });

    bool showBrakeGammaBox = false;
    std::vector<std::string> extras = {};
    g_menu.OptionPlus(lang.Tr("axesmenu.brakegamma"), extras, &showBrakeGammaBox,
        [=] { return incVal(g_settings.Wheel.Brake.Gamma, 5.0f, 0.01f); },
        [=] { return decVal(g_settings.Wheel.Brake.Gamma, 0.1f, 0.01f); },
        lang.Tr("axesmenu.brakegamma"),
        { lang.Tr("axesmenu.brakegamma.desc") });

    if (showBrakeGammaBox) {
        extras = ShowGammaCurve("Brake", g_controls.BrakeVal, g_settings.Wheel.Brake.Gamma);
        g_menu.OptionPlusPlus(extras, lang.Tr("axesmenu.brakegamma"));
    }

    bool showThrottleGammaBox = false;
    extras = {};
    g_menu.OptionPlus(lang.Tr("axesmenu.throttlegamma"), extras, &showThrottleGammaBox,
        [=] { return incVal(g_settings.Wheel.Throttle.Gamma, 5.0f, 0.01f); },
        [=] { return decVal(g_settings.Wheel.Throttle.Gamma, 0.1f, 0.01f); },
        lang.Tr("axesmenu.throttlegamma"),
        { lang.Tr("axesmenu.throttlegamma.desc") });

    if (showThrottleGammaBox) {
        extras = ShowGammaCurve("Throttle", g_controls.ThrottleVal, g_settings.Wheel.Throttle.Gamma);
        g_menu.OptionPlusPlus(extras, lang.Tr("axesmenu.throttlegamma"));
    }

    bool showSteeringGammaBox = false;
    extras = {};
    g_menu.OptionPlus(lang.Tr("axesmenu.steergamma"), extras, &showSteeringGammaBox,
        [=] { return incVal(g_settings.Wheel.Steering.Gamma, 5.0f, 0.01f); },
        [=] { return decVal(g_settings.Wheel.Steering.Gamma, 0.1f, 0.01f); },
        lang.Tr("axesmenu.steergamma"),
        { lang.Tr("axesmenu.steergamma.desc") });

    if (showSteeringGammaBox) {
        float steerValL = map(g_controls.SteerVal, 0.0f, 0.5f, 1.0f, 0.0f);
        float steerValR = map(g_controls.SteerVal, 0.5f, 1.0f, 0.0f, 1.0f);
        float steerVal = g_controls.SteerVal < 0.5f ? steerValL : steerValR;
        extras = ShowGammaCurve("Steering", steerVal, g_settings.Wheel.Steering.Gamma);
        g_menu.OptionPlusPlus(extras, lang.Tr("axesmenu.steergamma"));
    }
}

void update_forcefeedbackmenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("ffbmenu.title"));
    g_menu.Subtitle("");

    g_menu.BoolOption(lang.Tr("ffbmenu.enable"), g_settings.Wheel.FFB.Enable,
        { lang.Tr("ffbmenu.enable.desc") });

    g_menu.FloatOption(lang.Tr("ffbmenu.satscale"), g_settings.Wheel.FFB.SATAmpMult, 0.05f, 10.0f, 0.05f,
        { lang.Tr("ffbmenu.satscale.desc1"),
            lang.Tr("ffbmenu.satscale.desc2"),
            lang.Tr("ffbmenu.satscale.desc3"),
            lang.Tr("ffbmenu.satscale.desc4") });

    g_menu.StringArray(lang.Tr("ffbmenu.linearitytype"), { ffbCurveTypes }, g_settings.Wheel.FFB.FFBProfile,
        { lang.Tr("ffbmenu.linearitytype.desc1"),
            lang.Tr("ffbmenu.linearitytype.desc2"),
            lang.Tr("ffbmenu.linearitytype.desc3") });

    bool showDynamicFfbCurveBox = false;
    if (g_menu.OptionPlus(fmt::format(lang.Tr("ffbmenu.responsecurve"), g_settings.Wheel.FFB.ResponseCurve), {}, &showDynamicFfbCurveBox,
            [=] { return incVal(g_settings.Wheel.FFB.ResponseCurve, 5.00f, 0.01f); },
            [=] { return decVal(g_settings.Wheel.FFB.ResponseCurve, 0.01f, 0.01f); },
            lang.Tr("ffbmenu.responsecurve"), {
                lang.Tr("ffbmenu.responsecurve.desc1"),
                lang.Tr("ffbmenu.responsecurve.desc2"),
                lang.Tr("ffbmenu.responsecurve.desc3"),
                lang.Tr("ffbmenu.responsecurve.desc4") })) {
        float val = g_settings.Wheel.FFB.ResponseCurve;
        if (GetKbEntryFloat(val)) {
            g_settings.Wheel.FFB.ResponseCurve = val;
        }
    }

    if (showDynamicFfbCurveBox) {
        auto extras = ShowDynamicFfbCurve(abs(WheelInput::GetFFBConstantForce()),
                                          g_settings.Wheel.FFB.ResponseCurve,
                                          g_settings.Wheel.FFB.FFBProfile);
        g_menu.OptionPlusPlus(extras, lang.Tr("ffbmenu.responsecurve"));
    }

    g_menu.FloatOption(lang.Tr("ffbmenu.detailmult"), g_settings.Wheel.FFB.DetailMult, 0.0f, 10.0f, 0.1f,
        { lang.Tr("ffbmenu.detailmult.desc1"),
          lang.Tr("ffbmenu.detailmult.desc2") });

    g_menu.IntOption(lang.Tr("ffbmenu.detaillim"), g_settings.Wheel.FFB.DetailLim, 0, 20000, 100, 
        { lang.Tr("ffbmenu.detaillim.desc1"),
          lang.Tr("ffbmenu.detaillim.desc2") });

    g_menu.IntOption(lang.Tr("ffbmenu.detailmaw"), g_settings.Wheel.FFB.DetailMAW, 1, 100, 1,
        { lang.Tr("ffbmenu.detailmaw.desc1"),
        lang.Tr("ffbmenu.detailmaw.desc2")});

    g_menu.FloatOption(lang.Tr("ffbmenu.collisionmult"), g_settings.Wheel.FFB.CollisionMult, 0.0f, 10.0f, 0.1f,
        { lang.Tr("ffbmenu.collisionmult.desc") });

    g_menu.IntOption(lang.Tr("ffbmenu.dampermax"), g_settings.Wheel.FFB.DamperMax, 0, 200, 1,
        { lang.Tr("ffbmenu.dampermax.desc") });

    g_menu.IntOption(lang.Tr("ffbmenu.dampermin"), g_settings.Wheel.FFB.DamperMin, 0, 200, 1,
        { lang.Tr("ffbmenu.dampermin.desc") });

    g_menu.FloatOption(lang.Tr("ffbmenu.damperminspeed"), g_settings.Wheel.FFB.DamperMinSpeed, 0.0f, 40.0f, 0.2f,
        { lang.Tr("ffbmenu.damperminspeed.desc1"), lang.Tr("ffbmenu.damperminspeed.desc2") });

    if (g_settings.Wheel.FFB.LUTFile.empty()) {
        if (g_menu.Option(lang.Tr("ffbmenu.lutinactive"), {
            lang.Tr("ffbmenu.lutinactive.desc1"),
            lang.Tr("ffbmenu.lutinactive.desc2")
        })) {
            WAIT(20);
            PAD::SET_CONTROL_VALUE_NEXT_FRAME(0, ControlFrontendPause, 1.0f);
            ShellExecuteA(0, 0, "https://github.com/E66666666/GTAVManualTransmission/blob/master/doc/README.md#wheel-ffb-lut", 0, 0, SW_SHOW);
        }

        if (g_menu.Option(lang.Tr("ffbmenu.tuneantideadzone"))) {
            g_controls.PlayFFBCollision(0);
            g_controls.PlayFFBDynamics(0, 0);
            while (true) {
                if (IsKeyJustUp(GetKeyFromName(escapeKey))) {
                    break;
                }

                if (IsKeyJustUp(GetKeyFromName("LEFT"))) {
                    g_settings.Wheel.FFB.AntiDeadForce -= 100;
                    if (g_settings.Wheel.FFB.AntiDeadForce < 100) {
                        g_settings.Wheel.FFB.AntiDeadForce = 0;
                    }
                }

                if (IsKeyJustUp(GetKeyFromName("RIGHT"))) {
                    g_settings.Wheel.FFB.AntiDeadForce += 100;
                    if (g_settings.Wheel.FFB.AntiDeadForce > 10000 - 100) {
                        g_settings.Wheel.FFB.AntiDeadForce = 10000;
                    }
                }
                g_controls.UpdateValues(CarControls::InputDevices::Wheel, true);

                g_controls.PlayFFBDynamics(g_settings.Wheel.FFB.AntiDeadForce, 0);

                UI::ShowHelpText(fmt::format(lang.Tr("ffbmenu.tuneantideadzone.help"), g_settings.Wheel.FFB.AntiDeadForce, escapeKey));
                WAIT(0);
            }
        }
    }
    else {
        g_menu.Option(lang.Tr("ffbmenu.lutactive"), {
            fmt::format("{} {}", lang.Tr("ffbmenu.usinglut"), g_settings.Wheel.FFB.LUTFile),
            lang.Tr("ffbmenu.lutactive.desc")
        });
    }

    g_menu.MenuOption(lang.Tr("ffbmenu.normalization"), "ffbnormalizationmenu",
        { lang.Tr("ffbmenu.normalization.desc1"),
          lang.Tr("ffbmenu.normalization.desc2") });

    if (g_settings.Debug.ShowAdvancedFFBOptions) {
        g_menu.OptionPlus(lang.Tr("ffbmenu.legacyoptions"),
            { lang.Tr("ffbmenu.legacyoptions.desc1"),
              lang.Tr("ffbmenu.legacyoptions.desc2") });

        g_menu.FloatOption(lang.Tr("ffbmenu.satfactor"), g_settings.Wheel.FFB.SATFactor, 0.0f, 1.0f, 0.01f,
            { lang.Tr("ffbmenu.satfactor.desc1"),
              lang.Tr("ffbmenu.satfactor.desc2") });

        g_menu.FloatOption(lang.Tr("ffbmenu.satgamma"), g_settings.Wheel.FFB.Gamma, 0.01f, 2.0f, 0.01f,
            { lang.Tr("ffbmenu.satgamma.desc1"),
              lang.Tr("ffbmenu.satgamma.desc2"),
              lang.Tr("ffbmenu.satgamma.desc3") });

        g_menu.FloatOption(lang.Tr("ffbmenu.satmaxspeed"), g_settings.Wheel.FFB.MaxSpeed, 10.0f, 1000.0f, 1.0f,
            { lang.Tr("ffbmenu.satmaxspeed.desc"),
              fmt::format("{:.0f} kph / {:.0f} mph", g_settings.Wheel.FFB.MaxSpeed * 3.6f, g_settings.Wheel.FFB.MaxSpeed * 2.23694f) });
    }
}

void update_ffbnormalizationmenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("ffbnorm.title"));
    g_menu.Subtitle("");

    g_menu.OptionPlus(lang.Tr("ffbnorm.description"),
        {
            lang.Tr("ffbnorm.description.desc1"),
            lang.Tr("ffbnorm.description.desc2"),
            lang.Tr("ffbnorm.description.desc3"),
            lang.Tr("ffbnorm.description.desc4"),
            lang.Tr("ffbnorm.description.nonorm"),
            lang.Tr("ffbnorm.description.defnorm")
        }
    );

    if (g_menu.Option(lang.Tr("ffbnorm.nonormalization"), { lang.Tr("ffbnorm.nonormalization.desc") })) {
        g_settings.Wheel.FFB.SlipOptMin = 5.0f;
        g_settings.Wheel.FFB.SlipOptMinMult = 1.0f;
        g_settings.Wheel.FFB.SlipOptMax = 20.0f;
        g_settings.Wheel.FFB.SlipOptMaxMult = 1.0f;
    }

    if (g_menu.Option(lang.Tr("ffbnorm.defaultnormalization"), { lang.Tr("ffbnorm.defaultnormalization.desc") })) {
        g_settings.Wheel.FFB.SlipOptMin = 7.5f;
        g_settings.Wheel.FFB.SlipOptMinMult = 1.6f;
        g_settings.Wheel.FFB.SlipOptMax = 20.0f;
        g_settings.Wheel.FFB.SlipOptMaxMult = 1.0f;
    }

    g_menu.FloatOptionCb(lang.Tr("ffbnorm.slipoptlow"), g_settings.Wheel.FFB.SlipOptMin, 0.0f, 90.0f, 0.1f, GetKbEntryFloat);
    g_menu.FloatOptionCb(lang.Tr("ffbnorm.slipoptlowmult"), g_settings.Wheel.FFB.SlipOptMinMult, 0.0f, 10.0f, 0.1f, GetKbEntryFloat,
        { lang.Tr("ffbnorm.slipoptlowmult.desc1"),
          lang.Tr("ffbnorm.slipoptlowmult.desc2") });
    g_menu.FloatOptionCb(lang.Tr("ffbnorm.slipopthigh"), g_settings.Wheel.FFB.SlipOptMax, 0.0f, 90.0f, 0.1f, GetKbEntryFloat);
    g_menu.FloatOptionCb(lang.Tr("ffbnorm.slipopthighmult"), g_settings.Wheel.FFB.SlipOptMaxMult, 0.0f, 10.0f, 0.1f, GetKbEntryFloat,
        { lang.Tr("ffbnorm.slipopthighmult.desc1"),
          lang.Tr("ffbnorm.slipopthighmult.desc2") });
}

void update_buttonsmenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("buttonsmenu.title"));
    g_menu.Subtitle("");

    std::vector<std::string> wheelToKeyInfo = {
        lang.Tr("buttonsmenu.wtk.active"),
        lang.Tr("buttonsmenu.wtk.clear"),
    };

    if (g_controls.WheelToKey.empty()) {
        wheelToKeyInfo.push_back(lang.Tr("buttonsmenu.wtk.nokeys"));
    }
    for (const auto& input : g_controls.WheelToKey) {
        int index = g_settings.GUIDToDeviceIndex(input.Guid);
        std::string name = g_settings.GUIDToDeviceName(input.Guid);
        wheelToKeyInfo.push_back(fmt::format("DEV{}BUTTON{} = {} ({})", index, input.Control, input.ConfigTag, name));
        if (g_controls.GetWheel().IsButtonPressed(input.Control, input.Guid)) {
            wheelToKeyInfo.back() = fmt::format("{} ({})", wheelToKeyInfo.back(), lang.Tr("buttonsmenu.wtk.pressed"));
        }
    }

    if (g_menu.OptionPlus(lang.Tr("buttonsmenu.wtksetup"), wheelToKeyInfo, nullptr, clearWheelToKey, nullptr, lang.Tr("buttonsmenu.wtk.info"),
        { lang.Tr("buttonsmenu.wtksetup.desc") })) {
        bool result = configWheelToKey();
        UI::Notify(WARN, result ? lang.Tr("buttonsmenu.wtk.added") : lang.Tr("buttonsmenu.wtk.cancelled"));
    }

    std::vector<std::string> buttonInfo;
    buttonInfo.emplace_back(lang.Tr("buttonsmenu.clear"));
    buttonInfo.emplace_back(lang.Tr("buttonsmenu.active"));

    for (uint32_t i = 0; i < static_cast<uint32_t>(CarControls::WheelControlType::SIZEOF_WheelControlType); ++i) {
        const auto& input = g_controls.WheelButton[i];

        // Don't show the H-pattern shifter in this menu, it has its own options
        if (input.ConfigTag.rfind("HPATTERN_", 0) == 0 ||
            input.ConfigTag.rfind("AUTO_", 0) == 0)
            continue;

        if (g_controls.ButtonIn(static_cast<CarControls::WheelControlType>(i)))
            buttonInfo.emplace_back(input.Name);
    }
    if (buttonInfo.size() == 2)
        buttonInfo.emplace_back(lang.Tr("buttonsmenu.none"));

    for (const auto& input : g_controls.WheelButton) {
        // Don't show the H-pattern shifter in this menu, it has its own options
        if (input.ConfigTag.rfind("HPATTERN_", 0) == 0 ||
            input.ConfigTag.rfind("AUTO_", 0) == 0 ||
            input.ConfigTag.empty())
            continue;

        bool popTwo = false;
        if (input.Control != -1) {
            buttonInfo.push_back(fmt::format("{} {}", lang.Tr("buttonsmenu.device"), FormatDeviceGuidName(input.Guid)));
            buttonInfo.push_back(fmt::format("{} {}", lang.Tr("buttonsmenu.button"), input.Control));
            popTwo = true;
        }
        else {
            buttonInfo.push_back(fmt::format(lang.Tr("buttonsmenu.unassigned"), input.Name));
        }
        if (g_menu.OptionPlus(
            fmt::format(lang.Tr("buttonsmenu.assign"), input.Name),
            buttonInfo,
            nullptr,
            [&input] { return clearButton(input.ConfigTag); },
            nullptr,
            lang.Tr("buttonsmenu.currentinputs"))) {
            bool result = configButton(input.ConfigTag);
            UI::Notify(WARN, result ?
                fmt::format(lang.Tr("buttonsmenu.saved"), input.Name) :
                fmt::format(lang.Tr("buttonsmenu.cancelled"), input.Name));
            if (result)
                initWheel();
        }
        buttonInfo.pop_back();
        if (popTwo)
            buttonInfo.pop_back();
    }
}

void update_hudmenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("hudmenu.title"));
    g_menu.Subtitle("");

    g_menu.BoolOption(lang.Tr("hudmenu.enable"), g_settings.HUD.Enable,
        { lang.Tr("hudmenu.enable.desc") });

    g_menu.BoolOption(lang.Tr("hudmenu.always"), g_settings.HUD.Always,
        { lang.Tr("hudmenu.always.desc") });

    auto fontIt = std::find_if(fonts.begin(), fonts.end(), [](const SFont& font) { return font.ID == g_settings.HUD.Font; });
    if (fontIt != fonts.end()) {
        std::vector<std::string> strFonts;
        strFonts.reserve(fonts.size());
        for (const auto& font : fonts)
            strFonts.push_back(font.Name);
        int fontIndex = static_cast<int>(fontIt - fonts.begin());
        if (g_menu.StringArray(lang.Tr("hudmenu.font"), strFonts, fontIndex, { lang.Tr("hudmenu.font.desc") })) {
            g_settings.HUD.Font = fonts.at(fontIndex).ID;
        }
    }
    else {
        g_menu.Option(lang.Tr("hudmenu.invalidfont"), NativeMenu::solidRed, { lang.Tr("hudmenu.invalidfont.desc") });
    }

    g_menu.BoolOption(lang.Tr("hudmenu.outline"), g_settings.HUD.Outline);

    g_menu.StringArray(lang.Tr("hudmenu.notifylevel"), notifyLevelStrings, g_settings.HUD.NotifyLevel,
        { lang.Tr("hudmenu.notifylevel.desc"),
        "Debug: All",
        "Info: Mode switching",
        "UI: Menu actions and setup",
        "None: Hide all notifications" });

    g_menu.MenuOption(lang.Tr("hudmenu.gear"), "geardisplaymenu");
    g_menu.MenuOption(lang.Tr("hudmenu.speedo"), "speedodisplaymenu");
    g_menu.MenuOption(lang.Tr("hudmenu.rpm"), "rpmdisplaymenu");
    g_menu.MenuOption(lang.Tr("hudmenu.wheelinfo"), "wheelinfomenu");
    g_menu.MenuOption(lang.Tr("hudmenu.dashindicators"), "dashindicatormenu", 
        { lang.Tr("hudmenu.dashindicators.desc") });
    g_menu.MenuOption(lang.Tr("hudmenu.dsprot"), "dsprotmenu",
        { lang.Tr("hudmenu.dsprot.desc") });
    g_menu.MenuOption(lang.Tr("hudmenu.mouse"), "mousehudmenu");
}

void update_geardisplaymenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("geardisplay.title"));
    g_menu.Subtitle("");

    g_menu.BoolOption(lang.Tr("geardisplay.gear"), g_settings.HUD.Gear.Enable);
    g_menu.BoolOption(lang.Tr("geardisplay.shiftmode"), g_settings.HUD.ShiftMode.Enable);

    g_menu.FloatOption(lang.Tr("geardisplay.gearx"), g_settings.HUD.Gear.XPos, 0.0f, 1.0f, 0.005f);
    g_menu.FloatOption(lang.Tr("geardisplay.geary"), g_settings.HUD.Gear.YPos, 0.0f, 1.0f, 0.005f);
    g_menu.FloatOption(lang.Tr("geardisplay.gearsize"), g_settings.HUD.Gear.Size, 0.0f, 3.0f, 0.05f);
    g_menu.IntOption(lang.Tr("geardisplay.gearcolorr"), g_settings.HUD.Gear.ColorR, 0, 255);
    g_menu.IntOption(lang.Tr("geardisplay.gearcolog"), g_settings.HUD.Gear.ColorG, 0, 255);
    g_menu.IntOption(lang.Tr("geardisplay.gearcologb"), g_settings.HUD.Gear.ColorB, 0, 255);
    g_menu.IntOption(lang.Tr("geardisplay.geartopcolorr"), g_settings.HUD.Gear.TopColorR, 0, 255);
    g_menu.IntOption(lang.Tr("geardisplay.geartopcolorg"), g_settings.HUD.Gear.TopColorG, 0, 255);
    g_menu.IntOption(lang.Tr("geardisplay.geartopcolorb"), g_settings.HUD.Gear.TopColorB, 0, 255);

    g_menu.FloatOption(lang.Tr("geardisplay.shiftmodex"), g_settings.HUD.ShiftMode.XPos, 0.0f, 1.0f, 0.005f);
    g_menu.FloatOption(lang.Tr("geardisplay.shiftmodey"), g_settings.HUD.ShiftMode.YPos, 0.0f, 1.0f, 0.005f);
    g_menu.FloatOption(lang.Tr("geardisplay.shiftmodesize"), g_settings.HUD.ShiftMode.Size, 0.0f, 3.0f, 0.05f);
    g_menu.IntOption(lang.Tr("geardisplay.shiftmoder"), g_settings.HUD.ShiftMode.ColorR, 0, 255);
    g_menu.IntOption(lang.Tr("geardisplay.shiftmodeg"), g_settings.HUD.ShiftMode.ColorG, 0, 255);
    g_menu.IntOption(lang.Tr("geardisplay.shiftmodeb"), g_settings.HUD.ShiftMode.ColorB, 0, 255);
}

void update_speedodisplaymenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("speedodisplay.title"));
    g_menu.Subtitle("");

    int oldPos = 0;
    auto speedoTypeIt = std::find(speedoTypes.begin(), speedoTypes.end(), g_settings.HUD.Speedo.Speedo);
    if (speedoTypeIt != speedoTypes.end()) {
        oldPos = static_cast<int>(speedoTypeIt - speedoTypes.begin());
    }
    int newPos = oldPos;
    g_menu.StringArray(lang.Tr("speedodisplay.speedo"), speedoTypes, newPos);
    if (newPos != oldPos) {
        g_settings.HUD.Speedo.Speedo = speedoTypes.at(newPos);
    }
    g_menu.BoolOption(lang.Tr("speedodisplay.drivetrain"), g_settings.HUD.Speedo.UseDrivetrain,
        { lang.Tr("speedodisplay.drivetrain.desc") });
    g_menu.BoolOption(lang.Tr("speedodisplay.showunits"), g_settings.HUD.Speedo.ShowUnit);

    g_menu.FloatOption(lang.Tr("speedodisplay.x"), g_settings.HUD.Speedo.XPos, 0.0f, 1.0f, 0.005f);
    g_menu.FloatOption(lang.Tr("speedodisplay.y"), g_settings.HUD.Speedo.YPos, 0.0f, 1.0f, 0.005f);
    g_menu.FloatOption(lang.Tr("speedodisplay.size"), g_settings.HUD.Speedo.Size, 0.0f, 3.0f, 0.05f);

    g_menu.IntOption(lang.Tr("speedodisplay.colorr"), g_settings.HUD.Speedo.ColorR, 0, 255);
    g_menu.IntOption(lang.Tr("speedodisplay.colorg"), g_settings.HUD.Speedo.ColorG, 0, 255);
    g_menu.IntOption(lang.Tr("speedodisplay.colorb"), g_settings.HUD.Speedo.ColorB, 0, 255);
}

void update_rpmdisplaymenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("rpmdisplay.title"));
    g_menu.Subtitle("");

    g_menu.BoolOption(lang.Tr("rpmdisplay.enable"), g_settings.HUD.RPMBar.Enable);
    g_menu.FloatOption(lang.Tr("rpmdisplay.redline"), g_settings.HUD.RPMBar.Redline, 0.0f, 1.0f, 0.01f);

    g_menu.FloatOption(lang.Tr("rpmdisplay.xpos"), g_settings.HUD.RPMBar.XPos, 0.0f, 1.0f, 0.0025f);
    g_menu.FloatOption(lang.Tr("rpmdisplay.ypos"), g_settings.HUD.RPMBar.YPos, 0.0f, 1.0f, 0.0025f);
    g_menu.FloatOption(lang.Tr("rpmdisplay.width"), g_settings.HUD.RPMBar.XSz, 0.0f, 1.0f, 0.0025f);
    g_menu.FloatOption(lang.Tr("rpmdisplay.height"), g_settings.HUD.RPMBar.YSz, 0.0f, 1.0f, 0.0025f);

    g_menu.IntOption(lang.Tr("rpmdisplay.bgr"), g_settings.HUD.RPMBar.BgR, 0, 255);
    g_menu.IntOption(lang.Tr("rpmdisplay.bgg"), g_settings.HUD.RPMBar.BgG, 0, 255);
    g_menu.IntOption(lang.Tr("rpmdisplay.bgb"), g_settings.HUD.RPMBar.BgB, 0, 255);
    g_menu.IntOption(lang.Tr("rpmdisplay.bga"), g_settings.HUD.RPMBar.BgA, 0, 255);

    g_menu.IntOption(lang.Tr("rpmdisplay.fgr"), g_settings.HUD.RPMBar.FgR, 0, 255);
    g_menu.IntOption(lang.Tr("rpmdisplay.fgg"), g_settings.HUD.RPMBar.FgG, 0, 255);
    g_menu.IntOption(lang.Tr("rpmdisplay.fgb"), g_settings.HUD.RPMBar.FgB, 0, 255);
    g_menu.IntOption(lang.Tr("rpmdisplay.fga"), g_settings.HUD.RPMBar.FgA, 0, 255);

    g_menu.IntOption(lang.Tr("rpmdisplay.redliner"), g_settings.HUD.RPMBar.RedlineR, 0, 255);
    g_menu.IntOption(lang.Tr("rpmdisplay.redlineg"), g_settings.HUD.RPMBar.RedlineG, 0, 255);
    g_menu.IntOption(lang.Tr("rpmdisplay.redlineb"), g_settings.HUD.RPMBar.RedlineB, 0, 255);
    g_menu.IntOption(lang.Tr("rpmdisplay.redlinea"), g_settings.HUD.RPMBar.RedlineA, 0, 255);

    g_menu.IntOption(lang.Tr("rpmdisplay.revlimitr"), g_settings.HUD.RPMBar.RevLimitR, 0, 255);
    g_menu.IntOption(lang.Tr("rpmdisplay.revlimitg"), g_settings.HUD.RPMBar.RevLimitG, 0, 255);
    g_menu.IntOption(lang.Tr("rpmdisplay.revlimitb"), g_settings.HUD.RPMBar.RevLimitB, 0, 255);
    g_menu.IntOption(lang.Tr("rpmdisplay.revlimita"), g_settings.HUD.RPMBar.RevLimitA, 0, 255);

    g_menu.IntOption(lang.Tr("rpmdisplay.lcsr"), g_settings.HUD.RPMBar.LaunchControlStagedR, 0, 255);
    g_menu.IntOption(lang.Tr("rpmdisplay.lcsg"), g_settings.HUD.RPMBar.LaunchControlStagedG, 0, 255);
    g_menu.IntOption(lang.Tr("rpmdisplay.lcsb"), g_settings.HUD.RPMBar.LaunchControlStagedB, 0, 255);
    g_menu.IntOption(lang.Tr("rpmdisplay.lcsa"), g_settings.HUD.RPMBar.LaunchControlStagedA, 0, 255);

    g_menu.IntOption(lang.Tr("rpmdisplay.lcar"), g_settings.HUD.RPMBar.LaunchControlActiveR, 0, 255);
    g_menu.IntOption(lang.Tr("rpmdisplay.lcag"), g_settings.HUD.RPMBar.LaunchControlActiveG, 0, 255);
    g_menu.IntOption(lang.Tr("rpmdisplay.lcab"), g_settings.HUD.RPMBar.LaunchControlActiveB, 0, 255);
    g_menu.IntOption(lang.Tr("rpmdisplay.lcaa"), g_settings.HUD.RPMBar.LaunchControlActiveA, 0, 255);
}

void update_wheelinfomenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("wheelinfo.title"));
    g_menu.Subtitle("");

    g_menu.BoolOption(lang.Tr("wheelinfo.display"), g_settings.HUD.Wheel.Enable, { lang.Tr("wheelinfo.display.desc") });
    g_menu.BoolOption(lang.Tr("wheelinfo.ffblevel"), g_settings.HUD.Wheel.FFB.Enable, { lang.Tr("wheelinfo.ffblevel.desc") });
    g_menu.BoolOption(lang.Tr("wheelinfo.always"), g_settings.HUD.Wheel.Always, { lang.Tr("wheelinfo.always.desc") });

    g_menu.FloatOption(lang.Tr("wheelinfo.imgx"), g_settings.HUD.Wheel.ImgXPos, 0.0f, 1.0f, 0.01f);
    g_menu.FloatOption(lang.Tr("wheelinfo.imgy"), g_settings.HUD.Wheel.ImgYPos, 0.0f, 1.0f, 0.01f);
    g_menu.FloatOption(lang.Tr("wheelinfo.imgsize"), g_settings.HUD.Wheel.ImgSize, 0.0f, 1.0f, 0.01f);
    g_menu.FloatOption(lang.Tr("wheelinfo.pedalx"), g_settings.HUD.Wheel.PedalXPos, 0.0f, 1.0f, 0.01f);
    g_menu.FloatOption(lang.Tr("wheelinfo.pedaly"), g_settings.HUD.Wheel.PedalYPos, 0.0f, 1.0f, 0.01f);
    g_menu.FloatOption(lang.Tr("wheelinfo.pedalw"), g_settings.HUD.Wheel.PedalXSz, 0.0f, 1.0f, 0.01f);
    g_menu.FloatOption(lang.Tr("wheelinfo.pedalh"), g_settings.HUD.Wheel.PedalYSz, 0.0f, 1.0f, 0.01f);
    g_menu.FloatOption(lang.Tr("wheelinfo.pedalpadx"), g_settings.HUD.Wheel.PedalXPad, 0.0f, 1.0f, 0.01f);
    g_menu.FloatOption(lang.Tr("wheelinfo.pedalpady"), g_settings.HUD.Wheel.PedalYPad, 0.0f, 1.0f, 0.01f);
    g_menu.IntOption(lang.Tr("wheelinfo.pedalbga"), g_settings.HUD.Wheel.PedalBgA, 0, 255);
    g_menu.IntOption(lang.Tr("wheelinfo.throttler"), g_settings.HUD.Wheel.PedalThrottleR, 0, 255);
    g_menu.IntOption(lang.Tr("wheelinfo.throttleg"), g_settings.HUD.Wheel.PedalThrottleG, 0, 255);
    g_menu.IntOption(lang.Tr("wheelinfo.throttleb"), g_settings.HUD.Wheel.PedalThrottleB, 0, 255);
    g_menu.IntOption(lang.Tr("wheelinfo.throttlea"), g_settings.HUD.Wheel.PedalThrottleA, 0, 255);
    g_menu.IntOption(lang.Tr("wheelinfo.braker"), g_settings.HUD.Wheel.PedalBrakeR, 0, 255);
    g_menu.IntOption(lang.Tr("wheelinfo.brakg"), g_settings.HUD.Wheel.PedalBrakeG, 0, 255);
    g_menu.IntOption(lang.Tr("wheelinfo.brakeb"), g_settings.HUD.Wheel.PedalBrakeB, 0, 255);
    g_menu.IntOption(lang.Tr("wheelinfo.braka"), g_settings.HUD.Wheel.PedalBrakeA, 0, 255);
    g_menu.IntOption(lang.Tr("wheelinfo.clutchr"), g_settings.HUD.Wheel.PedalClutchR, 0, 255);
    g_menu.IntOption(lang.Tr("wheelinfo.clutchg"), g_settings.HUD.Wheel.PedalClutchG, 0, 255);
    g_menu.IntOption(lang.Tr("wheelinfo.clutchb"), g_settings.HUD.Wheel.PedalClutchB, 0, 255);
    g_menu.IntOption(lang.Tr("wheelinfo.clutcha"), g_settings.HUD.Wheel.PedalClutchA, 0, 255);

    g_menu.FloatOption(lang.Tr("wheelinfo.ffbx"), g_settings.HUD.Wheel.FFB.XPos, 0.0f, 1.0f, 0.01f);
    g_menu.FloatOption(lang.Tr("wheelinfo.ffby"), g_settings.HUD.Wheel.FFB.YPos, 0.0f, 1.0f, 0.01f);

    g_menu.FloatOption(lang.Tr("wheelinfo.ffbw"), g_settings.HUD.Wheel.FFB.XSz, 0.0f, 1.0f, 0.01f);
    g_menu.FloatOption(lang.Tr("wheelinfo.ffbh"), g_settings.HUD.Wheel.FFB.YSz, 0.0f, 1.0f, 0.01f);

    g_menu.IntOption(lang.Tr("wheelinfo.ffbbgr"), g_settings.HUD.Wheel.FFB.BgR, 0, 255);
    g_menu.IntOption(lang.Tr("wheelinfo.ffbbgg"), g_settings.HUD.Wheel.FFB.BgG, 0, 255);
    g_menu.IntOption(lang.Tr("wheelinfo.ffbbgb"), g_settings.HUD.Wheel.FFB.BgB, 0, 255);
    g_menu.IntOption(lang.Tr("wheelinfo.ffbbga"), g_settings.HUD.Wheel.FFB.BgA, 0, 255);

    g_menu.IntOption(lang.Tr("wheelinfo.ffbfgr"), g_settings.HUD.Wheel.FFB.FgR, 0, 255);
    g_menu.IntOption(lang.Tr("wheelinfo.ffbfgg"), g_settings.HUD.Wheel.FFB.FgG, 0, 255);
    g_menu.IntOption(lang.Tr("wheelinfo.ffbfgb"), g_settings.HUD.Wheel.FFB.FgB, 0, 255);
    g_menu.IntOption(lang.Tr("wheelinfo.ffbfga"), g_settings.HUD.Wheel.FFB.FgA, 0, 255);

    g_menu.IntOption(lang.Tr("wheelinfo.ffblimitr"), g_settings.HUD.Wheel.FFB.LimitR, 0, 255);
    g_menu.IntOption(lang.Tr("wheelinfo.ffblimitg"), g_settings.HUD.Wheel.FFB.LimitG, 0, 255);
    g_menu.IntOption(lang.Tr("wheelinfo.ffblimitb"), g_settings.HUD.Wheel.FFB.LimitB, 0, 255);
    g_menu.IntOption(lang.Tr("wheelinfo.ffblimita"), g_settings.HUD.Wheel.FFB.LimitA, 0, 255);
}

void update_dashindicatormenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("dashindicator.title"));
    g_menu.Subtitle("");

    g_menu.BoolOption(lang.Tr("dashindicator.enable"), g_settings.HUD.DashIndicators.Enable);
    g_menu.FloatOption(lang.Tr("dashindicator.posx"), g_settings.HUD.DashIndicators.XPos, 0.0f, 1.0f, 0.005f);
    g_menu.FloatOption(lang.Tr("dashindicator.posy"), g_settings.HUD.DashIndicators.YPos, 0.0f, 1.0f, 0.005f);
    g_menu.FloatOption(lang.Tr("dashindicator.size"), g_settings.HUD.DashIndicators.Size, 0.25f, 4.0f, 0.05f);

    g_menu.BoolOption(lang.Tr("dashindicator.lite"), g_settings.HUD.DashIndicators.Lite,
        { lang.Tr("dashindicator.lite.desc1"),
          lang.Tr("dashindicator.lite.desc2") });
    if (g_settings.HUD.DashIndicators.Lite) {
        g_menu.FloatOptionCb(lang.Tr("dashindicator.vertoffset"), g_settings.HUD.DashIndicators.TxtVOffset,
            -1.0f, 1.0f, 0.005f, GetKbEntryFloat,
            { lang.Tr("dashindicator.vertoffset.desc1"),
              lang.Tr("dashindicator.vertoffset.desc2"),
              lang.Tr("dashindicator.vertoffset.desc3") });

        g_menu.FloatOptionCb(lang.Tr("dashindicator.sizemult"), g_settings.HUD.DashIndicators.TxtSzMod,
            0.0f, 10.0f, 0.05f, GetKbEntryFloat,
            { lang.Tr("dashindicator.sizemult.desc1"),
              lang.Tr("dashindicator.sizemult.desc2"),
              lang.Tr("dashindicator.sizemult.desc3") });
    }
}

void update_dsprotmenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("dsprot.title"));
    g_menu.Subtitle("");

    extern int g_textureDsProtId;
    drawTexture(g_textureDsProtId, 0, -9998, 100,
        g_settings.HUD.DsProt.Size, g_settings.HUD.DsProt.Size,
        0.5f, 0.5f, // center of texture
        g_settings.HUD.DsProt.XPos, g_settings.HUD.DsProt.YPos,
        0.0f, GRAPHICS::GET_ASPECT_RATIO(FALSE), 1.0f, 1.0f, 1.0f, 1.0f);

    g_menu.BoolOption(lang.Tr("dsprot.enable"), g_settings.HUD.DsProt.Enable,
        { lang.Tr("dsprot.enable.desc") });

    g_menu.FloatOption(lang.Tr("dsprot.posx"), g_settings.HUD.DsProt.XPos, 0.0f, 1.0f, 0.005f);
    g_menu.FloatOption(lang.Tr("dsprot.posy"), g_settings.HUD.DsProt.YPos, 0.0f, 1.0f, 0.005f);
    g_menu.FloatOption(lang.Tr("dsprot.size"), g_settings.HUD.DsProt.Size, 0.01f, 1.0f, 0.01f);
}

void update_mousehudmenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("mousehud.title"));
    g_menu.Subtitle("");

    g_menu.BoolOption(lang.Tr("mousehud.enable"), g_settings.HUD.MouseSteering.Enable);

    g_menu.FloatOption(lang.Tr("mousehud.posx"), g_settings.HUD.MouseSteering.XPos, 0.0f, 1.0f, 0.0025f);
    g_menu.FloatOption(lang.Tr("mousehud.posy"), g_settings.HUD.MouseSteering.YPos, 0.0f, 1.0f, 0.0025f);
    g_menu.FloatOption(lang.Tr("mousehud.width"), g_settings.HUD.MouseSteering.XSz, 0.0f, 1.0f, 0.0025f);
    g_menu.FloatOption(lang.Tr("mousehud.height"), g_settings.HUD.MouseSteering.YSz, 0.0f, 1.0f, 0.0025f);
    g_menu.FloatOption(lang.Tr("mousehud.markerw"), g_settings.HUD.MouseSteering.MarkerXSz, 0.0f, 1.0f, 0.0025f);

    g_menu.IntOption(lang.Tr("mousehud.bgr"), g_settings.HUD.MouseSteering.BgR, 0, 255);
    g_menu.IntOption(lang.Tr("mousehud.bgg"), g_settings.HUD.MouseSteering.BgG, 0, 255);
    g_menu.IntOption(lang.Tr("mousehud.bgb"), g_settings.HUD.MouseSteering.BgB, 0, 255);
    g_menu.IntOption(lang.Tr("mousehud.bga"), g_settings.HUD.MouseSteering.BgA, 0, 255);

    g_menu.IntOption(lang.Tr("mousehud.fgr"), g_settings.HUD.MouseSteering.FgR, 0, 255);
    g_menu.IntOption(lang.Tr("mousehud.fgg"), g_settings.HUD.MouseSteering.FgG, 0, 255);
    g_menu.IntOption(lang.Tr("mousehud.fgb"), g_settings.HUD.MouseSteering.FgB, 0, 255);
    g_menu.IntOption(lang.Tr("mousehud.fga"), g_settings.HUD.MouseSteering.FgA, 0, 255);
}

void update_driveassistmenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("driveassist.title"));
    g_menu.Subtitle(MenuSubtitleConfig());

    g_menu.BoolOption(lang.Tr("driveassist.abs"), g_settings().DriveAssists.ABS.Enable,
        { lang.Tr("driveassist.abs.desc1"),
          lang.Tr("driveassist.abs.desc2") });

    g_menu.MenuOption(lang.Tr("driveassist.abssettings"), "abssettingsmenu",
        { lang.Tr("driveassist.abssettings.desc") });

    g_menu.BoolOption(lang.Tr("driveassist.tcs"), g_settings().DriveAssists.TCS.Enable,
        { lang.Tr("driveassist.tcs.desc1"),
          lang.Tr("driveassist.tcs.desc2") });

    g_menu.MenuOption(lang.Tr("driveassist.tcssettings"), "tcssettingsmenu",
        { lang.Tr("driveassist.tcssettings.desc") });

    g_menu.BoolOption(lang.Tr("driveassist.esp"), g_settings().DriveAssists.ESP.Enable,
        { lang.Tr("driveassist.esp.desc1"),
          lang.Tr("driveassist.esp.desc2") });

    g_menu.MenuOption(lang.Tr("driveassist.espsettings"), "espsettingsmenu", 
        { lang.Tr("driveassist.espsettings.desc") });

    g_menu.BoolOption(lang.Tr("driveassist.lc"), g_settings().DriveAssists.LaunchControl.Enable,
        { lang.Tr("driveassist.lc.desc") });

    g_menu.MenuOption(lang.Tr("driveassist.lcsettings"), "lcssettings",
        { lang.Tr("driveassist.lcsettings.desc") });

    g_menu.BoolOption(lang.Tr("driveassist.lsd"), g_settings().DriveAssists.LSD.Enable,
        { lang.Tr("driveassist.lsd.desc1"),
          lang.Tr("driveassist.lsd.desc2")});

    g_menu.MenuOption(lang.Tr("driveassist.lsdsettings"), "lsdsettingsmenu",
        { lang.Tr("driveassist.lsdsettings.desc") });

    g_menu.BoolOption(lang.Tr("driveassist.awd"), g_settings().DriveAssists.AWD.Enable,
        { lang.Tr("driveassist.awd.desc1"),
          lang.Tr("driveassist.awd.desc2")});

    g_menu.MenuOption(lang.Tr("driveassist.awdsettings"), "awdsettingsmenu");

    g_menu.BoolOption(lang.Tr("driveassist.cc"), g_settings().DriveAssists.CruiseControl.Enable);

    g_menu.MenuOption(lang.Tr("driveassist.ccsettings"), "cruisecontrolsettingsmenu");
}

void update_abssettingsmenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("abssettings.title"));
    g_menu.Subtitle(MenuSubtitleConfig());

    g_menu.BoolOption(lang.Tr("abssettings.filter"), g_settings().DriveAssists.ABS.Filter,
        { lang.Tr("abssettings.filter.desc1"),
          lang.Tr("abssettings.filter.desc2") });

    g_menu.BoolOption(lang.Tr("abssettings.flash"), g_settings().DriveAssists.ABS.Flash,
        { lang.Tr("abssettings.flash.desc") });

}

void update_tcssettingsmenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("tcssettings.title"));
    g_menu.Subtitle(MenuSubtitleConfig());

    g_menu.StringArray(lang.Tr("tcssettings.mode"), tcsStrings, g_settings().DriveAssists.TCS.Mode,
        { lang.Tr("tcssettings.mode.desc1"),
          lang.Tr("tcssettings.mode.desc2"),
          lang.Tr("tcssettings.mode.desc3") });

    g_menu.FloatOption(lang.Tr("tcssettings.slipmin"), g_settings().DriveAssists.TCS.SlipMin, 1.0f, 20.0f, 0.05f,
        { lang.Tr("tcssettings.slipmin.desc1"),
          lang.Tr("tcssettings.slipmin.desc2"),
          lang.Tr("tcssettings.slipmin.desc3") });

    float min = g_settings().DriveAssists.TCS.SlipMin;
    g_menu.FloatOption(lang.Tr("tcssettings.slipmax"), g_settings().DriveAssists.TCS.SlipMax, min, 20.0f, 0.05f,
        { lang.Tr("tcssettings.slipmax.desc1"),
          lang.Tr("tcssettings.slipmax.desc2"),
          lang.Tr("tcssettings.slipmax.desc3") });

    g_menu.FloatOptionCb(lang.Tr("tcssettings.brakemult"), g_settings().DriveAssists.TCS.BrakeMult, 0.0f, 10.0f, 0.05f, GetKbEntryFloat,
        { lang.Tr("tcssettings.brakemult.desc1"),
          lang.Tr("tcssettings.brakemult.desc2"),
          lang.Tr("tcssettings.brakemult.desc3"),
          lang.Tr("tcssettings.brakemult.desc4"),
          lang.Tr("tcssettings.brakemult.desc5") });
}

void update_espsettingsmenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("espsettings.title"));
    g_menu.Subtitle(MenuSubtitleConfig());

    g_menu.FloatOption(lang.Tr("espsettings.overmin"), g_settings().DriveAssists.ESP.OverMin, 0.0f, 90.0f, 0.1f,
        { lang.Tr("espsettings.overmin.desc") });
    g_menu.FloatOption(lang.Tr("espsettings.overmincomp"), g_settings().DriveAssists.ESP.OverMinComp, 0.0f, 10.0f, 0.1f,
        { lang.Tr("espsettings.overmincomp.desc") });
    g_menu.FloatOption(lang.Tr("espsettings.overmax"), g_settings().DriveAssists.ESP.OverMax, 0.0f, 90.0f, 0.1f,
        { lang.Tr("espsettings.overmax.desc") });
    g_menu.FloatOption(lang.Tr("espsettings.overmaxcomp"), g_settings().DriveAssists.ESP.OverMaxComp, 0.0f, 10.0f, 0.1f,
        { lang.Tr("espsettings.overmaxcomp.desc") });

    g_menu.FloatOption(lang.Tr("espsettings.undermin"), g_settings().DriveAssists.ESP.UnderMin, 0.0f, 90.0f, 0.1f,
        { lang.Tr("espsettings.undermin.desc") });
    g_menu.FloatOption(lang.Tr("espsettings.undermincomp"), g_settings().DriveAssists.ESP.UnderMinComp, 0.0f, 10.0f, 0.1f,
        { lang.Tr("espsettings.undermincomp.desc") });
    g_menu.FloatOption(lang.Tr("espsettings.undermax"), g_settings().DriveAssists.ESP.UnderMax, 0.0f, 90.0f, 0.1f,
        { lang.Tr("espsettings.undermax.desc") });
    g_menu.FloatOption(lang.Tr("espsettings.undermaxcomp"), g_settings().DriveAssists.ESP.UnderMaxComp, 0.0f, 10.0f, 0.1f,
        { lang.Tr("espsettings.undermaxcomp.desc") });
}

void update_lcssettingsmenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("lcssettings.title"));
    g_menu.Subtitle(MenuSubtitleConfig());

    g_menu.FloatOption(lang.Tr("lcssettings.rpm"), g_settings().DriveAssists.LaunchControl.RPM, 0.3f, 0.9f, 0.025f,
        { lang.Tr("lcssettings.rpm.desc") });

    g_menu.FloatOption(lang.Tr("lcssettings.slipmin"), g_settings().DriveAssists.LaunchControl.SlipMin, 0.0f, 20.0f, 0.1f,
        { lang.Tr("lcssettings.slipmin.desc1"),
          lang.Tr("lcssettings.slipmin.desc2"),
          lang.Tr("lcssettings.slipmin.desc3") });

    float min = g_settings().DriveAssists.LaunchControl.SlipMin;
    g_menu.FloatOption(lang.Tr("lcssettings.slipmax"), g_settings().DriveAssists.LaunchControl.SlipMax, min, 20.0f, 0.1f,
        { lang.Tr("lcssettings.slipmax.desc1"),
          lang.Tr("lcssettings.slipmax.desc2"),
          lang.Tr("lcssettings.slipmax.desc3") });

    const std::vector<std::string> instructions{
        lang.Tr("lcssettings.instructions.title"),
        lang.Tr("lcssettings.instructions.step1"),
        lang.Tr("lcssettings.instructions.step2"),
        lang.Tr("lcssettings.instructions.step3"),
        lang.Tr("lcssettings.instructions.step4"),
        lang.Tr("lcssettings.instructions.note"),
    };

    g_menu.OptionPlus(lang.Tr("lcssettings.instructions"), instructions);
}

void update_lsdsettingsmenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("lsdsettings.title"));
    g_menu.Subtitle(MenuSubtitleConfig());

    g_menu.FloatOption(lang.Tr("lsdsettings.viscosity"), g_settings().DriveAssists.LSD.Viscosity, 0.0f, 100.0f, 1.0f,
        { lang.Tr("lsdsettings.viscosity.desc1"),
          lang.Tr("lsdsettings.viscosity.desc2") });

    bool statusSelected = false;
    g_menu.OptionPlus(lang.Tr("lsdsettings.status"), {}, &statusSelected, nullptr, nullptr, lang.Tr("lsdsettings.status"), 
        { lang.Tr("lsdsettings.status.desc") });

    if (statusSelected) {
        std::vector<std::string> extra;

        if (!g_settings().DriveAssists.LSD.Enable) {
            extra.push_back(lang.Tr("lsdsettings.disabled"));
        }
        else {
            auto lsdData = DrivingAssists::GetLSD();
            std::string fddcol;
            if (lsdData.FDD > 0.1f) { fddcol = "~r~"; }
            if (lsdData.FDD < -0.1f) { fddcol = "~b~"; }

            std::string rddcol;
            if (lsdData.RDD > 0.1f) { rddcol = "~r~"; }
            if (lsdData.RDD < -0.1f) { rddcol = "~b~"; }

            extra.push_back(fmt::format("{}Front: L-R: {:.2f}", fddcol, lsdData.FDD));
            extra.push_back(fmt::format("{}Rear:  L-R: {:.2f}", rddcol, lsdData.RDD));

            extra.push_back(fmt::format("LF/RF [{:.2f}]/[{:.2f}]", lsdData.BrakeLF, lsdData.BrakeRF));
            extra.push_back(fmt::format("LR/RR [{:.2f}]/[{:.2f}]", lsdData.BrakeLR, lsdData.BrakeRR));
            extra.push_back(fmt::format("{}LSD: {}",
                lsdData.Use ? "~g~" : "~r~",
                lsdData.Use ? lang.Tr("lsdstatus.active") : lang.Tr("lsdstatus.idle")));
        }

        g_menu.OptionPlusPlus(extra, lang.Tr("lsdsettings.status"));
    }
}

void update_awdsettingsmenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("awdsettings.title"));
    g_menu.Subtitle(MenuSubtitleConfig());

    if (!HandlingReplacement::Available()) {
        if (g_menu.Option(lang.Tr("awdsettings.handlingmissing"),
            { lang.Tr("awdsettings.handlingmissing.desc") })) {
            WAIT(20);
            PAD::SET_CONTROL_VALUE_NEXT_FRAME(0, ControlFrontendPause, 1.0f);
            ShellExecuteA(0, 0, "https://www.gta5-mods.com/tools/handling-replacement-library", 0, 0, SW_SHOW);
        }
    }

    bool resolveAWD = false;
    g_menu.OptionPlus(lang.Tr("awdsettings.status"), {}, &resolveAWD, nullptr, nullptr, "", { lang.Tr("awdsettings.status.desc") });
    if (resolveAWD) {
        std::vector<std::string> awdStats;
        extern Vehicle g_playerVehicle;
        if (ENTITY::DOES_ENTITY_EXIST(g_playerVehicle)) {
            awdStats = GetAWDInfo(g_playerVehicle);
        }
        else {
            awdStats = { lang.Tr("awdsettings.novehicle") };
        }
        g_menu.OptionPlusPlus(awdStats, lang.Tr("awdsettings.status"));
    }

    g_menu.BoolOption(lang.Tr("awdsettings.usecustombias"), g_settings().DriveAssists.AWD.UseCustomBaseBias, 
        { lang.Tr("awdsettings.usecustombias.desc") });
    g_menu.FloatOption(lang.Tr("awdsettings.custombias"), g_settings().DriveAssists.AWD.CustomBaseBias, 0.01f, 0.99f, 0.01f,
        { lang.Tr("awdsettings.custombias.desc1"),
          lang.Tr("awdsettings.custombias.desc2") });

    g_menu.FloatOption(lang.Tr("awdsettings.custommin"), g_settings().DriveAssists.AWD.CustomMin, 0.01f, 0.99f, 0.01f,
        { lang.Tr("awdsettings.custommin.desc") });
    g_menu.FloatOption(lang.Tr("awdsettings.custommax"), g_settings().DriveAssists.AWD.CustomMax, 0.01f, 0.99f, 0.01f,
        { lang.Tr("awdsettings.custommax.desc") });

    g_menu.FloatOption(lang.Tr("awdsettings.biasmaxtransfer"), g_settings().DriveAssists.AWD.BiasAtMaxTransfer, 0.01f, 0.99f, 0.01f,
        { lang.Tr("awdsettings.biasmaxtransfer.desc1"),
          lang.Tr("awdsettings.biasmaxtransfer.desc2") });

    g_menu.BoolOption(lang.Tr("awdsettings.ontraction"), g_settings().DriveAssists.AWD.UseTraction, 
        { lang.Tr("awdsettings.ontraction.desc") } );

    g_menu.FloatOption(lang.Tr("awdsettings.speedmindiff"), g_settings().DriveAssists.AWD.TractionLossMin, 1.0f, 2.0f, 0.05f,
        { lang.Tr("awdsettings.speedmindiff.desc") });
    g_menu.FloatOption(lang.Tr("awdsettings.speedmaxdiff"), g_settings().DriveAssists.AWD.TractionLossMax, 1.0f, 2.0f, 0.05f,
        { lang.Tr("awdsettings.speedmaxdiff.desc") });

    // Should only be used for RWD-biased cars
    g_menu.BoolOption(lang.Tr("awdsettings.onoversteer"), g_settings().DriveAssists.AWD.UseOversteer,
        { lang.Tr("awdsettings.onoversteer.desc") });
    g_menu.FloatOption(lang.Tr("awdsettings.onoversteer") + " min", g_settings().DriveAssists.AWD.OversteerMin, 0.0f, 90.0f, 1.0f); // degrees
    g_menu.FloatOption(lang.Tr("awdsettings.onoversteer") + " max", g_settings().DriveAssists.AWD.OversteerMax, 0.0f, 90.0f, 1.0f); // degrees
    
    // Should only be used for FWD-biased cars
    g_menu.BoolOption(lang.Tr("awdsettings.onundersteer"), g_settings().DriveAssists.AWD.UseUndersteer,
        { lang.Tr("awdsettings.onundersteer.desc") });
    g_menu.FloatOption(lang.Tr("awdsettings.onundersteer") + " min", g_settings().DriveAssists.AWD.UndersteerMin, 0.0f, 90.0f, 1.0f); // degrees
    g_menu.FloatOption(lang.Tr("awdsettings.onundersteer") + " max", g_settings().DriveAssists.AWD.UndersteerMax, 0.0f, 90.0f, 1.0f); // degrees

    const std::vector<std::string> specialFlagsDescr = 
    {
        "Flags for extra features. Current flags:",
        "Bit 0: Enable torque transfer dial on y97y's BNR32",
        "Bit 1: Enable torque transfer dial on Wanted188's GT-R R32 (Remember to disable VehFuncs for torque dial)"
    };
    std::string specialFlagsStr = fmt::format("{:08X}", g_settings().DriveAssists.AWD.SpecialFlags);
    if (g_menu.Option(fmt::format(lang.Tr("awdsettings.specialflags"), specialFlagsStr), specialFlagsDescr)) {
        std::string newFlags = GetKbEntryStr(specialFlagsStr);
        SetFlags(g_settings().DriveAssists.AWD.SpecialFlags, newFlags);
    }
}

void update_cruisecontrolsettingsmenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("ccsettings.title"));
    g_menu.Subtitle(MenuSubtitleConfig());

    bool ccActive = CruiseControl::GetActive();
    if (g_menu.BoolOption(lang.Tr("ccsettings.active"), ccActive,
        { lang.Tr("ccsettings.active.desc1"),
          lang.Tr("ccsettings.active.desc2") })) {
        CruiseControl::SetActive(ccActive);
    }

    float speedValMul;
    float speedValRaw = g_settings().DriveAssists.CruiseControl.Speed;
    std::string speedNameUnit = GetSpeedUnitMultiplier(g_settings.HUD.Speedo.Speedo, speedValMul);
    float speedValUnit = speedValRaw * speedValMul;

    if (g_menu.FloatOptionCb(fmt::format(lang.Tr("ccsettings.speed"), speedNameUnit), speedValUnit, 0.0f, 500.0f, 5.0f,
        GetKbEntryFloat,
        { lang.Tr("ccsettings.speed.desc1"),
          lang.Tr("ccsettings.speed.desc2") })) {

        g_settings().DriveAssists.CruiseControl.Speed = speedValUnit / speedValMul;
    }

    g_menu.FloatOptionCb(lang.Tr("ccsettings.maxaccel"), g_settings().DriveAssists.CruiseControl.MaxAcceleration, 0.5f, 40.0f, 0.5f, GetKbEntryFloat,
        { lang.Tr("ccsettings.maxaccel.desc1"),
          lang.Tr("ccsettings.maxaccel.desc2") });

    g_menu.BoolOption(lang.Tr("ccsettings.adaptive"), g_settings().DriveAssists.CruiseControl.Adaptive,
        { lang.Tr("ccsettings.adaptive.desc") });

    g_menu.FloatOptionCb(lang.Tr("ccsettings.mindist"), g_settings().DriveAssists.CruiseControl.MinFollowDistance, 1.0f, 50.0f, 1.0f, GetKbEntryFloat,
        { lang.Tr("ccsettings.mindist.desc1"),
          lang.Tr("ccsettings.mindist.desc2") });

    g_menu.FloatOptionCb(lang.Tr("ccsettings.maxdist"), g_settings().DriveAssists.CruiseControl.MaxFollowDistance, 50.0f, 200.0f, 0.5f, GetKbEntryFloat,
        { lang.Tr("ccsettings.maxdist.desc1"),
          lang.Tr("ccsettings.maxdist.desc2") });

    g_menu.FloatOptionCb(lang.Tr("ccsettings.followdist"), g_settings().DriveAssists.CruiseControl.MinDistanceSpeedMult, 0.1f, 10.0f, 0.1f, GetKbEntryFloat,
        { lang.Tr("ccsettings.followdist.desc1"),
          lang.Tr("ccsettings.followdist.desc2"),
          lang.Tr("ccsettings.followdist.desc3"),
          lang.Tr("ccsettings.followdist.desc4") });

    g_menu.FloatOptionCb(lang.Tr("ccsettings.liftdist"), g_settings().DriveAssists.CruiseControl.MaxDistanceSpeedMult, 0.1f, 10.0f, 0.1f, GetKbEntryFloat,
        { lang.Tr("ccsettings.liftdist.desc1"),
          lang.Tr("ccsettings.liftdist.desc2"),
          lang.Tr("ccsettings.liftdist.desc3"),
          lang.Tr("ccsettings.liftdist.desc4") });

    g_menu.FloatOptionCb(lang.Tr("ccsettings.startbrake"), g_settings().DriveAssists.CruiseControl.MinDeltaBrakeMult, 0.1f, 10.0f, 0.1f, GetKbEntryFloat,
        { lang.Tr("ccsettings.startbrake.desc1"),
          lang.Tr("ccsettings.startbrake.desc2"),
          lang.Tr("ccsettings.startbrake.desc3") });

    g_menu.FloatOptionCb(lang.Tr("ccsettings.fullbrake"), g_settings().DriveAssists.CruiseControl.MaxDeltaBrakeMult, 0.1f, 10.0f, 0.1f, GetKbEntryFloat,
        { lang.Tr("ccsettings.fullbrake.desc1"),
          lang.Tr("ccsettings.fullbrake.desc2"),
          lang.Tr("ccsettings.fullbrake.desc3"),
          lang.Tr("ccsettings.fullbrake.desc4") });
}

void update_speedlimitersettingsmenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("speedlimiter.title"));
    g_menu.Subtitle(MenuSubtitleConfig());

    float speedValMul;
    float speedValRaw = g_settings().MTOptions.SpeedLimiter.Speed;
    std::string speedNameUnit = GetSpeedUnitMultiplier(g_settings.HUD.Speedo.Speedo, speedValMul);
    float speedValUnit = speedValRaw * speedValMul;

    if (g_menu.FloatOptionCb(fmt::format(lang.Tr("speedlimiter.maxspeed"), speedNameUnit), speedValUnit, 0.0f, 500.0f, 5.0f,
        GetKbEntryFloat,
        { lang.Tr("speedlimiter.maxspeed.desc") })) {

        g_settings().MTOptions.SpeedLimiter.Speed = speedValUnit / speedValMul;
    }
}

void update_gameassistmenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("gameassist.title"));
    g_menu.Subtitle("");

    g_menu.BoolOption(lang.Tr("gameassist.defaultneutral"), g_settings.GameAssists.DefaultNeutral,
        { lang.Tr("gameassist.defaultneutral.desc") });

    g_menu.BoolOption(lang.Tr("gameassist.disableautostart"), g_settings.GameAssists.DisableAutostart,
        { lang.Tr("gameassist.disableautostart.desc") });

    g_menu.BoolOption(lang.Tr("gameassist.leaveengine"), g_settings.GameAssists.LeaveEngineRunning,
        { lang.Tr("gameassist.leaveengine.desc") });

    g_menu.BoolOption(lang.Tr("gameassist.simplebike"), g_settings.GameAssists.SimpleBike,
        { lang.Tr("gameassist.simplebike.desc") });

    g_menu.BoolOption(lang.Tr("gameassist.hillgravity"), g_settings.GameAssists.HillGravity,
        { lang.Tr("gameassist.hillgravity.desc") });

    g_menu.BoolOption(lang.Tr("gameassist.autogear1"), g_settings.GameAssists.AutoGear1,
        { lang.Tr("gameassist.autogear1.desc") });

    g_menu.BoolOption(lang.Tr("gameassist.autolookback"), g_settings.GameAssists.AutoLookBack,
        { lang.Tr("gameassist.autolookback.desc") });

    g_menu.BoolOption(lang.Tr("gameassist.throttlestart"), g_settings.GameAssists.ThrottleStart,
        { lang.Tr("gameassist.throttlestart.desc") });
}

void update_steeringassistmenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("steerassist.title"));
    g_menu.Subtitle("");

    std::vector<std::string> steeringModeDescription;
    switch(g_settings.CustomSteering.Mode) {
    case 0:
        steeringModeDescription.emplace_back(lang.Tr("steerassist.mode.desc_default1"));
        steeringModeDescription.emplace_back(lang.Tr("steerassist.mode.desc_default2"));
        break;
    case 1:
        steeringModeDescription.emplace_back(lang.Tr("steerassist.mode.desc_enhanced1"));
        steeringModeDescription.emplace_back(lang.Tr("steerassist.mode.desc_enhanced2"));
        break;
    default:
        steeringModeDescription.emplace_back(lang.Tr("steerassist.mode.desc_invalid"));
    }
    std::vector<std::string> steerModeNames = { lang.Tr("steerassist.mode.default"), lang.Tr("steerassist.mode.enhanced") };
    g_menu.StringArray(lang.Tr("steerassist.mode"), steerModeNames, g_settings.CustomSteering.Mode, 
        steeringModeDescription);
    g_menu.FloatOptionCb(lang.Tr("steerassist.countermult"), g_settings.CustomSteering.CountersteerMult, 0.0f, 2.0f, 0.05f, GetKbEntryFloat,
        { lang.Tr("steerassist.countermult.desc") });
    g_menu.FloatOptionCb(lang.Tr("steerassist.counterlimit"), g_settings.CustomSteering.CountersteerLimit, 0.0f, 360.0f, 1.0f, GetKbEntryFloat,
        { lang.Tr("steerassist.counterlimit.desc") });
    g_menu.BoolOption(lang.Tr("steerassist.noredhandbrake"), g_settings.CustomSteering.NoReductionHandbrake,
        { lang.Tr("steerassist.noredhandbrake.desc") });
    g_menu.FloatOptionCb(lang.Tr("steerassist.gamma"), g_settings.CustomSteering.Gamma, 0.01f, 5.0f, 0.01f, GetKbEntryFloat,
        { lang.Tr("steerassist.gamma.desc") });
    g_menu.FloatOptionCb(lang.Tr("steerassist.steertime"), g_settings.CustomSteering.SteerTime, 0.000001f, 0.90f, 0.000001f,
        GetKbEntryFloat,
        { lang.Tr("steerassist.steertime.desc1"),
          lang.Tr("steerassist.steertime.desc2") });
    g_menu.FloatOptionCb(lang.Tr("steerassist.centertime"), g_settings.CustomSteering.CenterTime, 0.000001f, 0.99f, 0.000001f,
        GetKbEntryFloat,
        { lang.Tr("steerassist.centertime.desc1"),
          lang.Tr("steerassist.centertime.desc2") });

    g_menu.MenuOption(lang.Tr("steerassist.mouseoptions"), "mousesteeringoptionsmenu");
}

void update_mousesteeringoptionsmenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("mousesteer.title"));
    g_menu.Subtitle("");

    g_menu.BoolOption(lang.Tr("mousesteer.enable"), g_settings.CustomSteering.Mouse.Enable,
        { lang.Tr("mousesteer.enable.desc") });

    g_menu.FloatOptionCb(lang.Tr("mousesteer.sensitivity"), g_settings.CustomSteering.Mouse.Sensitivity, 0.05f, 2.0f, 0.05f, GetKbEntryFloat,
        { lang.Tr("mousesteer.sensitivity.desc") });

    g_menu.BoolOption(lang.Tr("mousesteer.disablecounter"), g_settings.CustomSteering.Mouse.DisableSteerAssist,
        { lang.Tr("mousesteer.disablecounter.desc") });

    g_menu.BoolOption(lang.Tr("mousesteer.disablereduction"), g_settings.CustomSteering.Mouse.DisableReduction,
        { lang.Tr("mousesteer.disablereduction.desc") });
}

void update_miscoptionsmenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("miscoptions.title"));
    g_menu.Subtitle("");

    if (SteeringAnimation::FileProblem()) {
        g_menu.Option(lang.Tr("miscoptions.animerror"), NativeMenu::solidRed,
            { lang.Tr("miscoptions.animerror.desc") });
    }
    else {
        g_menu.BoolOption(lang.Tr("miscoptions.syncanim"), g_settings.Misc.SyncAnimations,
            { lang.Tr("miscoptions.syncanim.desc1"),
              lang.Tr("miscoptions.syncanim.desc2"),
              lang.Tr("miscoptions.syncanim.desc3"),
              lang.Tr("miscoptions.syncanim.desc4") });
    }

    if (g_menu.BoolOption(lang.Tr("miscoptions.hideplayerfpv"), g_settings.Misc.HidePlayerInFPV,
        { lang.Tr("miscoptions.hideplayerfpv.desc1"),
          lang.Tr("miscoptions.hideplayerfpv.desc2")})) {
        functionHidePlayerInFPV(true);
    }

    g_menu.BoolOption(lang.Tr("miscoptions.hidewheelfpv"), g_settings.Misc.HideWheelInFPV);

    g_menu.BoolOption(lang.Tr("miscoptions.dashext"), g_settings.Misc.DashExtensions,
        { lang.Tr("miscoptions.dashext.desc") });

    if (g_menu.BoolOption(lang.Tr("miscoptions.udp"), g_settings.Misc.UDPTelemetry,
        { lang.Tr("miscoptions.udp.desc1"),
            lang.Tr("miscoptions.udp.desc2"),
            fmt::format("Endpoint: {}:{}", g_settings.Misc.UDPAddress, g_settings.Misc.UDPPort),
            lang.Tr("miscoptions.udp.desc3") })) {
        StartUDPTelemetry();
    }
}

void update_devoptionsmenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("devoptions.title"));
    g_menu.Subtitle("");

    g_menu.MenuOption(lang.Tr("devoptions.debug"), "debugmenu");

    g_menu.MenuOption(lang.Tr("devoptions.metrics"), "metricsmenu",
        { lang.Tr("devoptions.metrics.desc") });

    g_menu.MenuOption(lang.Tr("devoptions.compat"), "compatmenu",
        { lang.Tr("devoptions.compat.desc") });

    if (g_menu.Option(lang.Tr("devoptions.exportvehconfig"), 
        { lang.Tr("devoptions.exportvehconfig.desc") })) {
        const std::string saveFile = "baseVehicleConfig";
        const std::string vehConfigDir = Paths::GetModPath() + "\\Vehicles";
        const std::string finalFile = fmt::format("{}\\{}.ini", vehConfigDir, saveFile);

        VehicleConfig config;
        config.ModelNames = { "Model0", "Model1", "Model2" };
        config.Plates = { "46EEK572", "   MT   ", " NO GRIP" };
        config.Description = "This is a base configuration file with all possible sections and keys.";
        config.SaveSettings(&config, finalFile);
    }

    g_menu.BoolOption(lang.Tr("devoptions.enableupdate"), g_settings.Update.EnableUpdate,
        { lang.Tr("devoptions.enableupdate.desc")});

    if (g_menu.Option(lang.Tr("devoptions.checkupdate"), { lang.Tr("devoptions.checkupdate.desc") })) {
        g_settings.Update.EnableUpdate = true;
        g_settings.Update.IgnoredVersion = "v0.0.0";

        threadCheckUpdate(0);
    }

    g_menu.Option(lang.Tr("devoptions.modpath"),
        { lang.Tr("devoptions.modpath.desc1"),
          fmt::format("{}", Paths::GetModPath()) });
}

void update_debugmenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("debugmenu.title"));
    g_menu.Subtitle(lang.Tr("debugmenu.subtitle"));

    g_menu.BoolOption(lang.Tr("debugmenu.legacyffb"), g_settings.Debug.ShowAdvancedFFBOptions,
        { lang.Tr("debugmenu.legacyffb.desc") });
    g_menu.BoolOption(lang.Tr("debugmenu.displayinfo"), g_settings.Debug.DisplayInfo,
        { lang.Tr("debugmenu.displayinfo.desc") });
    g_menu.BoolOption(lang.Tr("debugmenu.wheelinfo"), g_settings.Debug.DisplayWheelInfo,
        { lang.Tr("debugmenu.wheelinfo.desc1"),
          lang.Tr("debugmenu.wheelinfo.desc2")});
    g_menu.BoolOption(lang.Tr("debugmenu.materialinfo"), g_settings.Debug.DisplayMaterialInfo,
        { lang.Tr("debugmenu.materialinfo.desc1"),
          lang.Tr("debugmenu.materialinfo.desc2") });
    g_menu.BoolOption(lang.Tr("debugmenu.tractioninfo"), g_settings.Debug.DisplayTractionInfo,
        { lang.Tr("debugmenu.tractioninfo.desc") });
    g_menu.BoolOption(lang.Tr("debugmenu.gearinginfo"), g_settings.Debug.DisplayGearingInfo,
        { lang.Tr("debugmenu.gearinginfo.desc") });
    g_menu.BoolOption(lang.Tr("debugmenu.npcinfo"), g_settings.Debug.DisplayNPCInfo,
        { lang.Tr("debugmenu.npcinfo.desc") });

    if (SteeringAnimation::FileProblem()) {
        g_menu.Option(lang.Tr("miscoptions.animerror"), NativeMenu::solidRed, 
            { lang.Tr("miscoptions.animerror.desc") });
    }
    else {
        std::vector <std::string> extras;

        extras.emplace_back(lang.Tr("debugmenu.animavailable"));

        const auto& anims = SteeringAnimation::GetAnimations();
        const size_t index = SteeringAnimation::GetAnimationIndex();
        for (size_t i = 0; i < anims.size(); ++i) {
            const auto& anim = anims[i];
            std::string mark = "[ ]";
            if (i == index) {
                mark = "[*]";
            }
            extras.emplace_back(fmt::format("{} {}", mark, anim.Dictionary));
        }

        extras.emplace_back("");
        extras.emplace_back(lang.Tr("debugmenu.animactive"));
        if (index >= SteeringAnimation::GetAnimations().size()) {
            extras.push_back(fmt::format(lang.Tr("debugmenu.animoutofrange"), index));
        }

        extras.emplace_back("");
        extras.emplace_back(lang.Tr("debugmenu.animchange"));

        std::function<void()> onLeft = [index, anims]() {
            if (!anims.empty()) {
                if (index == 0) {
                    // Set to "none"
                    SteeringAnimation::SetAnimationIndex(anims.size());
                }
                else {
                    SteeringAnimation::SetAnimationIndex(index - 1);
                }
            }
        };

        std::function<void()> onRight = [index, anims]() {
            if (!anims.empty()) {
                // allow 1 past, to set to none
                if (index >= anims.size()) {
                    SteeringAnimation::SetAnimationIndex(0);
                }
                else {
                    SteeringAnimation::SetAnimationIndex(index + 1);
                }
            }
        };

        if (g_menu.OptionPlus(lang.Tr("debugmenu.animinfo"), extras,
            nullptr, onRight, onLeft, "Animations", 
            { lang.Tr("debugmenu.animinfo.desc") })) {
            SteeringAnimation::Load();
        }
    }

    {
        auto fetchInfo = [](std::vector<std::string>& diDevicesInfo_) {
            logger.Write(DEBUG, "Re-scanning DirectInput devices");
            diDevicesInfo_.clear();

            g_controls.GetWheel().InitWheel();
            const auto& devices = g_controls.GetWheel().GetDevices();
            diDevicesInfo_.push_back(fmt::format("Devices: {}", devices.size()));
            diDevicesInfo_.push_back("");

            for (const auto& [guid, device] : devices) {
                diDevicesInfo_.push_back(fmt::format("{}", device.DeviceInstance.tszInstanceName));
                diDevicesInfo_.push_back(fmt::format("    GUID: {}", GUID2String(guid)));
                diDevicesInfo_.push_back(fmt::format("    Type: 0x{:X}", device.DeviceCapabilities.dwDevType));
                diDevicesInfo_.push_back(fmt::format("    FFB: {}", device.DeviceCapabilities.dwFlags & DIDC_FORCEFEEDBACK));
                diDevicesInfo_.push_back("");
            }
        };

        bool selected = false;
        if (g_menu.OptionPlus(lang.Tr("debugmenu.didevices"), diDevicesInfo, 
            &selected, nullptr, nullptr, "DirectInput info", { lang.Tr("debugmenu.didevices.desc") })) {
            fetchInfo(diDevicesInfo);
        }

        if (selected) {
            g_menu.OptionPlusPlus(diDevicesInfo, "DirectInput info");
        }
    }

    g_menu.BoolOption(lang.Tr("debugmenu.disablerpmlimit"), g_settings.Debug.DisableRPMLimit,
        { lang.Tr("debugmenu.disablerpmlimit.desc") });
}

void update_metricsmenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("metricsmenu.title"));
    g_menu.Subtitle("");

    g_menu.BoolOption(lang.Tr("metricsmenu.gforcemeter"), g_settings.Debug.Metrics.GForce.Enable,
        { lang.Tr("metricsmenu.gforcemeter.desc1"), 
            lang.Tr("metricsmenu.gforcemeter.desc2") });

    if (g_menu.BoolOption(lang.Tr("metricsmenu.enabletimers"), g_settings.Debug.Metrics.EnableTimers,
        { lang.Tr("metricsmenu.enabletimers.desc1"),
            "Timer0Unit = kph",
            "Timer0LimA = 0.0",
            "Timer0LimB = 120.0",
            "Timer0Tolerance = 0.1"})) {
        saveAllSettings();
        g_settings.Read(&g_controls);
        initTimers();
    }

    if (g_settings.Debug.Metrics.EnableTimers) {
        for (const auto& timerParams : g_settings.Debug.Metrics.Timers) {
            g_menu.Option(fmt::format("Timer {}-{} {} (+/- {})",
                timerParams.LimA,
                timerParams.LimB,
                timerParams.Unit,
                timerParams.Tolerance));
        }
    }
}

void update_compatmenu() {
    auto& lang = LanguageManager::Instance();
    g_menu.Title(lang.Tr("compatmenu.title"));
    g_menu.Subtitle("");

    g_menu.BoolOption(lang.Tr("compatmenu.disableNPCGearbox"), g_settings.Debug.DisableNPCGearbox,
        { lang.Tr("compatmenu.disableNPCGearbox.desc1"),
            lang.Tr("compatmenu.disableNPCGearbox.desc2"),
            lang.Tr("compatmenu.disableNPCGearbox.desc3") });

    g_menu.BoolOption(lang.Tr("compatmenu.disableNPCBrake"), g_settings.Debug.DisableNPCBrake,
        { lang.Tr("compatmenu.disableNPCBrake.desc1"),
            lang.Tr("compatmenu.disableNPCBrake.desc2") });

    g_menu.BoolOption(lang.Tr("compatmenu.disableInputDetect"), g_settings.Debug.DisableInputDetect,
        {  lang.Tr("compatmenu.disableInputDetect.desc1"),
            lang.Tr("compatmenu.disableInputDetect.desc2") });

    g_menu.BoolOption(lang.Tr("compatmenu.disablePlayerHide"), g_settings.Debug.DisablePlayerHide,
        { lang.Tr("compatmenu.disablePlayerHide.desc1"),
            lang.Tr("compatmenu.disablePlayerHide.desc2") });
}

void update_menu() {
    g_menu.CheckKeys();

    /* mainmenu */
    if (g_menu.CurrentMenu("mainmenu")) { update_mainmenu(); }

    /* mainmenu -> settingsmenu */
    if (g_menu.CurrentMenu("settingsmenu")) { update_settingsmenu(); }

    /* mainmenu -> settingsmenu -> featuresmenu */
    if (g_menu.CurrentMenu("featuresmenu")) { update_featuresmenu(); }

    /* mainmenu -> settingsmenu -> featuresmenu -> speedlimitersettingsmenu */
    if (g_menu.CurrentMenu("speedlimitersettingsmenu")) { update_speedlimitersettingsmenu(); }

    /* mainmenu -> settingsmenu -> finetuneoptionsmenu */
    if (g_menu.CurrentMenu("finetuneoptionsmenu")) { update_finetuneoptionsmenu(); }

    /* mainmenu -> settingsmenu -> shiftingoptionsmenu */
    if (g_menu.CurrentMenu("shiftingoptionsmenu")) { update_shiftingoptionsmenu(); }

    /* mainmenu -> settingsmenu -> finetuneautooptionsmenu */
    if (g_menu.CurrentMenu("finetuneautooptionsmenu")) { update_finetuneautooptionsmenu(); }

    /* mainmenu -> vehconfigmenu */
    if (g_menu.CurrentMenu("vehconfigmenu")) { update_vehconfigmenu(); }

    /* mainmenu -> controlsmenu */
    if (g_menu.CurrentMenu("controlsmenu")) { update_controlsmenu(); }

    /* mainmenu -> controlsmenu -> controllermenu */
    if (g_menu.CurrentMenu("controllermenu")) { update_controllermenu(); }

    /* mainmenu -> controlsmenu -> controllermenu -> controllerbindingsnativemenu */
    if (g_menu.CurrentMenu("controllerbindingsnativemenu")) { update_controllerbindingsnativemenu(); }

    /* mainmenu -> controlsmenu -> controllermenu -> controllerbindingsxinputmenu */
    if (g_menu.CurrentMenu("controllerbindingsxinputmenu")) { update_controllerbindingsxinputmenu(); }

    /* mainmenu -> controlsmenu -> keyboardmenu */
    if (g_menu.CurrentMenu("keyboardmenu")) { update_keyboardmenu(); }

    /* mainmenu -> controlsmenu -> steeringassistmenu */
    if (g_menu.CurrentMenu("steeringassistmenu")) { update_steeringassistmenu(); }

    /* mainmenu -> controlsmenu -> steeringassistmenu -> mousesteeringoptionsmenu */
    if (g_menu.CurrentMenu("mousesteeringoptionsmenu")) { update_mousesteeringoptionsmenu(); }

    /* mainmenu -> controlsmenu -> wheelmenu */
    if (g_menu.CurrentMenu("wheelmenu")) { update_wheelmenu(); }

    /* mainmenu -> controlsmenu -> wheelmenu -> anglemenu */
    if (g_menu.CurrentMenu("anglemenu")) { update_anglemenu(); }

    /* mainmenu -> controlsmenu -> wheelmenu -> axesmenu */
    if (g_menu.CurrentMenu("axesmenu")) { update_axesmenu(); }

    /* mainmenu -> controlsmenu -> wheelmenu -> forcefeedbackmenu */
    if (g_menu.CurrentMenu("forcefeedbackmenu")) { update_forcefeedbackmenu(); }

    /* mainmenu -> controlsmenu -> wheelmenu -> forcefeedbackmenu */
    if (g_menu.CurrentMenu("ffbnormalizationmenu")) { update_ffbnormalizationmenu(); }

    /* mainmenu -> controlsmenu -> wheelmenu -> buttonsmenu */
    if (g_menu.CurrentMenu("buttonsmenu")) { update_buttonsmenu(); }

    /* mainmenu -> controlsmenu -> update_controlsvehconfmenu */
    if (g_menu.CurrentMenu("controlsvehconfmenu")) { update_controlsvehconfmenu(); }

    /* mainmenu -> hudmenu */
    if (g_menu.CurrentMenu("hudmenu")) { update_hudmenu(); }

    /* mainmenu -> hudmenu -> geardisplaymenu*/
    if (g_menu.CurrentMenu("geardisplaymenu")) { update_geardisplaymenu(); }

    /* mainmenu -> hudmenu -> speedodisplaymenu*/
    if (g_menu.CurrentMenu("speedodisplaymenu")) { update_speedodisplaymenu(); }

    /* mainmenu -> hudmenu -> rpmdisplaymenu*/
    if (g_menu.CurrentMenu("rpmdisplaymenu")) { update_rpmdisplaymenu(); }

    /* mainmenu -> hudmenu -> wheelinfomenu*/
    if (g_menu.CurrentMenu("wheelinfomenu")) { update_wheelinfomenu(); }

    /* mainmenu -> hudmenu -> dashindicatormenu*/
    if (g_menu.CurrentMenu("dashindicatormenu")) { update_dashindicatormenu(); }

    /* mainmenu -> hudmenu -> dsprotmenu*/
    if (g_menu.CurrentMenu("dsprotmenu")) { update_dsprotmenu(); }

    /* mainmenu -> hudmenu -> mousehudmenu*/
    if (g_menu.CurrentMenu("mousehudmenu")) { update_mousehudmenu(); }

    /* mainmenu -> driveassistmenu */
    if (g_menu.CurrentMenu("driveassistmenu")) { update_driveassistmenu(); }

    /* mainmenu -> driveassistmenu -> abssettingsmenu */
    if (g_menu.CurrentMenu("abssettingsmenu")) { update_abssettingsmenu(); }

    /* mainmenu -> driveassistmenu -> tcssettingsmenu */
    if (g_menu.CurrentMenu("tcssettingsmenu")) { update_tcssettingsmenu(); }

    /* mainmenu -> driveassistmenu -> espsettingsmenu */
    if (g_menu.CurrentMenu("espsettingsmenu")) { update_espsettingsmenu(); }

    /* mainmenu -> driveassistmenu -> lcssettings */
    if (g_menu.CurrentMenu("lcssettings")) { update_lcssettingsmenu(); }

    /* mainmenu -> driveassistmenu -> lsdsettingsmenu */
    if (g_menu.CurrentMenu("lsdsettingsmenu")) { update_lsdsettingsmenu(); }

    /* mainmenu -> driveassistmenu -> awdsettingsmenu */
    if (g_menu.CurrentMenu("awdsettingsmenu")) { update_awdsettingsmenu(); }

    /* mainmenu -> driveassistmenu -> cruisecontrolsettingsmenu */
    if (g_menu.CurrentMenu("cruisecontrolsettingsmenu")) { update_cruisecontrolsettingsmenu(); }

    /* mainmenu -> gameassistmenu */
    if (g_menu.CurrentMenu("gameassistmenu")) { update_gameassistmenu(); }

    /* mainmenu -> miscoptionsmenu */
    if (g_menu.CurrentMenu("miscoptionsmenu")) { update_miscoptionsmenu(); }

    /* mainmenu -> devoptionsmenu */
    if (g_menu.CurrentMenu("devoptionsmenu")) { update_devoptionsmenu(); }

    /* mainmenu -> devoptionsmenu -> debugmenu */
    if (g_menu.CurrentMenu("debugmenu")) { update_debugmenu(); }

    /* mainmenu -> devoptionsmenu -> metricsmenu */
    if (g_menu.CurrentMenu("metricsmenu")) { update_metricsmenu(); }

    /* mainmenu -> devoptionsmenu -> compatmenu */
    if (g_menu.CurrentMenu("compatmenu")) { update_compatmenu(); }

    g_menu.EndMenu();
}
