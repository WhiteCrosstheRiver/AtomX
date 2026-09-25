// Style stack discipline tests: the compact right-panel metrics push
// style vars inside the "Properties" window, and every exit path of
// right() must pop them BEFORE ImGui::End() - ending a window with a
// deeper stack trips ImGui's "Missing PopStyleVar()" recovery whose
// assertion blocks the app at startup (the P8 regression).
//
// These tests drive a headless ImGui context and inspect the style var
// stack directly (imgui_internal.h), so the unbalanced-push condition
// is detected without tripping the library's own fatal assert.
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_guard.hpp"

#include <cstdio>
#include <stdexcept>

static int failures = 0;

static void require(bool cond, const char *msg)
{
    if (!cond)
    {
        fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    }
}

int main()
{
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800, 600);
    io.DeltaTime = 1.f / 60.f;
    io.Fonts->AddFontDefault();
    io.Fonts->Build();
    auto &stack = ImGui::GetCurrentContext()->StyleVarStack;

    // The right() flow: push metrics inside the window, release() the
    // guard, pop manually, then End. Stack must return to its base.
    ImGui::NewFrame();
    {
        int base = (int)stack.Size;
        ImGui::Begin("w");
        {
            ImGuiStyleVarGuard panelStyle(2);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5, 2));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 3));
            require((int)stack.Size == base + 2, "guard owns the pushed vars");
            panelStyle.release();
            require((int)stack.Size == base, "release() pops immediately");
        }
        ImGui::End();
        require((int)stack.Size == base, "release() before End keeps the stack balanced");
    }
    ImGui::Render();

    // The guard alone (no release): the destructor pops on scope exit,
    // which covers early returns that skip the manual pop.
    ImGui::NewFrame();
    {
        int base = (int)stack.Size;
        ImGui::Begin("w");
        {
            ImGuiStyleVarGuard panelStyle(1);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5, 2));
            require((int)stack.Size == base + 1, "guard owns its var");
        }
        ImGui::End();
        require((int)stack.Size == base, "destructor pops the owned var");
    }
    ImGui::Render();

    // The P8 bug condition, detected without tripping the library's own
    // assert: ending a window with an extra push is the error contract.
    ImGui::NewFrame();
    {
        int base = (int)stack.Size;
        ImGui::Begin("w");
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5, 2));
        require((int)stack.Size != base, "unbalanced push at End is detectable");
        ImGui::PopStyleVar();
        ImGui::End();
        require((int)stack.Size == base, "cleanup restores the balance");
    }
    ImGui::Render();

    ImGui::DestroyContext();
    if (failures)
    {
        fprintf(stderr, "style_stack_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("PASS: style stack guard release/ownership and End-balance contract\n");
    return 0;
}
