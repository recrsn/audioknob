#include "mic_process.h"

#include <math.h>
#include <stdlib.h>

#include "esp_ns.h"

#define DC_POLE 0.995f

struct mic_process {
    ns_handle_t ns;
    float gain;
    bool primed;
    int32_t dc_x1;
    float dc_y1;
    int16_t gained[MIC_PROCESS_FRAMES];
};

mic_process_t *mic_process_create(uint8_t gain_db, uint8_t noise_level) {
    mic_process_t *p = calloc(1, sizeof(*p));
    if (!p) {
        return NULL;
    }
    p->gain = powf(10.0f, gain_db / 20.0f);
    if (noise_level) {
        p->ns = ns_pro_create(10, noise_level - 1, 16000);
        if (!p->ns) {
            free(p);
            return NULL;
        }
    }
    return p;
}

void mic_process_block(mic_process_t *p, const int16_t *in, int16_t *out) {
    if (!p->primed) {
        // Starting from zero turns the mic's DC bias into an artificial pop.
        p->dc_x1 = in[0];
        p->primed = true;
    }
    for (int i = 0; i < MIC_PROCESS_FRAMES; i++) {
        int32_t x = in[i];
        float y = (float)(x - p->dc_x1) + p->dc_y1 * DC_POLE;
        p->dc_x1 = x;
        p->dc_y1 = y;
        float gained = y * p->gain;
        p->gained[i] = gained > INT16_MAX ? INT16_MAX : gained < INT16_MIN ? INT16_MIN : (int16_t)gained;
    }
    if (p->ns) {
        ns_process(p->ns, p->gained, out);
    } else {
        for (int i = 0; i < MIC_PROCESS_FRAMES; i++) {
            out[i] = p->gained[i];
        }
    }
}

void mic_process_destroy(mic_process_t *p) {
    if (p) {
        if (p->ns) {
            ns_destroy(p->ns);
        }
        free(p);
    }
}
