/* Super+Z 星系: 深空背景. 由 galaxy.c 按顺序 include (在 galaxy-render.c 之前).
 *
 * 开场时壁纸溶解成程序生成的深空, 回程 / 坍缩时再溶解回去:
 *   - 3 层可平铺的星点图 (远 / 中 / 近, 尺寸各不相同), 随镜头转动按不同速度平移, 形成视差;
 *     星的亮度按幂律分布 (亮星少暗星多), 色温从冷蓝到暖黄, 最亮的自带十字衍射芒
 *   - 星云在 GPU 上程序化生成 (galaxy-gl.c 的 galaxyglnebula), 画在光层最底下, 从第一帧起就有
 * 星点在 CPU 上生成, 分几帧完成 (每帧一层), 生成好后淡入; 结果按 屏幕尺寸 + 壁纸 缓存在进程里, 之后再按 Super+Z 直接复用 */

#define GALAXYSPACELAYERS 3

static Picture galaxyargb(int w, int h, Pixmap *pix);
static Picture galaxyopaque(int w, int h, Pixmap *pix);
static Picture galaxyupload(int w, int h, unsigned int *data, Pixmap *pix);
static void galaxyaffine(Picture p, double sx, double sy, double tx, double ty);
static Picture galaxywhite(double a);

/* 进程级缓存 (galaxyscene 每次启动都会清零, 这里不会) */
static struct {
    int ready, steps, w, h, vw, vh;
    Pixmap starpix[GALAXYSPACELAYERS];
    Picture star[GALAXYSPACELAYERS];
    int starsize[GALAXYSPACELAYERS];
    Pixmap farpix;                    /* 远景合成图: 底色 + 远 / 中两层星点, 比视口大 30% (视差平移时不露边) */
    Picture far;
    int fw, fh;
    double bakedat;                   /* 上次合成远景图的时刻 */
    double readyat;                   /* 本次 Super+Z 中生成完成的时刻 (之后淡入); 缓存命中时为负, 不淡入 */
} galaxyspace;

static const int galaxyspacesize[GALAXYSPACELAYERS] = { 1024, 896, 768 };
static const int galaxyspacecount[GALAXYSPACELAYERS] = { 750, 350, 130 };

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

static void
galaxyspacefree(void)
{
    int i;

    for (i = 0; i < GALAXYSPACELAYERS; i++) {
        if (galaxyspace.star[i]) XRenderFreePicture(dpy, galaxyspace.star[i]);
        if (galaxyspace.starpix[i]) XFreePixmap(dpy, galaxyspace.starpix[i]);
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

    /* 星点与壁纸无关 (星云在 GPU 上), 只看尺寸 */
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
    case 2: galaxyspacestars(2); galaxyspace.ready = 1; galaxyspace.readyat = galaxynow(); break;
    }
    if (r->log && galaxyspace.steps == 1)
        fprintf(r->log, "galaxy space: generating\n");
    if (r->log)
        fprintf(r->log, "galaxy space step %d %.1fms\n", galaxyspace.steps, (galaxynow() - t0) * 1000);
}

/* 合成远景图 (每帧只是平移拷贝): 底色 -> 远 / 中两层星点 */
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
    if (galaxyspace.star[0])
        XRenderComposite(dpy, PictOpOver, galaxyspace.star[0], galaxywhite(.8), galaxyspace.far, 0, 0, 0, 0, 0, 0,
                galaxyspace.fw, galaxyspace.fh);
    if (galaxyspace.star[1])    /* 中层星点也合进远景图: 每帧少一次整屏合成 */
        XRenderComposite(dpy, PictOpOver, galaxyspace.star[1], galaxywhite(.9), galaxyspace.far, 377, 211, 0, 0, 0, 0,
                galaxyspace.fw, galaxyspace.fh);
    galaxyspace.bakedat = galaxynow();
}

/* 深空: 远景图 (含远 / 中两层星点, 按镜头角度平移, 幅度有限) + 近层平铺星点 (平移更多, 形成纵深).
 * 每帧只有 2 次不缩放的合成. 星点生成好之前先铺深空底色 (碎块缝隙里不露出壁纸), 生成好后 0.25s 淡入 */
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

/* GPU 远景星空 (光层最底下): 跟深空的溶解走, 揭开壁纸时一起淡出 */
static void
galaxyrendernebula(void)
{
    GalaxyScene *r = &galaxyscene;

    galaxyglnebula(r->space * (1 - r->reveal), galaxynow(), r->nebseed, r->gentle);
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

/* 远景元素的位置: 离画面中心够远 (不抢焦点)、离边缘有一点距离、彼此不挤在一起; 按本次的星云种子生成 */
static void
galaxyfarinit(void)
{
    GalaxyScene *r = &galaxyscene;
    double hh = (double)r->vh / MAX(1, r->vw), pts[5][2], x, y, s = 1 / 2560.0;   /* s: 2560 宽屏幕上的 1 像素 (q 单位) */
    unsigned int seed = (unsigned int)(r->nebseed[0] * 1000 + r->nebseed[1] * 37);
    int i, j, n = 0, tries, ok;

    for (i = 0; i < 5; i++) {
        for (tries = 0; tries < 60; tries++) {
            x = .06 + .88 * galaxyhash(seed + i * 131 + tries * 7);
            y = hh * (.08 + .84 * galaxyhash(seed + i * 131 + tries * 7 + 3));
            ok = hypot(x - .5, y - hh * .5) > .3;
            for (j = 0; j < n && ok; j++)
                ok = hypot(x - pts[j][0], y - pts[j][1]) > .1;
            if (ok)
                break;
        }
        pts[n][0] = x;
        pts[n][1] = y;
        n++;
    }
    for (i = 0; i < 2; i++) {     /* 远方星系 */
        float *g = r->farg[i];
        g[0] = pts[i][0];
        g[1] = pts[i][1];
        g[2] = (18 + 27 * galaxyhash(seed + i * 17 + 1)) * s;
        g[3] = 2 * GALAXYPI * galaxyhash(seed + i * 17 + 2);
        g[4] = .35 + .65 * galaxyhash(seed + i * 17 + 3);
        g[5] = galaxyhash(seed + i * 17 + 4) < .67 ? 0 : 1;
        g[6] = .07 + .05 * galaxyhash(seed + i * 17 + 5);
        g[7] = 2 * GALAXYPI * galaxyhash(seed + i * 17 + 6);
    }
    for (i = 0; i < 3; i++) {     /* 偶尔闪一下的亮星 */
        r->fars[i][0] = pts[2 + i][0];
        r->fars[i][1] = pts[2 + i][1];
        r->fars[i][2] = 5 + 6 * galaxyhash(seed + i * 29 + 11);
        r->fars[i][3] = 2 * GALAXYPI * galaxyhash(seed + i * 29 + 12);
    }
}
