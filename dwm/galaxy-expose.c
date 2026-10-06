/* Super+A: 窗口总览 (expose). 与星系共用遮罩、截图和卡片渲染, 所有卡片都在 z=0 平面上, 镜头正对:
 * 打开时当前桌面的窗口从原位置滑进网格、其他 tag 的窗口在格子里淡入, 背景压暗并稍稍后退;
 * 悬停 / 键盘选中的卡片浮起并亮起光框, 卡片下方是标题; 选中后走与星系相同的「进入窗口」落位 (galaxyupdateland),
 * 不选则飞回原位 (galaxyupdatereturn). 遮罩只盖发起的那块屏, 其他屏照常显示 */

static int galaxykeyactive(void);

/* 标题放不下时按 UTF-8 字符截断并加省略号 */
static void
galaxyfittext(XftFont *font, char *text, int maxw)
{
    XGlyphInfo ext;
    size_t n = strlen(text);
    char buf[80];

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
            snprintf(text, 64, "%s", buf);
            return;
        }
    }
}

/* 网格: 列数取能放下的最小正方形, 每格按窗口比例缩放 (不放大), 各行居中; 坐标以视口中心为原点 */
static void
galaxyexposelayout(void)
{
    GalaxyScene *r = &galaxyscene;
    Monitor *m = r->savedmon;
    GalaxyStar *s;
    int n = r->nstars, cols, rows, row, i, j, k;
    double outer = .045 * r->vw, gap = .02 * r->vw, title = (r->titlefont ? r->titlefont->height : 16) + 16;
    double cw, ch, x, y, roww, rowh, total = 0, rowh_[64] = {0};

    if (n <= 0)
        return;
    for (cols = 1; cols * cols < n; cols++);
    rows = (cols - 1) * cols >= n ? cols - 1 : cols;
    cw = (m->ww - 2 * outer - (cols - 1) * gap) / cols;
    ch = (m->wh - 2 * outer - (rows - 1) * gap) / rows - title;
    for (i = 0; i < n; i++) {
        s = &r->stars[i];
        s->es = MIN(1, MIN(cw / s->w, ch / s->h));
    }
    for (row = 0; row < rows && row < 64; row++) {
        for (j = 0, rowh = 0; j < cols && row * cols + j < n; j++)
            rowh = MAX(rowh, r->stars[row * cols + j].h * r->stars[row * cols + j].es);
        rowh_[row] = rowh + title;
        total += rowh_[row] + (row ? gap : 0);
    }
    y = m->wy + (m->wh - total) * .5;
    for (row = 0; row < rows && row < 64; row++) {
        for (j = 0, roww = 0; j < cols && row * cols + j < n; j++)
            roww += r->stars[row * cols + j].w * r->stars[row * cols + j].es + (j ? gap : 0);
        x = m->wx + (m->ww - roww) * .5;
        for (j = 0; j < cols && (k = row * cols + j) < n; j++) {
            s = &r->stars[k];
            s->ex = x + s->w * s->es * .5 - r->vx - r->vw * .5;
            s->ey = y + (rowh_[row] - title) * .5 - r->vy - r->vh * .5;
            x += s->w * s->es + gap;
            galaxyfittext(r->titlefont, s->title, (int)MAX(160, s->w * s->es));
        }
        y += rowh_[row] + gap;
    }
}

/* 当前高亮的卡片: 键盘最近操作过就用键盘选中的, 否则用鼠标悬停的 */
static int
galaxyexposesel(void)
{
    GalaxyScene *r = &galaxyscene;

    if (galaxykeyactive() && r->ksel >= 0 && r->ksel < r->nstars)
        return r->ksel;
    return r->hover >= 0 && r->hover < r->nstars ? r->hover : -1;
}

static void
galaxyexposeupdate(double t, double dt)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyStar *s;
    GalaxyMat id = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    double open = galaxyeaseoutcubic(galaxyphase(t, .03, GALAXYXOPEN)), fadein = galaxysmoothstep(galaxyphase(t, .06, GALAXYXOPEN));
    int i, sel = galaxyexposesel();

    galaxysetcameraat(galaxyv(0, 0, 0), r->cam.focal, 0, 0, 0);
    r->nitems = 0;
    for (i = 0; i < r->nstars; i++) {
        s = &r->stars[i];
        s->hover = galaxyfollow(s->hover, i == sel, dt, .07);
        s->orient = id;
        s->kw = s->kh = 1;
        if (s->current) {   /* 当前桌面上的窗口: 第一帧与真实窗口重合, 滑进格子 */
            s->pos = galaxylerp(s->home, galaxyv(s->ex, s->ey, 0), open);
            s->size = galaxymix(1, s->es, open);
            s->alpha = 1;
        } else {            /* 其他 tag / 隐藏的窗口: 在格子里由小变大淡入 */
            s->pos = galaxyv(s->ex, s->ey, 0);
            s->size = s->es * galaxymix(.9, 1, open);
            s->alpha = fadein;
        }
        s->size *= 1 + .045 * s->hover;
        if (s->died)        /* 总览中关闭的窗口: 缩小淡出 */
            s->alpha *= 1 - galaxysmoothstep((galaxynow() - s->died) / .3);
        s->vis = s->alpha * (r->kqlen && !s->kmatch ? .3 : 1);
        s->tint = s->glow = 0;
        s->brightness = (sel >= 0 && i != sel ? galaxymix(1, .8, open) : 1) * (1 + .08 * s->hover);
        s->p = galaxyproject(s->pos);
        if (s->alpha > .002 && s->p.ok)     /* 高亮的卡片画在最上面 */
            r->items[r->nitems++] = (GalaxyItem){GalaxyStarItem, i, 1 - s->hover};
    }
    qsort(r->items, r->nitems, sizeof *r->items, galaxyitemcmp);
    r->bright = galaxymix(1, .42, open);
    r->vign = .55 * open;
    r->desk = 1 - galaxysmoothstep(galaxyphase(t, 0, .1));
    r->space = r->deskover = r->bar = 0;
}

