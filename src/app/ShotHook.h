#pragma once

namespace observex {

// The training screenshot shortcut's system-wide keyboard hook (core/ShotKey: Shift+Win), so it works while the game
// is in front. App keeps one while a screen is observed. Every key goes on to Windows and the game as it was. One at a
// time; made and destroyed on the UI thread, whose message loop runs the hook.
class ShotHook {
public:
    ShotHook();  // Throws if Windows refuses the hook.
    ~ShotHook();

    ShotHook(const ShotHook&) = delete;
    ShotHook& operator=(const ShotHook&) = delete;

    // True once after each Shift+Win.
    bool takePressed();

private:
    void* hook_ = nullptr; // HHOOK.
};

// Whether observex runs as administrator. A keyboard hook only hears keys typed into windows of programs that run as
// it does or lower (Windows' UIPI), and Lineage 2 runs as administrator (its Reborn launcher does, and starts it), so
// Shift+Win needs observex to be one too (found 2026-11-15).
bool runsAsAdministrator();

// observex started again as administrator with the same arguments and kAsAdministrator (the user asked, 2026-11-15:
// "how can we automatize it? i will keep forgetting about the admin"); Windows asks the user first. True if the new
// one started: this one is to end. False if the user said no or it could not start: this one goes on as it is.
bool restartAsAdministrator();
// The argument the restarted one gets, so it never restarts again.
inline constexpr wchar_t kAsAdministrator[] = L"--as-administrator";

} // namespace observex
