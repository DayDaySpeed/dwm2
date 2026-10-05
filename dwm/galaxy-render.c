/* Super+Z 星系: XRender 绘制. 卡片 / 光点 / 光带画布 / 轨道 / 星轨 / 开场与驻留光效 / 每帧合成.
 * 由 galaxy.c 按顺序 include. */

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

/* 光点精灵: 亚像素位置用变换的平移表示, 慢速运动不会逐像素跳动 */
static void
galaxysprite(int shape, int tint, double x, double y, double radius, double alpha)
{
    GalaxyScene *r = &galaxyscene;
    double d = radius * 2, s;
    int lvl, x0, y0, size;

    if (alpha < 1.0 / 255 || radius < .25 || x + radius < 0 || y + radius < 0
            || x - radius > r->w || y - radius > r->h)
        return;
    lvl = d > 24 ? 0 : d > 6 ? 1 : 2;
    s = galaxyspritesize[lvl] / d;
    x0 = (int)floor(x - radius) - 1;
    y0 = (int)floor(y - radius) - 1;
    size = (int)ceil(d) + 3;
    galaxyaffine(r->sprite[shape][tint][lvl], s, s, -(x - radius - x0) * s, -(y - radius - y0) * s);
    XRenderComposite(dpy, PictOpOver, r->sprite[shape][tint][lvl], galaxywhite(alpha), r->back,
            0, 0, 0, 0, x0, y0, size, size);
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

static void
galaxyrenderwindow(GalaxyStar *s, double vis, double tint, double light)
{
    GalaxyScene *r = &galaxyscene;
    static const double sx[4] = {-1, 1, 1, -1}, sy[4] = {-1, -1, 1, 1};
    double q[4][2], minx = 1e9, miny = 1e9, maxx = -1e9, maxy = -1e9, edge, hw, hh, dark;
    double left, top, rw, rh, persp;
    GalaxyVec corner, normal, tocam;
    GalaxyProj p;
    Picture mask;
    int i, lvl, x0, y0, x1, y1, cx, cy, tiled, aligned, pixelcopy, drawn, opaque;

    if (!s->snap || (vis < .004 && tint < .004))
        return;
    hw = s->w * .5 * s->size;
    hh = s->h * .5 * s->size;
    for (i = 0; i < 4; i++) {
        corner = galaxyadd(s->pos, galaxyapply(s->orient, galaxyv(sx[i] * hw, sy[i] * hh, 0)));
        p = galaxyproject(corner);
        if (!p.ok || p.z < r->cam.focal * .25)
            return;
        q[i][0] = p.x;
        q[i][1] = p.y;
        minx = MIN(minx, p.x); maxx = MAX(maxx, p.x);
        miny = MIN(miny, p.y); maxy = MAX(maxy, p.y);
    }
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
    pixelcopy = aligned && opaque && fabs(rw - s->mipw[lvl]) < 1.25 && fabs(rh - s->miph[lvl]) < 1.25;
    x0 = (int)floor(minx);
    y0 = (int)floor(miny);
    x1 = (int)ceil(maxx) + 1;
    y1 = (int)ceil(maxy) + 1;
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
    double rr = MIN(radius * p.scale, 80);   /* 俯冲时核心近在眼前: 限制光晕半径, 避免整屏的大面积合成 */

    if (outer > 0)
        galaxysprite(GalaxyHalo, tint, p.x, p.y, rr * 7 * (1 + .3 * blur), outer * alpha);
    galaxysprite(GalaxyHalo, tint, p.x, p.y, rr * (3 + .8 * blur), halo * alpha);
    galaxysprite(GalaxyDisc, tint, p.x, p.y, MAX(.7, rr), alpha * (1 - .5 * blur));
}

/* 细光带 (轨道环 / 尾迹 / 冲击环): 软件画进 a8 画布 (按距离算覆盖率的抗锯齿胶囊), 同一画布内取最大值, 重叠处不叠亮.
 * a 是白色的不透明度 (上限 .25), hw 是半宽 */
static void
galaxyband(GalaxyProj *pa, GalaxyProj *pb, double a, double hw)
{
    GalaxyScene *r = &galaxyscene;
    float ax = pa->x, ay = pa->y, dx = pb->x - pa->x, dy = pb->y - pa->y, l2, ext, t, px, py, ex, ey, d, cov;
    int x0, y0, x1, y1, x, y, lo, hi, v, tx, ty, steep;
    unsigned char *row;

    double t0;

    if (!r->bandbuf || a < 1 / 64.0)
        return;
    t0 = galaxynow();
    a = MIN(a, .25);
    hw = MIN(hw, 9);
    l2 = dx * dx + dy * dy;
    if (l2 < .09)
        return;
    ext = hw + 1;
    x0 = MAX(0, (int)floor(MIN(ax, ax + dx) - ext));
    x1 = MIN(r->w - 1, (int)ceil(MAX(ax, ax + dx) + ext));
    y0 = MAX(0, (int)floor(MIN(ay, ay + dy) - ext));
    y1 = MIN(r->h - 1, (int)ceil(MAX(ay, ay + dy) + ext));
    if (x0 > x1 || y0 > y1)
        return;
    steep = fabsf(dy) > fabsf(dx);
    /* 沿主轴逐列 (或逐行) 扫描, 每列只算光带附近的几个像素 */
    for (v = steep ? y0 : x0; v <= (steep ? y1 : x1); v++) {
        if (steep) {
            t = (v + .5f - ay) / dy;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            px = ax + t * dx;
            lo = MAX(x0, (int)floor(px - ext * sqrtf(l2) / fabsf(dy) - 1));
            hi = MIN(x1, (int)ceil(px + ext * sqrtf(l2) / fabsf(dy) + 1));
        } else {
            t = (v + .5f - ax) / dx;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            py = ay + t * dy;
            lo = MAX(y0, (int)floor(py - ext * sqrtf(l2) / fabsf(dx) - 1));
            hi = MIN(y1, (int)ceil(py + ext * sqrtf(l2) / fabsf(dx) + 1));
        }
        for (; lo <= hi; lo++) {
            x = steep ? lo : v;
            y = steep ? v : lo;
            ex = x + .5f - ax;
            ey = y + .5f - ay;
            t = (ex * dx + ey * dy) / l2;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            ex -= t * dx;
            ey -= t * dy;
            d = sqrtf(ex * ex + ey * ey);
            cov = hw + .5f - d;
            if (cov <= 0)
                continue;
            cov = (cov > 1 ? 1 : cov) * a * 255 + .5f;
            row = r->bandbuf + (size_t)y * r->w + x;
            if (*row < (unsigned char)cov)
                *row = (unsigned char)cov;
        }
    }
    for (ty = y0 / GALAXYBTILE; ty <= y1 / GALAXYBTILE; ty++)
        for (tx = x0 / GALAXYBTILE; tx <= x1 / GALAXYBTILE; tx++)
            if (!r->banddirty[ty * r->bandtw + tx]) {
                r->banddirty[ty * r->bandtw + tx] = 1;
                r->bandn++;
            }
    r->bandraster += galaxynow() - t0;
}

/* 画布上是否有尚未合成、且与这个屏幕矩形相交的光带 */
static int
galaxybandpending(double x0, double y0, double x1, double y1)
{
    GalaxyScene *r = &galaxyscene;
    int tx, ty, tx0, tx1, ty1;

    if (!r->bandn)
        return 0;
    tx0 = MAX(0, (int)floor(x0 / GALAXYBTILE));
    ty = MAX(0, (int)floor(y0 / GALAXYBTILE));
    tx1 = MIN(r->bandtw - 1, (int)floor(x1 / GALAXYBTILE));
    ty1 = MIN(r->bandth - 1, (int)floor(y1 / GALAXYBTILE));
    for (; ty <= ty1; ty++)
        for (tx = tx0; tx <= tx1; tx++)
            if (r->banddirty[ty * r->bandtw + tx])
                return 1;
    return 0;
}

/* 把画布上有内容的块 (同一行相邻的块合并) 上传并以白色合成到场景, 然后清空这些块 */
static void
galaxyflushbands(void)
{
    GalaxyScene *r = &galaxyscene;
    int tx, ty, run, x, y, w, h, j;
    double t0;

    if (!r->bandn)
        return;
    t0 = galaxynow();
    for (ty = 0; ty < r->bandth; ty++)
        for (tx = 0; tx < r->bandtw; tx++) {
            if (!r->banddirty[ty * r->bandtw + tx])
                continue;
            for (run = 1; tx + run < r->bandtw && r->banddirty[ty * r->bandtw + tx + run]; run++);
            x = tx * GALAXYBTILE;
            y = ty * GALAXYBTILE;
            w = MIN(run * GALAXYBTILE, r->w - x);
            h = MIN(GALAXYBTILE, r->h - y);
            XPutImage(dpy, r->bandpix, r->bandgc, r->bandimg, x, y, x, y, w, h);
            XRenderComposite(dpy, PictOpOver, r->white[GALAXYALPHAS - 1], r->bandpic, r->back, 0, 0, x, y, x, y, w, h);
            for (j = 0; j < h; j++)   /* XPutImage 已把数据拷进请求缓冲区, 可以马上清 */
                memset(r->bandbuf + (size_t)(y + j) * r->w + x, 0, w);
            memset(r->banddirty + ty * r->bandtw + tx, 0, run);
            r->bandtiles += run;
            tx += run - 1;
        }
    r->bandn = 0;
    r->bandflushes++;
    r->bandflush += galaxynow() - t0;
}

/* 局部轨道: 暗的外沿与清晰的细线叠在短弧内, 由画家算法处理穿插. */
static void
galaxyrenderring(int index)
{
    GalaxyScene *r = &galaxyscene;
    int gi = index / (GALAXYARCS * GALAXYRINGS), k = index / GALAXYARCS % GALAXYRINGS;
    int arc = index % GALAXYARCS, j;
    GalaxyCore *g = &r->galaxies[gi];
    GalaxyProj *pts = r->rpts + (gi * GALAXYRINGS + k) * (GALAXYSEG + 1);
    double base = r->ringalpha * MIN(1, g->alpha) * (1 + .55 * g->hover + 3 * g->callout), z, depth, width;
    double reveal = galaxyringreveal(g) * GALAXYSEG;

    for (j = arc * GALAXYARCSEG; j < (arc + 1) * GALAXYARCSEG; j++) {
        if (!pts[j].ok || !pts[j + 1].ok || j >= reveal)
            continue;
        if (j + 1 > reveal)   /* 光笔笔尖 */
            galaxysprite(GalaxyHalo, GalaxyCool, pts[j].x, pts[j].y, 14 * r->starscale * MAX(.5, pts[j].scale), .5 * MIN(1, g->alpha));
        z = (pts[j].z + pts[j + 1].z) * .5;
        depth = galaxydepthlight(z) * galaxynearfade(z);
        width = MAX(.65, (pts[j].scale + pts[j + 1].scale) * .5 * r->starscale);
        galaxyband(&pts[j], &pts[j + 1], base * depth * .24 * (1 - galaxyclamp(r->qualityvisual - 2)), width * 3.2);
        galaxyband(&pts[j], &pts[j + 1], base * depth * 1.25, width * .7);
    }
}

/* 椭圆群轨道底线: 很淡的细线, 涟漪经过时沿轨道亮起一圈 */
static void
galaxyrendercluster(int index)
{
    GalaxyScene *r = &galaxyscene;
    int lane = index / GALAXYARCS, arc = index % GALAXYARCS, j;
    GalaxyProj *pts = r->clusterpts[lane];
    double a, wave, width, rr;

    for (j = arc * GALAXYARCSEG; j < (arc + 1) * GALAXYARCSEG; j++) {
        if (!pts[j].ok || !pts[j + 1].ok || j >= r->lanereveal[lane] * GALAXYSEG)
            continue;
        if (j + 1 > r->lanereveal[lane] * GALAXYSEG)   /* 光笔笔尖 */
            galaxysprite(GalaxyHalo, GalaxyCool, pts[j].x, pts[j].y, 22 * r->starscale * MAX(.5, pts[j].scale), .6);
        a = r->clusteralpha * galaxydepthlight((pts[j].z + pts[j + 1].z) * .5) * galaxynearfade(pts[j].z);
        width = MAX(.6, .8 * (pts[j].scale + pts[j + 1].scale) * .5);
        wave = 0;
        if (r->rippleamp > .003) {
            rr = (r->clusterr[lane][j] + r->clusterr[lane][j + 1]) * .5;
            wave = r->rippleamp * exp(-pow((rr - r->ripple) / (.03 * r->vw), 2));
        }
        galaxyband(&pts[j], &pts[j + 1], a * (1 + 5 * wave), width * (.6 + .5 * wave));
        if (wave > .05)
            galaxyband(&pts[j], &pts[j + 1], .12 * wave * galaxydepthlight(pts[j].z), width * 3.5);
    }
}

/* 长曝光星轨: 从核心往回渐隐, 线宽从核心处向尾端变细. index = 核心 * 3 + 按深度划分的段 */
static void
galaxyrenderstreak(int index)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyCore *g = &r->galaxies[index / 3];
    GalaxyProj *pts = r->streakpts + index / 3 * (GALAXYSTREAK + 1);
    int j0 = index % 3 * GALAXYSTREAK / 3, j1 = (index % 3 + 1) * GALAXYSTREAK / 3, j;
    double base = r->streakalpha * MIN(1, g->alpha) * (1 + .6 * g->peri + .5 * g->flare + .8 * g->ripple) * (1 + .4 * g->hover);
    double u, a, width;

    for (j = j0; j < j1; j++) {
        if (!pts[j].ok || !pts[j + 1].ok || j >= g->streakreveal * GALAXYSTREAK)
            continue;
        u = (j + .5) / GALAXYSTREAK;
        a = base * pow(1 - u, 1.6) * galaxydepthlight((pts[j].z + pts[j + 1].z) * .5) * galaxynearfade(pts[j].z);
        width = MAX(.6, (pts[j].scale + pts[j + 1].scale) * .5 * (3 - 2.3 * u));
        galaxyband(&pts[j], &pts[j + 1], a, width);
        if (u < .5)
            galaxyband(&pts[j], &pts[j + 1], a * .35 * (1 - u / .5) * (1 - galaxyclamp(r->qualityvisual - 2)), width * 3.5);
    }
}

