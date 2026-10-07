/* Super+Z 星系: 绘制. 底层 (壁纸 / 深空 / 卡片) 用 XRender 画进 r->back, 光点和光带交给 GPU 光层 (galaxy-gl.c),
 * 文字和标签画进前景层 r->front; 每帧由 galaxyglpresent 合成. 由 galaxy.c 按顺序 include. */

/* ---------- XRender 绘制 ---------- */

static Picture galaxywhite(double a) { return galaxyscene.white[(int)(galaxyclamp(a) * (GALAXYALPHAS - 1) + .5)]; }
static Picture galaxyblack(double a) { return galaxyscene.black[(int)(galaxyclamp(a) * (GALAXYALPHAS - 1) + .5)]; }

static void
galaxyaffine(Picture p, double sx, double sy, double tx, double ty)
{
    XTransform tr = {{
        {XDoubleToFixed(sx), 0, XDoubleToFixed(tx)},
        {0, XDoubleToFixed(sy), XDoubleToFixed(ty)},
        {0, 0, XDoubleToFixed(1)}}};
    XRenderSetPictureTransform(dpy, p, &tr);
}

/* 光点: 交给 GPU 光层 (形状和原来的 sprite 一样, 白芯彩晕), 加法混合 */
static void
galaxysprite(int shape, int tint, double x, double y, double radius, double alpha)
{
    static double rgb[GalaxyTints][3];
    static int ready;
    int t;

    if (!ready) {
        for (t = 0; t < GalaxyTints; t++)
            galaxytintcolor(t, -1, rgb[t]);
        ready = 1;
    }
    galaxyglglow(shape, rgb[tint], 1, x, y, radius, alpha);
}

static void
galaxyinvert(double m[3][3], double o[3][3])
{
    double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
               - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
               + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    int i, j;

    o[0][0] = m[1][1] * m[2][2] - m[1][2] * m[2][1];
    o[0][1] = m[0][2] * m[2][1] - m[0][1] * m[2][2];
    o[0][2] = m[0][1] * m[1][2] - m[0][2] * m[1][1];
    o[1][0] = m[1][2] * m[2][0] - m[1][0] * m[2][2];
    o[1][1] = m[0][0] * m[2][2] - m[0][2] * m[2][0];
    o[1][2] = m[0][2] * m[1][0] - m[0][0] * m[1][2];
    o[2][0] = m[1][0] * m[2][1] - m[1][1] * m[2][0];
    o[2][1] = m[0][1] * m[2][0] - m[0][0] * m[2][1];
    o[2][2] = m[0][0] * m[1][1] - m[0][1] * m[1][0];
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            o[i][j] /= det;
}

/* 单位正方形 -> 屏幕四边形的单应矩阵 (Heckbert), 顺序: 左上 右上 右下 左下 */
static int
galaxysquaretoquad(double q[4][2], double m[3][3])
{
    double sx = q[0][0] - q[1][0] + q[2][0] - q[3][0];
    double sy = q[0][1] - q[1][1] + q[2][1] - q[3][1];
    double dx1 = q[1][0] - q[2][0], dx2 = q[3][0] - q[2][0];
    double dy1 = q[1][1] - q[2][1], dy2 = q[3][1] - q[2][1];
    double den = dx1 * dy2 - dx2 * dy1, g = 0, h = 0;

    if (fabs(den) < 1e-6)
        return 0;
    if (fabs(sx) > 1e-9 || fabs(sy) > 1e-9) {
        g = (sx * dy2 - dx2 * sy) / den;
        h = (dx1 * sy - sx * dy1) / den;
    }
    m[0][0] = q[1][0] - q[0][0] + g * q[1][0];
    m[0][1] = q[3][0] - q[0][0] + h * q[3][0];
    m[0][2] = q[0][0];
    m[1][0] = q[1][1] - q[0][1] + g * q[1][1];
    m[1][1] = q[3][1] - q[0][1] + h * q[3][1];
    m[1][2] = q[0][1];
    m[2][0] = g;
    m[2][1] = h;
    m[2][2] = 1;
    return 1;
}

/* 把屏幕四边形映射回截图: XRender 的变换是 目标坐标 -> 源坐标 */
static int
galaxyhomography(Picture p, double q[4][2], int bw, int bh, int srcw, int srch)
{
    double m[3][3], inv[3][3], k = 0;
    XTransform tr;
    int i, j;

    if (!galaxysquaretoquad(q, m))
        return 0;
    galaxyinvert(m, inv);
    for (j = 0; j < 3; j++) {
        inv[0][j] *= srcw;
        inv[1][j] *= srch;
    }
    if (fabs(inv[2][2]) < 1e-12)
        return 0;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            if (i != 2 || j != 2)
                inv[i][j] /= inv[2][2];
    inv[2][2] = 1;
    if (fabs(inv[2][0]) * bw + fabs(inv[2][1]) * bh < 2e-3) {
        inv[2][0] = inv[2][1] = 0;  /* 近似仿射: 走 XRender 的仿射快速路径 */
    } else {
        /* 真透视: 整体缩放矩阵 (齐次坐标不变), 让透视项在 16.16 定点数里有足够精度,
         * 同时 矩阵元素 x 目标坐标 不超出 pixman 中间计算的范围 (超出时整块采样会塌成竖条纹) */
        for (i = 0; i < 3; i++)
            for (j = 0; j < 3; j++)
                k = MAX(k, fabs(inv[i][j]));
        k = 16000 / (k * (bw + bh + 1));
        for (i = 0; i < 3; i++)
            for (j = 0; j < 3; j++)
                inv[i][j] *= k;
    }
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++) {
            if (fabs(inv[i][j]) > 32000)
                return 0;
            tr.matrix[i][j] = XDoubleToFixed(inv[i][j]);
        }
    XRenderSetPictureTransform(dpy, p, &tr);
    return 1;
}

/* 屏幕四边形 (左上 右上 右下 左下) 填充纯色: 两个抗锯齿三角形, 不需要纹理采样 */
static void
galaxyfillquad(Picture color, double q[4][2], int ox, int oy)
{
    GalaxyScene *r = &galaxyscene;
    XTriangle t[2];
    XPointFixed p[4];
    int i;

    for (i = 0; i < 4; i++)
        p[i] = (XPointFixed){XDoubleToFixed(q[i][0] + ox), XDoubleToFixed(q[i][1] + oy)};
    t[0] = (XTriangle){p[0], p[1], p[2]};
    t[1] = (XTriangle){p[0], p[2], p[3]};
    XRenderCompositeTriangles(dpy, PictOpOver, color, r->back, r->a8, 0, 0, t, 2);
}

/* 四角几乎共面成矩形时返回屏幕上的外接矩形. 开场前几帧卡片还没转, 走这条路径. */
static int
galaxyquadrect(double q[4][2], double eps, double *x, double *y, double *w, double *h)
{
    double l, r, t, b;

    if (fabs(q[0][1] - q[1][1]) > eps || fabs(q[2][1] - q[3][1]) > eps
            || fabs(q[0][0] - q[3][0]) > eps || fabs(q[1][0] - q[2][0]) > eps)
        return 0;
    l = (q[0][0] + q[3][0]) * .5;
    r = (q[1][0] + q[2][0]) * .5;
    t = (q[0][1] + q[1][1]) * .5;
    b = (q[3][1] + q[2][1]) * .5;
    if (r - l < 1 || b - t < 1)
        return 0;
    *x = l;
    *y = t;
    *w = r - l;
    *h = b - t;
    return 1;
}

/* 与截图像素 1:1 的轴对齐卡片: 无变换的拷贝, 比带蒙版的双线性合成便宜得多. */
static void
galaxycopywindow(GalaxyStar *s, int lvl, double left, double top)
{
    GalaxyScene *r = &galaxyscene;
    int dx = (int)lround(left), dy = (int)lround(top);
    int sx = 0, sy = 0, dw = s->mipw[lvl], dh = s->miph[lvl];

    if (dx < 0) {
        sx = -dx;
        dw += dx;
        dx = 0;
    }
    if (dy < 0) {
        sy = -dy;
        dh += dy;
        dy = 0;
    }
    if (dx + dw > r->w)
        dw = r->w - dx;
    if (dy + dh > r->h)
        dh = r->h - dy;
    if (dw < 1 || dh < 1)
        return;
    galaxyaffine(s->mip[lvl], 1, 1, 0, 0);
    XRenderSetPictureFilter(dpy, s->mip[lvl], FilterNearest, NULL, 0);
    XRenderComposite(dpy, PictOpSrc, s->mip[lvl], None, r->back, sx, sy, 0, 0, dx, dy, dw, dh);
    XRenderSetPictureFilter(dpy, s->mip[lvl], FilterBilinear, NULL, 0);
}

/* 软件透视很慢时, 先画进半分辨率离屏图再放大. 一次采样, 目标像素约为整屏的四分之一. */
static int
galaxyrenderhalf(GalaxyStar *s, int lvl, double vis, double q[4][2], int x0, int y0, int x1, int y1)
{
    GalaxyScene *r = &galaxyscene;
    double hq[4][2];
    int bw = x1 - x0, bh = y1 - y0, dw, dh, i, cx, cy, dx1, dy1;

    if (!r->spinpic || bw < 2 || bh < 2)
        return 0;
    dw = MIN(r->spinw, MAX(1, bw / 2));
    dh = MIN(r->spinh, MAX(1, bh / 2));
    for (i = 0; i < 4; i++) {
        hq[i][0] = q[i][0] * dw / (double)bw;
        hq[i][1] = q[i][1] * dh / (double)bh;
    }
    if (!galaxyhomography(s->mip[lvl], hq, dw, dh, s->mipw[lvl], s->miph[lvl]))
        return 0;
    XRenderComposite(dpy, PictOpSrc, s->mip[lvl], None, r->spinpic, 0, 0, 0, 0, 0, 0, dw, dh);
    galaxyaffine(r->spinpic, (double)dw / bw, (double)dh / bh, 0, 0);
    cx = MAX(0, x0);
    cy = MAX(0, y0);
    dx1 = MIN(r->w, x1);
    dy1 = MIN(r->h, y1);
    if (dx1 <= cx || dy1 <= cy)
        return 0;
    XRenderComposite(dpy, PictOpOver, r->spinpic, vis > .996 ? None : galaxywhite(vis), r->back,
            cx - x0, cy - y0, 0, 0, cx, cy, dx1 - cx, dy1 - cy);
    return 1;
}

/* 大卡片把透视面切成小块, 每块用仿射采样; A1 蒙版避免块与块之间的抗锯齿暗缝.
 * 只在单应矩阵放不下时使用: 分块在 GPU 上是十几次往返, 比一次透视采样更慢. */
static int
galaxyrendertiled(GalaxyStar *s, int lvl, double vis)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyProj p[4];
    GalaxyVec corner;
    XTriangle tri[2];
    XTransform tr;
    XRenderColor clear = {0, 0, 0, 0};
    double q[4][2], dx0, dx1, dy0, dy1, det, sx, sy, minx, miny, maxx, maxy;
    double ax, bx, cx, ay, by, cy, u, v;
    int tx, ty, j, x0, y0, x1, y1, used = 0;

    if (!r->tilemask)
        return 0;
    for (ty = 0; ty < GALAXYTILES; ty++)
        for (tx = 0; tx < GALAXYTILES; tx++) {
            for (j = 0; j < 4; j++) {
                u = (tx + (j == 1 || j == 2)) / (double)GALAXYTILES;
                v = (ty + (j >= 2)) / (double)GALAXYTILES;
                corner = galaxyadd(s->pos, galaxyapply(s->orient,
                            galaxyv((u - .5) * s->w * s->size, (v - .5) * s->h * s->size, 0)));
                p[j] = galaxyproject(corner);
                if (!p[j].ok)
                    break;
                q[j][0] = p[j].x;
                q[j][1] = p[j].y;
            }
            if (j < 4)
                continue;
            minx = maxx = q[0][0];
            miny = maxy = q[0][1];
            for (j = 1; j < 4; j++) {
                minx = MIN(minx, q[j][0]); maxx = MAX(maxx, q[j][0]);
                miny = MIN(miny, q[j][1]); maxy = MAX(maxy, q[j][1]);
            }
            x0 = MAX(0, (int)floor(minx)); y0 = MAX(0, (int)floor(miny));
            x1 = MIN(r->w, (int)ceil(maxx) + 1); y1 = MIN(r->h, (int)ceil(maxy) + 1);
            if (x1 <= x0 || y1 <= y0)
                continue;
            dx0 = q[1][0] - q[0][0]; dx1 = q[3][0] - q[0][0];
            dy0 = q[1][1] - q[0][1]; dy1 = q[3][1] - q[0][1];
            det = dx0 * dy1 - dx1 * dy0;
            if (fabs(det) < 1e-6)
                continue;
            sx = s->mipw[lvl] / (double)GALAXYTILES;
            sy = s->miph[lvl] / (double)GALAXYTILES;
            ax = sx * dy1 / det; bx = -sx * dx1 / det;
            ay = -sy * dy0 / det; by = sy * dx0 / det;
            cx = tx * sx - ax * q[0][0] - bx * q[0][1];
            cy = ty * sy - ay * q[0][0] - by * q[0][1];
            if (MAX(MAX(fabs(ax), fabs(bx)), MAX(fabs(cx), MAX(fabs(ay), MAX(fabs(by), fabs(cy))))) > 32000)
                continue;
            tr = (XTransform){{
                {XDoubleToFixed(ax), XDoubleToFixed(bx), XDoubleToFixed(cx)},
                {XDoubleToFixed(ay), XDoubleToFixed(by), XDoubleToFixed(cy)},
                {0, 0, XDoubleToFixed(1)}}};
            XRenderFillRectangle(dpy, PictOpSrc, r->tilemask, &clear, x0, y0, x1 - x0, y1 - y0);
            tri[0] = (XTriangle){
                {XDoubleToFixed(q[0][0]), XDoubleToFixed(q[0][1])},
                {XDoubleToFixed(q[1][0]), XDoubleToFixed(q[1][1])},
                {XDoubleToFixed(q[2][0]), XDoubleToFixed(q[2][1])}};
            tri[1] = (XTriangle){
                {XDoubleToFixed(q[0][0]), XDoubleToFixed(q[0][1])},
                {XDoubleToFixed(q[2][0]), XDoubleToFixed(q[2][1])},
                {XDoubleToFixed(q[3][0]), XDoubleToFixed(q[3][1])}};
            XRenderCompositeTriangles(dpy, PictOpOver, galaxywhite(vis), r->tilemask,
                    r->a1, 0, 0, tri, 2);
            XRenderSetPictureTransform(dpy, s->mip[lvl], &tr);
            XRenderComposite(dpy, PictOpOver, s->mip[lvl], r->tilemask, r->back,
                    x0, y0, x0, y0, x0, y0, x1 - x0, y1 - y0);
            used = 1;
        }
    return used;
}

/* GPU 画卡片 (截图纹理带 mip 链, 透视校正, 边缘光 / 高光): 画了返回 1; 纹理建不了返回 0, 由调用方走 XRender.
 * 与真实窗口逐像素对齐的原尺寸卡片 (开场第一帧 / 落定) 逐像素采样, 与桌面完全一致 */
