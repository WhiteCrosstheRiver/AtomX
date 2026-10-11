#pragma once
// AtomX's independent LJ 12-6 evaluator. Equations and mixing conventions:
// https://docs.lammps.org/pair_lj.html and pair_modify.html (see REFERENCES.md).
#include "core.hpp"
#include "lj_openkim.hpp"
#include <chrono>

namespace atomx::lj {
struct Parameter { std::string name; double sigma=1,epsilon=1,cutoff=4; };
struct Options { std::vector<Parameter> types; bool geometric=false,shift=true,periodic=true; };
struct Result {
    double energy=0,maxForce=0,seconds=0;
    uint64_t pairs=0;
    std::vector<double> atomicEnergy;
    std::vector<std::array<double,3>> forces;
};
inline Parameter openkim(std::string_view name) {
    for(const auto &p:openkimParameters)if(name==p.name)return {p.name,p.sigma,p.epsilon,p.cutoff};
    throw std::runtime_error("No OpenKIM parameter for species: "+std::string(name));
}
// Import the user's converted UFF CSV. Derive from original x and D columns,
// rather than the rounded sigma/eV display columns in the supplied spreadsheet.
inline std::vector<Parameter> readUff(const std::filesystem::path &path) {
    std::ifstream in(path);if(!in)throw std::runtime_error("Cannot open UFF CSV");
    std::string line;std::getline(in,line);
    if(line.find("uff_type,x_i_A,D_i_kcal_mol") == std::string::npos)
        throw std::runtime_error("Expected uff_type,x_i_A,D_i_kcal_mol CSV columns");
    std::vector<Parameter> rows;std::set<std::string> names;
    while(std::getline(in,line)) {
        if(line.empty() || line=="\r")continue;
        std::istringstream row(line);std::string name,x,d;
        if(!std::getline(row,name,',') || !std::getline(row,x,',') || !std::getline(row,d,','))
            throw std::runtime_error("Malformed UFF CSV row");
        auto number=[](const std::string &s){size_t n=0;double v=std::stod(s,&n);
            if(n!=s.size() || !std::isfinite(v) || v<=0)throw std::runtime_error("Invalid UFF parameter");return v;};
        const double sigma=number(x)/std::pow(2.,1./6.),epsilon=number(d)*0.043364104241800934;
        if(name.empty() || !names.insert(name).second)throw std::runtime_error("Duplicate/empty UFF type");
        rows.push_back({name,sigma,epsilon,4*sigma});
        if(rows.size()>1024)throw std::runtime_error("Too many UFF types");
    }
    if(rows.empty())throw std::runtime_error("Empty UFF CSV");return rows;
}
inline Parameter mix(const Parameter &a,const Parameter &b,bool geometric) {
    return {a.name+"/"+b.name,geometric?std::sqrt(a.sigma*b.sigma):(a.sigma+b.sigma)*.5,
        std::sqrt(a.epsilon*b.epsilon),geometric?std::sqrt(a.cutoff*b.cutoff):(a.cutoff+b.cutoff)*.5};
}
inline Result calculate(const Dataset &input,const Options &options,std::atomic<bool> *cancel=nullptr,
                         std::atomic<float> *progress=nullptr) {
    const auto started=std::chrono::steady_clock::now();
    validateNeighborAnalysisInput(input);
    if(input.atoms.empty())throw std::runtime_error("LJ requires atoms");
    if(options.types.size()!=input.species.size() || options.types.size()>512)
        throw std::runtime_error("Assign a parameter to every species (maximum 512 types)");
    double cutoff=0;
    for(const auto &p:options.types) {
        if(!std::isfinite(p.sigma)||!std::isfinite(p.epsilon)||!std::isfinite(p.cutoff)||
           p.sigma<=0||p.epsilon<0||p.cutoff<=0)throw std::runtime_error("Invalid sigma, epsilon or cutoff");
        cutoff=std::max(cutoff,p.cutoff);
    }
    auto check=[&]{if(cancel && cancel->load())throw std::runtime_error("Cancelled");};check();
    const size_t types=options.types.size();std::vector<Parameter> pairs;
    pairs.reserve(types*types);
    for(const auto &a:options.types)for(const auto &b:options.types)pairs.push_back(mix(a,b,options.geometric));
    Result out;out.atomicEnergy.resize(input.atoms.size());out.forces.resize(input.atoms.size());
    auto evaluate=[&](uint32_t i,uint32_t j,double r2,const std::array<double,3> &delta,double weight,bool symmetric) {
        const auto &p=pairs[size_t(input.atoms[i].type)*types+input.atoms[j].type];
        if(r2>=p.cutoff*p.cutoff)return;
        if(!(r2>1e-20))throw std::runtime_error("Overlapping atoms: LJ energy is undefined");
        const double s2=p.sigma*p.sigma/r2,s6=s2*s2*s2;
        double energy=4*p.epsilon*(s6*s6-s6);
        if(options.shift){const double c2=p.sigma*p.sigma/(p.cutoff*p.cutoff),c6=c2*c2*c2;
            energy-=4*p.epsilon*(c6*c6-c6);}
        const double f=24*p.epsilon*(s6-2*s6*s6)/r2;
        if(!std::isfinite(energy)||!std::isfinite(f))throw std::runtime_error("LJ overflow: check distances/parameters");
        out.energy+=weight*energy;out.atomicEnergy[i]+=.5*energy;
        for(int axis=0;axis<3;++axis){out.forces[i][axis]+=f*delta[axis];
            if(symmetric)out.forces[j][axis]-=f*delta[axis];}
        if(symmetric)out.atomicEnergy[j]+=.5*energy;
        ++out.pairs;
    };
    Dataset data;data.atoms=input.atoms;data.species=input.species;data.sourceCount=input.sourceCount;
    data.cell=input.cell;data.origin=input.origin;data.pbc=options.periodic?input.pbc:std::array<bool,3>{};
    for(const auto &a:data.atoms)if(a.type>=types || !std::isfinite(a.x)||!std::isfinite(a.y)||!std::isfinite(a.z))
        throw std::runtime_error("Invalid atom position or species");
    // Reciprocal rows (fractional coordinates) and perpendicular cell heights.
    const bool periodic=std::any_of(data.pbc.begin(),data.pbc.end(),[](bool b){return b;});
    std::array<std::array<double,3>,3> inverse{};
    bool minimumImage=true;
    if(periodic) {
        const auto &c=data.cell;
        inverse={{{c[4]*c[8]-c[5]*c[7],c[5]*c[6]-c[3]*c[8],c[3]*c[7]-c[4]*c[6]},
                  {c[7]*c[2]-c[8]*c[1],c[8]*c[0]-c[6]*c[2],c[6]*c[1]-c[7]*c[0]},
                  {c[1]*c[5]-c[2]*c[4],c[2]*c[3]-c[0]*c[5],c[0]*c[4]-c[1]*c[3]}}};
        const double det=c[0]*inverse[0][0]+c[1]*inverse[0][1]+c[2]*inverse[0][2];
        if(!std::isfinite(det)||std::abs(det)<1e-12)throw std::runtime_error("Periodic LJ requires a nonsingular 3D cell");
        for(int axis=0;axis<3;++axis) {
            for(auto &v:inverse[axis])v/=det;
            const double norm=std::hypot(inverse[axis][0],inverse[axis][1],inverse[axis][2]);
            if(!std::isfinite(norm))throw std::runtime_error("Invalid periodic cell");
            if(data.pbc[axis] && (2*cutoff*norm>=1 || c[axis*4]<=0))minimumImage=false;
        }
    }
    if(progress)*progress=.05f;
    if(minimumImage) {
        uint32_t progressIndex=UINT32_MAX;
        forEachNeighborPair(data,cutoff,[&](uint32_t i,uint32_t j,double r2,std::array<int32_t,3> image){
            if((out.pairs&4095)==0)check();
            std::array<double,3> delta{double(data.atoms[j].x)-data.atoms[i].x,
                double(data.atoms[j].y)-data.atoms[i].y,double(data.atoms[j].z)-data.atoms[i].z};
            for(int a=0;a<3;++a)for(int k=0;k<3;++k)delta[k]+=image[a]*data.cell[a*3+k];
            evaluate(i,j,r2,delta,1,true);
            if(progress && progressIndex!=i){progressIndex=i;*progress=.1f+.85f*float(i+1)/float(data.atoms.size());}
        },cancel);
    } else {
        // Full finite-cutoff periodic sum, including self images. Small cells
        // must not silently use only one nearest image. Ghost positions stay
        // double precision; each central atom receives its own force once.
        using Position=std::array<double,3>;
        std::vector<Position> positions;positions.reserve(data.atoms.size());
        Position lo{INFINITY,INFINITY,INFINITY},hi{-INFINITY,-INFINITY,-INFINITY};
        for(const auto &a:data.atoms) {
            Position p{a.x,a.y,a.z},wrap{};
            for(int axis=0;axis<3;++axis)if(data.pbc[axis]) {
                double f=0;for(int k=0;k<3;++k)f+=inverse[axis][k]*(p[k]-coordinate(Atom{data.origin.x,data.origin.y,data.origin.z,0},k));
                wrap[axis]=std::floor(f);
            }
            for(int k=0;k<3;++k){for(int axis=0;axis<3;++axis)p[k]-=wrap[axis]*data.cell[axis*3+k];
                lo[k]=std::min(lo[k],p[k]);hi[k]=std::max(hi[k],p[k]);}
            positions.push_back(p);
        }
        int span[3]{};uint64_t images=1;
        for(int a=0;a<3;++a)if(data.pbc[a]) {
            double extent=0;for(int k=0;k<3;++k)extent+=std::abs(inverse[a][k])*(hi[k]-lo[k]);
            double bound=std::ceil(extent+cutoff*std::hypot(inverse[a][0],inverse[a][1],inverse[a][2]));
            if(!std::isfinite(bound)||bound>64)throw std::runtime_error("Too many periodic images; enlarge cell or reduce cutoff");
            span[a]=int(bound);images*=uint64_t(2*span[a]+1);
        }
        if(images>100000 || images*positions.size()>100000000)
            throw std::runtime_error("Periodic image budget exceeded; enlarge cell or reduce cutoff");
        struct Ghost {uint32_t index;Position p;};
        std::unordered_map<NeighborBin,std::vector<Ghost>,NeighborBinHash> grid;
        auto bin=[&](const Position &p){NeighborBin b;for(int k=0;k<3;++k){double v=std::floor(p[k]/cutoff);
            if(!std::isfinite(v)||std::abs(v)>1e15)throw std::runtime_error("Coordinates exceed LJ grid range");binComponent(b,k)=int64_t(v);}return b;};
        uint64_t ghosts=0,attempts=0;
        for(int z=-span[2];z<=span[2];++z)for(int y=-span[1];y<=span[1];++y)for(int x=-span[0];x<=span[0];++x)
            for(uint32_t i=0;i<positions.size();++i) {
                if((++attempts&4095)==0)check();Position p=positions[i];bool inside=true;
                for(int k=0;k<3;++k){p[k]+=x*data.cell[k]+y*data.cell[3+k]+z*data.cell[6+k];
                    inside&=p[k]>=lo[k]-cutoff && p[k]<=hi[k]+cutoff;}
                if(!inside)continue;
                if(++ghosts>4000000)throw std::runtime_error("LJ ghost budget exceeded; reduce cutoff");
                grid[bin(p)].push_back({i,p});
            }
        uint64_t comparisons=0;
        for(uint32_t i=0;i<positions.size();++i) {
            check();const auto center=bin(positions[i]);
            for(int z=-1;z<=1;++z)for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x) {
                auto found=grid.find({center.x+x,center.y+y,center.z+z});if(found==grid.end())continue;
                for(const auto &g:found->second) {
                    if((++comparisons&4095)==0)check();
                    if(comparisons>300000000)throw std::runtime_error("LJ neighbor budget exceeded; reduce cutoff");
                    Position delta;double r2=0;for(int k=0;k<3;++k){delta[k]=g.p[k]-positions[i][k];r2+=delta[k]*delta[k];}
                    if(g.index==i && r2<1e-20)continue;
                    if(r2<cutoff*cutoff)evaluate(i,g.index,r2,delta,.5,false);
                }
            }
            if(progress)*progress=.15f+.8f*float(i+1)/float(positions.size());
        }
        out.pairs/=2;
    }
    check();
    for(const auto &f:out.forces){const double norm=std::hypot(f[0],f[1],f[2]);
        if(!std::isfinite(norm))throw std::runtime_error("LJ force overflow");out.maxForce=std::max(out.maxForce,norm);}
    if(!std::isfinite(out.energy))throw std::runtime_error("LJ energy overflow");
    out.seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
    if(progress)*progress=1;return out;
}
}
