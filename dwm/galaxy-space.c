/* Super+Z 星系: 深空背景. 由 galaxy.c 按顺序 include (在 galaxy-render.c 之前).
 *
 * 开场时壁纸溶解成程序生成的深空, 回程 / 坍缩时再溶解回去:
 *   - 3 层可平铺的星点图 (远 / 中 / 近, 尺寸各不相同), 随镜头转动按不同速度平移, 形成视差;
 *     星的亮度按幂律分布 (亮星少暗星多), 色温从冷蓝到暖黄, 最亮的自带十字衍射芒
 *   - 银河带 (沿轨道盘面的对角线方向, 中间一道暗尘带) 和 2 团星云: 低分辨率 a8 蒙版, 合成时用纯色上色并双线性放大
 *   - 星云颜色取自壁纸的主色调, 驻留时色调以 4 分钟为周期缓慢转动
 * 生成在 CPU 上做, 分几帧完成 (每帧一层), 结果按 屏幕尺寸 + 壁纸 缓存在进程里, 之后再按 Super+Z 直接复用 */

#define GALAXYSPACELAYERS 3
#define GALAXYSPACEMASKS  3           /* 0 银河, 1 / 2 星云 */
#define GALAXYSPACEMW     480         /* 蒙版宽度; 高度按视口比例 */

static Picture galaxyargb(int w, int h, Pixmap *pix);
static Picture galaxyopaque(int w, int h, Pixmap *pix);
static Picture galaxyupload(int w, int h, unsigned int *data, Pixmap *pix);
static void galaxyaffine(Picture p, double sx, double sy, double tx, double ty);
static Picture galaxywhite(double a);

/* 进程级缓存 (galaxyscene 每次启动都会清零, 这里不会) */
static struct {
    int ready, steps, w, h, vw, vh;
    unsigned long wall;               /* 生成时的壁纸 (_XROOTPMAP_ID), 变了就重新生成 */
    Pixmap starpix[GALAXYSPACELAYERS], maskpix[GALAXYSPACEMASKS];
    Picture star[GALAXYSPACELAYERS], mask[GALAXYSPACEMASKS];
    int starsize[GALAXYSPACELAYERS], mw, mh;
    Pixmap farpix;                    /* 远景合成图: 底色 + 星云 + 银河 + 最远一层星点, 比视口大 30% (视差平移时不露边) */
    Picture far;
    int fw, fh;
    double bakedrift, bakedat;        /* 上次合成远景图时的星云色调偏移 / 时刻 */
    double hue[2], sat[2];            /* 两团星云的色调 (度) 和饱和度, 取自壁纸 */
} galaxyspace;

static const int galaxyspacesize[GALAXYSPACELAYERS] = { 1024, 896, 768 };
static const int galaxyspacecount[GALAXYSPACELAYERS] = { 1500, 700, 260 };

/* 值噪声 + 分形叠加, 用于银河和星云 */
static double
galaxynoise(double x, double y, unsigned int seed)
{
    int xi = (int)floor(x), yi = (int)floor(y);
    double fx = x - xi, fy = y - yi, a, b, c, d;

    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    a = galaxyhash(seed + (unsigned int)(xi * 73856093 ^ yi * 19349663));
    b = galaxyhash(seed + (unsigned int)((xi + 1) * 73856093 ^ yi * 19349663));
    c = galaxyhash(seed + (unsigned int)(xi * 73856093 ^ (yi + 1) * 19349663));
    d = galaxyhash(seed + (unsigned int)((xi + 1) * 73856093 ^ (yi + 1) * 19349663));
    return galaxymix(galaxymix(a, b, fx), galaxymix(c, d, fx), fy);
}

static double
galaxyfbm(double x, double y, unsigned int seed)
{
    double v = 0, amp = .5;
    int o;

    for (o = 0; o < 5; o++, x *= 2.03, y *= 2.03, amp *= .5)
        v += amp * galaxynoise(x, y, seed + o * 1013);
    return v / .97;
}

