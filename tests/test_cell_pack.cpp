// render/cell_pack: the per-cell material texture a cell shader reads.
//
// The layout is a contract with a shader that cannot be tested from here, so
// it is checked two ways: byte by byte against the layout as cell_pack.h
// writes it out (not through the helpers that produce it), and as a pinned
// checksum over a fixture that exercises every field, so any change to what a
// texel means shows up as a failure someone has to look at.
//
// The fixture is written field by field into a bare Element array rather than
// grown by stepping a Grid, for two reasons: Grid has no setter for mass, flow
// or ticks, and a checksum over a simulated world would move with every physics
// change and so pin nothing about this file. One block below does run a real
// Grid, to check that the Grid overload reads the same cells get_element does.

#include "render/cell_pack.h"

#include <array>
#include <cstring>
#include <string>
#include <vector>
#include "test_util.h"

namespace {

using cell_pack::CellPack;
using cell_pack::PAD_TEXEL;

std::array<uint8_t, 4> bytes_of(uint32_t t) {
    std::array<uint8_t, 4> b{};
    std::memcpy(b.data(), &t, 4);
    return b;
}

std::string hex(uint32_t v) {
    char s[16];
    std::snprintf(s, sizeof s, "%08X", v);
    return s;
}

Element cell(ElementType type, int mass = 0, int flow = 0, int ticks = 0,
             int temperature = AMBIENT_TEMPERATURE) {
    Element e;
    e.type = type;
    e.mass = static_cast<uint16_t>(mass);
    e.flow = static_cast<int8_t>(flow);
    e.ticks = static_cast<uint8_t>(ticks);
    e.temperature = static_cast<uint8_t>(temperature);
    return e;
}

uint8_t green(const Element& e) { return bytes_of(cell_pack::texel_of(e))[1]; }
uint8_t alpha(const Element& e) { return bytes_of(cell_pack::texel_of(e))[3]; }

// A small world with every field varied: a fixed xorshift, so the fixture is
// the same on every machine and compiler. Colour is filled too, with junk, to
// prove it is never read.
constexpr int FW = 13, FH = 9;
std::vector<Element> fixture() {
    std::vector<Element> cells(static_cast<size_t>(FW) * FH);
    uint32_t s = 0x2545F491u;
    const auto next = [&s] {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    };
    for (Element& e : cells) {
        e.type = static_cast<ElementType>(next() % static_cast<uint32_t>(ElementType::Count));
        if (carries_mass(e.type)) e.mass = static_cast<uint16_t>(next() % 9000);
        if (is_liquid(e.type)) e.flow = static_cast<int8_t>(next() & 0xFF);
        e.ticks = static_cast<uint8_t>(next() & 0xFF);
        e.temperature = static_cast<uint8_t>(next() & 0xFF);
        e.color = next();
        e.updated_tag = static_cast<uint8_t>(next());
        e.piece_tag = static_cast<uint8_t>(next());
    }
    return cells;
}

uint64_t fnv1a(const std::vector<uint32_t>& texels) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t t : texels) {
        for (uint8_t b : bytes_of(t)) {
            h ^= b;
            h *= 0x100000001B3ull;
        }
    }
    return h;
}

// Pinned. A change here means a texel now means something else: update the
// shader that mirrors cell_pack.h in the same commit, then re-pin.
constexpr uint64_t GOLDEN = 0xD8E48B3CFDF65635ull;

} // namespace