/* 中心光源: 三条椭圆的共同焦点, 发出涟漪时脉冲一次 */
static void
galaxyrendersun(void)
{
    GalaxyScene *r = &galaxyscene;
    double a = r->sunalpha * (1 + 1.2 * r->sunpulse);

    galaxyrenderglow(GalaxyCool, r->sunp, 20 * r->starscale * (1 + .5 * r->sunpulse), MIN(1, .7 * a),
            galaxydepthblur(r->sunp.z), 1.1, .5 * r->glowscale * (1 + r->sunpulse));
}

static void
galaxyrenderstar(GalaxyStar *s)
{
    GalaxyScene *r = &galaxyscene;
    double blur = galaxydepthblur(s->p.z), rad, a;

    /* 截图面板背后的柔光, 让面板像发光体而不是贴图 */
    if (s->vis > .02 && s->glow > .01)
        galaxysprite(GalaxyHalo, GalaxyCool, s->p.x, s->p.y, .62 * hypot(s->w, s->h) * s->size * s->p.scale,
                .16 * r->glowscale * s->glow * MIN(1, s->vis * 1.3)
                * (r->mode == GalaxyOrbit ? 1 - galaxyclamp(r->qualityvisual - 1) : 1));
    galaxyrenderwindow(s, s->vis, s->tint, s->brightness);
    a = s->glow * (1 - .7 * MIN(1, s->vis * 1.5));
    rad = 8.5 * r->starscale * (s->focused ? 1.18 : 1) * (1 + .3 * s->hover);
    if (a > .002)
        galaxyrenderglow(GalaxyCool, s->p, rad, a, blur, .5 * r->glowscale * (1 + .5 * s->hover), 0);
    if (!s->hit && (a > .05 || s->vis > .05)) {
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
        if (r->items[i].kind == GalaxyStarItem && r->bandn) {
            /* 只有卡片会遮挡: 卡片之前若有与它相交的光带, 先合成 (光点 / 光晕之间的先后无关紧要) */
            GalaxyStar *st = &r->stars[r->items[i].index];
            double ext = .75 * MAX(st->w, st->h) * st->size * st->p.scale + 2;
            if (st->vis > .02 && galaxybandpending(st->p.x - ext, st->p.y - ext, st->p.x + ext, st->p.y + ext))
                galaxyflushbands();
        }
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
                + 3 * g->nova + 1.2 * g->bridge;
            galaxyrenderglow(GalaxyWarm, g->p, g->size * (1 + .15 * g->hover + .12 * g->peri + .2 * g->flare + .12 * g->ripple
                        + .4 * (g->ignite + g->callout) + .6 * g->nova + .2 * g->bridge),
                    MIN(1, g->alpha * galaxydepthlight(g->p.z) * (1 + a)) * galaxynearfade(g->p.z)
                    * (1 - .78 * g->eclipse),
                    galaxydepthblur(g->p.z), .55 * (1 + .4 * g->hover + a), .22 * r->glowscale * (1 + 1.5 * a)
                    * (r->mode == GalaxyOrbit ? 1 - galaxyclamp(r->qualityvisual - 2) : 1));
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
        case GalaxyRingItem:
            galaxyrenderring(r->items[i].index);
            break;
        case GalaxyClusterItem:
            galaxyrendercluster(r->items[i].index);
            break;
        case GalaxyStreakItem:
            galaxyrenderstreak(r->items[i].index);
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
    /* 起飞: 以焦点窗口为圆心, 屏幕空间的一圈光环 */
    u = galaxyphase(s, .08, .7);
    if (u > 0 && u < 1) {
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
        t = .62 + .06 * g->rank;
        u = galaxyphase(s, t, t + .5);
        if (u > 0 && u < 1 && g->p.ok)
            galaxyringfx(g->pos, g->plane, 2.4 * g->radius * galaxyeaseoutcubic(u), f * .24 * pow(1 - u, 1.3), 1.4);
    }
    /* 中心光源点火, 以及转速峰值时: 盘面 (xz) 上的大冲击环 */
    disk = galaxymul(r->world, galaxyrotx(GALAXYPI / 2));
    u = galaxyphase(s, 1.5, 2.1);
    if (u > 0 && u < 1)
        galaxyringfx(galaxyv(0, 0, 0), disk, .45 * r->vw * galaxyeaseoutcubic(u), f * .22 * (1 - u), 1.6);
    u = galaxyphase(s, 2.85, 3.5);
    if (u > 0 && u < 1)
        galaxyringfx(galaxyv(0, 0, 0), disk, .7 * r->vw * galaxyeaseoutcubic(u), f * .26 * (1 - u), 2.2);
    galaxyflushbands();
}

/* 屏幕空间光带的一个点 */
static GalaxyProj
galaxysp(double x, double y)
{
    return (GalaxyProj){x, y, 1, 1, 1};
}

/* 驻留特效: 轨道光流 / 星座连线 / 核心光桥 / 超新星冲击环 / 彗星 / 流星. 都是 motion 的函数, 强度乘 holdw */
static void
galaxyrenderholdfx(void)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyCore *g, *h;
    GalaxyStar *st;
    GalaxyProj pts[13], c, hp;
    GalaxyMat m, disk;
    GalaxyVec v, head, tail, A, B, perp;
    double f = r->holdw, t = r->motion * r->tscale, local, env, u, a, th, rad, ang, len, wave, phi, R;
    int i, j, k, l, n, order[32];

    if (f < .01 || (r->mode != GalaxyOrbit && r->mode != GalaxyCollapse))
        return;
    /* 轨道光流: 每条群椭圆 6 个光点, 每条局部环 2 个, 沿轨道流动, 带短尾 (安静模式不画) */
    for (l = 0; l < GALAXYLANES && !r->quiet; l++) {
        if (!(r->lanemask & 1 << l))
            continue;
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
                galaxysprite(GalaxyHalo, GalaxyCool, pts[0].x, pts[0].y, MAX(3, 10 * pts[0].scale * r->starscale),
                        f * .5 * (1 + wave) * galaxynearfade(pts[0].z));
        }
    }
    for (i = 0; i < r->ntags && !r->quiet; i++) {
        g = &r->galaxies[i];
        if (!g->nrings || g->alpha < .1 || !g->p.ok)
            continue;
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
                    galaxysprite(GalaxyHalo, GalaxyWarm, pts[0].x, pts[0].y, MAX(2.5, 7 * pts[0].scale * r->starscale),
                            f * .45 * g->alpha * galaxynearfade(pts[0].z));
            }
        }
    }
    /* 星座连线: 按轨道角把这个星系的窗口卡片连起来, 首颗再连到核心 */
    if (r->npop && galaxycycle(r->motion, 2, 4, &k, &local)) {
        g = &r->galaxies[r->constg >= 0 ? r->constg : r->popord[k % r->npop]];
        env = f * galaxysmoothstep(local / .4) * (1 - galaxysmoothstep((local - 1.6) / 1));
        for (i = n = 0; i < r->nstars && n < 32; i++)
            if (r->stars[i].galaxy == g->tag && r->stars[i].p.ok)
                order[n++] = i;
        for (i = 1; i < n; i++)     /* 按轨道角插入排序 (每个星系的星不多) */
            for (j = i; j > 0 && fmod(galaxyorbitangle(&r->stars[order[j]], GALAXYHOLD, r->motion) + 100 * GALAXYPI, 2 * GALAXYPI)
                    < fmod(galaxyorbitangle(&r->stars[order[j - 1]], GALAXYHOLD, r->motion) + 100 * GALAXYPI, 2 * GALAXYPI); j--) {
                l = order[j]; order[j] = order[j - 1]; order[j - 1] = l;
            }
        if (env > .01 && n) {
            for (i = 0; i < n; i++) {
                st = &r->stars[order[i]];
                hp = r->stars[order[(i + 1) % n]].p;
                if (n > 1 && (n > 2 || i == 0))
                    galaxyband(&st->p, &hp, .2 * env, 1.1);
                galaxysprite(GalaxyHalo, GalaxyCool, st->p.x, st->p.y, 12, .6 * env);
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
            galaxyband(&g->p, &h->p, .14 * env, 1.2);
            galaxyband(&g->p, &h->p, .05 * env, 5);
            u = galaxyeaseinoutcubic(galaxyphase(local, .25, 1.15));
            if (u > 0 && u < 1)
                galaxysprite(GalaxyHalo, GalaxyWarm, galaxymix(g->p.x, h->p.x, u), galaxymix(g->p.y, h->p.y, u), 18, .9 * env);
        }
    }
    /* 超新星: 核心所在轨道平面和盘面上各一圈冲击环 */
    if (r->novag >= 0 && galaxycycle(r->motion, 9, 20, &k, &local) && local < 2) {
        g = &r->galaxies[r->novag];
        disk = galaxymul(r->world, galaxyrotx(GALAXYPI / 2));
        u = galaxyphase(local, 0, 1.6);
        if (u < 1 && g->p.ok)
            galaxyringfx(g->pos, g->plane, 3 * MAX(g->radius, 60) * galaxyeaseoutcubic(u), f * .26 * (1 - u), 1.8);
        u = galaxyphase(local, .25, 2);
        if (u > 0 && u < 1 && g->p.ok)
            galaxyringfx(g->pos, disk, .3 * r->vw * galaxyeaseoutcubic(u), f * .18 * (1 - u), 1.3);
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
        for (j = 0; j <= 10; j++)
            pts[j] = galaxyproject(galaxyadd(head, galaxyscale(tail, j / 10.0)));
        for (j = 0; j < 10; j++)
            if (pts[j].ok && pts[j + 1].ok) {
                a = env * pow(1 - j / 10.0, 1.3) * galaxynearfade(pts[j].z);
                galaxyband(&pts[j], &pts[j + 1], .24 * a, MAX(.6, (2.6 - 2.2 * j / 10.0) * pts[j].scale));
                galaxyband(&pts[j], &pts[j + 1], .07 * a, MAX(2, 7 * pts[j].scale));
            }
        if (pts[0].ok && env * galaxynearfade(pts[0].z) > .01)
            galaxyrenderglow(GalaxyCool, pts[0], 14, MIN(1, .9 * env) * galaxynearfade(pts[0].z), galaxydepthblur(pts[0].z), 1, .35);
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
        for (j = 0; j < 12; j++)
            galaxyband(&pts[j], &pts[j + 1], .25 * env * pow(1 - j / 12.0, 1.5), 1.7 - 1.3 * j / 12);
        galaxysprite(GalaxyHalo, GalaxyCool, hp.x, hp.y, 12, .8 * env);
    }
    galaxyflushbands();
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
    XRenderFillRectangle(dpy, PictOpOver, r->back, &edge, x - 1, y - 1, w + 2, h + 2);
    XRenderFillRectangle(dpy, PictOpOver, r->back, &shade, x, y, w, h);
    XftDrawStringUtf8(r->titledraw, &r->titlecolor, r->queryfont, x + 24, y + 13 + r->queryfont->ascent, (XftChar8 *)text, len);
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
    XRenderFillRectangle(dpy, PictOpOver, r->back, &shade, x, y, w, h);
    XftDrawStringUtf8(r->titledraw, &r->titlecolor, r->titlefont, x + 11, y + 6 + r->titlefont->ascent,
            (XftChar8 *)s->title, len);
}

