// Literature-based per-element display defaults for AtomX particle types.
//   Colors:        Jmol/CPK color scheme (the colors Jmol uses for CPK coloring).
//   Covalent radii: Cordero et al., "Covalent radii revisited", Dalton Trans.
//                  2008, 2832-2846 (single-bond radii, angstrom).
//   Van der Waals radii: Bondi, J. Phys. Chem. 1964, 68, 441-451 (angstrom);
//                  0 marks elements absent from Bondi's 1964 tables.
// Element identity lookup strips trailing digits and charge characters from a
// particle type name and requires the remainder to equal one full element
// symbol case-insensitively: "Ni2+" matches nickel, "O_wat" and "Xx" do not.
#pragma once
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace atomx::elements {
struct Element {
    const char *symbol;
    int z;
    const char *name;
    unsigned rgb;    // 0xRRGGBB, Jmol/CPK scheme
    float covalent;  // Cordero 2008 single-bond covalent radius, angstrom
    float vdw;       // Bondi 1964 van der Waals radius, angstrom (0 = undefined)
};
// Z = 1..56 plus the common heavies W, Pt, Au, Hg, Pb, Bi, U, Pu (64 entries).
inline constexpr Element table[] = {
    {"H", 1, "Hydrogen", 0xFFFFFF, 0.31f, 1.20f},   {"He", 2, "Helium", 0xD9FFFF, 0.28f, 1.40f},
    {"Li", 3, "Lithium", 0xCC80FF, 1.28f, 1.82f},   {"Be", 4, "Beryllium", 0xC2FF00, 0.96f, 0},
    {"B", 5, "Boron", 0xFFB5B5, 0.84f, 0},          {"C", 6, "Carbon", 0x909090, 0.76f, 1.70f},
    {"N", 7, "Nitrogen", 0x3050F8, 0.71f, 1.55f},   {"O", 8, "Oxygen", 0xFF0D0D, 0.66f, 1.52f},
    {"F", 9, "Fluorine", 0x90E050, 0.57f, 1.47f},   {"Ne", 10, "Neon", 0xB3E3F5, 0.58f, 1.54f},
    {"Na", 11, "Sodium", 0xAB5CF2, 1.66f, 2.27f},   {"Mg", 12, "Magnesium", 0x8AFF00, 1.41f, 1.73f},
    {"Al", 13, "Aluminium", 0xBFA6A6, 1.21f, 0},    {"Si", 14, "Silicon", 0xF0C8A0, 1.11f, 2.10f},
    {"P", 15, "Phosphorus", 0xFF8000, 1.07f, 1.80f}, {"S", 16, "Sulfur", 0xFFFF30, 1.05f, 1.80f},
    {"Cl", 17, "Chlorine", 0x1FF01F, 1.02f, 1.75f}, {"Ar", 18, "Argon", 0x80D1E3, 1.06f, 1.88f},
    {"K", 19, "Potassium", 0x8F40D4, 2.03f, 2.75f}, {"Ca", 20, "Calcium", 0x3DFF00, 1.76f, 0},
    {"Sc", 21, "Scandium", 0xE6E6E6, 1.70f, 0},     {"Ti", 22, "Titanium", 0xBFC2C7, 1.60f, 0},
    {"V", 23, "Vanadium", 0xA6A6AB, 1.53f, 0},      {"Cr", 24, "Chromium", 0x8A99C7, 1.39f, 0},
    {"Mn", 25, "Manganese", 0x9C7AC7, 1.39f, 0},    {"Fe", 26, "Iron", 0xE06633, 1.32f, 2.04f},
    {"Co", 27, "Cobalt", 0xF090A0, 1.26f, 0},       {"Ni", 28, "Nickel", 0x50D050, 1.24f, 1.63f},
    {"Cu", 29, "Copper", 0xC88033, 1.32f, 1.96f},   {"Zn", 30, "Zinc", 0x7D80B0, 1.22f, 1.39f},
    {"Ga", 31, "Gallium", 0xC28F8F, 1.22f, 0},      {"Ge", 32, "Germanium", 0x668F8F, 1.20f, 0},
    {"As", 33, "Arsenic", 0xBD80E3, 1.19f, 1.85f},  {"Se", 34, "Selenium", 0xFFA100, 1.20f, 1.90f},
    {"Br", 35, "Bromine", 0xA62929, 1.20f, 1.85f},  {"Kr", 36, "Krypton", 0x5CB8D1, 1.16f, 2.02f},
    {"Rb", 37, "Rubidium", 0x702EB0, 2.20f, 0},     {"Sr", 38, "Strontium", 0x00FF00, 1.95f, 0},
    {"Y", 39, "Yttrium", 0x94FFFF, 1.90f, 0},       {"Zr", 40, "Zirconium", 0x94E0E0, 1.75f, 0},
    {"Nb", 41, "Niobium", 0x73C2C9, 1.64f, 0},      {"Mo", 42, "Molybdenum", 0x54B5B5, 1.54f, 0},
    {"Tc", 43, "Technetium", 0x3B9E9E, 1.47f, 0},   {"Ru", 44, "Ruthenium", 0x248F8F, 1.46f, 0},
    {"Rh", 45, "Rhodium", 0x0A7D8C, 1.42f, 0},      {"Pd", 46, "Palladium", 0x006985, 1.39f, 1.63f},
    {"Ag", 47, "Silver", 0xC0C0C0, 1.45f, 1.72f},   {"Cd", 48, "Cadmium", 0xFFD98F, 1.44f, 1.58f},
    {"In", 49, "Indium", 0xA67573, 1.42f, 0},       {"Sn", 50, "Tin", 0x668080, 1.39f, 0},
    {"Sb", 51, "Antimony", 0x9E63B5, 1.39f, 0},     {"Te", 52, "Tellurium", 0xD47A00, 1.38f, 2.06f},
    {"I", 53, "Iodine", 0x940094, 1.39f, 1.98f},    {"Xe", 54, "Xenon", 0x429EB0, 1.40f, 2.16f},
    {"Cs", 55, "Caesium", 0x57178F, 2.44f, 0},      {"Ba", 56, "Barium", 0x00C900, 2.15f, 0},
    {"W", 74, "Tungsten", 0x2194D6, 1.62f, 0},      {"Pt", 78, "Platinum", 0xD0D0E0, 1.36f, 1.75f},
    {"Au", 79, "Gold", 0xFFD123, 1.36f, 1.66f},     {"Hg", 80, "Mercury", 0xB8B8D0, 1.32f, 1.55f},
    {"Pb", 82, "Lead", 0x575961, 1.46f, 0},         {"Bi", 83, "Bismuth", 0x9E4FB5, 1.48f, 0},
    {"U", 92, "Uranium", 0x008FFF, 1.96f, 0},       {"Pu", 94, "Plutonium", 0x006BFF, 1.87f, 0},
};
inline const Element *find(std::string_view type) {
    while (!type.empty() && (type.back() == '+' || type.back() == '-' ||
                             (type.back() >= '0' && type.back() <= '9')))
        type.remove_suffix(1);
    if (type.empty() || type.size() > 2)
        return nullptr;
    for (const auto &e : table) {
        const std::string_view symbol = e.symbol;
        if (symbol.size() != type.size())
            continue;
        bool equal = true;
        for (size_t i = 0; i < symbol.size() && equal; ++i)
            equal = (symbol[i] | 0x20) == (type[i] | 0x20);
        if (equal)
            return &e;
    }
    return nullptr;
}
// Normalized RGBA of the Jmol/CPK color.
inline std::array<float, 4> color(const Element &e) {
    return {float((e.rgb >> 16) & 255) / 255.f, float((e.rgb >> 8) & 255) / 255.f,
            float(e.rgb & 255) / 255.f, 1.f};
}
// Applies the literature defaults to freshly initialized type styles: element
// matches receive the Jmol/CPK color and carry their Cordero covalent radius in
// visual[3] as the per-type default radius; visual[0] stays 0 ("Use default
// radius" stays checked) and non-matching styles stay untouched (palette color,
// global default radius). Works on any style type with `color` and `visual`
// float[4] members, so tests can mirror the renderer's creation path exactly.
template <typename Styles>
void applyTypeDefaults(const std::vector<std::string> &names, Styles &styles) {
    for (size_t i = 0; i < names.size() && i < styles.size(); ++i)
        if (const Element *e = find(names[i])) {
            styles[i].color = color(*e);
            styles[i].visual[3] = e->covalent;
        }
}
} // namespace atomx::elements
