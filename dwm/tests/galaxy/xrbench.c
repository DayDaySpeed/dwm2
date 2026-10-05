/* XRender 基准: 在离屏 Pixmap 上测 galaxy 用到的几种合成路径的真实耗时 (不创建窗口, 屏幕上看不到任何东西).
 * 每项前后读回 1 个像素作为 GPU 栅栏, 否则 NVIDIA 这类异步驱动的 XSync 不等绘制完成.
 *   cc -O2 -o xrbench xrbench.c -lX11 -lXrender -lm && DISPLAY=:0 ./xrbench
 * 2026-10 在 NVIDIA 专有驱动上的结论: sprite / 卡片 (仿射或透视) / 整屏合成都 < 0.3ms;
 * XRenderCompositeTriangles 约 7µs 一个三角形 (CPU 栅格化, 大三角形还按面积计费), 所以光带改成了进程内画布 */
#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static Display *dpy;
static Window root;
static Pixmap backpix;
static XRenderPictFormat *argb, *a8, *vis;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void fence(void) { XImage *im = XGetImage(dpy, backpix, 0, 0, 1, 1, AllPlanes, ZPixmap); if (im) XDestroyImage(im); }

#define BENCH(name, reps, ...) do { \
        double t0, best = 1e9, sum = 0; int rep_; \
        for (rep_ = 0; rep_ < (reps); rep_++) { fence(); t0 = now(); __VA_ARGS__; fence(); \
            double dt = now() - t0; sum += dt; if (dt < best) best = dt; } \
        printf("%-56s avg %7.2fms  best %7.2fms\n", name, 1000 * sum / (reps), 1000 * best); fflush(stdout); \
    } while (0)

static Picture
sprite(int n)
{
    Pixmap pix = XCreatePixmap(dpy, root, n, n, 32);
    GC gc = XCreateGC(dpy, pix, 0, NULL);
    unsigned int *data = malloc(n * n * 4);
    XImage *img;
    Picture p;
    int i, j;

    for (j = 0; j < n; j++)
        for (i = 0; i < n; i++) {
            double x = (i + .5) / n * 2 - 1, y = (j + .5) / n * 2 - 1;
            unsigned int a = (unsigned int)(255 * exp(-(x * x + y * y) / .15) + .5);
            data[j * n + i] = a << 24 | a << 16 | a << 8 | a;
        }
    img = XCreateImage(dpy, DefaultVisual(dpy, DefaultScreen(dpy)), 32, ZPixmap, 0, (char *)data, n, n, 32, 0);
    XPutImage(dpy, pix, gc, img, 0, 0, 0, 0, n, n);
    XDestroyImage(img);
    XFreeGC(dpy, gc);
    p = XRenderCreatePicture(dpy, pix, argb, 0, NULL);
    XRenderSetPictureFilter(dpy, p, FilterBilinear, NULL, 0);
    return p;
}

static void
scale(Picture p, double s)
{
    XTransform tr = {{{XDoubleToFixed(s), 0, 0}, {0, XDoubleToFixed(s), 0}, {0, 0, XDoubleToFixed(1)}}};
    XRenderSetPictureTransform(dpy, p, &tr);
}