/* 低透明度的尾迹: 回溯时间求出星体之前的 3D 位置, 用当前镜头投影.
 * 高速旋转时是短促的流光, 驻留时拉长成沿轨道的彗星弧, 指示运动方向 */
static void
galaxyrendertrails(void)
{
    GalaxyScene *r = &galaxyscene;
    double gain = r->trailgain, dt, tk, mk, rate, a, speed;
    int i, k, n = r->ntrail, stride = r->ntrail + 1;
    GalaxyProj *pa, *pb;
    GalaxyCore *g;
    GalaxyMat world, orient;
    GalaxyVec pos;

    if (gain < .01 || n < 1)
        return;
    if (r->mode == GalaxyOrbit && r->qualityvisual >= 1.99)
        n = MAX(4, n / 2);
    /* 尾迹覆盖的总时长: 高速旋转时 0.12s (短促流光), 驻留时 1s (沿轨道的彗星弧) */
    dt = galaxymix(.12, 1, galaxysmoothstep(galaxyphase(r->stage, 3.3, 4.2))) / r->ntrail;
    rate = r->mode == GalaxyOrbit ? 0 : 1;   /* 驻留时 stage 停住, 只有 motion 在走 */
    for (k = 0; k <= n; k++) {
        if (k == 0) {
            for (i = 0; i < r->ntags; i++) {
                r->tgpos[i] = r->galaxies[i].pos;
                r->tgplane[i] = r->galaxies[i].plane;
                r->tpts[(r->nstars + i) * stride] = r->stage >= 2.8 ? r->galaxies[i].p : (GalaxyProj){0};
            }
            for (i = 0; i < r->nstars; i++)
                r->tpts[i * stride] = r->stars[i].p;
            continue;
        }
        tk = r->mode == GalaxyIntro ? galaxyintrostage(MAX(0, MIN(r->scene, GALAXYIEND) - k * dt)) : r->stage - k * dt * rate;
        mk = r->motion - k * dt;
        world = r->mode == GalaxyCollapse ? r->cworld : galaxyworldat(tk, mk);
        for (i = 0; i < r->ntags; i++) {
            galaxycoreat(&r->galaxies[i], tk, mk, world, &r->tgpos[i], &r->tgplane[i]);
            r->tpts[(r->nstars + i) * stride + k] = r->stage >= 2.8 ? galaxyproject(r->tgpos[i]) : (GalaxyProj){0};
        }
        for (i = 0; i < r->nstars; i++) {
            g = &r->galaxies[r->stars[i].galaxy];
            galaxystarat(&r->stars[i], tk, mk, world, r->tgpos[r->stars[i].galaxy],
                    galaxymul(r->tgplane[r->stars[i].galaxy], g->ring[r->stars[i].ring]), &pos, &orient);
            r->tpts[i * stride + k] = galaxyproject(pos);
        }
    }
    for (i = 0; i < r->nstars + r->ntags; i++) {
        if (i < r->nstars) {
            speed = galaxylen(r->stars[i].vel) / r->cam.focal;
            a = gain * r->stars[i].alpha * MIN(1, r->stars[i].brightness) * (.12 + .2 * galaxyclamp(3 * speed));
        } else {
            a = .16 * gain * r->galaxies[i - r->nstars].alpha * (1 - r->streakalpha / .24);  /* 驻留时由长曝光星轨取代 */
        }
        for (k = 0; k < n; k++) {
            pa = &r->tpts[i * stride + k];
            pb = &r->tpts[i * stride + k + 1];
            if (!pa->ok || !pb->ok)
                break;
            galaxyband(pa, pb, a * (1 - (double)k / r->ntrail)
                    * (r->mode == GalaxyOrbit && k >= r->ntrail / 2 ? 1 - galaxyclamp(r->qualityvisual - 1) : 1),
                    MAX(.45, .9 * pa->scale * r->starscale));
        }
    }
    galaxyflushbands();
}

