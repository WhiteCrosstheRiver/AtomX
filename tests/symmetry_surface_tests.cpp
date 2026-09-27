#include "../src/symmetry.hpp"
#include <chrono>
#include <iostream>
using namespace atomx;

static void require(bool condition,const char *message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    try {
        Dataset nacl;
        nacl.species={"Na","Cl"};
        nacl.cell={5.64,0,0,0,5.64,0,0,0,5.64};
        nacl.pbc={true,true,true};
        const double fcc[4][3]={{0,0,0},{0,.5,.5},{.5,0,.5},{.5,.5,0}};
        for (uint32_t type=0;type<2;++type)
            for (const auto &f:fcc) {
                const Vec3 p=authoring::cartesian(nacl,
                    std::fmod(f[0]+.5*type,1.0),std::fmod(f[1]+.5*type,1.0),
                    std::fmod(f[2]+.5*type,1.0));
                nacl.atoms.push_back({p.x,p.y,p.z,type});
            }
        authoring::finish(nacl,"NaCl test cell");
        Dataset empty=nacl;
        empty.atoms.clear(); empty.species.clear();
        const auto generated=symmetry::expandAsymmetricUnit(empty,
            {{"Na",{0,0,0}},{"Cl",{.5,.5,.5}}},225);
        require(generated && generated->atoms.size()==8 &&
                symmetry::analyze(*generated)->number==225,
                "space-group operations build conventional NaCl from two sites");
        empty.cell[4]=6;
        require(!symmetry::expandAsymmetricUnit(empty,
                    {{"Na",{0,0,0}},{"Cl",{.5,.5,.5}}},225),
                "incompatible cubic space group and noncubic metric are rejected");
        const auto info=symmetry::analyze(nacl);
        require(info && info->number==225 && info->symbol=="Fm-3m",
                "NaCl rock salt resolves to Fm-3m (225)");
        require(info->primitiveAtoms==2,"NaCl primitive cell has two atoms");
        const auto primitive=symmetry::primitive(nacl);
        require(primitive && primitive->atoms.size()==2,"primitive conversion reduces eight atoms to two");
        require(std::abs(authoring::latticeOf(*primitive).volume*4-
                         authoring::latticeOf(nacl).volume)<1e-2,
                "primitive cell has one quarter of conventional-cell volume");
        require(symmetry::analyze(*primitive)->number==225,
                "primitive cell keeps the rock-salt space group");
        const auto large=authoring::replicate(nacl,4,4,4);
        const auto start=std::chrono::steady_clock::now();
        const auto largeInfo=symmetry::analyze(large);
        const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now()-start).count();
        require(largeInfo && largeInfo->number==225,
                "512-atom NaCl supercell retains the recognized space group");
        std::cout<<"512-atom symmetry search: "<<elapsed<<" ms\n";
        for (const auto hkl: {std::array<int,3>{1,0,0}, {1,1,0}, {1,1,1},
                              {2,1,3}, {-1,1,0}, {2,2,0}}) {
            const auto slab=authoring::millerSurface(nacl,hkl[0],hkl[1],hkl[2],3,12);
            require(slab && slab->atoms.size()==24 && slab->pbc[0] && slab->pbc[1] &&
                    !slab->pbc[2],"Miller slab has three layers and open surface boundary");
            const auto axes=authoring::axes(slab->cell);
            const Vec3 reciprocal{float(hkl[0]),float(hkl[1]),float(hkl[2])};
            require(std::abs(authoring::dot(axes.a,reciprocal))<1e-4 &&
                    std::abs(authoring::dot(axes.b,reciprocal))<1e-4,
                    "surface basis vectors lie in the requested Miller plane");
            int na=0,cl=0;
            for (const auto &atom:slab->atoms) (atom.type==0?na:cl)++;
            require(na==12&&cl==12,"Miller slab preserves stoichiometry");
        }
        require(!authoring::millerSurface(nacl,0,0,0,1,10),"zero Miller vector rejected");
        Dataset skew;
        skew.species={"C"};
        skew.cell={2,0,0,1,3,0,.2,.4,4};
        skew.pbc={true,true,true};
        skew.atoms={{.3f,.5f,.7f,0}};
        authoring::finish(skew,"skew cell");
        const auto skewSlab=authoring::millerSurface(skew,1,2,3,2,8);
        require(skewSlab && skewSlab->atoms.size()==2,
                "triclinic crystal accepts arbitrary Miller plane");
        const auto old=authoring::axes(skew.cell);
        const double volume=authoring::dot(old.a,authoring::cross(old.b,old.c));
        const Vec3 normal=authoring::add(authoring::scale(authoring::cross(old.b,old.c),1/volume),
            authoring::add(authoring::scale(authoring::cross(old.c,old.a),2/volume),
                           authoring::scale(authoring::cross(old.a,old.b),3/volume)));
        const auto oriented=authoring::axes(skewSlab->cell);
        require(std::abs(authoring::dot(oriented.a,normal))<1e-5 &&
                std::abs(authoring::dot(oriented.b,normal))<1e-5,
                "triclinic surface vectors lie in the reciprocal Miller plane");
        nacl.pbc[2]=false;
        require(!symmetry::analyze(nacl),"nonperiodic slab is not given a 3D space group");
        require(!authoring::millerSurface(nacl,1,0,0,1,10),
                "nonperiodic slab cannot be cleaved as a bulk crystal");
        std::cout<<"PASS: NaCl space group, primitive conversion, arbitrary Miller slabs\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr<<"FAIL: "<<error.what()<<'\n';
        return 1;
    }
}