/* 一层可平铺的星点图 (ARGB, 预乘) */
static void
galaxyspacestars(int layer)
{
    int n = galaxyspacesize[layer], count = galaxyspacecount[layer], i, dx, dy, ext, x, y;
    unsigned int *data = calloc((size_t)n * n, 4), seed = 9001 + layer * 7777;
    double px, py, b, rad, sig, t, col[3], a, d2, spike, near = layer / 2.0;
    unsigned int *p, v[4];

    if (!data)
        return;
    for (i = 0; i < count; i++) {
        px = galaxyhash(seed + i * 4) * n;
        py = galaxyhash(seed + i * 4 + 1) * n;
        b = pow(galaxyhash(seed + i * 4 + 2), 3.2);            /* 幂律: 绝大多数是暗星 */
        b = .18 + .82 * b;
        rad = (.55 + .9 * near) + 1.6 * pow(b, 1.6) * (.6 + .6 * near);
        sig = rad * .55;
        t = galaxyhash(seed + i * 4 + 3);                         /* 色温: 多数偏白, 少数偏蓝 / 偏黄 */
        col[0] = t < .2 ? .72 : t > .85 ? 1 : .95;
        col[1] = t < .2 ? .82 : t > .85 ? .86 : .95;
        col[2] = t < .2 ? 1 : t > .85 ? .66 : .98;
        spike = b > .92 ? 7 + 9 * (b - .92) / .08 : 0;           /* 最亮的星自带衍射芒 */
        ext = (int)ceil(MAX(3 * sig, spike));
        for (dy = -ext; dy <= ext; dy++)
            for (dx = -ext; dx <= ext; dx++) {
                d2 = (dx + .5 - (px - floor(px))) * (dx + .5 - (px - floor(px)))
                   + (dy + .5 - (py - floor(py))) * (dy + .5 - (py - floor(py)));
                a = b * exp(-d2 / (2 * sig * sig));
                if (spike > 0 && (dx == 0 || dy == 0))
                    a = MAX(a, b * .55 * pow(galaxyclamp(1 - (abs(dx) + abs(dy)) / spike), 2));
                if (a < 1 / 255.0)
                    continue;
                x = ((int)floor(px) + dx % n + n) % n;
                y = ((int)floor(py) + dy % n + n) % n;
                p = &data[(size_t)y * n + x];
                v[3] = MIN(255, (*p >> 24) + (unsigned int)(a * 255));
                v[0] = MIN(v[3], ((*p >> 16) & 255) + (unsigned int)(a * col[0] * 255));
                v[1] = MIN(v[3], ((*p >> 8) & 255) + (unsigned int)(a * col[1] * 255));
                v[2] = MIN(v[3], (*p & 255) + (unsigned int)(a * col[2] * 255));
                *p = v[3] << 24 | v[0] << 16 | v[1] << 8 | v[2];
            }
    }
    galaxyspace.starsize[layer] = n;
    galaxyspace.star[layer] = galaxyupload(n, n, data, &galaxyspace.starpix[layer]);
    if (galaxyspace.star[layer])
        XRenderChangePicture(dpy, galaxyspace.star[layer], CPRepeat, &(XRenderPictureAttributes){.repeat = RepeatNormal});
}

/* a8 蒙版: 0 = 银河带 (沿对角线, 中间暗尘带), 1 / 2 = 星云团 */
static void
galaxyspacemask(int which)
{
    GalaxyScene *r = &galaxyscene;
    int w = galaxyspace.mw, h = galaxyspace.mh, i, j, stride = (w + 3) & ~3;
    unsigned char *data = calloc((size_t)stride * h, 1);
    double ang = -GALAXY_DIAG * GALAXYPI / 180, ca = cos(ang), sa = sin(ang), x, y, u, v, a, n, k;
    XImage *img;
    GC gc;

    if (!data)
        return;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) {
            x = (i + .5) / w * 2 - 1;
            y = ((j + .5) / h * 2 - 1) * h / w;
            if (which == 0) {
                u = x * ca - y * sa;              /* 沿银河 */
                v = x * sa + y * ca;              /* 垂直银河 */
                n = galaxyfbm(u * 2.4 + 7, v * 6 + 3, 501);
                a = exp(-v * v / (.16 * .16)) * (.35 + .65 * n) * (.75 + .25 * galaxyfbm(u * 9, v * 9, 77));
                k = exp(-pow((v - .015 - .03 * (galaxyfbm(u * 3, 1, 9) - .5)) / .028, 2));   /* 暗尘带 */
                a *= 1 - .75 * k * (.5 + .5 * galaxyfbm(u * 7, v * 20, 31));
            } else {
                n = galaxyfbm(x * 1.7 + which * 4.3, y * 1.7 + which * 2.1, 1300 + which * 97);
                a = galaxysmoothstep((n - .5) / .32);
                k = which == 1 ? hypot(x + .45, y + .12) : hypot(x - .5, y - .18);   /* 两团各占一侧 */
                a *= 1 - galaxysmoothstep((k - .25) / .6);
            }
            data[(size_t)j * stride + i] = (unsigned char)(galaxyclamp(a) * 255 + .5);
        }
    galaxyspace.maskpix[which] = XCreatePixmap(dpy, root, w, h, 8);
    galaxyspace.mask[which] = XRenderCreatePicture(dpy, galaxyspace.maskpix[which], r->a8, 0, NULL);
    img = XCreateImage(dpy, DefaultVisual(dpy, screen), 8, ZPixmap, 0, (char *)data, w, h, 8, stride);
    if (!img) {
        free(data);
        return;
    }
    gc = XCreateGC(dpy, galaxyspace.maskpix[which], 0, NULL);
    XPutImage(dpy, galaxyspace.maskpix[which], gc, img, 0, 0, 0, 0, w, h);
    XFreeGC(dpy, gc);
    XDestroyImage(img);
    XRenderSetPictureFilter(dpy, galaxyspace.mask[which], FilterBilinear, NULL, 0);
}

