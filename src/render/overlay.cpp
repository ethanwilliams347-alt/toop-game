#include "render/overlay.h"
#include <algorithm>
#include "ui/hotbar.h"
#include "ui/text.h"

namespace overlay {

void draw(gfx::Canvas& canvas, const Params& p) {
    // The reticle. Three things it has to do, and a one-cell filled square
    // does none of them well enough to survive a playtest.
    //
    // Drawn in screen pixels, not cells. A cell is a handful of screen pixels
    // at the usual scale, so a cell-sized marker has no room for a shape in
    // it. Screen space also keeps its size and legibility if the scale
    // changes.
    //
    // Colour carries range only -- full white in reach, dim white out of it.
    // A marker in the same colour family as Fire disappears against the one
    // thing you most want to aim at.
    //
    // Each arm is drawn over a dark outline. A white reticle would vanish
    // against snow and steam exactly as an orange one vanishes against fire;
    // an outlined one cannot vanish against anything, because whatever the
    // background is, one of the two contrasts with it.
    //
    // Whether the range test passed is the caller's answer, not this file's:
    // it is a distance in world cells measured against DigTool::RANGE, and
    // nothing under src/render/ may reach for either of those.

    // A gap wider than the arms are long would read as four separate marks
    // rather than one reticle; the point of leaving the centre open is that
    // the cell being aimed at stays visible, so the gap is sized to the cell.
    constexpr int TICK_GAP = 4;
    constexpr int TICK_LEN = 7;
    constexpr int TICK_THICK = 2;

    const gfx::Rect arms[4] = {
        {p.mouse_x - TICK_THICK / 2, p.mouse_y - TICK_GAP - TICK_LEN, TICK_THICK, TICK_LEN}, // N
        {p.mouse_x - TICK_THICK / 2, p.mouse_y + TICK_GAP, TICK_THICK, TICK_LEN}, // S
        {p.mouse_x - TICK_GAP - TICK_LEN, p.mouse_y - TICK_THICK / 2, TICK_LEN, TICK_THICK}, // W
        {p.mouse_x + TICK_GAP, p.mouse_y - TICK_THICK / 2, TICK_LEN, TICK_THICK}, // E
    };

    // Only while the pointer is actually over this window -- see show_reticle
    // in overlay.h for why that question cannot be asked from in here.
    if (p.show_reticle) {
        canvas.set_blend(gfx::Blend::Alpha);

        canvas.set_draw_color(0, 0, 0, p.in_range ? 170 : 120);
        for (const gfx::Rect& arm : arms) {
            const gfx::Rect outline{arm.x - 1, arm.y - 1, arm.w + 2, arm.h + 2};
            canvas.fill_rect(outline);
        }

        canvas.set_draw_color(255, 255, 255, p.in_range ? 255 : 115);
        for (const gfx::Rect& arm : arms) canvas.fill_rect(arm);

        canvas.set_blend(gfx::Blend::None);
    }

    // Drawn here, in the game window, rather than left to the OS title bar --
    // a solid backing rect first so the text stays readable over whatever the
    // simulation is doing underneath it.
    //
    // Scaled with the window rather than fixed (see DisplayMode::ui_scale): a
    // HUD that keeps its pixel size shrinks as a fraction of the screen every
    // time a wider mode is added, and this one is an instrument.
    const int hud_scale = p.ui_scale;
    const int hud_x = 8, hud_y = 8;
    const gfx::Rect hud_backing{hud_x - 4, hud_y - 4, ui::text_width(p.hud_text, hud_scale) + 8,
                                ui::GLYPH_HEIGHT * hud_scale + 8};
    canvas.set_draw_color(0, 0, 0, 180);
    canvas.set_blend(gfx::Blend::Alpha);
    canvas.fill_rect(hud_backing);
    canvas.set_blend(gfx::Blend::None);
    ui::draw_text(canvas, hud_x, hud_y, hud_scale, p.hud_text, 0xFFE0E0E0);

    // The lines under the HUD, stacked in the order they are drawn. A cursor
    // rather than a y computed per line, because several of them are
    // conditional and a set of independently-computed offsets is how two end
    // up drawn on top of each other in whichever combination nobody tried.
    //
    // A vector built by the caller rather than a lambda called from three
    // places: which lines exist is a question about the recorder, the debug
    // modes and the inspector, and none of those may be read from here.
    int next_line_y = hud_y + ui::GLYPH_HEIGHT * hud_scale + 8;
    for (const Line& line : p.hud_lines) {
        const gfx::Rect backing{hud_x - 4, next_line_y - 4,
                                ui::text_width(line.text, hud_scale) + 8,
                                ui::GLYPH_HEIGHT * hud_scale + 8};
        canvas.set_draw_color(0, 0, 0, 180);
        canvas.set_blend(gfx::Blend::Alpha);
        canvas.fill_rect(backing);
        canvas.set_blend(gfx::Blend::None);
        ui::draw_text(canvas, hud_x, next_line_y, hud_scale, line.text, line.colour);
        next_line_y += ui::GLYPH_HEIGHT * hud_scale + 8;
    }

    // --- the material hotbar ---
    //
    // The selected slot is looked up by the caller rather than stored here, so
    // current_brush stays the single fact about what is selected; -1 draws no
    // highlight.
    ui::draw_hotbar(canvas, p.window_w, p.ui_scale, p.hotbar_selected);

    // --- how a finished run says so ---
    //
    // Two lines over a dimming wash, drawn after the hotbar and before the
    // settings menu, so ESC still opens a menu that sits on top of this.
    //
    // Over a wash rather than a solid panel, for the same reason the settings
    // screen is: the world behind it is the thing the player has to be able to
    // look at -- the fire they walked into, the drop they misjudged -- and a
    // panel would hide the whole content of the ending. The simulation is
    // frozen behind this, not running.
    //
    // Deliberately not a menu. A run that has ended offers exactly one choice,
    // and a cursor over one item is furniture.
    if (p.run_over) {
        const int scale = p.ui_scale;
        const std::string headline = p.won ? "OBJECTIVE REACHED" : "YOU DIED";
        const std::string prompt = "PRESS R FOR A NEW RUN";

        canvas.set_blend(gfx::Blend::Alpha);
        canvas.set_draw_color(0, 0, 0, 140);
        const gfx::Rect wash{0, 0, p.window_w, p.window_h};
        canvas.fill_rect(wash);

        // The headline is drawn at twice the HUD's scale, which is the only
        // place in the game anything is -- it is the one string that has to
        // read from across a room rather than be looked at.
        const int head_scale = scale * 2;
        const int head_y = p.window_h / 2 - (ui::GLYPH_HEIGHT * head_scale) / 2;
        ui::draw_text(canvas, p.window_w / 2 - ui::text_width(headline, head_scale) / 2, head_y,
                      head_scale, headline, p.won ? 0xFFF0C040 : 0xFFD05050);
        ui::draw_text(canvas, p.window_w / 2 - ui::text_width(prompt, scale) / 2,
                      head_y + ui::GLYPH_HEIGHT * head_scale + 6 * scale, scale, prompt,
                      0xFFC0C0C0);
        canvas.set_blend(gfx::Blend::None);
    }

    // --- the settings menu ---
    //
    // Drawn last, over a dimming wash rather than a solid panel, so the world
    // stays visible behind it. That is not decoration: the only way to judge
    // "is this the resolution I want" is to see how much of the world it
    // shows, and a menu that hides the world hides the thing the setting
    // changes.
    if (p.settings_open) {
        const int scale = p.ui_scale;
        const int line_h = (ui::GLYPH_HEIGHT + 3) * scale;
        const int char_w = (ui::GLYPH_WIDTH + 1) * scale;
        const menu::View& v = p.settings;

        canvas.set_blend(gfx::Blend::Alpha);
        canvas.set_draw_color(0, 0, 0, 170);
        const gfx::Rect wash{0, 0, p.window_w, p.window_h};
        canvas.fill_rect(wash);

        // Two columns. Every value starts the same distance after the longest
        // label, so a changed value can be read down the column and the arrows
        // of the selected one never push it sideways.
        size_t label_chars = 0;
        for (const menu::RowView& r : v.rows) label_chars = std::max(label_chars, r.label.size());
        const int value_dx = static_cast<int>(label_chars + 4) * char_w;

        // Centred on the two columns, at the width of the longest value any row
        // can hold (BORDERLESS, FULLSCREEN), with the notes and the footer hanging
        // to the right of where that puts the block. Not centred on the widest
        // line drawn: a note comes and goes as a value is stepped, and a menu that
        // re-centres on every key press slides sideways under the reader's eye.
        constexpr int VALUE_COLUMN_CHARS = 10;
        const int menu_x = p.window_w / 2 - (value_dx + VALUE_COLUMN_CHARS * char_w) / 2;
        const int line_count = static_cast<int>(v.rows.size()) + 5;
        int y = p.window_h / 2 - line_count * line_h / 2;

        ui::draw_text(canvas, menu_x, y, scale, "SETTINGS", 0xFFFFFFFF);
        y += line_h * 2;

        for (const menu::RowView& r : v.rows) {
            // Greyed while there is nothing it would do (Apply with nothing
            // pending), but still selectable: Enter there closes the menu.
            uint32_t colour = r.selected ? 0xFFFFFFFF : 0xFFA0A0A0;
            if (!r.enabled) colour = r.selected ? 0xFF909090 : 0xFF606060;
            // The cursor is drawn beside the label as well as in its colour,
            // because a colour alone is the cue that does not survive a bright
            // world behind the wash.
            if (r.selected) ui::draw_text(canvas, menu_x - 2 * char_w, y, scale, ">", colour);
            ui::draw_text(canvas, menu_x, y, scale, r.label, colour);

            if (!r.value.empty()) {
                // A value not applied yet is amber, so the rows say which of them
                // Enter is about to act on. One that left and right cannot move
                // (borderless's size, which is the desktop's) is dimmed.
                uint32_t vc = colour;
                if (r.changed)
                    vc = 0xFFFFD060;
                else if (!r.adjustable)
                    vc = 0xFF808080;
                const int vx = menu_x + value_dx;
                const int vw = ui::text_width(r.value, scale);
                if (r.selected && r.adjustable) {
                    ui::draw_text(canvas, vx - 2 * char_w, y, scale, "<", vc);
                    ui::draw_text(canvas, vx + vw + char_w, y, scale, ">", vc);
                }
                ui::draw_text(canvas, vx, y, scale, r.value, vc);
                if (!r.note.empty())
                    ui::draw_text(canvas, vx + vw + 3 * char_w, y, scale, r.note, 0xFF808080);
            }
            y += line_h;
        }

        // The key help, or the question a new mode is waiting on -- the latter in
        // the notice colour, because it is the one line on the screen that will
        // act on its own if it is not read.
        y += line_h;
        ui::draw_text(canvas, menu_x, y, scale, v.footer, v.confirming ? 0xFFFFC080 : 0xFF808080);
        // The notice is drawn when there is one; its timer is the caller's, and an
        // expired notice arrives here as an empty string.
        if (!p.notice.empty()) {
            y += line_h;
            ui::draw_text(canvas, menu_x, y, scale, p.notice, 0xFFFFC080);
        }
        canvas.set_blend(gfx::Blend::None);
    }
}

} // namespace overlay
