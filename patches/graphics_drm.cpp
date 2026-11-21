/*
 * Copyright (C) 2015 The Android Open Source Project
 * Copyright (C) 2026 TeamWin Recovery Project
 * Copyright (C) 2026 Zeusx212 & 7sarus
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <drm_fourcc.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/cdefs.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "minuitwrp/minui.h"
#include "graphics.h"
#include <pixelflinger/pixelflinger.h>

#define ARRAY_SIZE(A) (sizeof(A)/sizeof(*(A)))

/*
 * Qualcomm MDSS / SDE Electrophoretic Display Controller (EPDC) Update Parameters.
 * Recovered from Qualcomm Linux 4.19 kernel driver (sde_plane.c / onyx_epdc).
 * Total size: 8 * 40 bytes = 320 bytes.
 */
struct epdc_update_params {
    int32_t rect[4];        /* +0x00: { x, y, width, height } */
    int32_t waveform_mode;  /* +0x10: 2 = GC16 (full quality), 1 = DU, 12 = A2 */
    int32_t update_mode;    /* +0x14: 0 = normal full update */
    int32_t update_marker;  /* +0x18: monotonically increasing submission counter */
    int32_t temp;           /* +0x1c: ambient temperature index (0 = default/auto) */
    int32_t flag;           /* +0x20: 0x21000 accepted by Qualcomm EPDC driver */
    int32_t reserved;       /* +0x24: reserved / padding */
};

static struct epdc_update_params g_epdc_parms[8];
static uint32_t g_update_marker = 0;

struct drm_surface {
    GRSurface base;
    uint32_t fb_id;
    uint32_t handle;
};

static drm_surface *drm_surfaces[2];
static int current_buffer = 0;
static GRSurface *draw_buf = NULL;

static drmModeCrtc *main_monitor_crtc = NULL;
static drmModeConnector *main_monitor_connector = NULL;

static int drm_fd = -1;
static int screen_width = 0;
static int screen_height = 0;

/* Atomic DRM Plane Properties */
static uint32_t g_plane_id = 0;
static uint32_t prop_plane_fb_id = 0;
static uint32_t prop_plane_crtc_id = 0;
static uint32_t prop_plane_crtc_x = 0;
static uint32_t prop_plane_crtc_y = 0;
static uint32_t prop_plane_crtc_w = 0;
static uint32_t prop_plane_crtc_h = 0;
static uint32_t prop_plane_src_x = 0;
static uint32_t prop_plane_src_y = 0;
static uint32_t prop_plane_src_w = 0;
static uint32_t prop_plane_src_h = 0;
static uint32_t prop_plane_zpos = 0;
static uint32_t prop_epdc_parms_addr = 0;
static uint32_t prop_epdc_cnt = 0;

/* Atomic DRM CRTC Properties */
static uint32_t prop_crtc_active = 0;
static uint32_t prop_crtc_mode_id = 0;
static uint32_t g_mode_blob_id = 0;

/* Atomic DRM Connector Properties */
static uint32_t prop_conn_crtc_id = 0;

static bool g_has_atomic = false;

static void drm_disable_crtc(int fd, drmModeCrtc *crtc) {
    if (crtc) {
        drmModeSetCrtc(fd, crtc->crtc_id,
                       0, // fb_id
                       0, 0,  // x,y
                       NULL,  // connectors
                       0,     // connector_count
                       NULL); // mode
    }
}

static void drm_enable_crtc(int fd, drmModeCrtc *crtc, struct drm_surface *surface) {
    if (!crtc || !surface || !main_monitor_connector)
        return;

    int32_t ret = drmModeSetCrtc(fd, crtc->crtc_id,
                                surface->fb_id,
                                0, 0,
                                &main_monitor_connector->connector_id,
                                1,
                                &main_monitor_crtc->mode);
    if (ret)
        printf("minui: drmModeSetCrtc failed ret=%d\n", ret);
}

static void drm_destroy_surface(struct drm_surface *surface) {
    struct drm_gem_close gem_close;
    int ret;

    if (!surface)
        return;

    if (surface->base.data)
        munmap(surface->base.data, surface->base.row_bytes * surface->base.height);

    if (surface->fb_id) {
        ret = drmModeRmFB(drm_fd, surface->fb_id);
        if (ret)
            printf("minui: drmModeRmFB failed ret=%d\n", ret);
    }

    if (surface->handle) {
        memset(&gem_close, 0, sizeof(gem_close));
        gem_close.handle = surface->handle;
        ret = drmIoctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &gem_close);
        if (ret)
            printf("minui: DRM_IOCTL_GEM_CLOSE failed ret=%d\n", ret);
    }

    free(surface);
}

