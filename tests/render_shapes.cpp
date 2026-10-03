#include "../src/renderer.hpp"
#include <iostream>
#include <limits>
#include <set>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
int main(int argc, char **argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const LUID adapter = {0x1234, 7};
    const LUID alias = {0x1234, 7};
    const LUID sameModelDifferentDevice = {0x5678, 7};
    if (!sameAdapterIdentity(adapter, alias) ||
        sameAdapterIdentity(adapter, sameModelDifferentDevice))
        throw std::runtime_error("DXGI aliases must deduplicate by LUID while distinct same-model GPUs remain separate");
    auto expectColor = [](std::array<float, 3> color, std::array<float, 3> expected,
                          const char *name) {
        for (int channel = 0; channel < 3; ++channel)
            if (std::abs(color[channel] - expected[channel]) > 1.f / 255.f + 1e-6f)
                throw std::runtime_error(std::string(name) + " color table endpoint mismatch");
    };
    expectColor(sampleColorGradient(7, 0), {0, 0, 4 / 255.f}, "Magma");
    expectColor(sampleColorGradient(8, 0), {68 / 255.f, 1 / 255.f, 84 / 255.f}, "Viridis");
    expectColor(sampleColorGradient(9, 0), {13 / 255.f, 8 / 255.f, 135 / 255.f}, "Plasma");
    expectColor(sampleColorGradient(8, 1), {253 / 255.f, 231 / 255.f, 37 / 255.f}, "Viridis");
    for (int gradient = 0; gradient < colorGradientCount; ++gradient)
        for (int sample = 0; sample <= 256; ++sample) {
            auto color = sampleColorGradient(gradient, float(sample) / 256.f);
            for (float channel : color)
                if (!std::isfinite(channel) || channel < 0 || channel > 1)
                    throw std::runtime_error("Color gradient sample must be finite and normalized");
        }
    HWND w = CreateWindowExW(0, L"STATIC", L"AtomX shape validation", WS_OVERLAPPEDWINDOW, 0, 0,
                             256, 256, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    try {
        Renderer renderer;
        renderer.init(w, argc > 1 ? std::stoi(argv[1]) : -1);
        std::cout << "adapter=";
        for (auto c : renderer.adapterName)
            std::cout << char(c < 128 ? c : '?');
        std::cout << '\n';
        atomx::Dataset d;
        d.species = {"X"};
        d.atoms = {{0, 0, 0, 0}};
        d.bounds();
        renderer.upload(d, {});
        renderer.resetStyles(1);
        Target t;
        renderer.target(t, 256, 256);
        Camera cam;
        cam.mode = 7;
        float bg[] = {0, 0, 0, 1};
        std::set<uint64_t> hashes;
        std::filesystem::create_directories("build/shape-validation");
        {
            std::ofstream file("build/shape-validation/tetra.obj");
            file
                << "v 1 1 1\nv -1 -1 1\nv -1 1 -1\nv 1 -1 -1\nf 1 2 3\nf 1 4 2\nf 1 3 4\nf 2 4 3\n";
        }
        renderer.loadParticleMesh("build/shape-validation/tetra.obj");
        for (int shape = 0; shape < 7; ++shape) {
            renderer.styles[0].axes = {1, 1, 1.5f, 0};
            renderer.draw(t, d, cam, .3f, shape, 0, 0, 0, 0, 1, false, false, false, bg);
            auto output = std::filesystem::path("build/shape-validation") /
                          ("shape-" + std::to_string(shape) + ".png");
            renderer.png(t, output);
            D3D11_TEXTURE2D_DESC td{};
            t.texture->GetDesc(&td);
            td.Usage = D3D11_USAGE_STAGING;
            td.BindFlags = 0;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> readback;
            check(renderer.device->CreateTexture2D(&td, nullptr, &readback), "Readback");
            renderer.context->CopyResource(readback.Get(), t.texture.Get());
            D3D11_MAPPED_SUBRESOURCE map{};
            check(renderer.context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &map), "Map");
            uint64_t hash = 1469598103934665603ULL;
            int lit = 0;
            for (int y = 0; y < 256; ++y)
                for (int x = 0; x < 256; ++x) {
                    auto p = (unsigned char *)map.pData + y * map.RowPitch + x * 4;
                    for (int c = 0; c < 3; ++c) {
                        hash ^= p[c];
                        hash *= 1099511628211ULL;
                    }
                    if (p[0] + p[1] + p[2] > 0)
                        ++lit;
                }
            renderer.context->Unmap(readback.Get(), 0);
            t.depth->GetDesc(&td);
            td.Usage = D3D11_USAGE_STAGING;
            td.BindFlags = 0;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> depth;
            check(renderer.device->CreateTexture2D(&td, nullptr, &depth), "Depth readback");
            renderer.context->CopyResource(depth.Get(), t.depth.Get());
            check(renderer.context->Map(depth.Get(), 0, D3D11_MAP_READ, 0, &map), "Depth map");
            bool valid = true;
            for (int y = 0; y < 256; ++y)
                for (int x = 0; x < 256; ++x) {
                    float v = ((float *)((unsigned char *)map.pData + y * map.RowPitch))[x];
                    valid = valid && std::isfinite(v) && v >= 0 && v <= 1;
                }
            renderer.context->Unmap(depth.Get(), 0);
            if (!valid)
                throw std::runtime_error("Non-finite or invalid shape depth");
            if (lit < 100)
                throw std::runtime_error("Shape is invisible");
            hashes.insert(hash);
            std::cout << "shape=" << shape << " lit=" << lit << " hash=" << hash << '\n';
        }
        if (hashes.size() != 7)
            throw std::runtime_error("Shape images must differ");
        auto imageHash = [&](const Target &target,int minX=0,int maxX=INT_MAX) {
            D3D11_TEXTURE2D_DESC td{};
            target.texture->GetDesc(&td);
            td.Usage = D3D11_USAGE_STAGING;
            td.BindFlags = 0;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> readback;
            check(renderer.device->CreateTexture2D(&td, nullptr, &readback), "Color readback");
            renderer.context->CopyResource(readback.Get(), target.texture.Get());
            D3D11_MAPPED_SUBRESOURCE map{};
            check(renderer.context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &map), "Color map");
            uint64_t hash = 1469598103934665603ULL;
            for (int y = 0; y < target.h; ++y)
                for (int x = std::max(0,minX); x < std::min(target.w,maxX); ++x) {
                    auto pixel = (unsigned char *)map.pData + y * map.RowPitch + x * 4;
                    for (int c = 0; c < 3; ++c) { hash ^= pixel[c]; hash *= 1099511628211ULL; }
                }
            renderer.context->Unmap(readback.Get(), 0);
            return hash;
        };
        auto litInXRange = [&](const Target &target, int minX, int maxX) {
            D3D11_TEXTURE2D_DESC td{};
            target.texture->GetDesc(&td);
            td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> readback;
            check(renderer.device->CreateTexture2D(&td, nullptr, &readback), "Fit color readback");
            renderer.context->CopyResource(readback.Get(), target.texture.Get());
            D3D11_MAPPED_SUBRESOURCE map{};
            check(renderer.context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &map), "Fit color map");
            uint64_t lit = 0;
            for (int y = 0; y < target.h; ++y)
                for (int x = std::clamp(minX, 0, target.w); x < std::clamp(maxX, 0, target.w); ++x) {
                    const auto *pixel = (const unsigned char *)map.pData + y * map.RowPitch + x * 4;
                    if (pixel[0] || pixel[1] || pixel[2]) ++lit;
                }
            renderer.context->Unmap(readback.Get(), 0);
            return lit;
        };
        auto litXBounds = [&](const Target &target) {
            D3D11_TEXTURE2D_DESC td{};
            target.texture->GetDesc(&td);
            td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> readback;
            check(renderer.device->CreateTexture2D(&td, nullptr, &readback), "Fit bounds readback");
            renderer.context->CopyResource(readback.Get(), target.texture.Get());
            D3D11_MAPPED_SUBRESOURCE map{};
            check(renderer.context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &map), "Fit bounds map");
            int lo = target.w, hi = -1;
            for (int y = 0; y < target.h; ++y)
                for (int x = 0; x < target.w; ++x) {
                    const auto *pixel = (const unsigned char *)map.pData + y * map.RowPitch + x * 4;
                    if (pixel[0] || pixel[1] || pixel[2]) { lo = std::min(lo, x); hi = std::max(hi, x); }
                }
            renderer.context->Unmap(readback.Get(), 0);
            return std::pair{lo, hi};
        };
        {
            atomx::Dataset masked;
            masked.species={"X"}; masked.atoms={{-1,0,0,0},{0,0,0,0},{1,0,0,0}};
            masked.bonds={{0,1,{}},{1,2,{}}}; masked.bounds();
            renderer.upload(masked,{}, {}, {1,0,0});
            renderer.draw(t,masked,cam,.3f,0,0,0,0,0,1,false,false,false,bg);
            const auto hiddenEndpoint=imageHash(t);
            auto explicitFiltered=masked; explicitFiltered.bonds={{1,2,{}}};
            renderer.upload(explicitFiltered,{}, {}, {1,0,0});
            renderer.draw(t,explicitFiltered,cam,.3f,0,0,0,0,0,1,false,false,false,bg);
            if (imageHash(t)!=hiddenEndpoint) throw std::runtime_error("Bonds attached to hidden atoms must be omitted");
            renderer.upload(masked,{1,1,1}, {1,1,1}, {1,1,1});
            if (renderer.bondsUploaded()) throw std::runtime_error("All hidden atoms omit all bonds");
            renderer.draw(t,masked,cam,.3f,0,0,0,0,0,1,false,false,false,bg);
            const auto allHidden=imageHash(t);
            renderer.draw(t,masked,cam,.3f,0,0,0,0,0,1,false,false,false,bg,false);
            if (imageHash(t)!=allHidden) throw std::runtime_error("Hidden GPU atoms must write no color fragments");
            renderer.upload(masked,{},{});
            renderer.draw(t,masked,cam,.3f,0,0,0,0,0,1,false,false,false,bg);
            if (imageHash(t)==allHidden || masked.atoms[0].type!=0)
                throw std::runtime_error("Show all restores particles without changing source type bits");
        }
        atomx::Dataset fitData;
        fitData.species={"X"}; fitData.atoms={{0,0,0,0},{100,0,0,0}}; fitData.bounds();
        renderer.upload(fitData,{});
        Camera fitAll=cam; fitAll.mode=2;
        renderer.draw(t,fitData,fitAll,.3f,0,0,0,0,0,1,false,false,false,bg);
        const auto allParticleFit=imageHash(t);
        Camera fitSelected=fitAll; fitSelected.fitSelected=true;
        fitSelected.fitLo={-.6f,-.6f,-.6f}; fitSelected.fitHi={.6f,.6f,.6f};
        renderer.draw(t,fitData,fitSelected,.3f,0,0,0,0,0,1,false,false,false,bg);
        if (imageHash(t)==allParticleFit)
            throw std::runtime_error("Fit selected camera bounds must frame the selected region independently of outliers");
        Target portrait;
        renderer.target(portrait, 256, 512);
        renderer.resetStyles(1);
        Camera perspectiveFit;
        perspectiveFit.mode = 7;
        perspectiveFit.yaw = 0;
        perspectiveFit.pitch = 0;
        renderer.draw(portrait, fitData, perspectiveFit, .3f, 0, 0, 0, 0, 0, 1,
                      false, false, false, bg);
        renderer.png(portrait, "build/shape-validation/fit-portrait.png");
        const auto portraitLeft = litInXRange(portrait, 0, portrait.w / 2);
        const auto portraitRight = litInXRange(portrait, portrait.w / 2, portrait.w);
        std::cout << "portrait fit lit halves=" << portraitLeft << "," << portraitRight << '\n';
        if (portraitLeft == 0 || portraitRight == 0)
            throw std::runtime_error("Perspective Fit must keep both ends of an elongated structure visible in a portrait viewport");
        Target landscape;
        renderer.target(landscape, 512, 256);
        renderer.draw(landscape, fitData, perspectiveFit, .3f, 0, 0, 0, 0, 0, 1,
                      false, false, false, bg);
        renderer.png(landscape, "build/shape-validation/fit-landscape.png");
        const auto landscapeLeft = litInXRange(landscape, 0, landscape.w / 2);
        const auto landscapeRight = litInXRange(landscape, landscape.w / 2, landscape.w);
        std::cout << "landscape fit lit halves=" << landscapeLeft << "," << landscapeRight << '\n';
        if (landscapeLeft == 0 || landscapeRight == 0)
            throw std::runtime_error("Perspective Fit must keep both ends of an elongated structure visible in a landscape viewport");
        const auto [landscapeLo, landscapeHi] = litXBounds(landscape);
        if (landscapeHi - landscapeLo < int(landscape.w * .75f))
            throw std::runtime_error("Perspective Fit should use the landscape viewport while preserving the structure's aspect ratio");
        Camera orthographicFit = perspectiveFit;
        orthographicFit.mode = 2;
        renderer.draw(portrait, fitData, orthographicFit, .3f, 0, 0, 0, 0, 0, 1,
                      false, false, false, bg);
        if (litInXRange(portrait, 0, portrait.w / 2) == 0 ||
            litInXRange(portrait, portrait.w / 2, portrait.w) == 0)
            throw std::runtime_error("Orthographic Fit must include particle extents at portrait aspect ratios");
        {
            using namespace DirectX;
            Camera rolled=orthographicFit;rolled.roll=XM_PIDIV2;
            renderer.draw(landscape,fitData,rolled,.3f,0,0,0,0,0,1,false,false,false,bg);
            const auto [lo,hi]=litXBounds(landscape);
            if(hi<lo || hi-lo>int(landscape.w*.1f))
                throw std::runtime_error("Screen roll must rotate the elongated GPU image vertically");
            for(int mode:{2,7}) for(float aspect:{.5f,2.f}) for(float roll:{-.7f,.7f,XM_PIDIV2}) {
                rolled.mode=mode;rolled.roll=roll;
                const auto matrix=renderer.matrix(fitData,rolled,aspect,false,.3f);
                const auto inverse=XMMatrixInverse(nullptr,matrix);
                for(const auto &atom:fitData.atoms) {
                    XMFLOAT3 screen,restored;
                    const auto at=XMVectorSet(atom.x,atom.y,atom.z,1);
                    const auto projected=XMVector3TransformCoord(at,matrix);
                    XMStoreFloat3(&screen,projected);
                    XMStoreFloat3(&restored,XMVector3TransformCoord(projected,inverse));
                    if(std::abs(screen.x)>.96f || std::abs(screen.y)>.96f || screen.z<0 || screen.z>1 ||
                       std::abs(restored.x-atom.x)>.02f || std::abs(restored.y-atom.y)>.02f || std::abs(restored.z-atom.z)>.02f)
                        throw std::runtime_error("Rolled cameras must fit both ends and support screen-to-world editing");
                }
            }
            Camera panRoll=orthographicFit;panRoll.roll=.7f;
            XMMATRIX beforeView,afterView;
            renderer.matrix(fitData,panRoll,2,false,.3f,&beforeView);
            panRoll.panX=.1f;renderer.matrix(fitData,panRoll,2,false,.3f,&afterView);
            XMFLOAT3 before,after;
            XMStoreFloat3(&before,XMVector3TransformCoord(XMVectorZero(),beforeView));
            XMStoreFloat3(&after,XMVector3TransformCoord(XMVectorZero(),afterView));
            if(after.x<=before.x || std::abs(after.y-before.y)>1e-5f)
                throw std::runtime_error("Pan must remain horizontal in screen space after rolling");
        }
        atomx::Dataset coded;
        coded.species = {"X"};
        coded.atoms = {{-.5f, 0, 0, 0}, {.5f, 0, 0, 0}};
        coded.scalarProperties["Color coding"] = {0, 1};
        coded.bounds();
        renderer.upload(coded, {});
        std::set<uint64_t> paletteImages;
        uint64_t firstPropertyImage = 0;
        for (int gradient = 0; gradient < colorGradientCount; ++gradient) {
            renderer.draw(t, coded, cam, .3f, 0, 0, 0, gradient, 0, 1, true, false, false, bg);
            const auto hash = imageHash(t);
            if (gradient == 0) firstPropertyImage = hash;
            paletteImages.insert(hash);
        }
        if (paletteImages.size() < 8)
            throw std::runtime_error("GPU color gradients must render distinct palettes from the shared color tables");
        coded.scalarProperties["Color coding"] = {1, 0};
        renderer.upload(coded, {});
        renderer.draw(t, coded, cam, .3f, 0, 0, 0, 0, 0, 1, true, false, false, bg);
        if (imageHash(t) == firstPropertyImage)
            throw std::runtime_error("GPU color coding must follow the selected particle property");
        coded.atoms={{0,0,0,0}};
        coded.scalarProperties["Color coding"] = {-std::numeric_limits<float>::max()};
        renderer.upload(coded, {});
        renderer.draw(t,coded,cam,.3f,0,0,0,8,-std::numeric_limits<float>::max(),
                      std::numeric_limits<float>::max(),true,false,false,bg);
        const auto wideNegative=imageHash(t);
        coded.scalarProperties["Color coding"] = {-1};
        renderer.upload(coded, {});
        renderer.draw(t,coded,cam,.3f,0,0,0,8,-1,1,true,false,false,bg);
        const auto narrowNegative=imageHash(t);
        coded.scalarProperties["Color coding"] = {1};
        renderer.upload(coded, {});
        renderer.draw(t,coded,cam,.3f,0,0,0,8,-1,1,true,false,false,bg);
        const auto narrowPositive=imageHash(t);
        if (narrowPositive==narrowNegative)
            throw std::runtime_error("Single-particle color coding fixture must visibly distinguish low and high property values");
        coded.scalarProperties["Color coding"] = {std::numeric_limits<float>::max()};
        renderer.upload(coded, {});
        renderer.draw(t,coded,cam,.3f,0,0,0,8,-std::numeric_limits<float>::max(),
                      std::numeric_limits<float>::max(),true,false,false,bg);
        const auto widePositive=imageHash(t);
        if (widePositive==wideNegative)
            throw std::runtime_error("GPU color coding must distinguish both ends of the widest finite float range");
        coded.atoms={{-.5f,0,0,0},{.5f,0,0,0}};
        coded.bounds();
        coded.scalarProperties.clear();
        coded.particleColors={{1,0,0},{0,1,0}};
        renderer.upload(coded,{});
        renderer.draw(t,coded,cam,.3f,0,0,0,0,0,1,false,false,false,bg);
        const auto assignedColors=imageHash(t);
        coded.particleColors={{0,0,1},{1,1,0}};
        renderer.upload(coded,{});
        renderer.draw(t,coded,cam,.3f,0,0,0,0,0,1,false,false,false,bg);
        if (imageHash(t)==assignedColors)
            throw std::runtime_error("Per-particle assigned colors must reach GPU rendering");
        renderer.upload(coded, {1, 0});
        renderer.draw(t, coded, cam, .3f, 0, 0, 0, 0, 0, 1, true, false, true, bg);
        auto selectedOnlyImage = imageHash(t);
        renderer.draw(t, coded, cam, .3f, 0, 0, 0, 0, 0, 1, true, false, false, bg);
        if (imageHash(t) == selectedOnlyImage)
            throw std::runtime_error("GPU selected-only color coding must preserve unselected type colors");
        atomx::Dataset bonded;
        bonded.species = {"X"};
        bonded.atoms = {{-.7f,0,0,0},{.7f,0,0,0}};
        bonded.bounds();
        renderer.upload(bonded,{});
        renderer.draw(t,bonded,cam,.18f,0,0,0,0,0,1,false,false,false,bg);
        const auto withoutBonds=imageHash(t);
        bonded.bonds.push_back({0,1,{0,0,0}});
        renderer.upload(bonded,{});
        renderer.draw(t,bonded,cam,.18f,0,0,0,0,0,1,false,false,false,bg);
        if (imageHash(t)==withoutBonds)
            throw std::runtime_error("GPU bond lines must be visible in viewport rendering");
        bonded.bondStyle.visible=false;
        renderer.upload(bonded,{});
        renderer.draw(t,bonded,cam,.18f,0,0,0,0,0,1,false,false,false,bg);
        if (imageHash(t)!=withoutBonds)
            throw std::runtime_error("Bond visibility setting must hide the bond geometry");
        bonded.bondStyle.visible=true;
        bonded.bondStyle.color={1,0,0,1};
        bonded.bondStyle.width=5;
        renderer.upload(bonded,{});
        renderer.draw(t,bonded,cam,.18f,0,0,0,0,0,1,false,false,false,bg);
        const auto styledBondImage=imageHash(t);
        bonded.bondStyle.radius=.08f;
        renderer.draw(t,bonded,cam,.18f,0,0,0,0,0,1,false,false,false,bg);
        const auto cylinderBondImage=imageHash(t);
        if (cylinderBondImage==styledBondImage)
            throw std::runtime_error("3D bond cylinders must render differently from screen-space lines");
        bonded.bondStyle.radius=.16f;
        renderer.draw(t,bonded,cam,.18f,0,0,0,0,0,1,false,false,false,bg);
        if (imageHash(t)==cylinderBondImage)
            throw std::runtime_error("Bond cylinder radius must change rendered coverage");
        bonded.bondStyle.radius=0;
        bonded.bondStyle.width=.5f;
        renderer.draw(t,bonded,cam,.18f,0,0,0,0,0,1,false,false,false,bg);
        if (imageHash(t)==styledBondImage)
            throw std::runtime_error("Bond width setting must change line coverage");
        bonded.bondStyle.width=5;
        bonded.bondStyle.color={0,1,0,1};
        renderer.draw(t,bonded,cam,.18f,0,0,0,0,0,1,false,false,false,bg);
        if (imageHash(t)==styledBondImage)
            throw std::runtime_error("Bond appearance settings must change rendered output");
        bonded.bonds.assign(atomx::interactiveBondBudget+1, {0,1,{0,0,0}});
        renderer.upload(bonded,{});
        if (!renderer.bondsOmittedForPerformance() || renderer.bondsUploaded())
            throw std::runtime_error("Oversized external bond topology must skip GPU geometry upload");
        bonded.bonds.resize(1);
        renderer.upload(bonded,{});
        atomx::Dataset typed;
        typed.species = {"A", "B"};
        typed.atoms = {{-.55f, 0, 0, 0}, {.55f, 0, 0, 1}};
        typed.bounds();
        renderer.upload(typed, {});
        renderer.resetStyles(2);
        renderer.styles[0].color = {1, .05f, .05f, 1};
        renderer.styles[0].visual = {.22f, 0, 1, 0};
        renderer.styles[0].axes = {1, 1, 1, 0};
        renderer.styles[1].color = {.05f, .2f, 1, 1};
        renderer.styles[1].visual = {.42f, 2, 1, 0};
        renderer.styles[1].axes = {1.8f, .65f, 1, 0};
        typed.bonds.push_back({0,1,{0,0,0}});
        typed.bondStyle.radius=.14f;
        typed.bondStyle.colorByType=true;
        renderer.upload(typed, {});
        renderer.draw(t, typed, cam, .3f, 0, 0, 0, 0, 0, 1, false, false, false, bg);
        const auto splitColorBondImage = imageHash(t);
        typed.bondStyle.colorByType=false;
        renderer.draw(t, typed, cam, .3f, 0, 0, 0, 0, 0, 1, false, false, false, bg);
        if (imageHash(t) == splitColorBondImage)
            throw std::runtime_error("Bond cylinders must honor particle-colored and uniform modes");
        typed.bonds.clear();
        {
            atomx::Dataset ordered;
            ordered.species={"X"}; ordered.atoms={{-2,0,0,0},{2,0,0,0}};
            ordered.bonds={{0,1,{}}}; ordered.bounds();
            renderer.styles[0].visual={.22f,0,1,0}; renderer.styles[0].axes={1,1,1,0};
            Camera front=cam; front.mode=2;
            for (float cylinderRadius:{0.f,.1f}) {
                ordered.bondStyle.radius=cylinderRadius;
                std::set<uint64_t> orderImages;
                for (int order=1;order<=4;++order) {
                    ordered.bonds[0].order=uint8_t(order);
                    renderer.upload(ordered,{});
                    renderer.draw(t,ordered,front,.22f,0,0,0,0,0,1,false,false,false,bg);
                    orderImages.insert(imageHash(t));
                    renderer.png(t,"build/shape-validation/bond-order-"+std::to_string(order)+
                        (cylinderRadius>0?"-cylinder.png":"-line.png"));
                }
                if (orderImages.size()!=4)
                    throw std::runtime_error("Single/double/triple/aromatic bonds must produce distinct GPU strand images");
            }
            atomx::creation::Display attached;attached.defaultPreset=3;attached.ballRadius=.32f;attached.stickRadius=.2f;
            front.fitSelected=true;front.fitLo={-5,-3,-3};front.fitHi={5,3,3};
            auto verticalBounds=[&] {
                D3D11_TEXTURE2D_DESC desc{};t.texture->GetDesc(&desc);desc.BindFlags=0;desc.MiscFlags=0;
                desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                ComPtr<ID3D11Texture2D> copy;check(renderer.device->CreateTexture2D(&desc,nullptr,&copy),"Attached bond readback");
                renderer.context->CopyResource(copy.Get(),t.texture.Get());D3D11_MAPPED_SUBRESOURCE map{};
                check(renderer.context->Map(copy.Get(),0,D3D11_MAP_READ,0,&map),"Attached bond pixels");
                int lo=int(desc.Height),hi=-1;
                for(UINT y=0;y<desc.Height;++y)for(UINT x=0;x<desc.Width;++x) {
                    const auto *p=static_cast<const unsigned char *>(map.pData)+y*map.RowPitch+x*4;
                    if(std::max({p[0],p[1],p[2]})>8){lo=std::min(lo,int(y));hi=std::max(hi,int(y));}
                }
                renderer.context->Unmap(copy.Get(),0);return std::pair{lo,hi};
            };
            ordered.bondStyle.visible=false;renderer.upload(ordered,{},{},{},&attached);
            renderer.draw(t,ordered,front,.3f,0,0,0,0,0,1,false,false,false,bg,true,false,.43f,&attached);
            const auto sphereBounds=verticalBounds();ordered.bondStyle.visible=true;
            for(int order:{2,3,4}) {
                ordered.bonds[0].order=uint8_t(order);renderer.upload(ordered,{},{},{},&attached);
                renderer.draw(t,ordered,front,.3f,0,0,0,0,0,1,false,false,false,bg,true,false,.43f,&attached);
                const auto bounds=verticalBounds();
                if(bounds.first<sphereBounds.first-2 || bounds.second>sphereBounds.second+2)
                    throw std::runtime_error("Multiple bond strands must stay within the endpoint sphere envelope at default stick radius");
                renderer.png(t,"build/shape-validation/attached-bond-"+std::to_string(order)+".png");
            }
        }
        {
            atomx::Dataset styled;
            styled.species={"C","O"}; styled.atoms={{-2,0,0,0},{2,0,0,1},{0,0,100,0}};
            styled.bonds={{0,1,{},1}}; styled.bounds(); styled.bondStyle.radius=.12f;
            renderer.resetStyles(2,&styled.species);
            atomx::creation::Display display;
            Camera front=cam; front.mode=2; front.fitSelected=true;
            front.fitLo={-5,-3,-3}; front.fitHi={5,3,3};
            const auto originalStyles=renderer.styles;
            std::set<uint64_t> images;
            for(int preset=0;preset<=4;++preset) {
                display.setPreset(2,{},uint8_t(preset),true);
                renderer.upload(styled,{},{},{},&display);
                renderer.draw(t,styled,front,.3f,0,0,0,0,0,1,false,false,false,bg,true,false,.43f,&display);
                images.insert(imageHash(t));
                renderer.png(t,"build/shape-validation/preset-"+std::to_string(preset)+".png");
            }
            if(images.size()!=5 || renderer.styles!=originalStyles || styled.atoms[1].type!=1)
                throw std::runtime_error("GPU presets must render distinct images without altering source or saved styles");
            display.setPreset(3,{},0,true);
            // An offscreen CPK atom establishes the same conservative fit
            // margin before and after changing the selected foreground atom.
            display.setPreset(3,{2},4,false);
            renderer.upload(styled,{},{},{0,0,1},&display);
            renderer.draw(t,styled,front,.3f,0,0,0,0,0,1,false,false,false,bg,true,false,.43f,&display);
            const auto originalRight=litInXRange(t,155,256), originalImage=imageHash(t);
            display.setPreset(3,{0},4,false);
            renderer.upload(styled,{},{},{0,0,1},&display);
            renderer.draw(t,styled,front,.3f,0,0,0,0,0,1,false,false,false,bg,true,false,.43f,&display);
            if(imageHash(t)==originalImage || litInXRange(t,155,256)!=originalRight)
                throw std::runtime_error("Selected CPK must preserve unselected atom and bond-half coverage with fixed camera bounds");
            renderer.png(t,"build/shape-validation/preset-mixed.png");
            display={}; styled.bondStyle.colorByType=true;
            auto colorImage=[&]() {
                renderer.upload(styled,{},{},{0,0,1},&display);
                renderer.draw(t,styled,front,.3f,0,0,0,0,0,1,false,false,false,bg,true,false,.43f,&display);
                return imageHash(t);
            };
            const auto sourceImage=colorImage(),sourceRight=imageHash(t,155,256);
            atomx::creation::ColorRule cr;cr.kind=atomx::creation::ColorKind::Custom;cr.rgb={0,1,0};
            display.setColor(3,{0},cr,false);
            if(colorImage()==sourceImage || imageHash(t,155,256)!=sourceRight)
                throw std::runtime_error("Selected custom colors must change atom/half-bond pixels while preserving the other side exactly");
            cr.kind=atomx::creation::ColorKind::Property;styled.scalarProperties["Charge"]={-1,1,NAN};
            display.setColor(3,{},cr,true);const auto mappedImage=colorImage();
            styled.scalarProperties["Charge"]={1,-1,NAN};
            if(colorImage()==mappedImage) throw std::runtime_error("Creation property colors must refresh with current data on GPU upload");
            display={};if(colorImage()!=sourceImage || renderer.styles!=originalStyles)
                throw std::runtime_error("Returning to source colors must exactly restore GPU appearance and element styles");
        }
        renderer.resetStyles(2,&typed.species);
        renderer.upload(typed, {});
        renderer.draw(t, typed, cam, .3f, 0, 0, 0, 0, 0, 1, false, false, false, bg);
        const auto perTypeImage = imageHash(t);
        renderer.styles[0].visual[2] = 0;
        renderer.draw(t, typed, cam, .3f, 0, 0, 0, 0, 0, 1, false, false, false, bg);
        const auto hiddenTypeImage = imageHash(t);
        if (hiddenTypeImage == perTypeImage)
            throw std::runtime_error("Per-type visibility, color, radius, shape and axes must render independently");
        renderer.styles[0].visual[2] = 1;
        renderer.styles[1].color = {.05f, 1, .1f, 1};
        renderer.draw(t, typed, cam, .3f, 0, 0, 0, 0, 0, 1, false, false, false, bg);
        if (imageHash(t) == perTypeImage)
            throw std::runtime_error("Changing one particle type appearance must update GPU output");
        renderer.upload(d, {});
        renderer.styles[0].visual[2] = 0;
        renderer.draw(t, d, cam, .3f, 0, 0, 0, 0, 0, 1, false, false, false, bg);
        renderer.png(t, "build/shape-validation/hidden.png");
        renderer.styles[0].visual[2] = 1;
        renderer.draw(t,d,cam,.3f,0,0,0,8,-1,1,true,false,false,bg);
        ColorLegendOptions legend{true,"Position.X",8,-1,1,false,false};
        auto legendPath=std::filesystem::path("build/shape-validation/color-legend.png");
        renderer.png(t,legendPath,legend);
        if (!std::filesystem::exists(legendPath) || std::filesystem::file_size(legendPath)<1000)
            throw std::runtime_error("Color legend export must produce a visible PNG artifact");
        legend.reverse=true; legend.discrete=true;
        auto reversedLegendPath=std::filesystem::path("build/shape-validation/color-legend-reversed-discrete.png");
        renderer.png(t,reversedLegendPath,legend);
        auto readFile=[](const std::filesystem::path &file) {
            std::ifstream in(file,std::ios::binary);
            return std::vector<char>((std::istreambuf_iterator<char>(in)),{});
        };
        if (readFile(legendPath)==readFile(reversedLegendPath))
            throw std::runtime_error("PNG color legend must reflect reversed/discrete gradient settings");
        std::cout << "PASS: seven GPU shapes, per-type styles, property/assigned colors, bond lines/cylinders, legends, color, width and visibility rendered\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        DestroyWindow(w);
        return 1;
    }
    DestroyWindow(w);
    CoUninitialize();
    return 0;
}
