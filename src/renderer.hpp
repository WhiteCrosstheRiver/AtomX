#pragma once
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <wincodec.h>
#include "core.hpp"
using Microsoft::WRL::ComPtr;
inline void check(HRESULT hr, const char *message) {
    if (FAILED(hr))
        throw std::runtime_error(std::string(message) + " (HRESULT " +
                                 std::to_string(uint32_t(hr)) + ")");
}
struct Camera {
    float yaw = .65f, pitch = .48f, zoom = 1.0f, panX = 0, panY = 0;
    int mode = 7;
};
struct Target {
    ComPtr<ID3D11Texture2D> texture, depth;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11DepthStencilView> dsv;
    int w = 0, h = 0;
};
struct Adapter {
    std::wstring name;
    uint64_t memory;
    UINT index;
};
class Renderer {
    struct Chunk {
        ComPtr<ID3D11Buffer> buffer;
        ComPtr<ID3D11ShaderResourceView> srv;
        UINT count;
    };
    struct Constants {
        DirectX::XMFLOAT4X4 view, projection;
        float radius, shape, colorAxis, colorMin;
        float colorMax, colorMode, colorDiscrete, unused;
        DirectX::XMFLOAT4 colors[8];
    };
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11RasterizerState> wireRaster;
    ComPtr<ID3D11DepthStencilState> depthState;
    std::vector<Chunk> chunks;

