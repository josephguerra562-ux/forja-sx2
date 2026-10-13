// (AI-assisted) Compatibility declarations for core diagnostic calls on PS4.
#pragma once

namespace ps4::debug
{
    void set_stage(unsigned stage);
    void set_build(const char* tag);
    void set_line(int line, const char* fmt, ...);
    void notify(const char* message);
    void start_overlay();
    bool active();
} // namespace ps4::debug
