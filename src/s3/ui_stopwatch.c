#include "ui_tools.h"

#include <stdio.h>
#include <string.h>

#define SW_TIME_Y     (-38)
#define SW_LAPS_Y     12
#define SW_LAP_ROWS   3
#define SW_RING_RANGE 600 // one revolution per minute, 0.1 s resolution

static struct {
    lv_obj_t *ring;
    lv_obj_t *time_label;
    lv_obj_t *cs_label;
    lv_obj_t *lap_labels[SW_LAP_ROWS];
    lv_obj_t *start_icon;
    lv_obj_t *lap_icon;
    bool running;
    uint32_t base_ms; // elapsed before the current run
    uint32_t start_tick;
    uint32_t last_split_ms;
    uint32_t lap_ms[SW_LAP_ROWS]; // newest first
    uint32_t lap_count;
} sw;

static uint32_t elapsed_ms(void) {
    return sw.base_ms + (sw.running ? lv_tick_elaps(sw.start_tick) : 0);
}

static void format_split(uint32_t ms, char *out, size_t len) {
    uint32_t s = ms / 1000;
    if (s >= 3600) {
        snprintf(out, len, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60),
                 (unsigned long)(s % 60));
    } else {
        snprintf(out, len, "%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
    }
}

static void refresh(void) {
    uint32_t ms = elapsed_ms();
    char buf[24];
    format_split(ms, buf, sizeof(buf));
    ui_label_set_text(sw.time_label, buf);
    snprintf(buf, sizeof(buf), ".%02lu", (unsigned long)(ms / 10 % 100));
    ui_label_set_text(sw.cs_label, buf);

    tool_ring_set(sw.ring, (int32_t)(ms % 60000 / 100));

    for (uint32_t i = 0; i < SW_LAP_ROWS; i++) {
        bool shown = i < sw.lap_count;
        ui_set_visible(sw.lap_labels[i], shown);
        if (shown) {
            char split[16];
            format_split(sw.lap_ms[i], split, sizeof(split));
            snprintf(buf, sizeof(buf), "Lap %lu   %s.%02lu", (unsigned long)(sw.lap_count - i), split,
                     (unsigned long)(sw.lap_ms[i] / 10 % 100));
            ui_label_set_text(sw.lap_labels[i], buf);
        }
    }

    ui_label_set_text(sw.start_icon, sw.running ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    ui_label_set_text(sw.lap_icon, sw.running || ms == 0 ? "Lap" : "Reset");
    tool_button_set_enabled(sw.lap_icon, sw.running || ms > 0);
}

static void toggle(void) {
    if (sw.running) {
        sw.base_ms = elapsed_ms();
        sw.running = false;
    } else {
        sw.start_tick = lv_tick_get();
        sw.running = true;
    }
    refresh();
}

static void lap_or_reset(void) {
    if (sw.running) {
        uint32_t now = elapsed_ms();
        memmove(&sw.lap_ms[1], &sw.lap_ms[0], sizeof(sw.lap_ms[0]) * (SW_LAP_ROWS - 1));
        sw.lap_ms[0] = now - sw.last_split_ms;
        sw.last_split_ms = now;
        sw.lap_count++;
    } else {
        sw.base_ms = 0;
        sw.last_split_ms = 0;
        sw.lap_count = 0;
    }
    refresh();
}

static void tick(void) {
    if (sw.running) {
        refresh();
    }
}

static bool status(char *buf, size_t len) {
    uint32_t ms = elapsed_ms();
    if (ms == 0) {
        return false;
    }
    format_split(ms, buf, len);
    return true;
}

static void toggle_cb(lv_event_t *e) {
    (void)e;
    toggle();
}

static void lap_cb(lv_event_t *e) {
    (void)e;
    lap_or_reset();
}

void stopwatch_page_build(lv_obj_t *screen) {
    lv_color_t tint = lv_color_hex(UI_TINT_STOPWATCH);
    tool_page_register(screen, &(tool_page_desc_t){.name = "Stopwatch",
                                                   .icon = ICON_STOPWATCH,
                                                   .tint = UI_TINT_STOPWATCH,
                                                   .press = toggle,
                                                   .tick = tick,
                                                   .status = status});

    sw.ring = tool_dial_create(screen, tint, SW_RING_RANGE);
    tool_title_create(screen, "STOPWATCH", tint);

    // Hundredths ride beside the big digits at half height, baseline-aligned
    // to read as one number.
    lv_obj_t *row = lv_obj_create(screen);
    lv_obj_remove_style_all(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, SW_TIME_Y);

    sw.time_label = tool_time_label_create(row);
    sw.cs_label = lv_label_create(row);
    lv_obj_set_style_text_font(sw.cs_label, UI_FONT_READING_SM, 0);
    lv_obj_set_style_text_color(sw.cs_label, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_obj_set_style_pad_bottom(sw.cs_label, UI_FONT_READING->base_line - UI_FONT_READING_SM->base_line, 0);

    lv_obj_t *laps = lv_obj_create(screen);
    lv_obj_remove_style_all(laps);
    lv_obj_remove_flag(laps, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(laps, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(laps, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(laps, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(laps, UI_SPACE_XXS, 0);
    lv_obj_align(laps, LV_ALIGN_TOP_MID, 0, LV_VER_RES / 2 + SW_LAPS_Y);
    for (int i = 0; i < SW_LAP_ROWS; i++) {
        sw.lap_labels[i] = lv_label_create(laps);
        lv_obj_set_style_text_font(sw.lap_labels[i], UI_FONT_BODY, 0);
        lv_obj_set_style_text_color(sw.lap_labels[i], lv_color_hex(UI_COLOR_TEXT), 0);
        lv_obj_set_style_text_opa(sw.lap_labels[i], i == 0 ? UI_OPA_TEXT_SOFT : UI_OPA_TEXT_MUTED, 0);
    }

    lv_obj_t *buttons = tool_button_row_create(screen);
    sw.lap_icon = tool_button_create(buttons, "Lap", lap_cb);
    sw.start_icon = tool_primary_button_create(buttons, tint, toggle_cb);

    refresh();
}
