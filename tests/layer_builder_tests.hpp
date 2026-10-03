#pragma once
#include "../src/layer_builder.hpp"
inline void testLayerBuilder() {
    auto close=[](double a,double b) { return std::abs(a-b)<2e-5; };
    auto fail=[](auto action) { try { action(); } catch (const std::exception &) { return true; } return false; };
    Dataset a; a.species={"Cu"}; a.pbc={true,true,true}; a.cell={2,0,0,0,2,0,1,.5,4}; a.origin={10,20,30};
    a.atoms={{10.25f,20.125f,31,0},{11.25f,20.125f,31,0}};
    a.bonds={{0,1,{},2},{1,0,{1,0,0},1},{0,0,{0,0,1},1}};
    a.scalarProperties["Charge"]={1,2}; a.vectorProperties["Force"]={{1,2,3},{4,5,6}};
    a.globalAttributes["TotalEnergy"]=12; a.tables.push_back({"Reference",{"Name"},{{"source"}}});
    motion::create(a,{0},"site",1); a.bounds(); a.sourceCount=2;
    Dataset b=a; b.origin={}; b.species={"Ni"}; b.cell={4,0,0,0,4,0,0,0,2};
    b.atoms={{1,1,.5f,0},{3,1,.5f,0}}; b.bonds={{0,1,{},3}}; b.scalarProperties.erase("Charge");
    const auto originalA=a.atoms,originalB=b.atoms;
    layers::Options o; o.matching=1;
    std::vector<layers::Detail> details{{"Copper",3,.75,0},{"Nickel",2,0,0}};
    auto built=layers::build({&a,&b},details,o); const auto &out=built.data;
    require(out.atoms.size()==4 && out.species==std::vector<std::string>{"Cu","Ni"},"heterostructure merges counts and species");
    // Copper volume 2*2*4=16: matched area 4*4=16 => thickness 1.
    require(close(out.cell[8],8) && close(out.cell[6],2) && close(out.cell[7],1),"constant volume preserves each tilted layer volume and adds normal gaps");
    require(close(out.atoms[0].x,3.5) && close(out.atoms[0].y,.25) && close(out.atoms[0].z,.25),"fractional matching, origin and fractional offset");
    require(close(out.atoms[1].x,1.5) && close(out.atoms[2].z,4.5),"in-plane wrapping and cumulative tilted stack");
    require(out.bonds.size()==3 && built.cutBonds==1 && out.bonds[0].image==std::array<int32_t,3>{1,0,0} && out.bonds[0].order==2,"offset wrapping remaps periodic images, retains order and cuts c crossing");
    const auto v=bondVector(out,out.bonds[0]); require(close(v[0],2) && close(v[1],0) && close(v[2],0),"wrapped bond is the transformed physical bond");
    require(out.scalarProperties.at("Charge")[1]==2 && std::isnan(out.scalarProperties.at("Charge")[2]) &&
            out.vectorProperties.at("Force")[0].x==1,"property rows preserve source values and missing values are NaN");
    require(out.scalarProperties.at("AtomX.Layer")==std::vector<double>{1,1,2,2} && motion::catalog(out).size()==2 &&
            motion::members(out,1)==std::vector<int>{0} && motion::members(out,2)==std::vector<int>{2},"layer labels and remapped independent motion groups");
    require(out.globalAttributes.at("Layer1.TotalEnergy")==12 && !out.globalAttributes.contains("TotalEnergy"),"aggregate source attributes are namespaced");
    o.constantVolume=false; auto thickness=layers::build({&a,&b},details,o);
    require(close(thickness.data.cell[8],11),"constant thickness leaves crystal slab thickness unchanged");
    o.matching=-1; const auto matched=layers::match({&a,&b},o);
    require(close(matched.al,3) && close(matched.bl,3) && matched.mismatch[0]>5,"average lattice and mismatch preflight");
    // Surface inputs contain exterior vacuum: use actual normal atom extent.
    Dataset surface=b; surface.pbc[2]=false; surface.cell[8]=30;
    surface.atoms={{1,1,12,0},{3,1,14,0}}; surface.bonds={{0,1,{},1}};
    o.matching=1; o.surface=true; o.constantVolume=true;
    details[0].offsetA=0; details[1].gap=99;
    auto slab=layers::build({&surface,&b},details,o);
    require(slab.data.pbc==std::array<bool,3>{true,true,false} && close(slab.data.cell[8],7) && close(slab.data.atoms[0].z,0) && close(slab.data.atoms[2].z,5.5),"surface strips source vacuum and ignores last gap");
    // Monolayers are supported, without inventing a nonzero atomic thickness.
    surface.atoms[1].z=surface.atoms[0].z; details[0].gap=3;
    auto twoSheets=layers::build({&surface,&surface},details,o);
    require(close(twoSheets.data.cell[8],3) && close(twoSheets.data.atoms[2].z,3),"monolayers stack by explicit gap");
    // Rotate source reference lattice into yz; output uses selected orientation.
    Dataset rotated=b; rotated.cell={0,4,0,0,0,4,2,0,0}; rotated.atoms={{.5f,1,1,0},{.5f,3,1,0}};
    o.surface=false; o.orientation=1; o.matching=1; details[1].gap=2;
    auto orient=layers::build({&b,&rotated,&b},{details[0],details[1],{"third",1,.2,.1}},o);
    require(close(orient.data.cell[0],0) && close(orient.data.cell[1],4) && close(orient.data.cell[8],0) && close(orient.data.cell[6],12),"three-layer stack adopts chosen source orientation");
    require(a.atoms[0].x==originalA[0].x && b.atoms[0].x==originalB[0].x && a.bonds.size()==3,"inputs remain independent and unchanged");
    o.atomLimit=3;
    require(fail([&]{(void)layers::build({&a,&b},details,o);}),"atom budget rejects before allocation"); o.atomLimit=100;
    auto sampled=a; sampled.stride=2;
    require(fail([&]{(void)layers::build({&sampled,&b},details,o);}),"sampled sources rejected");
    auto invalid=a; invalid.pbc[0]=false;
    require(fail([&]{(void)layers::build({&invalid,&b},details,o);}),"nonperiodic source rejected");
    invalid=a; invalid.cell[8]=-4;
    require(fail([&]{(void)layers::build({&invalid,&b},details,o);}),"left handed cell rejected");
    details[0].gap=-1;
    require(fail([&]{(void)layers::build({&a,&b},details,o);}),"negative vacuum rejected without mutation");
    Dataset large; large.species={"Cu"}; large.pbc={true,true,true}; large.cell={100,0,0,0,100,0,0,0,100};
    large.atoms.resize(70000); large.sourceCount=large.atoms.size();
    for(size_t i=0;i<large.atoms.size();++i) large.atoms[i]={float(i%100),float((i/100)%100),float(i/10000),0};
    o={}; const auto many=layers::build({&large,&large},{{"one",3,0,0},{"two",3,0,0}},o);
    require(many.data.atoms.size()==140000 && many.data.bonds.empty(),"large layers exceed old 60000 cap without generating neighbors or topology");
}
