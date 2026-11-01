/* FBInk owns device-specific EPDC ioctls. SPDX-License-Identifier: GPL-3.0-or-later */
#include "eink.h"
#include "fbink.h"
#include <dlfcn.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
static void *lib;
static int fd = -1;
static int (*refresh_fn)(int, uint32_t, uint32_t, uint32_t, uint32_t, const FBInkConfig *);
static int (*close_fn)(int);
static int (*wait_fn)(int, uint32_t);
static int (*init_fn)(int, const FBInkConfig *);
static FBInkConfig cfg;
int eink_init(int required) {
    if (!required) return 0;
    lib = dlopen("libfbink.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) lib = dlopen("libfbink.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "FBInk unavailable: %s\n", dlerror()); return -1; }
    int (*open_fn)(void) = dlsym(lib, "fbink_open");
    init_fn = dlsym(lib, "fbink_init");
    wait_fn = dlsym(lib, "fbink_wait_for_complete");
    refresh_fn = dlsym(lib, "fbink_refresh"); close_fn = dlsym(lib, "fbink_close");
    memset(&cfg, 0, sizeof(cfg)); cfg.is_quiet = true; cfg.wfm_mode = WFM_DU4;
    if (!open_fn || !init_fn || !refresh_fn || !close_fn) { eink_close(); return -1; }
    fd = open_fn();
    if (fd < 0 || init_fn(fd, &cfg) < 0) { eink_close(); return -1; }
    return 0;
}
int eink_reinit(void) {
    return fd >= 0 && init_fn ? init_fn(fd, &cfg) : 0;
}
int eink_refresh(int full) {
    if (fd < 0) return 0;
    cfg.is_flashing = full; cfg.wfm_mode = full ? WFM_GC16 : WFM_DU4;
    /* Fence flashing refreshes so surrounding fast video updates cannot merge
     * into the clearing pass. Never wait for each normal video frame. */
    if (full && wait_fn) wait_fn(fd, LAST_MARKER);
    int result = refresh_fn(fd, 0, 0, 0, 0, &cfg);
    if (full && result >= 0 && wait_fn) {
        int waited = wait_fn(fd, LAST_MARKER);
        if (waited < 0) return waited;
    }
    return result;
}
int eink_clear(void) {
    if (fd < 0) return 0; /* Desktop preview must not run a device command. */
    if (wait_fn) wait_fn(fd, LAST_MARKER);
    /* Fixed command: no user input or host address is interpolated. */
    return system("eips -c > /dev/null 2>&1") == 0 ? 0 : -1;
}
void eink_close(void) {
    if (fd >= 0 && close_fn) close_fn(fd);
    fd = -1; if (lib) dlclose(lib); lib = NULL;
}
