#pragma once
#include "creation_document.hpp"
#include "renderer.hpp"

// A versioned workspace archive. Each dataset is a bounded native document;
// modifiers are written field by field, never as ABI-dependent C++ objects.
namespace atomx::project {
inline constexpr uint64_t byteLimit=2ull*1024*1024*1024;
inline std::filesystem::path pathFromUtf8(const std::string &text) {
    if(text.empty())return {};
    document::valid(text.size()<=131068);
    const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0);
    document::valid(n>0 && n<=32767);std::wstring wide(size_t(n),L'\0');
    document::valid(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),wide.data(),n)==n);
    document::valid(wide.find(L'\0')==std::wstring::npos);return wide;
}
using document::Writer;using document::Reader;using document::valid;
template<class T> inline void field(Writer &w,const T &v) {
    if constexpr(std::is_same_v<T,bool>)w.flag(v);
    else if constexpr(std::is_same_v<T,std::string>)w.text(v);
    else if constexpr(std::is_same_v<T,Vec3>)w.vec(v);
    else if constexpr(std::is_enum_v<T>)w.value(int32_t(v));
    else if constexpr(std::is_arithmetic_v<T>)w.value(v);
    else if constexpr(requires{v.size();v.data();typename T::value_type;}) {
        if constexpr(requires(T container){container.resize(0);})w.value(uint64_t(v.size()));
        for(const auto &x:v)field(w,x);
    } else for(const auto &x:v)field(w,x);
}
template<class T> inline void field(Reader &r,T &v) {
    if constexpr(std::is_same_v<T,bool>)v=r.flag();
    else if constexpr(std::is_same_v<T,std::string>)v=r.text();
    else if constexpr(std::is_same_v<T,Vec3>){v=r.vec();valid(document::finite(v));}
    else if constexpr(std::is_enum_v<T>)v=T(r.value<int32_t>());
    else if constexpr(std::is_arithmetic_v<T>){v=r.value<T>();if constexpr(std::is_floating_point_v<T>)valid(std::isfinite(v));}
    else if constexpr(requires{v.size();v.data();typename T::value_type;}) {
        if constexpr(requires{v.resize(0);})v.resize(r.count(sizeof(typename T::value_type),2000000));
        for(auto &x:v)field(r,x);
    } else for(auto &x:v)field(r,x);
}
inline void writeModifier(Writer &w,const ModifierNode &m) {
    field(w,m.op);
    field(w,m.enabled);
    field(w,m.value);
    field(w,m.axis);
    field(w,m.type);
    field(w,m.upper);
    field(w,m.property);
    field(w,m.adaptive);
    field(w,m.outputProperty);
    field(w,m.discardExistingBonds);
    field(w,m.bondLowerCutoff);
    field(w,m.bondTypeCutoffsEnabled);
    field(w,m.bondCylinders);
    field(w,m.bondColorByType);
    field(w,m.bondShowPeriodicImages);
    field(w,m.bondTypeCutoffs);
    field(w,m.colorGradient);
    field(w,m.colorAutoRange);
    field(w,m.colorSymmetricRange);
    field(w,m.colorReverse);
    field(w,m.colorAllFramesRange);
    field(w,m.colorDiscrete);
    field(w,m.colorSelectedOnly);
    field(w,m.colorKeepSelection);
    field(w,m.colorLegend);
    field(w,m.colorMin);
    field(w,m.colorMax);
    field(w,m.reduceOperation);
    field(w,m.scatterXProperty);
    field(w,m.scatterYProperty);
    field(w,m.scatterSelectedOnly);
    field(w,m.bondsVisible);
    field(w,m.bondWidth);
    field(w,m.bondRadius);
    field(w,m.bondColor);
    field(w,m.assignColor);
    field(w,m.manualSelection);
    field(w,m.editedCell);
    field(w,m.editedOrigin);
    field(w,m.editedPbc);
    field(w,m.transformCoordinatesWithCell);
    field(w,m.overlapUseRadii);
    field(w,m.transformVectorProperties);
    field(w,m.clusterByBonds);
    field(w,m.clusterOnlySelected);
    field(w,m.clusterSortBySize);
    field(w,m.affineTransform);
    field(w,m.histogramNormalization);
    field(w,m.rdfBins);
    field(w,m.neighborOnlySelected);
    field(w,m.histogramSelectedOnly);
    field(w,m.histogramSelectRange);
    field(w,m.histogramRangeStart);
    field(w,m.histogramRangeEnd);
    field(w,m.sliceNormal);
    field(w,m.sliceDistance);
    field(w,m.sliceWidth);
    field(w,m.sliceInvert);
    field(w,m.sliceShowPlane);
    field(w,m.sliceCreateSelection);
    field(w,m.sliceApplySelectionOnly);
    field(w,m.sliceOperateOnParticles);
    field(w,m.replicateN);
    field(w,m.replicateAdjustBox);
    field(w,m.cspNeighbors);
    field(w,m.cspMode);
    field(w,m.cspOnlySelected);
    field(w,m.displacementFrame);
    field(w,m.displacementRelative);
    field(w,m.displacementOffset);
    field(w,m.displacementCellMapping);
    field(w,m.displacementMinimumImage);
    field(w,m.freezeFrame);
    field(w,m.affineReducedCoords);
    field(w,m.affineOnlySelected);
    field(w,m.expandMode);
    field(w,m.expandNeighbors);
    field(w,m.selectedTypes);
    field(w,m.id);field(w,m.displayName);field(w,m.category);
}
inline ModifierNode readModifier(Reader &r) {
    ModifierNode m;
    field(r,m.op);
    field(r,m.enabled);
    field(r,m.value);
    field(r,m.axis);
    field(r,m.type);
    field(r,m.upper);
    field(r,m.property);
    field(r,m.adaptive);
    field(r,m.outputProperty);
    field(r,m.discardExistingBonds);
    field(r,m.bondLowerCutoff);
    field(r,m.bondTypeCutoffsEnabled);
    field(r,m.bondCylinders);
    field(r,m.bondColorByType);
    field(r,m.bondShowPeriodicImages);
    field(r,m.bondTypeCutoffs);
    field(r,m.colorGradient);
    field(r,m.colorAutoRange);
    field(r,m.colorSymmetricRange);
    field(r,m.colorReverse);
    field(r,m.colorAllFramesRange);
    field(r,m.colorDiscrete);
    field(r,m.colorSelectedOnly);
    field(r,m.colorKeepSelection);
    field(r,m.colorLegend);
    field(r,m.colorMin);
    field(r,m.colorMax);
    field(r,m.reduceOperation);
    field(r,m.scatterXProperty);
    field(r,m.scatterYProperty);
    field(r,m.scatterSelectedOnly);
    field(r,m.bondsVisible);
    field(r,m.bondWidth);
    field(r,m.bondRadius);
    field(r,m.bondColor);
    field(r,m.assignColor);
    field(r,m.manualSelection);
    field(r,m.editedCell);
    field(r,m.editedOrigin);
    field(r,m.editedPbc);
    field(r,m.transformCoordinatesWithCell);
    field(r,m.overlapUseRadii);
    field(r,m.transformVectorProperties);
    field(r,m.clusterByBonds);
    field(r,m.clusterOnlySelected);
    field(r,m.clusterSortBySize);
    field(r,m.affineTransform);
    field(r,m.histogramNormalization);
    field(r,m.rdfBins);
    field(r,m.neighborOnlySelected);
    field(r,m.histogramSelectedOnly);
    field(r,m.histogramSelectRange);
    field(r,m.histogramRangeStart);
    field(r,m.histogramRangeEnd);
    field(r,m.sliceNormal);
    field(r,m.sliceDistance);
    field(r,m.sliceWidth);
    field(r,m.sliceInvert);
    field(r,m.sliceShowPlane);
    field(r,m.sliceCreateSelection);
    field(r,m.sliceApplySelectionOnly);
    field(r,m.sliceOperateOnParticles);
    field(r,m.replicateN);
    field(r,m.replicateAdjustBox);
    field(r,m.cspNeighbors);
    field(r,m.cspMode);
    field(r,m.cspOnlySelected);
    field(r,m.displacementFrame);
    field(r,m.displacementRelative);
    field(r,m.displacementOffset);
    field(r,m.displacementCellMapping);
    field(r,m.displacementMinimumImage);
    field(r,m.freezeFrame);
    field(r,m.affineReducedCoords);
    field(r,m.affineOnlySelected);
    field(r,m.expandMode);
    field(r,m.expandNeighbors);
    field(r,m.selectedTypes);
    field(r,m.id);field(r,m.displayName);field(r,m.category);
    valid(int(m.op)>=0 && m.op<=Op::UnwrapTrajectories);m.dirty=true;return m;
}
inline void writeGraph(Writer &w,const std::vector<ModifierNode> &nodes) {
    valid(nodes.size()<=1000);w.value(uint64_t(nodes.size()));for(const auto &m:nodes)writeModifier(w,m);
}
inline std::vector<ModifierNode> readGraph(Reader &r) {
    std::vector<ModifierNode> out(r.count(8,1000));for(auto &m:out)m=readModifier(r);return out;
}
inline void writeCamera(Writer &w,const Camera &c) {
    field(w,c.yaw);field(w,c.pitch);field(w,c.zoom);field(w,c.panX);field(w,c.panY);field(w,c.roll);
    field(w,c.mode);field(w,c.fitSelected);field(w,c.fitLo);field(w,c.fitHi);
}
inline Camera readCamera(Reader &r) {
    Camera c;field(r,c.yaw);field(r,c.pitch);field(r,c.zoom);field(r,c.panX);field(r,c.panY);field(r,c.roll);
    field(r,c.mode);field(r,c.fitSelected);field(r,c.fitLo);field(r,c.fitHi);valid(c.zoom>0 && c.mode>=0 && c.mode<=7);return c;
}
inline void writeDocument(Writer &w,const Dataset &d,const document::View &v) {
    w.value(std::max<uint64_t>(d.sourceCount,d.atoms.size()));w.value(d.stride);
    const auto position=w.file.tellp();w.value(uint64_t(0));const auto start=w.file.tellp();
    if(d.sampled()) {auto preview=d;preview.stride=1;preview.sourceCount=preview.atoms.size();document::write(w.file,preview,v,w.cancel);}
    else document::write(w.file,d,v,w.cancel);
    const auto end=w.file.tellp();valid(end>=start && uint64_t(end)<=byteLimit);
    w.file.seekp(position);w.value(uint64_t(end-start));w.file.seekp(end);if(!w.file)throw std::runtime_error("Project write failed");
}
inline document::Content readDocument(Reader &r,uint64_t atomBudget) {
    const auto count=r.value<uint64_t>(),stride=r.value<uint64_t>(),size=r.value<uint64_t>();
    valid(stride>=1 && size<=r.remaining);r.remaining-=size;
    auto out=document::read(r.file,size,atomBudget,r.cancel);
    valid(count>=out.data.atoms.size());out.data.sourceCount=count;out.data.stride=stride;return out;
}
} // namespace atomx::project