static int drm_format_to_bpp(uint32_t format) {
    switch (format) {
        case DRM_FORMAT_ABGR8888:
        case DRM_FORMAT_BGRA8888:
        case DRM_FORMAT_RGBX8888:
        case DRM_FORMAT_BGRX8888:
        case DRM_FORMAT_XBGR8888:
        case DRM_FORMAT_ARGB8888:
        case DRM_FORMAT_XRGB8888:
        case DRM_FORMAT_RGBA8888:
            return 32;
        case DRM_FORMAT_RGB565:
            return 16;
        default:
            printf("minui: Unknown format %d, defaulting to 32bpp\n", format);
            return 32;
    }
}

static drm_surface *drm_create_surface(int width, int height) {
    struct drm_surface *surface;
    struct drm_mode_create_dumb create_dumb;
    uint32_t format;
    __u32 base_format;
    int ret;

    surface = (struct drm_surface*)calloc(1, sizeof(*surface));
    if (!surface) {
        printf("minui: Can't allocate surface memory\n");
        return NULL;
    }

#if defined(RECOVERY_ABGR)
    format = DRM_FORMAT_RGBA8888;
    base_format = GGL_PIXEL_FORMAT_RGBA_8888;
#elif defined(RECOVERY_BGRA)
    format = DRM_FORMAT_ARGB8888;
    base_format = GGL_PIXEL_FORMAT_RGBA_8888;
#elif defined(RECOVERY_RGBA)
    format = DRM_FORMAT_ABGR8888;
    base_format = GGL_PIXEL_FORMAT_BGRA_8888;
#elif defined(RECOVERY_RGBX)
    format = DRM_FORMAT_XBGR8888;
    base_format = GGL_PIXEL_FORMAT_RGBA_8888;
#else
    format = DRM_FORMAT_RGBA8888;
    base_format = GGL_PIXEL_FORMAT_RGBA_8888;
#endif

    memset(&create_dumb, 0, sizeof(create_dumb));
    create_dumb.height = height;
    create_dumb.width = width;
    create_dumb.bpp = drm_format_to_bpp(format);
    create_dumb.flags = 0;

    ret = drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_dumb);
    if (ret) {
        printf("minui: DRM_IOCTL_MODE_CREATE_DUMB failed ret=%d\n", ret);
        drm_destroy_surface(surface);
        return NULL;
    }
    surface->handle = create_dumb.handle;

    uint32_t handles[4] = { surface->handle, 0, 0, 0 };
    uint32_t pitches[4] = { create_dumb.pitch, 0, 0, 0 };
    uint32_t offsets[4] = { 0, 0, 0, 0 };

    ret = drmModeAddFB2(drm_fd, width, height, format, handles, pitches, offsets, &(surface->fb_id), 0);
    if (ret) {
        printf("minui: drmModeAddFB2 failed ret=%d\n", ret);
        drm_destroy_surface(surface);
        return NULL;
    }

    struct drm_mode_map_dumb map_dumb;
    memset(&map_dumb, 0, sizeof(map_dumb));
    map_dumb.handle = create_dumb.handle;
    ret = drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map_dumb);
    if (ret) {
        printf("minui: DRM_IOCTL_MODE_MAP_DUMB failed ret=%d\n", ret);
        drm_destroy_surface(surface);
        return NULL;
    }

    surface->base.height = height;
    surface->base.width = width;
    surface->base.row_bytes = create_dumb.pitch;
    surface->base.pixel_bytes = create_dumb.bpp / 8;
    surface->base.format = base_format;
    surface->base.data = (unsigned char*)mmap(NULL,
                                              surface->base.height * surface->base.row_bytes,
                                              PROT_READ | PROT_WRITE,
                                              MAP_SHARED,
                                              drm_fd,
                                              map_dumb.offset);
    if (surface->base.data == MAP_FAILED) {
        perror("minui: mmap() failed");
        drm_destroy_surface(surface);
        return NULL;
    }

    return surface;
}

static drmModeCrtc *find_crtc_for_connector(int fd, drmModeRes *resources, drmModeConnector *connector) {
    int i, j;
    drmModeEncoder *encoder;
    int32_t crtc = -1;

    if (connector->encoder_id)
        encoder = drmModeGetEncoder(fd, connector->encoder_id);
    else
        encoder = NULL;

    if (encoder && encoder->crtc_id) {
        crtc = encoder->crtc_id;
        drmModeFreeEncoder(encoder);
        printf("minui: connector %d using active encoder crtc %d\n", connector->connector_id, crtc);
        return drmModeGetCrtc(fd, crtc);
    }

    if (encoder)
        drmModeFreeEncoder(encoder);

    for (i = 0; i < connector->count_encoders; i++) {
        encoder = drmModeGetEncoder(fd, connector->encoders[i]);
        if (encoder) {
            for (j = 0; j < resources->count_crtcs; j++) {
                if (!(encoder->possible_crtcs & (1 << j)))
                    continue;
                /* Prefer physical EPDC display CRTC (ID 112 on Qualcomm Bengal) */
                if (resources->crtcs[j] == 112) {
                    crtc = resources->crtcs[j];
                    break;
                }
                if (crtc < 0) {
                    crtc = resources->crtcs[j];
                }
            }
            drmModeFreeEncoder(encoder);
            if (crtc >= 0) {
                printf("minui: connector %d mapped to crtc %d\n", connector->connector_id, crtc);
                return drmModeGetCrtc(fd, crtc);
            }
        }
    }

    return NULL;
}

