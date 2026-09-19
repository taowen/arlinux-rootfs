/* SPDX-License-Identifier: Apache-2.0
 * Arlinux TAWC-DRI wire protocol. All clients and the X server use this definition.
 */

#ifndef ARLINUX_TAWC_DRI_H
#define ARLINUX_TAWC_DRI_H

#include <stdint.h>
#include <stddef.h>

#define TAWC_DRI_NAME              "TAWC-DRI"
#define TAWC_DRI_MAJOR             0
#define TAWC_DRI_MINOR             4

#define X_TAWCDRI_QueryVersion     0
#define X_TAWCDRI_PresentBuffer    1
#define X_TAWCDRI_SelectInput      2
#define X_TAWCDRI_PresentBuffer2   3

#define TAWC_DRI_PRESENT_OPAQUE    (1u << 0)

/* SelectInput event mask bits / XGE evtypes (v0.3). */
#define TAWC_DRI_EVENT_CONFIGURE_NOTIFY       0
#define TAWC_DRI_EVENT_BUFFER_RELEASE         1
#define TAWC_DRI_EVENT_MASK_CONFIGURE_NOTIFY  (1u << 0)
#define TAWC_DRI_EVENT_MASK_BUFFER_RELEASE    (1u << 1)

typedef struct {
    uint8_t  major_opcode;
    uint8_t  minor_opcode;
    uint16_t length;
    uint32_t major_version;
    uint32_t minor_version;
} tawc_dri_query_version_req;

typedef struct {
    uint8_t  response_type;
    uint8_t  pad0;
    uint16_t sequence;
    uint32_t length;
    uint32_t major_version;
    uint32_t minor_version;
    uint32_t pad1[4];
} tawc_dri_query_version_reply;

/* v0.3: `serial` is a client-chosen cookie echoed back in the
 * buffer-release event when the compositor releases the wl_buffer
 * this present created. */
typedef struct __attribute__((packed)) {
    uint8_t  major_opcode;
    uint8_t  minor_opcode;
    uint16_t length;
    uint32_t window;
    uint16_t num_fds;
    uint16_t num_ints;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint32_t usage_lo;
    uint32_t usage_hi;
    uint32_t serial;
} tawc_dri_present_buffer_req;

/* v0.4: explicit composition, independent of the native buffer's alpha channel
 * and X visual depth. Flags zero preserves the v0.3 premultiplied-alpha path.
 * OPAQUE ignores alpha for composition without changing the buffer contents.
 * The native-handle ints follow this entire header. Unknown flags are invalid. */
typedef struct __attribute__((packed)) {
    tawc_dri_present_buffer_req base;
    uint32_t flags;
} tawc_dri_present_buffer2_req;

/* v0.3: select TAWC-DRI events on `window`. `eid` is a client-allocated
 * XID (xcb_generate_id), the routing key for libxcb's special event
 * queue (xcb_register_for_special_xge). Selecting CONFIGURE_NOTIFY
 * immediately delivers one event with the window's current size. */
typedef struct __attribute__((packed)) {
    uint8_t  major_opcode;
    uint8_t  minor_opcode;
    uint16_t length;
    uint32_t eid;
    uint32_t window;
    uint32_t event_mask;
} tawc_dri_select_input_req;

/* Events are XGE generic events; layout matches Present's events
 * (evtype at bytes 8-9, eid at bytes 12-15) because libxcb's
 * special-event matching reads the eid at that fixed offset. */

typedef struct __attribute__((packed)) {
    uint8_t  response_type;     /* 35 = XCB_GE_GENERIC */
    uint8_t  extension;         /* TAWC-DRI major opcode */
    uint16_t sequence;
    uint32_t length;            /* 0 */
    uint16_t evtype;            /* TAWC_DRI_EVENT_CONFIGURE_NOTIFY */
    uint16_t pad1;
    uint32_t eid;
    uint32_t window;
    uint32_t width;
    uint32_t height;
    uint32_t pad2;
} tawc_dri_configure_notify_event;

typedef struct __attribute__((packed)) {
    uint8_t  response_type;     /* 35 = XCB_GE_GENERIC */
    uint8_t  extension;         /* TAWC-DRI major opcode */
    uint16_t sequence;
    uint32_t length;            /* 0 */
    uint16_t evtype;            /* TAWC_DRI_EVENT_BUFFER_RELEASE */
    uint16_t pad1;
    uint32_t eid;
    uint32_t window;
    uint32_t serial;            /* echoed from PresentBuffer */
    uint32_t pad2;
    uint32_t pad3;
} tawc_dri_buffer_release_event;


/* Wire sizes are shared by the X server, Mesa WSI and hybris clients. */
#if defined(__cplusplus)
#define ARLINUX_WIRE_ASSERT static_assert
#else
#define ARLINUX_WIRE_ASSERT _Static_assert
#endif
ARLINUX_WIRE_ASSERT(sizeof(tawc_dri_query_version_req) == 12, "QueryVersion request");
ARLINUX_WIRE_ASSERT(sizeof(tawc_dri_query_version_reply) == 32, "QueryVersion reply");
ARLINUX_WIRE_ASSERT(sizeof(tawc_dri_present_buffer_req) == 40, "PresentBuffer request");
ARLINUX_WIRE_ASSERT(sizeof(tawc_dri_present_buffer2_req) == 44, "PresentBuffer2 request");
ARLINUX_WIRE_ASSERT(sizeof(tawc_dri_select_input_req) == 16, "SelectInput request");
ARLINUX_WIRE_ASSERT(sizeof(tawc_dri_configure_notify_event) == 32, "ConfigureNotify event");
ARLINUX_WIRE_ASSERT(sizeof(tawc_dri_buffer_release_event) == 32, "BufferRelease event");
ARLINUX_WIRE_ASSERT(offsetof(tawc_dri_configure_notify_event, eid) == 12, "XGE routing offset");
ARLINUX_WIRE_ASSERT(offsetof(tawc_dri_buffer_release_event, eid) == 12, "XGE routing offset");
#undef ARLINUX_WIRE_ASSERT

#endif
