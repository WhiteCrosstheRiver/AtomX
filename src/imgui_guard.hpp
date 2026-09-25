// RAII owner of ImGui style vars pushed on the global style stack.
//
// right() pushes tighter panel metrics right after it Begins the
// "Properties" window; every exit path of the function (including the
// data-export popup early-out) must pop them *before* ImGui::End() -
// ending a window with style vars still pushed trips ImGui's
// "Missing PopStyleVar()" recovery, whose assertion dialog blocks the
// app at startup.
//
// release() pops the owned vars immediately and disarms the guard - call
// it right before ImGui::End(). On any other exit path (early return,
// exception) the destructor pops them instead.
#pragma once

#include "imgui.h"

class ImGuiStyleVarGuard
{
public:
    explicit ImGuiStyleVarGuard(int count) : count_(count) {}
    ~ImGuiStyleVarGuard()
    {
        if (count_ > 0)
            ImGui::PopStyleVar(count_);
    }
    ImGuiStyleVarGuard(const ImGuiStyleVarGuard &) = delete;
    ImGuiStyleVarGuard &operator=(const ImGuiStyleVarGuard &) = delete;
    // Pop the owned vars now and disarm the guard.
    void release()
    {
        if (count_ > 0)
        {
            ImGui::PopStyleVar(count_);
            count_ = 0;
        }
    }

private:
    int count_;
};