static drmModeConnector *find_used_connector_by_type(int fd, drmModeRes *resources, unsigned type) {
    int i;
    for (i = 0; i < resources->count_connectors; i++) {
        drmModeConnector *connector = drmModeGetConnector(fd, resources->connectors[i]);
        if (connector) {
            if ((connector->connector_type == type) &&
                (connector->connection == DRM_MODE_CONNECTED) &&
                (connector->count_modes > 0)) {
                /* Prioritize real physical EPDC panel (Connector 46 / DSI-2) */
                if (connector->connector_id == 46 ||
                    (connector->modes[0].hdisplay == 301 && connector->modes[0].vdisplay == 769)) {
                    printf("minui: selected physical EPDC connector %d (timing %dx%d)\n",
                           connector->connector_id, connector->modes[0].hdisplay, connector->modes[0].vdisplay);
                    return connector;
                }
                /* Skip Qualcomm dummy virtual panel (Connector 29 / DSI-1) */
                if (connector->connector_id == 29) {
                    printf("minui: skipping dummy virtual connector %d (DSI-1)\n", connector->connector_id);
                    drmModeFreeConnector(connector);
                    continue;
                }
                return connector;
            }
            drmModeFreeConnector(connector);
        }
    }
    return NULL;
}

static drmModeConnector *find_first_connected_connector(int fd, drmModeRes *resources) {
    int i;
    for (i = 0; i < resources->count_connectors; i++) {
        drmModeConnector *connector = drmModeGetConnector(fd, resources->connectors[i]);
        if (connector) {
            if ((connector->count_modes > 0) && (connector->connection == DRM_MODE_CONNECTED)) {
                if (connector->connector_id == 46 ||
                    (connector->modes[0].hdisplay == 301 && connector->modes[0].vdisplay == 769)) {
                    printf("minui: selected physical EPDC connector %d (timing %dx%d)\n",
                           connector->connector_id, connector->modes[0].hdisplay, connector->modes[0].vdisplay);
                    return connector;
                }
                if (connector->connector_id == 29) {
                    printf("minui: skipping dummy virtual connector %d (DSI-1)\n", connector->connector_id);
                    drmModeFreeConnector(connector);
                    continue;
                }
                return connector;
            }
            drmModeFreeConnector(connector);
        }
    }
    return NULL;
}

static drmModeConnector *find_main_monitor(int fd, drmModeRes *resources, uint32_t *mode_index) {
    unsigned i = 0;
    int modes;
    unsigned kConnectorPriority[] = {
        DRM_MODE_CONNECTOR_DSI,
        DRM_MODE_CONNECTOR_eDP,
        DRM_MODE_CONNECTOR_LVDS,
    };

    drmModeConnector *main_monitor = NULL;
    do {
        main_monitor = find_used_connector_by_type(fd, resources, kConnectorPriority[i]);
        i++;
    } while (!main_monitor && i < ARRAY_SIZE(kConnectorPriority));

    if (!main_monitor)
        main_monitor = find_first_connected_connector(fd, resources);

    if (!main_monitor)
        return NULL;

    *mode_index = 0;
    for (modes = 0; modes < main_monitor->count_modes; modes++) {
        if (main_monitor->modes[modes].type & DRM_MODE_TYPE_PREFERRED) {
            *mode_index = modes;
            break;
        }
    }

    return main_monitor;
}

