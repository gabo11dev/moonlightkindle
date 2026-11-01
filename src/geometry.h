#pragma once
#include <stdbool.h>
typedef struct { int x, y, width, height; } Viewport;
Viewport viewport_fit(int area_w, int area_h, int video_w, int video_h);
bool viewport_point(Viewport v, double x, double y, int *out_x, int *out_y);
