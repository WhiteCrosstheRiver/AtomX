#include "renderer.hpp"
#include "analysis.hpp"
#include "structure_io.hpp"
#include "desktop.hpp"
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#include <commdlg.h>
#include <shellapi.h>
#include <future>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <optional>
#include <memory>
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
    if (msg == desktop::taskbarCreated && desktop::inTray) {
        if (!Shell_NotifyIconW(NIM_ADD, &desktop::tray)) desktop::restore(h);
        return 0;
    }
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wp) return 0;
        break;
    case WM_NCHITTEST:
        return desktop::hitTest(h, lp);
    case WM_CLOSE:
        desktop::hide(h);
        return 0;
    case desktop::trayMessage:
        if (lp == WM_LBUTTONDBLCLK) desktop::restore(h);
        if (lp == WM_RBUTTONUP) {
            auto menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, L"Restore AtomX");
            AppendMenuW(menu, MF_STRING, 2, L"Exit AtomX");
            POINT p; GetCursorPos(&p); SetForegroundWindow(h);
            int action = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, h, nullptr);
            DestroyMenu(menu);
            if (action == 1) desktop::restore(h);
            if (action == 2) PostQuitMessage(0);
            PostMessageW(h, WM_NULL, 0, 0);
        }
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize = {1200, 820};
        {
            MONITORINFO monitor{sizeof(monitor)};
            GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &monitor);
            auto *m = (MINMAXINFO *)lp;
            m->ptMaxPosition = {monitor.rcWork.left - monitor.rcMonitor.left,
                                monitor.rcWork.top - monitor.rcMonitor.top};
            m->ptMaxSize = {monitor.rcWork.right - monitor.rcWork.left,
                            monitor.rcWork.bottom - monitor.rcWork.top};
        }
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
static float uiScale = 1;
static float U(float value) { return value * uiScale; }
static ImFont *headingFont = nullptr;
static ImVec4 accent{.10f,.34f,.62f,1};
static void theme(int choice = 1) {
    ImGui::GetStyle() = ImGuiStyle{};
    if (choice == 1) ImGui::StyleColorsLight(); else ImGui::StyleColorsDark();
    auto &s = ImGui::GetStyle();
    s.WindowPadding = {10,8}; s.FramePadding = {8,5}; s.ItemSpacing = {8,7};
    s.WindowRounding = 0; s.ChildRounding = 3; s.FrameRounding = 3; s.PopupRounding = 5;
    s.ScrollbarSize = 13; s.WindowBorderSize = 0; s.ChildBorderSize = 1;
    s.FrameBorderSize = 1; s.PopupBorderSize = 1; s.GrabRounding = 2;
    s.DisabledAlpha = .72f;
    auto *c = s.Colors;
    if (choice == 1) {
        accent = {.08f,.32f,.61f,1};
        c[ImGuiCol_Text] = {.08f,.10f,.13f,1};
        c[ImGuiCol_TextDisabled] = {.35f,.38f,.42f,1};
        c[ImGuiCol_WindowBg] = {.935f,.945f,.955f,1};
        c[ImGuiCol_ChildBg] = {1,1,1,1};
        c[ImGuiCol_PopupBg] = {.95f,.96f,.97f,1};
        c[ImGuiCol_Border] = {.64f,.68f,.73f,1};
        c[ImGuiCol_FrameBg] = {1,1,1,1};
        c[ImGuiCol_FrameBgHovered] = {.91f,.95f,1,1};
        c[ImGuiCol_FrameBgActive] = {.84f,.91f,.99f,1};
        c[ImGuiCol_Button] = {.97f,.98f,.99f,1};
        c[ImGuiCol_ButtonHovered] = {.85f,.92f,1,1};
        c[ImGuiCol_ButtonActive] = {.73f,.85f,.98f,1};
        c[ImGuiCol_Header] = {.84f,.90f,.97f,1};
        c[ImGuiCol_HeaderHovered] = {.80f,.88f,.98f,1};
        c[ImGuiCol_HeaderActive] = {.73f,.84f,.96f,1};
        c[ImGuiCol_Tab] = {.87f,.89f,.92f,1};
        c[ImGuiCol_TabHovered] = {.79f,.87f,.97f,1};
        c[ImGuiCol_TabSelected] = {1,1,1,1};
        c[ImGuiCol_TitleBgActive] = {.86f,.90f,.95f,1};
        c[ImGuiCol_TableHeaderBg] = {.86f,.89f,.93f,1};
        c[ImGuiCol_TableRowBgAlt] = {.10f,.25f,.45f,.045f};
    } else {
        accent = {.48f,.74f,1,1};
        c[ImGuiCol_Text] = {.95f,.96f,.98f,1};
        c[ImGuiCol_TextDisabled] = {.70f,.74f,.80f,1};
        c[ImGuiCol_WindowBg] = choice == 2 ? ImVec4{.07f,.08f,.10f,1} : ImVec4{.14f,.16f,.19f,1};
        c[ImGuiCol_ChildBg] = {.10f,.12f,.15f,1};
        c[ImGuiCol_PopupBg] = {.16f,.18f,.22f,1};
        c[ImGuiCol_Border] = {.39f,.44f,.51f,1};
        c[ImGuiCol_FrameBg] = {.09f,.11f,.14f,1};
        c[ImGuiCol_Button] = {.23f,.27f,.33f,1};
        c[ImGuiCol_Header] = {.25f,.34f,.45f,1};
        c[ImGuiCol_TabSelected] = {.28f,.35f,.44f,1};
    }
    c[ImGuiCol_CheckMark] = accent; c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_PlotHistogram] = accent; c[ImGuiCol_PlotLines] = accent;
    c[ImGuiCol_Separator] = c[ImGuiCol_Border];
    s.ScaleAllSizes(uiScale);
}
static void heading(const char *title) {
    ImGui::Spacing();
    auto p = ImGui::GetCursorScreenPos();
    float width = ImGui::GetContentRegionAvail().x, height = ImGui::GetFontSize()+U(12);
    auto *d = ImGui::GetWindowDrawList();
    d->AddRectFilled(p,{p.x+width,p.y+height},ImGui::GetColorU32(ImGuiCol_Tab),U(3));
    if (headingFont) ImGui::PushFont(headingFont);
    d->AddText({p.x+U(8),p.y+U(6)},ImGui::GetColorU32(ImGuiCol_Text),title);
    if (headingFont) ImGui::PopFont();
    ImGui::Dummy({width,height});
}
struct Loaded {
    Dataset data;
    std::vector<Frame> frames;
    std::filesystem::path path;
    int frame;
};
struct HistogramPlotView {
    int firstBin = 0;
    int lastBin = -1;
    float yMaximum = 0; // zero means automatic
};
struct PipelineJobResult {
    PipelineResult result;
    size_t checkpointNode = SIZE_MAX;
    std::optional<PipelineResult> checkpoint;
};
struct App {
    HWND window;
    desktop::Preferences preferences;
    ComPtr<ID3D11ShaderResourceView> logo;
    HICON icon = nullptr;
    bool showSettings = false, refreshFont = false, selectAnalysis = false, selectPipeline = false;
    ImVec2 catalogAnchor{};
    char modifierSearch[128]{};
    bool showWorkspace = false, indexing = false;
    int pendingFrame = -1;
    Renderer &gpu;
    Dataset source = crystal(24);
    PipelineResult result;
    PipelineGraph modifierGraph;
    std::vector<ModifierNode> &mods = modifierGraph.nodes;
    std::vector<std::vector<ModifierNode>> undo, redo;
    InteractiveEditCheckpoint editCheckpoint;
    uint64_t nextModifierId = 1;
    std::filesystem::path path;
    std::vector<Frame> frames;
    int current = 0, budget = 2000000;
    bool playing = false, quad = true, particles = true, cell = true, showTable = false,
         showCatalog = false;
    bool histogramPreviewTab = false;
    std::map<std::string, HistogramPlotView> histogramPlotViews;
    float radius = .32f, bg[4] = {0, 0, 0, 1}, fps = 12;
    int particleShape = 0;
    int appearanceType = 0;
    std::vector<std::string> appearanceNames;
    std::unordered_map<std::string, ParticleStyle> appearanceMemory;
    std::unordered_map<std::string, float> customRadiusMemory;