/* 卡片四周的光框 (高亮 / 选中时) */
static void
galaxyexposeframe(GalaxyStar *s, double a)
{
    GalaxyProj c[5];
    double x0 = s->bx0 - 3, y0 = s->by0 - 3, x1 = s->bx1 + 3, y1 = s->by1 + 3;
    int i;

    c[0] = galaxysp(x0, y0);
    c[1] = galaxysp(x1, y0);
    c[2] = galaxysp(x1, y1);
    c[3] = galaxysp(x0, y1);
    c[4] = c[0];
    for (i = 0; i < 4; i++) {
        galaxyband(&c[i], &c[i + 1], .75 * a, 1.3);
        galaxyband(&c[i], &c[i + 1], .16 * a, 8);
    }
}

static void
galaxyexposerender(void)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyStar *s;
    XRenderColor shade = {0, 0, 0, 0};
    double depth = 1 - .035 * galaxyclamp((1 - r->bright) / .58), t;
    int i, x = r->vx, y = r->vy, w = r->vw, h = r->vh, open = r->mode != GalaxyReturn;

    /* 背景: 壁纸稍稍后退 (缩小) 并压暗; 开始时截的桌面在第一帧盖在上面, 很快淡出 */
    if (depth < .9995) {
        /* 目标 -> 源: p = c + (d - c) / depth, 壁纸以视口中心缩小 (边缘露出的一窄条在暗角里) */
        galaxyaffine(r->wallpaper, 1 / depth, 1 / depth, (1 - 1 / depth) * (x + w * .5), (1 - 1 / depth) * (y + h * .5));
        XRenderSetPictureFilter(dpy, r->wallpaper, FilterBilinear, NULL, 0);
    }
    XRenderComposite(dpy, PictOpSrc, r->wallpaper, None, r->back, x, y, 0, 0, x, y, w, h);
    if (depth < .9995) {
        galaxyaffine(r->wallpaper, 1, 1, 0, 0);
        XRenderSetPictureFilter(dpy, r->wallpaper, FilterNearest, NULL, 0);
    }
    if (r->desk > .004)
        XRenderComposite(dpy, PictOpOver, r->desktop, galaxywhite(r->desk), r->back, x, y, 0, 0, x, y, w, h);
    shade.alpha = (unsigned short)(65535 * galaxyclamp(1 - r->bright));
    if (shade.alpha)
        XRenderFillRectangle(dpy, PictOpOver, r->back, &shade, x, y, w, h);
    if (r->vignette && r->vign > .004)
        XRenderComposite(dpy, PictOpOver, r->vignette, galaxywhite(r->vign), r->back, x, y, 0, 0, x, y, w, h);
    for (i = 0; i < r->nstars; i++)
        r->stars[i].hit = 0;
    for (i = 0; i < r->nitems; i++) {
        if (r->items[i].kind != GalaxyStarItem)
            continue;
        s = &r->stars[r->items[i].index];
        if (r->bandn)
            galaxyflushbands();     /* 前面卡片的光框不能盖在后面的卡片上 */
        if (s->glow > .01)          /* 进入时领先的卡片背后的柔光 */
            galaxysprite(GalaxyHalo, GalaxyCool, s->p.x, s->p.y, .62 * hypot(s->w * s->kw, s->h * s->kh) * s->size * s->p.scale,
                    MIN(1, .22 * s->glow));
        galaxyrenderwindow(s, s->vis, s->tint, s->brightness);
        if (open && s->hover > .01 && s->hit)
            galaxyexposeframe(s, s->hover * s->alpha);
    }
    galaxyflushbands();
    /* 标题: 卡片下方居中, 高亮的更亮 */
    for (i = 0; open && i < r->nstars; i++) {
        s = &r->stars[i];
        t = galaxyclamp(1 - r->desk) * s->vis * (.55 + .45 * s->hover);
        if (s->hit && s->title[0] && r->titlefont)
            galaxytext(r->titlefont, (s->bx0 + s->bx1) * .5, s->by1 + 10 + r->titlefont->ascent, s->title, t, 1);
    }
    galaxyrenderquery();
    galaxyrenderfront();
    galaxypresent();
    XSync(dpy, False);
}