int
main(void)
{
    int W = 2560, H = 1440, i, k, depth;
    Picture back, full, spr, card, half, white, mk;
    Pixmap mp;
    GC g8;
    XImage *im;
    XRenderColor wc = {0xffff, 0xffff, 0xffff, 0xffff}, hc = {0x4000, 0x4000, 0x4000, 0x4000};
    XTriangle tri[600], q2[2];
    XTransform persp;

    if (!(dpy = XOpenDisplay(NULL)))
        return 1;
    root = DefaultRootWindow(dpy);
    depth = DefaultDepth(dpy, DefaultScreen(dpy));
    argb = XRenderFindStandardFormat(dpy, PictStandardARGB32);
    a8 = XRenderFindStandardFormat(dpy, PictStandardA8);
    vis = XRenderFindVisualFormat(dpy, DefaultVisual(dpy, DefaultScreen(dpy)));
    backpix = XCreatePixmap(dpy, root, W, H, depth);
    back = XRenderCreatePicture(dpy, backpix, vis, 0, NULL);
    full = XRenderCreatePicture(dpy, XCreatePixmap(dpy, root, W, H, depth), vis, 0, NULL);
    white = XRenderCreateSolidFill(dpy, &wc);
    half = XRenderCreateSolidFill(dpy, &hc);
    spr = sprite(128);
    card = XRenderCreatePicture(dpy, XCreatePixmap(dpy, root, 1268, 688, 32), argb, 0, NULL);
    XRenderFillRectangle(dpy, PictOpSrc, card, &(XRenderColor){0x3000, 0x6000, 0x9000, 0xffff}, 0, 0, 1268, 688);
    XRenderSetPictureFilter(dpy, card, FilterBilinear, NULL, 0);
    printf("server: %s %d\n", ServerVendor(dpy), VendorRelease(dpy));

    BENCH("fence only (1px readback)", 20, ;);
    BENCH("fullscreen Src copy", 20, XRenderComposite(dpy, PictOpSrc, full, None, back, 0, 0, 0, 0, 0, 0, W, H));
    BENCH("sprite 128 -> 1100px, bilinear, Over + alpha mask", 20,
        scale(spr, 128.0 / 1100); XRenderComposite(dpy, PictOpOver, spr, half, back, 0, 0, 0, 0, 700, 100, 1100, 1100));
    BENCH("20x sprite 128 -> 140px", 20,
        for (i = 0; i < 20; i++) { scale(spr, 128.0 / 140); XRenderComposite(dpy, PictOpOver, spr, half, back, 0, 0, 0, 0, 100 * i, 300, 140, 140); });
    for (i = 0; i < 300; i++) {   /* 大圆上的 300 段细带 (600 个三角形), 包围盒约整屏 */
        double a0 = 2 * M_PI * i / 300, a1 = 2 * M_PI * (i + 1) / 300;
        double x0 = 1280 + 1100 * cos(a0), y0 = 720 + 650 * sin(a0), x1 = 1280 + 1100 * cos(a1), y1 = 720 + 650 * sin(a1);
        double len = hypot(x1 - x0, y1 - y0), nx = -(y1 - y0) / len * 1.5, ny = (x1 - x0) / len * 1.5;
        tri[2 * i] = (XTriangle){{XDoubleToFixed(x0 + nx), XDoubleToFixed(y0 + ny)}, {XDoubleToFixed(x0 - nx), XDoubleToFixed(y0 - ny)},
                                 {XDoubleToFixed(x1 - nx), XDoubleToFixed(y1 - ny)}};
        tri[2 * i + 1] = (XTriangle){tri[2 * i].p1, tri[2 * i].p3, {XDoubleToFixed(x1 + nx), XDoubleToFixed(y1 + ny)}};
    }
    BENCH("triangles: 600 thin, 1 call (a8)", 20, XRenderCompositeTriangles(dpy, PictOpOver, half, back, a8, 0, 0, tri, 600));
    BENCH("triangles: same 600 in 30 calls", 20,
        for (k = 0; k < 30; k++) XRenderCompositeTriangles(dpy, PictOpOver, half, back, a8, 0, 0, tri + 20 * k, 20));
    BENCH("8 x 600 thin triangles (old orbit bands)", 5,
        for (k = 0; k < 8; k++) XRenderCompositeTriangles(dpy, PictOpOver, half, back, a8, 0, 0, tri, 600));
    q2[0] = (XTriangle){{XDoubleToFixed(800), XDoubleToFixed(400)}, {XDoubleToFixed(1500), XDoubleToFixed(430)}, {XDoubleToFixed(1480), XDoubleToFixed(800)}};
    q2[1] = (XTriangle){q2[0].p1, q2[0].p3, {XDoubleToFixed(790), XDoubleToFixed(770)}};
    BENCH("tint quad 700x380 (2 big triangles)", 20, XRenderCompositeTriangles(dpy, PictOpOver, half, back, a8, 0, 0, q2, 2));
    BENCH("tint 700x380 as solid rect", 20, XRenderComposite(dpy, PictOpOver, half, None, back, 0, 0, 0, 0, 800, 400, 700, 380));
    mp = XCreatePixmap(dpy, root, W, H, 8);
    mk = XRenderCreatePicture(dpy, mp, a8, 0, NULL);
    g8 = XCreateGC(dpy, mp, 0, NULL);
    im = XCreateImage(dpy, NULL, 8, ZPixmap, 0, calloc(128 * 128, 1), 128, 128, 8, 128);
    BENCH("240 tiles 128x128 (fullscreen): XPutImage a8 + composite", 10,
        for (k = 0; k < 240; k++) { int x = k % 20 * 128, y = k / 20 * 128;
            XPutImage(dpy, mp, g8, im, 0, 0, x, y, 128, 128);
            XRenderComposite(dpy, PictOpOver, white, mk, back, 0, 0, x, y, x, y, 128, 128); });
    BENCH("card 1268x688 -> 700px affine bilinear", 20,
        scale(card, 1268.0 / 700); XRenderComposite(dpy, PictOpOver, card, None, back, 0, 0, 0, 0, 800, 400, 700, 380));
    persp = (XTransform){{{XDoubleToFixed(1.8), XDoubleToFixed(.1), 0}, {XDoubleToFixed(.05), XDoubleToFixed(1.8), 0},
                          {XDoubleToFixed(.0004), XDoubleToFixed(.0002), XDoubleToFixed(1)}}};
    BENCH("card -> 700px projective bilinear", 20,
        XRenderSetPictureTransform(dpy, card, &persp); XRenderComposite(dpy, PictOpOver, card, None, back, 0, 0, 0, 0, 800, 400, 700, 380));
    return 0;
}
