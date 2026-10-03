#include "../src/motion_groups.hpp"
#include "../src/creation_document.hpp"
#include "../src/fragment_library.hpp"
#include <chrono>
#include <iostream>
using namespace atomx;
void check(bool ok,const char *message) { if (!ok) throw std::runtime_error(message); }
int main() {
    try {
        Dataset d; d.species={"C","H"}; d.atoms={{0,0,0,0},{2,0,0,1},{4,0,0,1},{8,0,0,0}};
        d.bonds={{0,1,{},1},{1,2,{},2}}; d.scalarProperties["Mass"]={12,1,1,12};
        const auto assignment=motion::automatic(d.atoms.size(),d.bonds);
        check(assignment.labels.size()==2 && assignment.values[0]==assignment.values[2] && assignment.values[3]!=assignment.values[0],
              "automatic grouping uses explicit connectivity and includes isolated atoms");
        auto network=d; network.bonds.push_back({2,0,{1,0,0},1});
        const auto periodic=motion::automatic(4,network.bonds);
        check(periodic.skipped==3 && periodic.labels.size()==1 && periodic.values[0]==0,"periodic networks excluded as a whole");
        const auto key=motion::prepare(d,{0,1},"Rigid end"); motion::create(d,{0,1},"Rigid end",key);
        check(motion::catalog(d)[0].count==2 && motion::members(d,key)==std::vector<int>{0,1},"manual grouping records unique membership");
        bool rejected=false; try { motion::prepare(d,{1,2},"overlap"); } catch (...) { rejected=true; }
        check(rejected && motion::members(d,key).size()==2,"overlapping group rejected before publication");
        motion::rename(d,key,"Renamed end");
        const auto center=motion::massCenter(d,{0,1});
        check(center && std::abs(center->x-2.f/13)<1e-6,"mass center uses explicit Mass property");
        const auto moved=authoring::transformedSelection(d,{0,1},{},{0,0,1},90,center);
        check(std::abs(moved[0].second.x-2.f/13)<1e-5 && std::abs(moved[1].second.y-24.f/13)<1e-5,
              "group rotation is rigid about its mass center");
        d.bounds(); d.sourceCount=4;
        const auto file=std::filesystem::temp_directory_path()/("atomx-group-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".atomx");
        { std::ofstream output(file,std::ios::binary); document::write(output,d); }
        const auto reopened=document::read(file); std::filesystem::remove(file);
        check(motion::catalog(reopened.data)[0].name=="Renamed end" && motion::members(reopened.data,key)==std::vector<int>{0,1},
              "group names and membership survive native document round trip");
        authoring::eraseAtoms(d,{0}); check(motion::members(d,key)==std::vector<int>{0},"deletion remaps group members with scientific properties");
        motion::erase(d,key); check(motion::catalog(d).empty() && d.atoms.size()==3,"ungroup retains all atoms and positions");
        d.atoms.push_back({12,0,0,0}); d.scalarProperties[motion::property].push_back(NAN);
        check(motion::catalog(d).empty(),"new atom missing values mean unassigned");
        rejected=false; try { motion::automatic(513,{}); } catch (...) { rejected=true; }
        check(rejected,"excessive disconnected group count rejected");
        auto fragment=fragments::builtins()[0]; motion::create(fragment.data,{0,1},"inside",1);
        const auto custom=fragments::define(fragment.data,1,"clean fragment","library");
        check(!custom.data.scalarProperties.contains(motion::property),"custom fragment definition does not copy source group identity");
        Dataset target=fragment.data;
        auto placement=fragments::place(target,fragment,1,{8,0,0},{1,0,0},{0,0,1}); fragments::apply(target,fragment,placement);
        check(motion::members(target,1)==std::vector<int>{0,1},"new fragment atoms cannot join an unrelated target group by numeric ID");
        std::cout<<"motion group tests: PASS\n"; return 0;
    } catch(const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
}
