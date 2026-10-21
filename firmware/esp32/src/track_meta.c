#include "track_meta.h"

#include <stdbool.h>
#include <string.h>

#include "avrcp/controller.h"
#include "esp_log.h"
#include "protocol.h"
#include "state_sync.h"

static const char *TAG = "track_meta";

static esp_avrc_rn_evt_cap_mask_t s_peer_caps;
static track_info_t s_track;

// Cover art. A fetch streams straight to the S3 as data events arrive; a
// handle that changes mid-fetch waits in s_want until the fetch ends.
static bool s_ca_connected;
static bool s_fetching;
static uint8_t s_want[ESP_AVRC_CA_IMAGE_HANDLE_LEN];
static uint8_t s_fetch[ESP_AVRC_CA_IMAGE_HANDLE_LEN];
static uint8_t s_shown[ESP_AVRC_CA_IMAGE_HANDLE_LEN];
static uint8_t s_art_id;
static uint32_t s_art_offset;

static void request_metadata(void) {
    uint8_t mask = ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST | ESP_AVRC_MD_ATTR_ALBUM;
    if (s_ca_connected) {
        mask |= ESP_AVRC_MD_ATTR_COVER_ART;
    }
    esp_avrc_ct_send_metadata_cmd(avrcp_controller_alloc_tl(), mask);
}

// AVRCP notifications fire once per registration, so this re-arms after each.
static void register_track_change(void) {
    if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &s_peer_caps, ESP_AVRC_RN_TRACK_CHANGE)) {
        esp_avrc_ct_send_register_notification_cmd(avrcp_controller_alloc_tl(), ESP_AVRC_RN_TRACK_CHANGE, 0);
    }
}

// Copies at most cap-1 bytes without splitting a UTF-8 sequence.
static void copy_text(char *dst, size_t cap, const uint8_t *src, int len) {
    size_t n = len < 0 ? 0 : (size_t)len;
    if (n > cap - 1) {
        n = cap - 1;
        while (n > 0 && (src[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void clear_art(void) {
    memset(s_shown, 0, sizeof(s_shown));
    state_sync_send_art_end(++s_art_id, 0);
}

static void maybe_fetch_art(void) {
    if (s_fetching || !s_ca_connected || !s_want[0] || memcmp(s_want, s_shown, sizeof(s_want)) == 0) {
        return;
    }
    memcpy(s_fetch, s_want, sizeof(s_fetch));
    s_art_id++;
    s_art_offset = 0;
    s_fetching = esp_avrc_ct_cover_art_get_linked_thumbnail(s_fetch) == ESP_OK;
}

static void finish_fetch(bool ok) {
    s_fetching = false;
    if (!s_want[0]) {
        // The track changed to one without art while this was in flight.
        clear_art();
        return;
    }
    // Marked shown even on failure, so a broken image isn't retried forever.
    memcpy(s_shown, s_fetch, sizeof(s_shown));
    state_sync_send_art_end(s_art_id, ok ? s_art_offset : 0);
    if (ok) {
        ESP_LOGI(TAG, "cover art sent (%u bytes)", (unsigned)s_art_offset);
    }
    maybe_fetch_art();
}

static void on_art_handle(const uint8_t *handle, int len) {
    if (len != ESP_AVRC_CA_IMAGE_HANDLE_LEN) {
        memset(s_want, 0, sizeof(s_want));
        if (s_shown[0] && !s_fetching) {
            clear_art();
        }
        return;
    }
    memcpy(s_want, handle, sizeof(s_want));
    maybe_fetch_art();
}

static void on_art_data(const esp_avrc_ct_cb_param_t *rc) {
    if (!s_fetching) {
        return;
    }
    if (rc->cover_art_data.status != ESP_BT_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "cover art fetch failed (%d)", rc->cover_art_data.status);
        finish_fetch(false);
        return;
    }
    const uint8_t *data = rc->cover_art_data.p_data;
    uint32_t len = rc->cover_art_data.data_len;
    for (uint32_t sent = 0; sent < len;) {
        uint32_t n = len - sent > ART_CHUNK_MAX_DATA ? ART_CHUNK_MAX_DATA : len - sent;
        state_sync_send_art_chunk(s_art_id, s_art_offset, data + sent, (uint8_t)n);
        s_art_offset += n;
        sent += n;
    }
    if (rc->cover_art_data.final) {
        finish_fetch(true);
    }
}

static void on_metadata(const esp_avrc_ct_cb_param_t *rc) {
    const uint8_t *text = rc->meta_rsp.attr_text;
    int len = rc->meta_rsp.attr_length;
    switch (rc->meta_rsp.attr_id) {
    case ESP_AVRC_MD_ATTR_TITLE:
        copy_text(s_track.title, sizeof(s_track.title), text, len);
        break;
    case ESP_AVRC_MD_ATTR_ARTIST:
        copy_text(s_track.artist, sizeof(s_track.artist), text, len);
        break;
    case ESP_AVRC_MD_ATTR_ALBUM:
        copy_text(s_track.album, sizeof(s_track.album), text, len);
        break;
    case ESP_AVRC_MD_ATTR_COVER_ART:
        on_art_handle(text, len);
        return;
    default:
        return;
    }
    // Each attribute arrives as its own event; frames are small enough to
    // just resend the whole track on each.
    state_sync_send_track(&s_track);
}

static void reset(void) {
    memset(&s_track, 0, sizeof(s_track));
    memset(&s_peer_caps, 0, sizeof(s_peer_caps));
    memset(s_want, 0, sizeof(s_want));
    s_ca_connected = false;
    s_fetching = false;
    state_sync_send_track(&s_track);
    clear_art();
}

void track_meta_handle_ct_event(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *rc) {
    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        reset();
        if (rc->conn_stat.connected) {
            esp_avrc_ct_send_get_rn_capabilities_cmd(avrcp_controller_alloc_tl());
        }
        break;

    case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
        if (rc->rmt_feats.tg_feat_flag & ESP_AVRC_FEAT_FLAG_TG_COVER_ART) {
            ESP_LOGI(TAG, "phone offers cover art, connecting");
            esp_avrc_ct_cover_art_connect(ESP_AVRC_CA_MTU_MAX);
        }
        break;

    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
        s_peer_caps = rc->get_rn_caps_rsp.evt_set;
        register_track_change();
        request_metadata();
        break;

    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
        if (rc->change_ntf.event_id == ESP_AVRC_RN_TRACK_CHANGE) {
            register_track_change();
            request_metadata();
        }
        break;

    case ESP_AVRC_CT_METADATA_RSP_EVT:
        on_metadata(rc);
        break;

    case ESP_AVRC_CT_COVER_ART_STATE_EVT:
        s_ca_connected = rc->cover_art_state.state == ESP_AVRC_COVER_ART_CONNECTED;
        ESP_LOGI(TAG, "cover art channel %s", s_ca_connected ? "connected" : "disconnected");
        if (s_ca_connected) {
            // The first metadata request may have gone out without the
            // cover-art attribute; ask again now that it can be fetched.
            request_metadata();
        } else if (s_fetching) {
            // Unlike a failed fetch, this one is worth retrying on reconnect.
            s_fetching = false;
            clear_art();
        }
        break;

    case ESP_AVRC_CT_COVER_ART_DATA_EVT:
        on_art_data(rc);
        break;

    default:
        break;
    }
}
