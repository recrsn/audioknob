#pragma once

#include "airplay/receiver/rtsp.h"

// Adds Apple-Response when the request carries an Apple-Challenge.
void ap1_apple_challenge(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp);

void ap1_options(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp);
void ap1_announce(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp);
void ap1_setup(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp);
void ap1_record(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp);
void ap1_flush(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp);
void ap1_teardown(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp);

// Releases what ANNOUNCE allocated.
void ap1_free(ap2_conn_t *conn);