static int
galaxyrendercardgl(GalaxyStar *s, double q[4][2], double qz[4], double vis, double tint, double light, int aligned,
        double left, double top, double rw, double rh, double snap)
{
    GalaxyScene *r = &galaxyscene;
    static const double neb[3] = {.38, .32, .62};
    double qe[4][2], d, f, t;
    int orbit = r->mode == GalaxyOrbit, exact, i, moving;
    GalaxyVec normal, lightdir = galaxyv(-.35, -.55, -.76);
    GalaxyCardFx fx = {0};

    if (!galaxygl.win || s->glfail || !s->mippix[s->base])
        return 0;
    /* 纹理第一次画时才建, 每帧最多 1 张 (驻留 2 张); 还没轮到的这一帧先走 XRender */
    if (!s->gltex) {
        if (galaxygl.cardups >= (r->mode == GalaxyOrbit ? 2 : 1))
            return 0;
        galaxygl.cardups++;
        if (!(s->gltex = galaxyglcardtex(s->mippix[s->base], s->mipw[s->base], s->miph[s->base], &s->glpix, &s->glsrc))) {
            s->glfail = 1;
            return 0;
        }
        s->gldirty = 0;
    } else if (s->gldirty && galaxygl.cardups < 2) {   /* 截图刷新过: 原地重拷 (同样计入每帧限额, 超额就先用旧的) */
        galaxygl.cardups++;
        galaxyglcardcopy(s->glpix, s->glsrc, s->gltex, s->mipw[s->base], s->miph[s->base]);
        s->gldirty = 0;
    }
    exact = aligned && s->base == 0 && fabs(rw - s->mipw[0]) < snap && fabs(rh - s->miph[0]) < snap
        && (r->mode != GalaxyReturn || (fabs(left - lround(left)) < snap && fabs(top - lround(top)) < snap));
    if (exact) {
        qe[0][0] = qe[3][0] = lround(left);
        qe[0][1] = qe[1][1] = lround(top);
        qe[1][0] = qe[2][0] = lround(left) + s->mipw[0];
        qe[2][1] = qe[3][1] = lround(top) + s->miph[0];
        q = qe;
    }
    galaxyglcutout(q, qz, vis);
    galaxytintcolor(GALAXYTAGTINT(s->galaxy), -1, fx.rimc);
    normal = galaxyapply(s->orient, galaxyv(0, 0, -1));
    if (orbit) {
        /* 驻留: 卡片边缘一圈 tag 色的光 (悬停时更亮), 转动时一道高光从卡面扫过 */
        fx.rim = .3 * vis * (1 + 3 * s->hover);
        d = galaxydot(normal, lightdir);
        fx.specat = .5 + 1.4 * d;
        fx.spec = .1 * vis;
    }
    /* 环境光: 卡面映出星云的紫和所属核心的 tag 色, 朝向核心的一面更亮 (驻留 / 坍缩) */
    if ((orbit || r->mode == GalaxyCollapse) && s->galaxy >= 0 && s->galaxy < r->ntags) {
        f = .5 + .5 * galaxydot(normal, galaxynormalize(galaxysub(r->galaxies[s->galaxy].pos, s->pos)));
        for (i = 0; i < 3; i++)
            fx.env[i] = fx.rimc[i] * (.03 + .06 * f) + neb[i] * .04 * r->space;
    }
    /* 回程 / 落位快结束时, 落回原位的卡片上从左上到右下扫过一道细光 (焦点窗口 / 选中的先扫); u=0.96 前结束, 末帧不变 */
    if (r->mode == GalaxyReturn && (r->rkind == GalaxyLand ? s->land : s->back)) {
        int lead = r->rkind == GalaxyLand ? s - r->stars == r->rstar : s->focused;
        double a0 = lead ? .76 : .81, sp = galaxyphase(r->retu, a0, a0 + .14);

        if (sp > 0 && sp < 1) {
            fx.specat = -.25 + 1.5 * sp;
            fx.spec = .14 * sin(GALAXYPI * sp) * vis;
        }
    }
    /* 点击涟漪: 点中后 0.45s 内从点击处扩散一圈 */
    if (s->clickat > 0 && (t = (galaxynow() - s->clickat) / .45) < 1) {
        fx.ripple[0] = s->clickuv[0];
        fx.ripple[1] = s->clickuv[1];
        fx.ripple[2] = MAX(.001, t);
    }
    for (i = 0; i < 4 && !exact; i++)
        if (qz[i] <= 0)
            return 1;
    fx.vis = orbit ? vis * (.9 + .1 * MIN(1, light)) : vis;
    fx.tint = tint * MIN(1, light * 1.1);
    fx.dark = orbit ? 0 : (1 - MIN(1, light)) * vis;
    fx.bias = galaxydepthblur(s->p.z) > .55 ? 1 : 0;
    fx.aspect = (double)s->w / MAX(1, s->h);
    fx.exact = exact;
    /* 运动模糊: 只在开场 / 坍缩 / 回程这些快速段做 (驻留时卡片移动慢, 糊不出来还要多画几遍);
     * 只用上一帧也画过的四角, 模糊长度最多相当于 1/60s 的运动 (帧率低时不糊成一片) */
    moving = !orbit && s->pqframe + 1 == galaxygl.frameid;
    if (moving && r->pdt > 1.0 / 60) {
        f = 1.0 / 60 / r->pdt;
        for (i = 0; i < 4; i++) {
            s->pq[i][0] = q[i][0] + (s->pq[i][0] - q[i][0]) * f;
            s->pq[i][1] = q[i][1] + (s->pq[i][1] - q[i][1]) * f;
        }
    }
    galaxyglcard(s->gltex, q, qz, &fx, moving ? s->pq : NULL);
    memcpy(s->pq, q, sizeof s->pq);
    s->pqframe = galaxygl.frameid;
    return 1;
}

static void
galaxyrenderwindow(GalaxyStar *s, double vis, double tint, double light)
{
    GalaxyScene *r = &galaxyscene;
    static const double sx[4] = {-1, 1, 1, -1}, sy[4] = {-1, -1, 1, 1};
    double q[4][2], minx = 1e9, miny = 1e9, maxx = -1e9, maxy = -1e9, edge, hw, hh, dark;
    double left = 0, top = 0, rw = 0, rh = 0, persp, snap, qa[4][2], qz[4], qp[4][2], tx = 0, ty = 0;
    GalaxyVec corner, normal, tocam;
    GalaxyProj p;
    Picture mask;
    int i, lvl, x0, y0, x1, y1, cx, cy, tiled, aligned, pixelcopy, drawn, opaque;

    if (!s->snap || (vis < .004 && tint < .004))
        return;
    hw = s->w * .5 * s->size * s->kw;
    hh = s->h * .5 * s->size * s->kh;
    if (r->mode == GalaxyOrbit && s->hover > .001 && s->bx1 > s->bx0 && s->by1 > s->by0) {
        ty = 8 * GALAXYPI / 180 * s->hover * MAX(-1, MIN(1, (r->mx - (s->bx0 + s->bx1) * .5) / ((s->bx1 - s->bx0) * .5)));
        tx = 8 * GALAXYPI / 180 * s->hover * MAX(-1, MIN(1, (r->my - (s->by0 + s->by1) * .5) / ((s->by1 - s->by0) * .5)));
    }
    for (i = 0; i < 4; i++) {
        /* 悬停: 指针所在的一侧略微往里压 (最多约 8°) */
        corner = galaxyadd(s->pos, galaxyapply(s->orient, galaxyv(sx[i] * hw * cos(ty), sy[i] * hh * cos(tx),
                        sx[i] * hw * sin(ty) + sy[i] * hh * sin(tx))));
        p = galaxyproject(corner);
        if (!p.ok || p.z < r->cam.focal * .25)
            return;
        q[i][0] = p.x;
        q[i][1] = p.y;
        qz[i] = p.z;
        minx = MIN(minx, p.x); maxx = MAX(maxx, p.x);
        miny = MIN(miny, p.y); maxy = MAX(maxy, p.y);
    }
    memcpy(qp, q, sizeof qp);   /* 真实透视的四角 (GL 路径用; 下面的仿射近似只给 XRender) */
    persp = hypot(q[0][0] - q[1][0] + q[2][0] - q[3][0], q[0][1] - q[1][1] + q[2][1] - q[3][1]);
    /* 驻留时卡片朝向镜头, 透视误差很小: 一律走仿射, 不随尺寸在两条路径之间切换 (切换那一帧卡片形状会跳一下) */
    if ((r->mode == GalaxyOrbit && (MAX(maxx - minx, maxy - miny) < 480 || persp < 6))
            || (r->mode != GalaxyOrbit && r->iclock > 1 && persp < 4)) {
        /* 小卡片, 或几乎平行于画面的卡片, 以仿射路径采样; 透视误差小于几像素, 合成开销低得多 (约 1/7). */
        q[2][0] = q[1][0] + q[3][0] - q[0][0];
        q[2][1] = q[1][1] + q[3][1] - q[0][1];
        minx = miny = 1e9;
        maxx = maxy = -1e9;
        for (i = 0; i < 4; i++) {
            minx = MIN(minx, q[i][0]); maxx = MAX(maxx, q[i][0]);
            miny = MIN(miny, q[i][1]); maxy = MAX(maxy, q[i][1]);
        }
    }
    if (maxx < 0 || maxy < 0 || minx > r->w || miny > r->h || maxx - minx < 1 || maxy - miny < 1)
        return;
    if (vis > .3) {
        s->bx0 = minx; s->by0 = miny;
        s->bx1 = maxx; s->by1 = maxy;
        s->hit = 1;
    }
    /* 卡片背面朝向镜头时更暗 (单应变换自动镜像截图) */
    normal = galaxyapply(s->orient, galaxyv(0, 0, -1));
    tocam = galaxysub(r->cam.pos, s->pos);
    if (galaxydot(normal, tocam) < 0)
        light *= .4;
    /* 选择 mip: 源像素 / 屏幕像素 不超过 2, 远处再降一级 (景深模糊) */
    edge = MAX(hypot(q[1][0] - q[0][0], q[1][1] - q[0][1]), hypot(q[3][0] - q[0][0], q[3][1] - q[0][1]) * s->w / MAX(1, s->h));
    for (lvl = s->base; lvl < GALAXYMIPS - 1 && s->mip[lvl + 1] && s->mipw[lvl] > 2 * edge; lvl++);
    if (r->mode == GalaxyOrbit) {
        int budget = edge < 150 ? 256 : 512;
        while (lvl < GALAXYMIPS - 1 && s->mip[lvl + 1] && s->mipw[lvl] > budget)
            lvl++;
    }
    if (galaxydepthblur(s->p.z) > .55 && lvl < GALAXYMIPS - 1 && s->mip[lvl + 1])
        lvl++;
    /* 开场仍与桌面截图像素重合时直接拷贝. 一开始就转的全屏卡如果走 4x4 分块,
     * 每帧十几次合成, 在 :0 上会从约 8ms 掉到 40ms 以上. */
    aligned = galaxyquadrect(q, .75, &left, &top, &rw, &rh);
    persp = hypot(q[0][0] - q[1][0] + q[2][0] - q[3][0], q[0][1] - q[1][1] + q[2][1] - q[3][1]);
    opaque = vis > .996 && r->mode != GalaxyOrbit;
    /* 回程中卡片还在做亚像素的收尾: 按取整位置拷贝会让最后几帧跳 1px、清晰度突变 (落定时看起来抖一下),
     * 只在真正落定 (位置和大小都对齐到像素) 时才拷贝 */
    snap = r->mode == GalaxyReturn ? .02 : 1.25;
    pixelcopy = aligned && opaque && fabs(rw - s->mipw[lvl]) < snap && fabs(rh - s->miph[lvl]) < snap
            && (r->mode != GalaxyReturn || (fabs(left - lround(left)) < snap && fabs(top - lround(top)) < snap));
    x0 = (int)floor(minx);
    y0 = (int)floor(miny);
    x1 = (int)ceil(maxx) + 1;
    y1 = (int)ceil(maxy) + 1;
    memcpy(qa, q, sizeof qa);
    if (galaxyrendercardgl(s, qp, qz, vis, tint, light, aligned && opaque, left, top, rw, rh, snap))
        return;
    /* 光层上按卡片形状挖洞: 它后面的光被挡住, 粒子按它的深度测试 */
    galaxyglcutout(qa, qz, vis);
    for (i = 0; i < 4; i++) {
        q[i][0] -= x0;
        q[i][1] -= y0;
    }
    drawn = tiled = cx = cy = 0;
    if (pixelcopy) {
        galaxycopywindow(s, lvl, left, top);
        drawn = 1;
    } else if (opaque && (maxx - minx) * (maxy - miny) > 450000 && persp > 1 && r->projslow) {
        drawn = galaxyrenderhalf(s, lvl, vis, q, x0, y0, x1, y1);
    }
    if (!drawn) {
        if (!galaxyhomography(s->mip[lvl], q, x1 - x0, y1 - y0, s->mipw[lvl], s->miph[lvl])) {
            if (vis < .004 || !galaxyrendertiled(s, lvl, vis))
                return;
            tiled = 1;
        }
        cx = MAX(0, x0);
        cy = MAX(0, y0);
        x1 = MIN(r->w, x1);
        y1 = MIN(r->h, y1);
        if (x1 <= cx || y1 <= cy)
            return;
        if (vis >= .004 && !tiled) {
            /* 不透明卡片不用实心蒙版. 透视时四边形外的角是透明的, 必须 Over, 不能 Src. */
            mask = opaque ? None : galaxywhite(r->mode == GalaxyOrbit ? vis * (.7 + .3 * MIN(1, light)) : vis);
            XRenderComposite(dpy, opaque && aligned ? PictOpSrc : PictOpOver, s->mip[lvl], mask, r->back,
                    cx - x0, cy - y0, 0, 0, cx, cy, x1 - cx, y1 - cy);
        }
    }
    dark = r->mode == GalaxyOrbit ? 0 : (1 - MIN(1, light)) * vis;
    for (i = 0; i < 2; i++) {
        double a = i ? tint * MIN(1, light * 1.1) : dark;
        Picture color = i ? galaxywhite(a) : galaxyblack(a);

        if (a < .004)
            continue;
        /* 压暗 / 发光叠加: 用卡片自己 (已设好变换) 当蒙版, GPU 直接按卡片形状合成.
         * NVIDIA 上大三角形是 CPU 栅格化的, 一张大卡片约 0.8ms */
        if (pixelcopy)
            XRenderComposite(dpy, PictOpOver, color, None, r->back, 0, 0, 0, 0,
                    (int)lround(left), (int)lround(top), s->mipw[lvl], s->miph[lvl]);
        else if (!drawn && !tiled)
            XRenderComposite(dpy, PictOpOver, color, s->mip[lvl], r->back, 0, 0, cx - x0, cy - y0, cx, cy, x1 - cx, y1 - cy);
        else
            galaxyfillquad(color, q, x0, y0);
    }
}

static void
galaxyrenderglow(int tint, GalaxyProj p, double radius, double alpha, double blur, double halo, double outer)
{
    /* 点光源: 靠近时核心只按透视比例的平方根慢慢变大 (有上限), 主要是变亮, 多出的能量交给眩光和泛光;
     * 眩光越大单位面积越淡 (能量守恒), 近在眼前也不会糊成一张白饼 */
    double s = MAX(.05, p.scale), core = MIN(24, radius * sqrt(s)), boost = MIN(1.8, pow(s, .3));
    double glare = core * (3.5 + 2.5 * MIN(1, s / 3)) * (1 + .5 * blur), sf;

    if (outer > 0)
        galaxysprite(GalaxyHalo, tint, p.x, p.y, glare * 2.2, outer * alpha * boost / MAX(1, glare * 2.2 / 180));
    galaxysprite(GalaxyHalo, tint, p.x, p.y, glare, halo * alpha * boost / MAX(1, glare / 110));
    galaxysprite(GalaxyPoint, tint, p.x, p.y, MAX(1.5, core * 2.4), MIN(1, alpha * (1 - .5 * blur) * boost));
    /* 镜头贴近时 (按真实透视的屏幕半径 30~80px 渐显): 叠一层恒星表面, 有临边昏暗、米粒纹理和日冕 */
    sf = galaxysmoothstep((radius * s - 30) / 50) * (1 - blur);
    if (sf > .01)
        galaxysprite(GalaxySurface, tint, p.x, p.y, MIN(150, radius * s) / .55, MIN(1, .6 * alpha * sf));
}

/* 细光带 (轨道环 / 尾迹 / 冲击环): 软件画进 a8 画布 (按距离算覆盖率的抗锯齿胶囊), 同一画布内取最大值, 重叠处不叠亮.
 * a 是白色的不透明度 (上限 .25), hw 是半宽 */
static double galaxyprand(void);
static GalaxyVec galaxyprandvec(double len);
static void galaxyemit(GalaxyVec pos, GalaxyVec vel, double life, double size, int tint, double alpha, double drag, int screen);
static int galaxyemitcount(double rate);

/* 接下来的光带用这个颜色 (打包 0xRRGGBB, 见 galaxytintrgb); galaxybandwhite 恢复白色 (每帧开始也会恢复) */
static void galaxybandcolor(unsigned int c) { galaxyscene.bandcolor = c | 0xff000000u; }
static void galaxybandwhite(void) { galaxyscene.bandcolor = 0xffffffffu; }

static void
galaxyband(GalaxyProj *pa, GalaxyProj *pb, double a, double hw)
{
    GalaxyScene *r = &galaxyscene;

    if (a < 1 / 64.0)
        return;
    galaxyglstream(pa, pb, r->bandcolor, r->bandcolor, MIN(a, .25), MIN(hw, 9));
}

/* 画布上是否有尚未合成、且与这个屏幕矩形相交的光带 */

/* 把画布上有内容的块 (同一行相邻的块合并) 上传并以白色合成到场景, 然后清空这些块 */
/* 光带已经直接提交给 GPU, 这里不用再做什么 (保留调用点, 标明一组光带画完了) */
static void
galaxyflushbands(void)
{
}




static GalaxyProj galaxysp(double x, double y);

/* 镜头光晕: 很亮的光源沿 光源 -> 画面中心 的连线留下几枚彩色鬼影, 再加一道横向拉丝 (画在光层最前面, 不被卡片挡).
 * 按已知光源解析地画, 不从泛光里取 (那样大面积爆闪会把整屏染色) */
