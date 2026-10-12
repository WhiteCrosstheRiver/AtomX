#pragma once
#include "imgui.h"

// Native interpretation of the user supplied AtomX 1.0 v5.dc.html tokens.
// No web runtime, texture effects or additional render passes are required.
namespace atomx::ui_design {
inline ImVec4 rgb(unsigned value) {
    return {float((value>>16)&255)/255.f,float((value>>8)&255)/255.f,float(value&255)/255.f,1.f};
}
inline bool light(){return ImGui::GetStyleColorVec4(ImGuiCol_WindowBg).x>.5f;}
inline ImVec4 chrome(){return rgb(light()?0xe4e7eb:0x0b0d0f);}
inline ImVec4 sunk(){return rgb(light()?0xe8ebee:0x0f1114);}
inline ImVec4 buttonBorder(){return rgb(light()?0xbcc3cb:0x4a515a);}
inline ImVec4 secondary(){return rgb(light()?0x56606b:0x9aa2ab);}
inline ImVec4 tool(unsigned dark,unsigned pale){return rgb(light()?pale:dark);}
inline void button(ImDrawList *draw,ImVec2 lo,ImVec2 hi,ImVec4 tone,bool selected,bool hovered,float scale) {
    const auto fill=selected?tone:ImGui::GetStyleColorVec4(hovered?ImGuiCol_ButtonHovered:ImGuiCol_Button);
    draw->AddRectFilled(lo,hi,ImGui::GetColorU32(fill),8*scale);
    draw->AddRect(lo,hi,ImGui::GetColorU32(selected?tone:buttonBorder()),8*scale);
    // One-pixel inset highlight from v5, without blurred shadows.
    if(!selected)draw->AddLine({lo.x+6*scale,lo.y+scale},{hi.x-6*scale,lo.y+scale},
        light()?IM_COL32(255,255,255,150):IM_COL32(255,255,255,20));
    if(selected)draw->AddRect({lo.x-2.5f*scale,lo.y-2.5f*scale},{hi.x+2.5f*scale,hi.y+2.5f*scale},
        ImGui::GetColorU32(tone),10*scale,0,1.5f*scale);
}
}