static void teardown_non_main_pipeline(int fd, drmModeRes *resources, drmModeCrtc *main_crtc, uint32_t main_conn_id, uint32_t main_plane_id) {
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (!req) return;

    /* 1. Disable all other connectors */
    for (int i = 0; i < resources->count_connectors; i++) {
        uint32_t conn_id = resources->connectors[i];
        if (conn_id != main_conn_id) {
            drmModeObjectProperties *props = drmModeObjectGetProperties(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR);
            if (props) {
                for (uint32_t j = 0; j < props->count_props; j++) {
                    drmModePropertyRes *pr = drmModeGetProperty(fd, props->props[j]);
                    if (!pr) continue;
                    if (!strcmp(pr->name, "CRTC_ID")) {
                        drmModeAtomicAddProperty(req, conn_id, pr->prop_id, 0);
                    }
                    drmModeFreeProperty(pr);
                }
                drmModeFreeObjectProperties(props);
            }
        }
    }

    /* 2. Disable all other CRTCs */
    for (int i = 0; i < resources->count_crtcs; i++) {
        uint32_t crtc_id = resources->crtcs[i];
        if (crtc_id != main_crtc->crtc_id) {
            drmModeObjectProperties *props = drmModeObjectGetProperties(fd, crtc_id, DRM_MODE_OBJECT_CRTC);
            if (props) {
                for (uint32_t j = 0; j < props->count_props; j++) {
                    drmModePropertyRes *pr = drmModeGetProperty(fd, props->props[j]);
                    if (!pr) continue;
                    if (!strcmp(pr->name, "ACTIVE")) {
                        drmModeAtomicAddProperty(req, crtc_id, pr->prop_id, 0);
                    } else if (!strcmp(pr->name, "MODE_ID")) {
                        drmModeAtomicAddProperty(req, crtc_id, pr->prop_id, 0);
                    }
                    drmModeFreeProperty(pr);
                }
                drmModeFreeObjectProperties(props);
            }
        }
    }

    /* 3. Detach all planes other than main_plane_id (clears continuous splash plane 49, etc.) */
    drmModePlaneRes *plane_res = drmModeGetPlaneResources(fd);
    if (plane_res) {
        for (uint32_t i = 0; i < plane_res->count_planes; i++) {
            uint32_t p_id = plane_res->planes[i];
            if (p_id != main_plane_id) {
                drmModeObjectProperties *props = drmModeObjectGetProperties(fd, p_id, DRM_MODE_OBJECT_PLANE);
                if (props) {
                    for (uint32_t j = 0; j < props->count_props; j++) {
                        drmModePropertyRes *pr = drmModeGetProperty(fd, props->props[j]);
                        if (!pr) continue;
                        if (!strcmp(pr->name, "CRTC_ID")) {
                            drmModeAtomicAddProperty(req, p_id, pr->prop_id, 0);
                        } else if (!strcmp(pr->name, "FB_ID")) {
                            drmModeAtomicAddProperty(req, p_id, pr->prop_id, 0);
                        }
                        drmModeFreeProperty(pr);
                    }
                    drmModeFreeObjectProperties(props);
                }
            }
        }
        drmModeFreePlaneResources(plane_res);
    }

    int ret = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
    printf("minui: teardown non-main pipeline atomic commit returned %d\n", ret);
    drmModeAtomicFree(req);
}

/*
 * Discover and cache Qualcomm Atomic DRM Plane, CRTC, and EPDC properties.
 */