static void
galaxyrenderbackground(void)
{
    GalaxyScene *r = &galaxyscene;
    XRenderColor shade = {0, 0, 0, 0};
    double key[4] = {r->bright, r->vign, r->desk, r->live ? r->reveal : 0};

    /* 参数与上一帧相同 (驻留态): 直接用缓存, 省掉全屏暗角缩放和填充 */
    if (r->bg && r->bgok && !memcmp(key, r->bgkey, sizeof key)) {
        XRenderComposite(dpy, PictOpSrc, r->bg, None, r->back, 0, 0, 0, 0, 0, 0, r->w, r->h);
        return;
    }
    XRenderComposite(dpy, PictOpSrc, r->wallpaper, None, r->back, 0, 0, 0, 0, 0, 0, r->w, r->h);
    if (r->live && r->reveal > 0)
        XRenderComposite(dpy, PictOpOver, r->live, galaxywhite(r->reveal), r->back, 0, 0, 0, 0, 0, 0, r->w, r->h);
    if (r->desktop && r->desk > 0)
        XRenderComposite(dpy, PictOpOver, r->desktop, galaxywhite(r->desk), r->back, 0, 0, 0, 0, 0, 0, r->w, r->h);
    shade.alpha = (unsigned short)(65535 * galaxyclamp(1 - r->bright));
    if (shade.alpha)
        XRenderFillRectangle(dpy, PictOpOver, r->back, &shade, 0, 0, r->w, r->h);
    if (r->vignette && r->vign > .004)
        XRenderComposite(dpy, PictOpOver, r->vignette, galaxywhite(r->vign), r->back, 0, 0, 0, 0, 0, 0, r->w, r->h);
    /* 连续两帧参数相同: 存下来, 之后直接复用 */
    if (r->bg && !memcmp(key, r->bglast, sizeof key)) {
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

    if (!r->desktop)
        return;
    if (r->deskover > .004)
        XRenderComposite(dpy, PictOpOver, r->desktop, galaxywhite(r->deskover), r->back, 0, 0, 0, 0, 0, 0, r->w, r->h);
    if (r->bar > .004 && r->barw > 0 && r->barh > 0)
        XRenderComposite(dpy, PictOpOver, r->desktop, galaxywhite(r->bar), r->back,
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

static void
galaxypresent(void)
{
    GalaxyScene *r = &galaxyscene;
    XRenderComposite(dpy, PictOpSrc, r->back, None, r->overlaypic, 0, 0, 0, 0, 0, 0, r->w, r->h);
}

static double galaxynow(void);

static double
galaxynow(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec - galaxyscene.start.tv_sec + (now.tv_nsec - galaxyscene.start.tv_nsec) / 1e9;
}

static void
galaxyrender(void)
{
    GalaxyScene *r = &galaxyscene;
    double t = galaxynow(), next;
    galaxyrenderbackground();
    next = galaxynow(); r->phasecost[1] += next - t; t = next;
    galaxyrendertrails();
    next = galaxynow(); r->phasecost[2] += next - t; t = next;
    galaxyrenderitems();
    galaxyrenderholdfx();
    galaxyrenderfx();
    galaxyrendertitle();
    galaxyrenderquery();
    galaxyrendercentral();
    galaxyrenderfront();
    next = galaxynow(); r->phasecost[3] += next - t; t = next;
    galaxypresent();
    next = galaxynow(); r->phasecost[4] += next - t; t = next;
    /* 等服务器画完这一帧: 既是帧时间的真实测量, 也避免请求堆积 */
    XSync(dpy, False);
    r->phasecost[5] += galaxynow() - t;
}
