#pragma once
#define NOMINMAX
#pragma comment(lib, "gdi32.lib")
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <wincodec.h>
#include "core.hpp"
#include "particle_mesh.hpp"
using Microsoft::WRL::ComPtr;
inline void check(HRESULT hr, const char *message) {
    if (FAILED(hr))
        throw std::runtime_error(std::string(message) + " (HRESULT " +
                                 std::to_string(uint32_t(hr)) + ")");
}
struct Camera {
    float yaw = .65f, pitch = .48f, zoom = 1.0f, panX = 0, panY = 0;
    int mode = 7;
    bool fitSelected = false;
    atomx::Vec3 fitLo{}, fitHi{};
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
    LUID luid{};
    bool duplicate = false;
};
struct ParticleStyle {
    std::array<float, 4> color{.76f, .57f, .38f, 1};
    // radius=0 and shape=-1 inherit the particle visual defaults.
    std::array<float, 4> visual{0, -1, 1, 0};
    std::array<float, 4> axes{1, 1, 1, 0};
    bool operator==(const ParticleStyle &) const = default;
};
static_assert(sizeof(ParticleStyle) == 48);
inline std::array<float, 3> sampleColorGradient(int gradient, float u) {
    u = std::clamp(u, 0.f, 1.f);
    auto mix = [](std::array<float,3> a, std::array<float,3> b, float t) {
        t = std::clamp(t, 0.f, 1.f);
        return std::array<float,3>{a[0]+(b[0]-a[0])*t, a[1]+(b[1]-a[1])*t, a[2]+(b[2]-a[2])*t};
    };
    if (gradient == 0) {
        const std::array<float,3> a{.10f,.15f,.85f}, b{.12f,.85f,.75f}, c{.98f,.88f,.08f}, d{.9f,.08f,.04f};
        return u < .5f ? mix(a,b,u*2) : u < .8f ? mix(b,c,(u-.5f)*3.3333333f) : mix(c,d,(u-.8f)*5);
    }
    if (gradient == 1)
        return u < .5f ? mix({.1f,.15f,.9f},{1,1,1},u*2) : mix({1,1,1},{.9f,.05f,.05f},(u-.5f)*2);
    if (gradient == 2)
        return {.5f+.5f*std::cos(6.2831853f*u), .5f+.5f*std::cos(6.2831853f*(u+.33f)), .5f+.5f*std::cos(6.2831853f*(u+.67f))};
    if (gradient == 3) return mix({.02f,.02f,.02f},{1,.95f,.1f},u);
    if (gradient == 4) return {u,u,u};
    if (gradient == 5) return mix({.02f,.02f,.15f},{1,.02f,0},u);
    if (gradient == 6) return {std::clamp(1.5f-std::abs(4*u-3),0.f,1.f), std::clamp(1.5f-std::abs(4*u-2),0.f,1.f), std::clamp(1.5f-std::abs(4*u-1),0.f,1.f)};
    if (gradient == 7) return mix({.05f,.01f,.2f},{1,.3f,.02f},u);
    return mix({.27f,.01f,.33f},{.99f,.9f,.14f},u);
}
struct ColorLegendOptions {
    bool visible = false;
    std::string property;
    int gradient = 0;
    float minimum = 0, maximum = 1;
    bool reverse = false, discrete = false;
};
class Renderer {
    struct BondVertex { DirectX::XMFLOAT3 position; };
    struct Chunk {
        ComPtr<ID3D11Buffer> buffer;
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11Buffer> propertyBuffer;
        ComPtr<ID3D11ShaderResourceView> propertyView;
        ComPtr<ID3D11Buffer> colorBuffer;
        ComPtr<ID3D11ShaderResourceView> colorView;
        UINT count;
    };
    struct Constants {
        DirectX::XMFLOAT4X4 view, projection;
        float radius, shape, colorAxis, colorMin;
        float colorMax, colorMode, colorDiscrete, colorGradient;
        DirectX::XMFLOAT4 colors[8];
    };
    struct BondConstants {
        DirectX::XMFLOAT4X4 viewProjection;
        DirectX::XMFLOAT2 viewport;
        float width = 1.5f, pad = 0;
        DirectX::XMFLOAT4 color{.72f,.78f,.86f,1};
    };
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11RasterizerState> wireRaster;
    ComPtr<ID3D11DepthStencilState> depthState;
    std::vector<Chunk> chunks;
    std::vector<ParticleStyle> cachedStyles;
    ComPtr<ID3D11Buffer> styleBuffer;
    ComPtr<ID3D11ShaderResourceView> styleView;
    ComPtr<ID3D11Buffer> meshBuffer;
    ComPtr<ID3D11ShaderResourceView> meshView;
    ComPtr<ID3D11Buffer> bondBuffer, bondConstants;
    ComPtr<ID3D11VertexShader> bondVS;
    ComPtr<ID3D11PixelShader> bondPS;
    ComPtr<ID3D11GeometryShader> bondGS;
    ComPtr<ID3D11InputLayout> bondLayout;
    UINT bondVertexCount = 0;