int main() {
    // --- the layout, byte by byte ---
    {
        const Element w = cell(ElementType::Water, Grid::LIQUID_FULL, -37, 5, 33);
        const auto b = bytes_of(cell_pack::texel_of(w));
        check("R is the type", b[0] == static_cast<uint8_t>(ElementType::Water));
        check("G is a full liquid cell's amount, saturated", b[1] == 255);
        check("B is the temperature", b[2] == 33);
        // flow -37: left, |37| >> 2 = 9 in bits 3-7.
        check("A carries flow sign and magnitude", b[3] == ((1u << 2) | (9u << 3)),
              std::to_string(b[3]));

        const auto sb = bytes_of(cell_pack::texel_of(cell(ElementType::Wall, 0, 0, 7, 140)));
        check("a solid: ticks in G, occluder bit, no flow",
              sb[0] == static_cast<uint8_t>(ElementType::Wall) && sb[1] == 7 && sb[2] == 140 &&
                  sb[3] == (1u << 1));
        check("the bytes are R, G, B, A in memory order",
              cell_pack::texel(1, 2, 3, 4) == 0x04030201u);
    }

    // --- mass mapping ---
    {
        bool ok = true;
        std::string detail;
        struct Case {
            int mass, g;
        };
        constexpr Case CASES[] = {{1, 0},      {15, 0},     {16, 1},     {2048, 128},
                                  {4080, 255}, {4095, 255}, {4096, 255}, {8191, 255}};
        for (const ElementType t : {ElementType::Water, ElementType::Oil}) {
            for (const Case& c : CASES) {
                const uint8_t g = green(cell(t, c.mass, 0, 99));
                if (g != c.g) {
                    ok = false;
                    detail += std::to_string(c.mass) + "->" + std::to_string(g) + " ";
                }
            }
        }
        check("liquid G is min(mass >> 4, 255), ticks ignored", ok, detail);
        check("steam's G is its lifetime, not its mass",
              green(cell(ElementType::Steam, Grid::LIQUID_FULL, 0, 7)) == 7);
        check("fire's G is its lifetime", green(cell(ElementType::Fire, 0, 0, 42)) == 42);
        check("a falling wall's G is its fall clock",
              green(cell(ElementType::Wall, 0, 0, 12)) == 12);
    }

    // --- flags ---
    {
        bool ok = true;
        std::string wrong;
        for (int i = 0; i < static_cast<int>(ElementType::Count); ++i) {
            const auto t = static_cast<ElementType>(i);
            const bool expect = t == ElementType::Sand || t == ElementType::Wall ||
                                t == ElementType::Wood || t == ElementType::Charred;
            const bool got = (alpha(cell(t)) & cell_pack::FLAG_OCCLUDER) != 0;
            if (got != expect) {
                ok = false;
                wrong += std::string(material_of(t).name) + " ";
            }
        }
        check("occluder bit: solids and powders only", ok, wrong);
        check("no cell in the grid is flagged hidden",
              (alpha(cell(ElementType::Wall)) & cell_pack::FLAG_HIDDEN) == 0);
        check("flow +127 saturates at magnitude 31, rightward",
              alpha(cell(ElementType::Water, 4096, 127)) == (31u << 3));
        check("flow -128 saturates at magnitude 31, leftward",
              alpha(cell(ElementType::Water, 4096, -128)) == ((31u << 3) | (1u << 2)));
        check("flow +3 rounds to zero magnitude, no sign",
              alpha(cell(ElementType::Oil, 4096, 3)) == 0);
        check("flow +4 is magnitude 1", alpha(cell(ElementType::Oil, 4096, 4)) == (1u << 3));
        check("Empty packs as all zero bar ambient temperature",
              PAD_TEXEL == cell_pack::texel(0, 0, AMBIENT_TEMPERATURE, 0), hex(PAD_TEXEL));
    }

    // --- hidden cells ---
    {
        std::vector<Element> cells(static_cast<size_t>(8) * 4, cell(ElementType::Empty));
        // A hot stone body at row 1, columns 2..4, in water.
        for (int x = 2; x <= 4; ++x) cells[8 + x] = cell(ElementType::Wall, 0, 0, 3, 90);
        const std::vector<frame::HiddenCell> hidden = {
            {8 + 2, 0xFF2E7F96u, ElementType::Water},
            {8 + 3, 0u, ElementType::Empty},
            {8 + 4, 0xFFC6A970u, ElementType::Sand},
            {-1, 0u, ElementType::Water},     // out of range: ignored
            {8 * 4, 0u, ElementType::Water},  // one past the end: ignored
        };
        CellPack p;
        p.pack(cells.data(), 8, 4, 0, 0, 8, 4, hidden);
        const std::vector<uint32_t>& t = p.texels();
        using cell_pack::FLAG_HIDDEN;
        using cell_pack::FLAG_OCCLUDER;
        check("hidden in water: water, full, own temperature, hidden",
              t[10] ==
                  cell_pack::texel(static_cast<uint32_t>(ElementType::Water), 255, 90, FLAG_HIDDEN),
              hex(t[10]));
        check("hidden in air: Empty, hidden, not an occluder",
              t[11] == cell_pack::texel(0, 0, 90, FLAG_HIDDEN), hex(t[11]));
        check("hidden in sand: sand, hidden and occluder",
              t[12] == cell_pack::texel(static_cast<uint32_t>(ElementType::Sand), 0, 90,
                                        FLAG_HIDDEN | FLAG_OCCLUDER),
              hex(t[12]));
        check("cells not listed are packed as themselves", t[9] == PAD_TEXEL && t[0] == PAD_TEXEL);

        // The same body with the window moved so one of its cells is outside.
        CellPack q;
        q.pack(cells.data(), 8, 4, 3, 0, 4, 4, hidden);
        check("a hidden cell left of the window is skipped, the rest land",
              q.texels()[4 + 0] == cell_pack::texel(0, 0, 90, FLAG_HIDDEN) &&
                  (q.texels()[4 + 1] & 0xFFu) == static_cast<uint32_t>(ElementType::Sand));
    }

    // --- a window past the grid's edges ---
    {
        const std::vector<Element> cells = fixture();
        CellPack p;
        const int x0 = -3, y0 = -2, w = FW + 7, h = FH + 5;
        p.pack(cells.data(), FW, FH, x0, y0, w, h);
        bool sized =
            p.width() == w && p.height() == h && p.texels().size() == static_cast<size_t>(w) * h;
        bool inside_ok = true, pad_ok = true;
        for (int wy = 0; wy < h; ++wy) {
            for (int wx = 0; wx < w; ++wx) {
                const int gx = x0 + wx, gy = y0 + wy;
                const uint32_t got = p.texels()[static_cast<size_t>(wy) * w + wx];
                if (gx < 0 || gy < 0 || gx >= FW || gy >= FH) {
                    if (got != PAD_TEXEL) pad_ok = false;
                } else if (got != cell_pack::texel_of(cells[static_cast<size_t>(gy) * FW + gx])) {
                    inside_ok = false;
                }
            }
        }
        check("a window past every edge is sized as asked", sized);
        check("cells on the grid pack as themselves", inside_ok);
        check("cells off the grid pack as Empty", pad_ok);

        CellPack off;
        off.pack(cells.data(), FW, FH, FW + 10, -50, 6, 3);
        bool all_pad = true;
        for (uint32_t t : off.texels()) all_pad = all_pad && t == PAD_TEXEL;
        check("a window wholly off the grid is all padding", all_pad && off.texels().size() == 18);

        CellPack none;
        none.pack(cells.data(), FW, FH, 0, 0, 0, 5);
        check("an empty window packs nothing", none.texels().empty() && none.width() == 0);
    }

    // --- no allocation once warm ---
    {
        const std::vector<Element> cells = fixture();
        CellPack p;
        p.pack(cells.data(), FW, FH, -1, -1, FW + 2, FH + 2);
        const size_t cap = p.texels().capacity();
        const uint32_t* data = p.texels().data();
        bool stable = true;
        for (int i = 0; i < 50; ++i) {
            p.pack(cells.data(), FW, FH, i % 5 - 2, i % 3 - 1, FW + 2, FH + 2);
            stable = stable && p.texels().capacity() == cap && p.texels().data() == data;
        }
        // A smaller window (a narrower mode) and back keeps the buffer too.
        p.pack(cells.data(), FW, FH, 0, 0, 4, 4);
        p.pack(cells.data(), FW, FH, 0, 0, FW + 2, FH + 2);
        stable = stable && p.texels().capacity() == cap && p.texels().data() == data;
        check("repeated packs reuse one buffer", stable, std::to_string(cap) + " texels");
    }

    // --- the Grid overload reads what get_element reads ---
    {
        Grid g(48, 32);
        for (int x = 0; x < 48; ++x) g.set_element(x, 31, ElementType::Wall);
        for (int y = 4; y < 12; ++y)
            for (int x = 10; x < 20; ++x) g.set_element(x, y, ElementType::Water);
        for (int x = 26; x < 30; ++x) g.set_element(x, 3, ElementType::Sand);
        for (int i = 0; i < 20; ++i) g.update();  // let the water move, so flow and mass vary

        bool same = true;
        const Element* raw = g.cell_data();
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 48; ++x) {
                const Element a = raw[static_cast<size_t>(y) * 48 + x], b = g.get_element(x, y);
                same = same && std::memcmp(&a, &b, sizeof a) == 0;
            }
        check("cell_data is get_element's cells, row-major", same);

        CellPack p;
        p.pack(g, 5, 2, 30, 25);
        bool match = true;
        for (int wy = 0; wy < 25; ++wy)
            for (int wx = 0; wx < 30; ++wx)
                match = match && p.texels()[static_cast<size_t>(wy) * 30 + wx] ==
                                     cell_pack::texel_of(g.get_element(5 + wx, 2 + wy));
        check("packing a Grid packs its cells", match);
    }

    // --- pinned ---
    {
        const std::vector<Element> cells = fixture();
        const std::vector<frame::HiddenCell> hidden = {
            {FW * 2 + 3, 0xFF2E7F96u, ElementType::Water},
            {FW * 5 + 7, 0u, ElementType::Empty},
            {FW * 8 + 12, 0xFFC6A970u, ElementType::Sand},
        };
        CellPack p;
        p.pack(cells.data(), FW, FH, -2, -1, FW + 3, FH + 2, hidden);
        const uint64_t sum = fnv1a(p.texels());
        char detail[48];
        std::snprintf(detail, sizeof detail, "got 0x%016llX", static_cast<unsigned long long>(sum));
        check("the packed fixture matches the pinned checksum", sum == GOLDEN, detail);
    }

    return report();
}