  public:
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swap;
    ComPtr<ID3D11RenderTargetView> back;
    std::vector<Adapter> adapters;
    std::wstring adapterName;
    uint64_t gpuBytes = 0;
    bool software = false;
    void init(HWND window, int requested = -1) {
        ComPtr<IDXGIFactory1> factory;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
        ComPtr<IDXGIAdapter1> chosen;
        SIZE_T best = 0;
        for (UINT i = 0;; i++) {
            ComPtr<IDXGIAdapter1> a;
            if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                continue;
            adapters.push_back({d.Description, d.DedicatedVideoMemory, i});
            if ((requested < 0 && (!chosen || d.DedicatedVideoMemory > best)) ||
                requested == int(i)) {
                chosen = a;
                best = d.DedicatedVideoMemory;
                adapterName = d.Description;
            }
        }
        if (requested >= 0 && !std::any_of(adapters.begin(), adapters.end(),
                                           [&](auto a) { return a.index == UINT(requested); }))
            throw std::runtime_error("Requested adapter is unavailable");
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount = 2;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = window;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        D3D_FEATURE_LEVEL level;
        D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
        check(D3D11CreateDeviceAndSwapChain(
                  chosen.Get(), chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                  nullptr, 0, levels, 1, D3D11_SDK_VERSION, &sd, &swap, &device, &level, &context),
              "Create GPU device (feature level 11.0 required)");
        factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
        resize();
        const char *shader = R"(
cbuffer C : register(b0) {row_major float4x4 view;row_major float4x4 proj;float radius;float shape;float colorAxis;float colorMin;float colorMax;float colorMode;float colorDiscrete;float unused;float4 colors[8];};
struct Atom {float3 pos;uint type;};StructuredBuffer<Atom> atoms : register(t0);
struct V {float4 pos:SV_POSITION;float2 uv:TEXCOORD0;float3 center:TEXCOORD1;nointerpolation uint type:TEXCOORD2;nointerpolation float3 world:TEXCOORD3;};
V vertex(uint id:SV_VertexID,uint instance:SV_InstanceID) {
 float2 q[6]={float2(-1,-1),float2(-1,1),float2(1,-1),float2(1,-1),float2(-1,1),float2(1,1)};
 Atom a=atoms[instance];V o;o.center=mul(float4(a.pos,1),view).xyz;o.world=a.pos;o.uv=q[id];o.type=a.type;
 o.pos=mul(float4(o.center+float3(o.uv*radius,0),1),proj);return o;
}
struct P {float4 color:SV_TARGET;float depth:SV_DEPTH;};
P pixel(V i) {float r=dot(i.uv,i.uv);if(shape<0.5) clip(1-r); else if(shape<1.5) { clip(1-r); } else if(shape<2.5) { } else if(shape<3.5) { clip(1-abs(i.uv.x)); } else { clip(1-r); } float3 n=(shape<0.5||shape>3.5)?float3(i.uv,sqrt(max(0,1-r))):float3(0,0,1);float3 p=i.center+n*radius;
 float4 clipPos=mul(float4(p,1),proj);P o;o.depth=clipPos.z/clipPos.w;
 float3 base=(i.type&0x80000000)?float3(1,.83,.32):colors[(i.type&0x7fffffff)%8].rgb;
 if(colorMode>0.5 && (colorMode<1.5 || (i.type&0x80000000))) { float value = colorAxis<0.5 ? i.world.x : colorAxis<1.5 ? i.world.y : i.world.z; float u=saturate((value-colorMin)/max(colorMax-colorMin,1e-12)); if(colorDiscrete>0.5) u=floor(u*12)/11; float3 c0=float3(0.10,.15,.85), c1=float3(.12,.85,.75), c2=float3(.98,.88,.08), c3=float3(.9,.08,.04); base=u<.5?lerp(c0,c1,u*2):u<.8?lerp(c1,c2,(u-.5)*3.333):lerp(c2,c3,(u-.8)*5); }
 float diffuse=max(0,dot(n,normalize(float3(-.45,.65,1))));float rim=pow(1-sqrt(1-r),3);
 float spec=pow(max(0,dot(n,normalize(float3(-.22,.32,1)))),36);
 o.color=float4(base*(.28+.72*diffuse)+spec*.3+rim*.045,1);return o;}
)";
        ComPtr<ID3DBlob> v, p, err;
        auto hr = D3DCompile(shader, strlen(shader), nullptr, nullptr, nullptr, "vertex", "vs_5_0",
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &v, &err);
        if (FAILED(hr))
            throw std::runtime_error(err ? (char *)err->GetBufferPointer()
                                         : "Vertex shader failed");
        check(D3DCompile(shader, strlen(shader), nullptr, nullptr, nullptr, "pixel", "ps_5_0",
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &p, &err),
              "Pixel shader");
        check(device->CreateVertexShader(v->GetBufferPointer(), v->GetBufferSize(), nullptr, &vs),
              "Vertex shader");
        check(device->CreatePixelShader(p->GetBufferPointer(), p->GetBufferSize(), nullptr, &ps),
              "Pixel shader");
        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = sizeof(Constants);
        cb.Usage = D3D11_USAGE_DEFAULT;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        check(device->CreateBuffer(&cb, nullptr, &constants), "Constants");
        D3D11_RASTERIZER_DESC rs{};
        rs.FillMode = D3D11_FILL_SOLID;
        rs.CullMode = D3D11_CULL_NONE;
        rs.DepthClipEnable = TRUE;
        check(device->CreateRasterizerState(&rs, &raster), "Rasterizer");
        rs.FillMode = D3D11_FILL_WIREFRAME;
        check(device->CreateRasterizerState(&rs, &wireRaster), "Wire rasterizer");
        D3D11_DEPTH_STENCIL_DESC ds{};
        ds.DepthEnable = TRUE;
        ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        ds.DepthFunc = D3D11_COMPARISON_LESS;
        check(device->CreateDepthStencilState(&ds, &depthState), "Depth state");
    }
    void resize() {
        if (!swap)
            return;
        context->OMSetRenderTargets(0, nullptr, nullptr);
        back.Reset();
        check(swap->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0), "Resize");
        ComPtr<ID3D11Texture2D> t;
        check(swap->GetBuffer(0, IID_PPV_ARGS(&t)), "Backbuffer");
        check(device->CreateRenderTargetView(t.Get(), nullptr, &back), "Backbuffer view");
    }
    void upload(const atomx::Dataset &d, const std::vector<uint8_t> &selected) {
        std::vector<Chunk> next;
        const size_t block = 1048576;
        for (size_t start = 0; start < d.atoms.size(); start += block) {
            Chunk c;
            c.count = UINT(std::min(block, d.atoms.size() - start));
            std::vector<atomx::Atom> tmp(d.atoms.begin() + start,
                                         d.atoms.begin() + start + c.count);
            for (size_t j = 0; j < tmp.size(); j++)
                if (start + j < selected.size() && selected[start + j])
                    tmp[j].type |= 0x80000000;
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = c.count * sizeof(atomx::Atom);
            bd.Usage = D3D11_USAGE_IMMUTABLE;
            bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            bd.StructureByteStride = sizeof(atomx::Atom);
            D3D11_SUBRESOURCE_DATA data{tmp.data(), 0, 0};
            check(device->CreateBuffer(&bd, &data, &c.buffer), "Atom GPU allocation");
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = DXGI_FORMAT_UNKNOWN;
            sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            sv.Buffer.NumElements = c.count;
            check(device->CreateShaderResourceView(c.buffer.Get(), &sv, &c.srv),
                  "Atom buffer view");
            next.push_back(std::move(c));
        }
        chunks = std::move(next);
        gpuBytes = d.atoms.size() * sizeof(atomx::Atom);
    }
    void target(Target &t, int w, int h) {
        w = std::max(1, w);
        h = std::max(1, h);
        if (t.w == w && t.h == h)
            return;
        t = {};
        t.w = w;
        t.h = h;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        check(device->CreateTexture2D(&td, nullptr, &t.texture), "Viewport texture");
        check(device->CreateRenderTargetView(t.texture.Get(), nullptr, &t.rtv), "Viewport RTV");
        check(device->CreateShaderResourceView(t.texture.Get(), nullptr, &t.srv), "Viewport SRV");
        td.Format = DXGI_FORMAT_D32_FLOAT;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        check(device->CreateTexture2D(&td, nullptr, &t.depth), "Depth texture");
        check(device->CreateDepthStencilView(t.depth.Get(), nullptr, &t.dsv), "Depth view");
    }
    DirectX::XMMATRIX matrix(const atomx::Dataset &d, const Camera &cam, float aspect,
                             DirectX::XMMATRIX *viewOut = nullptr,
                             DirectX::XMMATRIX *projOut = nullptr) {
        using namespace DirectX;
        auto lo = d.lo, hi = d.hi;
        if (d.cell[0] != 0 || d.cell[4] != 0 || d.cell[8] != 0) {
            for (int j = 0; j < 8; j++) {
                float x = float((j & 1 ? d.cell[0] : 0) + (j & 2 ? d.cell[3] : 0) +
                                (j & 4 ? d.cell[6] : 0));
                float y = float((j & 1 ? d.cell[1] : 0) + (j & 2 ? d.cell[4] : 0) +
                                (j & 4 ? d.cell[7] : 0));
                float z = float((j & 1 ? d.cell[2] : 0) + (j & 2 ? d.cell[5] : 0) +
                                (j & 4 ? d.cell[8] : 0));
                lo.x = std::min(lo.x, x);
                lo.y = std::min(lo.y, y);
                lo.z = std::min(lo.z, z);
                hi.x = std::max(hi.x, x);
                hi.y = std::max(hi.y, y);
                hi.z = std::max(hi.z, z);
            }
        }
        auto center = XMVectorSet((lo.x + hi.x) * .5f, (lo.y + hi.y) * .5f, (lo.z + hi.z) * .5f, 0);
        float span = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z, 1.f});
        auto up = XMVectorSet(0, 0, 1, 0);
        auto dir = XMVectorSet(sinf(cam.yaw) * cosf(cam.pitch), -cosf(cam.yaw) * cosf(cam.pitch),
                               sinf(cam.pitch), 0);
        if (cam.mode < 6) {
            const XMFLOAT3 directions[] = {{0, 0, 1}, {0, 0, -1}, {0, -1, 0},
                                           {0, 1, 0}, {-1, 0, 0}, {1, 0, 0}};
            dir = XMLoadFloat3(&directions[cam.mode]);
            if (cam.mode < 2)
                up = XMVectorSet(0, 1, 0, 0);
        }
        float fit = std::max(1.f, 1.f / std::max(aspect, .1f));
        float dist = span * 2.8f * cam.zoom * fit;
        auto v = XMMatrixLookAtRH(center + dir * dist, center, up) *
                 XMMatrixTranslation(cam.panX * span, cam.panY * span, 0);
        auto p = cam.mode == 7 ? XMMatrixPerspectiveFovRH(.65f, aspect, span * .001f, span * 1000)
                               : XMMatrixOrthographicRH(span * 1.8f * cam.zoom * fit * aspect,
                                                        span * 1.8f * cam.zoom * fit, span * .001f,
                                                        span * 1000);
        if (viewOut)
            *viewOut = v;
        if (projOut)
            *projOut = p;
        return v * p;
    }
    void draw(Target &t, const atomx::Dataset &d, const Camera &cam, float radius, int shape, int renderMode, int colorAxis, float colorMin, float colorMax, bool colorCoding, bool discrete, bool selectedOnly, const float *bg,
              bool visible = true) {
        using namespace DirectX;
        context->OMSetRenderTargets(1, t.rtv.GetAddressOf(), t.dsv.Get());
        context->ClearRenderTargetView(t.rtv.Get(), bg);
        context->ClearDepthStencilView(t.dsv.Get(), D3D11_CLEAR_DEPTH, 1, 0);
        D3D11_VIEWPORT vp{0, 0, float(t.w), float(t.h), 0, 1};
        context->RSSetViewports(1, &vp);
        context->RSSetState(renderMode == 1 ? wireRaster.Get() : raster.Get());
        context->OMSetDepthStencilState(depthState.Get(), 0);
        context->OMSetBlendState(nullptr, nullptr, ~0u);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->PSSetShader(ps.Get(), nullptr, 0);
        context->GSSetShader(nullptr, nullptr, 0);
        Constants c{};
        XMMATRIX view, proj;
        matrix(d, cam, float(t.w) / t.h, &view, &proj);
        XMStoreFloat4x4(&c.view, view);
        XMStoreFloat4x4(&c.projection, proj);
        c.radius = radius;
        c.shape = float(shape);
        c.colorAxis = float(colorAxis); c.colorMin=colorMin; c.colorMax=colorMax; c.colorMode=colorCoding?(selectedOnly?2.f:1.f):0.f; c.colorDiscrete=discrete?1.f:0.f;
        XMFLOAT4 colors[] = {{.76f, .57f, .38f, 1}, {.35f, .68f, .78f, 1}, {.62f, .76f, .46f, 1},
                             {.78f, .44f, .52f, 1}, {.69f, .52f, .81f, 1}, {.88f, .76f, .43f, 1},
                             {.47f, .61f, .85f, 1}, {.8f, .8f, .8f, 1}};
        std::copy(std::begin(colors), std::end(colors), c.colors);
        context->UpdateSubresource(constants.Get(), 0, nullptr, &c, 0, 0);
        context->VSSetConstantBuffers(0, 1, constants.GetAddressOf());
        context->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
        if (visible)
            for (auto &ch : chunks) {
                context->VSSetShaderResources(0, 1, ch.srv.GetAddressOf());
                context->DrawInstanced(6, ch.count, 0, 0);
            }
        ID3D11ShaderResourceView *empty = nullptr;
        context->VSSetShaderResources(0, 1, &empty);
        context->OMSetRenderTargets(0, nullptr, nullptr);
    }
    void png(Target &t, const std::filesystem::path &path) {
        D3D11_TEXTURE2D_DESC td{};
        t.texture->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        check(device->CreateTexture2D(&td, nullptr, &staging), "Export staging");
        context->CopyResource(staging.Get(), t.texture.Get());
        D3D11_MAPPED_SUBRESOURCE map{};
        check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map), "Read export");
        std::vector<BYTE> pixels(size_t(t.w) * t.h * 4);
        for (int y = 0; y < t.h; y++)
            memcpy(pixels.data() + size_t(y) * t.w * 4,
                   (BYTE *)map.pData + size_t(y) * map.RowPitch, t.w * 4);
        context->Unmap(staging.Get(), 0);
        ComPtr<IWICImagingFactory> f;
        check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                               IID_PPV_ARGS(&f)),
              "WIC factory");
        ComPtr<IWICStream> stream;
        check(f->CreateStream(&stream), "PNG stream");
        check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "PNG file");
        ComPtr<IWICBitmapEncoder> enc;
        check(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc), "PNG encoder");
        check(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache), "PNG init");
        ComPtr<IWICBitmapFrameEncode> frame;
        check(enc->CreateNewFrame(&frame, nullptr), "PNG frame");
        check(frame->Initialize(nullptr), "PNG frame init");
        check(frame->SetSize(t.w, t.h), "PNG size");
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        check(frame->SetPixelFormat(&fmt), "PNG format");
        if (fmt != GUID_WICPixelFormat32bppBGRA)
            throw std::runtime_error("Unsupported PNG pixel format");
        for (size_t i = 0; i < pixels.size(); i += 4)
            std::swap(pixels[i], pixels[i + 2]);
        check(frame->WritePixels(t.h, t.w * 4, UINT(pixels.size()), pixels.data()), "PNG pixels");
        check(frame->Commit(), "PNG commit");
        check(enc->Commit(), "PNG complete");
    }
};