static void
galaxylensflare(double x, double y, double k)
{
    GalaxyScene *r = &galaxyscene;
    static const double at[5] = { .42, .78, 1.2, 1.55, 2.05 }, size[5] = { 22, 58, 16, 92, 38 }, alpha[5] = { .1, .05, .14, .04, .07 };
    static const int tint[5] = { GalaxyCyan, GalaxyViolet, GalaxyGold, GalaxyRose, GalaxyCool };
    double cx = r->vx + r->vw * .5, cy = r->vy + r->vh * .5, s = r->vw / 2560.0;
    GalaxyProj a, b;
    int i;

    k = galaxyclamp(k);
    if (k < .02 || r->gentle)
        return;
    for (i = 0; i < 5; i++)
        galaxysprite(i % 2 ? GalaxyHalo : GalaxyDisc, tint[i], x + (cx - x) * at[i], y + (cy - y) * at[i], size[i] * s, k * alpha[i]);
    a = galaxysp(x - .3 * r->vw, y);
    b = galaxysp(x + .3 * r->vw, y);
    galaxyglline(&a, &b, 0x7fb8ff, 0x7fb8ff, .1 * k, 1.1);
    galaxyglline(&a, &b, 0x4d8bff, 0x4d8bff, .035 * k, 6);
}

/* 中心光源: 三条椭圆的共同焦点, 发出涟漪时脉冲一次 */
static void
galaxyrendersun(void)
{
    GalaxyScene *r = &galaxyscene;
    double a = r->sunalpha * (1 + 1.2 * r->sunpulse), th = 2 * GALAXYPI * r->motion * r->tscale / 9, rad = .025 * r->vw, w;
    GalaxyProj p;
    double chime = r->chimeat > 0 && r->mode == GalaxyOrbit ? galaxyflash(galaxynow() - r->chimeat, .3, 1.1) : 0;
    int i;

    /* 双星: 一冷一暖两颗粒子恒星绕共同质心 (群轨道的焦点) 互转, 随相位轻微脉动 */
    for (i = 0; i < 2; i++) {
        p = galaxyproject(galaxyapply(r->world, galaxyv((i ? -1 : 1) * rad * cos(th), 0, (i ? -1 : 1) * rad * sin(th))));
        if (!p.ok)
            continue;
        w = 1 + .12 * sin(th * 2 + i * GALAXYPI);
        /* 粒子恒星 (一冷一暖); 脉冲 / 整点报时只让粒子团亮一点、胀一点, 不出现白光大球 */
        {
            double rgb[3], e = MIN(2, r->sunpulse + 1.5 * chime);
            GalaxyVec c = galaxyapply(r->world, galaxyv((i ? -1 : 1) * rad * cos(th), 0, (i ? -1 : 1) * rad * sin(th)));

            galaxytintcolor(i ? GalaxyWarm : GalaxyCool, -1, rgb);
            galaxyglstarball(c, &r->world, (i ? 14 : 17) * 2.4 * r->starscale * w * (1 + .2 * e), 1.1, rgb,
                    .1 * MIN(1, a) * (1 + .5 * e), r->starscale, (int)(2200 * (r->quiet ? .5 : 1)), i ? 77.1 : 53.9);
            galaxysprite(GalaxyHalo, i ? GalaxyWarm : GalaxyCool, p.x, p.y, MIN(160, (i ? 14 : 17) * 6 * p.scale),
                    .12 * MIN(1, a) * (1 + .5 * e) * galaxynearfade(p.z));
        }
    }
}

/* CPU 色温: 闲时是所在 tag 的颜色 -> 暖白 (约 30%) -> 橙红 (80% 以上). w[0..2] 对应 tint[0..2] */
static void
galaxyheatweights(double heat, int tag, int tint[3], double w[3])
{
    tint[0] = GALAXYTAGTINT(tag);
    tint[1] = GalaxyWarm;
    tint[2] = GalaxyHot;
    w[2] = galaxysmoothstep((heat - .35) / .45);
    w[0] = 1 - galaxysmoothstep((heat - .04) / .26);
    w[1] = MAX(0, 1 - w[2] - w[0]);
}

/* 请求关注的窗口: 1.2s 周期脉动 (0..1) */
static double
galaxyurgentpulse(void)
{
    return .5 + .5 * sin(2 * GALAXYPI * galaxynow() / 1.2);
}

static void
galaxyrenderstar(GalaxyStar *s)
{
    GalaxyScene *r = &galaxyscene;
    double blur = galaxydepthblur(s->p.z), rad, a, w[3], halo, pulse = s->urgent ? galaxyurgentpulse() : 0;
    double span = hypot(s->w * s->kw, s->h * s->kh) * s->size * s->p.scale;
    int t, tint[3];

    galaxyheatweights(s->heat, s->galaxy, tint, w);
    /* 截图面板背后的柔光, 让面板像发光体而不是贴图; 颜色随 CPU 占用变化 */
    if (s->vis > .02 && s->glow > .01) {
        halo = .16 * r->glowscale * s->glow * MIN(1, s->vis * 1.3) * (r->mode == GalaxyOrbit ? 1 - galaxyclamp(r->qualityvisual - 1) : 1);
        for (t = 0; t < 3; t++)
            if (w[t] > .02)
                galaxysprite(GalaxyHalo, tint[t], s->p.x, s->p.y, .62 * span, halo * w[t] * (t == 2 ? 1.6 : 1.25));
    }
    if (s->urgent)  /* 请求关注: 卡片背后一圈脉动的红光 */
        galaxysprite(GalaxyHalo, GalaxyHot, s->p.x, s->p.y, MAX(40, .8 * span) * (1 + .12 * pulse), (.22 + .35 * pulse) * s->alpha);
    galaxyrenderwindow(s, s->vis, s->tint, s->brightness);
    a = s->glow * (1 - .7 * MIN(1, s->vis * 1.5));
    rad = 8.5 * r->starscale * (s->focused ? 1.18 : 1) * (1 + .3 * s->hover);
    for (t = 0; t < 3 && a > .002; t++)
        if (w[t] > .02)
            galaxyrenderglow(tint[t], s->p, rad, a * w[t], blur, .5 * r->glowscale * (1 + .5 * s->hover), 0);
    if (s->urgent && s->alpha > .05) {
        galaxyrenderglow(GalaxyHot, s->p, rad * 1.5 * (1 + .3 * pulse), MIN(1, (.6 + .4 * pulse) * s->alpha), blur, .8, .25);
        galaxysprite(GalaxySpike, GalaxyHot, s->p.x, s->p.y, MIN(220, 120 * MAX(.5, s->p.scale) * (1 + .3 * pulse)),
                .65 * pulse * s->alpha * galaxynearfade(s->p.z));
    }
    if (!s->hit && !s->died && (a > .05 || s->vis > .05)) {
        rad = MAX(16, 3 * rad * s->p.scale);
        s->bx0 = s->p.x - rad; s->by0 = s->p.y - rad;
        s->bx1 = s->p.x + rad; s->by1 = s->p.y + rad;
        s->hit = 1;
    }
}

static void
galaxyrenderitems(void)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyCore *g;
    GalaxyDust *d;
    double a, rad;
    int i;

    for (i = 0; i < r->nstars; i++)
        r->stars[i].hit = 0;
    for (i = 0; i < r->ntags; i++)
        r->galaxies[i].hit = 0;
    for (i = 0; i < r->nitems; i++) {
        switch (r->items[i].kind) {
        case GalaxyDustItem:
            d = &r->dust[r->items[i].index];
            a = d->light * r->dustfade * galaxysmoothstep(galaxyphase(d->p.z, r->cam.near * 1.5, r->cam.near * 3));
            if (r->mode == GalaxyOrbit) {
                if (r->items[i].index % 2)
                    a *= 1 - galaxyclamp(r->qualityvisual);
                if (r->items[i].index % 3)
                    a *= 1 - galaxyclamp(r->qualityvisual - 1);
            }
            rad = MAX(.6, d->size * d->p.scale);
            if (r->mode == GalaxyOrbit && !d->disk && d->size >= 8) {
                /* 闪烁星空: 各自频率明暗起伏, 每颗约 20s 一次短暂闪亮 */
                double tt = r->motion * r->tscale, sp = fmod(tt / 20 + d->tw * 7, 1);
                a *= .55 + .7 * (.5 + .5 * sin(tt * (1.3 + 2.1 * d->tw) + d->tw * 40));
                if (sp < .04) {
                    a *= 1 + 2.5 * sin(GALAXYPI * sp / .04);
                    rad *= 1 + .6 * sin(GALAXYPI * sp / .04);
                }
            }
            a *= 1 + 2 * d->boost;
            if (a < .003)
                break;
            galaxysprite(GalaxyHalo, GalaxyCool, d->p.x, d->p.y, rad, MIN(1, a));
            if (r->warpfx > .01) {
                /* 超空间跃迁: 尘埃沿屏幕中心向外拉成光线 */
                GalaxyProj q = d->p;
                q.x += (d->p.x - r->vx - r->vw * .5) * .25 * r->warpfx;
                q.y += (d->p.y - r->vy - r->vh * .5) * .25 * r->warpfx;
                galaxyband(&d->p, &q, MIN(.25, a * 1.2 * r->warpfx), MAX(.5, .45 * d->size * d->p.scale));
            }
            break;
        case GalaxyCoreItem:
            g = &r->galaxies[r->items[i].index];
            /* 近点 / 交会 / 涟漪 / 翻转时核心更亮, 光晕更大 */
            a = .35 * g->peri + .6 * g->flare + .7 * g->ripple + .25 * g->flip + 1.5 * g->ignite + 1.2 * g->callout
                + 1.5 * g->nova + 1.2 * g->bridge;
            /* 粒子恒星 (不再是明亮光斑 / 星芒): 事件发生时粒子团更亮、略微胀大; 外面只留一层很淡的光晕 */
            {
                double rgb[3], k = MIN(1, g->alpha) * (1 - .78 * g->eclipse) * galaxydepthlight(g->p.z), lc, pre = 0;
                int kk;

                galaxytintcolor(GALAXYTAGTINT(g->tag), -1, rgb);
                if (g - r->galaxies == r->novag && galaxycycle(r->motion, 9, 20, &kk, &lc) && lc < GALAXYNOVAPRE) {
                    pre = galaxysmoothstep(lc / GALAXYNOVAPRE);     /* 超新星爆发前: 收缩、变暗 */
                    k *= 1 - .5 * pre;
                }
                galaxyglstarball(g->pos, &g->plane, g->size * 2.4 * (1 + .12 * g->hover + .25 * MIN(2, a)) * (1 - .45 * pre), .8, rgb,
                        .09 * k * (1 + .6 * MIN(2, a) + .5 * g->hover), r->starscale,
                        (int)(2600 * (r->quiet ? .5 : 1)), 31.7 * (g->tag + 1));
                galaxysprite(GalaxyHalo, GALAXYTAGTINT(g->tag), g->p.x, g->p.y, MIN(160, g->size * 6 * g->p.scale),
                        .12 * k * (1 + .5 * MIN(2, a)) * galaxynearfade(g->p.z));
            }
            if (g->alpha > .2) {
                g->hx = g->p.x;
                g->hy = g->p.y;
                g->hr = MAX(18, 1.3 * g->size * g->p.scale);
                g->hit = 1;
            }
            break;
        case GalaxyStarItem:
            galaxyrenderstar(&r->stars[r->items[i].index]);
            break;
        case GalaxyRingItem:       /* 轨道环 / 群轨道 / 星轨不再画线: 由星体身后的粒子尾迹勾出 (galaxyrenderorbittrails) */
        case GalaxyClusterItem:
        case GalaxyStreakItem:
            break;
        case GalaxySunItem:
            galaxyrendersun();
            break;
        }
    }
    galaxyflushbands();
}

/* 冲击环: center 处 plane 的 xy 平面内的圆 (3D, 随透视变成椭圆) */
static void
galaxyringfx(GalaxyVec center, GalaxyMat plane, double rad, double a, double width)
{
    GalaxyProj pts[49];
    double th;
    int j;

    if (a < .004 || rad < 1)
        return;
    for (j = 0; j <= 48; j++) {
        th = 2 * GALAXYPI * j / 48;
        pts[j] = galaxyproject(galaxyadd(center, galaxyapply(plane, galaxyv(cos(th) * rad, sin(th) * rad, 0))));
    }
    for (j = 0; j < 48; j++)
        if (pts[j].ok && pts[j + 1].ok) {
            galaxyband(&pts[j], &pts[j + 1], a * galaxynearfade(pts[j].z), MAX(.8, width * pts[j].scale));
            galaxyband(&pts[j], &pts[j + 1], a * .3 * galaxynearfade(pts[j].z), MAX(2, 4 * width * pts[j].scale));
        }
}

/* 屏幕空间光带的一个点 */
static GalaxyProj
galaxysp(double x, double y)
{
    return (GalaxyProj){x, y, 1, 1, 1};
}

/* 镜头空间的点 -> 世界坐标 (镜头前方 z 处; x 右, y 下) */
static GalaxyVec
galaxycampoint(double x, double y, double z)
{
    GalaxyScene *r = &galaxyscene;

    return galaxyadd(r->cam.pos, galaxyapply(r->cam.rot, galaxyv(x, y, z)));
}

/* 开场 B: 星门. 一座正对镜头的光环从远处迎面而来, 环上 12 个光点跑动, 镜头从环中穿过, 穿过的一刻白光一闪 */
static void
galaxyrendergate(double f, double s)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyProj pts[49], lp;
    double F = r->cam.focal, u = galaxyphase(s, .4, 1.3), z, R = .4 * r->vw, env, th, spin = 2 * GALAXYPI * .3 * s, a, w;
    int j, k, ring;

    if (u > 0 && u < 1) {
        z = F * galaxymix(3.4, .12, galaxyeaseincubic(u));
        /* 离得很近时淡掉 (光带会非常宽, 也已经出了画面) */
        env = f * galaxysmoothstep(u / .2) * galaxysmoothstep((z / F - .2) / .5);
        for (ring = 0; ring < 2 && env > .01; ring++) {
            galaxybandcolor(galaxytintrgb(ring ? GalaxyViolet : GalaxyCyan, ring ? -1 : .4));   /* 星门: 青色外环, 紫色内环 */
            for (j = 0; j <= 48; j++) {
                th = 2 * GALAXYPI * j / 48 + (ring ? -spin : spin) * .3;
                pts[j] = galaxyproject(galaxycampoint(R * (ring ? .9 : 1) * cos(th), R * (ring ? .9 : 1) * sin(th), z));
            }
            for (j = 0; j < 48; j++)
                if (pts[j].ok && pts[j + 1].ok) {
                    w = MIN(6, pts[j].scale);
                    galaxyband(&pts[j], &pts[j + 1], env * (ring ? .35 : .85), MAX(1, (ring ? 1.6 : 4) * w));
                    if (!ring)
                        galaxyband(&pts[j], &pts[j + 1], env * .16, MAX(4, 22 * w));
                }
        }
        for (k = 0; k < 12 && env > .01; k++) {
            th = spin * 1.6 + 2 * GALAXYPI * k / 12;
            lp = galaxyproject(galaxycampoint(R * cos(th), R * sin(th), z));
            if (!lp.ok)
                continue;
            a = env * (.7 + .3 * sin(spin * 3 + k));
            galaxysprite(GalaxyHalo, k % 3 ? GalaxyCyan : GalaxyViolet, lp.x, lp.y, MAX(6, 26 * MIN(6, lp.scale)), MIN(1, a));
            if (k % 3 == 0)
                galaxysprite(GalaxySpike, GalaxyCyan, lp.x, lp.y, MIN(300, 110 * MIN(4, lp.scale)), .8 * a);
        }
        /* 外圈 9 个 V 形标记, 反向慢转 (星门的「锁定环」) */
        galaxybandcolor(galaxytintrgb(GalaxyViolet, -1));
        for (k = 0; k < 9 && env > .01; k++) {
            th = -spin * .5 + 2 * GALAXYPI * k / 9;
            for (j = 0; j < 3; j++)
                pts[j] = galaxyproject(galaxycampoint(R * (1.12 - .06 * (j == 1)) * cos(th + (j - 1) * .045),
                            R * (1.12 - .06 * (j == 1)) * sin(th + (j - 1) * .045), z));
            if (pts[0].ok && pts[1].ok && pts[2].ok) {
                galaxyband(&pts[0], &pts[1], .6 * env, MAX(1, 2.4 * MIN(6, pts[1].scale)));
                galaxyband(&pts[1], &pts[2], .6 * env, MAX(1, 2.4 * MIN(6, pts[1].scale)));
            }
        }
        /* 环心涌出的光流, 迎面而来 */
        for (k = galaxyemitcount(500 * env * galaxysmoothstep((u - .5) / .3)); k > 0; k--) {
            th = 2 * GALAXYPI * galaxyprand();
            a = R * .8 * sqrt(galaxyprand());
            galaxyemit(galaxycampoint(a * cos(th), a * sin(th), z), galaxyapply(r->cam.rot, galaxyv(0, 0, -F * (.6 + .6 * galaxyprand()))),
                    .5 + .3 * galaxyprand(), .0025 * F, galaxyprand() < .5 ? GalaxyCyan : GalaxyViolet, .7, 0, 0);
        }
        /* 环心: 一团淡淡的光, 越近越亮 */
        lp = galaxyproject(galaxycampoint(0, 0, z));
        if (lp.ok)
            galaxysprite(GalaxyHalo, GalaxyCyan, lp.x, lp.y, MIN(900, .55 * R * lp.scale), .25 * env);
        galaxybandwhite();
    }
    galaxyflushbands();
    a = f * galaxyflash(s - 1.18, .04, 12);
    if (a > .01)    /* 穿过星门: 青白色闪光 */
        galaxyglrect(r->vx, r->vy, r->vw, r->vh, (double[3]){.85, .97, 1}, .08 * MIN(1, a) * (r->gentle ? .25 : 1));
}

