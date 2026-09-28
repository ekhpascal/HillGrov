#pragma once
/* Browser firmware upload -- POST /api/fw/master (a raw app image into the inactive OTA slot) and POST /api/fw/zone (a
 * raw zone app image into the zone_fw partition behind the 16-byte HGFW header fw_srv.c validates).
 *
 * http_upload.c is only the HTTP face: the framing checks (Transfer-Encoding -> 400 CHUNKED_UNSUPPORTED, Content-Type
 * must be application/octet-stream -> 400 BAD_TYPE, content_len 0 -> 400 EMPTY_BODY), a recv source for the install
 * core, the drain-before-answer rule and the response. Every other guard -- one install at a time, never while the fleet
 * sequencer runs, the target's ready/heap/size checks, the image identity (magic, chip id, app-descriptor magic and
 * project name) before anything is erased -- is panel_svc's install core (psvc_fw.h), shared with the panel's microSD
 * install. The route's auth bit (http_routes.c) has already run before either handler is reached. */