/* 从壁纸取两种主色调: 缩到 32x18 读回, 按 饱和度 x 亮度 加权统计色相 */
static void
galaxyspacecolors(void)
{
    GalaxyScene *r = &galaxyscene;
    double hist[12] = {0}, rr, gg, bb, mx, mn, h, s, v, wsum[12] = {0}, ssum[12] = {0};
    int i, j, b1 = 7, b2 = 9;
    Pixmap pix;
    Picture pic;
    XImage *img;
    unsigned long px;

    galaxyspace.hue[0] = 220;
    galaxyspace.hue[1] = 280;
    galaxyspace.sat[0] = galaxyspace.sat[1] = .5;
    if (!r->wallpaper)
        return;
    pix = XCreatePixmap(dpy, root, 32, 18, DefaultDepth(dpy, screen));
    pic = XRenderCreatePicture(dpy, pix, XRenderFindVisualFormat(dpy, DefaultVisual(dpy, screen)), 0, NULL);
    galaxyaffine(r->wallpaper, (double)r->w / 32, (double)r->h / 18, 0, 0);
    XRenderComposite(dpy, PictOpSrc, r->wallpaper, None, pic, 0, 0, 0, 0, 0, 0, 32, 18);
    galaxyaffine(r->wallpaper, 1, 1, 0, 0);
    img = XGetImage(dpy, pix, 0, 0, 32, 18, AllPlanes, ZPixmap);
    XRenderFreePicture(dpy, pic);
    XFreePixmap(dpy, pix);
    if (!img)
        return;
    for (j = 0; j < 18; j++)
        for (i = 0; i < 32; i++) {
            px = XGetPixel(img, i, j);
            rr = (px >> 16 & 255) / 255.0;
            gg = (px >> 8 & 255) / 255.0;
            bb = (px & 255) / 255.0;
            mx = MAX(rr, MAX(gg, bb));
            mn = MIN(rr, MIN(gg, bb));
            if (mx - mn < .04)
                continue;
            h = mx == rr ? fmod((gg - bb) / (mx - mn) + 6, 6) : mx == gg ? (bb - rr) / (mx - mn) + 2 : (rr - gg) / (mx - mn) + 4;
            s = (mx - mn) / mx;
            v = mx;
            hist[(int)(h * 2) % 12] += s * v;
            wsum[(int)(h * 2) % 12] += 1;
            ssum[(int)(h * 2) % 12] += s;
        }
    XDestroyImage(img);
    for (i = 0; i < 12; i++)
        if (hist[i] > hist[b1])
            b1 = i;
    b2 = (b1 + 4) % 12;
    for (i = 0; i < 12; i++)
        if (abs(i - b1) > 1 && abs(i - b1) < 11 && hist[i] > hist[b2])
            b2 = i;
    if (hist[b2] < hist[b1] * .25)
        b2 = (b1 + 2) % 12;   /* 壁纸几乎单色: 第二团取相邻色调 */
    galaxyspace.hue[0] = b1 * 30 + 15;
    galaxyspace.hue[1] = b2 * 30 + 15;
    galaxyspace.sat[0] = wsum[b1] ? MAX(.35, MIN(.75, ssum[b1] / wsum[b1])) : .5;
    galaxyspace.sat[1] = wsum[b2] ? MAX(.35, MIN(.75, ssum[b2] / wsum[b2])) : .5;
}