/* 开场 C: 大爆炸. 焦点窗口中心爆闪, 两圈冲击波, 粒子向四周喷射; 核心从爆心飞出 (galaxyupdatecores) 后再点火 */
static void
galaxyrenderbang(double f, double s)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyProj c = galaxyproject(r->bang), pts[65], a0, a1;
    double t = s - .3, u, a, th, sp, d0, d1, R;
    int i, j;

    if (t < -.05 || t > 1.4 || !c.ok)
        return;
    /* 爆发前的一瞬: 光点向内收紧 */
    if (t < 0) {
        a = f * galaxysmoothstep((t + .05) / .05);
        galaxysprite(GalaxyHalo, GalaxyGold, c.x, c.y, 30, a);
        return;
    }
    a = f * galaxyflash(t, .04, 2.2);
    if (r->bangburst < 0) {     /* 爆发的一刻: 金橙色的粒子向四面八方喷射 */
        r->bangburst = 1;
        for (i = 0; i < 700; i++)
            galaxyemit(r->bang, galaxyscale(galaxynormalize(galaxyprandvec(1)), r->cam.focal * (.12 + .7 * pow(galaxyprand(), 1.5))),
                    1.2 + .9 * galaxyprand(), .003 * r->cam.focal, i % 3 ? GalaxyGold : i % 2 ? GalaxyOrange : GalaxyWarm, .85, 1.5, 0);
    }
    galaxysprite(GalaxyHalo, GalaxyGold, c.x, c.y, 60 + 380 * galaxyeaseoutcubic(t / .5), MIN(1, a));
    galaxysprite(GalaxySpike, GalaxyGold, c.x, c.y, MIN(600, 520 * (.5 + .5 * a)), MIN(1, 1.2 * a));
    galaxylensflare(c.x, c.y, a);
    if (f * galaxyflash(t, .03, 7) > .01)  /* 爆闪: 暖白 */
        galaxyglrect(r->vx, r->vy, r->vw, r->vh, (double[3]){1, .93, .8}, .1 * MIN(1, f * galaxyflash(t, .03, 12)) * (r->gentle ? .25 : 1));
    for (i = 0; i < 2; i++) {
        u = galaxyphase(t, .15 * i, .8 + .25 * i);
        if (u <= 0 || u >= 1)
            continue;
        R = (i ? .55 : .95) * r->vw * galaxyeaseoutcubic(u);
        for (j = 0; j <= 64; j++) {
            th = 2 * GALAXYPI * j / 64;
            pts[j] = galaxysp(c.x + cos(th) * R, c.y + sin(th) * R * .62);
        }
        for (j = 0; j < 64; j++) {
            galaxybandcolor(galaxytintrgb(i ? GalaxyOrange : GalaxyGold, .5));     /* 大爆炸: 金 / 橙冲击波 */
            galaxyband(&pts[j], &pts[j + 1], f * (i ? .14 : .26) * pow(1 - u, 1.4), 2 + 2 * u);
            galaxyband(&pts[j], &pts[j + 1], f * .06 * pow(1 - u, 1.4), 10 + 14 * u);
        }
    }
    /* 喷射的粒子: 各自方向和速度, 减速飞散, 拉出短线 */
    u = galaxyphase(t, 0, 1.3);
    for (i = 0; i < GALAXYBANGN && u < 1; i++) {
        th = 2 * GALAXYPI * galaxyhash(i * 3 + 7001);
        sp = r->vw * (.15 + .7 * pow(galaxyhash(i * 3 + 7002), 1.5));
        d1 = sp * galaxyeaseoutcubic(u);
        d0 = sp * galaxyeaseoutcubic(MAX(0, u - .06));
        a0 = galaxysp(c.x + cos(th) * d0, c.y + sin(th) * d0 * .7);
        a1 = galaxysp(c.x + cos(th) * d1, c.y + sin(th) * d1 * .7);
        a = f * pow(1 - u, 1.2) * (.5 + .5 * galaxyhash(i * 3 + 7003));
        galaxybandcolor(galaxytintrgb(i % 3 ? GalaxyGold : GalaxyOrange, .6));
        galaxyband(&a0, &a1, .5 * a, 1.1);
        galaxysprite(GalaxyHalo, i % 3 ? GalaxyGold : GalaxyOrange, a1.x, a1.y, 4 + 5 * galaxyhash(i * 3 + 7003), a);
    }
}

/* 开场的一次性光效: 起飞冲击波 / 核心点火冲击环 / 中心光源点火 / 转速峰值的盘面冲击环 */
static void
galaxyrenderfx(void)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyCore *g;
    GalaxyProj pts[65], c = {0};
    GalaxyMat disk;
    double f = galaxybeatw(), s = r->iclock, u, t;
    int i, j;

    if (f < .01)
        return;
    if (r->variant == GalaxyGate)
        galaxyrendergate(f, s);
    else if (r->variant == GalaxyBang)
        galaxyrenderbang(f, s);
    galaxybandwhite();
    /* 起飞: 以焦点窗口为圆心, 屏幕空间的一圈光环 (大爆炸有自己的冲击波) */
    u = galaxyphase(s, .08, .7);
    if (u > 0 && u < 1 && r->variant != GalaxyBang) {
        c.x = r->vx + r->vw * .5;
        c.y = r->vy + r->vh * .5;
        for (i = 0; i < r->nstars; i++)
            if (r->stars[i].focused && r->stars[i].p.ok)
                c = r->stars[i].p;
        for (j = 0; j <= 64; j++) {
            t = 2 * GALAXYPI * j / 64;
            pts[j] = (GalaxyProj){c.x + cos(t) * .7 * r->vw * galaxyeaseoutcubic(u), c.y + sin(t) * .7 * r->vw * galaxyeaseoutcubic(u), 1, 1, 1};
        }
        for (j = 0; j < 64; j++) {
            galaxyband(&pts[j], &pts[j + 1], f * .22 * pow(1 - u, 1.5), 1.5 + 2 * u);
            galaxyband(&pts[j], &pts[j + 1], f * .07 * pow(1 - u, 1.5), 8 + 10 * u);
        }
    }
    /* 核心点火: 在各自轨道平面里扩散的冲击环 */
    for (i = 0; i < r->ntags; i++) {
        g = &r->galaxies[i];
        t = galaxyignitet(g);
        galaxybandwhite();
        u = galaxyphase(s, t, t + .5);
        if (u > 0 && u < 1 && g->p.ok) {   /* 核心点火: 各自的 tag 色 */
            galaxybandcolor(galaxytintrgb(GALAXYTAGTINT(g->tag), .55));
            galaxyringfx(g->pos, g->plane, 2.4 * g->radius * galaxyeaseoutcubic(u), f * .24 * pow(1 - u, 1.3), 1.4);
        }
    }
    galaxybandwhite();
    /* 中心光源点火, 以及转速峰值时: 盘面 (xz) 上的大冲击环 */
    disk = galaxymul(r->world, galaxyrotx(GALAXYPI / 2));
    u = galaxyphase(s, 1.5, 2.1);
    if (u > 0 && u < 1) {
        galaxybandcolor(galaxytintrgb(GalaxyGold, .3));
        galaxyringfx(galaxyv(0, 0, 0), disk, .45 * r->vw * galaxyeaseoutcubic(u), f * .22 * (1 - u), 1.6);
        galaxybandwhite();
    }
    u = galaxyphase(s, 2.85, 3.5);
    if (u > 0 && u < 1)
        galaxyringfx(galaxyv(0, 0, 0), disk, .7 * r->vw * galaxyeaseoutcubic(u), f * .26 * (1 - u), 2.2);
    galaxyflushbands();
}

/* 星际尘埃流: 沿三条群轨道流动的细碎粒子, 按开普勒运动在近点加速, 各带一小段尾巴 */
static void
galaxyrenderriver(double f)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyProj a, b;
    GalaxyVec v;
    double t = r->motion * r->tscale, h1, h2, h3, M, sc, al;
    int i, l, n = r->quiet ? GALAXYRIVER / 2 : GALAXYRIVER;

    for (i = 0; i < n; i++) {
        l = i % GALAXYLANES;
        if (!(r->lanemask & 1 << l))
            continue;
        h1 = galaxyhash(i * 3 + 9001);
        h2 = galaxyhash(i * 3 + 9002);
        h3 = galaxyhash(i * 3 + 9003);
        /* 比核心快一些 (周期的 1/1.3–1/1.9), 粒子从核心旁边流过 */
        M = 2 * GALAXYPI * (h1 + galaxylanes[l].dir * t * (1.3 + .6 * h2) / galaxylanes[l].period);
        sc = 1 + .09 * (h2 - .5);
        v = galaxylanepoint(l, galaxykepler(M, galaxylanes[l].e));
        a = galaxyproject(galaxyapply(r->world, galaxyv(v.x * sc, v.y + (h3 - .5) * .03 * r->vw, v.z * sc)));
        v = galaxylanepoint(l, galaxykepler(M - galaxylanes[l].dir * .05, galaxylanes[l].e));
        b = galaxyproject(galaxyapply(r->world, galaxyv(v.x * sc, v.y + (h3 - .5) * .03 * r->vw, v.z * sc)));
        if (!a.ok)
            continue;
        al = f * (.25 + .3 * h3) * galaxynearfade(a.z) * (r->mode == GalaxyOrbit ? 1 - .5 * galaxyclamp(r->qualityvisual - 1) : 1);
        if (al < .01)
            continue;
        galaxybandcolor(galaxytintrgb(galaxylanetint[l], .35));   /* 尘埃流带上所在群轨道的颜色 */
        if (b.ok)
            galaxyband(&a, &b, .35 * al, MAX(.4, .9 * a.scale));
        galaxysprite(GalaxyHalo, h3 < .3 ? GalaxyWarm : galaxylanetint[l], a.x, a.y, MAX(1.5, 4.5 * a.scale * r->starscale), al);
    }
    galaxybandwhite();
}

/* 彗星 (粒子): 彗头一小团粒子彗发; 蓝色离子尾是笔直背向中心光源的快速粒子流 (ion, 长度即向量长度);
 * 白黄色尘埃尾宽一些、慢一些, 从来路方向 (back, 单位向量) 弯向离子尾一侧. w: 宽度倍数, seed: 每颗彗星不同 */
static void
galaxycomettails(GalaxyVec head, GalaxyVec ion, GalaxyVec back, double env, double w, double seed)
{
    GalaxyScene *r = &galaxyscene;
    static const double blue0[3] = {.75, .9, 1}, blue1[3] = {.3, .45, 1}, dust0[3] = {1, .97, .9};
    double L = galaxylen(ion), t = galaxynow();
    GalaxyVec bend = galaxyscale(ion, .2), dend = galaxyadd(head, galaxyadd(galaxyscale(back, .8 * L), galaxyscale(ion, .6)));
    GalaxyMat flat = {{{0, 0, 0}, {0, 0, 0}, {0, 0, 0}}}, curve = {{{0, bend.x, 0}, {0, bend.y, 0}, {0, bend.z, 0}}};
    double rgb[3] = {.85, .92, 1};
    static const double pale1[3] = {.95, .85, .65};

    if (env < .01 || L < 1)
        return;
    galaxyglemitter(2, head, &flat, galaxyadd(head, ion), 1.3, -.05 * L * w, 1, 1, t, blue0, blue1, .4 * env, .8 * r->starscale,
            1800, seed);
    galaxyglemitter(2, head, &curve, dend, .35, -.16 * L * w, 1, 1, t, dust0, pale1, .26 * env, .9 * r->starscale, 3500, seed + 1.7);
    galaxyglstarball(head, &r->world, .035 * L * w, 2, rgb, .12 * env, .9 * r->starscale, 500, seed + 3.1);
}