static void init_atomic_properties(int fd, drmModeCrtc *crtc, drmModeConnector *conn) {
    drmModePlaneRes *plane_res = drmModeGetPlaneResources(fd);
    if (!plane_res) {
        printf("minui: drmModeGetPlaneResources returned NULL; atomic disabled\n");
        return;
    }

    uint32_t crtc_mask = 0;
    drmModeRes *res = drmModeGetResources(fd);
    if (res) {
        for (int i = 0; i < res->count_crtcs; i++) {
            if (res->crtcs[i] == crtc->crtc_id) {
                crtc_mask = (1 << i);
                break;
            }
        }
        drmModeFreeResources(res);
    }

    /* Find matching primary plane for this CRTC */
    for (uint32_t i = 0; i < plane_res->count_planes; i++) {
        drmModePlane *plane = drmModeGetPlane(fd, plane_res->planes[i]);
        if (!plane) continue;

        if (crtc_mask && !(plane->possible_crtcs & crtc_mask)) {
            drmModeFreePlane(plane);
            continue;
        }

        g_plane_id = plane->plane_id;
        drmModeObjectProperties *props = drmModeObjectGetProperties(fd, g_plane_id, DRM_MODE_OBJECT_PLANE);
        if (props) {
            for (uint32_t j = 0; j < props->count_props; j++) {
                drmModePropertyRes *pr = drmModeGetProperty(fd, props->props[j]);
                if (!pr) continue;

                if (!strcmp(pr->name, "FB_ID"))
                    prop_plane_fb_id = pr->prop_id;
                else if (!strcmp(pr->name, "CRTC_ID"))
                    prop_plane_crtc_id = pr->prop_id;
                else if (!strcmp(pr->name, "CRTC_X"))
                    prop_plane_crtc_x = pr->prop_id;
                else if (!strcmp(pr->name, "CRTC_Y"))
                    prop_plane_crtc_y = pr->prop_id;
                else if (!strcmp(pr->name, "CRTC_W"))
                    prop_plane_crtc_w = pr->prop_id;
                else if (!strcmp(pr->name, "CRTC_H"))
                    prop_plane_crtc_h = pr->prop_id;
                else if (!strcmp(pr->name, "SRC_X"))
                    prop_plane_src_x = pr->prop_id;
                else if (!strcmp(pr->name, "SRC_Y"))
                    prop_plane_src_y = pr->prop_id;
                else if (!strcmp(pr->name, "SRC_W"))
                    prop_plane_src_w = pr->prop_id;
                else if (!strcmp(pr->name, "SRC_H"))
                    prop_plane_src_h = pr->prop_id;
                else if (!strcmp(pr->name, "zpos"))
                    prop_plane_zpos = pr->prop_id;
                else if (!strcmp(pr->name, "EPDC_UPDATE_PARMS_ADDR"))
                    prop_epdc_parms_addr = pr->prop_id;
                else if (!strcmp(pr->name, "EPDC_UPDATE_CNT"))
                    prop_epdc_cnt = pr->prop_id;

                drmModeFreeProperty(pr);
            }
            drmModeFreeObjectProperties(props);
        }

        drmModeFreePlane(plane);

        /* Stop once we find the EPDC plane or any plane that has FB_ID */
        if (prop_epdc_parms_addr && prop_epdc_cnt) {
            printf("minui: Found Qualcomm EPDC Plane %u (PARMS_ADDR=%u, CNT=%u)\n",
                   g_plane_id, prop_epdc_parms_addr, prop_epdc_cnt);
            break;
        }
    }
    drmModeFreePlaneResources(plane_res);

    /* Fetch CRTC Properties */
    drmModeObjectProperties *crtc_props = drmModeObjectGetProperties(fd, crtc->crtc_id, DRM_MODE_OBJECT_CRTC);
    if (crtc_props) {
        for (uint32_t j = 0; j < crtc_props->count_props; j++) {
            drmModePropertyRes *pr = drmModeGetProperty(fd, crtc_props->props[j]);
            if (!pr) continue;
            if (!strcmp(pr->name, "ACTIVE"))
                prop_crtc_active = pr->prop_id;
            else if (!strcmp(pr->name, "MODE_ID"))
                prop_crtc_mode_id = pr->prop_id;
            drmModeFreeProperty(pr);
        }
        drmModeFreeObjectProperties(crtc_props);
    }

    /* Fetch Connector Properties */
    drmModeObjectProperties *conn_props = drmModeObjectGetProperties(fd, conn->connector_id, DRM_MODE_OBJECT_CONNECTOR);
    if (conn_props) {
        for (uint32_t j = 0; j < conn_props->count_props; j++) {
            drmModePropertyRes *pr = drmModeGetProperty(fd, conn_props->props[j]);
            if (!pr) continue;
            if (!strcmp(pr->name, "CRTC_ID"))
                prop_conn_crtc_id = pr->prop_id;
            drmModeFreeProperty(pr);
        }
        drmModeFreeObjectProperties(conn_props);
    }

    /* Create mode blob */
    if (prop_crtc_mode_id) {
        int ret = drmModeCreatePropertyBlob(fd, &crtc->mode, sizeof(drmModeModeInfo), &g_mode_blob_id);
        if (ret)
            printf("minui: drmModeCreatePropertyBlob failed ret=%d\n", ret);
    }

    if (g_plane_id && prop_plane_fb_id && prop_plane_crtc_id) {
        g_has_atomic = true;
        printf("minui: Atomic modeset support initialized successfully.\n");
    } else {
        printf("minui: Atomic prerequisites missing (plane=%u, fb_id=%u, crtc_id=%u)\n",
               g_plane_id, prop_plane_fb_id, prop_plane_crtc_id);
    }
}

