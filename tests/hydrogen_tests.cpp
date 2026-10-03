#include "../src/hydrogen_adjust.hpp"
#include "../src/creation_document.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
using namespace atomx;
void requireH(bool ok,const char *message){if(!ok)throw std::runtime_error(message);}
Dataset molecule(std::initializer_list<std::string> symbols,std::initializer_list<Vec3> positions) {
    Dataset d;d.pbc={false,false,false};d.cell={20,0,0,0,20,0,0,0,20};
    auto pos=positions.begin();for(const auto &s:symbols){const auto t=authoring::speciesIndex(d,s);const auto p=*pos++;d.atoms.push_back({p.x,p.y,p.z,t});}
    d.sourceCount=d.atoms.size();d.bounds();return d;
}
int main() {try {
    using namespace hydrogens;using namespace authoring;
    Options all;
    auto methane=molecule({"C"},{{0,0,0}});
    auto p=prepare(methane,all);requireH(p.added.size()==4 && p.sites==1,"isolated carbon makes methane");
    apply(methane,p);requireH(methane.atoms.size()==5 && methane.bonds.size()==4,"explicit CH bonds retained");
    for(size_t i=1;i<5;++i)for(size_t j=i+1;j<5;++j)
        requireH(std::abs(dot(unit(point(methane,int(i))),unit(point(methane,int(j))))+1./3)<1e-5,"four tetrahedral H directions are distinct");
    requireH(!prepare(methane,all).changes(),"repeated adjustment is a no-op");
    for(int order=1;order<=3;++order) {
        auto d=molecule({"C","C"},{{0,0,0},{1.5f,0,0}});d.bonds.push_back({0,1,{},uint8_t(order)});
        const auto plan=prepare(d,all);requireH(plan.added.size()==size_t(2*(4-order)),"bond order affects H count");apply(d,plan);
        requireH(!prepare(d,all).changes(),"ethylene/ethyne/ethane adjustment stable");
        if(order==3)requireH(std::abs(point(d,2).y)<1e-5 && point(d,2).x<0,"SP geometry linear");
        if(order==2)for(size_t i=2;i<d.atoms.size();++i)requireH(std::abs(point(d,int(i)).y)<1e-5,"SP2 geometry planar");
    }
    auto ring=molecule({"C","C","C","C","C","C"},{{1.4f,0,0},{.7f,1.2124f,0},{-.7f,1.2124f,0},{-1.4f,0,0},{-.7f,-1.2124f,0},{.7f,-1.2124f,0}});
    for(uint32_t i=0;i<6;++i)ring.bonds.push_back({i,(i+1)%6,{},4});
    auto aromatic=prepare(ring,all);requireH(aromatic.added.size()==6,"benzene aromatic orders give one H per C");apply(ring,aromatic);
    for(size_t i=6;i<12;++i)requireH(std::abs(point(ring,int(i)).z)<1e-5,"benzene hydrogens planar");
    ring.atoms[0].type=speciesIndex(ring,"N");requireH(prepare(ring,all).ambiguous==1,"ambiguous aromatic N never guessed");
    for(const auto &[symbol,count]:std::vector<std::pair<std::string,size_t>>{{"N",3},{"O",2},{"F",1},{"Cl",1},{"Si",4},{"P",3},{"S",2},{"Cu",0}}) {
        auto d=molecule({symbol},{{0,0,0}});const auto plan=prepare(d,all);requireH(plan.added.size()==count,"common main group valence");
        if(symbol=="Cu")requireH(plan.unsupported==1,"metals skipped");
    }
    auto ammonium=molecule({"N"},{{0,0,0}});ammonium.scalarProperties["Charge"]={1};
    requireH(prepare(ammonium,all).added.size()==3,"partial charge not formal charge");
    ammonium.scalarProperties["FormalCharge"]={1};requireH(prepare(ammonium,all).added.size()==4,"formal ammonium charge used");
    Options charged;charged.setCharge=true;charged.charge=-1;
    auto chargedPlan=prepare(ammonium,charged);requireH(chargedPlan.added.size()==2 && chargedPlan.settings.size()==1,"explicit formal charge override");apply(ammonium,chargedPlan);
    requireH(ammonium.scalarProperties["AtomX.FormalCharge"][0]==-1 && ammonium.scalarProperties["Charge"][0]==1,"formal setting does not overwrite partial charge");
    auto surplus=methane;surplus.atoms[0].type=speciesIndex(surplus,"O");
    surplus.scalarProperties["Charge"]={.1,.2,.3,.4,.5};surplus.scalarProperties["AtomX.MotionGroup"]={1,1,1,2,2};
    surplus.vectorProperties["Force"]={{0,1,0},{0,1,0},{1,0,0},{0,0,1},{0,0,1}};
    surplus.particleColors={{1,0,0},{0,1,0},{0,0,1},{1,1,0},{1,0,1}};
    surplus.globalAttributes["Fixture"]=7;surplus.propertyComponents["Force"]="X Y Z";
    DataTable table;table.name="fixture";surplus.tables.push_back(table);
    auto surplusPlan=prepare(surplus,all);requireH(surplusPlan.removed.size()==2,"element change removes surplus terminal hydrogens");
    const auto oldO=point(surplus,0),oldHeavyForce=surplus.vectorProperties["Force"][0];
    apply(surplus,surplusPlan);requireH(surplus.atoms.size()==3 && surplus.bonds.size()==2,"only surplus H removed");
    requireH(length(sub(point(surplus,0),oldO))==0 && length(sub(surplus.vectorProperties["Force"][0],oldHeavyForce))==0,"heavy coordinates and vectors fixed");
    requireH(surplus.scalarProperties["Charge"][2]==.3 && surplus.particleColors[2].z==1 && surplus.tables[0].name=="fixture" && surplus.globalAttributes["Fixture"]==7,"scientific data identity survives compaction");
    auto preserving=methane;preserving.atoms[0].type=speciesIndex(preserving,"O");Options addOnly;addOnly.onlyAdd=true;
    requireH(!prepare(preserving,addOnly).changes(),"add-only keeps surplus hydrogens and positions");
    auto reposition=molecule({"C","C","H"},{{0,0,0},{1.5f,0,0},{0,1.5f,0}});reposition.bonds={{0,1,{},1},{0,2,{},1}};
    reposition.vectorProperties["Force"]={{0,0,0},{1,0,0},{0,2,0}};
    Options localCarbon;localCarbon.all=false;localCarbon.selection={0};
    const auto repositionPlan=prepare(reposition,localCarbon);
    requireH(repositionPlan.moved.size()==1 && repositionPlan.added.size()==2,"existing H repositioned without changing identity");
    apply(reposition,repositionPlan);requireH(std::abs(length(reposition.vectorProperties["Force"][2])-2)<1e-5 &&
        length(sub(unit(reposition.vectorProperties["Force"][2]),unit(point(reposition,2))))<1e-5,"finite H vector rotates with local direction");
    requireH(!prepare(reposition,localCarbon).changes(),"repositioning is idempotent");
    auto scope=molecule({"C","C"},{{0,0,0},{8,0,0}});scope.scalarProperties["Mass"]={12,13};scope.vectorProperties["Force"]={{1,2,3},{4,5,6}};scope.particleColors={{1,0,0},{0,1,0}};
    Options selected;selected.all=false;selected.selection={0};auto local=prepare(scope,selected);apply(scope,local);
    requireH(scope.atoms.size()==6 && scope.bonds.size()==4 && point(scope,1).x==8,"selection scope keeps other fragment intact");
    requireH(std::isnan(scope.scalarProperties["Mass"][2]) && std::isnan(scope.vectorProperties["Force"][2].x) && scope.particleColors[2].x==-1,"new scientific values remain missing");
    selected.selection={2};requireH(!prepare(scope,selected).changes(),"selected terminal H addresses heavy parent");
    std::vector<uint8_t> hidden(scope.atoms.size());hidden[2]=1;requireH(prepare(scope,selected,hidden).hidden==1,"hidden terminal H protects complete site");
    auto stale=prepare(scope,all);scope.atoms[1].x+=1;
    bool rejected=false;try{apply(scope,stale);}catch(const std::invalid_argument&){rejected=true;}requireH(rejected,"stale positions rejected before edits");
    auto periodic=molecule({"C","C"},{{0,0,0},{2,0,0}});periodic.pbc[0]=true;periodic.bonds.push_back({0,1,{1,0,0},1});
    auto skip=prepare(periodic,all);requireH(skip.periodic==2 && !skip.changes() && periodic.bonds[0].image[0]==1,"periodic sites explicitly skipped with topology untouched");
    auto linear=molecule({"C","C","C"},{{0,0,0},{1,0,0},{-1,0,0}});linear.bonds={{0,1,{},1},{0,2,{},1}};
    selected.selection={0};requireH(prepare(linear,selected).invalid==1,"degenerate fixed directions cannot generate NaN geometry");
    auto malformed=molecule({"C"},{{0,0,0}});malformed.scalarProperties["Broken"]={};rejected=false;
    try{prepare(malformed,all);}catch(const std::invalid_argument&){rejected=true;}requireH(rejected,"malformed property rows rejected, not erased");
    Options sp2;sp2.hybridization=2;auto planar=molecule({"N"},{{0,0,0}});apply(planar,prepare(planar,sp2));
    requireH(planar.scalarProperties["AtomX.Hybridization"][0]==2 && planar.atoms.size()==4,"explicit planar N geometry stored");
    requireH(!prepare(planar,all).changes(),"stored hybridization reused");
    Options reset;reset.hybridization=4;auto resetPlan=prepare(planar,reset);
    requireH(resetPlan.settings.size()==1,"explicit hybridization can be reset to automatic");
    auto automaticN=planar;apply(automaticN,resetPlan);
    requireH(automaticN.scalarProperties["AtomX.Hybridization"][0]==0 && !prepare(automaticN,all).changes(),"reset is persisted and stable");
    const auto native=std::filesystem::temp_directory_path()/("AtomX-hydrogen-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".atomx");
    document::View view;view.creation=true;std::ofstream file(native,std::ios::binary);document::write(file,planar,view);file.close();
    const auto saved=document::read(native);requireH(saved.data.bonds.size()==3 && saved.data.scalarProperties.at("AtomX.Hybridization")[0]==2,"chemical settings native document round trip");std::filesystem::remove(native);
    auto extended=molecule({"C"},{{0,0,0}});extended.scalarProperties["FormalCharge"]={0};
    extended.scalarProperties["AtomX.Hybridization"]={0};extended.scalarProperties["Charge"]={.2};
    extended.vectorProperties["Force"]={{1,2,3}};extended.particleColors={{.1f,.2f,.3f}};
    extended.atoms.push_back({8,0,0,0});extendRows(extended,1);
    requireH(extended.scalarProperties["FormalCharge"][1]==0 && extended.scalarProperties["AtomX.Hybridization"][1]==0 &&
        std::isnan(extended.scalarProperties["Charge"][1]) && extended.vectorProperties["Force"][0].x==1,
        "new sketch rows keep measured science missing and chemical settings neutral automatic");
    Options localScope;localScope.all=false;localScope.selection={1};auto scoped=prepare(extended,localScope);
    requireH(scoped.added.size()==4 && scoped.sites==1,"new chemical rows allow local automatic hydrogen");
    requireH(remapIndex(7,{2,4,8})==5 && remapIndex(4,{2,4,8})==-1 && remapIndex(-1,{2})==-1,"compaction distinguishes removed and shifted selections");
    Dataset ringDraft;ringDraft.scalarProperties["AtomX.Hybridization"]={};ringDraft.scalarProperties["Charge"]={};
    const auto ringEdit=prepareRing(ringDraft,ringSketch(ringDraft,6,{4,4,0},{1,0,0},{0,0,1},-1,-1,true));
    applyRing(ringDraft,ringEdit);requireH(prepare(ringDraft,all).added.size()==6 && std::isnan(ringDraft.scalarProperties["Charge"][0]),
        "ring defaults preserve measured missing values and automatic chemical settings");
    Dataset large;large.species={"C"};large.atoms.resize(60000);
    for(size_t i=0;i<large.atoms.size();++i){large.atoms[i].x=float(i*5);if(i%2)large.bonds.push_back({uint32_t(i-1),uint32_t(i),{},3});}
    large.sourceCount=large.atoms.size();large.bounds();const auto start=std::chrono::steady_clock::now();auto bulk=prepare(large,all);
    const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
    requireH(bulk.added.size()==60000 && bulk.sites==60000,"large graph has no old 2500-atom cutoff");
    std::cout<<"PASS: explicit valence, ideal geometry, scoped changes, preservation, native settings, 60000 atoms in "<<ms<<" ms\n";return 0;
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;} }