/* 驻留特效: 轨道光流 / 星座连线 / 核心光桥 / 超新星冲击环 / 彗星 / 流星. 都是 motion 的函数, 强度乘 holdw */
static void
galaxyrenderholdfx(void)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyCore *g, *h;
    GalaxyStar *st;
    GalaxyProj pts[13], c, hp;
    GalaxyMat m;
    GalaxyVec v, head, tail, A, B, perp;
    double f = r->holdw, t = r->motion * r->tscale, local, env, u, th, rad, ang, len, wave, phi, R;
    int i, j, k, l, n, order[32];

    if (f < .01 || (r->mode != GalaxyOrbit && r->mode != GalaxyCollapse))
        return;
    galaxyrenderriver(f);
    /* 轨道光流: 每条群椭圆 6 个光点, 每条局部环 2 个, 沿轨道流动, 带短尾 (安静模式不画) */
    for (l = 0; l < GALAXYLANES && !r->quiet; l++) {
        if (!(r->lanemask & 1 << l))
            continue;
        galaxybandcolor(galaxytintrgb(galaxylanetint[l], .45));
        for (i = 0; i < 6; i++) {
            for (j = 0; j <= 4; j++) {
                th = 2 * GALAXYPI * i / 6 + galaxylanes[l].dir * (.3 * t - .045 * j);
                v = galaxylanepoint(l, th);
                pts[j] = galaxyproject(galaxyapply(r->world, v));
            }
            wave = r->rippleamp * exp(-pow((galaxylen(v) - r->ripple) / (.04 * r->vw), 2));
            for (j = 0; j < 4; j++)
                if (pts[j].ok && pts[j + 1].ok)
                    galaxyband(&pts[j], &pts[j + 1], f * .2 * (1 + 2 * wave) * (1 - j / 4.0) * galaxynearfade(pts[j].z),
                            MAX(.6, 1.6 * pts[j].scale));
            if (pts[0].ok)
                galaxysprite(GalaxyHalo, galaxylanetint[l], pts[0].x, pts[0].y, MAX(3, 10 * pts[0].scale * r->starscale),
                        f * .5 * (1 + wave) * galaxynearfade(pts[0].z));
        }
    }
    for (i = 0; i < r->ntags && !r->quiet; i++) {
        g = &r->galaxies[i];
        if (!g->nrings || g->alpha < .1 || !g->p.ok)
            continue;
        galaxybandcolor(galaxytintrgb(GALAXYTAGTINT(g->tag), .5));
        for (k = 0; k < g->nrings; k++) {
            m = galaxymul(g->plane, g->ring[k]);
            rad = g->ringr[k] * galaxybreathe(g, r->motion);
            for (l = 0; l < 2; l++) {
                for (j = 0; j <= 3; j++) {
                    th = (1.1 + .2 * k) * (k == 1 ? -1 : 1) * (t - .1 * j) + GALAXYPI * l + g->phase;
                    pts[j] = galaxyproject(galaxyadd(g->pos, galaxyapply(m, galaxyv(cos(th) * rad, sin(th) * rad, 0))));
                }
                for (j = 0; j < 3; j++)
                    if (pts[j].ok && pts[j + 1].ok)
                        galaxyband(&pts[j], &pts[j + 1], f * .18 * g->alpha * (1 - j / 3.0) * galaxynearfade(pts[j].z),
                                MAX(.6, 1.3 * pts[j].scale));
                if (pts[0].ok)
                    galaxysprite(GalaxyHalo, GALAXYTAGTINT(g->tag), pts[0].x, pts[0].y, MAX(2.5, 7 * pts[0].scale * r->starscale),
                            f * .45 * g->alpha * galaxynearfade(pts[0].z));
            }
        }
    }
    f *= r->calm;   /* 以下是随机特效: 天象 (诞生 / 流星 / 通知 / 报时) 发生时让位 */
    /* 星座连线: 按轨道角把这个星系的窗口卡片连起来, 首颗再连到核心 */
    if (r->npop && galaxycycle(r->motion, 2, 4, &k, &local)) {
        g = &r->galaxies[r->constg >= 0 ? r->constg : r->popord[k % r->npop]];
        env = f * galaxysmoothstep(local / .4) * (1 - galaxysmoothstep((local - 1.6) / 1));
        for (i = n = 0; i < r->nstars && n < 32; i++)
            if (r->stars[i].galaxy == g->tag && r->stars[i].p.ok && !r->stars[i].died)
                order[n++] = i;
        for (i = 1; i < n; i++)     /* 按轨道角插入排序 (每个星系的星不多) */
            for (j = i; j > 0 && fmod(galaxyorbitangle(&r->stars[order[j]], GALAXYHOLD, r->motion) + 100 * GALAXYPI, 2 * GALAXYPI)
                    < fmod(galaxyorbitangle(&r->stars[order[j - 1]], GALAXYHOLD, r->motion) + 100 * GALAXYPI, 2 * GALAXYPI); j--) {
                l = order[j]; order[j] = order[j - 1]; order[j - 1] = l;
            }
        galaxybandcolor(galaxytintrgb(GALAXYTAGTINT(g->tag), .4));
        if (env > .01 && n) {
            for (i = 0; i < n; i++) {
                st = &r->stars[order[i]];
                hp = r->stars[order[(i + 1) % n]].p;
                if (n > 1 && (n > 2 || i == 0))
                    galaxyband(&st->p, &hp, .2 * env, 1.1);
                galaxysprite(GalaxyHalo, GALAXYTAGTINT(g->tag), st->p.x, st->p.y, 12, .6 * env);
            }
            if (g->p.ok)
                galaxyband(&r->stars[order[0]].p, &g->p, .14 * env, .9);
        }
    }
    /* 核心光桥: 一道光线连起两个核心, 一个亮点沿光线跑过去 */
    if (r->bri >= 0 && r->brj >= 0 && galaxycycle(r->motion, 3, 7, &k, &local) && k == r->bridgek) {
        g = &r->galaxies[r->bri];
        h = &r->galaxies[r->brj];
        env = f * galaxysmoothstep(local / .25) * (1 - galaxysmoothstep((local - 1.3) / .6));
        if (env > .01 && g->p.ok && h->p.ok) {
            galaxybandcolor(galaxymixrgb(galaxytintrgb(GALAXYTAGTINT(g->tag), .45), galaxytintrgb(GALAXYTAGTINT(h->tag), .45), .5));
            galaxyband(&g->p, &h->p, .14 * env, 1.2);
            galaxyband(&g->p, &h->p, .05 * env, 5);
            u = galaxyeaseinoutcubic(galaxyphase(local, .25, 1.15));
            if (u > 0 && u < 1)
                galaxysprite(GalaxyHalo, GALAXYTAGTINT(u < .5 ? g->tag : h->tag), galaxymix(g->p.x, h->p.x, u), galaxymix(g->p.y, h->p.y, u), 18, .9 * env);
        }
    }
    /* 超新星: 核心先收缩变暗 GALAXYNOVAPRE 秒, 然后一个粒子球壳爆发 (白 -> 蓝紫 -> 橙, 带纤维结构, 减速扩张),
     * 再留下一团慢慢散开的残骸星云 */
    if (r->novag >= 0 && galaxycycle(r->motion, 9, 20, &k, &local) && local < 6 && local > GALAXYNOVAPRE) {
        static const double white[3] = {1, .97, .92}, violet[3] = {.62, .45, 1}, orange[3] = {1, .55, .25}, rose[3] = {.9, .45, .75};
        double R, et = local - GALAXYNOVAPRE;

        g = &r->galaxies[r->novag];
        R = 3.4 * MAX(g->size * 7, g->nrings ? g->ringr[g->nrings - 1] : g->size * 7);
        galaxyglemitter(0, g->pos, &g->plane, g->pos, R / 3, 0, 3, .12, et, violet, orange, .35 * f, r->starscale,
                (int)(8000 * (r->quiet ? .5 : 1)), 5.3 + k);
        galaxyglemitter(0, g->pos, &g->plane, g->pos, R * .35 / 5, 0, 5, .5, et, white, rose, .14 * f, r->starscale * .9,
                (int)(3000 * (r->quiet ? .5 : 1)), 9.1 + k);
    }
    /* 彗星: 每 15s 一颗, 从星系群外侧穿过盘面, 尾巴背向中心光源 */
    if (!r->quiet && galaxycycle(r->motion, 6, 15, &k, &local) && local < 5) {
        u = local / 5;
        phi = 2 * GALAXYPI * galaxyhash(k * 3 + 5);
        R = .75 * r->vw;
        perp = galaxyv(-sin(phi) * .18 * r->vw, 0, cos(phi) * .18 * r->vw);
        A = galaxyadd(galaxyv(R * cos(phi), -.12 * r->vw, R * sin(phi)), perp);
        B = galaxyadd(galaxyv(-.9 * R * cos(phi), .08 * r->vw, -.9 * R * sin(phi)), perp);
        head = galaxyapply(r->world, galaxylerp(A, B, u));
        tail = galaxyscale(galaxynormalize(head), .2 * r->vw);
        env = f * galaxysmoothstep(u / .1) * (1 - galaxysmoothstep((u - .85) / .15));
        galaxycomettails(head, tail, galaxynormalize(galaxyapply(r->world, galaxysub(A, B))), env, 1, 21.7 + k);
        pts[0] = galaxyproject(head);
        if (pts[0].ok && env * galaxynearfade(pts[0].z) > .01)
            galaxyrenderglow(GalaxyCool, pts[0], 10, MIN(1, .9 * env) * galaxynearfade(pts[0].z), galaxydepthblur(pts[0].z), .7, .2);
    }
    /* 流星: 每 2.6s 一颗, 大致沿轨道盘面的对角线方向 (右上 -> 左下) 划过画面 */
    if (!r->quiet && galaxycycle(r->motion, .8, 2.6, &k, &local) && local < 1.1) {
        u = local / 1.1;
        ang = (180 - GALAXYDIAG + (galaxyhash(k * 7 + 11) - .5) * 30) * GALAXYPI / 180;
        len = r->vw * (.5 + .3 * galaxyhash(k * 7 + 13));
        /* 起点在画面上边或右边之外, 流星从边缘飞入, 不会在画面中间凭空出现 */
        c = galaxyhash(k * 7 + 12) < .55
            ? galaxysp(r->vx + r->vw * (.3 + .7 * galaxyhash(k * 7 + 14)), r->vy - .06 * r->vh)
            : galaxysp(r->vx + 1.04 * r->vw, r->vy + r->vh * (.05 + .45 * galaxyhash(k * 7 + 14)));
        env = f * galaxysmoothstep(u / .2) * (1 - galaxysmoothstep((u - .75) / .25));
        hp = galaxysp(c.x + cos(ang) * len * galaxyeaseoutcubic(u), c.y + sin(ang) * len * galaxyeaseoutcubic(u));
        for (j = 0; j <= 12; j++)
            pts[j] = galaxysp(hp.x - cos(ang) * .22 * r->vw * j / 12 * MIN(1, u * 3), hp.y - sin(ang) * .22 * r->vw * j / 12 * MIN(1, u * 3));
        l = galaxyhash(k * 7 + 15) < .5 ? GalaxyGreen : GalaxyOrange;     /* 流星: 绿或橙 */
        for (j = 0; j < 12; j++) {
            galaxybandcolor(galaxytintrgb(l, j < 2 ? .3 : .7));
            galaxyband(&pts[j], &pts[j + 1], .25 * env * pow(1 - j / 12.0, 1.5), 1.7 - 1.3 * j / 12);
        }
        galaxysprite(GalaxyHalo, l, hp.x, hp.y, 12, .8 * env);
        for (j = galaxyemitcount(150 * env); j > 0; j--)   /* 流星头部迸出的火花 */
            galaxyemit(galaxyv(hp.x, hp.y, 0), galaxyv(-cos(ang) * 120 * (.4 + galaxyprand()) + (galaxyprand() - .5) * 60,
                        -sin(ang) * 120 * (.4 + galaxyprand()) + (galaxyprand() - .5) * 60, 0),
                    .35 + .35 * galaxyprand(), 1.6 + 1.6 * galaxyprand(), l, .75, 2, 1);
    }
    galaxybandwhite();
    galaxyflushbands();
}

/* 带透明度的文字 (先画一层暗影保证在亮处也看得清). Xft 的颜色按预乘处理 */
static void
galaxytext(XftFont *font, double x, double y, const char *text, double a, int center)
{
    GalaxyScene *r = &galaxyscene;
    XRenderColor rc;
    XftColor col;
    XGlyphInfo ext;
    int len = strlen(text), k;

    if (!r->titledraw || !font || a < .02 || !len)
        return;
    if (center) {
        XftTextExtentsUtf8(dpy, font, (XftChar8 *)text, len, &ext);
        x -= ext.xOff * .5;
    }
    for (k = 0; k < 2; k++) {
        double v = k ? 1 : 0, al = k ? a : .55 * a;
        rc.red = (unsigned short)(0xe900 * v * al);
        rc.green = (unsigned short)(0xf400 * v * al);
        rc.blue = (unsigned short)(0xffff * v * al);
        rc.alpha = (unsigned short)(0xffff * al);
        if (!XftColorAllocValue(dpy, r->argbvisual, r->argbcmap, &rc, &col))
            return;
        XftDrawStringUtf8(r->titledraw, &col, font, (int)x + (k ? 0 : 1), (int)y + (k ? 0 : 2), (XftChar8 *)text, len);
        XftColorFree(dpy, r->argbvisual, r->argbcmap, &col);
    }
}

/* 脉冲星: 一个空 tag 的核心 (没有空的就选最外圈的) 沿磁轴喷出两股相反的粒子流, 绕倾斜的轴每 14s 扫一圈, 扫向镜头时闪亮 */
static void
galaxyrenderpulsar(double f)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyCore *g;
    GalaxyVec axis, u, v, dir, tocam;
    double t = r->motion * r->tscale, th, align, L = .32 * r->vw;
    int i, side;

    if (r->pulsar == -1) {
        r->pulsar = -2;
        for (i = 0; i < r->ntags && r->pulsar < 0; i++)
            if (!r->galaxies[i].nstars)
                r->pulsar = i;
        for (i = 0; i < r->ntags && r->pulsar < 0; i++)
            if (r->galaxies[i].lane == GALAXYLANES - 1)
                r->pulsar = i;
        if (r->log && r->pulsar >= 0)
            fprintf(r->log, "galaxy pulsar: tag %d\n", r->pulsar + 1), fflush(r->log);
    }
    if (r->pulsar < 0 || r->quiet)
        return;
    g = &r->galaxies[r->pulsar];
    f *= 1 - g->fill;           /* 这个 tag 诞生了窗口: 脉冲星平息, 变回普通星系 */
    if (f < .01 || !g->p.ok || g->alpha < .05)
        return;
    axis = galaxyapply(g->plane, galaxyv(.34, .94, 0));
    u = galaxynormalize(galaxycross(axis, galaxyv(0, 0, 1)));
    v = galaxycross(axis, u);
    th = 2 * GALAXYPI * t / 14;
    dir = galaxyadd(galaxyscale(axis, .5), galaxyscale(galaxyadd(galaxyscale(u, cos(th)), galaxyscale(v, sin(th))), .866));
    tocam = galaxynormalize(galaxysub(r->cam.pos, g->pos));
    /* 两股粒子喷流沿磁轴向外喷出 (青 -> 蓝紫), 随轴刚性地扫动, 像灯塔的光束; 越远越淡越散 */
    {
        static const double cyan[3] = {.55, .95, 1}, violet[3] = {.5, .4, 1};
        GalaxyVec u2 = galaxynormalize(galaxycross(dir, galaxyv(0, 1, 0))), v2 = galaxycross(dir, u2);
        GalaxyMat jb = {{{u2.x, v2.x, dir.x}, {u2.y, v2.y, dir.y}, {u2.z, v2.z, dir.z}}};

        galaxyglemitter(1, g->pos, &jb, g->pos, L / 1.6, .07, 1.6, 0, t, cyan, violet, .3 * f * g->alpha, .9 * r->starscale,
                4000, 3.3);
    }
    for (side = 0; side < 2; side++) {
        align = MAX(0, galaxydot(dir, tocam) * (side ? -1 : 1));
        if (align > .7)
            galaxysprite(GalaxySpike, GalaxyCyan, g->p.x, g->p.y, MIN(240, 160 * MAX(.5, g->p.scale) * align),
                    MIN(1, f * pow(align, 8)) * galaxynearfade(g->p.z));
    }
    /* 核心本身按 0.9s 的周期脉动 */
    galaxysprite(GalaxyHalo, GalaxyCyan, g->p.x, g->p.y, MAX(6, 26 * g->p.scale * r->starscale),
            f * g->alpha * .5 * pow(.5 + .5 * cos(2 * GALAXYPI * t / .9), 4) * galaxynearfade(g->p.z));
}

/* 通知彗星显示的文字. 屏保模式 (无人看着的屏幕) 只显示应用名, 不显示消息摘要 */
static const char *
galaxynotetext(const char *note, char *buf, size_t n)
{
    const char *sep;

    if (!galaxyscene.saver)
        return note;
    sep = strstr(note, ": ");
    snprintf(buf, n, "%.*s: 新通知", sep ? (int)(sep - note) : (int)strlen(note), note);
    return buf;
}

