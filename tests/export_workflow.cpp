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
            strcpy_s(app.modifierSearch,"Radial distribution function (RDF)");
            const ImGuiStyle baseStyle=ImGui::GetStyle();
            auto frame=[&]() {
                guiIO.DeltaTime=1.f/60.f;
                app.uiTestItems.clear();
                ImGui::NewFrame();
                app.ui();
                ImGui::Render();
            };
            auto click=[&](const std::string &name,float xFraction=.5f) {
                auto found=app.uiTestItems.find(name);
                requireExport(found!=app.uiTestItems.end(),"required interactive UI control was not recorded");
                const auto item=found->second;
                requireExport(item.id!=0,"interactive control must expose a stable nonzero ImGui ID");
                requireExport(item.max.x>item.min.x && item.max.y>item.min.y,
                              "interactive UI control has an empty rectangle");
                requireExport(item.min.x>=0 && item.min.y>=0 &&
                                  item.max.x<=guiIO.DisplaySize.x && item.max.y<=guiIO.DisplaySize.y,
                              ("interactive UI control exceeds application bounds: "+name).c_str());
                const ImVec2 point{item.min.x+(item.max.x-item.min.x)*xFraction,
                                   (item.min.y+item.max.y)*.5f};
                guiIO.AddMousePosEvent(point.x,point.y);
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
        }
        ImGui::DestroyContext();
        for (const auto &e : std::filesystem::directory_iterator(dir))
            std::filesystem::remove(e.path());
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