  public:
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swap;
    ComPtr<ID3D11RenderTargetView> back;
    std::vector<Adapter> adapters;
    std::wstring adapterName;
    uint64_t gpuBytes = 0;
    bool software = false;
    std::vector<ParticleStyle> styles;
    size_t meshTriangleCount = 0;
    void loadParticleMesh(const std::filesystem::path &path) {
        auto triangles = atomx::readParticleOBJ(path);
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = UINT(triangles.size() * sizeof(atomx::MeshTriangle));
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = sizeof(atomx::MeshTriangle);
        D3D11_SUBRESOURCE_DATA data{triangles.data(), 0, 0};
        ComPtr<ID3D11Buffer> buffer;
        ComPtr<ID3D11ShaderResourceView> srv;
        check(device->CreateBuffer(&bd, &data, &buffer), "Particle mesh buffer");
        check(device->CreateShaderResourceView(buffer.Get(), nullptr, &srv), "Particle mesh view");
        meshBuffer = std::move(buffer);
        meshView = std::move(srv);
        meshTriangleCount = triangles.size();
    }
    void resetStyles(size_t count) {
        const std::array<float, 4> palette[] = {{.76f, .57f, .38f, 1}, {.35f, .68f, .78f, 1},
                                                {.62f, .76f, .46f, 1}, {.78f, .44f, .52f, 1},
                                                {.69f, .52f, .81f, 1}, {.88f, .76f, .43f, 1},
                                                {.47f, .61f, .85f, 1}, {.8f, .8f, .8f, 1}};
        styles.assign(std::max<size_t>(count, 1), {});
        for (size_t i = 0; i < styles.size(); ++i)
            styles[i].color = palette[i % 8];
    }
    void uploadStyles(size_t count) {
        if (styles.size() != std::max<size_t>(count, 1))
            resetStyles(count);
        if (styles == cachedStyles && styleView)
            return;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = UINT(styles.size() * sizeof(ParticleStyle));
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = sizeof(ParticleStyle);
        D3D11_SUBRESOURCE_DATA data{styles.data(), 0, 0};
        ComPtr<ID3D11Buffer> buffer;
        ComPtr<ID3D11ShaderResourceView> view;
        check(device->CreateBuffer(&bd, &data, &buffer), "Particle styles");
        check(device->CreateShaderResourceView(buffer.Get(), nullptr, &view),
              "Particle style view");
        styleBuffer = std::move(buffer);
        styleView = std::move(view);
        cachedStyles = styles;
    }
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
            bool duplicate = std::any_of(adapters.begin(), adapters.end(), [&](const Adapter &known) {
                return known.luid.LowPart == d.AdapterLuid.LowPart &&
                       known.luid.HighPart == d.AdapterLuid.HighPart;
            });
            adapters.push_back({d.Description, d.DedicatedVideoMemory, i, d.AdapterLuid, duplicate});
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
cbuffer C : register(b0) {row_major float4x4 view;row_major float4x4 proj;float radius;float shape;float colorAxis;float colorMin;float colorMax;float colorMode;float colorDiscrete;float colorGradient;float4 colors[8];};
struct Atom {float3 pos;uint type;};StructuredBuffer<Atom> atoms : register(t0);
struct Style {float4 color;float4 visual;float4 axes;};StructuredBuffer<Style> styles:register(t1);
struct Triangle {float4 a;float4 b;float4 c;};StructuredBuffer<Triangle> mesh:register(t2);
StructuredBuffer<float> propertyValues:register(t3);
StructuredBuffer<float3> particleColors:register(t4);
struct V {
 float4 pos:SV_POSITION;float2 uv:TEXCOORD0;float3 center:TEXCOORD1;
 nointerpolation uint type:TEXCOORD2;nointerpolation float3 world:TEXCOORD3;
 nointerpolation float4 appearance:TEXCOORD4;nointerpolation float3 axes:TEXCOORD5;
 nointerpolation float4 color:TEXCOORD6;float3 plane:TEXCOORD7;
 nointerpolation float mappedValue:TEXCOORD8;
 nointerpolation float3 overrideColor:TEXCOORD9;
};
V vertex(uint id:SV_VertexID,uint instance:SV_InstanceID) {
 float2 q[6]={float2(-1,-1),float2(-1,1),float2(1,-1),float2(1,-1),float2(-1,1),float2(1,1)};
 Atom a=atoms[instance];Style s=styles[a.type&0x3fffffff];V o;
 o.overrideColor=colors[1].w>.5?particleColors[instance]:float3(-1,-1,-1);
 o.center=mul(float4(a.pos,1),view).xyz;o.world=a.pos;o.uv=q[id];o.type=a.type;
 float rad=s.visual.x>0?s.visual.x:radius;float kind=s.visual.y<0?shape:s.visual.y;
 float3 dims=max(s.axes.xyz,float3(.05,.05,.05))*rad;
 float bound=(kind<.5||kind>5.5)?max(dims.x,max(dims.y,dims.z)):kind>2.5&&kind<3.5?length(float2(rad,dims.z)):kind>3.5&&kind<4.5?rad+dims.z:length(dims);
 // Expand the projected bounding sphere for perspective views close to the camera.
 float expand=proj[3][3]<.5?abs(o.center.z)/max(abs(o.center.z)-bound,.001):1;
 o.plane=o.center+float3(o.uv*bound*expand,0);
 o.pos=mul(float4(o.plane,1),proj);o.axes=dims;o.appearance=float4(kind,rad,bound,s.visual.z);o.color=s.color;o.mappedValue=propertyValues[instance];
 return o;
}
float distanceToShape(float3 p,float3 a,float kind,float rad) {
 if(kind<.5) {float k0=length(p/a),k1=length(p/(a*a));return k0*(k0-1)/max(k1,1e-8);}
 if(kind<2.5) {float3 q=abs(p)-a;return length(max(q,0))+min(max(q.x,max(q.y,q.z)),0);}
 if(kind<3.5) {float2 q=float2(length(p.xy)-rad,abs(p.z)-a.z);return min(max(q.x,q.y),0)+length(max(q,0));}
 float3 q=p;q.z-=clamp(q.z,-a.z,a.z);return length(q)-rad;
}
struct P {float4 color:SV_TARGET;float depth:SV_DEPTH;};
P pixel(V i) {
 clip(i.appearance.w-.5);
 float kind=i.appearance.x,rad=i.appearance.y,bound=i.appearance.z;
 float3 p,n;float r=0;
 if((kind>.5&&kind<1.5)||(kind>4.5&&kind<5.5)) {
   float2 uv=(i.plane.xy-i.center.xy)/i.axes.xy;
   if(kind<1.5)clip(1-dot(uv,uv));else clip(1-max(abs(uv.x),abs(uv.y)));
   p=i.plane;n=float3(0,0,1);
 } else {
   bool perspective=proj[3][3]<.5;
   float3 origin=perspective?float3(0,0,0):float3(i.plane.xy,i.center.z+bound*2);
   float3 ray=perspective?normalize(i.plane):float3(0,0,-1);
   float3 relative=origin-i.center;
   float b=dot(relative,ray),c=dot(relative,relative)-bound*bound,disc=b*b-c;
   clip(disc);float root=sqrt(max(0,disc));float t=max(0,-b-root),end=-b+root;
   float3 localRay=mul(float4(ray,0),transpose(view)).xyz;
   float3 localOrigin=mul(float4(relative,0),transpose(view)).xyz;
   float3 hit;float3 meshNormal=float3(0,0,1);bool found=false;float eps=max(rad*.001,1e-6);
   if(kind>5.5){
     t=end+1;
     [loop]for(uint k=0;k<(uint)colors[0].w;++k){
       Triangle tri=mesh[k];float3 a=tri.a.xyz*i.axes,bv=tri.b.xyz*i.axes,cv=tri.c.xyz*i.axes;
       float3 e1=bv-a,e2=cv-a,pv=cross(localRay,e2);float det=dot(e1,pv);
       if(abs(det)<1e-9)continue;float inv=1/det;float3 tv=localOrigin-a;float u=dot(tv,pv)*inv;if(u<0||u>1)continue;
       float3 q=cross(tv,e1);float v=dot(localRay,q)*inv;if(v<0||u+v>1)continue;float candidate=dot(e2,q)*inv;
       if(candidate>=0&&candidate<t){t=candidate;meshNormal=normalize(cross(e1,e2));if(dot(meshNormal,localRay)>0)meshNormal=-meshNormal;found=true;}
     }
     hit=localOrigin+t*localRay;
   } else if(kind<.5){
     float3 ro=localOrigin/i.axes,rd=localRay/i.axes;
     float qa=dot(rd,rd),qb=dot(ro,rd),qc=dot(ro,ro)-1,qd=qb*qb-qa*qc;
     if(qd>=0){t=(-qb-sqrt(qd))/qa;if(t<0)t=(-qb+sqrt(qd))/qa;found=t>=0;}
     hit=localOrigin+t*localRay;
   } else {
     [loop]for(int step=0;step<96&&t<=end;++step){
       hit=localOrigin+t*localRay;
       float dist=distanceToShape(hit,i.axes,kind,rad);
       if(dist<eps){found=true;break;}t+=max(dist*.8,eps*.25);
     }
   }
   if(!found)discard;
   float3 ex=float3(eps,0,0),ey=float3(0,eps,0),ez=float3(0,0,eps);
   float3 normal=kind>5.5?meshNormal:kind<.5?normalize(hit/(i.axes*i.axes)):normalize(float3(
     distanceToShape(hit+ex,i.axes,kind,rad)-distanceToShape(hit-ex,i.axes,kind,rad),
     distanceToShape(hit+ey,i.axes,kind,rad)-distanceToShape(hit-ey,i.axes,kind,rad),
     distanceToShape(hit+ez,i.axes,kind,rad)-distanceToShape(hit-ez,i.axes,kind,rad)));
   n=normalize(mul(float4(normal,0),view).xyz);p=origin+ray*t;r=saturate(1-n.z*n.z);
 }
 float4 clipPos=mul(float4(p,1),proj);P o;o.depth=clipPos.z/clipPos.w;
 float3 base=i.overrideColor.x>=0?i.overrideColor:i.color.rgb;
 if(i.type&0x80000000)base=float3(1,.83,.32);
 bool selectedOnlyMode=(colorMode>1.5&&colorMode<2.5)||colorMode>3.5;
 if(colorMode>0.5 && (!selectedOnlyMode || (i.type&0x40000000))) { float value = colorMode>2.5 ? i.mappedValue : colorAxis<0.5 ? i.world.x : colorAxis<1.5 ? i.world.y : i.world.z; float u=saturate((value-colorMin)/(abs(colorMax-colorMin)<1e-12?1e-12:colorMax-colorMin)); if(colorDiscrete>0.5) u=min(floor(u*12),11)/11; float3 c0=float3(0.10,.15,.85), c1=float3(.12,.85,.75), c2=float3(.98,.88,.08), c3=float3(.9,.08,.04); if(colorGradient<.5) base=u<.5?lerp(c0,c1,u*2):u<.8?lerp(c1,c2,(u-.5)*3.333):lerp(c2,c3,(u-.8)*5); else if(colorGradient<1.5) base=u<.5?lerp(float3(0.1,.15,.9),float3(1,1,1),u*2):lerp(float3(1,1,1),float3(.9,.05,.05),(u-.5)*2); else if(colorGradient<2.5) base=float3(.5+.5*cos(6.283*(u+float3(0,.33,.67)))); else if(colorGradient<3.5) base=lerp(float3(.02,.02,.02),float3(1,.95,.1),u); else if(colorGradient<4.5) base=float3(u,u,u); else if(colorGradient<5.5) base=lerp(float3(.02,.02,.15),float3(1,.02,.0),u); else if(colorGradient<6.5) base=float3(saturate(1.5-abs(4*u-3)),saturate(1.5-abs(4*u-2)),saturate(1.5-abs(4*u-1))); else if(colorGradient<7.5) base=lerp(float3(.05,.01,.2),float3(1,.3,.02),u); else base=lerp(float3(.27,.01,.33),float3(.99,.9,.14),u); }
 float diffuse=max(0,dot(n,normalize(float3(-.45,.65,1))));float rim=pow(1-sqrt(max(0,1-r)),3);
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
        const char *bondShader = R"(
cbuffer BondCamera : register(b0) { row_major float4x4 vp; float2 viewport; float width; float pad; float4 color; };
struct I { float3 p:POSITION; };
struct O { float4 p:SV_POSITION; };
O bondVertex(I i) { O o; o.p=mul(float4(i.p,1),vp); return o; }
[maxvertexcount(4)] void bondGeometry(line O input[2], inout TriangleStream<O> stream) {
 float2 delta=(input[1].p.xy/input[1].p.w-input[0].p.xy/input[0].p.w)*viewport;
 float lengthDelta=max(length(delta),1e-5); float2 perpendicular=float2(-delta.y,delta.x)/lengthDelta;
 float2 offset=perpendicular*width/viewport;
 O a=input[0],b=input[1];
 a.p.xy+=offset*a.p.w; b.p.xy+=offset*b.p.w; stream.Append(a); stream.Append(b);
 a=input[0]; b=input[1]; a.p.xy-=offset*a.p.w; b.p.xy-=offset*b.p.w; stream.Append(a); stream.Append(b);
}
float4 bondPixel():SV_TARGET { return color; }
)";
        ComPtr<ID3DBlob> bondV, bondP, bondG;
        check(D3DCompile(bondShader, strlen(bondShader), nullptr, nullptr, nullptr, "bondVertex", "vs_5_0",
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bondV, &err), "Bond vertex shader");
        check(D3DCompile(bondShader, strlen(bondShader), nullptr, nullptr, nullptr, "bondPixel", "ps_5_0",
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bondP, &err), "Bond pixel shader");
        check(D3DCompile(bondShader, strlen(bondShader), nullptr, nullptr, nullptr, "bondGeometry", "gs_5_0",
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bondG, &err), "Bond geometry shader");
        check(device->CreateVertexShader(bondV->GetBufferPointer(), bondV->GetBufferSize(), nullptr, &bondVS), "Bond vertex shader");
        check(device->CreatePixelShader(bondP->GetBufferPointer(), bondP->GetBufferSize(), nullptr, &bondPS), "Bond pixel shader");
        check(device->CreateGeometryShader(bondG->GetBufferPointer(), bondG->GetBufferSize(), nullptr, &bondGS), "Bond geometry shader");
        D3D11_INPUT_ELEMENT_DESC bondElement{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0};
        check(device->CreateInputLayout(&bondElement,1,bondV->GetBufferPointer(),bondV->GetBufferSize(),&bondLayout), "Bond vertex layout");
        D3D11_BUFFER_DESC bondCb{}; bondCb.ByteWidth=sizeof(BondConstants); bondCb.Usage=D3D11_USAGE_DEFAULT; bondCb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        check(device->CreateBuffer(&bondCb,nullptr,&bondConstants), "Bond camera constants");
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
    void upload(const atomx::Dataset &d, const std::vector<uint8_t> &selected,
                const std::vector<uint8_t> &colorSelected = {}) {
        std::vector<BondVertex> bondVertices;
        bondVertices.reserve(d.bonds.size() * 2);
        for (const auto &bond : d.bonds) {
            if (bond.a >= d.atoms.size() || bond.b >= d.atoms.size()) continue;
            const auto &a=d.atoms[bond.a], &b=d.atoms[bond.b];
            bondVertices.push_back({{a.x,a.y,a.z}});
            bondVertices.push_back({{b.x+float(bond.image[0]*d.cell[0]+bond.image[1]*d.cell[3]+bond.image[2]*d.cell[6]),
                                     b.y+float(bond.image[0]*d.cell[1]+bond.image[1]*d.cell[4]+bond.image[2]*d.cell[7]),
                                     b.z+float(bond.image[0]*d.cell[2]+bond.image[1]*d.cell[5]+bond.image[2]*d.cell[8])}});
        }
        bondBuffer.Reset(); bondVertexCount=UINT(bondVertices.size());
        if (!bondVertices.empty()) {
            D3D11_BUFFER_DESC bondDesc{}; bondDesc.ByteWidth=UINT(bondVertices.size()*sizeof(BondVertex)); bondDesc.Usage=D3D11_USAGE_IMMUTABLE; bondDesc.BindFlags=D3D11_BIND_VERTEX_BUFFER;
            D3D11_SUBRESOURCE_DATA bondData{bondVertices.data(),0,0};
            check(device->CreateBuffer(&bondDesc,&bondData,&bondBuffer), "Bond vertex buffer");
        }
        std::vector<Chunk> next;
        const size_t block = 1048576;
        const std::vector<double> *mapped = nullptr;
        if (auto it = d.scalarProperties.find("Color coding");
            it != d.scalarProperties.end() && it->second.size() == d.atoms.size())
            mapped = &it->second;
        for (size_t start = 0; start < d.atoms.size(); start += block) {
            Chunk c;
            c.count = UINT(std::min(block, d.atoms.size() - start));
            std::vector<atomx::Atom> tmp(d.atoms.begin() + start,
                                         d.atoms.begin() + start + c.count);
            for (size_t j = 0; j < tmp.size(); j++) {
                if (start + j < selected.size() && selected[start + j])
                    tmp[j].type |= 0x80000000;
                if ((colorSelected.empty() && start + j < selected.size() && selected[start + j]) ||
                    (!colorSelected.empty() && start + j < colorSelected.size() && colorSelected[start + j]))
                    tmp[j].type |= 0x40000000;
            }
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
            std::vector<float> values(c.count, 0.f);
            if (mapped)
                for (size_t j = 0; j < values.size(); ++j) {
                    double value = (*mapped)[start + j];
                    values[j] = std::isfinite(value) ? float(value) : 0.f;
                }
            D3D11_BUFFER_DESC valueDesc{};
            valueDesc.ByteWidth = c.count * sizeof(float);
            valueDesc.Usage = D3D11_USAGE_IMMUTABLE;
            valueDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            valueDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            valueDesc.StructureByteStride = sizeof(float);
            D3D11_SUBRESOURCE_DATA valueData{values.data(), 0, 0};
            check(device->CreateBuffer(&valueDesc, &valueData, &c.propertyBuffer),
                  "Particle property GPU allocation");
            sv.Buffer.NumElements = c.count;
            check(device->CreateShaderResourceView(c.propertyBuffer.Get(), &sv, &c.propertyView),
                  "Particle property buffer view");
            if (d.particleColors.size()==d.atoms.size()) {
                std::vector<DirectX::XMFLOAT3> colors(c.count);
                for (size_t j=0;j<colors.size();++j) {
                    const auto &source=d.particleColors[start+j];
                    colors[j]={source.x,source.y,source.z};
                }
                D3D11_BUFFER_DESC colorDesc{}; colorDesc.ByteWidth=UINT(colors.size()*sizeof(DirectX::XMFLOAT3));
                colorDesc.Usage=D3D11_USAGE_IMMUTABLE; colorDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
                colorDesc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; colorDesc.StructureByteStride=sizeof(DirectX::XMFLOAT3);
                D3D11_SUBRESOURCE_DATA colorData{colors.data(),0,0};
                check(device->CreateBuffer(&colorDesc,&colorData,&c.colorBuffer),"Particle color buffer");
                sv.Buffer.NumElements=c.count;
                check(device->CreateShaderResourceView(c.colorBuffer.Get(),&sv,&c.colorView),"Particle color view");
            }
            next.push_back(std::move(c));
        }
        chunks = std::move(next);
        gpuBytes = d.atoms.size() * (sizeof(atomx::Atom) + sizeof(float) +
                                      (d.particleColors.size()==d.atoms.size()?sizeof(DirectX::XMFLOAT3):0));
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
    DirectX::XMMATRIX matrix(const atomx::Dataset &d, const Camera &cam, float aspect, bool includeCell,
                             DirectX::XMMATRIX *viewOut = nullptr,
                             DirectX::XMMATRIX *projOut = nullptr) {
        using namespace DirectX;
        auto lo = cam.fitSelected ? cam.fitLo : d.lo;
        auto hi = cam.fitSelected ? cam.fitHi : d.hi;
        if (includeCell && !cam.fitSelected && (d.cell[0] != 0 || d.cell[4] != 0 || d.cell[8] != 0)) {
            for (int j = 0; j < 8; j++) {
                float x = float((j & 1 ? d.cell[0] : 0) + (j & 2 ? d.cell[3] : 0) +
                                (j & 4 ? d.cell[6] : 0));
                float y = float((j & 1 ? d.cell[1] : 0) + (j & 2 ? d.cell[4] : 0) +
                                (j & 4 ? d.cell[7] : 0));
                float z = float((j & 1 ? d.cell[2] : 0) + (j & 2 ? d.cell[5] : 0) +
                                (j & 4 ? d.cell[8] : 0));
                x += d.origin.x;
                y += d.origin.y;
                z += d.origin.z;
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
        float dist = span * 2.8f * cam.zoom;
        auto baseView = XMMatrixLookAtRH(center + dir * dist, center, up);
        auto v = baseView * XMMatrixTranslation(cam.panX * span, cam.panY * span, 0);
        XMMATRIX p;
        if (cam.mode == 7) {
            p = XMMatrixPerspectiveFovRH(.65f, aspect, span * .001f, span * 1000);
        } else {
            // Fit the actual projected cell bounds. A single world-space max
            // dimension wastes most of a wide viewport for long nanowires.
            float minX = std::numeric_limits<float>::max(), minY = minX;
            float maxX = -minX, maxY = -minX;
            for (int corner = 0; corner < 8; ++corner) {
                XMVECTOR point = XMVectorSet(corner & 1 ? hi.x : lo.x,
                                             corner & 2 ? hi.y : lo.y,
                                             corner & 4 ? hi.z : lo.z, 1);
                XMFLOAT3 projected;
                XMStoreFloat3(&projected, XMVector3TransformCoord(point, baseView));
                minX = std::min(minX, projected.x); maxX = std::max(maxX, projected.x);
                minY = std::min(minY, projected.y); maxY = std::max(maxY, projected.y);
            }
            float width = std::max((maxX-minX)*1.12f, span*.02f);
            float height = std::max((maxY-minY)*1.12f, span*.02f);
            width = std::max(width, height * aspect);
            height = std::max(height, width / std::max(aspect, .1f));
            p = XMMatrixOrthographicRH(width * cam.zoom, height * cam.zoom,
                                       span * .001f, span * 1000);
        }
        if (viewOut)
            *viewOut = v;
        if (projOut)
            *projOut = p;
        return v * p;
    }
    void draw(Target &t, const atomx::Dataset &d, const Camera &cam, float radius, int shape, int renderMode, int colorAxis, int colorGradient, float colorMin, float colorMax, bool colorCoding, bool discrete, bool selectedOnly, const float *bg,
              bool visible = true, bool includeCellInFit = true) {
        using namespace DirectX;
        uploadStyles(d.species.size());
        context->VSSetShaderResources(1, 1, styleView.GetAddressOf());
        context->PSSetShaderResources(2, 1, meshView.GetAddressOf());
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
        matrix(d, cam, float(t.w) / t.h, includeCellInFit, &view, &proj);
        XMStoreFloat4x4(&c.view, view);
        XMStoreFloat4x4(&c.projection, proj);
        c.radius = radius;
        c.shape = float(shape);
        bool hasPropertyValues = false;
        if (auto it = d.scalarProperties.find("Color coding"); it != d.scalarProperties.end())
            hasPropertyValues = it->second.size() == d.atoms.size();
        c.colorAxis = float(colorAxis); c.colorMin=colorMin; c.colorMax=colorMax;
        c.colorMode = colorCoding ? (hasPropertyValues ? (selectedOnly ? 4.f : 3.f)
                                                       : (selectedOnly ? 2.f : 1.f)) : 0.f;
        c.colorDiscrete=discrete?1.f:0.f; c.colorGradient=float(colorGradient);
        XMFLOAT4 colors[] = {{.76f, .57f, .38f, 1}, {.35f, .68f, .78f, 1}, {.62f, .76f, .46f, 1},
                             {.78f, .44f, .52f, 1}, {.69f, .52f, .81f, 1}, {.88f, .76f, .43f, 1},
                             {.47f, .61f, .85f, 1}, {.8f, .8f, .8f, 1}};
        std::copy(std::begin(colors), std::end(colors), c.colors);
        c.colors[0].w = float(meshTriangleCount);
        c.colors[1].w = d.particleColors.size()==d.atoms.size()?1.f:0.f;
        context->UpdateSubresource(constants.Get(), 0, nullptr, &c, 0, 0);
        context->VSSetConstantBuffers(0, 1, constants.GetAddressOf());
        context->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
        if (visible)
            for (auto &ch : chunks) {
                context->VSSetShaderResources(0, 1, ch.srv.GetAddressOf());
                context->VSSetShaderResources(3, 1, ch.propertyView.GetAddressOf());
                context->VSSetShaderResources(4, 1, ch.colorView.GetAddressOf());
                context->DrawInstanced(6, ch.count, 0, 0);
            }
        if (visible && d.bondStyle.visible && bondBuffer && bondVertexCount) {
            BondConstants bondCamera;
            DirectX::XMStoreFloat4x4(&bondCamera.viewProjection, view * proj);
            bondCamera.viewport={float(t.w),float(t.h)};
            bondCamera.width=d.bondStyle.width;
            bondCamera.color={d.bondStyle.color[0],d.bondStyle.color[1],d.bondStyle.color[2],d.bondStyle.color[3]};
            context->UpdateSubresource(bondConstants.Get(),0,nullptr,&bondCamera,0,0);
            UINT stride=sizeof(BondVertex), offset=0;
            ID3D11Buffer *buffer=bondBuffer.Get();
            context->IASetInputLayout(bondLayout.Get());
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
            context->IASetVertexBuffers(0,1,&buffer,&stride,&offset);
            context->VSSetShader(bondVS.Get(),nullptr,0);
            context->PSSetShader(bondPS.Get(),nullptr,0);
            context->GSSetShader(bondGS.Get(),nullptr,0);
            context->VSSetConstantBuffers(0,1,bondConstants.GetAddressOf());
            context->GSSetConstantBuffers(0,1,bondConstants.GetAddressOf());
            context->PSSetConstantBuffers(0,1,bondConstants.GetAddressOf());
            context->RSSetState(raster.Get());
            context->Draw(bondVertexCount,0);
            context->GSSetShader(nullptr,nullptr,0);
        }
        ID3D11ShaderResourceView *empty = nullptr;
        context->VSSetShaderResources(0, 1, &empty);
        context->VSSetShaderResources(1, 1, &empty);
        context->VSSetShaderResources(3, 1, &empty);
        context->VSSetShaderResources(4, 1, &empty);
        context->PSSetShaderResources(2, 1, &empty);
        context->OMSetRenderTargets(0, nullptr, nullptr);
    }
    void png(Target &t, const std::filesystem::path &path, const ColorLegendOptions &legend = {}) {
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
        if (legend.visible && t.w >= 240 && t.h >= 120) {
            for (size_t i = 0; i < pixels.size(); i += 4)
                std::swap(pixels[i], pixels[i + 2]); // RGBA readback to top-down BGRA DIB.
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = t.w;
            info.bmiHeader.biHeight = -t.h;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            void *bits = nullptr;
            HBITMAP dib = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
            HDC dc = CreateCompatibleDC(nullptr);
            if (!dib || !dc || !bits) {
                if (dib) DeleteObject(dib);
                if (dc) DeleteDC(dc);
                throw std::runtime_error("Could not create PNG legend surface");
            }
            HGDIOBJ oldBitmap = SelectObject(dc, dib);
            memcpy(bits, pixels.data(), pixels.size());
            const float scale = std::clamp(std::min(t.w / 1280.f, t.h / 720.f), .65f, 2.5f);
            const int margin = int(std::round(14 * scale));
            const int boxW = int(std::round(258 * scale));
            const int boxH = int(std::round(88 * scale));
            const int left = t.w - boxW - margin;
            const int top = margin;
            auto *bgra = static_cast<BYTE *>(bits);
            for (int y = top; y < top + boxH; ++y)
                for (int x = left; x < left + boxW; ++x) {
                    size_t at = (size_t(y) * t.w + x) * 4;
                    bgra[at] = 22; bgra[at+1] = 18; bgra[at+2] = 14; bgra[at+3] = 245;
                }
            RECT frameRect{left, top, left+boxW, top+boxH};
            HBRUSH border = CreateSolidBrush(RGB(100, 117, 139));
            FrameRect(dc, &frameRect, border);
            DeleteObject(border);
            int fontHeight = std::max(10, int(std::round(13 * scale)));
            HFONT font = CreateFontW(fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(238, 243, 250));
            std::wstring propertyName;
            if (!legend.property.empty()) {
                int length = MultiByteToWideChar(CP_UTF8, 0, legend.property.c_str(), -1, nullptr, 0);
                if (length > 1) {
                    propertyName.resize(size_t(length));
                    MultiByteToWideChar(CP_UTF8, 0, legend.property.c_str(), -1, propertyName.data(), length);
                    propertyName.resize(size_t(length - 1));
                }
            }
            if (propertyName.empty()) propertyName = L"Color coding";
            RECT title{left+int(10*scale), top+int(5*scale), left+boxW-int(8*scale), top+int(25*scale)};
            DrawTextW(dc, propertyName.c_str(), -1, &title, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            const int barX = left + int(10*scale), barY = top + int(30*scale);
            const int barW = boxW - int(20*scale), barH = std::max(6, int(15*scale));
            for (int x = 0; x < barW; ++x) {
                float u = barW > 1 ? float(x) / float(barW-1) : 0;
                if (legend.reverse) u = 1-u;
                if (legend.discrete) u = std::min(std::floor(u*12),11.f)/11.f;
                auto color = sampleColorGradient(legend.gradient, u);
                BYTE red = BYTE(std::clamp(color[0],0.f,1.f)*255), green = BYTE(std::clamp(color[1],0.f,1.f)*255), blue = BYTE(std::clamp(color[2],0.f,1.f)*255);
                for (int y = 0; y < barH; ++y) {
                    size_t at = (size_t(barY+y)*t.w + barX+x)*4;
                    bgra[at]=blue; bgra[at+1]=green; bgra[at+2]=red; bgra[at+3]=255;
                }
            }
            std::wostringstream lowText, highText;
            lowText << std::setprecision(5) << legend.minimum;
            highText << std::setprecision(5) << legend.maximum;
            RECT lowLabel{barX, barY+barH+int(3*scale), left+boxW/2, top+boxH-int(4*scale)};
            RECT highLabel{left+boxW/2, barY+barH+int(3*scale), left+boxW-int(7*scale), top+boxH-int(4*scale)};
            DrawTextW(dc, lowText.str().c_str(), -1, &lowLabel, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            DrawTextW(dc, highText.str().c_str(), -1, &highLabel, DT_RIGHT | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (oldFont) SelectObject(dc, oldFont);
            if (font) DeleteObject(font);
            memcpy(pixels.data(), bits, pixels.size());
            SelectObject(dc, oldBitmap);
            DeleteObject(dib);
            DeleteDC(dc);
        } else {
            for (size_t i = 0; i < pixels.size(); i += 4)
                std::swap(pixels[i], pixels[i + 2]);
        }
        check(frame->WritePixels(t.h, t.w * 4, UINT(pixels.size()), pixels.data()), "PNG pixels");
        check(frame->Commit(), "PNG commit");
        check(enc->Commit(), "PNG complete");
    }
};