/* 天象: 新星诞生的闪光 / 关闭窗口的流星尾 / 通知彗星和标题 / 整点光波和时间 */
static void
galaxyrenderevents(void)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyStar *s;
    GalaxyProj pts[11], c;
    GalaxyVec head, tail, dir, A, B, C;
    GalaxyMat disk;
    double now = galaxynow(), f = r->holdw, t, u, env, a, L, z, sx, sy, h;
    char clock[8], buf[160];
    int i, j, ntext = 0;
    struct GalaxyText { double x, y, a; int note; } text[GALAXYNOTES + 1];

    if (r->mode != GalaxyIntro && r->mode != GalaxyOrbit && r->mode != GalaxyCollapse)
        return;
    galaxyrenderpulsar(f);
    for (i = 0; i < r->nstars; i++) {
        s = &r->stars[i];
        if (s->born > 0 && s->p.ok) {
            t = now - s->born;
            env = galaxyflash(t, .15, 2.4) * galaxynearfade(s->p.z);
            if (t <= r->pdt + 1e-6)     /* 诞生的一刻: 一圈青白色的光尘向外散开 */
                for (j = 0; j < 160; j++)
                    galaxyemit(s->pos, galaxyscale(galaxynormalize(galaxyprandvec(1)), r->cam.focal * (.04 + .12 * galaxyprand())),
                            1 + .6 * galaxyprand(), .0024 * r->cam.focal, j % 4 ? GalaxyCyan : GALAXYTAGTINT(s->galaxy), .8, 1.4, 0);
            galaxysprite(GalaxySpike, GalaxyCyan, s->p.x, s->p.y, MIN(340, 300 * MAX(.5, s->p.scale) * (.6 + .4 * env)), MIN(1, 1.2 * env));
            galaxysprite(GalaxyHalo, GalaxyCyan, s->p.x, s->p.y, MAX(12, 110 * s->p.scale * galaxyeaseoutcubic(t / 1.2)),
                    .6 * env);
            u = galaxyphase(t, .1, 1.3);
            if (u > 0 && u < 1) {   /* 新星诞生: 青白的环 */
                galaxybandcolor(galaxytintrgb(GalaxyCyan, .35));
                galaxyringfx(s->pos, galaxymul(r->galaxies[s->galaxy].plane, r->galaxies[s->galaxy].ring[s->ring]),
                        .11 * r->vw * galaxyeaseoutcubic(u), .45 * (1 - u), 1.8);
                galaxybandwhite();
            }
        }
        if (s->died > 0 && s->p.ok && (t = now - s->died) < 1.6) {
            /* 流星尾: 沿速度反方向, 越飞越长 */
            dir = galaxynormalize(s->vel);
            L = r->cam.focal * (.05 + .5 * galaxyeaseoutcubic(galaxyphase(t, .2, 1.2)));
            env = galaxysmoothstep(t / .25) * (1 - galaxysmoothstep(galaxyphase(t, 1.1, 1.6)));
            /* 化作流星: 关闭的一刻迸出一团火花, 飞行中持续向后洒出火星 */
            for (j = t <= r->pdt + 1e-6 ? 90 : galaxyemitcount(100 * env); j > 0; j--)
                galaxyemit(galaxyadd(s->pos, galaxyprandvec(.004 * r->cam.focal)),
                        galaxyadd(galaxyscale(dir, -.06 * r->cam.focal * galaxyprand()), galaxyprandvec((t <= r->pdt + 1e-6 ? .12 : .03) * r->cam.focal)),
                        .6 + .5 * galaxyprand(), .0022 * r->cam.focal, j % 3 ? GalaxyOrange : GalaxyGold, .8, 1.2, 0);
            for (j = 0; j <= 10; j++)
                pts[j] = galaxyproject(galaxysub(s->pos, galaxyscale(dir, L * j / 10)));
            for (j = 0; j < 10; j++)
                if (pts[j].ok && pts[j + 1].ok) {
                    a = env * pow(1 - j / 10.0, 1.4) * galaxynearfade(pts[j].z);
                    galaxybandcolor(galaxytintrgb(GalaxyOrange, j < 2 ? .3 : .7));    /* 关窗流星: 偏橙 */
                    galaxyband(&pts[j], &pts[j + 1], .7 * a, MAX(.8, (4 - 3.4 * j / 10.0) * pts[j].scale));
                    galaxyband(&pts[j], &pts[j + 1], .16 * a, MAX(2.5, 11 * pts[j].scale));
                }
            galaxybandwhite();
            galaxyrenderglow(GalaxyOrange, s->p, 10, MIN(1, env) * galaxynearfade(s->p.z), galaxydepthblur(s->p.z), .7, .2);
            galaxysprite(GalaxySpike, GalaxyOrange, s->p.x, s->p.y, MIN(260, 200 * MAX(.5, s->p.scale)),
                    galaxyflash(t, .12, 3) * galaxynearfade(s->p.z));
        }
    }
    /* 通知彗星: 镜头空间里的一条弧 (不管镜头在哪都划过画面), 头部右侧跟着通知文字 */
    for (i = 0; i < r->nnote; i++) {
        if (r->noteat[i] <= 0 || (t = now - r->noteat[i]) >= 6)
            continue;
        u = t / 6;
        h = galaxyhash((unsigned int)(r->noteat[i] * 1000));
        z = 1.5 * r->cam.focal;
        A = galaxyv(.62 + .1 * h, -.30 + .12 * h, 1);
        C = galaxyv(.05, -.12 - .1 * h, .85);
        B = galaxyv(-.62, .05 + .15 * h, 1.1);
        for (j = 0; j < 2; j++) {   /* 头部现在的位置 (j=0) 和稍早的位置 (j=1, 求来路方向) */
            tail = galaxybezier(A, C, B, MAX(0, u - .03 * j));
            sx = tail.x * r->vw * z / r->cam.focal;
            sy = tail.y * r->vw * z / r->cam.focal;
            tail = galaxyadd(r->cam.pos, galaxyapply(r->cam.rot, galaxyv(sx, sy, z * tail.z)));
            if (j)
                C = galaxysub(tail, head);
            else
                head = tail;
        }
        tail = galaxyscale(galaxynormalize(galaxyadd(galaxynormalize(head), galaxyapply(r->cam.rot, galaxyv(.6, -.3, 0)))),
                .22 * r->vw);
        env = f * galaxysmoothstep(u / .08) * (1 - galaxysmoothstep((u - .85) / .15));
        galaxycomettails(head, tail, galaxylen(C) > 1 ? galaxynormalize(C) : galaxynormalize(tail), env * 1.6, 1.5, 41.3 + i);
        for (j = galaxyemitcount(140 * env); j > 0; j--)
            galaxyemit(galaxyadd(head, galaxyprandvec(.005 * r->cam.focal)),
                    galaxyadd(galaxyscale(galaxynormalize(tail), r->cam.focal * (.05 + .1 * galaxyprand())), galaxyprandvec(.012 * r->cam.focal)),
                    1 + .6 * galaxyprand(), .0024 * r->cam.focal, galaxyprand() < .6 ? GalaxyBlue : GalaxyCool, .7, .4, 0);
        pts[0] = galaxyproject(head);
        if (pts[0].ok && env > .01) {
            galaxyrenderglow(GalaxyCool, pts[0], 12, MIN(1, env), galaxydepthblur(pts[0].z), .7, .2);
            galaxysprite(GalaxySpike, GalaxyCool, pts[0].x, pts[0].y, 160, .7 * env);
            text[ntext++] = (struct GalaxyText){pts[0].x + 34, pts[0].y + 8, env * galaxysmoothstep((u - .04) / .1), i};
        }
    }
    /* 整点: 盘面上一圈慢速大光波 (第二圈稍晚), 双星旁显示时间 */
    if (r->chimeat > 0 && (t = now - r->chimeat) < 6) {
        disk = galaxymul(r->world, galaxyrotx(GALAXYPI / 2));
        for (j = 0; j < 2; j++) {
            u = galaxyphase(t, .5 * j, 5 + .5 * j);
            if (u > 0 && u < 1) {   /* 整点报时: 金色光波 */
                galaxybandcolor(galaxytintrgb(GalaxyGold, j ? .6 : .35));
                galaxyringfx(galaxyv(0, 0, 0), disk, .95 * r->vw * galaxyeaseoutcubic(u), f * (j ? .14 : .3) * pow(1 - u, 1.2),
                        j ? 1.5 : 2.6);
                galaxybandwhite();
            }
        }
        c = galaxyproject(galaxyv(0, 0, 0));
        env = f * galaxysmoothstep(t / .6) * (1 - galaxysmoothstep((t - 3.4) / .8));
        if (c.ok && env > .02)
            text[ntext++] = (struct GalaxyText){c.x, c.y + MAX(60, 90 * c.scale), env, -1};
    }
    galaxyflushbands();
    for (i = 0; i < ntext; i++)
        if (text[i].note < 0) {
            snprintf(clock, sizeof clock, "%02d:00", r->chimehour);
            galaxytext(r->clockfont, text[i].x, text[i].y + (r->clockfont ? r->clockfont->ascent : 0), clock, text[i].a, 1);
        } else {
            galaxytext(r->notefont, text[i].x, text[i].y, galaxynotetext(r->note[text[i].note], buf, sizeof buf), text[i].a, 0);
        }
}

/* ---------- 粒子 ---------- */

/* 0..1 的伪随机数 (确定性: 假时钟测试时画面可重复) */
static double
galaxyprand(void)
{
    return galaxyhash(++galaxyscene.pseed * 7919u + 13);
}

/* 球内均匀分布的随机向量, 长度不超过 len */
static GalaxyVec
galaxyprandvec(double len)
{
    GalaxyVec v;

    do
        v = galaxyv(galaxyprand() * 2 - 1, galaxyprand() * 2 - 1, galaxyprand() * 2 - 1);
    while (galaxydot(v, v) > 1);
    return galaxyscale(v, len);
}

/* 发射一个粒子. size: 世界尺寸 (屏幕粒子是像素); drag: 速度每秒衰减的指数 */
static void
galaxyemit(GalaxyVec pos, GalaxyVec vel, double life, double size, int tint, double alpha, double drag, int screen)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyParticle *p;
    double k;

    if (!r->parts || life <= 0)
        return;
    p = &r->parts[r->partnext];
    r->partnext = (r->partnext + 1) % GALAXYPARTICLES;
    if (r->nparts < GALAXYPARTICLES)
        r->nparts++;
    /* 长尾分布: 多数又小又暗, 少数大而亮 (k 约 .6, 极少数到 2.6); 色温在 tag 色上随机偏蓝或偏橙 */
    k = .6 + 2 * pow(galaxyprand(), 7);
    *p = (GalaxyParticle){pos, vel, 0, life, size * k, alpha * MIN(1.6, .7 + .45 * k), drag,
        galaxyprand() * 2 - 1, tint, screen};
}

/* 按速率发射: 返回这一帧要发几个 (小数部分按概率) */
static int
galaxyemitcount(double rate)
{
    GalaxyScene *r = &galaxyscene;
    double n = rate * r->pdt * (r->quiet ? .5 : 1) * (r->mode == GalaxyOrbit ? 1 - .5 * galaxyclamp(r->qualityvisual - 1) : 1);

    return (int)n + (galaxyprand() < n - (int)n);
}


/* 轨道由星体身后的粒子尾迹勾出: 每个核心沿群轨道、每个窗口星沿自己的轨道环, 回溯过去一段时间的位置,
 * 粒子在星体经过时生成、原地慢慢散开变暗 (细腻密集的光尘). 回程时冻结路径, 尾迹随之淡出 */
#define GALAXYTRAILN 48

/* motion 为 m 那一刻的 stage (尾迹回溯过去的位置时用; 用当前 stage 会让整条历史路径每帧跟着变, 粒子来回甩) */
static double
galaxystageat(double m)
{
    GalaxyScene *r = &galaxyscene;
    double u;

    if (r->mode == GalaxyCollapse) {
        u = r->celapsed - (r->motion - m) * r->tscale;
        if (u >= 0) {
            if (u < GALAXYPREP)
                return galaxymix(r->cstage, GALAXYHOLD, galaxyeaseinoutcubic(u / GALAXYPREP));
            if (u < GALAXYEXITSTART)
                return GALAXYHOLD;
            return GALAXYEXIT + (GALAXYEND - GALAXYEXIT) * galaxyphase(u, GALAXYEXITSTART, GALAXYCOLLAPSE);
        }
    }
    return m < GALAXYIEND ? galaxyintrostage(m) : GALAXYHOLD;
}
static void
galaxyrenderorbittrails(double k)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyCore *g;
    GalaxyStar *s;
    GalaxyMat world, plane, orient;
    GalaxyVec gpos;
    double rgb[3], lm, m, sm, w, grid, top, span = 0, stage = r->stage, motion = r->motion;
    int i, j, live = r->mode != GalaxyReturn;

    /* 坍缩时不提前淡出: 由吸入 / 爆发接手 (galaxyglcollapsefx) */
    w = r->mode == GalaxyReturn ? 1 - galaxysmoothstep(r->retu / .45)
        : r->mode == GalaxyCollapse ? 1 : galaxysmoothstep(galaxyphase(stage, 1.6, 2.6));
    if (w < .01)
        return;
    for (i = 0; i < r->ntags; i++)      /* 星系群半径: 核心大尾迹的最大长度按它算 */
        span = MAX(span, galaxylen(r->galaxies[i].pos));
    for (i = 0; i < r->ntags; i++) {
        g = &r->galaxies[i];
        if (g->alpha < .05)
            continue;
        lm = .55 * galaxylanes[g->lane].period / r->tscale;
        grid = lm / (GALAXYTRAILN - 2);
        top = floor(motion / grid) * grid;
        for (j = 0; live && j < GALAXYTRAILN; j++) {
            m = j ? top - (j - 1) * grid : motion;
            sm = j ? galaxystageat(m) : stage;
            world = r->world;   /* 尾迹在星系群自己的坐标系里: 整体自转 (演出) 时跟着一起转, 只留下相对轨道的运动 */
            galaxycoreat(g, sm, m, world, &g->trail[j], &plane);
        }
        galaxytintcolor(GALAXYTAGTINT(g->tag), -1, rgb);
        /* 星系轨道: 大拖尾 (最长约星系群半径, 越往后越宽); 坍缩开始后逐渐放长, 整条卷进中心 */
        galaxygltrail(g->trail, GALAXYTRAILN, lm, motion, motion - top, grid, MAX(1, span) * (.9 + (r->mode == GalaxyCollapse ? 2.5 * galaxysmoothstep(galaxyphase(r->celapsed, .8, 1.6)) : 0)),
                g->size * 2.2, rgb,
                .32 * w * MIN(1, g->alpha), .9 * r->starscale, (int)(16000 * k), 13.1 * (i + 1));
    }
    for (i = 0; i < r->nstars; i++) {
        s = &r->stars[i];
        if (s->died || s->alpha < .1 || s->galaxy < 0 || s->galaxy >= r->ntags)
            continue;
        g = &r->galaxies[s->galaxy];
        lm = 1.2 / MAX(.05, fabs(s->speed));
        grid = lm / (GALAXYTRAILN - 2);
        top = floor(motion / grid) * grid;
        for (j = 0; live && j < GALAXYTRAILN; j++) {
            m = j ? top - (j - 1) * grid : motion;
            sm = j ? galaxystageat(m) : stage;
            world = r->world;
            galaxycoreat(g, sm, m, world, &gpos, &plane);
            galaxystarat(s, sm, m, world, gpos, galaxymul(plane, g->ring[s->ring]), &s->trail[j], &orient);
        }
        galaxytintcolor(GALAXYTAGTINT(s->galaxy), -1, rgb);
        /* 恒星 (窗口星) 轨道: 细短的小拖尾, 不和核心的大尾迹抢视觉 */
        galaxygltrail(s->trail, GALAXYTRAILN, lm, motion, motion - top, grid, g->size * 3, g->size * .2, rgb,
                .22 * w * MIN(1, s->alpha), .7 * r->starscale, (int)(1200 * k), 7.7 * (i + 1));
    }
}

/* GPU 粒子: 每个核心一圈吸积盘 (跟轨道环一起显隐), 星系群周围一片被核心 / 中心光源照亮的星尘.
 * 数量按质量降级 / 安静模式减少; 不存状态, 每帧只是几次实例化绘制 */
static void
galaxyrendergpuparticles(void)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyCore *g;
    double rgb[3], rout, extent = 0, k = r->mode == GalaxyOrbit ? 1 - .4 * galaxyclamp(r->qualityvisual - 1) : 1;
    float lights[10][4], lcol[10][3];
    int i, j, nl = 0, n;

    if (r->quiet)
        k *= .5;
    /* Esc 收尾: 所有 GPU 粒子 (尾迹 / 吸积盘 / 粒子恒星 / 星尘) 螺旋吸进中心, 再随冲击波炸开 */
    if (r->mode == GalaxyCollapse) {
        double ext = 0, u = r->celapsed;

        for (i = 0; i < r->ntags; i++)
            ext = MAX(ext, galaxylen(r->galaxies[i].pos) + r->galaxies[i].size * 7);
        /* 吸入在冲击波出现的那一刻刚好收拢 (越到最后越快), 冲击波一出现就开始爆发 (起步最快), 中间不留空档 */
        galaxyglcollapsefx(pow(galaxyphase(u, GALAXYEXITSTART - .2, GALAXYSHOCK - .05), 2.2),
                1 - pow(1 - galaxyphase(u, GALAXYSHOCK - .05, GALAXYSHOCK + GALAXYSHOCKT + GALAXYAFTER), 2),
                galaxyapply(r->world, galaxyv(0, 1, 0)), MAX(1, ext) * 1.4 * (r->gentle ? .5 : 1));
    }
    galaxyrenderorbittrails(k);
    for (i = 0; i < r->ntags; i++) {
        g = &r->galaxies[i];
        if (g->alpha < .05)
            continue;
        rout = g->size * 7;
        for (j = 0; j < g->nrings; j++)
            rout = MAX(rout, g->ringr[j] * 1.15);
        rout *= galaxybreathe(g, r->motion) * galaxyeaseoutcubic(g->fill);
        extent = MAX(extent, galaxylen(g->pos) + rout);
        galaxytintcolor(GALAXYTAGTINT(g->tag), -1, rgb);
        n = (int)((g->nstars ? 9000 : 4000) * k);
        galaxygldisk(g->pos, &g->plane, g->size * 1.6, rout, .5, rgb, .8 * (r->mode == GalaxyCollapse ? .13 : r->ringalpha) * MIN(1, g->alpha),
                .9 * r->starscale, n, 17.3 * (i + 1));
        if (nl < 9) {
            lights[nl][0] = g->pos.x;
            lights[nl][1] = g->pos.y;
            lights[nl][2] = g->pos.z;
            lights[nl][3] = rout * 1.8;
            for (j = 0; j < 3; j++)
                lcol[nl][j] = .9 * rgb[j] * MIN(1, g->alpha);
            nl++;
        }
    }
    if (r->novag >= 0 && r->novag < r->ntags && nl < 9 && r->galaxies[r->novag].nova > .01) {   /* 超新星照亮附近的星尘 */
        g = &r->galaxies[r->novag];
        lights[nl][0] = g->pos.x;
        lights[nl][1] = g->pos.y;
        lights[nl][2] = g->pos.z;
        lights[nl][3] = extent * .45;
        lcol[nl][0] = 1.4 * g->nova;
        lcol[nl][1] = 1.1 * g->nova;
        lcol[nl][2] = 1.6 * g->nova;
        nl++;
    }
    if (nl && r->sunalpha > .01) {   /* 中心双星: 暖白, 照得更远 */
        lights[nl][0] = lights[nl][1] = lights[nl][2] = 0;
        lights[nl][3] = extent * .35;
        lcol[nl][0] = .9 * r->sunalpha;
        lcol[nl][1] = .85 * r->sunalpha;
        lcol[nl][2] = .75 * r->sunalpha;
        nl++;
    }
    galaxygldust(&r->world, extent * 1.25, lights, lcol, nl, .45 * (r->mode == GalaxyCollapse ? 1 : r->dustfade), .7, (int)(16000 * k), 91.7);
}