static void
galaxyspacefree(void)
{
    int i;

    for (i = 0; i < GALAXYSPACELAYERS; i++) {
        if (galaxyspace.star[i]) XRenderFreePicture(dpy, galaxyspace.star[i]);
        if (galaxyspace.starpix[i]) XFreePixmap(dpy, galaxyspace.starpix[i]);
    }
    for (i = 0; i < GALAXYSPACEMASKS; i++) {
        if (galaxyspace.mask[i]) XRenderFreePicture(dpy, galaxyspace.mask[i]);
        if (galaxyspace.maskpix[i]) XFreePixmap(dpy, galaxyspace.maskpix[i]);
    }
    if (galaxyspace.far) XRenderFreePicture(dpy, galaxyspace.far);
    if (galaxyspace.farpix) XFreePixmap(dpy, galaxyspace.farpix);
    memset(&galaxyspace, 0, sizeof galaxyspace);
}

/* 星系启动时调用: 缓存仍然有效就直接用, 否则清掉, 在接下来的几帧里重新生成 */
static void
galaxyspacebegin(void)
{
    GalaxyScene *r = &galaxyscene;
    Atom type;
    int fmt;
    unsigned long n, after, wall = 0;
    unsigned char *p = NULL;

    if (XGetWindowProperty(dpy, root, XInternAtom(dpy, "_XROOTPMAP_ID", False), 0, 1, False, XA_PIXMAP,
                &type, &fmt, &n, &after, &p) == Success && p) {
        if (n)
            wall = *(unsigned long *)p;
        XFree(p);
    }
    if (galaxyspace.ready && galaxyspace.w == r->w && galaxyspace.h == r->h && galaxyspace.vw == r->vw
            && galaxyspace.vh == r->vh && galaxyspace.wall == wall)
        return;
    galaxyspacefree();
    galaxyspace.w = r->w;
    galaxyspace.h = r->h;
    galaxyspace.vw = r->vw;
    galaxyspace.vh = r->vh;
    galaxyspace.wall = wall;
    galaxyspace.mw = GALAXYSPACEMW;
    galaxyspace.mh = MAX(8, GALAXYSPACEMW * r->vh / MAX(1, r->vw));
}

/* 每帧调用一次: 还没生成完就做下一步 (每步约 5–15ms), 开场开头几帧之后再开始, 不碰起飞的那几帧 */
static void
galaxyspacestep(void)
{
    GalaxyScene *r = &galaxyscene;
    double t0;

    if (galaxyspace.ready || r->stage < .12)
        return;
    t0 = galaxynow();
    switch (galaxyspace.steps++) {
    case 0: galaxyspacecolors(); galaxyspacestars(0); break;
    case 1: galaxyspacestars(1); break;
    case 2: galaxyspacestars(2); break;
    case 3: galaxyspacemask(0); break;
    case 4: galaxyspacemask(1); break;
    case 5: galaxyspacemask(2); galaxyspace.ready = 1; break;
    }
    if (r->log && galaxyspace.steps == 1)
        fprintf(r->log, "galaxy space: generating\n");
    if (r->log)
        fprintf(r->log, "galaxy space step %d %.1fms\n", galaxyspace.steps, (galaxynow() - t0) * 1000);
}

/* HSV -> 预乘的 XRenderColor */
static XRenderColor
galaxyhsv(double h, double s, double v, double a)
{
    double c = v * s, x, m = v - c, rr = 0, gg = 0, bb = 0;

    h = fmod(fmod(h, 360) + 360, 360) / 60;
    x = c * (1 - fabs(fmod(h, 2) - 1));
    if (h < 1) { rr = c; gg = x; }
    else if (h < 2) { rr = x; gg = c; }
    else if (h < 3) { gg = c; bb = x; }
    else if (h < 4) { gg = x; bb = c; }
    else if (h < 5) { rr = x; bb = c; }
    else { rr = c; bb = x; }
    a = galaxyclamp(a);
    return (XRenderColor){(unsigned short)(65535 * galaxyclamp(rr + m) * a), (unsigned short)(65535 * galaxyclamp(gg + m) * a),
        (unsigned short)(65535 * galaxyclamp(bb + m) * a), (unsigned short)(65535 * a)};
}

/* 用纯色把一张蒙版上色, 双线性放大铺满远景图 */
static void
galaxyspacetint(int which, XRenderColor col)
{
    Picture solid;
    double sx = (double)galaxyspace.mw / galaxyspace.fw, sy = (double)galaxyspace.mh / galaxyspace.fh;

    if (!galaxyspace.mask[which] || !col.alpha)
        return;
    galaxyaffine(galaxyspace.mask[which], sx, sy, 0, 0);
    solid = XRenderCreateSolidFill(dpy, &col);
    XRenderComposite(dpy, PictOpOver, solid, galaxyspace.mask[which], galaxyspace.far, 0, 0, 0, 0, 0, 0, galaxyspace.fw, galaxyspace.fh);
    XRenderFreePicture(dpy, solid);
}

