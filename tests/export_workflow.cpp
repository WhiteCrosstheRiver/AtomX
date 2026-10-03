// Exercise the actual asynchronous application export path, including staging
// and publication. The GUI entry point is renamed; no interactive window opens.
#define wWinMain atomx_gui_entry_for_test
#include "../src/main.cpp"
#undef wWinMain
#include <iostream>
void requireExport(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
int main() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HWND window = CreateWindowExW(0, L"STATIC", L"Export validation", WS_OVERLAPPEDWINDOW, 0, 0,
                                  256, 256, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    auto dir = std::filesystem::temp_directory_path() /
               ("atomx-export-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(dir);
    try {
        Renderer testRenderer;
        testRenderer.init(window);
        ImGui::CreateContext();
        auto &guiIO=ImGui::GetIO();
        guiIO.DisplaySize={1560,1000};
        guiIO.DeltaTime=1.f/60.f;
        guiIO.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
        guiIO.Fonts->AddFontDefault();
        guiIO.Fonts->Build();
        {
            App app(window, testRenderer);
            if (app.pipelineJob.valid()) app.pipelineJob.wait();
            app.poll();
            // Appearance follows element identity across reordered/missing trajectory types.
            app.syncAppearance({"Si", "Ge"});
            testRenderer.styles[0].color = {.1f,.2f,.3f,1};
            testRenderer.styles[0].visual = {.73f,2,1,0};
            testRenderer.styles[0].axes = {1,2,3,0};
            const auto silicon = testRenderer.styles[0];
            app.appearanceType = 0;
            app.syncAppearance({"Ge", "Si"});
            requireExport(app.appearanceType == 1 && testRenderer.styles[1] == silicon,
                          "Appearance and selected element must survive reordering");
            app.syncAppearance({"Ge"});
            app.syncAppearance({"Si", "Ge"});
            requireExport(testRenderer.styles[0] == silicon,
                          "Absent element appearance must survive frame changes");
            app.setDefaultRadius("Si", testRenderer.styles[0], true);
            requireExport(testRenderer.styles[0].visual[0] == 0, "Radius inheritance");
            app.radius = 1.5f;
            app.setDefaultRadius("Si", testRenderer.styles[0], false);
            requireExport(testRenderer.styles[0] == silicon, "Custom radius must be remembered");
            app.syncAppearance({"Si", "Ge"});
            requireExport(testRenderer.styles[0] == silicon, "Repeated UI sync must retain edits");
            Dataset d;
            d.species = {"Cu"};
            d.cell = {5, 0, 0, 0, 5, 0, 0, 0, 5};
            d.atoms = {{1, 2, 3, 0}};
            d.sourceCount = 1;
            auto input = dir / L"输入.xyz";
            {
                std::ofstream f(input);
                for (int i = 0; i < 3; ++i) {
                    d.atoms[0].x = float(i);
                    io::writeFrame(f, io::Format::XYZ, d, {}, i);
                }
            }
            app.path = input;
            app.frames = io::index(input);
            app.source = io::read(input, app.frames[0]);
            app.mods = {{Op::Translate, true, 2, 0}};
            std::vector<Modifier> exportModifiers;
            for (const auto &node : app.mods) exportModifiers.push_back(static_cast<const Modifier &>(node));
            app.result = evaluate(app.source, exportModifiers);
            auto finish = [&]() {
                auto message = app.exportJob.get();
                app.exporting = false;
                requireExport(message.find("Exported") != std::string::npos, "export result");
            };
            app.exportRange = true;
            app.exportFirst = 0;
            app.exportLast = 2;
            app.exportStep = 2;
            auto output = dir / L"输出.xyz";
            app.startDataExport(output);
            finish();
            auto frames = io::index(output);
            auto result = io::read(output, frames[1]);
            requireExport(frames.size() == 2 && result.atoms[0].x == 4,
                          "range/stride export applies pipeline to each frame");
            app.exportFormat = 1; // POSCAR requires separate files for frame ranges.
            app.exportStep = 1;
            app.startDataExport(dir / L"序列.vasp");
            finish();
            requireExport(std::filesystem::exists(dir / L"序列_000002.vasp"),
                          "forced file sequence");
            auto p = dir / L"序列_000001.vasp";
            result = io::read(p, io::index(p)[0]);
            requireExport(result.atoms[0].x == 3, "sequence contents");
            app.exportRange = false;
            app.exportFormat = 0;
            bool failed = false;
            try {
                app.startDataExport(input);
            } catch (...) {
                failed = true;
            }
            requireExport(failed, "source overwrite rejected");
            failed = false;
            try {
                app.startDataExport(dir / "wrong.cif");
            } catch (...) {
                failed = true;
            }
            requireExport(failed, "extension mismatch rejected");
            app.result.data.stride = 2;
            app.exportPreview = false;
            app.startDataExport(output);
            failed = false;
            try {
                finish();
            } catch (...) {
                failed = true;
                app.exporting = false;
            }
            requireExport(failed, "sample export requires explicit choice");
            frames = io::index(output);
            requireExport(frames.size() == 2, "failed export preserves old destination");
            for (const auto &e : std::filesystem::directory_iterator(dir))
                requireExport(e.path().extension() != ".tmp", "staging files removed");
            if (app.pipelineJob.valid()) app.pipelineJob.wait();
            app.poll();
            app.pipelineCheckpoint.reset();
            app.pipelineCheckpointNode=SIZE_MAX;
            app.mods.clear();
            app.modifierGraph.selected=0;
            app.frames.clear();
            app.path.clear();
            app.source.species={"Cu"};
            app.source.cell={4,0,0,0,4,0,0,0,4};
            app.source.origin={};
            app.source.pbc={true,true,true};
            app.source.atoms={{1,1,1,0}};
            app.source.sourceCount=1;
            app.result=evaluate(app.source,std::vector<Modifier>{});
            app.error.clear();
            app.status="Ready";
            app.selectPipeline=true;
            app.refreshFont=false;
            app.captureUiTestItems=true;
            // The headless harness builds only the default font, so every
            // compact icon button renders its text fallback here. This pins
            // the graceful-degradation contract: identical recorded names,
            // rectangles and actions with and without the Segoe MDL2 icon
            // font (the real icon path is asserted by the smoke run through
            // the icon_font line of smoke-report.txt).
            requireExport(!app.usingIconFont(),
                          "icon font stays unloaded in the headless text-fallback harness");
            strcpy_s(app.modifierSearch,"Radial distribution function (RDF)");
            const ImGuiStyle baseStyle=ImGui::GetStyle();
            auto frame=[&]() {
                guiIO.DeltaTime=1.f/60.f;
                app.uiTestItems.clear();
                ImGui::NewFrame();
                app.ui();
                ImGui::Render();
            };
            auto settlePipeline=[&]() {
                for (int attempt=0; attempt<8 && app.pipelineBusy; ++attempt) {
                    if (app.pipelineJob.valid()) app.pipelineJob.wait();
                    frame();
                }
                requireExport(!app.pipelineBusy,"asynchronous pipeline did not settle after repeated cancellation/relaunch");
            };
            auto click=[&](const std::string &name,float xFraction=.5f) {
                auto found=app.uiTestItems.find(name);
                requireExport(found!=app.uiTestItems.end(),"required interactive UI control was not recorded");
                const auto item=found->second;
                requireExport(item.id!=0,"interactive control must expose a stable nonzero ImGui ID");
                requireExport(item.max.x>item.min.x && item.max.y>item.min.y,
                              ("interactive UI control has an empty rectangle: "+name).c_str());
                requireExport(item.min.x>=0 && item.min.y>=0 &&
                                  item.max.x<=guiIO.DisplaySize.x && item.max.y<=guiIO.DisplaySize.y,
                              ("interactive UI control exceeds application bounds: "+name).c_str());
                const ImVec2 point{item.min.x+(item.max.x-item.min.x)*xFraction,
                                   (item.min.y+item.max.y)*.5f};
                // Move first, press in a later frame: coalescing a jump with
                // the button press into one frame leaves the hover test one
                // frame behind, and a press hovering nothing parks ImGui's
                // click-ownership on no window (all subsequent hover clears).
                guiIO.AddMousePosEvent(point.x,point.y);
                frame();
                guiIO.AddMouseButtonEvent(0,true); frame();
                auto down=app.uiTestItems.find(name);
                if (down==app.uiTestItems.end() || !down->second.hovered || !down->second.clicked)
                    throw std::runtime_error("simulated pointer missed control: "+name+
                        " rect="+std::to_string(item.min.x)+","+std::to_string(item.min.y)+"-"+
                        std::to_string(item.max.x)+","+std::to_string(item.max.y)+
                        " hovered="+(down!=app.uiTestItems.end()&&down->second.hovered?"true":"false")+
                        " clicked="+(down!=app.uiTestItems.end()&&down->second.clicked?"true":"false"));
                guiIO.AddMouseButtonEvent(0,false); frame();
            };
            frame();
            app.pipelinePane = 268.f;
            app.propertiesPane = 316.f;
            app.workspacePane = 220.f;
            app.inspectorPane = 180.f;
            app.showTable = true;
            frame();
            requireExport(app.uiTestItems.contains("layout.inspector-splitter") &&
                          app.uiTestItems.contains("layout.timeline-splitter") &&
                          app.uiTestItems.contains("layout.Pipeline edge-splitter") &&
                          app.uiTestItems.contains("layout.Properties edge-splitter"),
                          "major normal-mode panels expose draggable splitters");
            for (const auto &name : {"layout.Pipeline edge-splitter",
                                     "layout.Properties edge-splitter"}) {
                const auto splitter = app.uiTestItems.at(name);
                const float x = (splitter.min.x + splitter.max.x) * .5f;
                const float y = (splitter.min.y + splitter.max.y) * .5f;
                float &pane = name == std::string("layout.Pipeline edge-splitter")
                    ? app.pipelinePane : app.propertiesPane;
                const float before = pane;
                guiIO.AddMousePosEvent(x,y); frame();
                guiIO.AddMouseButtonEvent(0,true); frame();
                guiIO.AddMousePosEvent(x+60.f,y); frame();
                if (!(std::abs(pane-before)>20.f)) {
                    const auto item=app.uiTestItems.at(name);
                    throw std::runtime_error(std::string("side-panel splitter drag failed: ")+name+
                        " rect="+std::to_string(splitter.min.x)+","+std::to_string(splitter.min.y)+
                        "-"+std::to_string(splitter.max.x)+","+std::to_string(splitter.max.y)+
                        " hovered="+(item.hovered?"yes":"no")+
                        " clicked="+(item.clicked?"yes":"no")+
                        " before="+std::to_string(before)+" after="+std::to_string(pane));
                }
                app.layoutDirty=false;
                guiIO.AddMouseButtonEvent(0,false); frame();
                pane=before;
                frame();
            }
            {
                const auto splitter = app.uiTestItems.at("layout.inspector-splitter");
                const float x = (splitter.min.x + splitter.max.x) * .5f;
                const float y = (splitter.min.y + splitter.max.y) * .5f;
                const float before = app.inspectorPane;
                guiIO.AddMousePosEvent(x, y); frame();
                guiIO.AddMouseButtonEvent(0, true); frame();
                guiIO.AddMousePosEvent(x, y + 80.f); frame();
                if (!(app.inspectorPane < before - 30.f)) {
                    const auto item=app.uiTestItems.at("layout.inspector-splitter");
                    throw std::runtime_error("inspector splitter drag failed: rect="+
                        std::to_string(splitter.min.x)+","+std::to_string(splitter.min.y)+"-"+
                        std::to_string(splitter.max.x)+","+std::to_string(splitter.max.y)+
                        " hovered="+(item.hovered?"yes":"no")+
                        " clicked="+(item.clicked?"yes":"no")+
                        " before="+std::to_string(before)+
                        " after="+std::to_string(app.inspectorPane));
                }
                app.layoutDirty = false;
                guiIO.AddMouseButtonEvent(0, false); frame();
                app.inspectorPane = 400.f;
                app.showTable = false;
                frame();
            }
            const int originalTab = app.activeTab;
            const size_t originalTabCount = app.tabs.size();
            const size_t originalAtomCount = app.result.data.atoms.size();
            click("toolbar.creation-mode");
            frame();
            requireExport(app.creationMode && app.tabs.size() == originalTabCount + 1 &&
                              app.activeTab != originalTab &&
                              app.source.atoms.size() == originalAtomCount &&
                              app.uiTestItems.contains("creation.return-view"),
                          "Creation Mode opens an editable copy in a separate workspace tab");
            settlePipeline();
            {
                const auto viewport=app.uiTestItems.at("creation.viewport");
                const float x=(viewport.min.x+viewport.max.x)*.5f;
                const float y=(viewport.min.y+viewport.max.y)*.5f;
                auto rightDrag=[&](bool alt,bool shift) {
                    guiIO.AddKeyEvent(ImGuiMod_Alt,alt);
                    guiIO.AddKeyEvent(ImGuiMod_Shift,shift);
                    guiIO.AddMousePosEvent(x,y); frame();
                    guiIO.AddMouseButtonEvent(1,true); frame();
                    requireExport(ImGui::GetCurrentContext()->OpenPopupStack.empty(),
                                  "right mouse down must not open a menu and interrupt dragging");
                    guiIO.AddMousePosEvent(x+40,y+25); frame();
                    guiIO.AddMouseButtonEvent(1,false); frame();
                    requireExport(ImGui::GetCurrentContext()->OpenPopupStack.empty(),
                                  "right drag release must not open a context menu");
                    guiIO.AddKeyEvent(ImGuiMod_Alt,false);
                    guiIO.AddKeyEvent(ImGuiMod_Shift,false); frame();
                };
                const float yaw=app.cameras[3].yaw;
                rightDrag(false,false);
                requireExport(std::abs(app.cameras[3].yaw-yaw)>.1f,"right drag rotates the creation camera");
                const float pan=app.cameras[3].panX;
                rightDrag(true,false);
                requireExport(std::abs(app.cameras[3].panX-pan)>.001f,"Alt right drag pans the camera");
                app.selectCreationAtom(0,false);
                const Atom original=app.source.atoms[0];
                const size_t undoCount=app.authorUndo.size();
                rightDrag(true,true); settlePipeline();
                requireExport(app.authorUndo.size()==undoCount+1 &&
                              (std::abs(app.source.atoms[0].x-original.x)>.001f ||
                               std::abs(app.source.atoms[0].y-original.y)>.001f),
                              "Shift Alt right drag moves selected atoms in one undo step");
                app.history(false); settlePipeline();
                requireExport(app.source.atoms[0].x==original.x && app.source.atoms[0].y==original.y &&
                              app.source.atoms[0].z==original.z,"undo restores the dragged position exactly");
                app.editStructure("rotation fixture",[](Dataset &data) {
                    auto atom=data.atoms[0]; atom.x+=1; data.atoms.push_back(atom);
                }); settlePipeline();
                app.creationSelection={0,1}; app.creationPick=0;
                const double separation=authoring::distance(app.source,0,1);
                const auto spinStart=app.source.atoms[1];
                const size_t spinUndo=app.authorUndo.size();
                rightDrag(false,true); settlePipeline();
                requireExport(app.authorUndo.size()==spinUndo+1 &&
                              std::abs(authoring::distance(app.source,0,1)-separation)<1e-5 &&
                              (std::abs(app.source.atoms[1].y-spinStart.y)>.001 ||
                               std::abs(app.source.atoms[1].z-spinStart.z)>.001),
                              "Shift right drag rotates a group rigidly in one history step");
                app.history(false); settlePipeline();
                app.history(false); settlePipeline();
                app.selectCreationAtom(0,false); frame();
                click("creation.edit-position");
                app.creationPositionFractional=true;
                app.creationPositionDraft[0]=.25f; app.creationPositionDraft[1]=.5f;
                app.creationPositionDraft[2]=.75f; frame();
                const size_t positionUndo=app.authorUndo.size();
                click("creation.position-apply"); settlePipeline();
                requireExport(app.authorUndo.size()==positionUndo+1 &&
                              app.source.atoms[0].x==1 && app.source.atoms[0].y==2 &&
                              app.source.atoms[0].z==3,
                              "coordinate dialog applies fractional coordinates as one undo step");
                app.history(false); settlePipeline();
                app.selectCreationAtom(0,false);
                app.selectCreationAtom(0,true);
                requireExport(app.creationSelection.size()==1,"Shift adds without deselecting an existing atom");
                app.selectCreationAtom(0,true,true);
                requireExport(app.creationSelection.empty(),"Ctrl toggles an existing atom off");
                guiIO.AddMousePosEvent(x,y); frame();
                guiIO.AddMouseButtonEvent(1,true); frame();
                guiIO.AddMouseButtonEvent(1,false); frame();
                requireExport(!ImGui::GetCurrentContext()->OpenPopupStack.empty(),
                              "stationary right click opens the context menu on release");
                guiIO.AddKeyEvent(ImGuiKey_Escape,true); frame();
                guiIO.AddKeyEvent(ImGuiKey_Escape,false); frame();
            }
            {
                app.editStructure("movement fixture",[](Dataset &data) {
                    auto atom=data.atoms[0]; atom.x+=2; data.atoms.push_back(atom);
                    atom.x+=2; data.atoms.push_back(atom);
                }); settlePipeline();
                app.creationSelection={0,1}; app.creationPick=0; frame();
                const auto a=app.source.atoms[0], b=app.source.atoms[1], outside=app.source.atoms[2];
                const auto crystal=app.source.cell;
                click("creation.edit-movement"); frame();
                requireExport(app.creationMovementAngle==45,"movement dialog defaults to the MS 45 degree step");
                click("creation.movement-world-axes");
                app.creationMovementDistance=1.25f; frame();
                const size_t undo=app.authorUndo.size();
                click("creation.movement-right"); settlePipeline();
                requireExport(app.authorUndo.size()==undo+1 && app.source.atoms[0].x==a.x+1.25f &&
                              app.source.atoms[1].x==b.x+1.25f && app.source.atoms[2].x==outside.x &&
                              app.source.cell==crystal,"precise group translation preserves unselected atoms and cell");
                app.creationMovementAngle=90; frame();
                click("creation.movement-rotate-z"); settlePipeline();
                requireExport(app.authorUndo.size()==undo+2 &&
                              std::abs(app.source.atoms[0].x-(a.x+2.25f))<1e-5 &&
                              std::abs(app.source.atoms[0].y-(a.y-1.f))<1e-5 &&
                              std::abs(app.source.atoms[1].y-(b.y+1.f))<1e-5 &&
                              app.source.atoms[2].x==outside.x,
                              "precise 90 degree rotation is about the selected group center");
                guiIO.AddKeyEvent(ImGuiMod_Ctrl,true);
                guiIO.AddKeyEvent(ImGuiKey_D,true); frame();
                guiIO.AddKeyEvent(ImGuiKey_D,false);
                guiIO.AddKeyEvent(ImGuiMod_Ctrl,false); frame();
                requireExport(app.creationSelection.size()==2,"modal editor owns shortcuts and preserves selection");
                click("creation.movement-close");
                app.history(false); settlePipeline();
                app.history(false); settlePipeline();
                requireExport(app.source.atoms[0].x==a.x && app.source.atoms[1].x==b.x &&
                              app.source.atoms[0].y==a.y,"precise transformation undo restores exact coordinates");
                app.creationSelection={0,1}; app.creationPick=0; frame();
                click("creation.edit-movement"); frame();
                click("creation.movement-screen-axes");
                app.creationMovementDistance=10; frame();
                click("creation.movement-percent");
                const float span=app.creationMovementScreenSpan;
                const auto screenAxis=app.creationMovementAxes[0];
                click("creation.movement-right"); settlePipeline();
                const auto moved=app.source.atoms[0];
                requireExport(span>0 && std::abs(moved.x-a.x-screenAxis.x*span*.1f)<1e-5 &&
                              std::abs(moved.y-a.y-screenAxis.y*span*.1f)<1e-5 &&
                              std::abs(moved.z-a.z-screenAxis.z*span*.1f)<1e-5,
                              "screen percentage movement follows the camera at the selected center depth");
                const auto depthAxis=app.creationMovementAxes[2];
                click("creation.movement-in"); settlePipeline();
                const auto inward=app.source.atoms[0];
                requireExport(std::abs(inward.x-moved.x+depthAxis.x*span*.1f)<1e-5 &&
                              std::abs(inward.y-moved.y+depthAxis.y*span*.1f)<1e-5 &&
                              std::abs(inward.z-moved.z+depthAxis.z*span*.1f)<1e-5,
                              "inward screen movement travels away from the viewer in a right handed camera");
                click("creation.movement-out"); settlePipeline();
                requireExport(std::abs(app.source.atoms[0].x-moved.x)<1e-5 &&
                              std::abs(app.source.atoms[0].y-moved.y)<1e-5 &&
                              std::abs(app.source.atoms[0].z-moved.z)<1e-5,
                              "outward screen movement reverses inward movement");
                app.creationMovementDistance=NAN; frame();
                const size_t beforeInvalid=app.authorUndo.size();
                const auto disabledMove=app.uiTestItems.at("creation.movement-right");
                guiIO.AddMousePosEvent((disabledMove.min.x+disabledMove.max.x)*.5f,
                                      (disabledMove.min.y+disabledMove.max.y)*.5f); frame();
                guiIO.AddMouseButtonEvent(0,true); frame();
                guiIO.AddMouseButtonEvent(0,false); frame();
                requireExport(app.authorUndo.size()==beforeInvalid,"invalid movement step is disabled without history");
                click("creation.movement-close");
                app.history(false); settlePipeline();
                app.history(false); settlePipeline();
                app.history(false); settlePipeline();
                app.history(false); settlePipeline();
                requireExport(app.source.atoms.size()==originalAtomCount,"movement fixture cleanup restores original structure");
            }
            {
                const size_t baseline=app.authorUndo.size();
                app.editStructure("sketch fixture",[](Dataset &data) {
                    data.species={"C"}; data.pbc={false,false,false};
                    data.cell={10,0,0,0,10,0,0,0,10};
                    data.atoms={{1,1,1,0},{4,1,1,0},{7,1,1,0}};
                    data.bonds.clear();
                }); settlePipeline();
                app.cameras[3].mode=2; app.fitCamera(3,false);
                strcpy_s(app.creationElement,"C");
                app.chooseCreationTool(App::CreationTool::Sketch); frame();
                auto pointOf=[&](int index) {
                    const auto viewport=app.uiTestItems.at("creation.viewport");
                    const ImVec2 size{viewport.max.x-viewport.min.x,viewport.max.y-viewport.min.y};
                    const auto matrix=app.creationProjection(app.result.data,app.cameras[3],size).combined;
                    const auto &a=app.source.atoms[size_t(index)];
                    DirectX::XMFLOAT4 q;
                    DirectX::XMStoreFloat4(&q,DirectX::XMVector4Transform(
                        DirectX::XMVectorSet(a.x,a.y,a.z,1),matrix));
                    return ImVec2{viewport.min.x+(q.x/q.w+1)*size.x*.5f,
                                  viewport.min.y+(1-q.y/q.w)*size.y*.5f};
                };
                auto sketchClick=[&](ImVec2 point) {
                    guiIO.AddMousePosEvent(point.x,point.y); frame();
                    guiIO.AddMouseButtonEvent(0,true); frame();
                    guiIO.AddMouseButtonEvent(0,false); frame();
                    settlePipeline();
                };
                const size_t sketchBase=app.authorUndo.size();
                sketchClick(pointOf(0));
                requireExport(app.creationSketchAnchor==0 && app.authorUndo.size()==sketchBase,
                              "click existing atom starts sketch without a geometry history step");
                const auto anchor=pointOf(0);
                guiIO.AddMousePosEvent(anchor.x+20,anchor.y+35); frame();
                requireExport(app.creationSketchPreviewValid && app.source.atoms.size()==3 &&
                              std::abs(authoring::length(authoring::sub(app.creationSketchPreview,{1,1,1}))-1.52)<1e-4,
                              "virtual atom follows cursor on covalent-radius sphere without editing source");
                app.creationSketchOrder=2;
                sketchClick(pointOf(1));
                requireExport(app.source.atoms.size()==3 && app.source.bonds.size()==1 &&
                              app.source.bonds[0].order==2 && app.creationSketchAnchor==1 &&
                              app.authorUndo.size()==sketchBase+1,
                              "sketch snaps to an existing atom and creates one double bond/history step");
                // The second click on the same atom completes the chain.
                sketchClick(pointOf(1));
                requireExport(app.creationSketchAnchor<0 && app.source.atoms.size()==3 &&
                              app.authorUndo.size()==sketchBase+1,
                              "double-click finish does not create an overlapping atom or extra history");
                app.creationSelection={0,1}; app.creationPick=1; frame();
                click("creation.bond-order-3"); settlePipeline();
                requireExport(app.source.bonds.size()==1 && app.source.bonds[0].order==3,
                              "selected pair bond-order buttons edit existing topology");
                app.history(false); settlePipeline();
                requireExport(app.source.bonds[0].order==2,"undo restores previous bond order");
                app.chooseCreationTool(App::CreationTool::Sketch); frame();
                const auto bondStart=pointOf(0),bondEnd=pointOf(1);
                sketchClick({(bondStart.x+bondEnd.x)*.5f,(bondStart.y+bondEnd.y)*.5f});
                requireExport(app.source.bonds.size()==1 && app.source.bonds[0].order==3 &&
                              app.source.atoms.size()==3 && app.creationSketchAnchor<0,
                              "click bond cycles its order without starting a chain or adding an atom");
                app.history(false); settlePipeline();
                strcpy_s(app.creationElement,"O");
                guiIO.AddKeyEvent(ImGuiMod_Alt,true); frame();
                sketchClick(pointOf(2));
                guiIO.AddKeyEvent(ImGuiMod_Alt,false); frame();
                requireExport(app.source.species[app.source.atoms[2].type]=="O" &&
                              app.source.atoms.size()==3 && app.creationSketchAnchor<0,
                              "Alt click existing atom changes its element without extending the chain");
                app.history(false); settlePipeline();
                strcpy_s(app.creationElement,"C");
                app.creationSketchContinuous=false;
                sketchClick(pointOf(0));
                requireExport(app.creationSketchAnchor==0,"single-step sketch can still start from an existing atom");
                sketchClick(pointOf(2));
                requireExport(app.creationSketchAnchor<0 && app.source.bonds.size()==2,
                              "disabling continuous sketch finishes after one new bond");
                app.history(false); settlePipeline(); app.creationSketchContinuous=true;
                app.cell=false; app.fitCamera(3,false); frame();
                // Let the previous double-click interval elapse before restarting at atom #1.
                for (int i=0;i<24;++i) frame();
                sketchClick(pointOf(1));
                const auto start=pointOf(1);
                sketchClick({start.x+40,start.y+55});
                requireExport(app.source.atoms.size()==4 && app.source.bonds.size()==2 &&
                              app.creationSketchAnchor==3 && app.authorUndo.size()==sketchBase+2,
                              "continuous sketch adds an atom and its bond in one history step");
                sketchClick({start.x+40,start.y+55});
                requireExport(app.creationSketchAnchor<0 && app.source.atoms.size()==4 &&
                              app.authorUndo.size()==sketchBase+2,
                              "double click at old cursor position finishes even after automatic camera bounds change");
                for (int i=0;i<24;++i) frame();
                sketchClick(pointOf(2));
                requireExport(app.creationSketchAnchor==2,"sketch can restart after double-click finish");
                guiIO.AddKeyEvent(ImGuiKey_Escape,true); frame();
                guiIO.AddKeyEvent(ImGuiKey_Escape,false); frame();
                requireExport(app.creationSketchAnchor<0 && !app.creationSketchPreviewValid &&
                              app.source.atoms.size()==4,"Escape discards only the virtual sketch atom");
                app.history(false); settlePipeline();
                requireExport(app.source.atoms.size()==3 && app.source.bonds.size()==1,
                              "undo removes the new atom and its incident bond together");
                while (app.authorUndo.size()>baseline) { app.history(false); settlePipeline(); }
                app.authorRedo.clear(); app.chooseCreationTool(App::CreationTool::Select);
                app.cell=true;
                app.cameras[3].mode=7; app.fitCamera(3,false); frame();
            }
            {
                const size_t baseline=app.authorUndo.size();
                app.editStructure("ring fixture",[](Dataset &data) {
                    data.species={"C"}; data.pbc={false,false,false};
                    data.cell={10,0,0,0,10,0,0,0,10};
                    data.atoms={{2,2,2,0},{3.52f,2,2,0}}; data.bonds={{0,1,{}}};
                    data.scalarProperties.clear(); data.vectorProperties.clear(); data.particleColors.clear();
                }); settlePipeline();
                app.cameras[3].mode=2; app.cell=true; app.fitCamera(3,false); frame();
                click("creation.tool-ring"); frame();
                requireExport(app.creationTool==App::CreationTool::Ring && app.uiTestItems.contains("creation.ring-size-4"),
                              "ring toolbar opens size controls in the existing creation view");
                auto pointOf=[&](Vec3 a) {
                    const auto vp=app.uiTestItems.at("creation.viewport");
                    const ImVec2 size{vp.max.x-vp.min.x,vp.max.y-vp.min.y};
                    const auto matrix=app.creationProjection(app.result.data,app.cameras[3],size).combined;
                    DirectX::XMFLOAT4 q;
                    DirectX::XMStoreFloat4(&q,DirectX::XMVector4Transform(DirectX::XMVectorSet(a.x,a.y,a.z,1),matrix));
                    return ImVec2{vp.min.x+(q.x/q.w+1)*size.x*.5f,vp.min.y+(1-q.y/q.w)*size.y*.5f};
                };
                auto pointerClick=[&](ImVec2 at) {
                    guiIO.AddMousePosEvent(at.x,at.y); frame();
                    guiIO.AddMouseButtonEvent(0,true); frame();
                    guiIO.AddMouseButtonEvent(0,false); frame(); settlePipeline();
                };
                const size_t start=app.authorUndo.size();
                auto blank=pointOf({6,5,2});
                guiIO.AddMousePosEvent(blank.x,blank.y); frame();
                const auto topology=app.source.bonds;
                requireExport(app.creationRingPreviewValid && app.source.atoms.size()==2,"ring follows mouse as an uncommitted preview");
                for (int i=0;i<6;++i) frame();
                requireExport(app.authorUndo.size()==start && app.source.bonds==topology,"idle ring preview keeps source topology and history untouched");
                pointerClick(blank);
                requireExport(app.source.atoms.size()==8 && app.source.bonds.size()==7 && app.authorUndo.size()==start+1 && app.creationSelection.size()==6,
                              "isolated six member ring adds six atoms and six bonds in one history step");
                app.history(false); settlePipeline();
                requireExport(app.source.atoms.size()==2 && app.source.bonds.size()==1,"one undo removes the whole ring");
                click("creation.ring-size-4"); frame();
                auto anchor=pointOf({2,2,2});
                guiIO.AddMousePosEvent(anchor.x,anchor.y); frame(); guiIO.AddMouseButtonEvent(0,true); frame();
                guiIO.AddMousePosEvent(anchor.x+35,anchor.y+20); frame();
                requireExport(app.creationDrag==App::CreationDrag::Ring && app.creationRingPreviewValid && app.source.atoms.size()==2 &&
                              std::abs(app.creationRingPreview.points[1].z-app.creationRingStart.points[1].z)>.01,
                              "held atom attached ring rotates its preview without mutating the source");
                guiIO.AddKeyEvent(ImGuiKey_Escape,true); frame(); guiIO.AddKeyEvent(ImGuiKey_Escape,false); frame();
                guiIO.AddMouseButtonEvent(0,false); frame();
                requireExport(app.source.atoms.size()==2 && app.authorUndo.size()==start,"Escape cancels ring placement with no history step");
                pointerClick(pointOf({2,2,2}));
                requireExport(app.source.atoms.size()==5 && app.source.bonds.size()==5 && app.creationSelection[0]==0,
                              "four member ring placed on existing atom shares its anchor");
                app.history(false); settlePipeline();
                click("creation.ring-size-6"); frame();
                guiIO.AddKeyEvent(ImGuiMod_Alt,true); frame();
                pointerClick(pointOf({2.76f,2,2}));
                guiIO.AddKeyEvent(ImGuiMod_Alt,false); frame();
                requireExport(app.source.atoms.size()==6 && app.source.bonds.size()==6 &&
                              std::all_of(app.source.bonds.begin(),app.source.bonds.end(),[](const Bond &b){return b.order==4;}),
                              "Alt bond placement shares two endpoints and creates explicit aromatic ring topology");
                const auto ringFile=dir/"gui-ring.atomx";
                io::ExportOptions ringOptions; ringOptions.documentView=app.captureDocumentView();
                io::write(ringFile,io::Format::AtomX,app.source,ringOptions);
                const auto restored=document::read(ringFile);
                requireExport(restored.data.bonds==app.source.bonds && restored.view.tool==5 && restored.view.ringSize==6,
                              "ring topology and active tool persist in the native document");
                const auto saved=app.captureTab(); app.creationRingSize=4; app.restoreTab(saved);
                requireExport(app.creationRingSize==6 && !app.creationRingPreviewValid && app.creationDrag==App::CreationDrag::None,
                              "tab restoration restores its ring size and discards the old provisional gesture");
                settlePipeline();
                while (app.authorUndo.size()>baseline) { app.history(false); settlePipeline(); }
                app.authorRedo.clear(); app.chooseCreationTool(App::CreationTool::Select); app.cameras[3].mode=7; frame();
            }
            {
                const size_t baseline=app.authorUndo.size();
                app.editStructure("display style fixture",[](Dataset &data) {
                    data={}; data.species={"C","O"};
                    data.atoms={{-2,0,0,0},{0,0,0,1},{2,0,0,0}}; data.bonds={{0,1,{},1},{1,2,{},1}};
                }); settlePipeline(); app.creationSelection={0}; app.creationPick=0; frame();
                const auto topology=app.source.bonds; const auto styles=app.gpu.styles;
                auto stylePixels=[&]() {
                    const auto &target=app.targets[3]; D3D11_TEXTURE2D_DESC desc{}; target.texture->GetDesc(&desc);
                    desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                    ComPtr<ID3D11Texture2D> copy; check(app.gpu.device->CreateTexture2D(&desc,nullptr,&copy),"Style history readback");
                    app.gpu.context->CopyResource(copy.Get(),target.texture.Get());
                    D3D11_MAPPED_SUBRESOURCE map{}; check(app.gpu.context->Map(copy.Get(),0,D3D11_MAP_READ,0,&map),"Style history pixels");
                    uint64_t hash=1469598103934665603ULL;
                    for(int y=0;y<target.h;++y) for(int x=0;x<target.w*4;++x) {
                        hash^=static_cast<const uint8_t *>(map.pData)[y*map.RowPitch+x]; hash*=1099511628211ULL;
                    }
                    app.gpu.context->Unmap(copy.Get(),0); return hash;
                };
                const auto originalPixels=stylePixels();
                click("creation.edit-styles"); frame();
                requireExport(!app.creationStyleAll,"style dialog defaults to current selection");
                click("creation.style-preset-4"); click("creation.style-apply");
                requireExport(app.creationDisplay.presetAt(0)==4 && app.creationDisplay.presetAt(1)==0 &&
                              !app.authorUndo.back().data && !app.pipelineBusy && app.source.bonds==topology && app.source.atoms[0].type==0,
                              "CPK selection is display-only history and preserves species and topology");
                frame(); requireExport(app.gpu.styles==styles,"drawing and projection cannot mutate saved type styles");
                const auto cpkPixels=stylePixels(); requireExport(cpkPixels!=originalPixels,"selected CPK updates actual viewport pixels");
                app.history(false); frame();
                requireExport(app.creationDisplay.presets.empty(),"style undo restores original appearance");
                requireExport(stylePixels()==originalPixels,"style undo restores the actual GPU image");
                app.history(true); frame();
                requireExport(stylePixels()==cpkPixels,"style redo restores the actual GPU image");
                click("creation.style-all"); click("creation.style-preset-3"); click("creation.style-apply");
                requireExport(app.creationDisplay.defaultPreset==3 && app.creationDisplay.presets.empty(),"global ball-stick clears prior selected override");
                click("creation.style-close"); app.creationSelection={1}; app.creationPick=1; frame();
                click("creation.edit-styles"); frame(); click("creation.style-preset-4"); click("creation.style-apply"); click("creation.style-close");
                const auto styleFile=dir/"gui-styles.atomx"; io::ExportOptions styleOptions;
                styleOptions.documentView=app.captureDocumentView(); io::write(styleFile,io::Format::AtomX,app.source,styleOptions);
                requireExport(document::read(styleFile).view.display==app.creationDisplay,"native document persists mixed appearance and parameters");
                const auto styleTab=app.captureTab(); app.restoreTab(styleTab); frame();
                requireExport(!app.showCreationStyles && app.creationDisplay.defaultPreset==3 && app.creationDisplay.presetAt(1)==4,"tab restore keeps mixed styles and closes stale dialog");
                while(app.authorUndo.size()>baseline) {app.history(false); settlePipeline();}
                app.authorRedo.clear(); frame();
            }
            {
                const size_t baseline=app.authorUndo.size();
                app.editStructure("geometry monitor fixture",[](Dataset &data) {
                    data={}; data.species={"C"}; data.cell={12,0,0,0,12,0,0,0,12};
                    data.atoms={{1,4,2,0},{1,2,2,0},{4,2,2,0},{4,4,4,0},{6,4,4,0}};
                    data.bonds={{0,1},{1,2},{2,3},{3,4}};
                }); settlePipeline(); app.creationSelection.clear(); app.creationPick=-1;
                app.cameras[3].mode=0; app.fitCamera(3,false); frame();
                auto pointOf=[&](int index) {
                    const auto vp=app.uiTestItems.at("creation.viewport");
                    const ImVec2 size{vp.max.x-vp.min.x,vp.max.y-vp.min.y};
                    const auto matrix=app.creationProjection(app.result.data,app.cameras[3],size).combined;
                    const auto at=geometry::at(app.source,index); DirectX::XMFLOAT4 q;
                    DirectX::XMStoreFloat4(&q,DirectX::XMVector4Transform(DirectX::XMVectorSet(at.x,at.y,at.z,1),matrix));
                    return ImVec2{vp.min.x+(q.x/q.w+1)*size.x*.5f,vp.min.y+(1-q.y/q.w)*size.y*.5f};
                };
                auto pointerClick=[&](ImVec2 at) {
                    guiIO.AddMousePosEvent(at.x,at.y); frame(); guiIO.AddMouseButtonEvent(0,true); frame();
                    guiIO.AddMouseButtonEvent(0,false); frame(); settlePipeline();
                };
                click("creation.tool-distance"); frame();
                const auto p1=pointOf(1),p2=pointOf(2);
                pointerClick({(p1.x+p2.x)*.5f,(p1.y+p2.y)*.5f});
                requireExport(app.creationDisplay.monitors.size()==1 && app.creationDisplay.monitors[0].atoms==std::array<int32_t,4>{1,2,-1,-1} &&
                    !app.authorUndo.back().data,"distance bond click creates a persistent display-only monitor");
                app.geometryTarget=4; frame(); click("creation.geometry-apply"); settlePipeline();
                requireExport(std::abs(authoring::distance(app.source,1,2)-4)<1e-5 && app.source.atoms[1].x==1,"numeric geometry apply reaches exact distance and fixes first side");
                app.history(false); settlePipeline(); frame();
                requireExport(std::abs(authoring::distance(app.source,1,2)-3)<1e-5,"numeric edit undo restores geometry and monitor");
                const auto vp=app.uiTestItems.at("creation.viewport");
                const ImVec2 blank{vp.max.x-80,vp.min.y+80};
                const size_t beforeDrag=app.authorUndo.size();
                guiIO.AddMousePosEvent(blank.x,blank.y); frame(); guiIO.AddMouseButtonEvent(0,true); frame();
                guiIO.AddMousePosEvent(blank.x+30,blank.y-40); frame();
                requireExport(app.creationDrag==App::CreationDrag::Geometry && app.source.atoms[2].x==4 && app.result.data.atoms[2].x>4,"drag previews in result while source remains unchanged");
                guiIO.AddMouseButtonEvent(0,false); frame(); settlePipeline();
                requireExport(app.authorUndo.size()==beforeDrag+1 && authoring::distance(app.source,1,2)>3,"geometry drag commits one history step");
                app.history(false); settlePipeline(); frame();
                guiIO.AddKeyEvent(ImGuiMod_Alt,true); frame();
                guiIO.AddMousePosEvent(blank.x,blank.y); frame(); guiIO.AddMouseButtonEvent(0,true); frame();
                guiIO.AddMousePosEvent(blank.x+20,blank.y-20); frame(); guiIO.AddMouseButtonEvent(0,false); frame(); settlePipeline();
                guiIO.AddKeyEvent(ImGuiMod_Alt,false); frame();
                requireExport(app.source.atoms[2].x==4 && app.source.atoms[1].x<1,"Alt drag reverses moving side");
                app.history(false); settlePipeline(); frame();
                guiIO.AddMousePosEvent(blank.x,blank.y); frame(); guiIO.AddMouseButtonEvent(0,true); frame();
                guiIO.AddMousePosEvent(blank.x+40,blank.y-40); frame(); guiIO.AddKeyEvent(ImGuiKey_Escape,true); frame();
                guiIO.AddKeyEvent(ImGuiKey_Escape,false); guiIO.AddMouseButtonEvent(0,false); frame();
                requireExport(app.authorUndo.size()==beforeDrag && app.result.data.atoms[2].x==4,"Escape cancels preview without a history entry");
                click("creation.tool-angle"); frame();
                for(int index:{0,1,2}) {
                    const auto vpNow=app.uiTestItems.at("creation.viewport"); const ImVec2 extent{vpNow.max.x-vpNow.min.x,vpNow.max.y-vpNow.min.y};
                    const auto screen=pointOf(index);
                    requireExport(app.creationHit(vpNow.min,extent,app.cameras[3],screen)==index,"geometry fixture exposes each measurement point");
                    pointerClick(screen);
                }
                requireExport(app.creationDisplay.monitors.size()==2 && app.creationDisplay.monitors[1].count==3,
                    ("three pointer picks create angle with second atom as vertex; tool="+std::to_string(int(app.creationTool))+" pending="+std::to_string(app.geometryPending.size())+" monitors="+std::to_string(app.creationDisplay.monitors.size())+" status="+app.status).c_str());
                app.geometryTarget=125; frame(); click("creation.geometry-apply"); settlePipeline();
                requireExport(std::abs(authoring::bondAngle(app.source,0,1,2)-125)<1e-4,"angle apply rotates the connected branch");
                app.history(false); settlePipeline(); frame();
                click("creation.tool-torsion"); frame();
                pointerClick(pointOf(0)); pointerClick(pointOf(1)); pointerClick(pointOf(2)); pointerClick(pointOf(3));
                requireExport(app.creationDisplay.monitors.size()==3 && app.creationDisplay.monitors[2].count==4,"four pointer picks create signed torsion monitor");
                app.geometryTarget=-60; frame(); click("creation.geometry-apply"); settlePipeline();
                requireExport(std::abs(authoring::dihedralAngle(app.source,0,1,2,3)+60)<1e-4,"torsion numeric apply reaches signed target");
                const auto monitorTab=app.captureTab(); app.restoreTab(monitorTab); frame();
                requireExport(app.creationDisplay.monitors.size()==3 && app.geometryPending.empty(),"tab restore retains independent monitors and cancels unfinished picks");
                const auto monitorFile=dir/"geometry.atomx"; io::ExportOptions monitorOptions;
                monitorOptions.documentView=app.captureDocumentView(); io::write(monitorFile,io::Format::AtomX,app.source,monitorOptions);
                requireExport(document::read(monitorFile).view.display.monitors==app.creationDisplay.monitors,"GUI-created monitors persist in native document");
                click("creation.geometry-remove"); requireExport(app.creationDisplay.monitors.size()==2,"remove control deletes monitor only");
                app.history(false); frame(); requireExport(app.creationDisplay.monitors.size()==3,"display-only undo restores removed monitor");
                while(app.authorUndo.size()>baseline) { app.history(false); settlePipeline(); }
                app.authorRedo.clear(); app.chooseCreationTool(App::CreationTool::Select); frame();
            }
            {
                const size_t baseline=app.authorUndo.size();
                app.editStructure("motion group fixture",[](Dataset &data) {
                    data={}; data.species={"C","H"};
                    data.atoms={{0,0,0,0},{2,0,0,1},{5,0,0,0},{7,0,0,1}};
                    data.bonds={{0,1,{},1},{2,3,{},1}}; data.scalarProperties["Mass"]={12,1,12,1};
                }); settlePipeline(); app.creationSelection={0,1}; app.creationPick=1; frame();
                click("creation.edit-motion-groups"); frame(); strcpy_s(app.motionGroupName,"GUI group");
                click("creation.group-create"); settlePipeline(); frame();
                requireExport(motion::catalog(app.source).size()==1 && motion::members(app.source,1)==std::vector<int>{0,1},
                              "motion group dialog creates exclusive membership from the selected atoms");
                const auto beforeOverlap=app.authorUndo.size(); click("creation.group-create");
                requireExport(app.authorUndo.size()==beforeOverlap,"overlapping group creation cannot add history");
                strcpy_s(app.motionGroupName,"Renamed GUI group"); click("creation.group-rename"); settlePipeline(); frame();
                requireExport(motion::catalog(app.source)[0].name=="Renamed GUI group","group name edit is stored in native metadata");
                click("creation.group-row-1"); click("creation.group-move"); frame();
                requireExport(app.creationMovementSelection==std::vector<int>{0,1} && app.creationMovementMassCenter,
                              "whole-group movement captures all members and uses valid mass center");
                const auto massCenter=motion::massCenter(app.source,{0,1}); const auto original=app.source.atoms;
                click("creation.movement-world-axes"); app.creationMovementAngle=90; frame();
                click("creation.movement-rotate-z"); settlePipeline();
                const auto changedCenter=motion::massCenter(app.source,{0,1});
                requireExport(std::abs(changedCenter->x-massCenter->x)<1e-5 && std::abs(changedCenter->y-massCenter->y)<1e-5 &&
                              std::abs(app.source.atoms[1].y-24.f/13)<1e-5 && app.source.atoms[2].x==original[2].x,
                              "group rigid rotation preserves mass center and unrelated atoms");
                click("creation.movement-close"); app.history(false); settlePipeline(); frame();
                requireExport(app.source.atoms[0].x==original[0].x && app.source.atoms[1].y==0,"whole-group transform undo restores precise coordinates");
                click("creation.edit-motion-groups"); frame(); click("creation.group-row-1"); click("creation.group-select"); frame();
                requireExport(app.creationSelection==std::vector<int>{0,1},"group selection selects the complete visible membership");
                click("creation.group-remove"); settlePipeline(); frame();
                requireExport(motion::catalog(app.source).empty() && app.source.atoms.size()==4,"ungroup preserves atoms");
                click("creation.group-auto"); app.motionGroupJob.wait(); app.poll(); settlePipeline(); frame();
                requireExport(motion::catalog(app.source).size()==2 && motion::members(app.source,2)==std::vector<int>{2,3},
                              "background automatic grouping finds disconnected explicit fragments");
                const auto saved=app.captureTab(); app.restoreTab(saved); frame();
                requireExport(!app.showMotionGroups && motion::catalog(app.source).size()==2,"tab restoration retains group metadata and closes stale selection dialog");
                while (app.authorUndo.size()>baseline) { app.history(false); settlePipeline(); }
                app.authorRedo.clear(); frame();
            }
            {
                app.fragmentLibraryDirectory=dir/"library"; app.fragmentLibraryRequested=false;
                const size_t baseline=app.authorUndo.size();
                app.editStructure("fragment fixture",[&](Dataset &data) {
                    data=app.fragmentLibrary[0].data;
                    data.cell={10,0,0,0,10,0,0,0,10};
                    for (auto &a:data.atoms) { a.x+=2; a.y+=2; a.z+=2; }
                    data.scalarProperties["Charge"]={1,2,3,4,5};
                }); settlePipeline();
                app.creationDisplay.labels[4]={creation::LabelKind::Custom,"survivor"};
                app.cameras[3].mode=2; app.cell=true; app.fitCamera(3,false); frame();
                click("creation.tool-fragment"); frame();
                requireExport(app.showFragmentBrowser && app.uiTestItems.contains("creation.fragment-preview"),
                              "fragment toolbar opens the classified browser and connection point preview");
                click("creation.fragment-place"); frame();
                requireExport(!app.showFragmentBrowser && app.creationTool==App::CreationTool::Fragment,
                              "fragment browser starts placement in the same creation viewport");
                auto pointOf=[&](Vec3 a) {
                    const auto vp=app.uiTestItems.at("creation.viewport"); const ImVec2 size{vp.max.x-vp.min.x,vp.max.y-vp.min.y};
                    const auto matrix=app.creationProjection(app.result.data,app.cameras[3],size).combined; DirectX::XMFLOAT4 q;
                    DirectX::XMStoreFloat4(&q,DirectX::XMVector4Transform(DirectX::XMVectorSet(a.x,a.y,a.z,1),matrix));
                    return ImVec2{vp.min.x+(q.x/q.w+1)*size.x*.5f,vp.min.y+(1-q.y/q.w)*size.y*.5f};
                };
                auto pointerClick=[&](ImVec2 at) {
                    guiIO.AddMousePosEvent(at.x,at.y); frame(); guiIO.AddMouseButtonEvent(0,true); frame();
                    guiIO.AddMouseButtonEvent(0,false); frame(); settlePipeline();
                };
                const size_t start=app.authorUndo.size(); const auto blank=pointOf({6,5,2});
                guiIO.AddMousePosEvent(blank.x,blank.y); frame();
                requireExport(app.creationFragmentPreviewValid && app.source.atoms.size()==5,"fragment preview never edits the source");
                pointerClick(blank);
                requireExport(app.source.atoms.size()==10 && app.source.bonds.size()==8 && app.authorUndo.size()==start+1,
                              "isolated capped template placement creates one history step");
                app.history(false); settlePipeline();
                click("creation.fragment-browser"); frame();
                click("creation.fragment-entry-builtin/hydroxyl"); frame();
                click("creation.fragment-place"); frame();
                auto terminal=pointOf({.91f,2,2});
                guiIO.AddMousePosEvent(terminal.x,terminal.y); frame(); guiIO.AddMouseButtonEvent(0,true); frame();
                const auto provisional=app.creationFragmentPreview.points;
                guiIO.AddMousePosEvent(terminal.x+40,terminal.y+20); frame();
                requireExport(app.creationDrag==App::CreationDrag::Fragment && app.creationFragmentPreviewValid &&
                              app.source.atoms.size()==5 && std::abs(app.creationFragmentPreview.points[2].z-provisional[2].z)>.01,
                              "held attachment rotates a small ghost without modifying the large document");
                guiIO.AddKeyEvent(ImGuiKey_Escape,true); frame(); guiIO.AddKeyEvent(ImGuiKey_Escape,false); frame();
                guiIO.AddMouseButtonEvent(0,false); frame();
                requireExport(app.source.atoms.size()==5 && app.authorUndo.size()==start,"Escape cancels provisional fragment placement");
                pointerClick(pointOf({.91f,2,2}));
                requireExport(app.source.atoms.size()==6 && app.source.bonds.size()==5 &&
                              app.creationDisplay.labelAt(3).text=="survivor" &&
                              app.source.scalarProperties.at("Charge")[1]==3 && app.creationSelection.size()==2,
                              "terminal H substitution creates methanol while remapping surviving attributes and labels");
                app.history(false); settlePipeline();
                requireExport(app.source.atoms.size()==5 && app.creationDisplay.labelAt(4).text=="survivor", "undo restores replaced hydrogen and atom identities");
                app.history(true); settlePipeline();
                app.selectCreationAtom(5,false); app.defineFragment(); frame();
                strcpy_s(app.fragmentName,"GUI methanol"); frame(); click("creation.fragment-save");
                app.fragmentSaveJob.wait(); app.poll(); frame();
                requireExport(app.currentFragment()->name=="GUI methanol" && app.currentFragment()->key.starts_with("user/"),
                              "definition dialog saves the current connected component with its selected terminal point");
                const auto customKey=app.creationFragmentKey;
                requireExport(fragments::load(app.fragmentLibraryDirectory/(customKey.substr(5)+".atomx")).name=="GUI methanol",
                              "application custom library entry survives reopening from disk");
                app.showFragmentBrowser=false; frame();
                const auto tab=app.captureTab(); app.selectFragment(app.fragmentLibrary[0]); app.restoreTab(tab);
                requireExport(app.creationFragmentKey==customKey && app.creationFragmentConnector==5 && !app.creationFragmentPreviewValid,
                              "tabs retain independent fragment and connection point without carrying a gesture");
                settlePipeline();
                while (app.authorUndo.size()>baseline) { app.history(false); settlePipeline(); }
                app.authorRedo.clear(); app.chooseCreationTool(App::CreationTool::Select); app.cameras[3].mode=7; frame();
            }
            {
                const size_t baseline=app.authorUndo.size();
                app.editStructure("display fixture",[](Dataset &data) {
                    auto atom=data.atoms[0]; atom.x+=2; data.atoms.push_back(atom);
                    atom.x+=2; data.atoms.push_back(atom);
                }); settlePipeline();
                app.selectCreationAtom(0,false); frame();
                const auto sourceBefore=app.source.atoms;
                const auto viewport=app.uiTestItems.at("creation.viewport");
                const ImVec2 size{viewport.max.x-viewport.min.x,viewport.max.y-viewport.min.y};
                const auto projection=app.creationProjection(app.result.data,app.cameras[3],size);
                const auto &firstAtom=app.source.atoms[0];
                DirectX::XMFLOAT4 q;
                DirectX::XMStoreFloat4(&q,DirectX::XMVector4Transform(
                    DirectX::XMVectorSet(firstAtom.x,firstAtom.y,firstAtom.z,1),projection.combined));
                const ImVec2 point{viewport.min.x+(q.x/q.w+1)*size.x*.5f,
                                   viewport.min.y+(1-q.y/q.w)*size.y*.5f};
                requireExport(app.creationHit(viewport.min,size,app.cameras[3],point)==0,
                              "visibility fixture atom is pickable before hiding");
                click("creation.hide-selected");
                requireExport(app.creationDisplay.hiddenCount==1 && app.creationSelection.empty() &&
                              !app.authorUndo.back().data && !app.pipelineBusy &&
                              app.source.atoms.size()==sourceBefore.size() &&
                              app.source.atoms[0].x==sourceBefore[0].x,
                              "hide uses display-only history without copying or evaluating structure");
                requireExport(app.creationHit(viewport.min,size,app.cameras[3],point)!=0,
                              "hidden atoms cannot be picked by the creation viewport");
                // Hiding clears selection; observe the resulting smaller right panel.
                frame();
                click("creation.show-all");
                app.history(false); frame();
                requireExport(app.creationDisplay.isHidden(0),"undo show all restores the prior visibility mask");
                app.history(true); frame();
                requireExport(app.creationDisplay.hiddenCount==0,"redo show all restores display without altering geometry");
                app.selectCreationAtom(1,false); frame();
                click("creation.show-only");
                requireExport(app.creationDisplay.hiddenCount==2 && !app.creationDisplay.isHidden(1),
                              "show only retains just the selected atom");
                frame();
                click("creation.edit-labels"); frame();
                app.creationLabelKind=int(creation::LabelKind::Custom);
                strcpy_s(app.creationLabelText,"site A"); frame();
                click("creation.label-apply");
                click("creation.label-close");
                requireExport(app.creationDisplay.labelAt(1).text=="site A" &&
                              app.creationDisplay.labelAt(0).kind==creation::LabelKind::None,
                              "label dialog applies custom text to the captured selection only");
                app.saveCreationSnapshot("display state");
                {
                    const int originalCreationTab=app.activeTab;
                    const auto currentCamera=app.cameras[3];
                    const auto currentStyles=app.gpu.styles;
                    app.creationSketchOrder=3; app.creationSketchContinuous=false;
                    app.exportFormat=int(io::Format::AtomX); app.exportRange=false;
                    const auto nativePath=dir/"creation.atomx";
                    auto finishNative=[&]() {
                        app.exportJob.wait(); app.poll();
                        requireExport(!app.exporting && app.status.find("Exported")!=std::string::npos,
                                      "native export finishes through application polling");
                    };
                    app.startDataExport(nativePath); finishNative();
                    requireExport(app.tabs[size_t(app.activeTab)].documentPath==nativePath,
                                  "first native save remembers this creation tab's document path");
                    app.saveSessionState(false); finishNative();
                    const auto saved=document::read(nativePath);
                    requireExport(saved.view.creation && saved.view.display.hiddenCount==2 &&
                                  saved.view.display.labelAt(1).text=="site A",
                                  "application native save captures creation annotations");
                    app.openFileTab(nativePath);
                    app.job.wait(); app.poll();
                    // A stale two-type viewport can draw between document
                    // restoration and publication of its three-type pipeline.
                    // Renderer resize must not replace saved C/H/O styles.
                    const std::vector<std::string> staleTypes={"Cu","Ni"};
                    app.gpu.uploadStyles(staleTypes.size(),&staleTypes);
                    settlePipeline();
                    requireExport(app.creationMode && app.activeTab!=originalCreationTab && app.source.atoms.size()==3 &&
                                  app.creationDisplay.hiddenCount==2 && app.creationDisplay.labelAt(1).text=="site A" &&
                                  app.gpu.styles==currentStyles && app.cameras[3].zoom==currentCamera.zoom &&
                                  app.authorUndo.empty() && app.creationSnapshots.size()==1 &&
                                  app.creationSketchOrder==3 && !app.creationSketchContinuous,
                                  "opening native file restores an independent creation tab with camera, styles and display");
                    app.editCreationBond(0,1,3); settlePipeline();
                    app.saveSessionState(false); finishNative();
                    requireExport(document::read(nativePath).data.bonds.size()==1 &&
                                  document::read(nativePath).data.bonds[0].order==3,
                                  "Ctrl S native save atomically updates the reopened file with manual topology");
                    app.closeTab(app.activeTab); settlePipeline();
                    requireExport(app.activeTab==originalCreationTab && app.source.bonds.empty() &&
                                  app.creationDisplay.hiddenCount==2 &&
                                  app.tabs[size_t(app.activeTab)].documentPath==nativePath,
                                  "editing and saving the reopened document leaves the original creation tab independent");
                    app.creationSketchOrder=1; app.creationSketchContinuous=true; app.exportFormat=0;
                }
                const int snapshot=int(app.creationSnapshots.size())-1;
                app.selectCreationAtom(1,false); app.deletePickedAtom(); settlePipeline();
                requireExport(app.source.atoms.size()==2 && app.creationDisplay.hiddenCount==2 &&
                              app.creationDisplay.labels.empty(),"deleting the labeled atom remaps surviving visibility");
                app.history(false); settlePipeline();
                requireExport(app.source.atoms.size()==3 && app.creationDisplay.labelAt(1).text=="site A" &&
                              !app.creationDisplay.isHidden(1),"undo deletion restores atom identity and annotations");
                frame(); click("creation.show-all"); frame();
                click("creation.edit-labels"); frame();
                click("creation.label-remove-all"); click("creation.label-close");
                app.restoreCreationSnapshot(snapshot); settlePipeline();
                requireExport(app.creationDisplay.hiddenCount==2 && app.creationDisplay.labelAt(1).text=="site A",
                              "workspace snapshot restores labels and visibility alongside structure");
                app.history(false); settlePipeline();
                requireExport(app.creationDisplay.hiddenCount==0 && app.creationDisplay.labels.empty(),
                              "undo snapshot restore recovers the intervening display settings");
                while (app.authorUndo.size()>baseline) { app.history(false); settlePipeline(); }
                requireExport(app.source.atoms.size()==originalAtomCount && app.creationDisplay.hiddenCount==0 &&
                              app.creationDisplay.labels.empty(),"display test restores its original creation document");
            }
            click("creation.properties-toggle");
            requireExport(!app.creationPropertiesOpen,"creation file properties can collapse");
            frame();
            click("creation.properties-toggle");
            requireExport(app.creationPropertiesOpen,"creation file properties can reopen");
            click("creation.return-view");
            frame();
            requireExport(!app.creationMode && app.activeTab == originalTab,
                          "leaving Creation Mode restores the original view tab");
            settlePipeline();
            click("tabs.tab."+std::to_string(originalTabCount));
            requireExport(app.creationMode && app.activeTab==int(originalTabCount),
                          "browser tab pointer click restores the creation document");
            settlePipeline();
            click("tabs.tab."+std::to_string(originalTab));
            requireExport(!app.creationMode && app.activeTab==originalTab,
                          "browser tab pointer click returns to the view document");
            // The compact icon row replaced the Pipeline/Render/Analysis/
            // System text tab bar; each icon switches the panel section.
            requireExport(app.uiTestItems.contains("panel.tab.pipeline") &&
                              app.uiTestItems.contains("panel.tab.render") &&
                              app.uiTestItems.contains("panel.tab.analysis") &&
                              app.uiTestItems.contains("panel.tab.system"),
                          "icon section switcher replaces the text tab bar");
            click("panel.tab.render");
            requireExport(app.rightTab == 1, "Render icon switches the panel section");
            click("panel.tab.system");
            requireExport(app.rightTab == 3, "System icon switches the panel section");
            click("panel.tab.analysis");
            requireExport(app.rightTab == 2, "Analysis icon switches the panel section");
            click("panel.tab.pipeline");
            requireExport(app.rightTab == 0 &&
                              app.uiTestItems.contains("pipeline.add-modification"),
                          "Pipeline icon restores the pipeline section with its controls");
            // Viewport tool cursor mapping; the OS-level application of the
            // cursor is exercised by the real smoke run (WM_SETCURSOR path).
            requireExport(cursorForViewportTool(0) == ViewportCursor::Magnifier &&
                              cursorForViewportTool(1) == ViewportCursor::Hand &&
                              cursorForViewportTool(2) == ViewportCursor::ResizeAll &&
                              cursorForViewportTool(3) == ViewportCursor::Arrow,
                          "viewport tool cursor mapping");
            // The removed full-width status bar lives on as a fading overlay
            // inside the active viewport.
            app.status = "Opened test.xyz: 108 atoms";
            frame();
            requireExport(app.overlayStatus == "Opened test.xyz: 108 atoms",
                          "status messages surface through the viewport overlay");
            click("pipeline.add-modification");
            frame(); // Allow the newly opened popup to settle before using its recorded item rectangles.
            requireExport(app.uiTestItems.contains("catalog.Radial distribution function (RDF)"),
                          "catalog operation is reachable by its stable label");
            const auto catalogId=app.uiTestItems.at("catalog.Radial distribution function (RDF)").id;
            frame();
            requireExport(app.uiTestItems.contains("catalog.Radial distribution function (RDF)") &&
                              app.uiTestItems.at("catalog.Radial distribution function (RDF)").id==catalogId,
                          "catalog widget IDs stay stable while its popup remains open");
            click("catalog.Radial distribution function (RDF)");
            requireExport(app.mods.size()==1 && app.mods[0].op==Op::RadialDistribution,
                          "mouse interaction inserts the selected real modifier");
            if (app.pipelineJob.valid()) app.pipelineJob.wait();
            app.selectPipeline=true;
            frame();
            const auto originalNodeId=app.mods[0].id;
            requireExport(app.uiTestItems.contains("pipeline.rdf-bins."+originalNodeId),
                          "selected modifier exposes its RDF parameter control");
            click("pipeline.rdf-bins."+originalNodeId,.28f); // Aim inside the numeric text area, away from the spin buttons.
            guiIO.AddKeyEvent(ImGuiKey_LeftCtrl,true);
            guiIO.AddKeyEvent(ImGuiKey_A,true); frame();
            guiIO.AddKeyEvent(ImGuiKey_A,false);
            guiIO.AddKeyEvent(ImGuiKey_LeftCtrl,false);
            guiIO.AddInputCharactersUTF8("16"); frame();
            guiIO.AddKeyEvent(ImGuiKey_Enter,true); frame();
            guiIO.AddKeyEvent(ImGuiKey_Enter,false); frame();
            if (app.pipelineJob.valid()) app.pipelineJob.wait();
            frame();
            requireExport(app.mods[0].rdfBins==16 && app.result.data.tables.back().rows.size()==16 &&
                              app.error.empty(),
                          "keyboard editing changes the actual RDF modifier and its computed table");
            click("toolbar.undo");
            if (app.pipelineJob.valid()) app.pipelineJob.wait();
            frame();
            requireExport(app.mods[0].rdfBins==128 && app.result.data.tables.back().rows.size()==128,
                          "Undo control restores the modifier parameters and recomputes its original table");
            click("toolbar.redo");
            if (app.pipelineJob.valid()) app.pipelineJob.wait();
            frame();
            requireExport(app.mods[0].rdfBins==16 && app.result.data.tables.back().rows.size()==16,
                          "Redo control restores the edited modifier parameters and recomputes its table");
            click("pipeline.node.copy."+originalNodeId);
            requireExport(app.mods.size()==2 && app.mods[1].op==Op::RadialDistribution &&
                              app.mods[1].rdfBins==16,
                          "pipeline Copy duplicates the selected node and its independent parameters");
            if (app.pipelineJob.valid()) app.pipelineJob.wait();
            frame();
            const auto copiedNodeId=app.mods[1].id;
            click("pipeline.node.select."+copiedNodeId);
            frame();
            click("pipeline.rdf-bins."+copiedNodeId,.28f);
            guiIO.AddKeyEvent(ImGuiKey_LeftCtrl,true);
            guiIO.AddKeyEvent(ImGuiKey_A,true); frame();
            guiIO.AddKeyEvent(ImGuiKey_A,false);
            guiIO.AddKeyEvent(ImGuiKey_LeftCtrl,false);
            guiIO.AddInputCharactersUTF8("8"); frame();
            guiIO.AddKeyEvent(ImGuiKey_Enter,true); frame();
            guiIO.AddKeyEvent(ImGuiKey_Enter,false); frame();
            if (app.pipelineJob.valid()) app.pipelineJob.wait();
            frame();
            requireExport(app.mods[0].rdfBins==16 && app.mods[1].rdfBins==8 &&
                              app.result.data.tables.back().rows.size()==8,
                          "copied node parameters are independent and drive its real recomputation");
            click("pipeline.node.enabled."+copiedNodeId);
            if (app.pipelineJob.valid()) app.pipelineJob.wait();
            frame();
            requireExport(!app.mods[1].enabled && app.result.data.tables.back().rows.size()==16,
                          "disabling a node bypasses its work and publishes the upstream table");
            click("pipeline.node.enabled."+copiedNodeId);
            if (app.pipelineJob.valid()) app.pipelineJob.wait();
            frame();
            requireExport(app.mods[1].enabled && app.result.data.tables.back().rows.size()==8,
                          "re-enabling a node restores its computed output");
            click("pipeline.node.down."+copiedNodeId);
            if (app.pipelineJob.valid()) app.pipelineJob.wait();
            frame();
            requireExport(app.mods[0].id==copiedNodeId && app.mods[1].id==originalNodeId &&
                              app.result.data.tables.back().rows.size()==16,
                          "visual down moves the node earlier in execution order");
            click("pipeline.node.up."+copiedNodeId);
            if (app.pipelineJob.valid()) app.pipelineJob.wait();
            frame();
            requireExport(app.mods[0].id==originalNodeId && app.mods[1].id==copiedNodeId &&
                              app.result.data.tables.back().rows.size()==8,
                          "visual up moves the node later in execution order");
            click("pipeline.node.delete."+copiedNodeId);
            requireExport(app.mods.size()==1 && app.mods[0].id==originalNodeId,
                          "pipeline Delete removes only the selected copied node");
            if (app.pipelineJob.valid()) app.pipelineJob.wait();
            frame();
            for (const auto &[width,height,scale] : std::vector<std::tuple<float,float,float>>{
                     {1280.f,900.f,1.f},{1560.f,1000.f,1.f},
                     {1600.f,1000.f,1.5f},{1920.f,1080.f,2.f}}) {
                guiIO.DisplaySize={width,height};
                uiScale=scale;
                ImGui::GetStyle()=baseStyle;
                ImGui::GetStyle().ScaleAllSizes(scale);
                frame();
                for (const auto &control : {std::string("pipeline.add-modification"),
                                            "pipeline.node.copy."+originalNodeId,
                                            "pipeline.node.delete."+originalNodeId}) {
                    auto found=app.uiTestItems.find(control);
                    requireExport(found!=app.uiTestItems.end(),
                                  "responsive workspace keeps Pipeline controls present");
                    requireExport(found->second.min.x>=0 && found->second.min.y>=0 &&
                                      found->second.max.x<=width && found->second.max.y<=height,
                                  ("responsive Pipeline control exceeds viewport bounds: "+control).c_str());
                }
                click("pipeline.node.copy."+originalNodeId);
                if (app.pipelineJob.valid()) app.pipelineJob.wait();
                frame();
                const auto responsiveCopyId=app.mods[1].id;
                click("pipeline.node.delete."+responsiveCopyId);
                if (app.pipelineJob.valid()) app.pipelineJob.wait();
                frame();
                requireExport(app.mods.size()==1 && app.mods[0].id==originalNodeId,
                              "Pipeline copy/delete controls remain clickable at tested width and scale");
            }
            uiScale=1.f;
            ImGui::GetStyle()=baseStyle;
            guiIO.DisplaySize={1560,1000};
            frame();

            strcpy_s(app.modifierSearch,"Reduce property");
            click("pipeline.add-modification");
            frame();
            requireExport(app.uiTestItems.contains("catalog.Reduce property"),
                          "Reduce property is available in the Analysis catalog");
            click("catalog.Reduce property");
            settlePipeline();
            const auto reduceNodeId=app.mods.back().id;
            const auto meanKey=std::string("ReduceProperty.Position.X.mean");
            requireExport(app.mods.back().op==Op::ReduceProperty &&
                              app.result.data.globalAttributes.contains(meanKey) &&
                              std::abs(app.result.data.globalAttributes.at(meanKey)-1.0)<1e-12 &&
                              app.uiTestItems.contains("pipeline.analysis-property."+reduceNodeId) &&
                              app.uiTestItems.contains("pipeline.reduce-operation."+reduceNodeId),
                          "Reduce property computes the default mean and exposes the node controls");
            click("pipeline.reduce-operation."+reduceNodeId);
            guiIO.AddKeyEvent(ImGuiKey_DownArrow,true); frame();
            guiIO.AddKeyEvent(ImGuiKey_DownArrow,false); frame();
            guiIO.AddKeyEvent(ImGuiKey_Enter,true); frame();
            guiIO.AddKeyEvent(ImGuiKey_Enter,false); frame();
            settlePipeline();
            const auto sumKey=std::string("ReduceProperty.Position.X.sum");
            requireExport(app.mods.back().reduceOperation==3 &&
                              app.result.data.globalAttributes.contains(sumKey) &&
                              !app.result.data.globalAttributes.contains(meanKey) &&
                              std::abs(app.result.data.globalAttributes.at(sumKey)-1.0)<1e-12,
                          "editing the Reduction control recomputes and replaces the correct global attribute");
            app.showTable=true;
            app.globalAttributesTab=true;
            frame();
            frame();
            requireExport(app.uiTestItems.contains("inspector.global-attribute."+sumKey),
                          "Global Attributes inspector displays the Reduce property pipeline result");
            requireExport(app.uiTestItems.contains("inspector.filter.global-attribute-filter") &&
                              app.uiTestItems.contains("inspector.count.attributes"),
                          "Global Attributes inspector exposes the filter field and count label");
            requireExport(app.uiTestItems.contains("inspector.global-attribute.SourceFrame") &&
                              app.uiTestItems.contains("inspector.global-attribute.Time") &&
                              app.uiTestItems.contains("inspector.global-attribute.SourceFile") ==
                                  !app.path.empty(),
                          "Global Attributes inspector standard entries are published");
            // P9 Bonds-page click-through: selecting the Bonds inspector tab
            // exposes the filter field and a count label for the published
            // bonds (two synthetic rows; the label is driven by the row count
            // the table renders).
            app.result.data.bonds = {{0, 0, {0, 0, 0}}, {0, 0, {0, 1, 0}}};
            app.bondsTab = true;
            frame(); // SetSelected queues tab focus at the end of the ImGui frame.
            frame();
            requireExport(!app.bondsTab,
                          "ImGui SetSelected opens the Bonds inspector tab");
            requireExport(app.uiTestItems.contains("inspector.filter.bond-filter"),
                          "Bonds inspector exposes the filter field");
            requireExport(app.uiTestItems.contains("inspector.count.bonds"),
                          "Bonds inspector exposes the bonds count label");
            requireExport(app.mods.size()==2 && app.mods.back().id==reduceNodeId &&
                              app.mods.front().op==Op::RadialDistribution,
                          "analysis parameter edits retain the stable pipeline node identity");

            strcpy_s(app.modifierSearch,"Scatter plot");
            click("pipeline.add-modification");
            frame();
            requireExport(app.uiTestItems.contains("catalog.Scatter plot"),
                          "Scatter plot is available as an executable Analysis catalog operation");
            click("catalog.Scatter plot");
            if (app.pipelineJob.valid()) app.pipelineJob.wait();
            frame();
            const auto scatterNodeId=app.mods.back().id;
            requireExport(app.mods.back().op==Op::ScatterPlot &&
                              app.result.data.tables.back().name=="Scatter plot: Position.X vs Position.Y" &&
                              app.result.data.tables.back().rows.size()==1 &&
                              app.uiTestItems.contains("pipeline.scatter-x."+scatterNodeId) &&
                              app.uiTestItems.contains("pipeline.scatter-y."+scatterNodeId) &&
                              app.uiTestItems.contains("pipeline.scatter-selected."+scatterNodeId),
                          "adding Scatter plot computes a data table and exposes per-node property/selection settings");
            app.showTable=true;
            app.histogramPreviewTab=true;
            frame(); // SetSelected queues tab focus at the end of the ImGui frame.
            frame();
            requireExport(!app.histogramPreviewTab,
                          "ImGui SetSelected opens the Data Tables inspector tab");
            requireExport(app.uiTestItems.contains("inspector.scatter-chart"),
                          "Data Tables inspector draws the actual scatter chart for the pipeline result");
            const auto scatterChartRect=app.uiTestItems.at("inspector.scatter-chart");
            // P5 reclaimed the dataset strip and status bar, so viewports and
            // the timeline stretch taller and the Data inspector child sits
            // lower. The chart is laid out inside that child's scroll content
            // (clipped by the child until scrolled into view), so the bounds
            // contract is: non-degenerate, horizontally inside the display,
            // and its top edge on-screen.
            const bool scatterChartFits=scatterChartRect.max.x>scatterChartRect.min.x &&
                              scatterChartRect.max.y>scatterChartRect.min.y &&
                              scatterChartRect.min.x>=0 && scatterChartRect.min.y>=0 &&
                              scatterChartRect.min.y<=guiIO.DisplaySize.y &&
                              scatterChartRect.max.x<=guiIO.DisplaySize.x;
            requireExport(scatterChartFits,
                          ("scatter chart rectangle "+std::to_string(scatterChartRect.min.x)+","+
                           std::to_string(scatterChartRect.min.y)+"-"+
                           std::to_string(scatterChartRect.max.x)+","+
                           std::to_string(scatterChartRect.max.y)+" in display "+
                           std::to_string(guiIO.DisplaySize.x)+"x"+
                           std::to_string(guiIO.DisplaySize.y)).c_str());

            // P10 menu bar: the three OVITO menus live in the title strip and
            // each opens with its entries; disabled entries still record.
            frame();
            requireExport(app.uiTestItems.contains("menu.file") &&
                              app.uiTestItems.contains("menu.edit") &&
                              app.uiTestItems.contains("menu.help"),
                          "menu bar exposes File, Edit and Help");
            click("menu.file");
            frame();
            // Recent Files entries live behind a submenu that this test does
            // not open, and the persisted preference list may be non-empty on
            // a developer machine; clear it so the honest-disabled-entries
            // contract is deterministic in the harness.
            app.preferences.recentFiles.clear();
            frame();
            requireExport(app.uiTestItems.contains("menu.file.load-file") &&
                              app.uiTestItems.contains("menu.file.load-remote") &&
                              app.uiTestItems.contains("menu.file.export") &&
                              app.uiTestItems.contains("menu.file.recent.0") ==
                                  !app.preferences.recentFiles.empty() &&
                              app.uiTestItems.contains("menu.file.load-session") &&
                              app.uiTestItems.contains("menu.file.save-session") &&
                              app.uiTestItems.contains("menu.file.save-session-as") &&
                              app.uiTestItems.contains("menu.file.run-python") &&
                              app.uiTestItems.contains("menu.file.quit"),
                          "File menu lists its real and honestly-disabled entries");
            guiIO.AddKeyEvent(ImGuiKey_Escape, true); frame();
            guiIO.AddKeyEvent(ImGuiKey_Escape, false); frame();
            click("menu.edit");
            frame();
            requireExport(app.uiTestItems.contains("menu.edit.undo") &&
                              app.uiTestItems.contains("menu.edit.redo") &&
                              app.uiTestItems.contains("menu.edit.settings"),
                          "Edit menu lists undo, redo and settings");
            guiIO.AddKeyEvent(ImGuiKey_Escape, true); frame();
            guiIO.AddKeyEvent(ImGuiKey_Escape, false); frame();
            click("menu.help");
            frame();
            requireExport(app.uiTestItems.contains("menu.help.user-manual") &&
                              app.uiTestItems.contains("menu.help.scripting") &&
                              app.uiTestItems.contains("menu.help.request-feature") &&
                              app.uiTestItems.contains("menu.help.system-info") &&
                              app.uiTestItems.contains("menu.help.about"),
                          "Help menu lists its entries");
            guiIO.AddKeyEvent(ImGuiKey_Escape, true); frame();
            guiIO.AddKeyEvent(ImGuiKey_Escape, false); frame();
            // P10 Quick command search: focus, substring filter, Enter runs
            // the highlighted entry through the real add(Op) path.
            app.paletteRequested = true;
            frame();
            frame();
            requireExport(app.uiTestItems.contains("palette.search"),
                          "command palette exposes the toolbar search field");
            guiIO.AddInputCharactersUTF8("slice"); frame();
            requireExport(app.uiTestItems.contains("palette.item.Modification: Slice"),
                          "palette filters to the Slice modifier entry");
            guiIO.AddKeyEvent(ImGuiKey_Enter, true); frame();
            guiIO.AddKeyEvent(ImGuiKey_Enter, false); frame();
            settlePipeline();
            requireExport(std::any_of(app.mods.begin(), app.mods.end(),
                                      [](const ModifierNode &n) { return n.op == Op::Slice; }),
                          "executing the palette entry adds a real Slice modifier");
            requireExport(app.commandSearch[0] == 0 && !app.paletteActive,
                          "executing a palette command clears and closes the palette");

            // P11 timeline ruler: the pure 1/2/5-decade label-step helper and
            // seek-by-click on the redesigned track. The frame number spinner
            // in the transport cluster stays the canonical frame display.
            // This runs before the failing-node section below: the "Operation
            // failed" modal that test leaves open clears ImGui hover for every
            // other window and would swallow the pointer interaction here.
            requireExport(timelineLabelStep(0, 1400) == 1 &&
                              timelineLabelStep(2, 1400) == 1 &&
                              timelineLabelStep(29, 1400) == 5 &&
                              timelineLabelStep(999999, 1400) >= 100,
                          "timeline label step follows the 1/2/5 series and grows with frame count");
            {
                std::vector<int64_t> labels;
                for (int64_t f = 0; f <= 29; f += timelineLabelStep(29, 1400))
                    labels.push_back(f);
                requireExport(labels == std::vector<int64_t>{0, 5, 10, 15, 20, 25},
                              "a 30-frame ruler labels exactly 0,5,10,15,20,25");
            }
            app.path = input;
            app.frames = io::index(input);
            frame();
            requireExport(app.uiTestItems.contains("timeline.ruler"),
                          "the redesigned timeline ruler records its stable UI-test control");
            if (app.job.valid()) app.job.wait();
            app.poll();
            click("timeline.ruler", .99f);
            if (app.job.valid()) app.job.wait();
            app.poll();
            if (app.busy) { app.job.wait(); app.poll(); }
            requireExport(app.current == 2 && app.pendingFrame < 0,
                          "clicking the right end of the ruler track seeks to the last frame");
            click("timeline.ruler", .02f);
            if (app.job.valid()) app.job.wait();
            app.poll();
            if (app.busy) { app.job.wait(); app.poll(); }
            requireExport(app.current == 0,
                          "clicking the left end of the ruler track seeks to frame 0");
            for (int attempt=0; attempt<16 && app.pipelineBusy; ++attempt) {
                if (app.pipelineJob.valid()) app.pipelineJob.wait();
                frame();
            }
            requireExport(!app.pipelineBusy,
                          "ruler seek pipeline settles before subsequent pipeline edits");
            // The settled re-evaluation may legitimately have flagged the RDF
            // node for this synthetic fixture; drop it so the failing-node
            // section below observes only its own published failure.
            app.error.clear();
            // Restore the synthetic source the ruler clicks replaced via the
            // real load path (XYZ round-trip drops the cell, which the RDF
            // node rejects), so later sections see the expected pipeline.
            app.source.species={"Cu"};
            app.source.cell={4,0,0,0,4,0,0,0,4};
            app.source.origin={};
            app.source.pbc={true,true,true};
            app.source.atoms={{1,1,1,0}};
            app.source.sourceCount=1;
            app.result=evaluate(app.source, std::vector<Modifier>{});
            app.update(0);
            for (int attempt=0; attempt<16 && app.pipelineBusy; ++attempt) {
                if (app.pipelineJob.valid()) app.pipelineJob.wait();
                frame();
            }
            requireExport(!app.pipelineBusy && app.error.empty(),
                          "pipeline settles cleanly after the ruler seek fixture restore");
            app.frames.clear();
            app.path.clear();
            frame();

            const size_t previouslyPublishedAtoms=app.result.data.atoms.size();
            const size_t previouslyPublishedTableRows=app.result.data.tables.back().rows.size();
            Modifier invalidScale{Op::Scale};
            invalidScale.value=0;
            const size_t failingNodeIndex=app.mods.size();
            app.modifierGraph.insert(app.makeNode(invalidScale));
            const auto failingNodeId=app.mods.back().id;
            Modifier downstreamReduction{Op::ReduceProperty};
            downstreamReduction.property="Missing downstream field";
            app.modifierGraph.insert(app.makeNode(downstreamReduction));
            const auto downstreamNodeId=app.mods.back().id;
            app.staleResult=false;
            app.update(failingNodeIndex);
            requireExport(app.staleResult,"a pipeline edit marks the previously published frame stale immediately");
            if (app.pipelineJob.valid()) app.pipelineJob.wait();
            frame(); // Poll and publish the asynchronous node failure.
            requireExport(app.error=="Scale must be positive" &&
                              app.mods[failingNodeIndex].id==failingNodeId &&
                              app.mods[failingNodeIndex].error==app.error &&
                              app.mods[failingNodeIndex+1].id==downstreamNodeId &&
                              app.mods[failingNodeIndex+1].error.empty(),
                          "asynchronous failure is assigned to its node and stops before the invalid downstream node");
            requireExport(app.staleResult && app.result.data.atoms.size()==previouslyPublishedAtoms &&
                              app.result.data.tables.back().rows.size()==previouslyPublishedTableRows,
                          "failed evaluation retains the prior result and keeps its stale marker");
        }
        {
            App tabs(window, testRenderer);
            tabs.configureCrystalPreset(1);
            const auto nacl=tabs.crystalFromDialog();
            requireExport(nacl.atoms.size()==8 && nacl.species.size()==2,
                          "NaCl crystal dialog preset builds the conventional rocksalt cell");
            auto ready = [&] {
                for (int i=0; i<3000 && tabs.documentsBusy(); ++i) {
                    tabs.poll();
                    Sleep(1);
                }
                requireExport(!tabs.documentsBusy(), "tab operation finishes");
            };
            ready();
            const auto fixture=dir / "tabs.xyz";
            Dataset structure;
            structure.species={"Na","Cl"};
            structure.cell={5.6f,0,0,0,5.6f,0,0,0,5.6f};
            structure.atoms={{0,0,0,0},{2.8f,2.8f,2.8f,1}};
            structure.sourceCount=2;
            io::write(fixture,io::Format::XYZ,structure,{});
            tabs.newHomeTab(); ready();
            requireExport(tabs.homeMode && tabs.tabs.size()==2,"Ctrl+T creates a home tab");
            tabs.openFileTab(fixture); ready();
            requireExport(tabs.tabs.size()==2 && !tabs.homeMode && tabs.source.atoms.size()==2,
                          "opening a file replaces an active home tab");
            const int view=tabs.activeTab;
            tabs.openCreationTab(); ready();
            const int first=tabs.activeTab;
            requireExport(first!=view && tabs.creationMode && tabs.tabs.size()==3,
                          "view context opens one creation tab");
            requireExport(tabs.creationSnapshots.size()==1 &&
                          tabs.creationSnapshots[0].data->atoms.size()==2,
                          "creation workspace starts with an immutable source snapshot");
            const ImVec2 viewportSize{1000,1000};
            const auto projection=tabs.creationProjection(tabs.result.data,tabs.cameras[3],viewportSize);
            requireExport(tabs.creationScreenRadius(tabs.result.data.atoms[0],projection,viewportSize)>U(12),
                          "selection halo follows the projected atom radius");
            const float yaw=tabs.cameras[3].yaw;
            auto pointerFrame=[&](float x,float y,bool middle) {
                guiIO.AddMousePosEvent(x,y);
                guiIO.AddMouseButtonEvent(2,middle);
                ImGui::NewFrame();
                tabs.creationPointer({0,0},{1000,1000},tabs.cameras[3],true);
                ImGui::Render();
            };
            pointerFrame(100,100,false);
            pointerFrame(100,100,true);
            pointerFrame(145,112,true);
            pointerFrame(145,112,false);
            requireExport(tabs.cameras[3].yaw!=yaw && tabs.creationDrag==App::CreationDrag::None,
                          "middle-button drag rotates creation camera and releases its drag state");
            tabs.addAtomAt({1,1,1}); ready();
            requireExport(tabs.source.atoms.size()==3 && !tabs.authorUndo.empty(),
                          "creation edit records history");
            tabs.saveCreationSnapshot("three atoms");
            tabs.addAtomAt({2,2,2}); ready();
            requireExport(tabs.source.atoms.size()==4 &&
                          tabs.creationSnapshots.back().data->atoms.size()==3,
                          "later edits do not mutate a saved workspace result");
            tabs.restoreCreationSnapshot(1); ready();
            requireExport(tabs.source.atoms.size()==3 && tabs.creationSnapshotSelected==1,
                          "restoring a snapshot stays in the same tab and records history");
            tabs.creationPropertiesOpen=false;
            tabs.selectCreationAtom(1,false);
            tabs.creationVisibility(1);
            auto annotated=tabs.creationDisplay;
            annotated.setLabels(tabs.source.atoms.size(),{1},{creation::LabelKind::Custom,"isolated site"},false);
            tabs.editCreationDisplay(std::move(annotated),"label isolation");
            tabs.switchTab(view); ready();
            requireExport(!tabs.creationMode && tabs.source.atoms.size()==2 && tabs.creationDisplay.hiddenCount==0 &&
                          tabs.creationDisplay.labels.empty(),
                          "creation edit leaves view document intact");
            tabs.openCreationTab(); ready();
            requireExport(tabs.activeTab!=first && tabs.tabs.size()==4 &&
                          tabs.source.atoms.size()==2 && tabs.authorUndo.empty() &&
                          tabs.creationSnapshots.size()==1 && tabs.creationPropertiesOpen &&
                          tabs.creationDisplay.hiddenCount==0 && tabs.creationDisplay.labels.empty(),
                          "reopening creates an independent creation copy");
            tabs.switchTab(first); ready();
            requireExport(tabs.source.atoms.size()==3 && !tabs.authorUndo.empty() &&
                          tabs.creationSnapshots.size()==2 && !tabs.creationPropertiesOpen &&
                          tabs.creationDisplay.hiddenCount==2 && tabs.creationDisplay.labelAt(1).text=="isolated site",
                          "creation copy restores its own structure, workspace and panel state");
        }
        ImGui::DestroyContext();
        for (const auto &e : std::filesystem::directory_iterator(dir)) {
            requireExport(e.path().parent_path()==dir,"fixture cleanup stays inside its owned temporary directory");
            std::filesystem::remove_all(e.path());
        }
        std::filesystem::remove(dir);
        DestroyWindow(window);
        CoUninitialize();
        std::cout << "PASS: application export, real ImGui pointer/keyboard events, "
                     "modifier creation and parameter recomputation\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "; fixtures: " << dir << '\n';
        DestroyWindow(window);
        return 1;
    }
}