/* 更新并画出所有粒子: 透视投影, 按年龄淡出、缩小, 交给 GPU (按深度被卡片挡住) */
static void
galaxyrenderparticles(void)
{
    GalaxyScene *r = &galaxyscene;
    static double rgb[GalaxyTints][3];
    static const double cool[3] = {.68, .8, 1}, warm[3] = {1, .76, .5};
    static int ready;
    GalaxyParticle *p;
    GalaxyProj pr, pp;
    double dt = r->pdt, u, a, rad, col[3], w, len, coc, rad2, F = r->cam.focal, flow, ff = 5 / F, t = r->motion;
    int i, k;

    if (!ready) {
        for (i = 0; i < GalaxyTints; i++)
            galaxytintcolor(i, -1, rgb[i]);
        ready = 1;
    }
    galaxyrendergpuparticles();
    flow = .012 * F * r->holdw;
    for (i = 0; i < r->nparts; i++) {
        p = &r->parts[i];
        if (p->age >= p->life)
            continue;
        p->age += dt;
        if (p->age >= p->life)
            continue;
        p->pos = galaxyadd(p->pos, galaxyscale(p->vel, dt));
        p->vel = galaxyscale(p->vel, exp(-p->drag * dt));
        /* 气流: 无散度的正弦流场 (每个分量只随另外两个坐标变化), 驻留时飘散的光尘像被气流带着走 */
        if (!p->screen && flow > 1e-6)
            p->pos = galaxyadd(p->pos, galaxyscale(galaxyv(sin(p->pos.y * ff + t * .7), sin(p->pos.z * ff + t * .5 + 1.7),
                            sin(p->pos.x * ff + t * .6 + 3.1)), flow * dt));
        u = p->age / p->life;
        a = p->alpha * pow(1 - u, 1.5) * galaxysmoothstep(u / .08);
        if (r->mode == GalaxyReturn)    /* 回程前半段淡完, 交给真实桌面时不会有残留的光点突然消失 */
            a *= 1 - galaxysmoothstep(r->retu / .5);
        w = .22 * fabs(p->temp);
        for (k = 0; k < 3; k++)
            col[k] = galaxymix(rgb[p->tint][k], p->temp < 0 ? cool[k] : warm[k], w);
        /* 快速粒子画成拉丝: 长度是 1/60s 内走过的屏幕距离 (与帧率无关), 亮度按 直径 / 长度 摊薄 */
        if (p->screen) {
            rad = p->size * (1 - .5 * u);
            len = sqrt(galaxydot(p->vel, p->vel)) / 60;
            if (len > 2.5 * rad)
                galaxyglpartline(p->pos.x - p->vel.x / 60, p->pos.y - p->vel.y / 60, p->pos.x, p->pos.y, p->pos.z, MAX(.5, .45 * rad), col, .15,
                        a * MAX(.2, MIN(1, 2.5 * rad / len)));
            else
                galaxyglparticle(p->pos.x, p->pos.y, p->pos.z, rad, col, .7, a, 0);   /* z: 0 最前, 远景流星放在最远处 (被卡片挡住) */
            continue;
        }
        pr = galaxyproject(p->pos);
        if (!pr.ok)
            continue;
        rad = MIN(40, p->size * pr.scale * (1 - .55 * u));
        a *= galaxynearfade(pr.z) * MIN(1, rad / .6);
        len = sqrt(galaxydot(p->vel, p->vel)) * pr.scale / 60;
        if (len > 2.5 * rad && (pp = galaxyproject(galaxysub(p->pos, galaxyscale(p->vel, 1.0 / 60)))).ok
                && (len = hypot(pr.x - pp.x, pr.y - pp.y)) > 2.5 * rad) {
            galaxyglpartline(pp.x, pp.y, pr.x, pr.y, pr.z, MAX(.5, .45 * rad), col, .15, a * MAX(.2, MIN(1, 2.5 * rad / len)));
            continue;
        }
        /* 景深: 离对焦距离 (镜头到目标) 越远, 光斑越大越淡; 只有明显失焦的才画成散景圆盘 (否则近处核心身后像一串气泡) */
        coc = MIN(10, 10 * fabs(1 - r->cam.dist / MAX(1, pr.z)));
        rad2 = MIN(13, sqrt(rad * rad + coc * coc));
        galaxyglparticle(pr.x, pr.y, pr.z, MAX(.6, rad2), col, .7, a * pow(MAX(.6, rad) / MAX(.6, rad2), 1.7), coc > 5 && rad2 > 6);
    }
}

/* 键盘过滤词: 屏幕下方居中的半透明框, 「过滤: 词 · N 个匹配 · 操作提示」 */
static void
galaxyrenderquery(void)
{
    GalaxyScene *r = &galaxyscene;
    XGlyphInfo ext;
    XRenderColor shade = {0x0600, 0x0a00, 0x1600, 0xd800}, edge = {0x5000, 0x6800, 0x9000, 0x9000};
    char text[160];
    int x, y, w, h, len;

    if (r->mode != GalaxyOrbit || !r->kqlen || !r->titledraw || !r->queryfont)
        return;
    r->kq[r->kqlen] = 0;
    len = snprintf(text, sizeof text, "过滤: %s    ·    %d 个匹配%s", r->kq, r->kn,
            r->kn ? "    ·    Enter 跳转    Tab 切换    Esc 清空" : "    ·    Esc 清空");
    XftTextExtentsUtf8(dpy, r->queryfont, (XftChar8 *)text, len, &ext);
    w = MIN(r->vw - 32, ext.xOff + 48);
    h = r->queryfont->height + 26;
    x = r->vx + (r->vw - w) / 2;
    y = r->vy + r->vh - h - r->vh / 9;
    XRenderFillRectangle(dpy, PictOpOver, r->front, &edge, x - 1, y - 1, w + 2, h + 2);
    XRenderFillRectangle(dpy, PictOpOver, r->front, &shade, x, y, w, h);
    XftDrawStringUtf8(r->titledraw, &r->titlecolor, r->queryfont, x + 24, y + 13 + r->queryfont->ascent, (XftChar8 *)text, len);
}

/* 标题放不下时按 UTF-8 字符截断并加省略号 */
static void
galaxyfittext(XftFont *font, char *text, size_t size, int maxw)
{
    XGlyphInfo ext;
    size_t n = strlen(text);
    char buf[256];

    if (!font)
        return;
    XftTextExtentsUtf8(dpy, font, (XftChar8 *)text, n, &ext);
    while (ext.xOff > maxw && n > 0) {
        do
            n--;
        while (n > 0 && (text[n] & 0xc0) == 0x80);
        snprintf(buf, sizeof buf, "%.*s…", (int)n, text);
        XftTextExtentsUtf8(dpy, font, (XftChar8 *)buf, strlen(buf), &ext);
        if (ext.xOff <= maxw || n == 0) {
            snprintf(text, size, "%s", buf);
            return;
        }
    }
}

/* 悬停 (或拖着卡片经过) 的核心: 下方显示 tag 图标、窗口数和前几个窗口的标题 */
static void
galaxyrendercorelabel(void)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyCore *g;
    XGlyphInfo ext;
    XRenderColor shade = {0x0700, 0x0b00, 0x1700, 0xd000};
    char line1[96], line2[200] = "";
    const char *icon;
    int i, n = 0, len, iw = 0, w, h, x, y, lh;

    if (r->mode != GalaxyOrbit || r->hovercore < 0 || r->hovercore >= r->ntags || !r->titledraw || !r->titlefont)
        return;
    g = &r->galaxies[r->hovercore];
    if (!g->hit || g->hover < .2)
        return;
    for (i = 0; i < r->nstars; i++)
        if (r->stars[i].galaxy == g->tag && !r->stars[i].died) {
            if (n < 3 && r->stars[i].title[0])
                snprintf(line2 + strlen(line2), sizeof line2 - strlen(line2), "%s%.40s", n ? "  ·  " : "", r->stars[i].title);
            n++;
        }
    if (n > 3)
        snprintf(line2 + strlen(line2), sizeof line2 - strlen(line2), "  …");
    if (n)
        snprintf(line1, sizeof line1, "tag %d  ·  %d 个窗口%s", g->tag + 1, n, r->dragstar >= 0 ? "    松手把窗口移到这里" : "");
    else
        snprintf(line1, sizeof line1, "tag %d  ·  空%s", g->tag + 1, r->dragstar >= 0 ? "    松手把窗口移到这里" : "");
    icon = g->tag < (int)LENGTH(tags) ? tags[g->tag] : "";
    if (r->iconfont && icon[0]) {
        XftTextExtentsUtf8(dpy, r->iconfont, (XftChar8 *)icon, strlen(icon), &ext);
        iw = ext.xOff + 12;
    }
    XftTextExtentsUtf8(dpy, r->titlefont, (XftChar8 *)line1, strlen(line1), &ext);
    w = iw + ext.xOff;
    if (line2[0]) {
        galaxyfittext(r->titlefont, line2, sizeof line2, 560);
        XftTextExtentsUtf8(dpy, r->titlefont, (XftChar8 *)line2, strlen(line2), &ext);
        w = MAX(w, ext.xOff);
    }
    lh = r->titlefont->height + 4;
    w += 24;
    h = lh * (line2[0] ? 2 : 1) + 12;
    x = MAX(8, MIN(r->w - w - 8, (int)(g->hx - w * .5)));
    y = MAX(8, MIN(r->h - h - 8, (int)(g->hy + g->hr + 14)));
    XRenderFillRectangle(dpy, PictOpOver, r->front, &shade, x, y, w, h);
    {   /* 左侧一条 tag 色竖条: 和这个星系的核心、轨道同色 */
        double c[3];
        galaxytintcolor(GALAXYTAGTINT(g->tag), .55, c);
        XRenderFillRectangle(dpy, PictOpOver, r->front, &(XRenderColor){(unsigned short)(c[0] * 65535),
                (unsigned short)(c[1] * 65535), (unsigned short)(c[2] * 65535), 0xffff}, x, y, 4, h);
    }
    if (iw)
        XftDrawStringUtf8(r->titledraw, &r->titlecolor, r->iconfont, x + 12, y + 6 + r->titlefont->ascent, (XftChar8 *)icon, strlen(icon));
    len = strlen(line1);
    XftDrawStringUtf8(r->titledraw, &r->titlecolor, r->titlefont, x + 12 + iw, y + 6 + r->titlefont->ascent, (XftChar8 *)line1, len);
    if (line2[0])
        galaxytext(r->titlefont, x + 12, y + 6 + lh + r->titlefont->ascent, line2, .7, 0);
}

/* F12 帧率面板: 右上角, 实时帧率 / 渲染耗时 / 质量级别 / GPU 合成和 CPU 扫描的开销 */
static void
galaxyrenderhud(void)
{
    GalaxyScene *r = &galaxyscene;
    XRenderColor shade = {0x0400, 0x0600, 0x0c00, 0xc800};
    char line[2][200];
    double sum = 0, fps;
    int i, n = 0, k, w = 0, lh, x, y;
    XGlyphInfo ext;

    if (!galaxyhud || !r->titledraw || !r->titlefont)
        return;
    for (i = r->ngaps - 1; i >= 0 && n < 60; i--, n++)
        sum += r->gaps[i];
    fps = sum > 0 ? n / sum : 0;
    snprintf(line[0], sizeof line[0], "%.0f fps  ·  渲染 %.1f ms  ·  质量 %d", fps, r->lastcost * 1000, r->quality);
    snprintf(line[1], sizeof line[1], "GPU 合成 %.2f ms/帧  ·  截图刷新 %d 张 / 平均 %.2f ms  ·  CPU 扫描 %.2f ms/帧  ·  %s",
            r->frames ? galaxygl.gputime / r->frames * 1000 : 0, r->refreshn, r->refreshn ? r->refreshsum / r->refreshn * 1000 : 0,
            r->frames ? r->heatcost / r->frames * 1000 : 0, galaxymodename[r->mode]);
    for (k = 0; k < 2; k++) {
        XftTextExtentsUtf8(dpy, r->titlefont, (XftChar8 *)line[k], strlen(line[k]), &ext);
        w = MAX(w, ext.xOff);
    }
    lh = r->titlefont->height + 2;
    w += 24;
    x = r->vx + r->vw - w - 24;
    y = r->vy + 24;
    XRenderFillRectangle(dpy, PictOpOver, r->front, &shade, x, y, w, lh * 2 + 14);
    for (k = 0; k < 2; k++)
        XftDrawStringUtf8(r->titledraw, &r->titlecolor, r->titlefont, x + 12, y + 7 + k * lh + r->titlefont->ascent,
                (XftChar8 *)line[k], strlen(line[k]));
}

static void
galaxyrendertitle(void)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyStar *s;
    XGlyphInfo ext;
    XRenderColor shade = {0x0700, 0x0b00, 0x1700, 0xd000};
    int x, y, w, h, len;

    if (r->mode != GalaxyOrbit || r->hover < 0 || r->hover >= r->nstars || !r->titledraw || !r->titlefont)
        return;
    s = &r->stars[r->hover];
    if (!s->hit || s->hover < .2 || !s->title[0])
        return;
    len = strlen(s->title);
    XftTextExtentsUtf8(dpy, r->titlefont, (XftChar8 *)s->title, len, &ext);
    w = MIN(r->w - 16, ext.xOff + 22);
    h = r->titlefont->height + 12;
    x = MAX(8, MIN(r->w - w - 8, (int)(s->p.x - w * .5)));
    y = MAX(8, MIN(r->h - h - 8, (int)(s->by1 + 12)));
    XRenderFillRectangle(dpy, PictOpOver, r->front, &shade, x, y, w, h);
    XftDrawStringUtf8(r->titledraw, &r->titlecolor, r->titlefont, x + 11, y + 6 + r->titlefont->ascent,
            (XftChar8 *)s->title, len);
}

/* 低透明度的尾迹: 回溯时间求出星体之前的 3D 位置, 用当前镜头投影.
 * 高速旋转时是短促的流光, 驻留时拉长成沿轨道的彗星弧, 指示运动方向 */

/* 开场 A: 桌面碎块. 由近及远依次开始, 绕灭点 (视口中心) 旋涡状收缩、自转、变小, 末端化成拖尾的光点.
 * 每块: 源图和蒙版设同一个仿射变换 (蒙版带 1px 透明边, 双线性采样后边缘抗锯齿), 一次合成 */
static int
galaxyshardsactive(void)
{
    GalaxyScene *r = &galaxyscene;

    return r->variant == GalaxyShatter && r->shardsrc && r->shardmask[GALAXYSHARDA - 1]
        && (r->mode == GalaxyIntro || r->mode == GalaxyCollapse) && r->iclock < 1.6 && galaxybeatw() > .01;
}

static void
galaxyshardat(int i, int j, double s, double *cx, double *cy, double *k, double *a, double *u)
{
    GalaxyScene *r = &galaxyscene;
    double vx = r->vx + r->vw * .5, vy = r->vy + r->vh * .5, x0 = r->vx + (i + .5) * r->shardw, y0 = r->vy + (j + .5) * r->shardh;
    double d = hypot(x0 - vx, y0 - vy) / hypot(r->vw * .5, r->vh * .5), t0, e, sw;

    t0 = .2 + .42 * d + .1 * galaxyhash(i * 31 + j * 7 + 3);
    *u = galaxyphase(s, t0, t0 + .75);
    e = pow(*u, 2.2);
    sw = (1.5 + .9 * galaxyhash(i * 31 + j * 7 + 4)) * e;
    *cx = vx + (cos(sw) * (x0 - vx) - sin(sw) * (y0 - vy)) * (1 - e);
    *cy = vy + (sin(sw) * (x0 - vx) + cos(sw) * (y0 - vy)) * (1 - e);
    *k = pow(1 - e, 1.4);
    *a = sw + (galaxyhash(i * 31 + j * 7 + 5) - .5) * 2.5 * e;
}