/* 合成远景图 (放大 / 上色只在这里做, 每帧只是平移拷贝): 底色 -> 星云 x2 -> 银河 -> 最远一层星点 */
static void
galaxyspacebake(double drift)
{
    GalaxyScene *r = &galaxyscene;
    XRenderColor base = {0x0300, 0x0480, 0x0a00, 0xffff};

    if (!galaxyspace.far) {
        galaxyspace.fw = r->vw * 13 / 10;
        galaxyspace.fh = r->vh * 13 / 10;
        galaxyspace.far = galaxyopaque(galaxyspace.fw, galaxyspace.fh, &galaxyspace.farpix);
        if (!galaxyspace.far)
            return;
    }
    XRenderFillRectangle(dpy, PictOpSrc, galaxyspace.far, &base, 0, 0, galaxyspace.fw, galaxyspace.fh);
    /* 哈勃配色: 氢的玫红 + 氧的青绿 (壁纸取色仍记在日志里, 星云不再用它), 随时间缓慢漂移 */
    galaxyspacetint(1, galaxyhsv(345 + drift, .6, .62, .78));
    galaxyspacetint(2, galaxyhsv(178 - drift, .55, .55, .68));
    galaxyspacetint(0, galaxyhsv(40, .18, .95, .34));
    if (galaxyspace.star[0])
        XRenderComposite(dpy, PictOpOver, galaxyspace.star[0], galaxywhite(.8), galaxyspace.far, 0, 0, 0, 0, 0, 0,
                galaxyspace.fw, galaxyspace.fh);
    if (galaxyspace.star[1])    /* 中层星点也合进远景图: 每帧少一次整屏合成 */
        XRenderComposite(dpy, PictOpOver, galaxyspace.star[1], galaxywhite(.9), galaxyspace.far, 377, 211, 0, 0, 0, 0,
                galaxyspace.fw, galaxyspace.fh);
    galaxyspace.bakedrift = drift;
    galaxyspace.bakedat = galaxynow();
}

/* 深空: 远景图 (含远 / 中两层星点, 按镜头角度平移, 幅度有限) + 近层平铺星点 (平移更多, 形成纵深).
 * 每帧只有 2 次不缩放的合成; 星云色调每 8 秒重新合成一次远景图, 随时间缓慢变化 */
static void
galaxyrenderspace(void)
{
    GalaxyScene *r = &galaxyscene;
    double a = r->space, drift, yaw = r->cam.ry, pitch = r->cam.rx, k, tx, ty;
    static const double depth[GALAXYSPACELAYERS] = { .25, .55, 1 };
    int i, ox, oy;

    if (a < .004 || !galaxyspace.ready)
        return;
    drift = 25 * sin(2 * GALAXYPI * r->motion * r->tscale / 240);
    if (!galaxyspace.far || (r->mode == GalaxyOrbit && galaxynow() - galaxyspace.bakedat > 8))
        galaxyspacebake(drift);
    if (!galaxyspace.far)
        return;
    /* 远景: 平移量随镜头角度周期变化, 幅度不超过 15% 视口 (远景图四周各多出 15%) */
    ox = (int)lround((galaxyspace.fw - r->vw) * .5 * (1 + sin(yaw)));
    oy = (int)lround((galaxyspace.fh - r->vh) * .5 * (1 - sin(pitch)));
    /* 完全显出后用 Src 直接拷贝 (最快的路径), 溶解过程中才带透明度 */
    if (a > .995)
        XRenderComposite(dpy, PictOpSrc, galaxyspace.far, None, r->back, ox, oy, 0, 0, r->vx, r->vy, r->vw, r->vh);
    else
        XRenderComposite(dpy, PictOpOver, galaxyspace.far, galaxywhite(a), r->back, ox, oy, 0, 0, r->vx, r->vy, r->vw, r->vh);
    for (i = 2; i < GALAXYSPACELAYERS; i++) {
        if (!galaxyspace.star[i])
            continue;
        k = depth[i] * .35 * r->cam.focal;
        tx = fmod(yaw * k + r->cam.target.x * depth[i] * .05, galaxyspace.starsize[i]);
        ty = fmod(-pitch * k + r->cam.target.y * depth[i] * .05, galaxyspace.starsize[i]);
        if (tx < 0) tx += galaxyspace.starsize[i];
        if (ty < 0) ty += galaxyspace.starsize[i];
        XRenderComposite(dpy, PictOpOver, galaxyspace.star[i], a > .995 ? None : galaxywhite(a), r->back,
                (int)tx, (int)ty, 0, 0, r->vx, r->vy, r->vw, r->vh);
    }
}