static GRSurface* drm_init(minui_backend* backend __unused) {
    drmModeRes *res = NULL;
    uint32_t selected_mode;
    char *dev_name;
    int ret, i;

    for (i = 0; i < DRM_MAX_MINOR; i++) {
        uint64_t cap = 0;

        ret = asprintf(&dev_name, DRM_DEV_NAME, DRM_DIR_NAME, i);
        if (ret < 0) continue;

        drm_fd = open(dev_name, O_RDWR, 0);
        free(dev_name);
        if (drm_fd < 0) continue;

        /* Enable Universal Planes & Atomic capabilities early */
        drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
        drmSetClientCap(drm_fd, DRM_CLIENT_CAP_ATOMIC, 1);

        ret = drmGetCap(drm_fd, DRM_CAP_DUMB_BUFFER, &cap);
        if (ret || cap == 0) {
            close(drm_fd);
            continue;
        }

        res = drmModeGetResources(drm_fd);
        if (!res) {
            close(drm_fd);
            continue;
        }

        if (res->count_crtcs > 0 && res->count_connectors > 0)
            if (find_first_connected_connector(drm_fd, res))
                break;

        drmModeFreeResources(res);
        close(drm_fd);
        res = NULL;
    }

    if (drm_fd < 0 || res == NULL) {
        perror("minui: cannot find/open a usable drm device");
        return NULL;
    }

    main_monitor_connector = find_main_monitor(drm_fd, res, &selected_mode);
    if (!main_monitor_connector) {
        printf("minui: main_monitor_connector not found\n");
        drmModeFreeResources(res);
        close(drm_fd);
        return NULL;
    }

    main_monitor_crtc = find_crtc_for_connector(drm_fd, res, main_monitor_connector);
    if (!main_monitor_crtc) {
        printf("minui: main_monitor_crtc not found\n");
        drmModeFreeResources(res);
        close(drm_fd);
        return NULL;
    }

    main_monitor_crtc->mode = main_monitor_connector->modes[selected_mode];

    /*
     * For Onyx Boox C69HD (Qualcomm SM6225 Bengal):
     * The physical EPDC timing over DSI is hactive x vactive (301x769 @ 90Hz).
     * The Qualcomm SDE kernel driver validates crtc_w <= mode.hdisplay and
     * rejects any atomic commit exceeding hdisplay with -E2BIG (-7).
     * Matching stock librecovery_ui.so, we set screen_width and screen_height
     * directly from the CRTC display timing mode.
     */
    screen_width = main_monitor_crtc->mode.hdisplay;
    screen_height = main_monitor_crtc->mode.vdisplay;
    printf("minui: Physical display resolution: %dx%d @ %d Hz\n",
           screen_width, screen_height, main_monitor_crtc->mode.vrefresh);

    /* Initialize Atomic Properties & discover primary EPDC plane (g_plane_id) */
    init_atomic_properties(drm_fd, main_monitor_crtc, main_monitor_connector);

    /* Teardown non-main pipeline (disable secondary CRTCs/connectors & detach Plane 49) */
    teardown_non_main_pipeline(drm_fd, res, main_monitor_crtc, main_monitor_connector->connector_id, g_plane_id);

    drmModeFreeResources(res);

    drm_surfaces[0] = drm_create_surface(screen_width, screen_height);
    drm_surfaces[1] = drm_create_surface(screen_width, screen_height);
    if (!drm_surfaces[0] || !drm_surfaces[1]) {
        drm_destroy_surface(drm_surfaces[0]);
        drm_destroy_surface(drm_surfaces[1]);
        close(drm_fd);
        return NULL;
    }

    draw_buf = (GRSurface *)malloc(sizeof(GRSurface));
    if (!draw_buf) {
        printf("minui: failed to alloc draw_buf\n");
        drm_destroy_surface(drm_surfaces[0]);
        drm_destroy_surface(drm_surfaces[1]);
        close(drm_fd);
        return NULL;
    }

    memcpy(draw_buf, &drm_surfaces[0]->base, sizeof(GRSurface));
    draw_buf->data = (unsigned char *)calloc(draw_buf->height * draw_buf->row_bytes, 1);
    if (!draw_buf->data) {
        printf("minui: failed to alloc draw_buf surface data\n");
        free(draw_buf);
        drm_destroy_surface(drm_surfaces[0]);
        drm_destroy_surface(drm_surfaces[1]);
        close(drm_fd);
        return NULL;
    }

    current_buffer = 0;

    /* Perform initial modeset */
    if (g_has_atomic) {
        drmModeAtomicReq *req = drmModeAtomicAlloc();
        if (req) {
            /* Detach all other planes (continuous splash plane 49, etc.) in this modeset commit */
            drmModePlaneRes *pres = drmModeGetPlaneResources(drm_fd);
            if (pres) {
                for (uint32_t i = 0; i < pres->count_planes; i++) {
                    uint32_t pid = pres->planes[i];
                    if (pid != g_plane_id) {
                        drmModeObjectProperties *props = drmModeObjectGetProperties(drm_fd, pid, DRM_MODE_OBJECT_PLANE);
                        if (props) {
                            for (uint32_t j = 0; j < props->count_props; j++) {
                                drmModePropertyRes *pr = drmModeGetProperty(drm_fd, props->props[j]);
                                if (!pr) continue;
                                if (!strcmp(pr->name, "CRTC_ID")) {
                                    drmModeAtomicAddProperty(req, pid, pr->prop_id, 0);
                                } else if (!strcmp(pr->name, "FB_ID")) {
                                    drmModeAtomicAddProperty(req, pid, pr->prop_id, 0);
                                }
                                drmModeFreeProperty(pr);
                            }
                            drmModeFreeObjectProperties(props);
                        }
                    }
                }
                drmModeFreePlaneResources(pres);
            }

            if (prop_crtc_active)
                drmModeAtomicAddProperty(req, main_monitor_crtc->crtc_id, prop_crtc_active, 1);
            if (prop_crtc_mode_id && g_mode_blob_id)
                drmModeAtomicAddProperty(req, main_monitor_crtc->crtc_id, prop_crtc_mode_id, g_mode_blob_id);
            if (prop_conn_crtc_id)
                drmModeAtomicAddProperty(req, main_monitor_connector->connector_id, prop_conn_crtc_id, main_monitor_crtc->crtc_id);

            drmModeAtomicAddProperty(req, g_plane_id, prop_plane_fb_id, drm_surfaces[1]->fb_id);
            drmModeAtomicAddProperty(req, g_plane_id, prop_plane_crtc_id, main_monitor_crtc->crtc_id);
            if (prop_plane_zpos) drmModeAtomicAddProperty(req, g_plane_id, prop_plane_zpos, 0);
            if (prop_plane_src_x) drmModeAtomicAddProperty(req, g_plane_id, prop_plane_src_x, 0);
            if (prop_plane_src_y) drmModeAtomicAddProperty(req, g_plane_id, prop_plane_src_y, 0);
            if (prop_plane_src_w) drmModeAtomicAddProperty(req, g_plane_id, prop_plane_src_w, ((uint64_t)screen_width) << 16);
            if (prop_plane_src_h) drmModeAtomicAddProperty(req, g_plane_id, prop_plane_src_h, ((uint64_t)screen_height) << 16);
            if (prop_plane_crtc_x) drmModeAtomicAddProperty(req, g_plane_id, prop_plane_crtc_x, 0);
            if (prop_plane_crtc_y) drmModeAtomicAddProperty(req, g_plane_id, prop_plane_crtc_y, 0);
            if (prop_plane_crtc_w) drmModeAtomicAddProperty(req, g_plane_id, prop_plane_crtc_w, screen_width);
            if (prop_plane_crtc_h) drmModeAtomicAddProperty(req, g_plane_id, prop_plane_crtc_h, screen_height);

            if (prop_epdc_parms_addr && prop_epdc_cnt) {
                memset(g_epdc_parms, 0, sizeof(g_epdc_parms));
                g_epdc_parms[0].rect[0] = 0;
                g_epdc_parms[0].rect[1] = 0;
                g_epdc_parms[0].rect[2] = screen_width;
                g_epdc_parms[0].rect[3] = screen_height;
                g_epdc_parms[0].waveform_mode = 2; // GC16 full refresh on boot
                g_epdc_parms[0].update_mode = 0;
                g_epdc_parms[0].update_marker = ++g_update_marker;
                g_epdc_parms[0].temp = 24; // 24 C ambient temperature index
                g_epdc_parms[0].flag = 0;

                drmModeAtomicAddProperty(req, g_plane_id, prop_epdc_parms_addr, (uint64_t)(uintptr_t)&g_epdc_parms[0]);
                drmModeAtomicAddProperty(req, g_plane_id, prop_epdc_cnt, 1);
            }

            int ret = drmModeAtomicCommit(drm_fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
            if (ret) {
                printf("minui: initial atomic commit returned %d, attempting legacy drmModeSetCrtc fallback\n", ret);
                drm_enable_crtc(drm_fd, main_monitor_crtc, drm_surfaces[1]);
            } else {
                printf("minui: initial atomic commit with EPDC refresh successful!\n");
            }
            drmModeAtomicFree(req);
        }
    } else {
        drm_enable_crtc(drm_fd, main_monitor_crtc, drm_surfaces[1]);
    }

    return draw_buf;
}

static GRSurface* drm_flip(minui_backend* backend __unused) {
    memcpy(drm_surfaces[current_buffer]->base.data,
           draw_buf->data,
           draw_buf->height * draw_buf->row_bytes);

    bool commit_ok = false;

    if (g_has_atomic) {
        drmModeAtomicReq *req = drmModeAtomicAlloc();
        if (req) {
            drmModeAtomicAddProperty(req, g_plane_id, prop_plane_fb_id, drm_surfaces[current_buffer]->fb_id);
            drmModeAtomicAddProperty(req, g_plane_id, prop_plane_crtc_id, main_monitor_crtc->crtc_id);
            if (prop_plane_zpos) drmModeAtomicAddProperty(req, g_plane_id, prop_plane_zpos, 0);
            if (prop_plane_src_x) drmModeAtomicAddProperty(req, g_plane_id, prop_plane_src_x, 0);
            if (prop_plane_src_y) drmModeAtomicAddProperty(req, g_plane_id, prop_plane_src_y, 0);
            if (prop_plane_src_w) drmModeAtomicAddProperty(req, g_plane_id, prop_plane_src_w, ((uint64_t)screen_width) << 16);
            if (prop_plane_src_h) drmModeAtomicAddProperty(req, g_plane_id, prop_plane_src_h, ((uint64_t)screen_height) << 16);
            if (prop_plane_crtc_x) drmModeAtomicAddProperty(req, g_plane_id, prop_plane_crtc_x, 0);
            if (prop_plane_crtc_y) drmModeAtomicAddProperty(req, g_plane_id, prop_plane_crtc_y, 0);
            if (prop_plane_crtc_w) drmModeAtomicAddProperty(req, g_plane_id, prop_plane_crtc_w, screen_width);
            if (prop_plane_crtc_h) drmModeAtomicAddProperty(req, g_plane_id, prop_plane_crtc_h, screen_height);

            /* Inject Qualcomm EPDC update waveform parameters */
            if (prop_epdc_parms_addr && prop_epdc_cnt) {
                memset(g_epdc_parms, 0, sizeof(g_epdc_parms));
                g_epdc_parms[0].rect[0] = 0;
                g_epdc_parms[0].rect[1] = 0;
                g_epdc_parms[0].rect[2] = screen_width;
                g_epdc_parms[0].rect[3] = screen_height;
                g_epdc_parms[0].waveform_mode = 2; // GC16 (matching stock librecovery_ui.so)
                g_epdc_parms[0].update_mode = 0;
                g_epdc_parms[0].update_marker = ++g_update_marker;
                g_epdc_parms[0].temp = 24; // 24 C ambient temperature index
                g_epdc_parms[0].flag = 0;

                drmModeAtomicAddProperty(req, g_plane_id, prop_epdc_parms_addr, (uint64_t)(uintptr_t)&g_epdc_parms[0]);
                drmModeAtomicAddProperty(req, g_plane_id, prop_epdc_cnt, 1);
            }

            int ret = drmModeAtomicCommit(drm_fd, req, 0, NULL);
            if (ret != 0) {
                /* Retry with ALLOW_MODESET if non-modeset atomic flip rejected */
                ret = drmModeAtomicCommit(drm_fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
            }

            if (ret == 0) {
                commit_ok = true;
            } else {
                printf("minui: drmModeAtomicCommit flip returned error %d\n", ret);
            }

            drmModeAtomicFree(req);
        }
    }

    if (!commit_ok) {
        int ret = drmModePageFlip(drm_fd,
                                  main_monitor_crtc->crtc_id,
                                  drm_surfaces[current_buffer]->fb_id,
                                  0,
                                  NULL);
        if (ret < 0) {
            drmModeSetCrtc(drm_fd, main_monitor_crtc->crtc_id,
                           drm_surfaces[current_buffer]->fb_id,
                           0, 0,
                           &main_monitor_connector->connector_id,
                           1,
                           &main_monitor_crtc->mode);
        }
    }

    current_buffer = 1 - current_buffer;
    return draw_buf;
}

static void drm_blank(minui_backend* backend __unused, bool blank) {
    if (blank) {
        if (g_has_atomic && prop_crtc_active) {
            drmModeAtomicReq *req = drmModeAtomicAlloc();
            if (req) {
                drmModeAtomicAddProperty(req, main_monitor_crtc->crtc_id, prop_crtc_active, 0);
                drmModeAtomicCommit(drm_fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
                drmModeAtomicFree(req);
            }
        } else {
            drm_disable_crtc(drm_fd, main_monitor_crtc);
        }
    } else {
        drm_flip(backend);
    }
}

static void drm_exit(minui_backend* backend __unused) {
    drm_blank(backend, true);
    drm_destroy_surface(drm_surfaces[0]);
    drm_destroy_surface(drm_surfaces[1]);
    if (g_mode_blob_id) {
        drmModeDestroyPropertyBlob(drm_fd, g_mode_blob_id);
        g_mode_blob_id = 0;
    }
    if (main_monitor_crtc) {
        drmModeFreeCrtc(main_monitor_crtc);
        main_monitor_crtc = NULL;
    }
    if (main_monitor_connector) {
        drmModeFreeConnector(main_monitor_connector);
        main_monitor_connector = NULL;
    }
    if (drm_fd >= 0) {
        close(drm_fd);
        drm_fd = -1;
    }
}

static minui_backend drm_backend = {
    .init = drm_init,
    .flip = drm_flip,
    .blank = drm_blank,
    .exit = drm_exit,
};

minui_backend* open_drm() {
    return &drm_backend;
}
