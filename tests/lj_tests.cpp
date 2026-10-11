#include "../src/lj.hpp"
#include <iostream>
#include <numeric>
using namespace atomx;
void require(bool ok,const char *message){if(!ok)throw std::runtime_error(message);}
void near(double a,double b,double tolerance,const char *message){require(std::abs(a-b)<=tolerance*(1+std::abs(b)),message);}
template<class F>void rejects(F f){bool caught=false;try{f();}catch(const std::exception &){caught=true;}require(caught,"Invalid input rejected");}
// Independent exhaustive lattice oracle, intentionally no spatial grid or
// production mixing/evaluator functions. +/-6 covers these small fixtures.
lj::Result brute(const Dataset &d,const lj::Options &o) {
    lj::Result r;r.forces.resize(d.atoms.size());r.atomicEnergy.resize(d.atoms.size());
    for(size_t i=0;i<d.atoms.size();++i)for(size_t j=0;j<d.atoms.size();++j)
        for(int x=-6;x<=6;++x)for(int y=-6;y<=6;++y)for(int z=-6;z<=6;++z) {
            if((x && (!o.periodic || !d.pbc[0]))||(y && (!o.periodic || !d.pbc[1]))||(z && (!o.periodic || !d.pbc[2])))continue;
            if(i==j && !x && !y && !z)continue;
            const auto &a=o.types[d.atoms[i].type],&b=o.types[d.atoms[j].type];
            const double sigma=o.geometric?std::sqrt(a.sigma*b.sigma):(a.sigma+b.sigma)/2;
            const double epsilon=std::sqrt(a.epsilon*b.epsilon),cut=o.geometric?std::sqrt(a.cutoff*b.cutoff):(a.cutoff+b.cutoff)/2;
            double delta[3],r2=0;
            for(int k=0;k<3;++k){delta[k]=double(coordinate(d.atoms[j],k))-coordinate(d.atoms[i],k)+x*d.cell[k]+y*d.cell[3+k]+z*d.cell[6+k];r2+=delta[k]*delta[k];}
            if(r2>=cut*cut)continue;
            const double q=std::pow(sigma/std::sqrt(r2),6),qc=std::pow(sigma/cut,6);
            const double e=4*epsilon*(q*q-q-(o.shift?qc*qc-qc:0));
            r.energy+=e/2;r.atomicEnergy[i]+=e/2;++r.pairs;
            for(int k=0;k<3;++k)r.forces[i][k]+=24*epsilon*(q-2*q*q)/r2*delta[k];
        }
    r.pairs/=2;return r;
}
int main(int argc,char **argv) {
    try {
        Dataset d;d.species={"Ar"};d.atoms={{0,0,0,0},{1,0,0,0}};d.sourceCount=2;
        lj::Options o;o.types={{"test",1,1,2.5}};o.shift=false;
        auto r=lj::calculate(d,o);near(r.energy,0,1e-12,"sigma zero crossing");
        near(r.forces[0][0],-24,1e-12,"repulsive force sign/magnitude");
        near(r.forces[1][0],24,1e-12,"Newton third law");require(r.pairs==1,"pair counted once");
        d.atoms[1].x=float(std::pow(2.,1./6.));r=lj::calculate(d,o);
        near(r.energy,-1,1e-12,"equilibrium energy");near(r.maxForce,0,2e-5,"equilibrium force");
        o.shift=true;r=lj::calculate(d,o);const double c6=std::pow(1/2.5,6);
        near(r.energy,-1-4*(c6*c6-c6),1e-12,"energy shift");
        d.atoms[1].x=2.5f;r=lj::calculate(d,o);require(r.energy==0 && r.pairs==0,"cutoff excludes boundary");
        d.species.push_back("Ne");d.atoms[1]={1.7f,.2f,0,1};o.types.push_back({"other",1.3,.25,3.1});
        for(bool geometric:{false,true}){o.geometric=geometric;r=lj::calculate(d,o);auto expected=brute(d,o);
            near(r.energy,expected.energy,1e-12,"mixed-species energy");near(r.forces[0][0],expected.forces[0][0],1e-12,"mixed force");}
        const double h=.001;const float original=d.atoms[1].x;
        d.atoms[1].x=original+float(h);const double ep=lj::calculate(d,o).energy;
        d.atoms[1].x=original-float(h);const double em=lj::calculate(d,o).energy;
        const double distance=d.atoms[1].x;d.atoms[1].x=original;r=lj::calculate(d,o);
        near(r.forces[1][0],-(ep-em)/(double(original+float(h))-distance),1e-5,"force is negative energy derivative");
        // Minimum-image and multiple-image sums, self images, skew cells,
        // partial PBC, negative positions and out-of-cell translations.
        for(double length:{10.,2.})for(bool skew:{false,true})for(int mask:{1,3,7}) {
            d.cell={length,0,0,skew?.6:0,length,0,skew?.3:0,skew?.2:0,length};
            d.pbc={bool(mask&1),bool(mask&2),bool(mask&4)};
            d.atoms={{-.15f,.1f,.2f,0},{1.2f,.6f,.7f,1},{.4f,1.3f,.9f,0}};d.sourceCount=3;
            for(bool geometric:{false,true}){o.geometric=geometric;const auto actual=lj::calculate(d,o),expected=brute(d,o);
                near(actual.energy,expected.energy,2e-10,"periodic lattice energy");require(actual.pairs==expected.pairs,"periodic pair count");
                for(size_t i=0;i<d.atoms.size();++i)for(int k=0;k<3;++k)near(actual.forces[i][k],expected.forces[i][k],2e-10,"periodic force oracle");
                near(std::accumulate(actual.atomicEnergy.begin(),actual.atomicEnergy.end(),0.),actual.energy,1e-12,"atomic energies sum");
            }
        }
        d.species={"Ar"};d.atoms={{0,0,0,0}};d.sourceCount=1;d.cell={2,0,0,0,2,0,0,0,2};d.pbc={true,true,true};o.types.resize(1);
        r=lj::calculate(d,o);require(r.pairs==3,"six self images contribute three half-pairs");near(r.maxForce,0,1e-12,"self images have zero net force");
        d.cell[0]=-2;const auto reversed=lj::calculate(d,o);near(reversed.energy,r.energy,1e-12,"reversed lattice basis preserves periodic energy");
        d.cell={10,0,0,0,10,0,0,0,10};d.atoms={{.2f,.4f,.3f,0},{1.4f,.5f,.6f,0}};d.sourceCount=2;
        r=lj::calculate(d,o);d.atoms[1].x+=20;const auto translated=lj::calculate(d,o);
        near(translated.energy,r.energy,2e-6,"out-of-cell lattice translation invariance");
        d.atoms={{0,0,0,0}};d.sourceCount=1;
        o.periodic=false;r=lj::calculate(d,o);require(r.pairs==0,"isolated single atom has no pairs");
        d.atoms.push_back(d.atoms[0]);rejects([&]{lj::calculate(d,o);});d.atoms.pop_back();
        d.stride=2;rejects([&]{lj::calculate(d,o);});d.stride=1;
        std::atomic<bool> cancel{true};rejects([&]{lj::calculate(d,o,&cancel);});
        o.types[0].sigma=NAN;rejects([&]{lj::calculate(d,o);});
        require(std::size(lj::openkimParameters)==118,"118 real-element OpenKIM preset rows");
        near(lj::openkim("C").epsilon,6.36953,1e-12,"official carbon dissociation energy retained");
        near(lj::openkim("Og").sigma,2.79742,1e-12,"Og preset supported independently of display table");
        rejects([&]{lj::openkim("unknown");});
        if(argc>1){const auto uff=lj::readUff(argv[1]);require(uff.size()==128,"all user UFF types imported");
            auto carbon=std::find_if(uff.begin(),uff.end(),[](const auto &p){return p.name=="C_3";});require(carbon!=uff.end(),"UFF C_3 exists");
            near(carbon->sigma,3.851/std::pow(2.,1./6.),1e-12,"UFF sigma derived from original x");
            near(carbon->epsilon,.105*.043364104241800934,1e-12,"UFF original D converted to eV");}
        std::cout<<"LJ energy, forces, mixing, periodic oracle, cancellation, parameters: passed\n";
    } catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
