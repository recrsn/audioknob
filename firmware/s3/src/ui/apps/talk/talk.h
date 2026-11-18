#pragma once

// Private to the Talk page: talk.c is the page (the list of knobs, the call
// screen, routing between them); thread.c is one knob's conversation, its
// voice notes and calls, with the buttons under it.

#include "ui/apps/tools.h"

#define TALK_ACTIONS_Y 108 // the round buttons' row, from the centre
#define TALK_PAIR_X    44  // either side of centre for a pair of buttons

// thread.c
void talk_thread_build(lv_obj_t *screen);
// Hidden while the list or a call shows.
void talk_thread_hide(void);
// Shows thread under its talk state; `scroll_end` jumps to the newest item.
// Returns whether the avatar should stand in for the empty conversation.
bool talk_thread_render(const ui_thread_t *thread, const ui_talk_t *talk, bool scroll_end);
// The knob, as scrolling.
void talk_thread_scroll(int steps);

// talk.c, for thread.c's buttons.
const ui_note_handlers_t *talk_note_handlers(void);
void talk_dial(uint32_t peer);
void talk_back(void);

// One line about an item: "Missed call", "Voice note 0:12".
void talk_item_summary(const ui_note_t *n, char *buf, size_t len);
// "now", "5m", "3h", "Tue", "12 Nov"; "" before the clock was set.
void talk_time_ago(uint32_t t, char *buf, size_t len);
void talk_duration(uint16_t tenths, char *buf, size_t len);
