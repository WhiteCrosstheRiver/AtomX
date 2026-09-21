#include "renderer.hpp"
#include "analysis.hpp"
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#include <commdlg.h>
#include <shellapi.h>
#include <future>
#include <chrono>
#include <iomanip>
#include <optional>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dwmapi.lib")
using namespace atomx;
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
static Renderer *renderer = nullptr;
static bool resized = false;
static std::filesystem::path dropped;
static std::string utf8(const std::wstring &s) {
    if (s.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string r(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), r.data(), n, nullptr, nullptr);
    return r;
}
static LRESULT WINAPI wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp))
        return 1;
    switch (msg) {
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize = {1200, 820};
        return 0;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED)
            resized = true;
        return 0;
    case WM_DROPFILES: {
        wchar_t p[32768];
        DragQueryFileW((HDROP)wp, 0, p, 32768);
        dropped = p;
        DragFinish((HDROP)wp);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU)
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
static std::filesystem::path dialog(HWND window, bool save, const wchar_t *filter,
                                    const wchar_t *ext) {
    wchar_t path[32768]{};
    OPENFILENAMEW of{};
    of.lStructSize = sizeof(of);
    of.hwndOwner = window;
    of.lpstrFilter = filter;
    of.lpstrFile = path;
    of.nMaxFile = 32768;
    of.lpstrDefExt = ext;
    of.Flags =
        OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    return (save ? GetSaveFileNameW(&of) : GetOpenFileNameW(&of)) ? std::filesystem::path(path)
                                                                  : std::filesystem::path();
}
static std::string number(uint64_t n) {
    auto s = std::to_string(n);
    for (int i = int(s.size()) - 3; i > 0; i -= 3)
        s.insert(i, ",");
    return s;
}
static ImVec4 accent{.45f, .68f, .91f, 1};
static void theme() {
    auto &s = ImGui::GetStyle();
    s.WindowPadding = {14, 12};
    s.FramePadding = {9, 6};
    s.ItemSpacing = {8, 9};
    s.WindowRounding = 0;
    s.ChildRounding = 3;
    s.FrameRounding = 4;
    s.PopupRounding = 5;
    s.ScrollbarSize = 11;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 1;
    s.FrameBorderSize = 0;
    s.GrabRounding = 3;
    auto *c = s.Colors;
    c[ImGuiCol_Text] = {.86f, .88f, .90f, 1};
    c[ImGuiCol_TextDisabled] = {.53f, .57f, .63f, 1};
    c[ImGuiCol_WindowBg] = {.157f, .173f, .2f, 1};
    c[ImGuiCol_ChildBg] = {.157f, .173f, .2f, 1};
    c[ImGuiCol_PopupBg] = {.184f, .204f, .243f, 1};
    c[ImGuiCol_Border] = {.215f, .235f, .275f, 1};
    c[ImGuiCol_FrameBg] = {.20f, .225f, .267f, 1};
    c[ImGuiCol_FrameBgHovered] = {.25f, .29f, .35f, 1};
    c[ImGuiCol_FrameBgActive] = {.27f, .33f, .42f, 1};
    c[ImGuiCol_Button] = {.21f, .24f, .29f, 1};
    c[ImGuiCol_ButtonHovered] = {.28f, .35f, .45f, 1};
    c[ImGuiCol_ButtonActive] = {.29f, .40f, .55f, 1};
    c[ImGuiCol_Header] = {.22f, .29f, .39f, 1};
    c[ImGuiCol_HeaderHovered] = {.25f, .32f, .42f, 1};
    c[ImGuiCol_HeaderActive] = {.29f, .38f, .5f, 1};
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_Tab] = {.184f, .204f, .243f, 1};
    c[ImGuiCol_TabSelected] = {.25f, .30f, .38f, 1};
    c[ImGuiCol_Separator] = c[ImGuiCol_Border];
    c[ImGuiCol_PlotHistogram] = accent;
}
static void heading(const char *title) {
    ImGui::Spacing();
    ImGui::TextDisabled("%s", title);
    ImGui::Separator();
    ImGui::Spacing();
}
struct Loaded {
    Dataset data;
    std::vector<Frame> frames;
    std::filesystem::path path;
    int frame;
};
struct App {
    HWND window;
    Renderer &gpu;
    Dataset source = crystal(24);
    PipelineResult result;
    std::vector<Modifier> mods;
    std::vector<std::vector<Modifier>> undo, redo;
    std::filesystem::path path;
    std::vector<Frame> frames;
    int current = 0, budget = 2000000;
    bool playing = false, quad = true, particles = true, cell = true, showTable = true,
         showCatalog = false;
    float radius = .23f, bg[4] = {.105f, .12f, .145f, 1}, fps = 12;
    int active = 3, propertyAxis = 2, tab = 0;
    Camera cameras[4];
    Target targets[4];
    std::future<Loaded> job;
    std::atomic<float> progress{0};
    std::atomic<bool> cancel{false};
    bool busy = false;
    std::string status = "Ready", error;
    double lastFrame = 0;
    int exportW = 1920, exportH = 1080;
    std::string filter;
    Statistics cachedStats[3];
    size_t selectedCount = 0;
    float cutoff = .8f;
    std::optional<NeighborAnalysis> analysis;
    std::future<NeighborAnalysis> analysisJob;
    bool analyzing = false;
    uint64_t revision = 0, analysisRevision = 0;
    App(HWND w, Renderer &r) : window(w), gpu(r) {
        cameras[0].mode = 0;
        cameras[1].mode = 6;
        cameras[2].mode = 4;
        update();
    }
    ~App() {
        cancel = true;
        if (job.valid())
            job.wait();
        if (analysisJob.valid())
            analysisJob.wait();
    }
    void update() {
        try {
            auto next = evaluate(source, mods);
            gpu.upload(next.data, next.selected);
            result = std::move(next);
            for (int k = 0; k < 3; k++)
                cachedStats[k] = statistics(result.data, k);
            selectedCount = std::count(result.selected.begin(), result.selected.end(), 1);
            analysis.reset();
            revision++;
        } catch (const std::exception &e) {
            error = e.what();
        }
    }
    void checkpoint() {
        if (undo.size() >= 128)
            undo.erase(undo.begin());
        undo.push_back(mods);
        redo.clear();
    }
    void add(Op op) {
        checkpoint();
        Modifier m{op};
        if (op == Op::Slice)
            m.value = (result.data.lo.z + result.data.hi.z) * .5f;
        if (op == Op::Scale)
            m.value = 1;
        mods.push_back(m);
        update();
        status = std::string("Added ") + opName(op);
    }
    void history(bool forward) {
        auto &from = forward ? redo : undo;
        auto &to = forward ? undo : redo;
        if (from.empty())
            return;
        to.push_back(mods);
        mods = std::move(from.back());
        from.pop_back();
        update();
    }
    void load(const std::filesystem::path &p, int frame = 0) {
        if (busy || p.empty())
            return;
        busy = true;
        cancel = false;
        progress = 0;
        playing = playing && p == path;
        status = "Reading trajectory...";
        auto known = p == path ? frames : std::vector<Frame>{};
        auto b = budget;
        job =
            std::async(std::launch::async, [this, p, frame, known = std::move(known), b]() mutable {
                if (known.empty())
                    known = indexXYZ(p, &progress, &cancel);
                if (frame < 0 || frame >= int(known.size()))
                    throw std::runtime_error("Frame out of range");
                auto d = readXYZ(p, known[frame], b, &progress, &cancel);
                return Loaded{std::move(d), std::move(known), p, frame};
            });
    }
    void poll() {
        if (analyzing &&
            analysisJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            analyzing = false;
            try {
                auto a = analysisJob.get();
                if (revision == analysisRevision) {
                    analysis = std::move(a);
                    status = "Neighbor analysis completed";
                } else
                    status = "Dataset changed; analysis result discarded";
            } catch (const std::exception &e) {
                error = e.what();
            }
        }
        if (busy && job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            busy = false;
            try {
                auto l = job.get();
                bool changed = path != l.path;
                source = std::move(l.data);
                frames = std::move(l.frames);
                path = l.path;
                current = l.frame;
                if (changed) {
                    mods.clear();
                    undo.clear();
                    redo.clear();
                    for (auto &c : cameras)
                        c.zoom = 1;
                }
                update();
                status = "Loaded " + number(source.sourceCount) + " atoms";
            } catch (const std::exception &e) {
                error = e.what();
                playing = false;
                status = "Load stopped";
            }
        }
        if (!dropped.empty()) {
            load(dropped);
            dropped.clear();
        }
    }
    void open() {
        load(dialog(window, false, L"XYZ trajectory\0*.xyz;*.extxyz\0All files\0*.*\0", L"xyz"));
    }
    void exportImage() {
        auto p = dialog(window, true, L"PNG image\0*.png\0", L"png");
        if (p.empty())
            return;
        Target t;
        gpu.target(t, exportW, exportH);
        gpu.draw(t, result.data, cameras[active], radius, bg, particles);
        gpu.png(t, p);
        status = "Rendered " + utf8(p.filename().wstring());
    }
    void fixed(const char *name, float x, float y, float w, float h) {
        ImGui::SetNextWindowPos({x, y});
        ImGui::SetNextWindowSize({std::max(w, 1.f), std::max(h, 1.f)});
        ImGui::Begin(name, nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoSavedSettings);
    }
    void top(float w) {
        fixed("Top", 0, 0, w, 58);
        ImGui::TextColored(accent, "A T O M X");
        ImGui::SameLine(140);
        if (ImGui::Button("Open trajectory"))
            open();
        ImGui::SameLine();
        if (ImGui::Button("Undo"))
            history(false);
        ImGui::SameLine();
        if (ImGui::Button("Redo"))
            history(true);
        ImGui::SameLine();
        if (ImGui::Button(quad ? "Single view" : "Four views"))
            quad = !quad;
        ImGui::SameLine();
        if (ImGui::Button("Fit"))
            cameras[active] = Camera{.65f, .48f, 1, 0, 0, cameras[active].mode};
        ImGui::SameLine();
        if (ImGui::Button("Modifiers"))
            showCatalog = true;
        ImGui::SameLine();
        if (ImGui::Button("Render PNG"))
            exportImage();
        ImGui::SameLine();
        ImGui::TextDisabled("  C++ / GPU workspace");
        ImGui::End();
    }
    void left(float h) {
        fixed("Workspace", 0, 58, 220, h - 88);
        heading("WORKSPACE");
        ImGui::TextColored(accent, "ATOMIC STRUCTURES");
        ImGui::Spacing();
        ImGui::Selectable(path.empty() ? "  Cu-Ni / FCC specimen"
                                       : ("  " + utf8(path.filename().wstring())).c_str(),
                          true);
        ImGui::TextDisabled("    %s", path.empty() ? "Generated dataset" : "XYZ trajectory");
        ImGui::Spacing();
        if (ImGui::Button("+ Import dataset", {-1, 32}))
            open();
        heading("SCENE");
        ImGui::Checkbox("Particles", &particles);
        ImGui::Checkbox("Simulation cell", &cell);
        ImGui::Checkbox("Data inspector", &showTable);
        heading("PARTICLE TYPES");
        for (size_t i = 0; i < source.species.size(); ++i) {
            ImGui::BulletText("%s", source.species[i].c_str());
        }
        heading("DATASET");
        ImGui::TextDisabled("Source atoms");
        ImGui::Text("%s", number(source.sourceCount).c_str());
        ImGui::TextDisabled("Displayed atoms");
        ImGui::Text("%s", number(result.data.atoms.size()).c_str());
        if (source.sampled())
            ImGui::TextColored({.9f, .73f, .42f, 1}, "SAMPLED  /  1 in %llu", source.stride);
        ImGui::TextDisabled("Frames");
        ImGui::Text("%zu", std::max<size_t>(frames.size(), 1));
        heading("QUICK ACTIONS");
        if (ImGui::Button("Slice specimen", {-1, 30}))
            add(Op::Slice);
        if (ImGui::Button("Select particle type", {-1, 30}))
            add(Op::SelectType);
        if (ImGui::Button("Reset pipeline", {-1, 30})) {
            checkpoint();
            mods.clear();
            update();
        }
        if (ImGui::Button("Load demo crystal", {-1, 30}) && !busy) {
            source = crystal(24);
            path.clear();
            frames.clear();
            current = 0;
            mods.clear();
            undo.clear();
            redo.clear();
            update();
        }
        ImGui::End();
    }
    const char *views = "Top\0Bottom\0Front\0Back\0Left\0Right\0Ortho\0Perspective\0";
    void viewport(int i, float w, float h) {
        ImGui::PushID(i);
        ImGui::BeginChild("view", {w, h}, ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        auto &cam = cameras[i];
        ImGui::SetNextItemWidth(140);
        ImGui::Combo("##camera", &cam.mode, views);
        ImGui::SameLine();
        ImGui::TextDisabled(i == active ? "ACTIVE" : "");
        ImGui::SameLine();
        if (ImGui::SmallButton("Fit")) {
            cam.zoom = 1;
            cam.panX = cam.panY = 0;
        }
        auto p = ImGui::GetCursorScreenPos();
        auto avail = ImGui::GetContentRegionAvail();
        gpu.target(targets[i], int(avail.x), int(avail.y));
        gpu.draw(targets[i], result.data, cam, radius, bg, particles);
        ImGui::Image((ImTextureID)(intptr_t)targets[i].srv.Get(), avail);
        if (ImGui::IsItemHovered()) {
            if (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1) || ImGui::GetIO().MouseWheel)
                active = i;
            if (ImGui::IsMouseDragging(0)) {
                cam.yaw -= ImGui::GetIO().MouseDelta.x * .008f;
                cam.pitch =
                    std::clamp(cam.pitch + ImGui::GetIO().MouseDelta.y * .008f, -1.55f, 1.55f);
                if (cam.mode < 6)
                    cam.mode = 6;
            }
            if (ImGui::IsMouseDragging(1) || ImGui::IsMouseDragging(2)) {
                cam.panX += ImGui::GetIO().MouseDelta.x / std::max(avail.x, 1.f) * cam.zoom;
                cam.panY -= ImGui::GetIO().MouseDelta.y / std::max(avail.y, 1.f) * cam.zoom;
            }
            cam.zoom = std::clamp(cam.zoom * powf(.85f, ImGui::GetIO().MouseWheel), .01f, 50.f);
        }
        auto *draw = ImGui::GetWindowDrawList();
        if (cell) {
            using namespace DirectX;
            auto m = gpu.matrix(result.data, cam, avail.x / std::max(avail.y, 1.f));
            ImVec2 corners[8];
            bool valid[8];
            bool lattice = source.cell[0] != 0 || source.cell[4] != 0 || source.cell[8] != 0;
            for (int j = 0; j < 8; j++) {
                float x, y, z;
                if (lattice) {
                    auto &c = result.data.cell;
                    x = float((j & 1 ? c[0] : 0) + (j & 2 ? c[3] : 0) + (j & 4 ? c[6] : 0));
                    y = float((j & 1 ? c[1] : 0) + (j & 2 ? c[4] : 0) + (j & 4 ? c[7] : 0));
                    z = float((j & 1 ? c[2] : 0) + (j & 2 ? c[5] : 0) + (j & 4 ? c[8] : 0));
                } else {
                    x = j & 1 ? result.data.hi.x : result.data.lo.x;
                    y = j & 2 ? result.data.hi.y : result.data.lo.y;
                    z = j & 4 ? result.data.hi.z : result.data.lo.z;
                }
                XMFLOAT4 q;
                XMStoreFloat4(&q, XMVector4Transform(XMVectorSet(x, y, z, 1), m));
                valid[j] = q.w > 0 && q.z > 0;
                corners[j] = {p.x + (q.x / q.w + 1) * avail.x * .5f,
                              p.y + (1 - q.y / q.w) * avail.y * .5f};
            }
            draw->PushClipRect(p, {p.x + avail.x, p.y + avail.y}, true);
            for (int j = 0; j < 8; j++)
                for (int k = 1; k <= 4; k *= 2)
                    if (!(j & k) && valid[j] && valid[j | k])
                        draw->AddLine(corners[j], corners[j | k], IM_COL32(126, 145, 164, 145));
            draw->PopClipRect();
        }
        draw->AddText({p.x + 12, p.y + avail.y - 25}, IM_COL32(130, 148, 169, 255),
                      "LMB orbit   RMB pan   Wheel zoom");
        if (i == active)
            draw->AddRect(p, {p.x + avail.x, p.y + avail.y}, IM_COL32(90, 141, 192, 210));
        ImGui::EndChild();
        ImGui::PopID();
    }
    void center(float w, float h) {
        fixed("Viewport workspace", 220, 58, w - 558, h - 88);
        ImGui::TextDisabled("SCENE  /  %s", path.empty() ? "Cu-Ni specimen"
                                                         : utf8(path.filename().wstring()).c_str());
        ImGui::Separator();
        auto avail = ImGui::GetContentRegionAvail();
        float dataH = showTable ? 175.f : 0;
        float sceneH = std::max(150.f, avail.y - dataH - 67);
        if (quad) {
            float vw = (avail.x - 8) * .5f, vh = (sceneH - 8) * .5f;
            viewport(0, vw, vh);
            ImGui::SameLine();
            viewport(1, vw, vh);
            viewport(2, vw, vh);
            ImGui::SameLine();
            viewport(3, vw, vh);
        } else
            viewport(active, avail.x, sceneH);
        ImGui::BeginChild("Timeline", {-1, 57});
        if (ImGui::Button("|<") && !frames.empty())
            load(path, 0);
        ImGui::SameLine();
        if (ImGui::Button(playing ? "Pause" : "Play"))
            playing = !playing;
        ImGui::SameLine();
        if (ImGui::Button(">|") && !frames.empty())
            load(path, int(frames.size()) - 1);
        ImGui::SameLine();
        int next = current;
        ImGui::SetNextItemWidth(std::max(120.f, avail.x - 290));
        if (ImGui::SliderInt("##frame", &next, 0, int(std::max<size_t>(frames.size(), 1)) - 1) &&
            !busy)
            load(path, next);
        ImGui::SameLine();
        ImGui::Text("%d / %zu", current + 1, std::max<size_t>(frames.size(), 1));
        ImGui::EndChild();
        if (showTable) {
            ImGui::BeginChild("Inspector", {-1, 0}, ImGuiChildFlags_Borders);
            if (ImGui::BeginTabBar("Data")) {
                if (ImGui::BeginTabItem("Particles")) {
                    ImGui::TextDisabled("%s rows  |  %zu selected%s",
                                        number(result.data.atoms.size()).c_str(), selectedCount,
                                        source.sampled() ? "  |  sampled preview" : "");
                    if (ImGui::BeginTable("Atoms", 5,
                                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                              ImGuiTableFlags_BordersInnerV,
                                          {-1, 83})) {
                        for (auto name :
                             {"Index", "Type", "Position X", "Position Y", "Position Z"})
                            ImGui::TableSetupColumn(name);
                        ImGui::TableHeadersRow();
                        ImGuiListClipper clip;
                        clip.Begin(int(result.data.atoms.size()));
                        while (clip.Step())
                            for (int j = clip.DisplayStart; j < clip.DisplayEnd; ++j) {
                                auto a = result.data.atoms[j];
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                if (ImGui::Selectable(std::to_string(j).c_str(),
                                                      result.selected[j] != 0,
                                                      ImGuiSelectableFlags_SpanAllColumns)) {
                                    checkpoint();
                                    mods.push_back({Op::SelectIndex, true, 0, 2, j});
                                    update();
                                }
                                ImGui::TableNextColumn();
                                ImGui::TextUnformatted(result.data.species[a.type].c_str());
                                ImGui::TableNextColumn();
                                ImGui::Text("%.5f", a.x);
                                ImGui::TableNextColumn();
                                ImGui::Text("%.5f", a.y);
                                ImGui::TableNextColumn();
                                ImGui::Text("%.5f", a.z);
                            }
                        ImGui::EndTable();
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Simulation cell")) {
                    for (int row = 0; row < 3; row++)
                        ImGui::Text("%12.4f   %12.4f   %12.4f", result.data.cell[row * 3],
                                    result.data.cell[row * 3 + 1], result.data.cell[row * 3 + 2]);
                    ImGui::Text("PBC: %s / %s / %s", source.pbc[0] ? "X" : "-",
                                source.pbc[1] ? "Y" : "-", source.pbc[2] ? "Z" : "-");
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Global attributes")) {
                    ImGui::TextWrapped("%s", source.comment.c_str());
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::EndChild();
        }
        ImGui::End();
    }
    void right(float w, float h) {
        fixed("Properties", w - 338, 58, 338, h - 88);
        if (ImGui::BeginTabBar("Settings")) {
            if (ImGui::BeginTabItem("Pipeline")) {
                heading("MODIFIER STACK");
                if (ImGui::Button("+ Add modifier", {-1, 32}))
                    showCatalog = true;
                ImGui::BeginChild("Stack", {-1, 230}, ImGuiChildFlags_Borders);
                if (mods.empty())
                    ImGui::TextDisabled("No modifiers\nSource passes through unchanged.");
                for (int i = int(mods.size()) - 1; i >= 0; i--) {
                    ImGui::PushID(i);
                    auto &m = mods[i];
                    bool enabled = m.enabled;
                    if (ImGui::Checkbox("##enabled", &enabled)) {
                        checkpoint();
                        m.enabled = enabled;
                        update();
                    }
                    ImGui::SameLine();
                    ImGui::TextUnformatted(opName(m.op));
                    if (ImGui::SmallButton("Remove")) {
                        checkpoint();
                        mods.erase(mods.begin() + i);
                        update();
                        ImGui::PopID();
                        break;
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Earlier") && i > 0) {
                        checkpoint();
                        std::swap(mods[i], mods[i - 1]);
                        update();
                    }
                    if (m.op == Op::Slice || m.op == Op::Translate || m.op == Op::Scale) {
                        float v = m.value;
                        ImGui::SetNextItemWidth(140);
                        if (ImGui::DragFloat("Value", &v, .1f)) {
                            checkpoint();
                            m.value = v;
                            update();
                        }
                        if (m.op != Op::Scale) {
                            int a = m.axis;
                            ImGui::SetNextItemWidth(140);
                            if (ImGui::Combo("Axis", &a, "X\0Y\0Z\0")) {
                                checkpoint();
                                m.axis = a;
                                update();
                            }
                        }
                    }
                    if (m.op == Op::SelectType) {
                        int t = m.type;
                        if (ImGui::InputInt("Type index", &t)) {
                            checkpoint();
                            m.type = std::clamp(t, 0, std::max(0, int(source.species.size()) - 1));
                            update();
                        }
                    }
                    ImGui::Separator();
                    ImGui::PopID();
                }
                ImGui::EndChild();
                heading("DATA SOURCE");
                ImGui::TextWrapped("%s", path.empty() ? "Generated FCC crystal"
                                                      : utf8(path.wstring()).c_str());
                ImGui::TextDisabled("Reader: XYZ / Extended XYZ");
                ImGui::TextDisabled("Trajectory: %zu frame(s)", std::max<size_t>(frames.size(), 1));
                heading("PARTICLE APPEARANCE");
                ImGui::SliderFloat("Radius", &radius, .02f, 2.f, "%.2f");
                ImGui::TextDisabled("Color: particle type / selection");
                heading("PROPERTY STATISTICS");
                ImGui::Combo("Property", &propertyAxis, "Position X\0Position Y\0Position Z\0");
                auto s = cachedStats[propertyAxis];
                ImGui::PlotHistogram("##hist", s.histogram.data(), 64, 0, nullptr, 0, FLT_MAX,
                                     {-1, 80});
                ImGui::Text("Min %.3f   Max %.3f", s.min, s.max);
                ImGui::Text("Mean %.3f", s.mean);
                if (source.sampled())
                    ImGui::TextColored({.9f, .73f, .42f, 1}, "Statistics describe preview only.");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Render")) {
                heading("GPU RENDERER");
                ImGui::TextWrapped("%s", utf8(gpu.adapterName).c_str());
                ImGui::TextDisabled("Direct3D 11 / sphere impostors");
                ImGui::Text("Atom buffers: %.1f MiB", gpu.gpuBytes / 1048576.);
                heading("CAMERA");
                ImGui::Combo("View", &cameras[active].mode, views);
                heading("OUTPUT");
                ImGui::InputInt("Width", &exportW);
                ImGui::InputInt("Height", &exportH);
                exportW = std::clamp(exportW, 64, 8192);
                exportH = std::clamp(exportH, 64, 8192);
                ImGui::ColorEdit3("Background", bg);
                ImGui::TextWrapped("PNG exports particles from the active camera. UI cell overlay "
                                   "is not included.");
                if (ImGui::Button("Render active viewport", {-1, 35}))
                    exportImage();
                heading("TRAJECTORY");
                ImGui::SliderFloat("Frames/sec", &fps, 1, 60, "%.0f");
                heading("EXPORT DATA");
                ImGui::TextWrapped(source.sampled()
                                       ? "Exports the processed preview, not all source atoms."
                                       : "Exports the processed particle dataset.");
                if (ImGui::Button("Export XYZ", {-1, 32})) {
                    auto p = dialog(window, true, L"XYZ file\0*.xyz\0", L"xyz");
                    if (!p.empty()) {
                        writeXYZ(p, result.data);
                        status = "XYZ exported";
                    }
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Analysis")) {
                heading("NEIGHBOR ANALYSIS");
                ImGui::TextWrapped("Coordination number and connected clusters using spatial bins "
                                   "and minimum-image periodic distances.");
                ImGui::InputFloat("Cutoff", &cutoff, .05f, .1f, "%.3f");
                if (ImGui::Button("Compute neighbors", {-1, 32}) && !analyzing) {
                    analysisRevision = revision;
                    auto d = result.data;
                    auto c = cutoff;
                    analyzing = true;
                    analysisJob = std::async(std::launch::async, [d = std::move(d), c, this] {
                        return neighbors(d, c, &cancel);
                    });
                }
                if (analyzing)
                    ImGui::TextDisabled("Computing in background...");
                if (analysis) {
                    ImGui::Text("Clusters: %u", analysis->clusters);
                    ImGui::Text("Neighbor pairs: %llu", analysis->bonds);
                    ImGui::Text("Mean coordination: %.4f", analysis->meanCoordination);
                    if (ImGui::Button("Export analysis CSV", {-1, 32})) {
                        auto p = dialog(window, true, L"CSV file\0*.csv\0", L"csv");
                        if (!p.empty()) {
                            std::ofstream out(p);
                            out << "index,coordination,cluster\n";
                            for (size_t i = 0; i < analysis->coordination.size(); i++)
                                out << i << "," << analysis->coordination[i] << ","
                                    << analysis->cluster[i] << "\n";
                            if (!out)
                                throw std::runtime_error("Cannot write analysis CSV");
                            status = "Analysis exported";
                        }
                    }
                }
                ImGui::TextWrapped("Requires unsampled data. Periodic cells must be orthogonal. "
                                   "Current analysis limit: 2 million atoms.");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("System")) {
                heading("AVAILABLE ADAPTERS");
                for (auto a : gpu.adapters) {
                    ImGui::TextWrapped("[%u] %s", a.index, utf8(a.name).c_str());
                    ImGui::TextDisabled("Dedicated memory: %.2f GiB", a.memory / 1073741824.);
                }
                ImGui::TextWrapped("Select at launch: AtomX.exe --adapter N");
                heading("MEMORY BUDGET");
                ImGui::InputInt("Preview atoms", &budget, 100000, 1000000);
                budget = std::clamp(budget, 1000, 20000000);
                ImGui::TextWrapped(
                    "Files larger than this budget use deterministic stride sampling. All records "
                    "are scanned; only preview atoms are retained.");
                if (ImGui::Button("Reload with budget", {-1, 32}))
                    load(path, current);
                heading("ENGINE");
                ImGui::BulletText("16 bytes / displayed atom");
                ImGui::BulletText("1,048,576 atoms / GPU chunk");
                ImGui::BulletText("64-bit file offsets and counts");
                ImGui::BulletText("Background trajectory import");
                ImGui::TextWrapped(
                    "Hundreds of millions of full-resolution atoms and advanced OVITO analysis are "
                    "development targets, not validated capabilities.");
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::End();
    }
    void catalog() {
        if (showCatalog) {
            ImGui::OpenPopup("Modifier library");
            showCatalog = false;
        }
        ImGui::SetNextWindowSize({650, 600}, ImGuiCond_FirstUseEver);
        if (ImGui::BeginPopupModal("Modifier library", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::TextColored(accent, "MODIFIER LIBRARY");
            ImGui::TextDisabled("Implemented operations are available below.");
            for (auto op : {Op::Slice, Op::SelectType, Op::Invert, Op::Clear, Op::Delete,
                            Op::Translate, Op::Scale, Op::Wrap, Op::ColorType}) {
                if (ImGui::Selectable(opName(op))) {
                    add(op);
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::Separator();
            ImGui::TextDisabled("PLANNED / NOT IMPLEMENTED");
            ImGui::BeginChild("planned", {-1, -45});
            ImGui::TextWrapped(
                "Analysis: atomic strain, bond analysis, difference between frames, dislocation "
                "analysis (DXA), displacement vectors, elastic strain, find rings, grain "
                "segmentation, reduce property, scatter plot, spatial binning, spatial "
                "correlation, structure factor, time averaging, time series, Voronoi, "
                "Wigner-Seitz.\n\nStructure: Ackland-Jones, centrosymmetry, Chill+, CNA, diamond "
                "identification, PTM, VoroTop.\n\nGeometry: surface mesh, coordination polyhedra, "
                "bonds, isosurface, trajectory lines.\n\nOther: ambient occlusion, custom colors, "
                "compute/freeze property, combine datasets, replicate, smooth/unwrap trajectories, "
                "expression/manual/expanded selection, Python modifiers, animation export, offline "
                "ray tracing.\n\nSee docs/FEATURE_MATRIX.md for the complete reference-image "
                "mapping.");
            ImGui::EndChild();
            if (ImGui::Button("Close", {120, 30}))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    void ui() {
        poll();
        auto &io = ImGui::GetIO();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O))
            open();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z))
            history(false);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y))
            history(true);
        if (playing && !busy && frames.size() > 1 && ImGui::GetTime() - lastFrame > 1 / fps) {
            lastFrame = ImGui::GetTime();
            load(path, (current + 1) % int(frames.size()));
        }
        float w = io.DisplaySize.x, h = io.DisplaySize.y;
        top(w);
        left(h);
        right(w, h);
        center(w, h);
        fixed("Status", 0, h - 30, w, 30);
        ImGui::SetCursorPosY(6);
        ImGui::TextColored(accent, "GPU");
        ImGui::SameLine();
        ImGui::TextDisabled("%s  |  %.1f FPS  |  %s drawn", utf8(gpu.adapterName).c_str(),
                            io.Framerate, number(result.data.atoms.size()).c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("  %s", status.c_str());
        ImGui::End();
        catalog();
        if (busy)
            ImGui::OpenPopup("Loading trajectory");
        if (ImGui::BeginPopupModal("Loading trajectory", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Indexing / reading XYZ records...");
            ImGui::ProgressBar(progress, {400, 20});
            if (ImGui::Button("Cancel"))
                cancel = true;
            if (!busy)
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (!error.empty())
            ImGui::OpenPopup("Operation failed");
        if (ImGui::BeginPopupModal("Operation failed", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("%s", error.c_str());
            if (ImGui::Button("OK")) {
                error.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
};
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    try {
        SetProcessDPIAware();
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        int argc;
        auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        int adapter = -1, smoke = 0;
        std::filesystem::path input, shot;
        for (int i = 1; i < argc; i++) {
            std::wstring a = argv[i];
            if (a == L"--adapter" && i + 1 < argc)
                adapter = _wtoi(argv[++i]);
            else if (a == L"--smoke" && i + 1 < argc)
                smoke = _wtoi(argv[++i]);
            else if (a == L"--screenshot" && i + 1 < argc)
                shot = argv[++i];
            else
                input = a;
        }
        LocalFree(argv);
        WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, wndProc,  0,
                       0,          instance,   nullptr,  LoadCursor(nullptr, IDC_ARROW),
                       nullptr,    nullptr,    L"AtomX", nullptr};
        RegisterClassExW(&wc);
        HWND window = CreateWindowW(wc.lpszClassName, L"AtomX - Atomic visualization workspace",
                                    WS_OVERLAPPEDWINDOW, 80, 50, 1560, 1000, nullptr, nullptr,
                                    instance, nullptr);
        Renderer gpu;
        renderer = &gpu;
        gpu.init(window, adapter);
        ImGui::CreateContext();
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf", 17);
        theme();
        ImGui_ImplWin32_Init(window);
        ImGui_ImplDX11_Init(gpu.device.Get(), gpu.context.Get());
        ShowWindow(window, SW_SHOWDEFAULT);
        DragAcceptFiles(window, TRUE);
        {
            App app(window, gpu);
            if (!input.empty())
                app.load(input);
            bool done = false;
            int ticks = 0;
            while (!done) {
                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                    if (msg.message == WM_QUIT)
                        done = true;
                }
                if (done)
                    break;
                if (IsIconic(window)) {
                    Sleep(20);
                    continue;
                }
                if (resized) {
                    gpu.resize();
                    resized = false;
                }
                ImGui_ImplDX11_NewFrame();
                ImGui_ImplWin32_NewFrame();
                ImGui::NewFrame();
                try {
                    app.ui();
                } catch (const std::exception &e) {
                    app.error = e.what();
                }
                ImGui::Render();
                float clear[4] = {.15f, .17f, .2f, 1};
                gpu.context->OMSetRenderTargets(1, gpu.back.GetAddressOf(), nullptr);
                gpu.context->ClearRenderTargetView(gpu.back.Get(), clear);
                ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
                if (smoke && !app.busy && ++ticks >= smoke) {
                    if (!app.error.empty())
                        throw std::runtime_error(app.error);
                    if (!shot.empty()) {
                        Target t;
                        ComPtr<ID3D11Resource> res;
                        gpu.back->GetResource(&res);
                        check(res.As(&t.texture), "Screenshot texture");
                        D3D11_TEXTURE2D_DESC d;
                        t.texture->GetDesc(&d);
                        t.w = d.Width;
                        t.h = d.Height;
                        gpu.png(t, shot);
                    }
                    std::ofstream report("smoke-report.txt");
                    report << "adapter=" << utf8(gpu.adapterName)
                           << "\natoms=" << app.result.data.atoms.size()
                           << "\nsource_atoms=" << app.source.sourceCount
                           << "\nsample_stride=" << app.source.stride
                           << "\ntrajectory_frames=" << std::max<size_t>(app.frames.size(), 1)
                           << "\ngpu_bytes=" << gpu.gpuBytes << "\nframes=" << ticks
                           << "\nfps=" << io.Framerate << "\n";
                    done = true;
                }
                check(gpu.swap->Present(1, 0), "Present");
            }
        }
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        DestroyWindow(window);
        UnregisterClassW(wc.lpszClassName, instance);
        CoUninitialize();
        return 0;
    } catch (const std::exception &e) {
        std::ofstream("atomx-error.txt") << e.what();
        MessageBoxA(nullptr, e.what(), "AtomX startup error", MB_ICONERROR);
        return 1;
    }
}
