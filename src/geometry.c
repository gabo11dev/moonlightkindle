#include "geometry.h"
Viewport viewport_fit(int aw, int ah, int vw, int vh) {
    Viewport v = {0};
    if (aw <= 0 || ah <= 0 || vw <= 0 || vh <= 0) return v;
    v.width = aw; v.height = (int)((long long)aw * vh / vw);
    if (v.height > ah) { v.height = ah; v.width = (int)((long long)ah * vw / vh); }
    if (v.width < 1) v.width = 1;
    if (v.height < 1) v.height = 1;
    v.x = (aw - v.width) / 2; v.y = (ah - v.height) / 2;
    return v;
}
bool viewport_point(Viewport v, double x, double y, int *ox, int *oy) {
    if (v.width < 1 || v.height < 1 || x < v.x || y < v.y ||
        x >= v.x + v.width || y >= v.y + v.height) return false;
    *ox = (int)(x - v.x); *oy = (int)(y - v.y);
    return true;
}
