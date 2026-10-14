#include "tz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int by_name(const void *key, const void *entry) {
    return strcmp(key, ((const tz_entry_t *)entry)->name);
}

int tz_db_find(const char *name) {
    const tz_entry_t *e = bsearch(name, tz_db, tz_db_count, sizeof(tz_db[0]), by_name);
    return e ? (int)(e - tz_db) : -1;
}

static void set_tz(const char *posix) {
    setenv("TZ", posix, 1);
    tzset();
}

bool tz_apply(const char *name) {
    int i = tz_db_find(name);
    if (i < 0) {
        return false;
    }
    set_tz(tz_db[i].posix);
    return true;
}

void tz_apply_offset(int offset_s) {
    // POSIX offsets are west-positive: UTC+05:30 is "<+0530>-5:30".
    int east = offset_s >= 0;
    int mins = abs(offset_s) / 60;
    char posix[32];
    snprintf(posix, sizeof(posix), "<%c%02d%02d>%c%d:%02d", east ? '+' : '-', mins / 60, mins % 60,
             east ? '-' : '+', mins / 60, mins % 60);
    set_tz(posix);
}
