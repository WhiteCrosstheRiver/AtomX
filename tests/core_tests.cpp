#include "../src/analysis.hpp"
#include <iostream>
using namespace atomx;
void require(bool b, const char *s) {
    if (!b)
        throw std::runtime_error(s);
}
int main() {
    try {
        auto angleOutputs=modifierOutputs(Modifier{Op::BondAngleDistribution},7);
        require(angleOutputs.size()==2 &&
                    angleOutputs[0].kind==DataObject::Kind::Table &&
                    angleOutputs[1].kind==DataObject::Kind::GlobalAttributes &&
                    angleOutputs[0].sourceNode==7,
                "bond analysis pipeline metadata declares its table and global-statistics outputs");
        auto cnaOutputs=modifierOutputs(Modifier{Op::CommonNeighborAnalysis},2);
        require(cnaOutputs.size()==3 && cnaOutputs[0].kind==DataObject::Kind::Particles &&
                    cnaOutputs[1].kind==DataObject::Kind::GlobalAttributes &&
                    cnaOutputs[2].kind==DataObject::Kind::Table,
                "CNA pipeline metadata declares every published result object");
        ModifierNode colorSnapshot{Op::ColorCoding};
        colorSnapshot.id="stable-color-node";
        colorSnapshot.colorAllFramesRange=true;
        colorSnapshot.colorMin=-7; colorSnapshot.colorMax=13;
        std::vector<ModifierNode> historyCurrent{colorSnapshot};
        std::vector<std::vector<ModifierNode>> historyUndo{historyCurrent}, historyRedo;
        historyCurrent[0].colorAllFramesRange=false;
        historyCurrent[0].colorMin=0; historyCurrent[0].colorMax=1;
        require(applyModifierHistory(historyCurrent,historyUndo,historyRedo,false) &&
                    historyCurrent[0].id=="stable-color-node" && historyCurrent[0].colorAllFramesRange &&
                    historyCurrent[0].colorMin==-7 && historyCurrent[0].colorMax==13,
                "undo restores trajectory-wide color range settings without dropping node identity");
        require(applyModifierHistory(historyCurrent,historyUndo,historyRedo,true) &&
                    !historyCurrent[0].colorAllFramesRange && historyCurrent[0].colorMin==0 &&
                    historyCurrent[0].colorMax==1,
                "redo restores the complete post-edit color range state");
        auto p = std::filesystem::temp_directory_path() / "atomx-core-fixture.xyz";
        {
            std::ofstream f(p);
            f << "4\nLattice=\"10 0 0 0 10 0 0 0 10\" Properties=species:S:1:pos:R:3 pbc=\"T F "
                 "T\"\nCu 0 1 2\nNi 3 4 5\nCu 6 7 8\nNi 11 2 -1\n1\nsecond\nC 9 8 7\n";
        }
        auto ix = indexXYZ(p);
        require(ix.size() == 2, "frame index");
        auto d = readXYZ(p, ix[0]);
        require(d.atoms.size() == 4 && d.species.size() == 2, "reader");
        {
            auto input = d;
            input.scalarProperties["Energy"] = {1, 2, 3, 4};
            input.vectorProperties["Velocity"] = std::vector<Vec3>(4);
            Modifier remove{Op::RemoveProperty}; remove.property = "Energy";
            auto output = evaluate(input, {remove});
            require(!output.data.scalarProperties.contains("Energy") &&
                    output.data.vectorProperties.contains("Velocity") &&
                    input.scalarProperties.contains("Energy"), "non-destructive property removal");
            remove.property = "Velocity";
            require(evaluate(input, {remove}).data.vectorProperties.empty(), "vector property removal");
            remove.enabled = false;
            require(evaluate(input, {remove}).data.vectorProperties.contains("Velocity"), "disabled removal");
        }
        require(d.cell[8] == 10 && d.pbc[0] && !d.pbc[1], "metadata");
        auto sample = readXYZ(p, ix[0], 2);
        require(sample.atoms.size() == 2 && sample.stride == 2 && sample.sourceCount == 4,
                "bounded sample");
        require(readXYZ(p, ix[1]).atoms[0].x == 9, "seek frame");
        auto r = evaluate(d, {{Op::SelectType, true, 0, 2, 1}, {Op::Delete}});
        require(r.data.atoms.size() == 2 && d.atoms.size() == 4,
                "selection deletion nondestructive");
        r = evaluate(d, {{Op::Slice, true, 4, 2}});
        require(r.data.atoms.size() == 2, "slice");
        r = evaluate(d, {{Op::Wrap}});
        require(r.data.atoms.back().x == 1 && r.data.atoms.back().z == 9, "periodic wrapping");
        Dataset triclinicWrap; triclinicWrap.species={"X"};
        triclinicWrap.cell={2,0,0, 1,2,0, 0,0,2}; triclinicWrap.origin={1,2,3};
        triclinicWrap.pbc={true,true,false}; triclinicWrap.atoms={{3.1f,1.4f,4,0}};
        auto wrappedTriclinic=evaluate(triclinicWrap,{{Op::Wrap}});
        require(std::abs(wrappedTriclinic.data.atoms[0].x-2.1f)<1e-5f &&
                    std::abs(wrappedTriclinic.data.atoms[0].y-3.4f)<1e-5f &&
                    wrappedTriclinic.data.atoms[0].z==4,
                "triclinic partial-periodic wrapping preserves the non-periodic component and cell origin");
        r = evaluate(d, {{Op::Scale, true, 2}});
        require(r.data.cell[0] == 20 && r.data.atoms[1].x == 6, "scale");
        r = evaluate(d, {{Op::SelectRange, true, 2, 0, 0, 7}, {Op::EditType,true,0,2,0}});
        require(r.selected[1] && r.selected[2] && !r.selected[0] && r.data.atoms[1].type == 0,
                "range selection and selected type assignment");
        r = evaluate(d, {{Op::SelectType,true,0,2,1}, {Op::Replicate,true,0,0,3}});
        require(r.data.atoms.size() == 12 && r.data.cell[0] == 30 &&
                r.data.atoms[8].x == 20 && r.selected[9], "replication cell positions selection");
        r = evaluate(d, {{Op::Rotate,true,90,2}});
        require(std::abs(r.data.atoms[1].x + 4) < 1e-5 &&
                std::abs(r.data.atoms[1].y - 3) < 1e-5 &&
                std::abs(r.data.cell[1] - 10) < 1e-5, "rotation transforms cell and particles");
        bool invalidScale = false;
        try { evaluate(d, {{Op::Scale,true,-1}}); } catch (...) { invalidScale = true; }
        require(invalidScale, "invalid scale rejected");
        auto s = statistics(d, 0);
        require(s.min == 0 && s.max == 11 && s.mean == 5, "statistics");
        float sum = 0;
        for (auto v : s.histogram)
            sum += v;
        require(sum == 4, "histogram population");
        writeXYZ(p, d);
        auto round = readXYZ(p, indexXYZ(p)[0]);
        require(round.atoms.size() == 4 && round.pbc == d.pbc, "round trip");
        {
            std::ofstream f(p);
            f << "1\nProperties=id:I:1:pos:R:3:species:S:1\n99 1.5 2.5 3.5 Si\n";
        }
        auto custom = readXYZ(p, indexXYZ(p)[0]);
        require(custom.species[0] == "Si" && custom.atoms[0].y == 2.5, "reordered schema");
        {
            std::ofstream f(p);
            f << "2\ntruncated\nCu 0 0 0\n";
        }
        bool failed = false;
        try {
            indexXYZ(p);
        } catch (...) {
            failed = true;
        }
        require(failed, "truncated input rejected");
        auto fcc = crystal(4);
        fcc.pbc = {true, true, true};
        auto n = neighbors(fcc, .8f);
        require(n.clusters == 1 && n.meanCoordination == 12, "periodic FCC coordination");
        double pairs = 0, integral = 0;
        for (int i=0;i<128;++i) {
            pairs += n.pairHistogram[i];
            double lo=double(n.cutoff)*i/128, hi=double(n.cutoff)*(i+1)/128;
            integral += n.rdf[i]*(4.0/3.0)*3.141592653589793*(hi*hi*hi-lo*lo*lo)*4;
        }
        require(n.rdfValid && pairs == n.bonds && std::abs(integral-12) < 1e-4,
                "RDF integral recovers FCC coordination and histogram pair count");
        for (auto c : n.coordination)
            require(c == 12, "FCC nearest neighbors");
        auto coordinationPipeline = evaluate(fcc, {{Op::CoordinationAnalysis,true,.8f}});
        require(coordinationPipeline.data.scalarProperties.at("Coordination").size() == fcc.atoms.size() &&
                    coordinationPipeline.data.scalarProperties.at("Coordination")[0] == 12 &&
                    coordinationPipeline.data.globalAttributes.at("CoordinationAnalysis.mean") == 12,
                "coordination modifier publishes per-particle and global results");
        auto clusterPipeline = evaluate(fcc, {{Op::ClusterAnalysis,true,.8f}});
        require(clusterPipeline.data.globalAttributes.at("ClusterAnalysis.count") == 1 &&
                    clusterPipeline.data.scalarProperties.at("Cluster").size() == fcc.atoms.size() &&
                    clusterPipeline.data.tables.back().rows.size() == 1,
                "cluster modifier publishes labels and a cluster-size table");
        Dataset clusterFixture;
        clusterFixture.species = {"X"};
        clusterFixture.atoms = {{0,0,0,0},{.5f,0,0,0},{5,0,0,0},{5.5f,0,0,0},{10,0,0,0}};
        const auto disconnectedClusters = evaluate(clusterFixture, {{Op::ClusterAnalysis,true,.75f}});
        require(disconnectedClusters.data.globalAttributes.at("ClusterAnalysis.count") == 3 &&
                    disconnectedClusters.data.scalarProperties.at("Cluster") ==
                        std::vector<double>({1,1,2,2,3}) &&
                    disconnectedClusters.data.tables.back().rows ==
                        std::vector<std::vector<std::string>>({{"1","2"},{"2","2"},{"3","1"}}),
                "cluster pipeline assigns deterministic connected-component labels and exact table sizes");
        Dataset extremePeriodicIndex;
        extremePeriodicIndex.species={"X"};
        extremePeriodicIndex.atoms={{0,0,0,0},{.25f,0,0,0}};
        extremePeriodicIndex.cell={1e300,0,0,0,1,0,0,0,1};
        extremePeriodicIndex.pbc={true,false,false};
        bool extremePeriodicIndexRejected=false;
        try { (void)evaluate(extremePeriodicIndex,{{Op::CoordinationAnalysis,true,.5f}}); }
        catch (const ModifierExecutionError &e) {
            extremePeriodicIndexRejected=e.nodeIndex==0 &&
                std::string(e.what()).find("spatial-index range")!=std::string::npos;
        }
        require(extremePeriodicIndexRejected,
                "neighbor analysis rejects periodic cell ratios outside the exact spatial-index range");
        auto rdfPipeline = evaluate(fcc, {{Op::RadialDistribution,true,.8f}});
        require(rdfPipeline.data.tables.back().rows.size() == 128 &&
                    rdfPipeline.data.tables.back().columns[2] == "g(r)",
                "RDF modifier publishes tabular radial distribution data");
        Modifier histogram{Op::Histogram}; histogram.type=8; histogram.property="Position.X";
        auto histogramPipeline=evaluate(fcc,{histogram});
        uint64_t histogramPopulation=0;
        for (const auto &row : histogramPipeline.data.tables.back().rows)
            histogramPopulation += std::stoull(row[1]);
        require(histogramPipeline.data.tables.back().rows.size()==8 && histogramPopulation==fcc.atoms.size(),
                "histogram modifier bins every finite property value");
        Dataset extremeValues;
        extremeValues.species={"X"};
        extremeValues.atoms={{0,0,0,0},{1,0,0,0}};
        extremeValues.scalarProperties["Extreme"]={-std::numeric_limits<double>::max(),
                                                       std::numeric_limits<double>::max()};
        Modifier extremeHistogram{Op::Histogram}; extremeHistogram.type=2; extremeHistogram.property="Extreme";
        const auto extremeHistogramResult=evaluate(extremeValues,{extremeHistogram});
        require(extremeHistogramResult.data.tables.back().rows.size()==2 &&
                    std::stoull(extremeHistogramResult.data.tables.back().rows[0][1])==1 &&
                    std::stoull(extremeHistogramResult.data.tables.back().rows[1][1])==1,
                "histogram safely bins finite values whose direct range subtraction overflows");
        DataTable csvTable{"CSV quoting",{"Name","Value"},{{"alpha, beta","say \"hi\""}}};
        const auto csvPath=std::filesystem::temp_directory_path()/"atomx-data-table.csv";
        writeDataTableCsv(csvPath,csvTable);
        std::ifstream csvInput(csvPath,std::ios::binary);
        std::string csvText((std::istreambuf_iterator<char>(csvInput)),{});
        require(csvText=="\"Name\",\"Value\"\r\n\"alpha, beta\",\"say \"\"hi\"\"\"\r\n",
                "data table CSV export quotes delimiters and embedded quotes");
        Modifier reduce{Op::ReduceProperty}; reduce.property="Position.X"; reduce.reduceOperation=2;
        auto reduced=evaluate(fcc,{reduce});
        double expectedX=0; for (const auto &atom:fcc.atoms) expectedX+=atom.x; expectedX/=fcc.atoms.size();
        require(std::abs(reduced.data.globalAttributes.at("ReduceProperty.Position.X.mean")-expectedX)<1e-10,
                "reduce property publishes numeric global mean");
        Modifier extremeMean{Op::ReduceProperty}; extremeMean.property="Extreme"; extremeMean.reduceOperation=2;
        const auto extremeMeanResult=evaluate(extremeValues,{extremeMean});
        require(extremeMeanResult.data.globalAttributes.at("ReduceProperty.Extreme.mean")==0,
                "reduce-property mean avoids intermediate overflow for opposite extreme values");
        extremeValues.scalarProperties["Extreme"]={std::numeric_limits<double>::max(),
                                                      std::numeric_limits<double>::max()};
        const auto sameExtremeMean=evaluate(extremeValues,{extremeMean});
        require(sameExtremeMean.data.globalAttributes.at("ReduceProperty.Extreme.mean")==
                    std::numeric_limits<double>::max(),
                "reduce-property mean remains finite when every input is the largest finite value");
        Modifier overflowingSum{Op::ReduceProperty}; overflowingSum.property="Extreme"; overflowingSum.reduceOperation=3;
        bool overflowingSumRejected=false;
        try { (void)evaluate(extremeValues,{overflowingSum}); }
        catch (const ModifierExecutionError &e) { overflowingSumRejected=e.nodeIndex==0; }
        require(overflowingSumRejected,"reduce-property rejects sums outside the finite numeric range at their node");
        Dataset bondedMeasurements;
        bondedMeasurements.species={"X"};
        bondedMeasurements.atoms={{0,0,0,0},{1,0,0,0},{1,2,0,0},{9.9f,0,0,0}};
        bondedMeasurements.cell={10,0,0,0,10,0,0,0,10};
        bondedMeasurements.pbc={true,false,false};
        bondedMeasurements.bonds={{0,1,{0,0,0}},{1,2,{0,0,0}},{0,3,{-1,0,0}}};
        Modifier bondLengths{Op::BondLengthDistribution}; bondLengths.type=3;
        auto bondLengthResult=evaluate(bondedMeasurements,{bondLengths});
        const auto &bondLengthTable=bondLengthResult.data.tables.back();
        require(bondLengthTable.name=="Bond length distribution" && bondLengthTable.rows.size()==3 &&
                    bondLengthResult.data.globalAttributes.at("BondLengthDistribution.count")==3 &&
                    std::abs(bondLengthResult.data.globalAttributes.at("BondLengthDistribution.minimum")-.1)<1e-5 &&
                    bondLengthResult.data.globalAttributes.at("BondLengthDistribution.maximum")==2,
                "bond-length distribution analyzes explicit and periodic-image bonds and publishes a table");
        bool missingBondTopologyRejected=false;
        auto noBondTopology=bondedMeasurements; noBondTopology.bonds.clear();
        try { (void)evaluate(noBondTopology,{bondLengths}); }
        catch (const ModifierExecutionError &e) { missingBondTopologyRejected=e.nodeIndex==0; }
        require(missingBondTopologyRejected,"bond-length distribution reports missing topology at its node");
        Dataset largeBondGeometry;
        largeBondGeometry.species={"X"};
        largeBondGeometry.atoms={{0,0,0,0},{0,0,0,0},{0,0,0,0}};
        largeBondGeometry.cell={1e150,0,0,0,1e150,0,0,0,1e150};
        largeBondGeometry.pbc={true,true,false};
        largeBondGeometry.bonds={{0,1,{1,0,0}},{0,2,{0,1,0}}};
        const auto largeBondLengths=evaluate(largeBondGeometry,{bondLengths});
        require(largeBondLengths.data.globalAttributes.at("BondLengthDistribution.count")==2 &&
                    largeBondLengths.data.globalAttributes.at("BondLengthDistribution.minimum")==1e150 &&
                    largeBondLengths.data.globalAttributes.at("BondLengthDistribution.maximum")==1e150,
                "bond-length distribution preserves representable lengths whose naive square overflows");
        Modifier bondAngles{Op::BondAngleDistribution}; bondAngles.type=18;
        auto bondAngleResult=evaluate(bondedMeasurements,{bondAngles});
        const auto &bondAngleTable=bondAngleResult.data.tables.back();
        require(bondAngleTable.name=="Bond angle distribution" && bondAngleTable.rows.size()==18 &&
                    bondAngleResult.data.globalAttributes.at("BondAngleDistribution.count")==2 &&
                    std::abs(bondAngleResult.data.globalAttributes.at("BondAngleDistribution.minimum")-90)<1e-8 &&
                    std::abs(bondAngleResult.data.globalAttributes.at("BondAngleDistribution.maximum")-180)<1e-8,
                "bond-angle distribution enumerates central bond pairs and honors periodic image vectors");
        const auto largeBondAngles=evaluate(largeBondGeometry,{bondAngles});
        require(largeBondAngles.data.globalAttributes.at("BondAngleDistribution.count")==1 &&
                    std::abs(largeBondAngles.data.globalAttributes.at("BondAngleDistribution.minimum")-90)<1e-8,
                "bond-angle distribution normalizes large finite vectors before the dot product");
        bool missingBondAnglesRejected=false;
        try { (void)evaluate(noBondTopology,{bondAngles}); }
        catch (const ModifierExecutionError &e) { missingBondAnglesRejected=e.nodeIndex==0; }
        require(missingBondAnglesRejected,"bond-angle distribution reports missing topology at its node");
        Dataset zeroLengthTopology=bondedMeasurements;
        zeroLengthTopology.bonds={{0,0,{0,0,0}}};
        for (const auto &analysis : {bondLengths,bondAngles}) {
            bool zeroLengthRejected=false;
            try { (void)evaluate(zeroLengthTopology,{analysis}); }
            catch (const ModifierExecutionError &e) { zeroLengthRejected=e.nodeIndex==0; }
            require(zeroLengthRejected,"bond distributions reject zero-length topology consistently");
        }
        Dataset nonPeriodicImageTopology=bondedMeasurements;
        nonPeriodicImageTopology.bonds={{0,1,{0,1,0}}};
        for (const auto &analysis : {bondLengths,bondAngles}) {
            bool nonPeriodicImageRejected=false;
            try { (void)evaluate(nonPeriodicImageTopology,{analysis}); }
            catch (const ModifierExecutionError &e) { nonPeriodicImageRejected=e.nodeIndex==0; }
            require(nonPeriodicImageRejected,"bond distributions reject image shifts on non-periodic axes");
        }
        Dataset rangeFrameA; rangeFrameA.species={"X"}; rangeFrameA.atoms={{1,0,0,0},{3,0,0,0}};
        rangeFrameA.scalarProperties["Q"]={-5,8}; rangeFrameA.vectorProperties["Velocity"]={{0,2,1},{0,-4,3}}; rangeFrameA.bounds();
        Dataset rangeFrameB=rangeFrameA; rangeFrameB.atoms={{-4,0,0,0},{7,0,0,0}};
        rangeFrameB.scalarProperties["Q"]={2,12}; rangeFrameB.vectorProperties["Velocity"]={{0,6,1},{0,1,3}}; rangeFrameB.bounds();
        std::vector<Dataset> rangeFrames{rangeFrameA,rangeFrameB};
        Modifier upstreamTranslate{Op::Translate}; upstreamTranslate.axis=0; upstreamTranslate.value=5;
        auto trajectoryPositionRange=colorRangeAcrossFrames(rangeFrames.size(),
            [&](size_t i){return rangeFrames.at(i);},{upstreamTranslate},"Position.X");
        auto trajectoryPropertyRange=colorRangeAcrossFrames(rangeFrames.size(),
            [&](size_t i){return rangeFrames.at(i);},{},"Q");
        auto trajectoryVectorRange=colorRangeAcrossFrames(rangeFrames.size(),
            [&](size_t i){return rangeFrames.at(i);}, {},"Velocity.Y");
        require(trajectoryPositionRange.first==1 && trajectoryPositionRange.second==12 &&
                    trajectoryPropertyRange.first==-5 && trajectoryPropertyRange.second==12 &&
                    trajectoryVectorRange.first==-4 && trajectoryVectorRange.second==6,
                "all-frame color range evaluates the upstream pipeline and aggregates every frame");
        Dataset cellEditData; cellEditData.species={"X"};
        cellEditData.cell={10,0,0,0,10,0,0,0,10}; cellEditData.pbc={true,true,true};
        cellEditData.atoms={{1,2,3,0}}; cellEditData.bounds();
        cellEditData.vectorProperties["Velocity"]={{1,2,3}};
        Modifier editCell{Op::EditCell};
        editCell.editedCell={20,0,0,0,20,0,0,0,20}; editCell.editedOrigin={1,1,1};
        editCell.editedPbc={true,false,true};
        auto fixedCoordinates=evaluate(cellEditData,{editCell});
        require(fixedCoordinates.data.atoms[0].x==1 && fixedCoordinates.data.atoms[0].y==2 &&
                    fixedCoordinates.data.cell[0]==20 && fixedCoordinates.data.origin.x==1 &&
                    fixedCoordinates.data.pbc==editCell.editedPbc,
                "simulation-cell edits change cell metadata without moving particle coordinates by default");
        editCell.transformCoordinatesWithCell=true;
        auto transformedCoordinates=evaluate(cellEditData,{editCell});
        require(std::abs(transformedCoordinates.data.atoms[0].x-3)<1e-6 &&
                    std::abs(transformedCoordinates.data.atoms[0].y-5)<1e-6 &&
                    std::abs(transformedCoordinates.data.atoms[0].z-7)<1e-6,
                "optional simulation-cell remapping preserves fractional particle coordinates");
        editCell.editedCell[8]=0;
        bool singularCellRejected=false;
        try { evaluate(cellEditData,{editCell}); }
        catch (const ModifierExecutionError &e) { singularCellRejected=e.nodeIndex==0; }
        require(singularCellRejected,"simulation-cell editor rejects degenerate vectors at its pipeline node");
        Modifier affine{Op::AffineTransform};
        affine.affineTransform={2,1,0,-1, 0,3,0,2, 0,0,.5,4};
        auto affineResult=evaluate(cellEditData,{affine});
        require(affineResult.data.atoms[0].x==3 && affineResult.data.atoms[0].y==8 &&
                    affineResult.data.atoms[0].z==5.5f && affineResult.data.cell[0]==20 &&
                    affineResult.data.cell[3]==10 && affineResult.data.cell[4]==30 &&
                    affineResult.data.cell[8]==5 && affineResult.data.origin.x==-1 &&
                    affineResult.data.origin.y==2 && affineResult.data.origin.z==4 &&
                    affineResult.data.vectorProperties.at("Velocity")[0].x==1,
                "general affine matrix transforms positions, cell vectors and origin");
        affine.transformVectorProperties = true;
        auto affineVectors = evaluate(cellEditData,{affine});
        const auto transformedVelocity = affineVectors.data.vectorProperties.at("Velocity")[0];
        require(transformedVelocity.x==4 && transformedVelocity.y==6 && transformedVelocity.z==1.5f,
                "optional affine vector-property transform uses the linear matrix without translation");
        Modifier overflowingAffine = affine;
        overflowingAffine.affineTransform={1e20,0,0,0, 0,1e20,0,0, 0,0,1e20,0};
        Dataset overflowingVectors = cellEditData;
        overflowingVectors.vectorProperties["Velocity"][0].x = 1e20f;
        bool affineVectorOverflowRejected=false;
        try { evaluate(overflowingVectors,{overflowingAffine}); }
        catch (const ModifierExecutionError &e) { affineVectorOverflowRejected=e.nodeIndex==0; }
        require(affineVectorOverflowRejected,
                "affine vector-property overflow reports an error at its pipeline node");
        affine.affineTransform[8]=0; affine.affineTransform[9]=0; affine.affineTransform[10]=0;
        bool singularAffineRejected=false;
        try { evaluate(cellEditData,{affine}); }
        catch (const ModifierExecutionError &e) { singularAffineRejected=e.nodeIndex==0; }
        require(singularAffineRejected,"singular affine matrices report an error at their pipeline node");
        std::atomic<float> rangeProgress{0};
        auto progressedRange=colorRangeAcrossFrames(rangeFrames.size(),
            [&](size_t i){return rangeFrames.at(i);}, {},"Q",nullptr,&rangeProgress);
        std::atomic<bool> cancelColorRange{true};
        bool colorRangeCancelled=false;
        try {
            colorRangeAcrossFrames(rangeFrames.size(),[&](size_t i){return rangeFrames.at(i);}, {},"Q",
                                   &cancelColorRange);
        } catch (const std::runtime_error &e) { colorRangeCancelled=std::string(e.what())=="Cancelled"; }
        require(progressedRange.first==-5 && rangeProgress==1 && colorRangeCancelled,
                "all-frame color range reports progress and observes cancellation");
        auto partial=fcc; partial.stride=2;
        bool sampledModifierRejected=false;
        try { evaluate(partial,{{Op::CoordinationAnalysis,true,.8f}}); }
        catch (const ModifierExecutionError &e) { sampledModifierRejected=e.nodeIndex==0; }
        require(sampledModifierRejected,"neighbor analysis modifier rejects sampled data at its node");
        Dataset pair;
        pair.species = {"X"};
        pair.atoms = {{.1f, 0, 0, 0}, {1.9f, 0, 0, 0}, {1, 1, 1, 0}};
        pair.cell = {2, 0, 0, 0, 2, 0, 0, 0, 2};
        pair.pbc = {true, true, true};
        pair.bounds();
        auto pn = neighbors(pair, .3f);
        require(pn.bonds == 1 && pn.clusters == 2 && pn.coordination[2] == 0,
                "minimum image cluster");
        auto twoBin = neighbors(pair, .9f);
        require(twoBin.bonds == 1, "deduplicate periodic bins");
        pair.stride = 2;
        failed = false;
        try {
            neighbors(pair, .3f);
        } catch (...) {
            failed = true;
        }
        require(failed, "sampled analysis rejected");
        auto dxa = dxaApproximate(fcc, .8f);
        require(dxa.analyzed == fcc.atoms.size() && dxa.structures["FCC"] == fcc.atoms.size() && dxa.defectAtoms == 0, "DXA FCC prepass");
        auto structures = classifyByCoordination(fcc, .8f);
        publishStructure(fcc, structures);
        require(structures.counts["FCC"] == fcc.atoms.size() &&
                    fcc.scalarProperties["Structure Type"].size() == fcc.atoms.size(),
                "structure property publication");
        Dataset wave;
        wave.species = {"X"};
        wave.cell = {4,0,0,0,4,0,0,0,4};
        wave.atoms = {{0,0,0,0},{1,0,0,0},{0,1,0,0},{0,0,1,0}};
        wave.sourceCount = wave.atoms.size(); wave.bounds();
        wave.scalarProperties["Coordination"] = {1,2,3,4};
        wave.vectorProperties["Velocity"]={{0,1,2},{3,4,5},{6,7,8},{9,10,11}};
        std::atomic<bool> cancelPropertyCopy{true};
        bool positionCopyCancelled=false, scalarCopyCancelled=false, vectorCopyCancelled=false;
        try { (void)particlePropertyValues(wave,"Position.X",&cancelPropertyCopy); }
        catch (const std::runtime_error &e) { positionCopyCancelled=std::string(e.what())=="Cancelled"; }
        try { (void)particlePropertyValues(wave,"Coordination",&cancelPropertyCopy); }
        catch (const std::runtime_error &e) { scalarCopyCancelled=std::string(e.what())=="Cancelled"; }
        try { (void)particlePropertyValues(wave,"Velocity.Z",&cancelPropertyCopy); }
        catch (const std::runtime_error &e) { vectorCopyCancelled=std::string(e.what())=="Cancelled"; }
        require(positionCopyCancelled&&scalarCopyCancelled&&vectorCopyCancelled,
                "particle-property extraction observes cancellation for position, scalar, and vector inputs");
        std::vector<Modifier> waveMods{{Op::CreateBonds,true,1.01f},
                                       {Op::ColorCoding,true,0,2,0,1,"Coordination"}};
        auto waveResult = evaluate(wave, waveMods);
        require(waveResult.data.bonds.size() == 3 &&
                    waveResult.data.scalarProperties["Color coding"].size() == 4,
                "bond and property color modifiers");
        require(waveResult.data.scalarProperties["Color coding"] ==
                    waveResult.data.scalarProperties["Coordination"],
                "color coding publishes selected property values, not position coordinates");
        auto inspectedPrefix = evaluatePrefix(wave, waveMods, 1);
        require(inspectedPrefix.data.bonds.size() == 3 &&
                    !inspectedPrefix.data.scalarProperties.contains("Color coding"),
                "pipeline prefix evaluation exposes data immediately after its selected node");
        auto finalPrefix = evaluatePrefix(wave, waveMods, waveMods.size());
        require(finalPrefix.data.scalarProperties.at("Color coding") ==
                    waveResult.data.scalarProperties.at("Color coding"),
                "full-length pipeline prefix matches final evaluation");
        bool invalidInspectionPrefixRejected = false;
        try { evaluatePrefix(wave, waveMods, waveMods.size() + 1); }
        catch (const std::runtime_error &) { invalidInspectionPrefixRejected = true; }
        require(invalidInspectionPrefixRejected,
                "pipeline inspection rejects node counts beyond the modifier stack");
        wave.vectorProperties["Velocity"]={{1,4,7},{2,5,8},{3,6,9},{4,7,10}};
        Modifier vectorColor{Op::ColorCoding}; vectorColor.property="Velocity.Y";
        auto vectorColorResult=evaluate(wave,{vectorColor});
        require(vectorColorResult.data.scalarProperties["Color coding"]==
                    std::vector<double>({4,5,6,7}),
                "color coding maps the selected component of a vector particle property");
        Modifier selectedColor{Op::ColorCoding};
        selectedColor.property = "Coordination";
        selectedColor.colorSelectedOnly = true;
        auto selectedColorResult = evaluate(wave, {{Op::SelectIndex,true,0,0,2}, selectedColor});
        require(selectedColorResult.colorSelected == std::vector<uint8_t>({0,0,1,0}) &&
                    std::count(selectedColorResult.selected.begin(), selectedColorResult.selected.end(), uint8_t(1)) == 0,
                "selected-only color snapshots its input selection and clears selection by default");
        selectedColor.colorKeepSelection = true;
        auto keptColorResult = evaluate(wave, {{Op::SelectIndex,true,0,0,2}, selectedColor});
        require(keptColorResult.colorSelected == std::vector<uint8_t>({0,0,1,0}) &&
                    keptColorResult.selected == std::vector<uint8_t>({0,0,1,0}),
                "keep selection preserves selection after selected-only coloring");
        selectedColor.colorKeepSelection=false;
        const auto colorThenReselectThenDelete=evaluate(wave,{
            {Op::SelectIndex,true,0,0,2},selectedColor,{Op::SelectIndex,true,0,0,1},{Op::Delete}});
        require(colorThenReselectThenDelete.data.atoms.size()==3 &&
                    colorThenReselectThenDelete.colorSelected==std::vector<uint8_t>({0,1,0}) &&
                    colorThenReselectThenDelete.data.scalarProperties.at("Color coding").size()==3 &&
                    std::count(colorThenReselectThenDelete.selected.begin(),
                               colorThenReselectThenDelete.selected.end(),uint8_t(1))==0,
                "selected-only color masks stay aligned after downstream selection and deletion");
        Modifier invalidColorRange{Op::ColorCoding};
        invalidColorRange.property="Coordination";
        invalidColorRange.colorAutoRange=false;
        invalidColorRange.colorMin=2;
        invalidColorRange.colorMax=2;
        bool invalidColorRangeRejected=false;
        try { (void)evaluate(wave,{invalidColorRange}); }
        catch (const ModifierExecutionError &e) { invalidColorRangeRejected=e.nodeIndex==0; }
        require(invalidColorRangeRejected,"color coding rejects a non-increasing manual range at its node");
        Modifier unsupportedGradient{Op::ColorCoding};
        unsupportedGradient.property="Coordination";
        unsupportedGradient.colorGradient=10;
        bool unsupportedGradientRejected=false;
        try { (void)evaluate(wave,{unsupportedGradient}); }
        catch (const ModifierExecutionError &e) { unsupportedGradientRejected=e.nodeIndex==0; }
        require(unsupportedGradientRejected,"color coding rejects an unknown gradient at its node");
        Dataset invalidColorValues=wave;
        invalidColorValues.scalarProperties["Bad values"]={
            std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::infinity(),
            -std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::quiet_NaN()};
        Modifier noFiniteColorValues{Op::ColorCoding}; noFiniteColorValues.property="Bad values";
        bool noFiniteColorValuesRejected=false;
        try { (void)evaluate(invalidColorValues,{noFiniteColorValues}); }
        catch (const ModifierExecutionError &e) { noFiniteColorValuesRejected=e.nodeIndex==0; }
        require(noFiniteColorValuesRejected,"color coding rejects properties without any finite values");
        invalidColorValues.scalarProperties["Bad values"]={1,2,3,1e100};
        bool unrepresentableColorValuesRejected=false;
        try { (void)evaluate(invalidColorValues,{noFiniteColorValues}); }
        catch (const ModifierExecutionError &e) { unrepresentableColorValuesRejected=e.nodeIndex==0; }
        require(unrepresentableColorValuesRejected,
                "color coding rejects finite doubles that cannot be represented by the GPU float buffer");
        Dataset overlapFixture;
        overlapFixture.species = {"X"};
        overlapFixture.atoms = {{0,0,0,0},{.5f,0,0,0},{4,0,0,0},{4.5f,0,0,0},{9,0,0,0}};
        overlapFixture.scalarProperties["Radius"] = {.3,.3,.1,.1,0};
        overlapFixture.bounds();
        Modifier overlapByRadius{Op::SelectOverlapping};
        overlapByRadius.overlapUseRadii = true;
        overlapByRadius.property = "Radius";
        const auto overlaps = evaluate(overlapFixture, {overlapByRadius});
        require(overlaps.selected == std::vector<uint8_t>({1,1,0,0,0}),
                "radius-based overlap selection uses each pair's radius sum rather than a global cutoff");
        Dataset periodicOverlap;
        periodicOverlap.species = {"X"};
        periodicOverlap.cell = {2,0,0,1,2,0,0,0,2};
        periodicOverlap.pbc = {true,true,true};
        periodicOverlap.atoms = {{.1f,.1f,.1f,0},{1.95f,.1f,.1f,0}};
        periodicOverlap.scalarProperties["Radius"] = {.1,.1};
        periodicOverlap.bounds();
        require(evaluate(periodicOverlap, {overlapByRadius}).selected ==
                    std::vector<uint8_t>({1,1}),
                "radius-based overlap selection detects contact across a triclinic periodic boundary");
        overlapByRadius.property = "Missing radius";
        bool missingOverlapRadiiRejected = false;
        try { (void)evaluate(overlapFixture, {overlapByRadius}); }
        catch (const ModifierExecutionError &e) { missingOverlapRadiiRejected = e.nodeIndex == 0; }
        require(missingOverlapRadiiRejected,
                "radius-based overlap selection reports a missing radius property at its pipeline node");
        overlapByRadius.property = "Radius";
        overlapFixture.scalarProperties["Radius"][2] = -1;
        bool invalidOverlapRadiusRejected = false;
        try { (void)evaluate(overlapFixture, {overlapByRadius}); }
        catch (const ModifierExecutionError &e) { invalidOverlapRadiusRejected = e.nodeIndex == 0; }
        require(invalidOverlapRadiusRejected, "negative particle radii are rejected at the overlap node");
        Modifier assignRed{Op::AssignColor}; assignRed.assignColor={1,0,0};
        auto assignedSelection=evaluate(wave,{{Op::SelectIndex,true,0,0,1},assignRed});
        require(assignedSelection.data.particleColors.size()==wave.atoms.size() &&
                    assignedSelection.data.particleColors[0].x<0 &&
                    assignedSelection.data.particleColors[1].x==1 &&
                    assignedSelection.data.particleColors[2].x<0,
                "assign color applies to the current selection and leaves other particles at type color");
        auto assignedAll=evaluate(wave,{assignRed});
        require(std::all_of(assignedAll.data.particleColors.begin(),assignedAll.data.particleColors.end(),
                    [](Vec3 color){return color.x==1&&color.y==0&&color.z==0;}),
                "assign color with an empty selection colors all particles");
        auto assignedDeleted=evaluate(wave,{{Op::SelectIndex,true,0,0,2},assignRed,
                                            {Op::SelectIndex,true,0,0,1},{Op::Delete}});
        require(assignedDeleted.data.particleColors.size()==3&&assignedDeleted.data.particleColors[1].x==1,
                "assigned colors stay aligned when selected particles are deleted");
        auto assignedReplicated=evaluate(wave,{assignRed,{Op::Replicate,true,0,0,2}});
        require(assignedReplicated.data.particleColors.size()==8&&assignedReplicated.data.particleColors[7].z==0,
                "assigned colors are duplicated with replicated particles");
        auto typeColors=evaluate(wave,{assignRed,{Op::ColorType}});
        require(typeColors.data.particleColors.empty()&&!typeColors.data.scalarProperties.contains("Color coding"),
                "color-by-type modifier replaces prior per-particle and scalar color operations");
        auto fixedCna = evaluate(fcc, {{Op::CommonNeighborAnalysis,true,.8f}});
        require(fixedCna.data.scalarProperties["Structure Type"].size() == fcc.atoms.size() &&
                    std::all_of(fixedCna.data.scalarProperties["Structure Type"].begin(),
                                fixedCna.data.scalarProperties["Structure Type"].end(),
                                [](double code) { return code == 1; }),
                "fixed-cutoff common-neighbor analysis identifies periodic FCC from pair topology");
        require(fixedCna.data.globalAttributes.at("CommonNeighborAnalysis.counts.FCC") == fcc.atoms.size() &&
                    fixedCna.data.globalAttributes.at("CommonNeighborAnalysis.counts.Other") == 0 &&
                    fixedCna.data.tables.size() == 1 && fixedCna.data.tables[0].rows.size() == 5,
                "CNA publishes OVITO-compatible structure counts");
        const auto fccSignatures = analyzeCommonNeighbors(fcc, .8f);
        require(fccSignatures.bondSignatureCounts.at({4,2,1}) == 12 * fcc.atoms.size() &&
                    fccSignatures.bondSignatureCounts.size() == 1,
                "fixed-cutoff FCC reference has twelve 1421 signatures per atom");
        Dataset topology;
        topology.species = {"X"};
        topology.atoms = {{.1f,0,0,0},{1.9f,0,0,0},{1,1,1,0}};
        topology.cell = {2,0,0,0,2,0,0,0,2}; topology.pbc = {true,true,true};
        topology.bounds();
        topology.bonds = {{0,1,{0,0,0}}};
        auto withCna = evaluate(topology, {{Op::CommonNeighborAnalysis,true,.3f}});
        require(withCna.data.bonds == topology.bonds,
                "CNA leaves pre-existing bond topology unchanged");
        auto preservedBonds = evaluate(topology, {{Op::CreateBonds,true,.3f}});
        require(preservedBonds.data.bonds.size() == 2 &&
                    preservedBonds.data.bonds[0] == topology.bonds[0],
                "create bonds preserves existing topology by default");
        const auto combinedTopology = evaluate(topology,
            {{Op::CreateBonds,true,.3f}, {Op::CommonNeighborAnalysis,true,.3f},
             {Op::SelectIndex,true,0,0,2}, {Op::Delete}});
        require(combinedTopology.data.atoms.size() == 2 &&
                    combinedTopology.data.bonds.size() == 2 &&
                    std::all_of(combinedTopology.data.bonds.begin(), combinedTopology.data.bonds.end(),
                                [&](const Bond &bond) { return bond.a < 2 && bond.b < 2; }) &&
                    combinedTopology.data.scalarProperties.at("Structure Type").size() == 2 &&
                    combinedTopology.selected.size() == 2,
                "create-bonds/CNA/select/delete pipeline preserves properties and remaps periodic topology");
        Dataset pairCutoffData;
        pairCutoffData.species={"A","B"};
        pairCutoffData.atoms={{0,0,0,0},{.8f,0,0,1},{1.6f,0,0,1}};
        pairCutoffData.bounds();
        Modifier pairCutoffBonds{Op::CreateBonds};
        pairCutoffBonds.bondTypeCutoffsEnabled=true;
        pairCutoffBonds.bondTypeCutoffs={.2f,.9f,.9f,.2f};
        auto pairCutoffResult=evaluate(pairCutoffData,{pairCutoffBonds});
        require(pairCutoffResult.data.bonds==std::vector<Bond>{{0,1,{0,0,0}}},
                "type-pair bond cutoffs use the symmetric type-pair threshold");
        pairCutoffBonds.bondCylinders=true;
        pairCutoffBonds.bondRadius=.14f;
        auto cylinderBondResult=evaluate(pairCutoffData,{pairCutoffBonds});
        require(cylinderBondResult.data.bondStyle.radius==.14f,
                "Create bonds publishes the configured world-space cylinder radius");
        pairCutoffBonds.bondRadius=0;
        bool invalidCylinderRadiusRejected=false;
        try { (void)evaluate(pairCutoffData,{pairCutoffBonds}); }
        catch (const ModifierExecutionError &e) { invalidCylinderRadiusRejected=e.nodeIndex==0; }
        require(invalidCylinderRadiusRejected,"invalid cylinder radius is reported at the Create bonds node");
        pairCutoffBonds.bondCylinders=false;
        pairCutoffBonds.bondRadius=.08f;
        require(evaluate(pairCutoffData,{pairCutoffBonds}).data.bondStyle.radius==0,
                "screen-space line representation remains the default");
        pairCutoffBonds.bondTypeCutoffs={0,0,0,0};
        require(evaluate(pairCutoffData,{pairCutoffBonds}).data.bonds.empty(),
                "zero type-pair cutoffs disable bond generation without rejecting the node");
        auto duplicateBondData=pairCutoffData;
        duplicateBondData.bonds={{0,1,{0,0,0}},{1,0,{0,0,0}}};
        require(evaluate(duplicateBondData,{pairCutoffBonds}).data.bonds==
                    std::vector<Bond>{{0,1,{0,0,0}}},
                "Create bonds deduplicates reverse-oriented existing topology without changing its endpoints");
        auto periodicSelfBondData=pairCutoffData;
        periodicSelfBondData.cell={2,0,0,0,2,0,0,0,2};
        periodicSelfBondData.pbc={true,false,false};
        periodicSelfBondData.bonds={{0,0,{1,0,0}},{0,0,{-1,0,0}}};
        require(evaluate(periodicSelfBondData,{pairCutoffBonds}).data.bonds==
                    std::vector<Bond>{{0,0,{1,0,0}}},
                "Create bonds keeps valid periodic self-image bonds and deduplicates their reverse orientation");
        auto invalidExistingBond=duplicateBondData;
        invalidExistingBond.bonds={{0,3,{0,0,0}}};
        bool invalidBondEndpointRejected=false;
        try { (void)evaluate(invalidExistingBond,{pairCutoffBonds}); }
        catch (const ModifierExecutionError &e) { invalidBondEndpointRejected=e.nodeIndex==0; }
        require(invalidBondEndpointRejected,"Create bonds reports malformed existing endpoints at its node");
        invalidExistingBond=duplicateBondData;
        invalidExistingBond.bonds={{0,0,{0,0,0}}};
        bool zeroSelfBondRejected=false;
        try { (void)evaluate(invalidExistingBond,{pairCutoffBonds}); }
        catch (const ModifierExecutionError &e) { zeroSelfBondRejected=e.nodeIndex==0; }
        require(zeroSelfBondRejected,"Create bonds rejects zero-displacement self bonds instead of silently dropping them");
        invalidExistingBond=duplicateBondData;
        invalidExistingBond.bonds={{0,1,{1,0,0}}};
        bool nonPeriodicImageRejected=false;
        try { (void)evaluate(invalidExistingBond,{pairCutoffBonds}); }
        catch (const ModifierExecutionError &e) { nonPeriodicImageRejected=e.nodeIndex==0; }
        require(nonPeriodicImageRejected,"Create bonds rejects image shifts on non-periodic axes");
        auto invalidPeriodicBond=duplicateBondData;
        invalidPeriodicBond.cell={0,0,0,0,0,0,0,0,0};
        invalidPeriodicBond.pbc={true,false,false};
        invalidPeriodicBond.bonds={{0,1,{1,0,0}}};
        bool invalidPeriodicVectorRejected=false;
        try { (void)evaluate(invalidPeriodicBond,{pairCutoffBonds}); }
        catch (const ModifierExecutionError &e) { invalidPeriodicVectorRejected=e.nodeIndex==0; }
        require(invalidPeriodicVectorRejected,"Create bonds rejects periodic images with a degenerate cell vector");
        auto invalidBondColor=pairCutoffBonds;
        invalidBondColor.bondColor[1]=std::numeric_limits<float>::quiet_NaN();
        bool invalidBondColorRejected=false;
        try { (void)evaluate(pairCutoffData,{invalidBondColor}); }
        catch (const ModifierExecutionError &e) { invalidBondColorRejected=e.nodeIndex==0; }
        require(invalidBondColorRejected,"Create bonds validates color channels in the core pipeline");
        pairCutoffBonds.bondTypeCutoffs={.2f,.9f,.8f,.2f};
        bool asymmetricPairCutoffsRejected=false;
        try { (void)evaluate(pairCutoffData,{pairCutoffBonds}); }
        catch (const ModifierExecutionError &e) { asymmetricPairCutoffsRejected=e.nodeIndex==0; }
        require(asymmetricPairCutoffsRejected,"asymmetric type-pair cutoff tables are rejected at their node");
        auto periodicSource = topology;
        periodicSource.bonds.clear();
        auto periodicBonds = evaluate(periodicSource, {{Op::CreateBonds,true,.3f}});
        require(periodicBonds.data.bonds.size() == 1 &&
                    periodicBonds.data.bonds[0].a == 0 && periodicBonds.data.bonds[0].b == 1 &&
                    periodicBonds.data.bonds[0].image == std::array<int32_t,3>{1,0,0},
                "created bonds preserve periodic image shift");
        auto periodicReplicate = evaluate(periodicBonds.data,
                                          {{Op::Replicate,true,0,0,2}});
        require(periodicReplicate.data.bonds.size() == 2 &&
                    periodicReplicate.data.bonds[0].a == 0 &&
                    periodicReplicate.data.bonds[0].b == 4 &&
                    periodicReplicate.data.bonds[0].image[0] == 0 &&
                    periodicReplicate.data.bonds[1].a == 3 &&
                    periodicReplicate.data.bonds[1].b == 1 &&
                    periodicReplicate.data.bonds[1].image[0] == 1,
                "replication remaps periodic bonds and retains supercell boundary shift");
        Dataset deleteTopology;
        deleteTopology.species = {"X"};
        deleteTopology.atoms = {{0,0,0,0},{1,0,0,0},{2,0,0,0}};
        deleteTopology.bounds();
        deleteTopology.bonds = {{0,2,{0,0,0}},{1,2,{0,0,0}}};
        auto deletedTopology = evaluate(deleteTopology,
                                        {{Op::SelectIndex,true,0,0,1}, {Op::Delete}});
        require(deletedTopology.data.atoms.size() == 2 &&
                    deletedTopology.data.bonds.size() == 1 &&
                    deletedTopology.data.bonds[0].a == 0 && deletedTopology.data.bonds[0].b == 1,
                "delete remaps retained bond endpoints and removes incident bonds");
        auto slicedTopology = evaluate(deleteTopology,
                                       {{Op::Slice,true,.5f,0}, {Op::Delete}});
        require(slicedTopology.data.atoms.size() == 1 && slicedTopology.data.bonds.empty(),
                "slice plus delete leaves no dangling bond endpoints");
        Dataset bcc;
        bcc.species = {"X"}; bcc.cell = {4,0,0,0,4,0,0,0,4}; bcc.pbc = {true,true,true};
        for (int z=0;z<4;++z) for (int y=0;y<4;++y) for (int x=0;x<4;++x) {
            bcc.atoms.push_back({float(x),float(y),float(z),0});
            bcc.atoms.push_back({x+.5f,y+.5f,z+.5f,0});
        }
        bcc.bounds();
        auto bccCna = evaluate(bcc, {{Op::CommonNeighborAnalysis,true,1.01f}});
        size_t bccCount = std::count(bccCna.data.scalarProperties["Structure Type"].begin(),
                                     bccCna.data.scalarProperties["Structure Type"].end(),3.0);
        require(bccCount == bcc.atoms.size(), "fixed-cutoff common-neighbor analysis identifies periodic BCC");
        const auto bccSignatures = analyzeCommonNeighbors(bcc, 1.01);
        require(bccSignatures.bondSignatureCounts.at({4,4,3}) == 6 * bcc.atoms.size() &&
                    bccSignatures.bondSignatureCounts.at({6,6,5}) == 8 * bcc.atoms.size() &&
                    bccSignatures.bondSignatureCounts.size() == 2,
                "fixed-cutoff BCC reference has six 443 and eight 665 signatures per atom");
        Dataset hcp;
        const double root3 = std::sqrt(3.0), cOverA = std::sqrt(8.0/3.0);
        hcp.species = {"X"};
        hcp.cell = {4,0,0, 2,2*root3,0, 0,0,4*cOverA};
        hcp.pbc = {true,true,true};
        for (int k=0;k<4;++k) for (int j=0;j<4;++j) for (int i=0;i<4;++i)
            for (int basis=0;basis<2;++basis) {
                const double u=i+(basis ? 1.0/3 : 0), v=j+(basis ? 1.0/3 : 0), w=k+(basis ? .5 : 0);
                hcp.atoms.push_back({float(u+.5*v),float(.5*root3*v),float(cOverA*w),0});
            }
        hcp.bounds();
        auto hcpCna = evaluate(hcp, {{Op::CommonNeighborAnalysis,true,1.1f}});
        require(std::all_of(hcpCna.data.scalarProperties["Structure Type"].begin(),
                            hcpCna.data.scalarProperties["Structure Type"].end(),
                            [](double code) { return code == 2; }),
                "fixed-cutoff CNA identifies ideal HCP in a triclinic periodic cell");
        const auto hcpSignatures = analyzeCommonNeighbors(hcp, 1.1);
        require(hcpSignatures.bondSignatureCounts.at({4,2,1}) == 6 * hcp.atoms.size() &&
                    hcpSignatures.bondSignatureCounts.at({4,2,2}) == 6 * hcp.atoms.size() &&
                    hcpSignatures.bondSignatureCounts.size() == 2,
                "fixed-cutoff HCP reference has six 1421 and six 1422 signatures per atom");
        Dataset icosa;
        icosa.species = {"X"};
        const double phi=(1+std::sqrt(5.0))/2, norm=std::sqrt(1+phi*phi);
        icosa.atoms.push_back({0,0,0,0});
        for (int sign : {-1,1}) for (int t : {-1,1}) {
            icosa.atoms.push_back({0,float(sign/norm),float(t*phi/norm),0});
            icosa.atoms.push_back({float(sign/norm),float(t*phi/norm),0,0});
            icosa.atoms.push_back({float(t*phi/norm),0,float(sign/norm),0});
        }
        icosa.bounds();
        auto icoCna=evaluate(icosa,{{Op::CommonNeighborAnalysis,true,1.1f}});
        require(icoCna.data.scalarProperties["Structure Type"][0]==4,
                "fixed-cutoff CNA identifies an isolated icosahedral center");
        const auto icoSignatures = analyzeCommonNeighbors(icosa, 1.1);
        require(icoSignatures.bondSignatureCounts.at({5,5,4}) >= 12,
                "icosahedral center reference has twelve 554 signatures");
        Dataset disordered;
        disordered.species = {"X"};
        uint32_t randomState = 0x5eed1234u;
        auto randomCoordinate = [&]() {
            randomState = randomState * 1664525u + 1013904223u;
            return float((randomState >> 8) * (1.0 / 16777216.0) * 10.0);
        };
        for (int i = 0; i < 128; ++i)
            disordered.atoms.push_back({randomCoordinate(), randomCoordinate(), randomCoordinate(), 0});
        const auto disorderedCna = analyzeCommonNeighbors(disordered, 1.7);
        require(std::all_of(disorderedCna.structure.begin(), disorderedCna.structure.end(),
                            [](uint8_t kind) { return kind == 0; }) &&
                    disorderedCna.counts.at("Other") == disordered.atoms.size(),
                "deterministic disordered reference remains unclassified by ideal CNA signatures");
        Dataset tilted;
        tilted.species={"X"}; tilted.cell={2,0,0, 1,2,0, 0,0,2};
        tilted.pbc={true,true,true}; tilted.atoms={{.15f,.1f,.1f,0},{1.95f,.1f,.1f,0}};
        tilted.bounds();
        auto tiltedBond=evaluate(tilted,{{Op::CreateBonds,true,.3f}});
        require(tiltedBond.data.bonds.size()==1 && tiltedBond.data.bonds[0].image==std::array<int32_t,3>{1,0,0},
                "triclinic periodic neighbor search keeps the correct image shift");
        Dataset slab;
        slab.species={"X"}; slab.cell={2,0,0, 1,2,0, 0,0,0}; slab.pbc={true,true,false};
        slab.atoms={{.15f,.1f,0,0},{1.95f,.1f,0,0},{.15f,.1f,1,0}}; slab.bounds();
        auto slabBonds=evaluate(slab,{{Op::CreateBonds,true,.3f}});
        require(slabBonds.data.bonds.size()==1 && slabBonds.data.bonds[0].a==0 &&
                    slabBonds.data.bonds[0].b==1 && slabBonds.data.bonds[0].image==std::array<int32_t,3>{1,0,0},
                "partially periodic triclinic slab works with a degenerate non-periodic cell vector");
        auto slabExpanded=evaluate(slab,{{Op::SelectIndex,true,0,0,0},
                                         {Op::ExpandSelection,true,.3f,2,1}});
        require(slabExpanded.selected[0] && slabExpanded.selected[1] && !slabExpanded.selected[2],
                "selection expansion uses the same triclinic partial-periodic minimum-image neighbors");
        Dataset chain;
        chain.species = {"X"};
        chain.atoms = {{0,0,0,0},{0.8f,0,0,0},{1.6f,0,0,0},{5,0,0,0}};
        chain.bounds();
        Dataset pipelineWorkingSet=chain;
        auto movedEvaluation=evaluate(std::move(pipelineWorkingSet),{{Op::Translate,true,2,0}});
        require(movedEvaluation.data.atoms[0].x==2 && movedEvaluation.data.atoms[3].x==7 &&
                    chain.atoms[0].x==0,
                "owned pipeline working set moves into its result while preserving the source dataset");
        std::atomic<size_t> activePipelineNode{0};
        auto stagedEvaluation=evaluate(chain,{{Op::Translate,true,1,0},{Op::Scale,true,2}},
                                       nullptr,&activePipelineNode);
        require(activePipelineNode==2 && stagedEvaluation.data.atoms[0].x==2 &&
                    stagedEvaluation.data.atoms[1].x==3.6f,
                "pipeline reports the active node while preserving modifier order");
        auto expanded = evaluate(chain, {{Op::SelectIndex,true,0,0,0},
                                         {Op::ExpandSelection,true,0.9f,2,2}});
        require(expanded.selected[0] && expanded.selected[1] && expanded.selected[2] &&
                    !expanded.selected[3], "selection expands through multiple neighbor shells");
        auto overlapping = evaluate(chain, {{Op::SelectOverlapping,true,0.9f}});
        require(overlapping.selected[0] && overlapping.selected[1] && overlapping.selected[2] &&
                    !overlapping.selected[3], "overlap selection includes every atom in close pairs");
        Modifier manualSelection{Op::ManualSelection};
        manualSelection.manualSelection={1,3};
        auto manuallySelected=evaluate(chain,{manualSelection});
        require(manuallySelected.selected==std::vector<uint8_t>({0,1,0,1}),
                "manual selection publishes a stable multi-particle selection");
        auto manuallyDeleted=evaluate(chain,{manualSelection,Modifier{Op::Delete}});
        require(manuallyDeleted.data.atoms.size()==2 && manuallyDeleted.selected.size()==2 &&
                    manuallyDeleted.data.atoms[0].x==0 && manuallyDeleted.data.atoms[1].x==1.6f,
                "manual selection composes with delete-selected in pipeline order");
        manualSelection.manualSelection={4};
        bool invalidManualIndexRejected=false;
        try { evaluate(chain,{manualSelection}); }
        catch (const ModifierExecutionError &e) { invalidManualIndexRejected=e.nodeIndex==0; }
        require(invalidManualIndexRejected,"manual selection reports stale particle indices clearly");
        chain.scalarProperties["Energy"] = {-2, 0, 3, 8};
        Modifier expression{Op::ExpressionSelect};
        expression.property = "Energy >= 0 && (x < 2 || abs(Energy) > 7)";
        auto expressionResult = evaluate(chain, {expression});
        require(!expressionResult.selected[0] && expressionResult.selected[1] &&
                    expressionResult.selected[2] && expressionResult.selected[3],
                "safe expression selection reads scalar properties and boolean expressions");
        chain.scalarProperties["Potential Energy"] = {-1, 0, 4, 9};
        expression.property = "`Potential Energy` >= 4";
        auto spacedPropertySelection = evaluate(chain, {expression});
        require(!spacedPropertySelection.selected[0] && !spacedPropertySelection.selected[1] &&
                    spacedPropertySelection.selected[2] && spacedPropertySelection.selected[3],
                "expression selection resolves scalar properties with spaces using backtick quoting");
        Modifier compute{Op::ComputeProperty};
        compute.property = "x*x + Energy";
        compute.outputProperty = "Derived";
        auto computed = evaluate(chain, {compute});
        require(computed.data.scalarProperties["Derived"].size() == chain.atoms.size() &&
                    std::abs(computed.data.scalarProperties["Derived"][2] - 5.56) < 1e-5,
                "compute property publishes expression values for all particles");
        expression.property = "Derived > 5";
        auto derivedSelection = evaluate(computed.data, {expression});
        require(derivedSelection.selected[2] && derivedSelection.selected[3] &&
                    !derivedSelection.selected[0], "computed properties feed later pipeline expressions");
        expression.property = "sqrt(Energy) > 0";
        bool expressionDomainRejected = false;
        try { evaluate(chain, {expression}); } catch (const std::runtime_error &) { expressionDomainRejected = true; }
        require(expressionDomainRejected, "expression domain errors are reported");
        bool correctlyAttributed = false;
        try { evaluate(chain, {{Op::Translate,true,1,0}, {Op::Scale,true,-1}}); }
        catch (const ModifierExecutionError &e) { correctlyAttributed = e.nodeIndex == 1; }
        require(correctlyAttributed, "pipeline failures report the modifier that raised them");
        std::atomic<bool> stopNow{true};
        bool cancellationReported = false;
        try { evaluate(chain, {{Op::ExpandSelection,true,.9f,2,1}}, &stopNow); }
        catch (const ModifierExecutionError &e) { cancellationReported = e.nodeIndex == 0 && std::string(e.what()) == "Cancelled"; }
        require(cancellationReported, "long-running pipeline operations honor cancellation");
        expression.property = "system(1)";
        bool unsafeExpressionRejected = false;
        try { evaluate(chain, {expression}); } catch (const std::runtime_error &) { unsafeExpressionRejected = true; }
        require(unsafeExpressionRejected, "expression rejects unapproved function calls");
        Dataset emptyExpressionData;
        Modifier invalidEmptyExpression{Op::ExpressionSelect};
        invalidEmptyExpression.property = "MissingProperty > 0";
        bool invalidPropertyOnEmptyDataRejected = false;
        try { evaluate(emptyExpressionData, {invalidEmptyExpression}); }
        catch (const ModifierExecutionError &e) { invalidPropertyOnEmptyDataRejected = e.nodeIndex == 0; }
        require(invalidPropertyOnEmptyDataRejected,
                "expression validation reports unknown properties on empty datasets");
        invalidEmptyExpression.property = "x + (";
        bool syntaxErrorOnEmptyDataRejected = false;
        try { evaluate(emptyExpressionData, {invalidEmptyExpression}); }
        catch (const ModifierExecutionError &e) { syntaxErrorOnEmptyDataRejected = e.nodeIndex == 0; }
        require(syntaxErrorOnEmptyDataRejected,
                "expression validation reports syntax errors on empty datasets");
        chain.stride = 2;
        bool sampledSelectionRejected = false;
        try { evaluate(chain, {{Op::ExpandSelection,true,0.9f,2,1}}); }
        catch (const std::runtime_error &) { sampledSelectionRejected = true; }
        require(sampledSelectionRejected, "neighbor selection refuses sampled previews");
        PipelineGraph graph;
        ModifierNode selectType{Op::SelectType, true, 0, 2, 1};
        selectType.id = "select-type";
        selectType.displayName = "Select type";
        graph.insert(std::move(selectType));
        ModifierNode deleteSelected{Op::Delete};
        deleteSelected.id = "delete-selected";
        graph.insert(std::move(deleteSelected));
        graph.nodes[1].dirty = false;
        graph.move(1, 0);
        require(graph.nodes[0].id == "delete-selected" && graph.nodes[1].dirty,
                "pipeline ordering and dependent dirty state");
        graph.move(0, 1);
        auto graphResult = evaluate(d, graph);
        require(graphResult.data.atoms.size() == 2 && graphResult.selected.size() == 2,
                "pipeline graph executes its owned modifier nodes in order");
        graph.erase(0);
        require(graph.nodes.size() == 1 && graph.nodes[0].id == "delete-selected",
                "pipeline erase");
        auto poscar = p.parent_path() / "atomx-test.POSCAR";
        writePOSCAR(poscar, fcc);
        auto pos = readPOSCAR(poscar);
        require(pos.atoms.size() == fcc.atoms.size() && pos.species.size() == fcc.species.size(), "POSCAR roundtrip");
        auto cif = p.parent_path() / "atomx-test.cif";
        writeCIF(cif, fcc);
        auto cifData = readCIF(cif);
        require(cifData.atoms.size() == fcc.atoms.size(), "CIF roundtrip");
        auto lmp = p.parent_path() / "atomx-test.data";
        writeLammpsData(lmp, fcc);
        auto lmpData = readLammpsData(lmp);
        require(lmpData.atoms.size() == fcc.atoms.size(), "LAMMPS data roundtrip");
        std::filesystem::remove(p); std::filesystem::remove(poscar); std::filesystem::remove(cif); std::filesystem::remove(lmp);
        std::cout << "PASS: index, seek, schema, metadata, sampling, selection, slice, wrap, "
                     "scale, scientific modifier tables, assign color, roundtrip, malformed input, "
                     "FCC/HCP/BCC/ICO CNA signatures and disordered reference, periodic topology, "
                     "disconnected cluster labels, radius-aware periodic overlap selection, "
                     "bonds/CNA/select/delete pipeline composition, "
                     "sampled analysis rejection\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
