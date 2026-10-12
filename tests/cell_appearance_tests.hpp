#pragma once
#include "../src/creation_document.hpp"
#include <sstream>

// Included by the application workflow harness, after requireExport.
inline void testCellAppearanceDocuments() {
    atomx::Dataset data;data.species={"Na"};data.atoms={{0,0,0,0}};
    data.cell={5,0,0,0,5,0,0,0,5};data.bounds();
    atomx::document::View view;view.cellColor={.2f,.5f,.8f,.7f};
    view.cellWidth=3.25f;view.cellGlow=.3f;view.cellDashed=true;view.cellLabels=false;view.cellDimension=0;
    std::stringstream stream(std::ios::in|std::ios::out|std::ios::binary);
    atomx::document::write(stream,data,view);
    const auto bytes=stream.str();
    auto result=atomx::document::read(stream,bytes.size());
    requireExport(result.view.cellColor==view.cellColor && result.view.cellWidth==view.cellWidth &&
        result.view.cellGlow==view.cellGlow && result.view.cellDashed && !result.view.cellLabels &&
        result.view.cellDimension==0,"cell appearance survives native document round-trip");
    // v15 appends exactly 30 bytes before DONE; a v14 document has no appearance block.
    auto legacy=bytes;legacy[8]=14;legacy.erase(legacy.size()-34,30);
    std::stringstream old(legacy,std::ios::in|std::ios::binary);
    const auto restored=atomx::document::read(old,legacy.size());
    const atomx::document::View defaults;
    requireExport(restored.view.cellColor==defaults.cellColor && restored.view.cellWidth==defaults.cellWidth &&
        restored.view.cellLabels,"v14 documents use readable default cell appearance");
    view.cellWidth=std::numeric_limits<float>::quiet_NaN();
    bool rejected=false;try{atomx::document::write(stream,data,view);}catch(const std::exception &){rejected=true;}
    requireExport(rejected,"non-finite cell appearance cannot be written to a document");
}
