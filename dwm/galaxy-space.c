/* Super+Z 星系: 深空背景. 由 galaxy.c 按顺序 include (在 galaxy-render.c 之前).
 *
 * 开场时壁纸溶解成程序生成的深空, 回程 / 坍缩时再溶解回去:
 *   - 3 层可平铺的星点图 (远 / 中 / 近, 尺寸各不相同), 随镜头转动按不同速度平移, 形成视差;
 *     星的亮度按幂律分布 (亮星少暗星多), 色温从冷蓝到暖黄, 最亮的自带十字衍射芒
 *   - 银河带 (沿轨道盘面的对角线方向, 中间一道暗尘带): 低分辨率 a8 蒙版, 合成时用纯色上色并双线性放大
 *   - 星云在 GPU 上程序化生成 (galaxy-gl.c 的 galaxyglnebula), 画在光层最底下, 从第一帧起就有
 * 星点和银河在 CPU 上生成, 分几帧完成 (每帧一层), 生成好后淡入; 结果按 屏幕尺寸 + 壁纸 缓存在进程里, 之后再按 Super+Z 直接复用 */

#define GALAXYSPACELAYERS 3
#define GALAXYSPACEMASKS  1           /* 0 银河 */
#define GALAXYSPACEMW     480         /* 蒙版宽度; 高度按视口比例 */

static Picture galaxyargb(int w, int h, Pixmap *pix);
static Picture galaxyopaque(int w, int h, Pixmap *pix);
static Picture galaxyupload(int w, int h, unsigned int *data, Pixmap *pix);
static void galaxyaffine(Picture p, double sx, double sy, double tx, double ty);
static Picture galaxywhite(double a);

/* 进程级缓存 (galaxyscene 每次启动都会清零, 这里不会) */
static struct {
    int ready, steps, w, h, vw, vh;
    Pixmap starpix[GALAXYSPACELAYERS], maskpix[GALAXYSPACEMASKS];
    Picture star[GALAXYSPACELAYERS], mask[GALAXYSPACEMASKS];
    int starsize[GALAXYSPACELAYERS], mw, mh;
    Pixmap farpix;                    /* 远景合成图: 底色 + 星云 + 银河 + 最远一层星点, 比视口大 30% (视差平移时不露边) */
    Picture far;
    int fw, fh;
    double bakedat;                   /* 上次合成远景图的时刻 */
    double readyat;                   /* 本次 Super+Z 中生成完成的时刻 (之后淡入); 缓存命中时为负, 不淡入 */
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

/* a8 蒙版: 银河带 (沿对角线, 中间暗尘带) */
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
            u = x * ca - y * sa;              /* 沿银河 */
            v = x * sa + y * ca;              /* 垂直银河 */
            n = galaxyfbm(u * 2.4 + 7, v * 6 + 3, 501);
            a = exp(-v * v / (.16 * .16)) * (.35 + .65 * n) * (.75 + .25 * galaxyfbm(u * 9, v * 9, 77));
            k = exp(-pow((v - .015 - .03 * (galaxyfbm(u * 3, 1, 9) - .5)) / .028, 2));   /* 暗尘带 */
            a *= 1 - .75 * k * (.5 + .5 * galaxyfbm(u * 7, v * 20, 31));
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

