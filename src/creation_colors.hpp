#pragma once
#include "core.hpp"
#include "color_gradient.hpp"

namespace atomx::creation {
enum class ColorKind : uint8_t { Source, Element, Custom, Property, Category };
struct ColorRule {
    ColorKind kind=ColorKind::Source;
    Vec3 rgb{.7f,.7f,.7f};
    std::string property="Charge";
    double low=-1,high=1;
    int32_t gradient=1;
    bool reverse=false,discrete=false,legend=true;
    bool operator==(const ColorRule &r) const {
        return kind==r.kind && rgb.x==r.rgb.x && rgb.y==r.rgb.y && rgb.z==r.rgb.z &&
            property==r.property && low==r.low && high==r.high && gradient==r.gradient &&
            reverse==r.reverse && discrete==r.discrete && legend==r.legend;
    }
};
inline bool validColor(const ColorRule &r) {
    return r.kind<=ColorKind::Category && r.property.size()<=1024 &&
        (r.kind<ColorKind::Property || !r.property.empty()) &&
        std::isfinite(r.low) && std::isfinite(r.high) && r.low<=r.high && r.gradient>=0 && r.gradient<10 &&
        std::isfinite(r.rgb.x) && std::isfinite(r.rgb.y) && std::isfinite(r.rgb.z) &&
        r.rgb.x>=0 && r.rgb.x<=1 && r.rgb.y>=0 && r.rgb.y<=1 && r.rgb.z>=0 && r.rgb.z<=1;
}
// Resolve the column once per upload/range calculation, rather than allocating
// a copy or looking up a property name for every atom in a large structure.
struct ColorValues {
    const Dataset *data=nullptr;
    const std::vector<double> *scalar=nullptr;
    const std::vector<Vec3> *vector=nullptr;
    int axis=-1;
    bool magnitude=false,position=false;
    ColorValues(const Dataset &d,const std::string &name):data(&d) {
        if(auto it=d.scalarProperties.find(name);it!=d.scalarProperties.end()) { scalar=&it->second; return; }
        if(name=="Position.X" || name=="Position.Y" || name=="Position.Z") {
            position=true; axis=name.back()-'X'; return;
        }
        std::string key=name;
        if(name.size()>2 && name[name.size()-2]=='.' && name.back()>='X' && name.back()<='Z') {
            key=name.substr(0,name.size()-2); axis=name.back()-'X';
        } else if(name.size()>2 && name.front()=='|' && name.back()=='|') {
            key=name.substr(1,name.size()-2); magnitude=true;
        } else return;
        if(auto it=d.vectorProperties.find(key);it!=d.vectorProperties.end()) vector=&it->second;
    }
    double at(size_t i) const {
        if(scalar && i<scalar->size()) return (*scalar)[i];
        if(position && i<data->atoms.size()) return coordinate(data->atoms[i],axis);
        if(vector && i<vector->size()) {
            const auto v=(*vector)[i];
            if(magnitude) return std::hypot(double(v.x),double(v.y),double(v.z));
            return axis==0?v.x:axis==1?v.y:v.z;
        }
        return std::numeric_limits<double>::quiet_NaN();
    }
};
inline Vec3 mappedColor(const ColorRule &r,double value) {
    if(!std::isfinite(value)) return {.5f,.5f,.5f};
    double u=.5;
    if(r.kind==ColorKind::Category) {
        if(value<0 || value!=std::floor(value)) return {.5f,.5f,.5f};
        // Stable identifier colors: inserting/deleting other groups does not
        // rescale or change the colors of surviving identifiers.
        u=std::fmod(value*.6180339887498949,.9999999999999999);
    } else if(r.low<r.high) {
        if(value<=r.low) u=0; else if(value>=r.high) u=1;
        else { const auto scale=std::max({std::abs(r.low),std::abs(r.high),1.});
            u=(value/scale-r.low/scale)/(r.high/scale-r.low/scale); }
    }
    if(r.reverse) u=1-u;
    if(r.discrete && r.kind!=ColorKind::Category) u=std::min(std::floor(u*12),11.)/11.;
    const auto c=sampleColorGradient(r.kind==ColorKind::Category?0:r.gradient,float(std::clamp(u,0.,1.)));
    return {c[0],c[1],c[2]};
}
inline std::pair<double,double> colorRange(const ColorValues &values,size_t count,
    const std::vector<int> &selection,bool all) {
    double lo=INFINITY,hi=-INFINITY;
    auto visit=[&](size_t i) { const double v=values.at(i); if(std::isfinite(v)) {lo=std::min(lo,v);hi=std::max(hi,v);} };
    if(all) for(size_t i=0;i<count;++i) visit(i);
    else for(int i:selection) if(i>=0 && size_t(i)<count) visit(size_t(i));
    if(!std::isfinite(lo)) throw std::runtime_error("属性没有有限数值");
    return {lo,hi};
}
}