static void
galaxyrendershards(void)
{
    GalaxyScene *r = &galaxyscene;
    XTransform xf = {{{0}}};
    GalaxyProj pa, pb;
    double f = galaxybeatw(), s = r->iclock, cx, cy, k, a, u, px, py, pk, pa_, pu, c, sn, bx, by, ex, ey, al, eh, hw = r->shardw * .5, hh = r->shardh * .5;
    static const double flashrgb[3] = {1, .95, .86};
    int i, j, lvl, x0, y0, x1, y1;

    if (!galaxyshardsactive())
        return;
    for (j = 0; j < GALAXYSHARDY; j++)
        for (i = 0; i < GALAXYSHARDX; i++) {
            galaxyshardat(i, j, s, &cx, &cy, &k, &a, &u);
            if (u >= 1 || k < .015)
                continue;
            al = f * (1 - galaxysmoothstep(galaxyphase(u, .7, 1)));
            if (u <= 0 && f > .99) {   /* 还没开始动: 原样拷贝 */
                XRenderComposite(dpy, PictOpSrc, r->shardflat, None, r->back, i * r->shardw, j * r->shardh, 0, 0,
                        r->vx + i * r->shardw, r->vy + j * r->shardh, MIN(r->shardw, r->vw - i * r->shardw), MIN(r->shardh, r->vh - j * r->shardh));
                continue;
            }
            lvl = (int)(al * GALAXYSHARDA + .5) - 1;
            if (lvl < 0)
                continue;
            /* 目标 -> 源图: p = c0 + R(-a) (d - c) / k (源图坐标相对视口) */
            c = cos(a) / k;
            sn = sin(a) / k;
            bx = (i + .5) * r->shardw - (c * cx + sn * cy);
            by = (j + .5) * r->shardh - (-sn * cx + c * cy);
            xf.matrix[0][0] = XDoubleToFixed(c);
            xf.matrix[0][1] = XDoubleToFixed(sn);
            xf.matrix[0][2] = XDoubleToFixed(bx);
            xf.matrix[1][0] = XDoubleToFixed(-sn);
            xf.matrix[1][1] = XDoubleToFixed(c);
            xf.matrix[1][2] = XDoubleToFixed(by);
            xf.matrix[2][2] = XDoubleToFixed(1);
            XRenderSetPictureTransform(dpy, r->shardsrc, &xf);
            xf.matrix[0][2] = XDoubleToFixed(bx - i * r->shardw + 1);
            xf.matrix[1][2] = XDoubleToFixed(by - j * r->shardh + 1);
            XRenderSetPictureTransform(dpy, r->shardmask[lvl], &xf);
            ex = k * (fabs(cos(a)) * hw + fabs(sin(a)) * hh) + 2;
            ey = k * (fabs(sin(a)) * hw + fabs(cos(a)) * hh) + 2;
            x0 = (int)floor(cx - ex);
            y0 = (int)floor(cy - ey);
            x1 = (int)ceil(cx + ex);
            y1 = (int)ceil(cy + ey);
            XRenderComposite(dpy, PictOpOver, r->shardsrc, r->shardmask[lvl], r->back, x0, y0, x0, y0, x0, y0, x1 - x0, y1 - y0);
            /* 碎块的边: 转过一个角度时迎着光亮起来 (像有厚度的玻璃片); 碎块缩小后不再描边 */
            if (u > 0 && (eh = al * .3 * fabs(sin(a)) * galaxysmoothstep((k - .25) / .35)) > .01) {
                GalaxyProj e[4];
                static const double ex_[4] = {-1, 1, 1, -1}, ey_[4] = {-1, -1, 1, 1};
                int m;

                for (m = 0; m < 4; m++)
                    e[m] = galaxysp(cx + k * (cos(a) * ex_[m] * hw - sin(a) * ey_[m] * hh), cy + k * (sin(a) * ex_[m] * hw + cos(a) * ey_[m] * hh));
                for (m = 0; m < 4; m++)
                    galaxyglline(&e[m], &e[(m + 1) % 4], 0xfff1dc, 0xdbe8ff, eh, .7);
            }
            /* 碎块变小后: 光点 + 指向来路的短尾 */
            if (u > .3) {
                galaxyshardat(i, j, s - .07, &px, &py, &pk, &pa_, &pu);
                pa = galaxysp(cx, cy);
                pb = galaxysp(px, py);
                galaxyband(&pa, &pb, .3 * al * galaxysmoothstep((u - .3) / .3), 1.2);
                galaxysprite(GalaxyHalo, (i + j) % 3 ? GalaxyCool : GalaxyWarm, cx, cy, 5 + 10 * (1 - k), .7 * al * galaxysmoothstep((u - .3) / .4));
            }
        }
    galaxyflushbands();
    /* 碎裂的一刻: 约 80ms 的漏光闪白, 给开场一个起拍 */
    galaxyglrect(r->vx, r->vy, r->vw, r->vh, flashrgb, .1 * f * galaxyflash(s - .2, .025, 18) * (r->gentle ? .25 : 1));
    xf.matrix[0][0] = xf.matrix[1][1] = xf.matrix[2][2] = XDoubleToFixed(1);
    xf.matrix[0][1] = xf.matrix[1][0] = xf.matrix[0][2] = xf.matrix[1][2] = 0;
    for (lvl = 0; lvl < GALAXYSHARDA; lvl++)
        XRenderSetPictureTransform(dpy, r->shardmask[lvl], &xf);
}

static void
galaxyrenderbackground(void)
{
    GalaxyScene *r = &galaxyscene;
    XRenderColor shade = {0, 0, 0, 0};
    double key[4] = {r->bright, r->vign, r->desk, r->live ? r->reveal : 0};

    /* 参数与上一帧相同 (驻留态): 直接用缓存, 省掉全屏暗角缩放和填充. 深空每帧随镜头平移, 不能用缓存 */
    if (r->space < .004 && r->bg && r->bgok && !memcmp(key, r->bgkey, sizeof key) && !galaxyshardsactive()) {
        XRenderComposite(dpy, PictOpSrc, r->bg, None, r->back, 0, 0, 0, 0, 0, 0, r->w, r->h);
        return;
    }
    XRenderComposite(dpy, PictOpSrc, r->wallpaper, None, r->back, 0, 0, 0, 0, 0, 0, r->w, r->h);
    if (r->live && r->reveal > 0)
        XRenderComposite(dpy, PictOpOver, r->live, galaxywhite(r->reveal), r->back, 0, 0, 0, 0, 0, 0, r->w, r->h);
    if (r->desktop && r->desk > 0 && !galaxyshardsactive())
        XRenderComposite(dpy, PictOpOver, r->desktop, galaxywhite(r->desk), r->back, 0, 0, 0, 0, 0, 0, r->w, r->h);
    shade.alpha = (unsigned short)(65535 * galaxyclamp(1 - r->bright));
    if (shade.alpha)
        XRenderFillRectangle(dpy, PictOpOver, r->back, &shade, 0, 0, r->w, r->h);
    galaxyrenderspace();
    if (galaxyshardsactive()) {
        /* 开场 A: 碎块盖在深空之上; 截下的整个桌面 (含窗口阴影 / 托盘等) 仍在最上面淡出, 首帧不变 */
        galaxyrendershards();
        if (r->desktop && r->desk > 0)
            XRenderComposite(dpy, PictOpOver, r->desktop, galaxywhite(r->desk), r->back, 0, 0, 0, 0, 0, 0, r->w, r->h);
    }
    if (r->vignette && r->vign > .004)
        XRenderComposite(dpy, PictOpOver, r->vignette, galaxywhite(r->vign), r->back, 0, 0, 0, 0, 0, 0, r->w, r->h);
    /* 连续两帧参数相同: 存下来, 之后直接复用 */
    if (r->space < .004 && r->bg && !memcmp(key, r->bglast, sizeof key) && !galaxyshardsactive()) {
        XRenderComposite(dpy, PictOpSrc, r->back, None, r->bg, 0, 0, 0, 0, 0, 0, r->w, r->h);
        memcpy(r->bgkey, key, sizeof key);
        r->bgok = 1;
    }
    memcpy(r->bglast, key, sizeof key);
}

/* 回程结尾盖在场景之上: 开始时截的桌面 / 状态栏 */
static void
galaxyrenderfront(void)
{
    GalaxyScene *r = &galaxyscene;
    int i;

    if (!r->desktop)
        return;
    if (r->deskover > .004)
        XRenderComposite(dpy, PictOpOver, r->desktop, galaxywhite(r->deskover), r->front, 0, 0, 0, 0, 0, 0, r->w, r->h);
    /* 回程 / 落位的最后: 落到原位的卡片外面淡入真实窗口的边框 (焦点窗口用选中色), 去掉遮罩时边框不会突然出现 */
    if (r->mode == GalaxyReturn && r->bar > .004 && !r->fulldesk)
        for (i = 0; i < r->nstars; i++) {
            GalaxyStar *s = &r->stars[i];
            Client *c = s->valid ? wintoclient(s->win) : NULL;
            XRenderColor bc;
            XRectangle rc[4];
            int bw, x0, y0, x1, y1;

            if (!c || !(r->rkind == GalaxyLand ? s->land : s->back) || !s->hit || (bw = c->bw) <= 0 || s->alpha < .5)
                continue;
            bc = scheme[c->win == r->twin ? SchemeSel : SchemeNorm][ColBorder].color;
            bc.red = (unsigned short)(bc.red * r->bar);
            bc.green = (unsigned short)(bc.green * r->bar);
            bc.blue = (unsigned short)(bc.blue * r->bar);
            bc.alpha = (unsigned short)(65535 * r->bar);
            x0 = (int)lround(s->bx0) - bw;
            y0 = (int)lround(s->by0) - bw;
            x1 = (int)lround(s->bx1) + bw;
            y1 = (int)lround(s->by1) + bw;
            rc[0] = (XRectangle){x0, y0, x1 - x0, bw};
            rc[1] = (XRectangle){x0, y1 - bw, x1 - x0, bw};
            rc[2] = (XRectangle){x0, y0 + bw, bw, y1 - y0 - 2 * bw};
            rc[3] = (XRectangle){x1 - bw, y0 + bw, bw, y1 - y0 - 2 * bw};
            XRenderFillRectangles(dpy, PictOpOver, r->front, &bc, rc, 4);
        }
    if (r->mode == GalaxyReturn && (r->rkind == GalaxyLand || r->moved) && !r->fulldesk) {
        /* 进入别的 tag: 状态栏 / 托盘用切换后实时截取的样子 */
        for (i = 0; i < r->nlandbar && r->bar > .004; i++)
            XRenderComposite(dpy, PictOpOver, r->landbar[i], galaxywhite(r->bar), r->front, 0, 0, 0, 0,
                    r->landbarx[i], r->landbary[i], r->landbarw[i], r->landbarh[i]);
        return;
    }
    if (r->bar > .004 && r->barw > 0 && r->barh > 0)
        XRenderComposite(dpy, PictOpOver, r->desktop, galaxywhite(r->bar), r->front,
                r->barx, r->bary, 0, 0, r->barx, r->bary, r->barw, r->barh);
}

/* 所有窗口星体回归核心后, 中心只剩一个柔和光点: Stellar Pulse */
static void
galaxyrendercentral(void)
{
    GalaxyScene *r = &galaxyscene;
    double pulse = r->pulse;
    GalaxyProj p;

    if (r->central < .004)
        return;
    p = galaxyproject(galaxyv(0, 0, 0));
    if (!p.ok)
        return;
    galaxyrenderglow(GalaxyWarm, p, 15 * (1 + .12 * pulse), r->central, 0, .45 * (1 + .6 * pulse), .14 * (1 + .6 * pulse));
}

/* 一帧画完: GPU 把底层、光层 (加泛光) 和前景层合成到遮罩窗口. 质量降级时减少泛光级数、关掉色差和颗粒 */
static void
galaxypresent(void)
{
    GalaxyScene *r = &galaxyscene;
    double q = r->mode == GalaxyOrbit ? r->qualityvisual : 0, u, cx, cy, far, sp, lens, sr = 0, sw = 1, sk = 0, glow;
    double now = galaxynow(), dt, expo, rays;
    GalaxyProj p, sun;

    /* 曝光适应: 光层变亮时约 0.12s 跟上, 变暗时约 0.8s 恢复; 曝光只乘在光上 (底层不变), 大闪光后画面短暂压暗 */
    dt = r->adaptat > 0 ? MAX(0, MIN(.1, now - r->adaptat)) : 0;
    r->adaptat = now;
    r->adapt += (galaxygl.lum - r->adapt) * MIN(1, dt / (galaxygl.lum > r->adapt ? .12 : .8));
    expo = r->gentle ? 1 : MAX(.55, 1 / (1 + 2.5 * MAX(0, r->adapt - .05)));
    r->lumsum += galaxygl.lum;
    r->lummax = MAX(r->lummax, galaxygl.lum);
    r->expomin = r->expomin > 0 ? MIN(r->expomin, expo) : expo;
    /* 体积光束: 从中心光源发出, 跟它的亮度走 */
    sun = galaxyproject(galaxyv(0, 0, 0));
    rays = sun.ok && r->mode != GalaxyReturn ? .12 * r->sunalpha * (1 + .5 * r->sunpulse) : 0;
    galaxyglpostfx(sun.x, sun.y, rays, expo);

    /* Esc 收尾: 收束时引力透镜把背景拽向中心; 然后中心爆出冲击波, 圆内露出壁纸, 余晖慢慢散去 */
    if (r->mode == GalaxyCollapse && galaxygl.gwall) {
        u = r->celapsed;
        p = galaxyproject(galaxyv(0, 0, 0));
        cx = p.ok ? p.x : r->vx + r->vw * .5;
        cy = p.ok ? p.y : r->vy + r->vh * .5;
        lens = .35 * galaxysmoothstep(galaxyphase(u, GALAXYEXITSTART, GALAXYSHOCK))
            * (1 - galaxysmoothstep(galaxyphase(u, GALAXYSHOCK, GALAXYSHOCK + .25)));
        glow = .6 * galaxysmoothstep(galaxyphase(u, GALAXYSHOCK - .08, GALAXYSHOCK))
            * pow(1 - galaxyphase(u, GALAXYSHOCK, GALAXYSHOCK + GALAXYSHOCKT + GALAXYAFTER), 2);
        if (u >= GALAXYSHOCK) {
            sp = galaxyphase(u, GALAXYSHOCK, GALAXYSHOCK + GALAXYSHOCKT);
            far = hypot(MAX(cx - r->vx, r->vx + r->vw - cx), MAX(cy - r->vy, r->vy + r->vh - cy)) + 160;
            sr = far * galaxyeaseoutcubic(sp);
            sw = 30 + 90 * sp;
            sk = 1.1 * pow(1 - sp, 1.5);
            if (r->live)    /* 动态壁纸: 揭开的是正在播放的画面 */
                XRenderComposite(dpy, PictOpSrc, r->live, None, r->wallpaper, 0, 0, 0, 0, 0, 0, r->w, r->h);
        }
        if (r->gentle) {   /* 减弱动效: 不扭曲背景, 闪光和光环只留四分之一 */
            lens = 0;
            glow *= .25;
            sk *= .25;
        }
        galaxylensflare(cx, cy, .8 * glow);
        galaxyglexitfx(cx, cy, lens, .95 * MAX(1, MIN(MIN(cx - r->vx, r->vx + r->vw - cx), MIN(cy - r->vy, r->vy + r->vh - cy))),
                sr, sw, sk, u >= GALAXYSHOCK, glow);
    }
    galaxyglpresent(.6, r->gentle ? 0 : 1 - galaxyclamp(q - 1), GALAXYBLOOM - (int)lround(galaxyclamp(q / 3) * 3));   /* 减弱动效: 不加色差 */
}

/* 一帧开始: 清空光层和前景层 */
static void
galaxyframebegin(void)
{
    GalaxyScene *r = &galaxyscene;
    XRenderColor clear = {0, 0, 0, 0};

    galaxyglframe();
    if (r->front)
        XRenderFillRectangle(dpy, PictOpSrc, r->front, &clear, 0, 0, r->w, r->h);
}

static double galaxynow(void);

static double
galaxynow(void)
{
    struct timespec now;

    if (galaxyscene.fakestep > 0)   /* 确定性时钟 (测试): 只随渲染的帧数前进 */
        return galaxyscene.fakeclock;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec - galaxyscene.start.tv_sec + (now.tv_nsec - galaxyscene.start.tv_nsec) / 1e9;
}

static void
galaxyrender(void)
{
    GalaxyScene *r = &galaxyscene;
    double t = galaxynow(), next;
    galaxyframebegin();
    galaxybandwhite();
    galaxyrendernebula();
    galaxyrenderbackground();
    next = galaxynow(); r->phasecost[1] += next - t; t = next;
    next = galaxynow(); r->phasecost[2] += next - t; t = next;
    galaxyrenderitems();
    if (r->trace && r->log && r->mode == GalaxyOrbit) {    /* 测试用: 核心在屏幕上的位置 (拖动测试找松手目标) */
        int i;
        fprintf(r->log, "cores");
        for (i = 0; i < r->ntags; i++)
            if (r->galaxies[i].hit)
                fprintf(r->log, " %d:%.0f,%.0f", i + 1, r->galaxies[i].hx, r->galaxies[i].hy);
        fprintf(r->log, " | stars");    /* 可点中的窗口卡片: 下标 / 所在 tag / 中心 */
        for (i = 0; i < r->nstars; i++)
            if (r->stars[i].hit && r->stars[i].vis > .3)
                fprintf(r->log, " %d/%d:%.0f,%.0f", i, r->stars[i].galaxy + 1,
                        (r->stars[i].bx0 + r->stars[i].bx1) * .5, (r->stars[i].by0 + r->stars[i].by1) * .5);
        fprintf(r->log, "\n");
        fflush(r->log);
    }
    galaxyrenderholdfx();
    galaxyrenderevents();
    galaxyrenderfx();
    galaxyrenderparticles();
    galaxyrendertitle();
    galaxyrendercorelabel();
    galaxyrenderquery();
    galaxyrendercentral();
    galaxyrenderfront();
    galaxyrenderhud();
    next = galaxynow(); r->phasecost[3] += next - t; t = next;
    galaxypresent();
    next = galaxynow(); r->phasecost[4] += next - t; t = next;
    /* 等服务器画完这一帧: 既是帧时间的真实测量, 也避免请求堆积 */
    XSync(dpy, False);
    r->phasecost[5] += galaxynow() - t;
}