    void syncAppearance(const std::vector<std::string>& names) {
        std::string selected;
        if (appearanceType >= 0 && size_t(appearanceType) < appearanceNames.size())
            selected = appearanceNames[appearanceType];
        for (size_t i = 0; i < appearanceNames.size() && i < gpu.styles.size(); ++i)
            appearanceMemory[appearanceNames[i]] = gpu.styles[i];
        if (names != appearanceNames || gpu.styles.size() != std::max<size_t>(names.size(), 1)) {
            gpu.resetStyles(names.size());
            for (size_t i = 0; i < names.size(); ++i) {
                auto found = appearanceMemory.find(names[i]);
                if (found != appearanceMemory.end()) gpu.styles[i] = found->second;
            }
            appearanceNames = names;
            auto found = std::find(names.begin(), names.end(), selected);
            appearanceType = found == names.end() ? 0 : int(found - names.begin());
        }
    }
    void setDefaultRadius(const std::string& name, ParticleStyle& style, bool inherit) {
        if (inherit) {
            if (style.visual[0] > 0) customRadiusMemory[name] = style.visual[0];
            style.visual[0] = 0;
        } else {
            auto found = customRadiusMemory.find(name);
            style.visual[0] = found == customRadiusMemory.end() ? radius : found->second;
        }
    }
    bool focusParticleAppearance = false;
    int renderMode = 0;
    float cellColor[4] = {0.82f,0.9f,1.f,0.88f};
    float cellWidth = 1.25f, cellGlow = 0.35f;
    bool cellDashed = false, cellLabels = false;
    int cellDimension = 1;
    bool colorCoding = false, colorDiscrete = false, colorSelectedOnly = false, colorAutoRange = false,
         colorSymmetricRange = false, colorReverse = false;
    bool colorLegend = true;
    int colorAxis = 0, colorGradient = 0;
    float colorMin = 0, colorMax = 1;
    std::string colorRangeProperty = "Position.X";
    int active = 3, propertyAxis = 2, tab = 0;
    int viewportTool = 2; // 0 zoom, 1 pan, 2 orbit, 3 perspective
    bool animationSettings = false, autoKey = false;
    Camera cameras[4];
    Target targets[4];
    std::future<Loaded> job;
    std::future<PipelineJobResult> pipelineJob;
    std::future<PipelineResult> inspectorJob;
    std::optional<PipelineResult> inspectorResult;
    std::atomic<bool> pipelineCancel{false};
    std::atomic<bool> inspectorCancel{false};
    std::atomic<size_t> pipelineActiveNode{0};
    std::atomic<size_t> inspectorActiveNode{0};
    bool pipelineBusy = false;
    bool inspectorBusy = false;
    bool pipelineDeferredForInspector = false;
    int inspectorNode = -1;
    uint64_t inspectorGeneration = 0;
    std::string inspectorNodeId;
    std::filesystem::path deferredLoadPath;
    int deferredLoadFrame = 0;
    uint64_t pipelineGeneration = 0, pipelineJobGeneration = 0;
    std::vector<std::string> pipelineJobNodeIds;
    std::shared_ptr<const PipelineResult> pipelineCheckpoint;
    size_t pipelineCheckpointNode = SIZE_MAX;
    std::atomic<float> progress{0};
    std::atomic<bool> cancel{false};
    bool busy = false, staleResult = false, staleBeforeLoad = false;
    std::string status = "Ready", error;
    std::string readerName = "Generated crystal";
    double lastFrame = 0;
    int exportW = 1920, exportH = 1080;
    bool showDataExport = false, exportRange = false, exportSequence = false, exporting = false,
         exportPreview = false;
    int exportFormat = 0, exportFirst = 0, exportLast = 0, exportStep = 1;
    io::ExportOptions exportOptions;
    std::future<std::string> exportJob;
    std::atomic<bool> exportCancel{false};
    std::atomic<float> exportProgress{0};
    std::string filter;
    struct UiTestItem { ImGuiID id=0; ImVec2 min{}, max{}; bool hovered=false, clicked=false; };
    bool captureUiTestItems = false;
    std::unordered_map<std::string,UiTestItem> uiTestItems;
    Statistics cachedStats[3];
    size_t selectedCount = 0;
    float cutoff = .8f;
    std::optional<NeighborAnalysis> analysis;
    std::future<NeighborAnalysis> analysisJob;
    bool analyzing = false;
    uint64_t revision = 0, analysisRevision = 0;
    std::optional<DXAResult> dxa;
    std::future<DXAResult> dxaJob;
    bool dxaRunning = false;
    float dxaCutoff = .8f;
    std::future<std::pair<double, double>> colorRangeJob;
    std::atomic<bool> colorRangeCancel{false};
    std::atomic<float> colorRangeProgress{0};
    bool colorRangeRunning = false;
    uint64_t colorRangePipelineGeneration = 0;
    std::string colorRangeNodeId;
    std::filesystem::path colorRangePath;
    App(HWND w, Renderer &r) : window(w), gpu(r) {
        preferences.load();
        theme(preferences.theme);
        refreshFont = true;
        logo = desktop::loadLogo(gpu.device.Get(), icon);
        SendMessageW(window, WM_SETICON, ICON_BIG, (LPARAM)icon);
        SendMessageW(window, WM_SETICON, ICON_SMALL, (LPARAM)icon);
        desktop::initTray(window, icon);
        cameras[0].mode = 0;
        cameras[1].mode = 2;
        cameras[2].mode = 4;
        update();
    }
    ~App() {
        Shell_NotifyIconW(NIM_DELETE, &desktop::tray);
        if (icon) DestroyIcon(icon);
        cancel = true;
        pipelineCancel = true;
        if (job.valid())
            job.wait();
        if (pipelineJob.valid()) pipelineJob.wait();
        inspectorCancel = true;
        if (inspectorJob.valid()) inspectorJob.wait();
        if (analysisJob.valid())
            analysisJob.wait();
        if (dxaJob.valid()) dxaJob.wait();
        colorRangeCancel = true;
        if (colorRangeJob.valid()) colorRangeJob.wait();
        exportCancel = true;
        if (exportJob.valid())
            exportJob.wait();
    }
    void publishPipeline(PipelineResult next) {
        try {
            auto activeColor = std::find_if(mods.rbegin(), mods.rend(), [](const Modifier &m) {
                return m.enabled && (m.op == Op::ColorCoding || m.op == Op::ColorType || m.op == Op::AssignColor);
            });
            colorCoding = activeColor != mods.rend() && activeColor->op == Op::ColorCoding;
            if (colorCoding) {
                colorGradient = activeColor->colorGradient;
                colorAutoRange = activeColor->colorAutoRange;
                colorSymmetricRange = activeColor->colorSymmetricRange;
                colorReverse = activeColor->colorReverse;
                colorLegend = activeColor->colorLegend;
                colorDiscrete = activeColor->colorDiscrete;
                colorSelectedOnly = activeColor->colorSelectedOnly;
                colorRangeProperty = activeColor->property;
                colorMin = activeColor->colorMin;
                colorMax = activeColor->colorMax;
            }
            if (colorCoding && activeColor->colorAutoRange) {
                double lo = activeColor->colorAllFramesRange ? activeColor->colorMin
                    : std::numeric_limits<double>::infinity();
                double hi = activeColor->colorAllFramesRange ? activeColor->colorMax
                    : -std::numeric_limits<double>::infinity();
                if (!activeColor->colorAllFramesRange) {
                    if (auto it = next.data.scalarProperties.find("Color coding");
                        it != next.data.scalarProperties.end() && it->second.size() == next.data.atoms.size()) {
                        for (double value : it->second)
                            if (std::isfinite(value)) { lo = std::min(lo, value); hi = std::max(hi, value); }
                    } else {
                        for (const auto &a : next.data.atoms) {
                            double value = coordinate(a, colorAxis);
                            lo = std::min(lo, value); hi = std::max(hi, value);
                        }
                    }
                }
                if (std::isfinite(lo) && std::isfinite(hi)) {
                    if (lo == hi) {
                        const float center = float(lo);
                        const double delta = std::max(std::abs(double(center)) * .01, .5);
                        const double lowValue = double(center) - delta;
                        const double highValue = double(center) + delta;
                        const double floatLimit = std::numeric_limits<float>::max();
                        float expandedLow = lowValue < -floatLimit
                            ? std::nextafter(center, -std::numeric_limits<float>::infinity())
                            : float(lowValue);
                        float expandedHigh = highValue > floatLimit
                            ? std::nextafter(center, std::numeric_limits<float>::infinity())
                            : float(highValue);
                        if (std::isfinite(expandedLow)) lo = expandedLow;
                        if (std::isfinite(expandedHigh)) hi = expandedHigh;
                    }
                    if (colorSymmetricRange) {
                        double extent = std::max(std::abs(lo), std::abs(hi));
                        lo = -extent; hi = extent;
                    }
                    colorMin = float(lo); colorMax = float(hi);
                }
            }
            gpu.upload(next.data, next.selected, next.colorSelected);
            syncAppearance(next.data.species);
            result = std::move(next);
            for (auto &camera : cameras)
                if (camera.fitSelected) refreshSelectionFit(camera,result.data,result.selected);
            for (size_t i = 0; i < modifierGraph.nodes.size(); ++i) {
                auto &node = modifierGraph.nodes[i];
                node.dirty = false;
                node.running = false;
                node.error.clear();
                node.outputs.clear();
                if (!node.enabled) continue;
                node.outputs=modifierOutputs(node,i);
            }
            for (int k = 0; k < 3; k++)
                cachedStats[k] = statistics(result.data, k);
            selectedCount = std::count(result.selected.begin(), result.selected.end(), 1);
            analysis.reset();
            staleResult = false;
            revision++;
        } catch (const std::exception &e) {
            error = e.what();
        }
    }
    void refreshSelectionFit(Camera &camera, const Dataset &data,
                             const std::vector<uint8_t> &selection, bool force = false) {
        if (!force && !camera.fitSelected) return;
        Vec3 lo{std::numeric_limits<float>::max(),std::numeric_limits<float>::max(),std::numeric_limits<float>::max()};
        Vec3 hi{-lo.x,-lo.y,-lo.z};
        bool found=false;
        for (size_t i=0;i<data.atoms.size() && i<selection.size();++i) {
            if (!selection[i]) continue;
            found=true;
            const auto &atom=data.atoms[i];
            lo.x=std::min(lo.x,atom.x); lo.y=std::min(lo.y,atom.y); lo.z=std::min(lo.z,atom.z);
            hi.x=std::max(hi.x,atom.x); hi.y=std::max(hi.y,atom.y); hi.z=std::max(hi.z,atom.z);
        }
        if (!found) { camera.fitSelected=false; return; }
        const float pad=std::max(radius*1.4f,.18f);
        camera.fitLo={lo.x-pad,lo.y-pad,lo.z-pad};
        camera.fitHi={hi.x+pad,hi.y+pad,hi.z+pad};
        camera.fitSelected=true;
    }
    void fitCamera(int index, bool selectionOnly) {
        auto &camera=cameras[std::clamp(index,0,3)];
        camera.zoom=1;
        camera.panX=camera.panY=0;
        if (selectionOnly) {
            refreshSelectionFit(camera,result.data,result.selected,true);
            if (!camera.fitSelected) {
                status="Select one or more particles before Fit selected";
                return;
            }
        } else camera.fitSelected=false;
    }
    void launchPipeline() {
        if (pipelineBusy) return;
        std::vector<Modifier> executable;
        executable.reserve(mods.size());
        pipelineJobNodeIds.clear();
        for (const auto &node : mods) {
            executable.push_back(static_cast<const Modifier &>(node));
            pipelineJobNodeIds.push_back(node.id);
        }
        const size_t checkpointTarget=mods.empty() ? SIZE_MAX :
            std::min(modifierGraph.selected,mods.size()-1);
        size_t firstNode=0;
        std::shared_ptr<const PipelineResult> cachedPrefix;
        if (pipelineCheckpoint && pipelineCheckpointNode<=checkpointTarget &&
            pipelineCheckpointNode<=executable.size()) {
            firstNode=pipelineCheckpointNode;
            cachedPrefix=pipelineCheckpoint;
        }
        Dataset input;
        if (!cachedPrefix) input=source;
        pipelineJobGeneration = pipelineGeneration;
        pipelineCancel = false;
        pipelineActiveNode = 0;
        pipelineBusy = true;
        error.clear();
        status = "Updating pipeline; previous evaluated result remains visible...";
        for (auto &node : mods) node.running = node.enabled;
        pipelineJob = std::async(std::launch::async,
            [this, input = std::move(input), executable = std::move(executable),
             cachedPrefix = std::move(cachedPrefix), firstNode, checkpointTarget]() mutable {
                PipelineResult initial;
                if (cachedPrefix) initial=*cachedPrefix;
                else {
                    const size_t count=input.atoms.size();
                    initial={std::move(input),std::vector<uint8_t>(count),
                             std::vector<uint8_t>(count,1)};
                }
                std::optional<PipelineResult> checkpoint;
                auto saveCheckpoint=[&](size_t,const PipelineResult &state) {
                    if (pipelineResultWithinCacheBudget(state)) checkpoint=state;
                };
                auto evaluated=evaluateFrom(std::move(initial),executable,firstNode,
                                             &pipelineCancel,&pipelineActiveNode,
                                             checkpointTarget,saveCheckpoint);
                return PipelineJobResult{std::move(evaluated),checkpointTarget,
                                         std::move(checkpoint)};
            });
    }
    void inspectPipelineNode(int nodeIndex) {
        if (nodeIndex < 0 || nodeIndex >= int(mods.size()) || busy || indexing || pipelineBusy ||
            inspectorBusy || inspectorJob.valid())
            return;
        std::vector<Modifier> executable;
        executable.reserve(size_t(nodeIndex) + 1);
        for (int i = 0; i <= nodeIndex; ++i)
            executable.push_back(static_cast<const Modifier &>(mods[size_t(i)]));
        Dataset input = source;
        inspectorNode = nodeIndex;
        inspectorNodeId = mods[size_t(nodeIndex)].id;
        inspectorGeneration = pipelineGeneration;
        inspectorCancel = false;
        inspectorActiveNode = 0;
        inspectorBusy = true;
        inspectorResult.reset();
        status = "Evaluating selected pipeline node for data inspection...";
        inspectorJob = std::async(std::launch::async,
            [this, input = std::move(input), executable = std::move(executable)]() mutable {
                return evaluate(std::move(input), executable, &inspectorCancel, &inspectorActiveNode);
            });
    }
    void update(size_t dirtyFrom = SIZE_MAX, bool preserveColorRanges = false) {
        const size_t first = dirtyFrom == SIZE_MAX
            ? (mods.empty() ? 0 : std::min(modifierGraph.selected, mods.size() - 1))
            : dirtyFrom;
        if (pipelineCheckpoint && first < pipelineCheckpointNode) {
            pipelineCheckpoint.reset();
            pipelineCheckpointNode=SIZE_MAX;
        }
        if (!preserveColorRanges) {
            for (size_t i = first; i < mods.size(); ++i)
                if (mods[i].op == Op::ColorCoding && mods[i].colorAllFramesRange && i > first)
                    mods[i].colorAllFramesRange = false;
        }
        modifierGraph.markDirtyFrom(first);
        ++pipelineGeneration;
        if (inspectorBusy) inspectorCancel = true;
        inspectorResult.reset();
        inspectorNode = -1;
        inspectorNodeId.clear();
        staleResult = true;
        if (pipelineBusy) {
            pipelineCancel = true;
            status = "Cancelling outdated pipeline evaluation...";
            return;
        }
        if (inspectorBusy) {
            pipelineDeferredForInspector = true;
            status = "Waiting for cancelled node inspection before updating the pipeline...";
            return;
        }
        launchPipeline();
    }
    ModifierNode makeNode(const Modifier &modifier) {
        ModifierNode node(modifier);
        node.id = std::to_string(nextModifierId++);
        node.displayName = opName(modifier.op);
        node.category = modifier.op == Op::ColorCoding || modifier.op == Op::ColorType || modifier.op == Op::AssignColor
                            ? "Coloring"
                            : modifier.op == Op::SelectType || modifier.op == Op::SelectIndex ||
                                      modifier.op == Op::SelectRange || modifier.op == Op::Invert ||
                                      modifier.op == Op::Clear || modifier.op == Op::ExpandSelection ||
                                      modifier.op == Op::SelectOverlapping || modifier.op == Op::ExpressionSelect ||
                                      modifier.op == Op::ManualSelection
                                  ? "Selection"
                                  : modifier.op == Op::CoordinationAnalysis || modifier.op == Op::ClusterAnalysis ||
                                            modifier.op == Op::RadialDistribution || modifier.op == Op::Histogram ||
                                            modifier.op == Op::ReduceProperty || modifier.op == Op::CommonNeighborAnalysis ||
                                            modifier.op == Op::BondLengthDistribution || modifier.op == Op::BondAngleDistribution ||
                                            modifier.op == Op::ScatterPlot
                                        ? "Analysis"
                                        : "Modification";
        return node;
    }
    void checkpoint() {
        if (!editCheckpoint.shouldRecord(ImGui::IsMouseDown(ImGuiMouseButton_Left)))
            return;
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
        if (op == Op::Scale) m.value = 1;
        if (op == Op::Replicate) m.type = 2;
        if (op == Op::Rotate) m.value = 90;
        if (op == Op::SelectRange) {
            m.value = result.data.lo.z; m.upper = result.data.hi.z;
        }
        if (op == Op::EditCell) {
            m.editedCell = result.data.cell;
            m.editedOrigin = result.data.origin;
            m.editedPbc = result.data.pbc;
        }
        if (op == Op::ColorType) { colorCoding = true; colorAxis = 0; colorAutoRange = true; }
        if (op == Op::ColorCoding) { colorCoding = true; colorAutoRange = true; }
        if (op == Op::CommonNeighborAnalysis || op == Op::CreateBonds ||
            op == Op::CoordinationAnalysis || op == Op::ClusterAnalysis || op == Op::RadialDistribution ||
            op == Op::ExpandSelection || op == Op::SelectOverlapping) m.value = cutoff;
        if (op == Op::Histogram) { m.type = 64; m.property = "Position.X"; }
        if (op == Op::BondLengthDistribution) m.type = 64;
        if (op == Op::BondAngleDistribution) m.type = 90;
        if (op == Op::ReduceProperty) m.property = "Position.X";
        if (op == Op::SelectOverlapping) m.property = "Radius";
        if (op == Op::ExpandSelection) m.type = 1;
        if (op == Op::ExpressionSelect) m.property = "Position.X > 0";
        if (op == Op::ComputeProperty) m.property = "x*x + y*y + z*z";
        modifierGraph.insert(makeNode(m));
        update();
        status = std::string("Added ") + opName(op);
    }
    bool colorPropertyCombo(const char *label, std::string &property,
                            bool includeVectorComponents = false) {
        const auto choices = pipelineInputPropertyChoices(
            source, mods, std::min(modifierGraph.selected, mods.size()), includeVectorComponents);
        bool changed = false;
        if (ImGui::BeginCombo(label, property.c_str())) {
            for (const auto &name : choices) {
                bool selected = property == name;
                if (ImGui::Selectable(name.c_str(), selected)) {
                    property = name;
                    changed = true;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        return changed;
    }
    void history(bool forward) {
        if (!applyModifierHistory(mods, undo, redo, forward)) return;
        modifierGraph.selected = mods.empty() ? 0 : std::min(modifierGraph.selected, mods.size() - 1);
        pipelineCheckpoint.reset();
        pipelineCheckpointNode=SIZE_MAX;
        update();
    }
    void load(const std::filesystem::path &p, int frame = 0) {
        if (busy || p.empty())
            return;
        if (inspectorBusy) {
            inspectorCancel = true;
            deferredLoadPath = p;
            deferredLoadFrame = frame;
            inspectorResult.reset();
            inspectorNode = -1;
            inspectorNodeId.clear();
            status = "Waiting for node inspection to stop before loading data...";
            return;
        }
        deferredLoadPath.clear();
        pipelineDeferredForInspector = false;
        pipelineCheckpoint.reset();
        pipelineCheckpointNode=SIZE_MAX;
        inspectorResult.reset();
        inspectorNode = -1;
        inspectorNodeId.clear();
        staleBeforeLoad = staleResult;
        staleResult = true;
        indexing = p != path || frames.empty();
        if (indexing) pendingFrame = -1;
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
                    known = io::index(p, &progress, &cancel);
                if (frame < 0 || frame >= int(known.size()))
                    throw std::runtime_error("Frame out of range");
                auto d = io::read(p, known[frame], b, &progress, &cancel);
                known[frame].count = d.sourceCount;
                return Loaded{std::move(d), std::move(known), p, frame};
            });
    }
    void computeColorRangeAllFrames(size_t nodeIndex) {
        if (colorRangeRunning || busy || indexing || pipelineBusy || nodeIndex >= mods.size())
            return;
        if (frames.empty() || path.empty()) {
            error = "Load a trajectory before computing an all-frame color range";
            return;
        }
        const auto &target = mods[nodeIndex];
        if (target.op != Op::ColorCoding) return;
        constexpr uint64_t maxExactAtomsPerFrame = 2000000;
        for (const auto &frame : frames) {
            if (frame.count > maxExactAtomsPerFrame) {
                error = "Exact all-frame color range is limited to 2 million atoms per frame";
                return;
            }
        }
        std::vector<Frame> frameSnapshot = frames;
        std::vector<Modifier> upstream;
        upstream.reserve(nodeIndex);
        for (size_t i = 0; i < nodeIndex; ++i)
            upstream.push_back(static_cast<const Modifier &>(mods[i]));
        const auto inputPath = path;
        const auto property = target.property;
        colorRangeNodeId = target.id;
        colorRangePath = inputPath;
        colorRangePipelineGeneration = pipelineGeneration;
        colorRangeCancel = false;
        colorRangeProgress = 0;
        colorRangeRunning = true;
        error.clear();
        status = "Computing exact color range across trajectory frames...";
        colorRangeJob = std::async(std::launch::async,
            [this, inputPath, frameSnapshot = std::move(frameSnapshot), upstream = std::move(upstream), property,
             maxExactAtomsPerFrame]() mutable {
                auto readFrame = [&](size_t index) {
                    if (colorRangeCancel) throw std::runtime_error("Cancelled");
                    const auto &frame = frameSnapshot.at(index);
                    // Static formats have one indexed frame whose particle count is unknown.
                    // Read just over the exact-work limit so the importer can report sampling
                    // or an over-limit source without silently calculating from a preview.
                    auto data = io::read(inputPath, frame, maxExactAtomsPerFrame + 1,
                                         nullptr, &colorRangeCancel);
                    if (data.sampled() || data.sourceCount > maxExactAtomsPerFrame ||
                        data.atoms.size() > maxExactAtomsPerFrame)
                        throw std::runtime_error(
                            "Exact all-frame color range is limited to 2 million atoms per frame; "
                            "the input was sampled or exceeds the limit");
                    return data;
                };
                return colorRangeAcrossFrames(frameSnapshot.size(), readFrame, upstream, property,
                                              &colorRangeCancel, &colorRangeProgress);
            });
    }
    void poll() {
        if (inspectorBusy && inspectorJob.valid() &&
            inspectorJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            inspectorBusy = false;
            try {
                auto inspected = inspectorJob.get();
                if (inspectorGeneration == pipelineGeneration && inspectorNode >= 0 &&
                    inspectorNode < int(mods.size()) &&
                    mods[size_t(inspectorNode)].id == inspectorNodeId) {
                    inspectorResult = std::move(inspected);
                    status = "Selected pipeline node output is ready for inspection";
                } else {
                    inspectorResult.reset();
                    inspectorNode = -1;
                    inspectorNodeId.clear();
                }
            } catch (const std::exception &e) {
                inspectorResult.reset();
                inspectorNode = -1;
                inspectorNodeId.clear();
                if (std::string(e.what()) != "Cancelled") error = e.what();
            }
            if (!inspectorBusy && !deferredLoadPath.empty()) {
                auto queuedPath = std::move(deferredLoadPath);
                const int queuedFrame = deferredLoadFrame;
                deferredLoadFrame = 0;
                load(queuedPath, queuedFrame);
            } else if (!inspectorBusy && pipelineDeferredForInspector) {
                pipelineDeferredForInspector = false;
                launchPipeline();
            }
        }
        if (pipelineBusy && pipelineJob.valid() &&
            pipelineJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            const uint64_t finishedGeneration = pipelineJobGeneration;
            pipelineBusy = false;
            if (finishedGeneration == pipelineGeneration) {
                try {
                    auto completed = pipelineJob.get();
                    publishPipeline(std::move(completed.result));
                    if (completed.checkpoint) {
                        pipelineCheckpoint=std::make_shared<PipelineResult>(std::move(*completed.checkpoint));
                        pipelineCheckpointNode=completed.checkpointNode;
                    }
                    for (auto &node : mods) {
                        node.running = false;
                        node.error.clear();
                    }
                    if (error.empty()) status = "Pipeline ready";
                } catch (const ModifierExecutionError &e) {
                    error = e.what();
                    for (auto &node : mods) node.running = false;
                    if (e.nodeIndex < mods.size() && e.nodeIndex < pipelineJobNodeIds.size() &&
                        mods[e.nodeIndex].id == pipelineJobNodeIds[e.nodeIndex])
                        mods[e.nodeIndex].error = error;
                    status = "Pipeline evaluation failed";
                } catch (const std::exception &e) {
                    error = e.what();
                    for (auto &node : mods) node.running = false;
                    status = "Pipeline evaluation failed";
                }
            } else {
                try { (void)pipelineJob.get(); } catch (...) {}
            }
            if (finishedGeneration != pipelineGeneration)
                launchPipeline();
        }
        if (exporting && exportJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            exporting = false;
            try {
                status = exportJob.get();
            } catch (const std::exception &e) {
                error = e.what();
            }
        }
        if (dxaRunning && dxaJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            dxaRunning=false;
            try { dxa= dxaJob.get(); status="DXA prepass completed"; } catch(const std::exception&e){error=e.what();}
        }
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
        if (colorRangeRunning && colorRangeJob.valid() &&
            colorRangeJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            colorRangeRunning = false;
            try {
                auto range = colorRangeJob.get();
                auto node = std::find_if(mods.begin(), mods.end(), [&](const ModifierNode &item) {
                    return item.id == colorRangeNodeId;
                });
                if (node == mods.end() || path != colorRangePath ||
                    pipelineGeneration != colorRangePipelineGeneration) {
                    status = "Dataset or pipeline changed; all-frame color range discarded";
                } else {
                    checkpoint();
                    node->colorMin = float(range.first);
                    node->colorMax = float(range.second);
                    node->colorAutoRange = true;
                    node->colorAllFramesRange = true;
                    const size_t index = size_t(node - mods.begin());
                    update(index, true);
                    status = "Color range computed across all frames";
                }
            } catch (const std::exception &e) {
                if (std::string(e.what()) == "Cancelled") status = "All-frame color range cancelled";
                else { error = e.what(); status = "All-frame color range failed"; }
            }
        }
        if (busy && job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            busy = false;
            try {
                auto l = job.get();
                readerName = io::info(io::detect(l.path)).name;
                bool changed = path != l.path;
                if (changed) {
                    appearanceMemory.clear();
                    customRadiusMemory.clear();
                    appearanceNames.clear();
                    gpu.resetStyles(0);
                }
                source = std::move(l.data);
                frames = std::move(l.frames);
                path = l.path;
                current = l.frame;
                if (changed) {
                    appearanceType = 0;
                    exportOptions.scalarProperties.clear();
                    exportOptions.vectorProperties.clear();
                    mods.clear();
                    modifierGraph.selected = 0;
                    undo.clear();
                    redo.clear();
                    for (auto &c : cameras)
                        c.zoom = 1;
                }
                update(SIZE_MAX, true);
                status = "Loaded " + number(source.sourceCount) + " atoms";
            } catch (const std::exception &e) {
                error = e.what();
                playing = false;
                staleResult = staleBeforeLoad;
                status = "Load stopped";
            }
        }
        if (!busy && pendingFrame >= 0) {
            int requested = pendingFrame; pendingFrame = -1;
            if (requested != current) load(path, requested);
        }
        if (!dropped.empty()) {
            load(dropped);
            dropped.clear();
        }
    }
    void open() {
        load(dialog(window, false,
                    L"Atom "
                    L"structures\0*.xyz;*.extxyz;*.vasp;*.poscar;*.contcar;POSCAR;CONTCAR;*.cif;*."
                    L"data;*.lmp;*.dump;*.lammpstrj;*.pdb;*.ent;*.gro\0All files\0*.*\0",
                    L"xyz"));
    }
    void exportImage() {
        auto p = dialog(window, true, L"PNG image\0*.png\0", L"png");
        if (p.empty())
            return;
        Target t;
        gpu.target(t, exportW, exportH);
            gpu.draw(t, result.data, cameras[active], radius, particleShape, renderMode, colorAxis, colorGradient, colorReverse?colorMax:colorMin, colorReverse?colorMin:colorMax, colorCoding, colorDiscrete, colorSelectedOnly, bg, particles, cell);
        ColorLegendOptions legend{colorCoding && colorLegend, colorRangeProperty, colorGradient,
                                  colorMin, colorMax, colorReverse, colorDiscrete};
        gpu.png(t, p, legend);
        status = "Rendered " + utf8(p.filename().wstring());
    }
    void fixed(const char *name, float x, float y, float w, float h) {
        ImGui::SetNextWindowPos({x, y});
        ImGui::SetNextWindowSize({std::max(w, 1.f), std::max(h, 1.f)});
        ImGui::Begin(name, nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoSavedSettings | ((std::string(name) == "Title" || std::string(name) == "Status") ? ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse : 0));
    }
    void recordUiTestItem(const std::string &name,const char *explicitLabel=nullptr) {
        if (!captureUiTestItems) return;
        const ImGuiID id=explicitLabel ? ImGui::GetID(explicitLabel) : ImGui::GetItemID();
        uiTestItems[name]={id,ImGui::GetItemRectMin(),ImGui::GetItemRectMax(),
                           ImGui::IsItemHovered(),ImGui::IsItemClicked()};
    }
    void control(const char *id, int kind, const char *tip) {
        ImGui::PushStyleColor(ImGuiCol_Button, {0,0,0,0});
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,0);
        if (ImGui::Button(id, {U(42),U(30)})) {
            if (kind == 0) ShowWindow(window, SW_MINIMIZE);
            if (kind == 1) ShowWindow(window, IsZoomed(window) ? SW_RESTORE : SW_MAXIMIZE);
            if (kind == 2) desktop::hide(window);
            if (kind == 3) PostQuitMessage(0);
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        auto p = ImGui::GetItemRectMin(); auto *d = ImGui::GetWindowDrawList();
        ImVec2 c{p.x+U(21),p.y+U(15)};
        auto color = ImGui::GetColorU32(kind == 3 ? ImVec4{.94f,.40f,.40f,1} : ImGui::GetStyleColorVec4(ImGuiCol_Text));
        if (kind == 0) d->AddLine({c.x-6,c.y+3},{c.x+6,c.y+3},color,1.5f);
        if (kind == 1) {
            if (IsZoomed(window)) d->AddRect({c.x-3,c.y-7},{c.x+7,c.y+3},color);
            d->AddRect({c.x-6,c.y-4},{c.x+4,c.y+6},color);
        }
        if (kind == 2) {
            d->AddLine({c.x-5,c.y-5},{c.x+5,c.y+5},color,1.5f);
            d->AddLine({c.x+5,c.y-5},{c.x-5,c.y+5},color,1.5f);
        }
        if (kind == 3) {
            d->PathArcTo(c,7,-.9f,4.04f,24); d->PathStroke(color,0,1.7f);
            d->AddLine({c.x,c.y-9},{c.x,c.y},color,1.7f);
        }
    }
    void top(float w) {
        fixed("Title", 0, 0, w, U(42));
        ImGui::SetCursorPosY(U(4));
        ImGui::Image((ImTextureID)(intptr_t)logo.Get(), {U(32),U(32)});
        ImGui::SameLine(); ImGui::SetCursorPosY(U(9));
        ImGui::TextUnformatted("AtomX");
        ImGui::SameLine(); ImGui::TextDisabled(" / Atomic visualization");
        ImGui::SameLine(w-U(210)); ImGui::SetCursorPosY(U(5));
        control("##minimize",0,"Minimize to taskbar"); ImGui::SameLine();
        control("##maximize",1,"Maximize / restore"); ImGui::SameLine();
        control("##tray",2,"Close to system tray (keep running)"); ImGui::SameLine();
        control("##exit",3,"Power off: exit AtomX completely");
        ImGui::End();
        fixed("Top", 0, U(42), w, U(48));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {U(11), U(7)});
        if (ImGui::Button("Open##toolbar"))
            open();
        ImGui::SameLine();
        if (ImGui::Button("Import##toolbar"))
            open();
        ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine();
        bool undoPressed=ImGui::Button("Undo##toolbar");
        recordUiTestItem("toolbar.undo","Undo##toolbar");
        if (undoPressed)
            history(false);
        ImGui::SameLine();
        bool redoPressed=ImGui::Button("Redo##toolbar");
        recordUiTestItem("toolbar.redo","Redo##toolbar");
        if (redoPressed)
            history(true);
        ImGui::SameLine();
        if (ImGui::Button("Select##toolbar")) active = active;
        ImGui::SameLine();
        if (ImGui::Button("Orbit##toolbar")) cameras[active].mode = 7;
        ImGui::SameLine();
        if (ImGui::Button("Rotate##toolbar")) cameras[active].yaw += .35f;
        ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine();
        if (ImGui::Button("Snapshot##toolbar"))
            exportImage();
        ImGui::SameLine();
        if (ImGui::Button(quad ? "Single view##toolbar" : "Four views##toolbar"))
            quad = !quad;
        ImGui::SameLine();
        if (ImGui::Button("Fit all##toolbar"))
            for (int i=0;i<4;++i) fitCamera(i,false);
        ImGui::SameLine();
        if (ImGui::Button("Modifiers##toolbar"))
            showCatalog = true;
        ImGui::SameLine();
        if (ImGui::Button("Render##toolbar"))
            exportImage();
        ImGui::SameLine();
        if (ImGui::Button("Settings##toolbar")) showSettings = true;
        ImGui::SameLine();
        if (ImGui::Button("Workspace##toolbar")) showWorkspace = !showWorkspace;
        ImGui::PopStyleVar();
        ImGui::End();
    }
    float leftWidth() const { return showWorkspace ? U(220) : 0.f; }
    float rightWidth() const { return U(preferences.size >= 19 ? 400.f : 370.f); }
    void left(float h) {
        fixed("Workspace", 0, U(90), leftWidth(), h - U(120));
        heading("WORKSPACE");
        ImGui::TextColored(accent, "ATOMIC STRUCTURES");
        ImGui::Spacing();
        ImGui::Selectable(path.empty() ? "  Cu-Ni / FCC specimen"
                                       : ("  " + utf8(path.filename().wstring())).c_str(),
                          true);
        ImGui::TextDisabled("    %s", path.empty() ? "Generated dataset" : "XYZ trajectory");
        ImGui::Spacing();
        if (ImGui::Button("+ Import dataset", {-1, U(32)}))
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
        if (ImGui::Button("Slice specimen", {-1, U(30)}))
            add(Op::Slice);
        if (ImGui::Button("Select particle type", {-1, U(30)}))
            add(Op::SelectType);
        if (ImGui::Button("Reset pipeline", {-1, U(30)})) {
            checkpoint();
            mods.clear();
            modifierGraph.selected = 0;
            pipelineCheckpoint.reset();
            pipelineCheckpointNode=SIZE_MAX;
            update();
        }
        if (ImGui::Button("Load demo crystal", {-1, U(30)}) && !busy) {
            source = crystal(24);
            path.clear();
            frames.clear();
            current = 0;
            mods.clear();
            modifierGraph.selected = 0;
            pipelineCheckpoint.reset();
            pipelineCheckpointNode=SIZE_MAX;
            undo.clear();
            redo.clear();
            update();
        }
        ImGui::End();
    }
    const char *views = "Top\0Bottom\0Front\0Back\0Left\0Right\0Ortho\0Perspective\0";
    void viewport(int i, float w, float h) {
        ImGui::PushID(i);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{U(4),U(4)});
        ImGui::PushStyleColor(ImGuiCol_ChildBg,{0,0,0,1});
        ImGui::BeginChild("view", {w, h}, ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        auto &cam = cameras[i];
        ImGui::PushStyleColor(ImGuiCol_Text,{.95f,.97f,1,1});
        ImGui::PushStyleColor(ImGuiCol_TextDisabled,{.73f,.78f,.85f,1});
        ImGui::PushStyleColor(ImGuiCol_FrameBg,{.035f,.045f,.06f,1});
        ImGui::PushStyleColor(ImGuiCol_Button,{.10f,.13f,.17f,1});
        ImGui::PushStyleColor(ImGuiCol_PopupBg,{.055f,.07f,.095f,1});
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered,{.16f,.26f,.39f,1});
        ImGui::PushStyleColor(ImGuiCol_Header,{.12f,.21f,.34f,1});
        ImGui::SetNextItemWidth(U(140));
        ImGui::Combo("##camera", &cam.mode, views);
        ImGui::SameLine();
        ImGui::TextDisabled(i == active ? "ACTIVE" : "");
        ImGui::SameLine();
        if (ImGui::SmallButton("Fit")) {
            fitCamera(i,false);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(selectedCount==0);
        if (ImGui::SmallButton("Fit selected")) fitCamera(i,true);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && selectedCount==0)
            ImGui::SetTooltip("Select particles in the data table first");
        ImGui::PopStyleColor(7);
        auto p = ImGui::GetCursorScreenPos();
        auto avail = ImGui::GetContentRegionAvail();
        gpu.target(targets[i], int(avail.x), int(avail.y));
            gpu.draw(targets[i], result.data, cam, radius, particleShape, renderMode, colorAxis, colorGradient, colorReverse?colorMax:colorMin, colorReverse?colorMin:colorMax, colorCoding, colorDiscrete, colorSelectedOnly, bg, particles, cell);
        ImGui::Image((ImTextureID)(intptr_t)targets[i].srv.Get(), avail);
        if (ImGui::IsItemHovered()) {
            if (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1) || ImGui::GetIO().MouseWheel)
                active = i;
            if (ImGui::IsMouseDragging(0) && viewportTool == 2) {
                cam.yaw -= ImGui::GetIO().MouseDelta.x * .008f;
                cam.pitch =
                    std::clamp(cam.pitch + ImGui::GetIO().MouseDelta.y * .008f, -1.55f, 1.55f);
                if (cam.mode < 6)
                    cam.mode = 6;
            }
            if ((ImGui::IsMouseDragging(1) || ImGui::IsMouseDragging(2)) || (ImGui::IsMouseDragging(0) && viewportTool == 1)) {
                cam.panX += ImGui::GetIO().MouseDelta.x / std::max(avail.x, 1.f) * cam.zoom;
                cam.panY -= ImGui::GetIO().MouseDelta.y / std::max(avail.y, 1.f) * cam.zoom;
            }
            if (viewportTool == 0 && ImGui::GetIO().MouseWheel == 0 && ImGui::IsMouseDragging(0))
                cam.zoom = std::clamp(cam.zoom * powf(.985f, ImGui::GetIO().MouseDelta.y), .01f, 50.f);
            cam.zoom = std::clamp(cam.zoom * powf(.85f, ImGui::GetIO().MouseWheel), .01f, 50.f);
        }
        auto *draw = ImGui::GetWindowDrawList();
        if (cell) {
            using namespace DirectX;
            auto m = gpu.matrix(result.data, cam, avail.x / std::max(avail.y, 1.f), true, radius);
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
                    x += result.data.origin.x;
                    y += result.data.origin.y;
                    z += result.data.origin.z;
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
                    if (!(j & k) && valid[j] && valid[j | k] &&
                        (cellDimension == 1 || (!(j & 4) && !((j | k) & 4)))) {
                        auto c = ImGui::ColorConvertFloat4ToU32({cellColor[0],cellColor[1],cellColor[2],cellColor[3]*cellGlow});
                        auto hi = ImGui::ColorConvertFloat4ToU32({cellColor[0],cellColor[1],cellColor[2],cellColor[3]});
                        draw->AddLine(corners[j], corners[j | k], c, cellWidth+4.f);
                        if (!cellDashed) draw->AddLine(corners[j], corners[j | k], hi, cellWidth);
                        else { auto a=corners[j], b=corners[j|k]; for(int q=0;q<8;q+=2){float t0=q/8.f,t1=(q+1)/8.f;draw->AddLine({a.x+(b.x-a.x)*t0,a.y+(b.y-a.y)*t0},{a.x+(b.x-a.x)*t1,a.y+(b.y-a.y)*t1},hi,cellWidth);} }
                    }
            if (cellLabels) { draw->AddText(corners[0], ImGui::ColorConvertFloat4ToU32({cellColor[0],cellColor[1],cellColor[2],1}), "O"); draw->AddText(corners[1], ImGui::ColorConvertFloat4ToU32({cellColor[0],cellColor[1],cellColor[2],1}), "A"); draw->AddText(corners[2], ImGui::ColorConvertFloat4ToU32({cellColor[0],cellColor[1],cellColor[2],1}), "B"); draw->AddText(corners[4], ImGui::ColorConvertFloat4ToU32({cellColor[0],cellColor[1],cellColor[2],1}), "C"); }
            draw->PopClipRect();
        }
        if (colorCoding && colorLegend && avail.x >= U(220) && avail.y >= U(110)) {
            const float sx = U(174), sy = U(64), pad = U(8);
            ImVec2 a{p.x + avail.x - sx - U(12), p.y + U(12)};
            ImVec2 b{a.x + sx, a.y + sy};
            draw->AddRectFilled(a,b,IM_COL32(15,19,25,238),U(4));
            draw->AddRect(a,b,IM_COL32(104,119,138,255),U(4));
            std::string title = colorRangeProperty;
            if (title.size() > 24) title = title.substr(0,21) + "...";
            draw->AddText({a.x+pad,a.y+U(5)},IM_COL32(238,243,250,255),title.c_str());
            const float barX=a.x+pad, barY=a.y+U(25), barW=sx-pad*2, barH=U(12);
            const int segments=colorDiscrete?12:96;
            for (int segment=0; segment<segments; ++segment) {
                float u=segments>1?float(segment)/float(segments-1):0;
                if (colorReverse) u=1-u;
                if (colorDiscrete) u=std::min(std::floor(u*12),11.f)/11.f;
                auto c=sampleColorGradient(colorGradient,u);
                auto packed=ImGui::ColorConvertFloat4ToU32({c[0],c[1],c[2],1});
                const float x0=barX+barW*segment/segments;
                const float x1=barX+barW*(segment+1)/segments+.5f;
                draw->AddRectFilled({x0,barY},{x1,barY+barH},packed);
            }
            char low[48]{}, high[48]{};
            snprintf(low,sizeof(low),"%.5g",colorMin);
            snprintf(high,sizeof(high),"%.5g",colorMax);
            draw->AddText({barX,a.y+U(42)},IM_COL32(218,226,237,255),low);
            auto highSize=ImGui::CalcTextSize(high);
            draw->AddText({barX+barW-highSize.x,a.y+U(42)},IM_COL32(218,226,237,255),high);
        }
        if (ImGui::IsItemHovered()) {
            const char* toolHint = viewportTool == 0 ? "Zoom" : viewportTool == 1 ? "Pan" : viewportTool == 2 ? "Orbit" : "FOV";
            draw->AddText({p.x + U(12), p.y + avail.y - U(25)}, IM_COL32(190, 202, 215, 255),
                          (std::string("Tool: ") + toolHint + "   Wheel zoom").c_str());
        }
        if (i == active)
            draw->AddRect(p, {p.x + avail.x, p.y + avail.y}, IM_COL32(95, 180, 255, 255));
        ImGui::EndChild();
        ImGui::PopStyleColor(); ImGui::PopStyleVar();
        ImGui::PopID();
    }
    void seekFrame(int frame) {
        if (frames.empty()) return;
        playing = false;
        pendingFrame = std::clamp(frame, 0, int(frames.size())-1);
    }
    bool transport(const char *id, int kind, const char *tip) {
        bool pressed = ImGui::Button(id, {U(34),U(30)});
        auto p = ImGui::GetItemRectMin();
        auto *d = ImGui::GetWindowDrawList();
        auto color = ImGui::GetColorU32(ImGuiCol_Text);
        ImVec2 c{p.x+U(17),p.y+U(15)};
        if (kind == 2 && playing) {
            d->AddRectFilled({c.x-U(5),c.y-U(6)},{c.x-U(2),c.y+U(6)},color);
            d->AddRectFilled({c.x+U(2),c.y-U(6)},{c.x+U(5),c.y+U(6)},color);
        } else {
            float direction = kind < 2 ? -1.f : 1.f;
            d->AddTriangleFilled({c.x+direction*U(5),c.y},{c.x-direction*U(4),c.y-U(6)},
                                 {c.x-direction*U(4),c.y+U(6)},color);
            if (kind == 0 || kind == 4)
                d->AddLine({c.x+direction*U(8),c.y-U(7)}, {c.x+direction*U(8),c.y+U(7)},color,U(2));
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s",tip);
        return pressed;
    }
    void timeline() {
        ImGui::BeginChild("Trajectory timeline", {-1,U(128)}, ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        int last = std::max(0,int(frames.size())-1);
        int selected = pendingFrame >= 0 ? pendingFrame : current;
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(accent, "TRAJECTORY"); ImGui::SameLine();
        ImGui::BeginDisabled(frames.size()<2 || indexing && busy);
        if (transport("##first",0,"First frame (Home)")) seekFrame(0); ImGui::SameLine();
        if (transport("##previous",1,"Previous frame (Left)")) seekFrame(selected-1); ImGui::SameLine();
        if (transport("##play",2,"Play / pause")) playing = !playing; ImGui::SameLine();
        if (transport("##next",3,"Next frame (Right)")) seekFrame(selected+1); ImGui::SameLine();
        if (transport("##last",4,"Last frame (End)")) seekFrame(last);
        ImGui::SameLine(); ImGui::SetNextItemWidth(U(90));
        int edit = selected + 1;
        if (ImGui::InputInt("##frame number", &edit, 0, 0)) seekFrame(std::max(0,edit-1));
        ImGui::EndDisabled();
        ImGui::SameLine(); ImGui::Text("/ %zu", std::max<size_t>(frames.size(), 1));
        if (frames.empty()) ImGui::SetItemTooltip("No trajectory frames are loaded");
        ImGui::SameLine(); ImGui::TextDisabled("  %s", playing ? "Playing" : "Paused");
        ImGui::SameLine();
        auto toolButton = [&](const char* label, int tool, const char* tip) {
            bool selectedTool = viewportTool == tool;
            if (selectedTool) ImGui::PushStyleColor(ImGuiCol_Button, accent);
            bool pressed = ImGui::SmallButton(label);
            if (selectedTool) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            if (pressed) viewportTool = tool;
        };
        toolButton("Zoom", 0, "Zoom active viewport (drag or wheel)"); ImGui::SameLine();
        toolButton("Pan", 1, "Pan active viewport"); ImGui::SameLine();
        toolButton("Orbit", 2, "Orbit active viewport"); ImGui::SameLine();
        toolButton("FOV", 3, "Adjust perspective field of view"); ImGui::SameLine();
        if (ImGui::SmallButton(quad ? "Max" : "Views")) quad = !quad;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", quad ? "Maximize active viewport" : "Show four viewports");
        ImGui::SameLine();
        if (ImGui::SmallButton("Clock")) animationSettings = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Animation settings");
        ImGui::SameLine();
        if (autoKey) ImGui::PushStyleColor(ImGuiCol_Button, accent);
        if (ImGui::SmallButton("Key")) autoKey = !autoKey;
        if (autoKey) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle auto-key mode");
        if (ImGui::GetContentRegionAvail().x > U(220)) {
            ImGui::SameLine();
            ImGui::TextDisabled(frames.size()>1 ? "  Drag ruler to scrub" : "  Single frame");
        }
        auto p = ImGui::GetCursorScreenPos();
        float width = ImGui::GetContentRegionAvail().x, height = U(65);
        ImGui::InvisibleButton("##frame ruler", {width,height}, ImGuiButtonFlags_EnableNav);
        bool hovered = ImGui::IsItemHovered(), focused = ImGui::IsItemFocused();
        auto *d = ImGui::GetWindowDrawList();
        float left = p.x+U(15), right = p.x+width-U(18), baseline = p.y+U(29);
        auto text = ImGui::GetColorU32(ImGuiCol_Text);
        auto tick = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        auto marker = ImGui::GetColorU32(accent);
        d->AddRectFilled({left,p.y+U(21)},{right,p.y+U(38)},IM_COL32(35,42,54,180),U(4));
        d->AddLine({left,baseline},{right,baseline},tick, U(1));
        // Choose 1/2/5 decade steps so labels remain separated at every frame count.
        int64_t major = 1;
        double desired = std::max(1.0, double(last)*U(72)/std::max(1.f,right-left));
        double decade = std::pow(10.,std::floor(std::log10(desired)));
        for (int step : {1,2,5,10}) if (step*decade >= desired) { major=int64_t(step*decade); break; }
        int64_t minor = major >= 5 ? major/5 : 1;
        auto xFor = [&](int frame) { return left+(right-left)*(last ? float(frame)/last : 0.f); };
        for (int64_t frame=0;frame<=last;frame+=minor) {
            float x=xFor(int(frame)); bool labeled=frame%major==0;
            d->AddLine({x,baseline},{x,baseline+U(labeled?12.f:6.f)},tick,U(1));
            if (labeled) {
                auto label=std::to_string(frame+1); auto size=ImGui::CalcTextSize(label.c_str());
                d->AddText({x-size.x*.5f,p.y+U(3)},text,label.c_str());
            }
        }
        float x=xFor(selected);
        d->AddRectFilled({left,p.y+U(21)},{x,p.y+U(38)},IM_COL32(92,145,205,90),U(4));
        d->AddLine({x,p.y},{x,baseline+U(14)},marker,U(2));
        d->AddTriangleFilled({x-U(5),baseline-U(5)},{x+U(5),baseline-U(5)},{x,baseline+U(1)},marker);
        if (last>0 && ImGui::IsItemActive() && ImGui::IsMouseDown(0))
            seekFrame(int(std::lround(std::clamp((ImGui::GetIO().MousePos.x-left)/(right-left),0.f,1.f)*last)));
        if (hovered && last>0) {
            int frame=int(std::lround(std::clamp((ImGui::GetIO().MousePos.x-left)/(right-left),0.f,1.f)*last));
            ImGui::SetTooltip("Frame %d / %d",frame+1,last+1);
        }
        if (focused && last>0) {
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) seekFrame(selected-1);
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) seekFrame(selected+1);
            if (ImGui::IsKeyPressed(ImGuiKey_Home)) seekFrame(0);
            if (ImGui::IsKeyPressed(ImGuiKey_End)) seekFrame(last);
        }
        ImGui::EndChild();
    }
    void center(float w, float h) {
        fixed("Viewport workspace", leftWidth(), U(90), w - leftWidth() - rightWidth(), h - U(120));
        ImGui::Text("%s", path.empty() ? "Cu-Ni specimen"
                                                         : utf8(path.filename().wstring()).c_str());
        ImGui::Separator();
        auto avail = ImGui::GetContentRegionAvail();
        float dataH = showTable ? U(280) : 0;
        float gap = ImGui::GetStyle().ItemSpacing.y;
        const float sceneChrome = U(showTable ? 278.f : 128.f);
        float sceneH = std::max(U(150), avail.y - dataH - sceneChrome - ImGui::GetFrameHeight() - gap*(showTable ? 4 : 3));
        if (quad) {
            float vw = (avail.x - ImGui::GetStyle().ItemSpacing.x) * .5f, vh = (sceneH - gap) * .5f;
            viewport(0, vw, vh);
            ImGui::SameLine();
            viewport(1, vw, vh);
            viewport(2, vw, vh);
            ImGui::SameLine();
            viewport(3, vw, vh);
        } else
            viewport(active, avail.x, sceneH);
        if (ImGui::Button(showTable ? "Hide data inspector" : "Data inspector")) showTable = !showTable;
        ImGui::SameLine();
        ImGui::Text("%s particles  |  %zu selected", number(result.data.atoms.size()).c_str(), selectedCount);
        if (showTable) {
            ImGui::BeginChild("Inspector", {-1, dataH}, ImGuiChildFlags_Borders);
            std::string inspectLabel = "Final pipeline output";
            if (inspectorNode >= 0 && inspectorNode < int(mods.size()))
                inspectLabel = "Node " + std::to_string(inspectorNode + 1) + ": " +
                               mods[size_t(inspectorNode)].displayName;
            if (ImGui::BeginCombo("Inspect output", inspectLabel.c_str())) {
                if (ImGui::Selectable("Final pipeline output", inspectorNode < 0)) {
                    inspectorCancel = true;
                    inspectorNode = -1;
                    inspectorNodeId.clear();
                    inspectorResult.reset();
                }
                for (size_t i = 0; i < mods.size(); ++i) {
                    ImGui::PushID(mods[i].id.c_str());
                    std::string label = "Node " + std::to_string(i + 1) + ": " + mods[i].displayName;
                    ImGui::BeginDisabled(busy || indexing || pipelineBusy || inspectorBusy);
                    const bool selected = inspectorNode == int(i);
                    if (ImGui::Selectable(label.c_str(), selected)) inspectPipelineNode(int(i));
                    ImGui::EndDisabled();
                    if (mods[i].outputs.empty()) {
                        ImGui::Indent();
                        ImGui::TextDisabled("%s", mods[i].enabled ? "Output not evaluated" : "Disabled");
                        ImGui::Unindent();
                    } else {
                        ImGui::Indent();
                        if (mods[i].dirty) ImGui::TextDisabled("Last result (stale):");
                        for (const auto &output : mods[i].outputs) {
                            const char *kind=output.kind==DataObject::Kind::Particles ? "Particles" :
                                output.kind==DataObject::Kind::Bonds ? "Bonds" :
                                output.kind==DataObject::Kind::Cell ? "Simulation cell" :
                                output.kind==DataObject::Kind::Table ? "Data table" :
                                output.kind==DataObject::Kind::GlobalAttributes ? "Global attributes" : "Data object";
                            ImGui::TextDisabled("↳ %s (%s)",output.name.c_str(),kind);
                        }
                        ImGui::Unindent();
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            const PipelineResult *inspected = inspectorNode < 0 ? &result :
                inspectorResult ? &*inspectorResult : nullptr;
            if (inspectorBusy && inspectorNode >= 0) {
                ImGui::TextDisabled("Evaluating the selected node output... this may temporarily use additional memory.");
                const size_t stageCount = size_t(std::max(inspectorNode + 1, 1));
                const size_t activeStage = std::min(inspectorActiveNode.load(), stageCount);
                const float inspectionProgress = float(activeStage) / float(stageCount);
                char overlay[48]{};
                snprintf(overlay, sizeof(overlay), "%zu / %zu stages", activeStage, stageCount);
                ImGui::ProgressBar(inspectionProgress, ImVec2(-1, 0), overlay);
                if (ImGui::Button("Cancel node inspection")) inspectorCancel = true;
            } else if (inspectorBusy) {
                ImGui::TextDisabled("Cancelling the previous node inspection...");
            } else if (!inspected) {
                ImGui::TextDisabled("Node output is unavailable. Select it again after the pipeline is ready.");
            }
            if (inspected && (!inspectorBusy || inspectorNode < 0) && ImGui::BeginTabBar("Data")) {
                if (ImGui::BeginTabItem("Particles")) {
                    const size_t inspectedSelected = std::count(inspected->selected.begin(),
                                                                 inspected->selected.end(), uint8_t(1));
                    ImGui::TextDisabled("%s rows  |  %zu selected%s",
                                        number(inspected->data.atoms.size()).c_str(), inspectedSelected,
                                        inspected->data.sampled() ? "  |  sampled preview" : "");
                    struct PropertyColumn { std::string name; int component; bool vector; bool assignedColor=false; };
                    std::vector<PropertyColumn> properties;
                    std::vector<std::string> scalarNames, vectorNames;
                    for (const auto &[name, values] : inspected->data.scalarProperties)
                        if (values.size() == inspected->data.atoms.size()) scalarNames.push_back(name);
                    for (const auto &[name, values] : inspected->data.vectorProperties)
                        if (values.size() == inspected->data.atoms.size()) vectorNames.push_back(name);
                    std::sort(scalarNames.begin(), scalarNames.end());
                    std::sort(vectorNames.begin(), vectorNames.end());
                    for (const auto &name : scalarNames) properties.push_back({name, -1, false});
                    for (const auto &name : vectorNames)
                        for (int component = 0; component < 3; ++component)
                            properties.push_back({name, component, true});
                    if (inspected->data.particleColors.size()==inspected->data.atoms.size())
                        for (int component=0;component<3;++component)
                            properties.push_back({"Color",component,true,true});
                    if (ImGui::BeginTable("Atoms", int(5 + properties.size()),
                                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                              ImGuiTableFlags_BordersInnerV,
                                          {-1, 0})) {
                        for (auto name : {"Index", "Type", "Position X", "Position Y", "Position Z"})
                            ImGui::TableSetupColumn(name, ImGuiTableColumnFlags_WidthFixed, U(74));
                        constexpr const char *components[] = {"X", "Y", "Z"};
                        for (const auto &column : properties) {
                            std::string label = column.name;
                            if (column.vector) label += "." + std::string(components[column.component]);
                            ImGui::TableSetupColumn(label.c_str(), ImGuiTableColumnFlags_WidthFixed, U(105));
                        }
                        ImGui::TableHeadersRow();
                        ImGuiListClipper clip;
                        clip.Begin(int(inspected->data.atoms.size()));
                        while (clip.Step())
                            for (int j = clip.DisplayStart; j < clip.DisplayEnd; ++j) {
                                auto a = inspected->data.atoms[j];
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                if (inspectorNode < 0 && ImGui::Selectable(std::to_string(j).c_str(),
                                                      inspected->selected[j] != 0,
                                                      ImGuiSelectableFlags_SpanAllColumns)) {
                                    checkpoint();
                                    size_t nodeIndex = mods.size();
                                    if (!mods.empty() && mods.back().enabled && mods.back().op == Op::ManualSelection)
                                        nodeIndex = mods.size()-1;
                                    if (nodeIndex == mods.size()) {
                                        Modifier manual{Op::ManualSelection};
                                        if (ImGui::GetIO().KeyCtrl)
                                            for (size_t k=0;k<result.selected.size();++k)
                                                if (result.selected[k]) manual.manualSelection.push_back(uint32_t(k));
                                        modifierGraph.insert(makeNode(manual));
                                        nodeIndex = mods.size()-1;
                                    }
                                    auto &indices = mods[nodeIndex].manualSelection;
                                    if (!ImGui::GetIO().KeyCtrl) indices.assign(1,uint32_t(j));
                                    else {
                                        auto found=std::lower_bound(indices.begin(),indices.end(),uint32_t(j));
                                        if (found!=indices.end() && *found==uint32_t(j)) indices.erase(found);
                                        else indices.insert(found,uint32_t(j));
                                    }
                                    modifierGraph.selected=nodeIndex;
                                    update(nodeIndex);
                                } else if (inspectorNode >= 0) ImGui::Text("%d", j);
                                ImGui::TableNextColumn();
                                ImGui::TextUnformatted(inspected->data.species[a.type].c_str());
                                ImGui::TableNextColumn();
                                ImGui::Text("%.5f", a.x);
                                ImGui::TableNextColumn();
                                ImGui::Text("%.5f", a.y);
                                ImGui::TableNextColumn();
                                ImGui::Text("%.5f", a.z);
                                for (const auto &column : properties) {
                                    ImGui::TableNextColumn();
                                    if (column.assignedColor) {
                                        const auto &color=inspected->data.particleColors[size_t(j)];
                                        const float component=column.component==0?color.x:column.component==1?color.y:color.z;
                                        if (component<0) ImGui::TextDisabled("type"); else ImGui::Text("%.4f",component);
                                    } else if (!column.vector) {
                                        const auto &values = inspected->data.scalarProperties.at(column.name);
                                        ImGui::Text("%.6g", values[j]);
                                    } else {
                                        const auto &v = inspected->data.vectorProperties.at(column.name)[j];
                                        ImGui::Text("%.6g", column.component == 0 ? v.x : column.component == 1 ? v.y : v.z);
                                    }
                                }
                            }
                        ImGui::EndTable();
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Simulation cell")) {
                    ImGui::TextDisabled("Cell vectors (a, b, c)");
                    for (int row = 0; row < 3; row++)
                        ImGui::Text("%c  %12.4f   %12.4f   %12.4f", 'a' + row,
                                    inspected->data.cell[row * 3],
                                    inspected->data.cell[row * 3 + 1], inspected->data.cell[row * 3 + 2]);
                    ImGui::Separator();
                    ImGui::Text("Origin: %.6g   %.6g   %.6g", inspected->data.origin.x,
                                inspected->data.origin.y, inspected->data.origin.z);
                    ImGui::Text("PBC: %s / %s / %s", inspected->data.pbc[0] ? "X" : "-",
                                inspected->data.pbc[1] ? "Y" : "-", inspected->data.pbc[2] ? "Z" : "-");
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Global attributes")) {
                    ImGui::TextWrapped("%s", inspected->data.comment.c_str());
                    std::vector<std::string> names;
                    names.reserve(inspected->data.globalAttributes.size());
                    for (const auto &[name, value] : inspected->data.globalAttributes) names.push_back(name);
                    std::sort(names.begin(), names.end());
                    for (const auto &name : names)
                        ImGui::Text("%s: %.8g", name.c_str(), inspected->data.globalAttributes.at(name));
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Bonds")) {
                    ImGui::Text("%zu bonds", inspected->data.bonds.size());
                    if (ImGui::BeginTable("Bond rows", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV)) {
                        for (const char *name : {"Particle A", "Particle B", "Image X", "Image Y", "Image Z"}) ImGui::TableSetupColumn(name);
                        ImGui::TableHeadersRow();
                        ImGuiListClipper clip; clip.Begin(int(inspected->data.bonds.size()));
                        while (clip.Step()) for (int j = clip.DisplayStart; j < clip.DisplayEnd; ++j) {
                            const auto &bond = inspected->data.bonds[size_t(j)];
                            ImGui::TableNextRow();
                            for (int column = 0; column < 5; ++column) {
                                ImGui::TableNextColumn();
                                int value = column == 0 ? int(bond.a) : column == 1 ? int(bond.b) : bond.image[column - 2];
                                ImGui::Text("%d", value);
                            }
                        }
                        ImGui::EndTable();
                    }
                    ImGui::EndTabItem();
                }
                const bool dataTablesOpen=ImGui::BeginTabItem("Data Tables", nullptr,
                                        histogramPreviewTab ? ImGuiTabItemFlags_SetSelected : 0);
                if (dataTablesOpen) {
                    histogramPreviewTab = false;
                    if (inspected->data.tables.empty()) ImGui::TextDisabled("No analysis tables have been produced at this output.");
                    for (size_t tableIndex = 0; tableIndex < inspected->data.tables.size(); ++tableIndex) {
                        const auto &table = inspected->data.tables[tableIndex];
                        ImGui::PushID(int(tableIndex));
                        if (ImGui::CollapsingHeader(table.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                            if (ImGui::SmallButton("Export CSV...")) {
                                const auto outputPath = dialog(window, true, L"CSV file\0*.csv\0", L"csv");
                                if (!outputPath.empty()) {
                                    try { writeDataTableCsv(outputPath, table); status = "Exported " + table.name; }
                                    catch (const std::exception &exception) { error = exception.what(); }
                                }
                            }
                            if (table.name.rfind("Histogram: ", 0) == 0 &&
                                table.columns.size() >= 2 && !table.rows.empty()) {
                                std::vector<float> bins;
                                bins.reserve(table.rows.size());
                                for (const auto &row : table.rows) {
                                    char *end = nullptr;
                                    const float value = row.size() > 1
                                        ? std::strtof(row[1].c_str(), &end) : 0.f;
                                    const float finiteValue = end && *end == '\0' && std::isfinite(value)
                                        ? std::max(0.f, value) : 0.f;
                                    bins.push_back(finiteValue);
                                }
                                const std::string viewKey =
                                    (inspectorNode < 0 ? std::string("final") : inspectorNodeId) +
                                    "|" + table.name + "|" + std::to_string(tableIndex);
                                auto &view = histogramPlotViews[viewKey];
                                if (view.lastBin < 0) view.lastBin = int(bins.size()) - 1;
                                view.firstBin = std::clamp(view.firstBin, 0, int(bins.size()) - 1);
                                view.lastBin = std::clamp(view.lastBin, view.firstBin, int(bins.size()) - 1);
                                int firstBin = view.firstBin + 1, lastBin = view.lastBin + 1;
                                ImGui::SetNextItemWidth(U(100));
                                if (ImGui::InputInt("First bin", &firstBin, 1, 8))
                                    view.firstBin = std::clamp(firstBin - 1, 0, int(bins.size()) - 1);
                                view.lastBin = std::max(view.lastBin, view.firstBin);
                                ImGui::SameLine();
                                ImGui::SetNextItemWidth(U(100));
                                if (ImGui::InputInt("Last bin", &lastBin, 1, 8))
                                    view.lastBin = std::clamp(lastBin - 1, view.firstBin, int(bins.size()) - 1);
                                ImGui::SameLine();
                                if (ImGui::SmallButton("Reset plot view")) {
                                    view.firstBin = 0;
                                    view.lastBin = int(bins.size()) - 1;
                                    view.yMaximum = 0;
                                }
                                ImGui::SetNextItemWidth(U(170));
                                ImGui::InputFloat("Y axis maximum (0 = auto)", &view.yMaximum,
                                                  0, 0, "%.6g");
                                view.yMaximum = std::max(0.f, std::isfinite(view.yMaximum)
                                    ? view.yMaximum : 0.f);
                                std::vector<float> visibleBins(
                                    bins.begin() + view.firstBin, bins.begin() + view.lastBin + 1);
                                float visibleMaximum = 0;
                                for (float value : visibleBins) visibleMaximum = std::max(visibleMaximum, value);
                                const float scaleMaximum = view.yMaximum > 0
                                    ? view.yMaximum : visibleMaximum > 0 ? visibleMaximum : 1.f;
                                ImGui::TextDisabled("%s across bins %d-%d of %zu",
                                    table.columns[1].c_str(), view.firstBin + 1, view.lastBin + 1, bins.size());
                                ImGui::PlotHistogram("##histogram-result", visibleBins.data(),
                                    int(visibleBins.size()), 0, nullptr, 0, scaleMaximum, {-1, U(72)});
                                if (ImGui::IsItemHovered())
                                    ImGui::SetTooltip("Bin centers are listed in the table below.");
                            }
                            if (table.name.rfind("Scatter plot: ",0)==0 &&
                                table.columns.size()>=2 && !table.rows.empty()) {
                                std::vector<std::pair<double,double>> points;
                                points.reserve(table.rows.size());
                                double minX=std::numeric_limits<double>::infinity();
                                double maxX=-minX,minY=minX,maxY=-minX;
                                for (const auto &row:table.rows) {
                                    if (row.size()<2) continue;
                                    const double x=std::stod(row[0]), y=std::stod(row[1]);
                                    points.emplace_back(x,y);
                                    minX=std::min(minX,x); maxX=std::max(maxX,x);
                                    minY=std::min(minY,y); maxY=std::max(maxY,y);
                                }
                                if (!points.empty()) {
                                    const ImVec2 chartSize{ImGui::GetContentRegionAvail().x,U(150)};
                                    ImGui::InvisibleButton("##scatter-plot",chartSize);
                                    recordUiTestItem("inspector.scatter-chart","##scatter-plot");
                                    const ImVec2 chartMin=ImGui::GetItemRectMin();
                                    const ImVec2 chartMax=ImGui::GetItemRectMax();
                                    auto *draw=ImGui::GetWindowDrawList();
                                    draw->AddRectFilled(chartMin,chartMax,ImGui::GetColorU32(ImGuiCol_FrameBg),U(3));
                                    draw->AddRect(chartMin,chartMax,ImGui::GetColorU32(ImGuiCol_Border),U(3));
                                    const float pad=U(12), innerW=std::max(1.f,chartMax.x-chartMin.x-pad*2),
                                                innerH=std::max(1.f,chartMax.y-chartMin.y-pad*2);
                                    const double scaleX=std::max(std::abs(minX),std::abs(maxX));
                                    const double scaleY=std::max(std::abs(minY),std::abs(maxY));
                                    const double loX=scaleX?minX/scaleX:0, hiX=scaleX?maxX/scaleX:1;
                                    const double loY=scaleY?minY/scaleY:0, hiY=scaleY?maxY/scaleY:1;
                                    const double spanX=hiX-loX, spanY=hiY-loY;
                                    const size_t step=std::max<size_t>(1,(points.size()+29999)/30000);
                                    const ImU32 pointColor=ImGui::GetColorU32(accent);
                                    for (size_t i=0;i<points.size();i+=step) {
                                        const double x=scaleX?points[i].first/scaleX:0;
                                        const double y=scaleY?points[i].second/scaleY:0;
                                        const float fx=float(spanX>0?(x-loX)/spanX:.5);
                                        const float fy=float(spanY>0?(y-loY)/spanY:.5);
                                        draw->AddCircleFilled({chartMin.x+pad+std::clamp(fx,0.f,1.f)*innerW,
                                                               chartMax.y-pad-std::clamp(fy,0.f,1.f)*innerH},
                                                              U(1.8f),pointColor);
                                    }
                                    draw->AddText({chartMin.x+pad,chartMin.y+pad},
                                                  ImGui::GetColorU32(ImGuiCol_TextDisabled),table.columns[1].c_str());
                                    draw->AddText({chartMax.x-pad-U(80),chartMax.y-pad-U(14)},
                                                  ImGui::GetColorU32(ImGuiCol_TextDisabled),table.columns[0].c_str());
                                    if (ImGui::IsItemHovered())
                                        ImGui::SetTooltip("%zu plotted pairs%s",points.size(),
                                            table.name.find("deterministic preview")!=std::string::npos
                                                ? " (deterministic preview subset)" : "");
                                }
                            }
                            if (ImGui::BeginTable("table", int(table.columns.size()), ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, {-1, U(240)})) {
                                for (const auto &column : table.columns) ImGui::TableSetupColumn(column.c_str());
                                ImGui::TableHeadersRow();
                                ImGuiListClipper clip;
                                clip.Begin(int(table.rows.size()));
                                while (clip.Step()) for (int rowIndex = clip.DisplayStart; rowIndex < clip.DisplayEnd; ++rowIndex) {
                                    const auto &row = table.rows[size_t(rowIndex)];
                                    ImGui::TableNextRow();
                                    for (size_t column = 0; column < table.columns.size(); ++column) {
                                        ImGui::TableNextColumn();
                                        if (column < row.size()) ImGui::TextUnformatted(row[column].c_str());
                                    }
                                }
                                ImGui::EndTable();
                            }
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::EndChild();
        }
        timeline();
        ImGui::End();
    }
    void right(float w, float h) {
        fixed("Properties", w - rightWidth(), U(90), rightWidth(), h - U(120));
        if (ImGui::BeginTabBar("Settings")) {
            if (ImGui::BeginTabItem("Pipeline", nullptr, selectPipeline ? ImGuiTabItemFlags_SetSelected : 0)) {
                selectPipeline = false;
                heading("Pipeline editor");
                if (ImGui::Button("Add modification...", {-1, U(32)}))
                    showCatalog = true;
                recordUiTestItem("pipeline.add-modification");
                {
                    auto p = ImGui::GetItemRectMax();
                    ImGui::GetWindowDrawList()->AddTriangleFilled({p.x-U(18),p.y-U(18)},{p.x-U(10),p.y-U(18)},{p.x-U(14),p.y-U(13)},ImGui::GetColorU32(ImGuiCol_Text));
                }
                catalogAnchor = {ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y + 2};
                ImGui::BeginChild("Stack", {-1, U(205)}, ImGuiChildFlags_Borders);
                if (mods.empty())
                    ImGui::TextWrapped("Add a modification to start processing.");
                for (int i = int(mods.size()) - 1; i >= 0; i--) {
                    auto &m = mods[i];
                    ImGui::PushID(m.id.c_str());
                    const auto &style=ImGui::GetStyle();
                    const float itemSpacing=style.ItemSpacing.x;
                    const float actionWidth=
                        ImGui::CalcTextSize("↑").x+style.FramePadding.x*2+
                        ImGui::CalcTextSize("↓").x+style.FramePadding.x*2+
                        ImGui::CalcTextSize("Copy").x+style.FramePadding.x*2+
                        ImGui::CalcTextSize("×").x+style.FramePadding.x*2;
                    const float labelWidth=std::max(0.f,ImGui::GetContentRegionAvail().x-
                        ImGui::GetFrameHeight()-actionWidth-itemSpacing*5);
                    bool enabled = m.enabled;
                    bool enabledChanged=ImGui::Checkbox("##enabled", &enabled);
                    recordUiTestItem(std::string("pipeline.node.enabled.")+m.id);
                    if (enabledChanged) {
                        checkpoint();
                        m.enabled = enabled;
                        update(size_t(i));
                    }
                    ImGui::SameLine();
                    const char *name = m.displayName.empty() ? opName(m.op) : m.displayName.c_str();
                    if (ImGui::Selectable(name, modifierGraph.selected == size_t(i),
                                          ImGuiSelectableFlags_None,{labelWidth,0}))
                        modifierGraph.selected = size_t(i);
                    recordUiTestItem(std::string("pipeline.node.select.")+m.id);
                    ImGui::SameLine();
                    ImGui::BeginDisabled(i + 1 >= int(mods.size()));
                    bool moveUpPressed=ImGui::SmallButton("↑");
                    ImGui::EndDisabled();
                    recordUiTestItem(std::string("pipeline.node.up.")+m.id);
                    if (moveUpPressed && i + 1 < int(mods.size())) {
                        checkpoint(); modifierGraph.move(size_t(i), size_t(i + 1)); update(size_t(i));
                        ImGui::PopID(); break;
                    }
                    ImGui::SameLine();
                    ImGui::BeginDisabled(i == 0);
                    bool moveDownPressed=ImGui::SmallButton("↓");
                    ImGui::EndDisabled();
                    recordUiTestItem(std::string("pipeline.node.down.")+m.id);
                    if (moveDownPressed && i > 0) {
                        checkpoint(); modifierGraph.move(size_t(i), size_t(i - 1)); update(size_t(i - 1));
                        ImGui::PopID(); break;
                    }
                    ImGui::SameLine();
                    bool copyPressed=ImGui::SmallButton("Copy");
                    recordUiTestItem(std::string("pipeline.node.copy.")+m.id);
                    if (copyPressed) {
                        checkpoint();
                        auto copy = m;
                        copy.id = std::to_string(nextModifierId++);
                        copy.error.clear(); copy.outputs.clear(); copy.dirty = true;
                        modifierGraph.insert(std::move(copy), size_t(i + 1));
                        update(); ImGui::PopID(); break;
                    }
                    ImGui::SameLine();
                    if (!m.error.empty())
                        ImGui::TextColored({1, .32f, .28f, 1}, "!");
                    if (ImGui::IsItemHovered() && !m.error.empty()) ImGui::SetTooltip("%s", m.error.c_str());
                    bool deletePressed=ImGui::SmallButton("×");
                    recordUiTestItem(std::string("pipeline.node.delete.")+m.id);
                    if (deletePressed) {
                        checkpoint();
                        modifierGraph.erase(size_t(i));
                        update(size_t(i));
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                }
                ImGui::EndChild();
                if (!mods.empty() && modifierGraph.selected < mods.size()) {
                    auto &m = mods[modifierGraph.selected];
                    ImGui::PushID(m.id.c_str());
                    heading(m.displayName.empty() ? opName(m.op) : m.displayName.c_str());
                    if (m.op == Op::Slice || m.op == Op::Translate || m.op == Op::Scale || m.op == Op::Rotate || m.op == Op::SelectRange) {
                        float v = m.value;
                        ImGui::SetNextItemWidth(U(140));
                        if (ImGui::DragFloat("Value", &v, .1f)) {
                            checkpoint();
                            m.value = v;
                            update();
                        }
                        if (m.op != Op::Scale) {
                            int a = m.axis;
                            ImGui::SetNextItemWidth(U(140));
                            if (ImGui::Combo("Axis", &a, "X\0Y\0Z\0")) {
                                checkpoint();
                                m.axis = a;
                                update();
                            }
                        }
                    }
                    if (m.op == Op::SelectRange) {
                        float upper = m.upper;
                        if (ImGui::DragFloat("Upper bound", &upper, .1f)) {
                            checkpoint(); m.upper = upper; update();
                        }
                    }
                    if (m.op == Op::EditCell) {
                        auto editedCell = m.editedCell;
                        auto origin = m.editedOrigin;
                        auto pbc = m.editedPbc;
                        bool transform = m.transformCoordinatesWithCell;
                        bool changed = false;
                        heading("Cell vectors");
                        if (ImGui::BeginTable("Edited cell vectors", 4,
                                              ImGuiTableFlags_SizingStretchSame)) {
                            ImGui::TableSetupColumn("Vector");
                            ImGui::TableSetupColumn("X"); ImGui::TableSetupColumn("Y");
                            ImGui::TableSetupColumn("Z"); ImGui::TableHeadersRow();
                            for (int row = 0; row < 3; ++row) {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0); ImGui::Text("%c", 'a' + row);
                                for (int column = 0; column < 3; ++column) {
                                    ImGui::TableSetColumnIndex(column + 1);
                                    ImGui::PushID(row * 3 + column);
                                    ImGui::SetNextItemWidth(-1);
                                    changed |= ImGui::InputDouble("##cell", &editedCell[row * 3 + column],
                                                                  0, 0, "%.8g");
                                    ImGui::PopID();
                                }
                            }
                            ImGui::EndTable();
                        }
                        ImGui::Text("Origin");
                        float originValues[3]{origin.x, origin.y, origin.z};
                        for (int axis = 0; axis < 3; ++axis) {
                            if (axis) ImGui::SameLine();
                            ImGui::PushID(100 + axis);
                            ImGui::SetNextItemWidth(U(88));
                            changed |= ImGui::InputFloat(axis == 0 ? "X" : axis == 1 ? "Y" : "Z",
                                                        &originValues[axis], 0, 0, "%.6g");
                            ImGui::PopID();
                        }
                        origin = {originValues[0], originValues[1], originValues[2]};
                        ImGui::Text("Periodic boundaries");
                        for (int axis = 0; axis < 3; ++axis) {
                            if (axis) ImGui::SameLine();
                            ImGui::PushID(200 + axis);
                            changed |= ImGui::Checkbox(axis == 0 ? "X" : axis == 1 ? "Y" : "Z",
                                                       &pbc[axis]);
                            ImGui::PopID();
                        }
                        changed |= ImGui::Checkbox("Transform particle coordinates with cell", &transform);
                        ImGui::TextWrapped("Off: particle positions stay fixed while the cell changes. On: preserve fractional coordinates in the edited cell.");
                        if (changed) {
                            checkpoint(); m.editedCell = editedCell; m.editedOrigin = origin;
                            m.editedPbc = pbc; m.transformCoordinatesWithCell = transform; update();
                        }
                    }
                    if (m.op == Op::AffineTransform) {
                        auto matrix = m.affineTransform;
                        bool transformVectors = m.transformVectorProperties;
                        bool changed = false;
                        ImGui::TextWrapped("Maps particle positions, cell vectors, and origin with a 3 x 4 matrix. Translation is not applied to vectors.");
                        if (ImGui::BeginTable("Affine matrix", 4,
                                              ImGuiTableFlags_SizingStretchSame)) {
                            for (const char *column : {"X", "Y", "Z", "Translation"})
                                ImGui::TableSetupColumn(column);
                            ImGui::TableHeadersRow();
                            for (int row = 0; row < 3; ++row) {
                                ImGui::TableNextRow();
                                for (int column = 0; column < 4; ++column) {
                                    ImGui::TableSetColumnIndex(column);
                                    ImGui::PushID(row * 4 + column);
                                    ImGui::SetNextItemWidth(-1);
                                    changed |= ImGui::InputDouble("##affine", &matrix[row * 4 + column],
                                                                  0, 0, "%.7g");
                                    ImGui::PopID();
                                }
                            }
                            ImGui::EndTable();
                        }
                        changed |= ImGui::Checkbox("Transform vector properties", &transformVectors);
                        ImGui::TextDisabled("Applies the 3 x 3 linear part. Normal vectors are not inverse-transpose transformed.");
                        if (changed) {
                            checkpoint(); m.affineTransform = matrix;
                            m.transformVectorProperties = transformVectors; update();
                        }
                    }
                    if (m.op == Op::ClusterAnalysis) {
                        bool useBonds=m.clusterByBonds;
                        if (ImGui::Checkbox("Group by existing bonds",&useBonds)) {
                            checkpoint(); m.clusterByBonds=useBonds; update();
                        }
                        bool onlySelected=m.clusterOnlySelected;
                        if (ImGui::Checkbox("Use only selected particles",&onlySelected)) {
                            checkpoint(); m.clusterOnlySelected=onlySelected; update();
                        }
                        bool sortBySize=m.clusterSortBySize;
                        if (ImGui::Checkbox("Sort clusters by size",&sortBySize)) {
                            checkpoint(); m.clusterSortBySize=sortBySize; update();
                        }
                        ImGui::TextDisabled("When enabled, the largest component receives Cluster ID 1.");
                        ImGui::TextDisabled(useBonds
                            ? "Connected components follow the input bond topology; selected-only mode assigns ID 0 to other particles."
                            : "Connected components follow the periodic minimum-image cutoff. Unselected particles get Cluster ID 0 when selected-only is enabled.");
                    }
                    if (m.op == Op::CreateBonds || m.op == Op::CommonNeighborAnalysis ||
                        m.op == Op::CoordinationAnalysis || m.op == Op::ClusterAnalysis ||
                        m.op == Op::RadialDistribution || m.op == Op::ExpandSelection) {
                        if (!(m.op==Op::CreateBonds && m.bondTypeCutoffsEnabled) &&
                            !(m.op==Op::ClusterAnalysis && m.clusterByBonds)) {
                            float value = m.value;
                            if (ImGui::DragFloat("Cutoff distance", &value, .01f, .0001f, 100000.f, "%.5g")) {
                                checkpoint(); m.value = value; update();
                            }
                        }
                    }
                    if (m.op==Op::RadialDistribution) {
                        int bins=m.rdfBins;
                        const float binsLabelWidth=ImGui::CalcTextSize("RDF histogram bins").x+
                                                   ImGui::GetStyle().ItemInnerSpacing.x;
                        ImGui::SetNextItemWidth(std::max(U(50),ImGui::GetContentRegionAvail().x-binsLabelWidth));
                        if (ImGui::InputInt("RDF histogram bins",&bins)) {
                            checkpoint(); m.rdfBins=std::clamp(bins,1,4096); update();
                        }
                        recordUiTestItem(std::string("pipeline.rdf-bins.")+m.id,"RDF histogram bins");
                    }
                    if (m.op==Op::RadialDistribution || m.op==Op::CoordinationAnalysis) {
                        bool onlySelected=m.neighborOnlySelected;
                        if (ImGui::Checkbox("Use only selected particles",&onlySelected)) {
                            checkpoint(); m.neighborOnlySelected=onlySelected; update();
                        }
                        ImGui::TextDisabled("Unselected particles are excluded as both analysis centers and neighbors.");
                    }
                    if (m.op == Op::SelectOverlapping) {
                        bool useRadii = m.overlapUseRadii;
                        if (ImGui::Checkbox("Use per-particle radii", &useRadii)) {
                            checkpoint(); m.overlapUseRadii = useRadii; update();
                        }
                        if (m.overlapUseRadii) {
                            std::vector<std::string> radiusProperties;
                            for (const auto &[name, values] : result.data.scalarProperties)
                                if (values.size() == result.data.atoms.size())
                                    radiusProperties.push_back(name);
                            std::sort(radiusProperties.begin(), radiusProperties.end());
                            if (radiusProperties.empty()) {
                                ImGui::TextWrapped("No scalar particle-radius property is available. Compute or import a Radius property first.");
                            } else if (ImGui::BeginCombo("Radius property", m.property.c_str())) {
                                for (const auto &name : radiusProperties) {
                                    const bool selected = m.property == name;
                                    if (ImGui::Selectable(name.c_str(), selected)) {
                                        checkpoint(); m.property = name; update();
                                    }
                                    if (selected) ImGui::SetItemDefaultFocus();
                                }
                                ImGui::EndCombo();
                            }
                            ImGui::TextWrapped("Two particles overlap when their minimum-image distance is less than the sum of their radii.");
                        } else {
                            float value = m.value;
                            if (ImGui::DragFloat("Pair cutoff", &value, .01f, .0001f, 100000.f, "%.5g")) {
                                checkpoint(); m.value = value; update();
                            }
                            ImGui::TextWrapped("Selects both endpoints of every pair closer than this distance. Enable per-particle radii for sphere-overlap testing.");
                        }
                    }
                    if (m.op == Op::BondLengthDistribution || m.op == Op::BondAngleDistribution) {
                        int bins=m.type;
                        if (ImGui::InputInt("Bins", &bins)) {
                            checkpoint(); m.type=std::clamp(bins,1,4096); update();
                        }
                        int normalization=m.histogramNormalization;
                        if (ImGui::Combo("Normalization",&normalization,
                                         "Absolute counts\0Relative frequency\0Probability density\0")) {
                            checkpoint(); m.histogramNormalization=normalization; update();
                        }
                        if (normalization==2)
                            ImGui::TextDisabled(m.op==Op::BondLengthDistribution ?
                                "Density is normalized per length unit." : "Density is normalized per degree.");
                        ImGui::TextWrapped("Uses explicit pipeline bonds, including periodic image shifts. Add Create bonds upstream if the dataset has no bond topology.");
                    }
                    if (m.op==Op::ScatterPlot) {
                        auto edited=m;
                        bool changed=colorPropertyCombo("X property",edited.scatterXProperty,true);
                        recordUiTestItem(std::string("pipeline.scatter-x.")+m.id,"X property");
                        changed|=colorPropertyCombo("Y property",edited.scatterYProperty,true);
                        recordUiTestItem(std::string("pipeline.scatter-y.")+m.id,"Y property");
                        if (ImGui::Checkbox("Use only selected particles",&edited.scatterSelectedOnly))
                            changed=true;
                        recordUiTestItem(std::string("pipeline.scatter-selected.")+m.id,
                                         "Use only selected particles");
                        ImGui::TextDisabled("Non-finite pairs are skipped; large plots use a deterministic exported preview sample.");
                        if (changed) {
                            checkpoint();
                            m.scatterXProperty=std::move(edited.scatterXProperty);
                            m.scatterYProperty=std::move(edited.scatterYProperty);
                            m.scatterSelectedOnly=edited.scatterSelectedOnly;
                            update();
                        }
                    }
                    if (m.op == Op::Histogram || m.op == Op::ReduceProperty) {
                        auto edited = m;
                        bool changed = colorPropertyCombo("Input property", edited.property, true);
                        if (m.op == Op::Histogram) {
                            int bins = edited.type;
                            if (ImGui::InputInt("Bins", &bins)) { edited.type = std::clamp(bins, 1, 4096); changed = true; }
                            if (ImGui::Checkbox("Use only selected elements", &edited.histogramSelectedOnly))
                                changed = true;
                            int normalization = edited.histogramNormalization;
                            if (ImGui::Combo("Normalization", &normalization,
                                             "Absolute counts\0Relative frequency\0Probability density\0")) {
                                edited.histogramNormalization = normalization;
                                changed = true;
                            }
                            if (ImGui::Checkbox("Select value range", &edited.histogramSelectRange))
                                changed = true;
                            if (edited.histogramSelectRange) {
                                changed |= ImGui::InputDouble("Range start", &edited.histogramRangeStart,
                                                              0, 0, "%.8g");
                                changed |= ImGui::InputDouble("Range end", &edited.histogramRangeEnd,
                                                              0, 0, "%.8g");
                                ImGui::TextDisabled("Replaces the output selection with particles in the inclusive range.");
                            }
                            ImGui::TextDisabled("The table uses this frame; selected-only filters its input samples.");
                        } else {
                            int reduction = edited.reduceOperation;
                            if (ImGui::Combo("Reduction", &reduction, "Minimum\0Maximum\0Mean\0Sum\0")) {
                                edited.reduceOperation = reduction; changed = true;
                            }
                        }
                        if (changed) {
                            checkpoint();
                            m.property = edited.property;
                            m.type = edited.type;
                            m.reduceOperation = edited.reduceOperation;
                            m.histogramSelectedOnly = edited.histogramSelectedOnly;
                            m.histogramNormalization = edited.histogramNormalization;
                            m.histogramSelectRange = edited.histogramSelectRange;
                            m.histogramRangeStart = edited.histogramRangeStart;
                            m.histogramRangeEnd = edited.histogramRangeEnd;
                            update();
                        }
                    }
                    if (m.op == Op::Replicate) {
                        int count = m.type, axis = m.axis;
                        if (ImGui::InputInt("Copies", &count)) {
                            checkpoint(); m.type = std::clamp(count,1,32); update();
                        }
                        if (ImGui::Combo("Cell vector", &axis,"A\0B\0C\0")) {
                            checkpoint(); m.axis = axis; update();
                        }
                    }
                    if (m.op == Op::SelectIndex) {
                        int index = m.type;
                        if (ImGui::InputInt("Atom index", &index)) {
                            checkpoint(); m.type = std::max(0,index); update();
                        }
                    }
                    if (m.op == Op::ManualSelection) {
                        ImGui::TextDisabled("%zu particles selected. Click a table row to replace; Ctrl-click to toggle.",
                                            m.manualSelection.size());
                    }
                    if (m.op == Op::SelectType || m.op == Op::EditType) {
                        int t = m.type;
                        if (ImGui::InputInt("Type index", &t)) {
                            checkpoint();
                            m.type = std::clamp(t, 0, std::max(0, int(source.species.size()) - 1));
                            update();
                        }
                    }
                    if (m.op == Op::ColorType)
                        ImGui::TextWrapped("Particles use their type colors from Particle appearance settings.");
                    if (m.op == Op::AssignColor) {
                        auto color=m.assignColor;
                        if (ImGui::ColorEdit3("Assigned color",color.data())) {
                            checkpoint(); m.assignColor=color; update();
                        }
                        ImGui::TextWrapped("Colors currently selected particles. If the selection is empty, colors all particles.");
                    }
                    if (m.op == Op::RemoveProperty) {
                        char property[256]{};
                        snprintf(property, sizeof(property), "%s", m.property.c_str());
                        if (ImGui::InputText("Property name", property, sizeof(property), ImGuiInputTextFlags_EnterReturnsTrue)) {
                            checkpoint(); m.property = property; update();
                        }
                        ImGui::TextWrapped("Enter the exact scalar or vector property name and press Enter. Position and particle types are retained.");
                    }
                    if (m.op == Op::ColorCoding) {
                        bool changed = false;
                        auto edited = m;
                        auto property = edited.property;
                        if (colorPropertyCombo("Input property", property, true)) {
                            edited.property = std::move(property); edited.colorAutoRange = true;
                            edited.colorAllFramesRange = false; changed = true;
                        }
                        int gradient = edited.colorGradient;
                        if (ImGui::Combo("Color gradient", &gradient, "Rainbow\0Blue-White-Red\0Cyclic Rainbow\0Fast\0Grayscale\0Hot\0Jet\0Magma\0Viridis\0Plasma\0")) { edited.colorGradient = gradient; changed = true; }
                        bool automatic = edited.colorAutoRange;
                        if (ImGui::Checkbox("Automatic range", &automatic)) { edited.colorAutoRange = automatic; changed = true; }
                        bool symmetric = edited.colorSymmetricRange;
                        if (ImGui::Checkbox("Symmetric range", &symmetric)) { edited.colorSymmetricRange = symmetric; changed = true; }
                        if (!edited.colorAutoRange) {
                            float low = edited.colorMin, high = edited.colorMax;
                            if (ImGui::DragFloat("Start value", &low, .01f)) { edited.colorMin = low; changed = true; }
                            if (ImGui::DragFloat("End value", &high, .01f)) { edited.colorMax = high; changed = true; }
                        }
                        bool selectedOnly = edited.colorSelectedOnly;
                        if (ImGui::Checkbox("Color only selected elements", &selectedOnly)) { edited.colorSelectedOnly = selectedOnly; changed = true; }
                        bool discrete = edited.colorDiscrete, reverse = edited.colorReverse;
                        if (ImGui::Checkbox("Discretize", &discrete)) { edited.colorDiscrete = discrete; changed = true; }
                        if (ImGui::Checkbox("Reverse range", &reverse)) { edited.colorReverse = reverse; changed = true; }
                        bool keepSelection = edited.colorKeepSelection;
                        if (ImGui::Checkbox("Keep selection", &keepSelection)) { edited.colorKeepSelection = keepSelection; changed = true; }
                        bool showLegend = edited.colorLegend;
                        if (ImGui::Checkbox("Show color legend", &showLegend)) {
                            checkpoint(); m.colorLegend = showLegend; colorLegend = showLegend;
                        }
                        ImGui::BeginDisabled(colorRangeRunning || busy || indexing || pipelineBusy || frames.empty());
                        if (ImGui::Button("Compute range across all frames"))
                            computeColorRangeAllFrames(modifierGraph.selected);
                        ImGui::EndDisabled();
                        if (colorRangeRunning) {
                            ImGui::ProgressBar(colorRangeProgress.load(), ImVec2(-1, 0), "Scanning trajectory");
                            if (ImGui::Button("Cancel range calculation")) colorRangeCancel = true;
                        } else if (m.colorAllFramesRange) {
                            ImGui::TextDisabled("Range: %.6g to %.6g across %zu frames", m.colorMin, m.colorMax, frames.size());
                            if (ImGui::SmallButton("Use current-frame automatic range")) {
                                checkpoint(); m.colorAllFramesRange = false; m.colorAutoRange = true; update();
                            }
                        }
                        if (changed) {
                            checkpoint();
                            m.property = std::move(edited.property);
                            m.colorGradient = edited.colorGradient;
                            m.colorAutoRange = edited.colorAutoRange;
                            m.colorSymmetricRange = edited.colorSymmetricRange;
                            m.colorReverse = edited.colorReverse;
                            m.colorDiscrete = edited.colorDiscrete;
                            m.colorSelectedOnly = edited.colorSelectedOnly;
                            m.colorKeepSelection = edited.colorKeepSelection;
                            m.colorMin = edited.colorMin; m.colorMax = edited.colorMax;
                            m.colorAllFramesRange = edited.colorAllFramesRange;
                            update();
                        }
                    }
                    if (m.op == Op::CommonNeighborAnalysis || m.op == Op::CreateBonds) {
                        if (m.op == Op::CreateBonds) {
                            bool discard = m.discardExistingBonds;
                            if (ImGui::Checkbox("Discard existing bonds", &discard)) {
                                checkpoint(); m.discardExistingBonds = discard; update();
                            }
                            const size_t typeCount=result.data.species.size();
                            if (typeCount>0 && typeCount<=32) {
                                bool usePairCutoffs=m.bondTypeCutoffsEnabled;
                                if (ImGui::Checkbox("Use type-pair cutoffs",&usePairCutoffs)) {
                                    checkpoint();
                                    m.bondTypeCutoffsEnabled=usePairCutoffs;
                                    if (usePairCutoffs)
                                        m.bondTypeCutoffs.assign(typeCount*typeCount,m.value);
                                    update();
                                }
                                if (m.bondTypeCutoffsEnabled) {
                                    if (m.bondTypeCutoffs.size()!=typeCount*typeCount) {
                                        ImGui::TextColored({1.f,.62f,.18f,1.f},"Type table changed; reset pair cutoffs to continue.");
                                        if (ImGui::Button("Reset type-pair cutoffs")) {
                                            checkpoint(); m.bondTypeCutoffs.assign(typeCount*typeCount,m.value); update();
                                        }
                                    } else if (ImGui::BeginTable("Bond type-pair cutoffs",int(typeCount+1),
                                               ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg|ImGuiTableFlags_ScrollX,
                                               {0,U(180)})) {
                                        ImGui::TableSetupColumn("Type",ImGuiTableColumnFlags_WidthFixed,U(90));
                                        for (const auto &name:result.data.species)
                                            ImGui::TableSetupColumn(name.c_str(),ImGuiTableColumnFlags_WidthFixed,U(96));
                                        ImGui::TableHeadersRow();
                                        for (size_t a=0;a<typeCount;++a) {
                                            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                                            ImGui::TextUnformatted(result.data.species[a].c_str());
                                            for (size_t b=0;b<typeCount;++b) {
                                                ImGui::TableSetColumnIndex(int(b+1));
                                                if (b<a) { ImGui::TextDisabled("same"); continue; }
                                                float pair=m.bondTypeCutoffs[a*typeCount+b];
                                                ImGui::PushID(int(a*typeCount+b));
                                                ImGui::SetNextItemWidth(-1);
                                                if (ImGui::DragFloat("##cutoff",&pair,.01f,0.f,100000.f,"%.4g")) {
                                                    checkpoint();
                                                    m.bondTypeCutoffs[a*typeCount+b]=pair;
                                                    m.bondTypeCutoffs[b*typeCount+a]=pair;
                                                    update();
                                                }
                                                ImGui::PopID();
                                            }
                                        }
                                        ImGui::EndTable();
                                    }
                                    ImGui::TextDisabled("Symmetric pair matrix; zero disables that type pair.");
                                }
                            } else ImGui::TextDisabled(typeCount==0 ? "Load particle types to configure bonds." :
                                                       "Type-pair mode supports at most 32 particle types.");
                            bool visible = m.bondsVisible;
                            bool cylinders = m.bondCylinders;
                            float width = m.bondWidth;
                            float bondRadius = m.bondRadius;
                            auto color = m.bondColor;
                            bool changed = ImGui::Checkbox("Show bonds", &visible);
                            const char *representations[] = {"Screen-space lines", "3D cylinders"};
                            int representation = cylinders ? 1 : 0;
                            if (ImGui::Combo("Bond representation", &representation, representations, 2)) {
                                cylinders = representation == 1;
                                changed = true;
                            }
                            if (cylinders)
                                changed |= ImGui::DragFloat("Cylinder radius", &bondRadius, .005f,
                                                            .001f, 100.f, "%.4g units");
                            else
                                changed |= ImGui::SliderFloat("Line width", &width, .5f, 12.f, "%.1f px");
                            changed |= ImGui::ColorEdit3("Bond color", color.data());
                            if (changed) {
                                checkpoint(); m.bondsVisible=visible; m.bondCylinders=cylinders;
                                m.bondWidth=width; m.bondRadius=bondRadius; m.bondColor=color; update();
                            }
                            ImGui::TextDisabled("%zu bonds", result.data.bonds.size());
                        }
                        else {
                            size_t counts[5]{};
                            if (auto codes = result.data.scalarProperties.find("Structure Type");
                                codes != result.data.scalarProperties.end())
                                for (double value : codes->second)
                                    if (value >= 0 && value < 5) ++counts[size_t(value)];
                            ImGui::TextDisabled("FCC %zu | HCP %zu | BCC %zu | ICO %zu | Other %zu",
                                                counts[1], counts[2], counts[3], counts[4], counts[0]);
                        }
                    }
                    if (m.op == Op::ExpandSelection) {
                        float c = m.value;
                        if (ImGui::DragFloat("Cutoff", &c, .01f, .001f, 100.f)) { checkpoint(); m.value = c; update(); }
                        size_t selectedParticles = std::count(result.selected.begin(), result.selected.end(), uint8_t(1));
                        int steps = m.type;
                        if (ImGui::InputInt("Expansion steps", &steps)) { checkpoint(); m.type = std::clamp(steps, 1, 64); update(); }
                        ImGui::TextDisabled("Selected: %zu / %zu", selectedParticles, result.data.atoms.size());
                    }
                    if (m.op == Op::SelectOverlapping) {
                        const size_t selectedParticles = std::count(result.selected.begin(), result.selected.end(), uint8_t(1));
                        ImGui::TextDisabled("Overlapping particles: %zu", selectedParticles);
                    }
                    if (m.op == Op::ExpressionSelect) {
                        char expression[512]{};
                        snprintf(expression, sizeof(expression), "%s", m.property.c_str());
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::InputText("Expression", expression, sizeof(expression), ImGuiInputTextFlags_EnterReturnsTrue)) {
                            checkpoint(); m.property = expression; update();
                        }
                        ImGui::TextWrapped("Use x/y/z, Position.X/Y/Z, type, scalar properties, arithmetic, comparisons, &&, ||, !, abs(), sqrt(), isfinite(). Write `Potential Energy` to reference a property name containing spaces; double a backtick to escape it. Press Enter to evaluate.");
                        size_t selectedParticles = std::count(result.selected.begin(), result.selected.end(), uint8_t(1));
                        ImGui::TextDisabled("Selected: %zu / %zu", selectedParticles, result.data.atoms.size());
                    }
                    if (m.op == Op::ComputeProperty) {
                        char expression[512]{}, outputName[128]{};
                        snprintf(expression, sizeof(expression), "%s", m.property.c_str());
                        snprintf(outputName, sizeof(outputName), "%s", m.outputProperty.c_str());
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::InputText("Output property", outputName, sizeof(outputName), ImGuiInputTextFlags_EnterReturnsTrue)) {
                            checkpoint(); m.outputProperty = outputName; update();
                        }
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::InputText("Expression", expression, sizeof(expression), ImGuiInputTextFlags_EnterReturnsTrue)) {
                            checkpoint(); m.property = expression; update();
                        }
                        ImGui::TextWrapped("Publishes one scalar value per particle. Use the safe expression language; press Enter to evaluate.");
                    }
                    ImGui::PopID();
                }
                heading("Scene visibility");
                ImGui::Checkbox("##particles-visible", &particles);
                ImGui::SameLine();
                if (ImGui::Button("Particles")) focusParticleAppearance = true;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Edit particle shape, radius and type colors");
                ImGui::SameLine();
                ImGui::Checkbox("Simulation cell", &cell);
                if (cell) {
                    ImGui::Combo("Cell dimensionality", &cellDimension, "2D\0" "3D\0");
                    ImGui::ColorEdit4("Cell line color", cellColor, ImGuiColorEditFlags_AlphaBar);
                    ImGui::SliderFloat("Line width", &cellWidth, .5f, 4.f, "%.1f px");
                    ImGui::SliderFloat("Glow", &cellGlow, 0.f, 1.f, "%.2f");
                    ImGui::Checkbox("Dashed secondary edges", &cellDashed); ImGui::Checkbox("Show cell labels", &cellLabels);
                    ImGui::TextDisabled("PBC: %s %s %s", source.pbc[0]?"X":"-", source.pbc[1]?"Y":"-", source.pbc[2]?"Z":"-");
                }
                heading("Data source");
                ImGui::TextWrapped("%s", path.empty() ? "Generated FCC crystal"
                                                      : utf8(path.wstring()).c_str());
                ImGui::TextDisabled("Reader: %s", readerName.c_str());
                ImGui::TextDisabled("Trajectory: %zu frame(s)", std::max<size_t>(frames.size(), 1));
                heading("Particle appearance");
                if (focusParticleAppearance) { ImGui::SetScrollHereY(0.f); focusParticleAppearance = false; }
                ImGui::SliderFloat("Radius", &radius, .02f, 2.f, "%.2f");
                const char *shapes = "Sphere / Ellipsoid\0Circle\0Cube / "
                                     "Box\0Cylinder\0Spherocylinder\0Square\0Mesh / User-defined\0";
                ImGui::Combo("Default shape", &particleShape, shapes);
                syncAppearance(result.data.species);
                if (!result.data.species.empty()) {
                    appearanceType =
                        std::clamp(appearanceType, 0, int(result.data.species.size()) - 1);
                    if (ImGui::BeginCombo("Particle type",
                                          result.data.species[appearanceType].c_str())) {
                        for (int i = 0; i < int(result.data.species.size()); ++i)
                            if (ImGui::Selectable(result.data.species[i].c_str(),
                                                  appearanceType == i))
                                appearanceType = i;
                        ImGui::EndCombo();
                    }
                    ImGui::PushID(result.data.species[appearanceType].c_str());
                    auto &style = gpu.styles[appearanceType];
                    bool visible = style.visual[2] > .5f;
                    if (ImGui::Checkbox("Show this type", &visible))
                        style.visual[2] = visible ? 1.f : 0.f;
                    ImGui::ColorEdit3("Type color", style.color.data());
                    bool inheritRadius = style.visual[0] == 0;
                    if (ImGui::Checkbox("Use default radius", &inheritRadius))
                        setDefaultRadius(result.data.species[appearanceType], style, inheritRadius);
                    if (!inheritRadius)
                        ImGui::SliderFloat("Type radius", &style.visual[0], .02f, 5.f, "%.3f");
                    int typeShape = int(style.visual[1]) + 1;
                    if (ImGui::Combo(
                            "Type shape", &typeShape,
                            "Use default\0Sphere / Ellipsoid\0Circle\0Cube / "
                            "Box\0Cylinder\0Spherocylinder\0Square\0Mesh / User-defined\0"))
                        style.visual[1] = float(typeShape - 1);
                    int effective = style.visual[1] < 0 ? particleShape : int(style.visual[1]);
                    if (effective == 0 || effective == 2 || effective == 6)
                        ImGui::SliderFloat3("Axis scales", style.axes.data(), .1f, 4.f, "%.2f");
                    else if (effective == 3 || effective == 4)
                        ImGui::SliderFloat("Half-length / radius", &style.axes[2], .1f, 6.f,
                                           "%.2f");
                    else {
                        ImGui::SliderFloat("Width scale", &style.axes[0], .1f, 4.f);
                        ImGui::SliderFloat("Height scale", &style.axes[1], .1f, 4.f);
                    }
                    ImGui::TextWrapped(
                        effective == 3 || effective == 4
                            ? "Cylinder axis: world Z. End caps and depth are rendered in 3D."
                            : "Type appearance overrides the particle defaults. Selection and "
                              "color coding may override type colors.");
                    if (effective == 6) {
                        ImGui::TextWrapped(
                            "Shared particle OBJ: triangulated, up to 256 triangles; centered and "
                            "normalized to unit radius. High mesh counts cost more GPU time.");
                        ImGui::Text("Loaded triangles: %zu", gpu.meshTriangleCount);
                        if (ImGui::Button("Load particle OBJ...")) {
                            auto p = dialog(window, false, L"Triangle mesh\0*.obj\0", L"obj");
                            if (!p.empty())
                                try {
                                    gpu.loadParticleMesh(p);
                                } catch (const std::exception &e) {
                                    error = e.what();
                                }
                        }
                    }
                    ImGui::PopID();
                }
                if (colorCoding) {
                    heading("ACTIVE COLOR CODING");
                    auto activeColor = std::find_if(mods.rbegin(), mods.rend(), [](const Modifier &m) {
                        return m.enabled && m.op == Op::ColorCoding;
                    });
                    if (activeColor != mods.rend()) {
                        ImGui::Text("Property: %s",activeColor->property.c_str());
                        const float shownMin=activeColor->colorAutoRange && !activeColor->colorAllFramesRange
                            ? colorMin : activeColor->colorMin;
                        const float shownMax=activeColor->colorAutoRange && !activeColor->colorAllFramesRange
                            ? colorMax : activeColor->colorMax;
                        ImGui::Text("Range: %.6g to %.6g%s",shownMin,shownMax,
                                    activeColor->colorAutoRange ? " (automatic)" : " (manual)");
                        ImGui::Text("Gradient: %s",activeColor->colorGradient>=0 && activeColor->colorGradient<10
                            ? std::array<const char *,10>{"Rainbow","Blue-White-Red","Cyclic Rainbow","Fast",
                                "Grayscale","Hot","Jet","Magma","Viridis","Plasma"}[activeColor->colorGradient]
                            : "Custom");
                        ImGui::Text("%s%s%s",activeColor->colorSelectedOnly ? "Selected only  " : "All particles  ",
                                    activeColor->colorDiscrete ? "Discrete  " : "Continuous  ",
                                    activeColor->colorReverse ? "Reversed" : "");
                        ImGui::TextDisabled("Edit coloring parameters on the selected Color coding node in Pipeline.");
                    }
                }
                heading("Position statistics");
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
                ImGui::Combo("Renderer", &renderMode, "Standard GPU\0Wireframe GPU\0Flat particle preview\0Cinematic GPU preview\0");
                ImGui::TextDisabled(renderMode==0 ? "Direct3D 11 / shaded impostors" : renderMode==1 ? "Direct3D 11 / wireframe raster" : "Direct3D 11 / fast preview mode");
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
                if (ImGui::Button("Render active viewport", {-1, U(35)}))
                    exportImage();
                heading("TRAJECTORY");
                ImGui::SliderFloat("Frames/sec", &fps, 1, 60, "%.0f");
                heading("EXPORT DATA");
                ImGui::TextWrapped(source.sampled()
                                       ? "Exports the processed preview, not all source atoms."
                                       : "Exports the processed particle dataset.");
                ImGui::BeginDisabled(exporting || busy);
                if (ImGui::Button("Export data...", {-1, U(32)})) {
                    showDataExport = true;
                    exportFirst = current;
                    exportLast = std::max(0, int(frames.size()) - 1);
                }
                ImGui::EndDisabled();
                if (exporting) {
                    ImGui::ProgressBar(exportProgress);
                    if (ImGui::Button("Cancel export"))
                        exportCancel = true;
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Analysis", nullptr, selectAnalysis ? ImGuiTabItemFlags_SetSelected : 0)) {
                selectAnalysis = false;
                heading("DISLOCATION ANALYSIS (DXA)");
                ImGui::TextWrapped("Runs a coordination-based DXA prepass and reports candidate defect-core atoms. Full Burgers circuit tracing and a dislocation mesh are planned for the next DXA stage.");
                ImGui::InputFloat("DXA cutoff", &dxaCutoff, .05f, .1f, "%.3f");
                if (ImGui::Button("Run DXA prepass", {-1, U(32)}) && !dxaRunning) {
                    auto d=result.data; auto c=dxaCutoff; auto selected=result.selected;
                    dxaRunning=true; dxaJob=std::async(std::launch::async,[d=std::move(d),c,selected=std::move(selected)](){return dxaApproximate(d,c,false,&selected);});
                }
                if (dxaRunning) ImGui::TextDisabled("Analyzing neighbor coordination...");
                if (dxa) {
                    ImGui::Text("Analyzed: %llu", dxa->analyzed); ImGui::Text("Candidate core atoms: %llu", dxa->defectAtoms);
                    for (auto& [name,count] : dxa->structures) ImGui::Text("%s: %llu", name.c_str(), count);
                    ImGui::TextWrapped("%s", dxa->message.c_str());
                    if (ImGui::Button("Export DXA prepass CSV", {-1,U(30)})) { auto p=dialog(window,true,L"CSV file\0*.csv\0",L"csv"); if(!p.empty()){std::ofstream out(p);out<<"atom_index,coordination\n"; auto n=neighbors(result.data,dxaCutoff); for(auto i:dxa->coreAtoms)out<<i<<","<<n.coordination[i]<<"\n"; status="DXA prepass exported";} }
                }
                heading("NEIGHBOR ANALYSIS");
                ImGui::TextWrapped("Coordination number and connected clusters using spatial bins "
                                   "and minimum-image periodic distances.");
                ImGui::InputFloat("Cutoff", &cutoff, .05f, .1f, "%.3f");
                if (ImGui::Button("Compute neighbors", {-1, U(32)}) && !analyzing) {
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
                    ImGui::TextDisabled("Neighbor distances (0 to %.3f)", analysis->cutoff);
                    std::vector<float> histogram(analysis->pairHistogram.size());
                    std::transform(analysis->pairHistogram.begin(),analysis->pairHistogram.end(),histogram.begin(),
                                   [](uint64_t value){return float(value);});
                    ImGui::PlotHistogram("##distances", histogram.data(),int(histogram.size()),0,nullptr,0,FLT_MAX,{-1,75});
                    if (analysis->rdfValid) {
                        std::vector<float> rdf(analysis->rdf.size());
                        std::transform(analysis->rdf.begin(),analysis->rdf.end(),rdf.begin(),
                                       [](double value){return float(value);});
                        ImGui::TextUnformatted("Radial distribution g(r)");
                        ImGui::PlotLines("##rdf",rdf.data(),int(rdf.size()),0,nullptr,0,FLT_MAX,{-1,75});
                    } else ImGui::TextWrapped("Bulk RDF requires a valid fully periodic 3D cell.");
                    if (ImGui::Button("Export distributions CSV", {-1, U(30)})) {
                        auto p = dialog(window, true, L"CSV file\0*.csv\0", L"csv");
                        if (!p.empty()) {
                            std::ofstream out(p); out << "r_min,r_max,pair_count,g_r\n";
                            for (size_t i=0;i<analysis->pairHistogram.size();++i) {
                                out << analysis->cutoff*i/analysis->pairHistogram.size() << ','
                                    << analysis->cutoff*(i+1)/analysis->pairHistogram.size()
                                    << ',' << analysis->pairHistogram[i] << ',';
                                if (analysis->rdfValid) out << analysis->rdf[i];
                                out << '\n';
                            }
                            if (!out) throw std::runtime_error("Cannot write distributions CSV");
                        }
                    }
                    if (ImGui::Button("Export analysis CSV", {-1, U(32)})) {
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
                ImGui::TextWrapped("Requires unsampled data. Periodic neighbor search supports orthogonal and triclinic cells. "
                                   "Current analysis limit: 2 million atoms.");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("System")) {
                heading("AVAILABLE ADAPTERS");
                for (auto a : gpu.adapters) {
                    if (a.duplicate) continue;
                    ImGui::TextWrapped("[%u] %s%s", a.index, utf8(a.name).c_str(),
                                       a.active ? "  • IN USE" : "");
                    ImGui::TextDisabled("Dedicated memory: %.2f GiB%s", a.memory / 1073741824.,
                                        a.active ? "  |  Active renderer" : "");
                }
                ImGui::TextDisabled("DXGI aliases are grouped by adapter LUID; distinct devices keep their own index.");
                ImGui::TextWrapped("Select at launch: AtomX.exe --adapter N");
                heading("MEMORY BUDGET");
                ImGui::InputInt("Preview atoms", &budget, 100000, 1000000);
                budget = std::clamp(budget, 1000, 20000000);
                ImGui::TextWrapped(
                    "Files larger than this budget use deterministic stride sampling. All records "
                    "are scanned; only preview atoms are retained.");
                if (ImGui::Button("Reload with budget", {-1, U(32)}))
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
    void rebuildFont() {
        if (!refreshFont) return;
        refreshFont = false;
        ImGui_ImplDX11_InvalidateDeviceObjects();
        auto &io = ImGui::GetIO();
        io.Fonts->Clear();
        const char *fonts[] = {"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf", "C:/Windows/Fonts/consola.ttf"};
        if (!io.Fonts->AddFontFromFileTTF(fonts[preferences.font], float(preferences.size)*uiScale))
            io.Fonts->AddFontDefault();
        headingFont = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeuib.ttf", float(preferences.size)*uiScale);
        ImGui_ImplDX11_CreateDeviceObjects();
    }
    void startDataExport(const std::filesystem::path &destination) {
        auto fmt = io::formats[exportFormat].id;
        bool range = exportRange && frames.size() > 1;
        int first = range ? exportFirst : current, last = range ? exportLast : current,
            step = range ? exportStep : 1;
        if (first < 0 || last < first || (range && last >= int(frames.size())) || step < 1)
            throw std::runtime_error("Invalid export frame range");
        bool sequence = range && (exportSequence || !io::info(fmt).trajectory);
        if (!path.empty() && std::filesystem::exists(destination) &&
            std::filesystem::equivalent(destination, path))
            throw std::runtime_error("Choose a destination different from the input file");
        auto ext = lowerExtension(destination);
        bool valid = ext == std::string(".") + io::info(fmt).extension;
        if (fmt == io::Format::POSCAR)
            valid = isPOSCAR(destination);
        if (fmt == io::Format::XYZ)
            valid = ext == ".xyz" || ext == ".extxyz";
        if (fmt == io::Format::LammpsData)
            valid = ext == ".data" || ext == ".lmp";
        if (fmt == io::Format::LammpsDump)
            valid = ext == ".dump" || ext == ".lammpstrj";
        if (!valid)
            throw std::runtime_error(
                "Filename extension does not match the selected export format");
        exportCancel = false;
        exportProgress = 0;
        exportJob = std::async(std::launch::async, [this, destination, fmt, range, sequence, first,
                                                    last, step, snapshot = result.data,
                                                    sourcePath = path, frameIndex = frames,
                                                    pipeline = modifierGraph, options = exportOptions,
                                                    atomBudget = budget,
                                                    allowPreview = exportPreview]() {
            std::vector<std::pair<std::filesystem::path, std::filesystem::path>> staged;
            auto nonce =
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
            auto cleanup = [&]() {
                for (const auto &[temp, target] : staged) {
                    std::error_code ec;
                    std::filesystem::remove(temp, ec);
                }
            };
            try {
                int total = (last - first) / step + 1;
                for (int i = 0; i < (sequence ? total : 1); ++i) {
                    auto target = destination;
                    if (sequence) {
                        std::wostringstream n;
                        n << destination.stem().wstring() << L'_' << std::setw(6)
                          << std::setfill(L'0') << first + i * step
                          << destination.extension().wstring();
                        target = destination.parent_path() / n.str();
                        if (std::filesystem::exists(target))
                            throw std::runtime_error("Sequence target already exists: " +
                                                     utf8(target.wstring()));
                    }
                    auto temp = target;
                    temp += L".atomx-" + std::wstring(nonce.begin(), nonce.end()) + L".tmp";
                    staged.emplace_back(temp, target);
                }
                std::ofstream single;
                if (!sequence) {
                    single.open(staged[0].first, std::ios::binary);
                    if (!single)
                        throw std::runtime_error("Cannot create export file");
                }
                for (int i = 0; i < total; ++i) {
                    io::checkpoint(&exportCancel);
                    int frame = first + i * step;
                    Dataset data =
                        range ? evaluate(io::read(sourcePath, frameIndex.at(frame),
                                                  uint64_t(atomBudget), nullptr, &exportCancel),
                                         pipeline)
                                    .data
                              : snapshot;
                    if (data.sampled() && !allowPreview)
                        throw std::runtime_error(
                            "A frame exceeds the full-data budget; reload with a larger budget or "
                            "explicitly export a sampled preview");
                    if (sequence) {
                        std::ofstream file(staged[i].first, std::ios::binary);
                        if (!file)
                            throw std::runtime_error("Cannot create sequence file");
                        io::writeFrame(file, fmt, data, options, frame);
                        file.flush();
                        if (!file)
                            throw std::runtime_error("Sequence write failed");
                    } else
                        io::writeFrame(single, fmt, data, options, frame);
                    exportProgress = float(i + 1) / total;
                }
                if (!sequence) {
                    single.flush();
                    if (!single)
                        throw std::runtime_error("Export write failed");
                    single.close();
                }
                io::checkpoint(&exportCancel);
                for (const auto &[temp, target] : staged)
                    if (!MoveFileExW(temp.c_str(), target.c_str(),
                                     sequence ? MOVEFILE_WRITE_THROUGH
                                              : MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                        throw std::runtime_error(
                            "Could not publish export file; check destination permissions");
                return std::string("Exported ") + std::to_string(total) + " frame(s) to " +
                       utf8(destination.wstring());
            } catch (...) {
                cleanup();
                throw;
            }
        });
        exporting = true;
    }
    void dataExportDialog() {
        if (showDataExport) {
            ImGui::OpenPopup("Data export settings");
            showDataExport = false;
        }
        ImGui::SetNextWindowSize({U(550), 0}, ImGuiCond_Appearing);
        if (!ImGui::BeginPopupModal("Data export settings", nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize))
            return;
        if (ImGui::BeginCombo("Format", io::formats[exportFormat].name)) {
            for (int i = 0; i < int(std::size(io::formats)); ++i)
                if (io::formats[i].writable &&
                    ImGui::Selectable(io::formats[i].name, exportFormat == i))
                    exportFormat = i;
            ImGui::EndCombo();
        }
        auto fmt = io::formats[exportFormat].id;
        ImGui::SeparatorText("Frame sequence");
        ImGui::BeginDisabled(frames.size() < 2);
        ImGui::Checkbox("Export frame range", &exportRange);
        ImGui::EndDisabled();
        if (exportRange && frames.size() > 1) {
            ImGui::InputInt("First frame", &exportFirst);
            ImGui::InputInt("Last frame", &exportLast);
            ImGui::InputInt("Every Nth frame", &exportStep);
            if (io::info(fmt).trajectory)
                ImGui::Checkbox("Separate file per frame", &exportSequence);
            else
                ImGui::TextDisabled("This format exports one file per frame.");
            ImGui::TextDisabled("Sequence naming: name_000000.ext");
        } else
            ImGui::Text("Current frame: %d", current);
        ImGui::SeparatorText("Format options");
        if (fmt != io::Format::GRO)
            ImGui::SliderInt("Numeric precision", &exportOptions.precision, 1, 17);
        else
            ImGui::TextWrapped("GRO uses nm, fixed-width 3 decimal coordinates, and at most 99999 "
                               "atoms. Residues are exported as MOL; topology is not retained.");
        if (fmt == io::Format::POSCAR) {
            ImGui::Checkbox("Fractional coordinates (Direct)", &exportOptions.fractionalPOSCAR);
            if (result.data.vectorProperties.count("MoveMask"))
                ImGui::Checkbox("Preserve selective dynamics", &exportOptions.constraints);
            ImGui::TextWrapped("Particle positions are grouped by type. POSCAR does not preserve "
                               "arbitrary properties or non-periodic flags.");
        }
        if (fmt == io::Format::CIF)
            ImGui::TextWrapped("P1 structure; cell lengths/angles and fractional positions. The "
                               "reader uses the conventional cell orientation.");
        if (fmt == io::Format::LammpsData)
            ImGui::TextWrapped("Atomic style; sequential IDs; no masses or bonds. Boundary flags "
                               "are not stored by this format.");
        if (fmt == io::Format::LammpsDump)
            ImGui::TextWrapped("id, type, element, x, y, z; sequential IDs per frame. Restricted "
                               "triclinic cells and periodic flags are preserved.");
        if (fmt == io::Format::XYZ) {
            ImGui::Checkbox("Extended XYZ (cell / PBC / properties)", &exportOptions.extendedXYZ);
            if (exportOptions.extendedXYZ) {
                ImGui::TextDisabled("Species and XYZ coordinates are required.");
                auto property = [&](const std::string &name, std::vector<std::string> &selected) {
                    bool on = std::find(selected.begin(), selected.end(), name) != selected.end();
                    if (ImGui::Checkbox(name.c_str(), &on)) {
                        if (on)
                            selected.push_back(name);
                        else
                            selected.erase(std::remove(selected.begin(), selected.end(), name),
                                           selected.end());
                    }
                };
                if (!result.data.scalarProperties.empty() ||
                    !result.data.vectorProperties.empty()) {
                    ImGui::BeginChild("Export properties", {0, U(110)}, ImGuiChildFlags_Borders);
                    for (const auto &[name, v] : result.data.scalarProperties)
                        if (name != "type")
                            property(name, exportOptions.scalarProperties);
                    for (const auto &[name, v] : result.data.vectorProperties)
                        property(name, exportOptions.vectorProperties);
                    ImGui::EndChild();
                }
            } else
                ImGui::TextWrapped("Basic XYZ contains only species and positions; cell and "
                                   "additional properties are omitted.");
        }
        if (source.sampled()) {
            ImGui::Separator();
            ImGui::Checkbox("Export sampled preview only", &exportPreview);
            ImGui::TextWrapped("The loaded data is sampled. Increase the import budget and reload "
                               "to export full data.");
        }
        ImGui::BeginDisabled(source.sampled() && !exportPreview);
        if (ImGui::Button("Choose file and export")) {
            try {
                std::string e = io::info(fmt).extension;
                std::wstring ext(e.begin(), e.end());
                std::wstring exportFilter = L"Selected format";
                exportFilter.push_back(0);
                exportFilter += L"*." + ext;
                exportFilter.push_back(0);
                exportFilter.push_back(0);
                auto p = dialog(window, true, exportFilter.c_str(), ext.c_str());
                if (!p.empty()) {
                    startDataExport(p);
                    ImGui::CloseCurrentPopup();
                }
            } catch (const std::exception &e) {
                error = e.what();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    void settings() {
        if (showSettings) { ImGui::OpenPopup("Settings"); showSettings = false; }
        ImGui::SetNextWindowSize({U(540),U(preferences.size >= 19 ? 480.f : 410.f)});
        if (ImGui::BeginPopupModal("Settings", nullptr, ImGuiWindowFlags_NoResize)) {
            heading("APPEARANCE");
            bool changed = ImGui::Combo("Theme", &preferences.theme, "Slate dark\0Classic light\0Midnight\0");
            bool fontChanged = ImGui::Combo("Font", &preferences.font, "Segoe UI\0Arial\0Consolas\0");
            fontChanged |= ImGui::SliderInt("Font size", &preferences.size, 14,20,"%d px");
            if (changed || fontChanged) {
                theme(preferences.theme);
                refreshFont |= fontChanged;
                preferences.save();
            }
            ImGui::TextWrapped("Appearance is applied immediately and saved for the next launch.");
            heading("WINDOW CONTROLS");
            ImGui::BulletText("Minimize: collect in the taskbar");
            ImGui::BulletText("X: keep running in the system tray");
            ImGui::BulletText("Power: exit the application completely");
            ImGui::TextWrapped("Double-click the tray logo to restore. Right-click for Restore / Exit.");
            if (ImGui::Button("Done",{U(100),U(30)})) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    void catalog() {
        if (showCatalog) {
            ImGui::OpenPopup("Add modification");
            showCatalog = false;
        }
        auto display = ImGui::GetIO().DisplaySize;
        float width = std::min(U(1360), display.x - U(24));
        ImGui::SetNextWindowPos({std::max(U(12),catalogAnchor.x-width),catalogAnchor.y}, ImGuiCond_Appearing);
        ImGui::SetNextWindowSize({width,std::min(U(790),display.y-catalogAnchor.y-U(36))}, ImGuiCond_Appearing);
        if (ImGui::BeginPopup("Add modification", ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##search", "Search modifications...", modifierSearch, sizeof(modifierSearch));
            auto matches = [&](const char *name) {
                std::string a=name, b=modifierSearch;
                for (auto &c:a) c=char(std::tolower((unsigned char)c));
                for (auto &c:b) c=char(std::tolower((unsigned char)c));
                return a.find(b)!=std::string::npos;
            };
            auto operation = [&](Op op, const char *hint) {
                if (!matches(opName(op))) return;
                if (ImGui::Selectable(opName(op))) { add(op); ImGui::CloseCurrentPopup(); }
                recordUiTestItem(std::string("catalog.")+opName(op));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s",hint);
            };
            auto planned = [&](const char *name) {
                if (!matches(name)) return;
                ImGui::BeginDisabled();
                ImGui::Selectable(name);
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Not implemented. This entry does not execute an algorithm.");
            };
            auto beginCard = [&](const char *title) {
                ImGui::BeginChild(title,{0,0},ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                                  ImGuiWindowFlags_NoScrollbar);
                heading(title);
            };
            auto endCard = [&] { ImGui::EndChild(); };
            if (ImGui::BeginTable("Modifier categories",4,ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX)) {
                ImGui::TableNextColumn();
                beginCard("Analysis");
                for (auto name : {"Atomic strain", "Bader charge integration", "Bond order"}) planned(name);
                operation(Op::BondLengthDistribution,"Build a bond-length histogram from current explicit topology, resolving periodic image shifts.");
                operation(Op::BondAngleDistribution,"Build a 0–180 degree histogram from pairs of incident bonds, resolving periodic image shifts.");
                operation(Op::ClusterAnalysis,"Build connected components by a periodic cutoff or existing bond topology; adds per-particle Cluster IDs and a cluster-size table."); planned("Difference between frames");
                for (auto name : {"Displacement vectors", "Elastic strain calculation", "Find rings", "Grain segmentation"}) planned(name);
                operation(Op::Histogram,"Build a finite-value histogram data table for a particle scalar or position component.");
                operation(Op::RadialDistribution,"Compute a periodic 3D radial distribution table from the current pipeline data.");
                operation(Op::ReduceProperty,"Reduce a scalar particle property to a global minimum, maximum, mean, or sum.");
                operation(Op::CoordinationAnalysis,"Compute periodic neighbor counts and publish the Coordination particle property.");
                operation(Op::ScatterPlot,"Plot two upstream particle properties and export the finite data pairs as a table; large inputs are deterministically sampled.");
                for (auto name : {"Spatial binning", "Spatial correlation function", "Structure factor", "Time averaging", "Time series", "Voronoi analysis", "Wigner-Seitz defect analysis"}) planned(name);
                endCard();
                ImGui::TableNextColumn();
                beginCard("Modification");
                operation(Op::AffineTransform,"Apply an invertible 3 x 4 affine matrix to particle positions, simulation-cell vectors and origin."); planned("Combine datasets");
                operation(Op::ComputeProperty,"Evaluate a scalar expression for every particle and publish the named property.");
                operation(Op::Delete,"Remove selected particles.");
                operation(Op::EditCell,"Edit cell origin, vectors and periodic boundaries. Particle coordinates stay fixed unless fractional-coordinate remapping is explicitly enabled."); operation(Op::EditType,"Edit particle type assignments.");
                for (auto name : {"Freeze property", "Load trajectory", "Python script (deferred)"}) planned(name);
                operation(Op::RemoveProperty,"Remove a scalar or vector particle property by name.");
                operation(Op::Replicate,"Repeat the system along a cell vector.");
                operation(Op::Slice,"Keep particles below the chosen coordinate plane.");
                for (auto name : {"Smooth trajectory", "Unwrap trajectories"}) planned(name);
                operation(Op::Wrap,"Wrap positions into orthogonal or triclinic periodic cells, including partially periodic cells.");
                operation(Op::Translate,"Translate particle positions along an axis.");
                operation(Op::Scale,"Scale positions and simulation cell uniformly.");
                operation(Op::Rotate,"Rotate positions and cell vectors around an axis (degrees).");
                endCard();
                ImGui::TableNextColumn();
                beginCard("Structure identification");
                for (auto name : {"Ackland-Jones analysis", "Centrosymmetry parameter", "Chill+"}) planned(name);
                operation(Op::CommonNeighborAnalysis,"Fixed-cutoff Honeycutt–Andersen pair signatures; FCC and BCC are verified against periodic reference crystals. Unsupported or ambiguous motifs remain Other.");
                for (auto name : {"Identify diamond structure", "Polyhedral template matching", "VoroTop analysis"}) planned(name);
                endCard();
                beginCard("Visualization");
                for (auto name : {"Add text labels", "Construct surface mesh", "Coordination polyhedra"}) planned(name);
                operation(Op::CreateBonds,"Create and GPU-render cutoff neighbor bonds as screen-space lines or 3D cylinders with periodic image shifts.");
                for (auto name : {"Create isosurface", "Generate trajectory lines"}) planned(name);
                endCard();
                beginCard("Modifier templates");
                planned("Manage templates...");
                endCard();
                ImGui::TableNextColumn();
                beginCard("Selection");
                operation(Op::Clear,"Clear the current selection.");
                operation(Op::ExpandSelection,"Expand the current selection through cutoff-neighbor shells.");
                operation(Op::ExpressionSelect,"Select particles using the safe native scalar expression language.");
                operation(Op::SelectOverlapping,"Select overlapping pairs using either a distance cutoff or the sum of a scalar radius property, with periodic minimum-image distances.");
                operation(Op::Invert,"Invert selected and unselected particles.");
                operation(Op::ManualSelection,"Select particles in the Particles table; Ctrl-click toggles rows.");
                operation(Op::SelectType,"Select particles of a specified type.");
                operation(Op::SelectRange,"Select particles in a coordinate interval.");
                endCard();
                beginCard("Python modifiers");
                for (auto name : {"Assign shared visual element", "Calculate local entropy", "Identify fcc planar faults", "Render LAMMPS regions", "Shrink-wrap simulation box"}) planned(name);
                endCard();
                beginCard("Coloring");
                planned("Ambient occlusion");
                operation(Op::AssignColor,"Assign a solid color to the selected particles; if there is no selection, apply it to all particles.");
                operation(Op::ColorType,"Use the particle-type color palette.");
                operation(Op::ColorCoding,"Map a particle property to a color gradient.");
                endCard();
                ImGui::EndTable();
            }
            ImGui::Separator();
            ImGui::TextWrapped("Muted items are in development. All available tools are unrestricted.");
            ImGui::EndPopup();
        }
    }
    void ui() {
        poll();
        editCheckpoint.beginFrame(ImGui::IsMouseDown(ImGuiMouseButton_Left));
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
        if (showWorkspace) left(h);
        right(w, h);
        center(w, h);
        fixed("Status", 0, h - U(30), w, U(30));
        ImGui::SetCursorPosY(U(5));
        ImGui::TextColored(accent, "GPU");
        ImGui::SameLine();
        ImGui::TextDisabled("%s  |  %.1f FPS  |  %s drawn", utf8(gpu.adapterName).c_str(),
                            io.Framerate, number(result.data.atoms.size()).c_str());
        ImGui::SameLine();
        if (pipelineBusy) {
            ImGui::TextDisabled("Pipeline stage %zu / %zu", pipelineActiveNode.load(), pipelineJobNodeIds.size());
            ImGui::SameLine();
        }
        if (staleResult) {
            ImGui::TextColored({1.f,.62f,.18f,1.f}, "STALE RESULT");
            ImGui::SameLine();
        }
        ImGui::TextDisabled("  %s", status.c_str());
        ImGui::End();
        catalog();
        settings();
        dataExportDialog();
        if (animationSettings) {
            ImGui::OpenPopup("Animation settings");
            animationSettings = false;
        }
        if (ImGui::BeginPopupModal("Animation settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextDisabled("Trajectory playback");
            ImGui::SliderFloat("Frames/sec", &fps, 1.f, 60.f, "%.0f");
            ImGui::TextWrapped("Playback repeats the loaded trajectory. Use the timeline ruler or frame field to scrub.");
            if (ImGui::Button("Close", {U(110), 0})) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (busy && indexing)
            ImGui::OpenPopup("Loading trajectory");
        if (ImGui::BeginPopupModal("Loading trajectory", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Indexing / reading XYZ records...");
            ImGui::ProgressBar(progress, {U(400), U(20)});
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
        bool smokeCatalog = false, smokeSettings = false, smokeExport = false, desktopTest = false,
             smokeColorLegend = false, smokeBondPairs = false, smokeInspectorNode = false,
             smokeHistogram = false;
        std::filesystem::path input, shot;
        for (int i = 1; i < argc; i++) {
            std::wstring a = argv[i];
            if (a == L"--catalog") smokeCatalog = true;
            else if (a == L"--settings") smokeSettings = true;
            else if (a == L"--export-settings")
                smokeExport = true;
            else if (a == L"--smoke-color-legend") smokeColorLegend = true;
            else if (a == L"--smoke-bond-pairs") smokeBondPairs = true;
            else if (a == L"--smoke-inspector-node") smokeInspectorNode = true;
            else if (a == L"--smoke-histogram") smokeHistogram = true;
            else if (a == L"--desktop-test") desktopTest = true;
            else if (a == L"--adapter" && i + 1 < argc)
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
        wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(102));
        wc.hIconSm = wc.hIcon;
        RegisterClassExW(&wc);
        HWND window = CreateWindowW(wc.lpszClassName, L"AtomX - Atomic visualization workspace",
                                    WS_OVERLAPPEDWINDOW, 80, 50, 1560, 1000, nullptr, nullptr,
                                    instance, nullptr);
        uiScale = std::max(1.f,GetDpiForWindow(window)/96.f);
        MONITORINFO workArea{sizeof(workArea)};
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &workArea);
        int initialW = std::min(int(U(1560)), int(workArea.rcWork.right-workArea.rcWork.left-40));
        int initialH = std::min(int(U(1000)), int(workArea.rcWork.bottom-workArea.rcWork.top-40));
        SetWindowPos(window,nullptr,workArea.rcWork.left+20,workArea.rcWork.top+20,
                     initialW,initialH,SWP_NOZORDER | SWP_FRAMECHANGED);
        Renderer gpu;
        renderer = &gpu;
        gpu.init(window, adapter);
        ImGui::CreateContext();
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf", 17);
        uiScale = std::max(1.f, GetDpiForWindow(window)/96.f);
        theme();
        ImGui_ImplWin32_Init(window);
        ImGui_ImplDX11_Init(gpu.device.Get(), gpu.context.Get());
        SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        ShowWindow(window, SW_SHOWDEFAULT);
        if (smoke) ShowWindow(window, SW_SHOWNORMAL);
        DragAcceptFiles(window, TRUE);
        {
            App app(window, gpu);
            app.showCatalog = smokeCatalog;
            app.showSettings = smokeSettings;
            app.showDataExport = smokeExport;
            if (smokeInspectorNode) {
                app.showTable = true;
                app.add(Op::Translate);
            }
            if (smokeHistogram) {
                app.showTable = true;
                app.histogramPreviewTab = true;
                app.add(Op::Histogram);
            }
            if (smokeColorLegend) app.add(Op::ColorCoding);
            if (smokeBondPairs) {
                app.add(Op::CreateBonds);
                auto &bondNode=app.mods.back();
                const size_t typeCount=app.source.species.size();
                bondNode.bondTypeCutoffsEnabled=true;
                bondNode.bondTypeCutoffs.assign(typeCount*typeCount,bondNode.value);
                app.update(app.mods.size()-1);
            }
            if (desktopTest) {
                auto requireWindow = [](bool ok, const char *message) {
                    if (!ok) throw std::runtime_error(message);
                };
                ShowWindow(window, SW_MINIMIZE);
                requireWindow(IsIconic(window), "Minimize must use taskbar");
                ShowWindow(window, SW_RESTORE);
                ShowWindow(window, SW_MAXIMIZE);
                requireWindow(IsZoomed(window), "Maximize failed");
                SendMessageW(window, WM_CLOSE, 0, 0);
                requireWindow(desktop::inTray && !IsWindowVisible(window), "Close must hide to tray");
                desktop::restore(window);
                requireWindow(IsWindowVisible(window) && IsZoomed(window), "Tray restore must preserve maximized state");
                ShowWindow(window, SW_RESTORE);
                requireWindow(!IsIconic(window) && !IsZoomed(window), "Restore failed");
                RECT r{}; GetWindowRect(window, &r);
                requireWindow(desktop::hitTest(window, MAKELPARAM(r.left+100,r.top+20)) == HTCAPTION,
                              "Custom title must support native dragging");
                std::ofstream("build/desktop-test.txt") << "PASS: minimize, maximize, close-to-tray, restore, title drag hit test\n";
            }
            if (!input.empty())
                app.load(input);
            if (desktopTest && !input.empty()) {
                app.job.wait(); app.poll();
                if (app.frames.size() < 2) throw std::runtime_error("Timeline test needs at least two frames");
                app.seekFrame(1); app.poll();
                app.seekFrame(int(app.frames.size())+100); // Replace request while the prior frame loads.
                app.job.wait(); app.poll();
                if (app.busy) { app.job.wait(); app.poll(); }
                if (app.current != int(app.frames.size())-1 || !app.error.empty())
                    throw std::runtime_error("Queued trajectory seek failed");
                app.seekFrame(-10); app.poll(); app.job.wait(); app.poll();
                if (app.current != 0) throw std::runtime_error("First-frame seek failed");
                app.seekFrame(int(app.frames.size())-1); app.poll(); app.job.wait(); app.poll();
                std::ofstream("build/desktop-test.txt",std::ios::app)
                    << "PASS: trajectory seek, latest-request wins, first/last bounds\n";
            }
            bool done = false;
            int ticks = 0;
            bool inspectorSmokeStarted = false;
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
                if (IsIconic(window) || !IsWindowVisible(window)) {
                    app.poll();
                    Sleep(20);
                    continue;
                }
                if (resized) {
                    gpu.resize();
                    resized = false;
                }
                app.rebuildFont();
                ImGui_ImplDX11_NewFrame();
                ImGui_ImplWin32_NewFrame();
                ImGui::NewFrame();
                try {
                    app.ui();
                } catch (const std::exception &e) {
                    app.error = e.what();
                }
                if (smokeInspectorNode && !inspectorSmokeStarted && !app.busy &&
                    !app.indexing && !app.pipelineBusy) {
                    app.inspectPipelineNode(0);
                    inspectorSmokeStarted = true;
                }
                ImGui::Render();
                float clear[4] = {.15f, .17f, .2f, 1};
                gpu.context->OMSetRenderTargets(1, gpu.back.GetAddressOf(), nullptr);
                gpu.context->ClearRenderTargetView(gpu.back.Get(), clear);
                ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
                if (smoke && !app.busy && !app.pipelineBusy && !app.inspectorBusy &&
                    (!smokeInspectorNode || inspectorSmokeStarted) && ++ticks >= smoke) {
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
