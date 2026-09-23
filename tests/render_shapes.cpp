#include "../src/renderer.hpp"
#include <iostream>
#include <set>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
int main(int argc, char **argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
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
        auto imageHash = [&](const Target &target) {
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
                for (int x = 0; x < target.w; ++x) {
                    auto pixel = (unsigned char *)map.pData + y * map.RowPitch + x * 4;
                    for (int c = 0; c < 3; ++c) { hash ^= pixel[c]; hash *= 1099511628211ULL; }
                }
            renderer.context->Unmap(readback.Get(), 0);
            return hash;
        };
        atomx::Dataset coded;
        coded.species = {"X"};
        coded.atoms = {{-.5f, 0, 0, 0}, {.5f, 0, 0, 0}};
        coded.scalarProperties["Color coding"] = {0, 1};
        coded.bounds();
        renderer.upload(coded, {});
        renderer.draw(t, coded, cam, .3f, 0, 0, 0, 0, 0, 1, true, false, false, bg);
        auto firstPropertyImage = imageHash(t);
        coded.scalarProperties["Color coding"] = {1, 0};
        renderer.upload(coded, {});
        renderer.draw(t, coded, cam, .3f, 0, 0, 0, 0, 0, 1, true, false, false, bg);
        if (imageHash(t) == firstPropertyImage)
            throw std::runtime_error("GPU color coding must follow the selected particle property");
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
        bonded.bondStyle.width=.5f;
        renderer.draw(t,bonded,cam,.18f,0,0,0,0,0,1,false,false,false,bg);
        if (imageHash(t)==styledBondImage)
            throw std::runtime_error("Bond width setting must change line coverage");
        bonded.bondStyle.width=5;
        bonded.bondStyle.color={0,1,0,1};
        renderer.draw(t,bonded,cam,.18f,0,0,0,0,0,1,false,false,false,bg);
        if (imageHash(t)==styledBondImage)
            throw std::runtime_error("Bond appearance settings must change rendered output");
        renderer.upload(d, {});
        renderer.styles[0].visual[2] = 0;
        renderer.draw(t, d, cam, .3f, 0, 0, 0, 0, 0, 1, false, false, false, bg);
        renderer.png(t, "build/shape-validation/hidden.png");
        std::cout << "PASS: seven GPU shapes, property/assigned colors, bond lines, color, width and visibility rendered\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        DestroyWindow(w);
        return 1;
    }
    DestroyWindow(w);
    CoUninitialize();
    return 0;
}