    /* 星点 / 银河与壁纸无关 (星云在 GPU 上), 只看尺寸 */
    if (galaxyspace.ready && galaxyspace.w == r->w && galaxyspace.h == r->h && galaxyspace.vw == r->vw
            && galaxyspace.vh == r->vh) {
        galaxyspace.readyat = -1;
        return;
    }
    galaxyspacefree();
    galaxyspace.w = r->w;
    galaxyspace.h = r->h;
    galaxyspace.vw = r->vw;
    galaxyspace.vh = r->vh;
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
    case 0: galaxyspacestars(0); break;
    case 1: galaxyspacestars(1); break;
    case 2: galaxyspacestars(2); break;
    case 3: galaxyspacemask(0); galaxyspace.ready = 1; galaxyspace.readyat = galaxynow(); break;
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

/* 合成远景图 (放大 / 上色只在这里做, 每帧只是平移拷贝): 底色 -> 银河 -> 远 / 中两层星点 */
static void
galaxyspacebake(void)
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
    galaxyspacetint(0, galaxyhsv(40, .18, .95, .34));
    if (galaxyspace.star[0])
        XRenderComposite(dpy, PictOpOver, galaxyspace.star[0], galaxywhite(.8), galaxyspace.far, 0, 0, 0, 0, 0, 0,
                galaxyspace.fw, galaxyspace.fh);
    if (galaxyspace.star[1])    /* 中层星点也合进远景图: 每帧少一次整屏合成 */
        XRenderComposite(dpy, PictOpOver, galaxyspace.star[1], galaxywhite(.9), galaxyspace.far, 377, 211, 0, 0, 0, 0,
                galaxyspace.fw, galaxyspace.fh);
    galaxyspace.bakedat = galaxynow();
}

/* 深空: 远景图 (含远 / 中两层星点, 按镜头角度平移, 幅度有限) + 近层平铺星点 (平移更多, 形成纵深).
 * 每帧只有 2 次不缩放的合成. 星点 / 银河生成好之前先铺深空底色 (碎块缝隙里不露出壁纸), 生成好后 0.25s 淡入 */
static void
galaxyrenderspace(void)
{
    GalaxyScene *r = &galaxyscene;
    XRenderColor dark = {0x0300, 0x0480, 0x0a00, 0xffff};
    double a = r->space, fade, yaw = r->cam.ry, pitch = r->cam.rx, k, tx, ty, d;
    static const double depth[GALAXYSPACELAYERS] = { .25, .55, 1 };
    int i, ox, oy;

    if (a < .004)
        return;
    fade = !galaxyspace.ready ? 0 : galaxyspace.readyat < 0 ? 1 : galaxysmoothstep((galaxynow() - galaxyspace.readyat) / .25);
    if (fade < .996) {
        d = galaxyclamp(a * (1 - fade));
        dark = (XRenderColor){(unsigned short)(dark.red * d), (unsigned short)(dark.green * d), (unsigned short)(dark.blue * d),
            (unsigned short)(65535 * d)};
        XRenderFillRectangle(dpy, PictOpOver, r->back, &dark, r->vx, r->vy, r->vw, r->vh);
    }
    if (!galaxyspace.ready || fade < .004)
        return;
    a *= fade;
    if (!galaxyspace.far)
        galaxyspacebake();
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

/* GPU 星云 (光层最底下): 强度跟深空的溶解走, 揭开壁纸时一起淡出; 降级 / 安静模式减少 fbm 倍频; 噪声偏移每次随机 */
static void
galaxyrendernebula(void)
{
    GalaxyScene *r = &galaxyscene;
    double q = r->mode == GalaxyOrbit ? r->qualityvisual : 0;

    galaxyglnebula(.5 * r->space * (1 - r->reveal), galaxynow(), q > 2 ? 3 : r->quiet ? 4 : 5, r->nebseed, r->aurora);
}

/* dwm 启动 (含原地重启) 时预先按当前显示器生成好 (约 85ms), 第一次按 Super+Z 时开场不再卡一下.
 * 星系没在运行时 galaxyscene 是空的: 临时填上生成要用的尺寸和格式, 生成完清空 */
static void
galaxyspaceprewarm(void)
{
    GalaxyScene *r = &galaxyscene;

    if (r->mode || !selmon)
        return;
    r->w = sw;
    r->h = sh;
    r->vw = selmon->mw;
    r->vh = selmon->mh;
    r->argb = XRenderFindStandardFormat(dpy, PictStandardARGB32);
    r->a8 = XRenderFindStandardFormat(dpy, PictStandardA8);
    r->stage = 1;
    if (r->argb && r->a8) {
        galaxyspacebegin();
        while (!galaxyspace.ready && galaxyspace.steps < 8)
            galaxyspacestep();
    }
    memset(r, 0, sizeof *r);
}
