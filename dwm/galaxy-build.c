/* Super+Z 星系: 资源与场景构建. 截图和 mip / 释放 / 星系布局与轨道分配 / 尘埃 / 背景 / 日志.
 * 由 galaxy.c 按顺序 include. */

/* ---------- 资源 ---------- */

static Picture
galaxyargb(int w, int h, Pixmap *pix)
{
    *pix = XCreatePixmap(dpy, root, MAX(1, w), MAX(1, h), 32);
    return XRenderCreatePicture(dpy, *pix, galaxyscene.argb, 0, NULL);
}

static Picture
galaxyopaque(int w, int h, Pixmap *pix)
{
    *pix = XCreatePixmap(dpy, root, w, h, DefaultDepth(dpy, screen));
    return XRenderCreatePicture(dpy, *pix, XRenderFindVisualFormat(dpy, DefaultVisual(dpy, screen)), 0, NULL);
}

/* 客户端算好的 ARGB 图上传成 Picture (只在开始时调用) */
static Picture
galaxyupload(int w, int h, unsigned int *data, Pixmap *pix)
{
    Picture p = galaxyargb(w, h, pix);
    XImage *img = XCreateImage(dpy, DefaultVisual(dpy, screen), 32, ZPixmap, 0, (char *)data, w, h, 32, 0);
    GC gc;

    if (!img) {
        free(data);
        return p;
    }
    gc = XCreateGC(dpy, *pix, 0, NULL);
    XPutImage(dpy, *pix, gc, img, 0, 0, 0, 0, w, h);
    XFreeGC(dpy, gc);
    XDestroyImage(img);
    XRenderSetPictureFilter(dpy, p, FilterBilinear, NULL, 0);
    return p;
}

static void
galaxybuildsprites(void)
{
    GalaxyScene *r = &galaxyscene;
    static const double tints[GalaxyTints][3] = {{1, .95, .87}, {.86, .91, 1}};
    unsigned int *data;
    double x, y, d, a, c;
    int shape, tint, lvl, n, i, j;

    for (shape = 0; shape < GalaxyShapes; shape++)
        for (tint = 0; tint < GalaxyTints; tint++)
            for (lvl = 0; lvl < GALAXYSPRITES; lvl++) {
                n = galaxyspritesize[lvl];
                if (!(data = malloc(n * n * 4)))
                    continue;
                for (j = 0; j < n; j++)
                    for (i = 0; i < n; i++) {
                        x = (i + .5) / n * 2 - 1;
                        y = (j + .5) / n * 2 - 1;
                        d = sqrt(x * x + y * y);
                        if (shape == GalaxyHalo)  /* 多层高斯叠加的柔光, 没有硬边 */
                            a = (.55 * exp(-d * d / .0288) + .3 * exp(-d * d / .1568) + .15 * exp(-d * d / .5))
                                * (1 - galaxysmoothstep((d - .75) / .25));
                        else
                            a = 1 - galaxysmoothstep((d - .4) / .6);
                        a = galaxyclamp(a);
                        c = a * 255;
                        data[j * n + i] = (unsigned int)(c + .5) << 24
                            | (unsigned int)(c * tints[tint][0] + .5) << 16
                            | (unsigned int)(c * tints[tint][1] + .5) << 8
                            | (unsigned int)(c * tints[tint][2] + .5);
                    }
                r->sprite[shape][tint][lvl] = galaxyupload(n, n, data, &r->spritepix[shape][tint][lvl]);
            }
}

static void
galaxybuildvignette(void)
{
    GalaxyScene *r = &galaxyscene;
    int w = 64, h = 36, i, j;
    unsigned int *data = malloc(w * h * 4);
    double x, y, a;

    if (!data)
        return;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) {
            x = (i + .5) / w * 2 - 1;
            y = (j + .5) / h * 2 - 1;
            a = .85 * galaxysmoothstep((sqrt(x * x + y * y) - .35) / .8);
            data[j * w + i] = (unsigned int)(a * 255 + .5) << 24;
        }
    r->vignette = galaxyupload(w, h, data, &r->vignettepix);
    galaxyaffine(r->vignette, (double)w / r->vw, (double)h / r->vh, -r->vx * (double)w / r->vw, -r->vy * (double)h / r->vh);
}

/* src 缩小到 dst (每级 2 倍, 双线性正好是 2x2 平均) */
static void
galaxyshrink(Picture src, int sw0, int sh0, Picture dst, int dw, int dh)
{
    galaxyaffine(src, (double)sw0 / dw, (double)sh0 / dh, 0, 0);
    XRenderSetPictureFilter(dpy, src, FilterBilinear, NULL, 0);
    XRenderComposite(dpy, PictOpSrc, src, None, dst, 0, 0, 0, 0, 0, 0, dw, dh);
}

static void
galaxybuildmips(GalaxyStar *s)
{
    int l;

    for (l = s->base + 1; l < GALAXYMIPS; l++) {
        s->mipw[l] = MAX(1, s->mipw[l - 1] / 2);
        s->miph[l] = MAX(1, s->miph[l - 1] / 2);
        if (s->mipw[l - 1] < 16 || s->miph[l - 1] < 16)
            break;
        s->mip[l] = galaxyargb(s->mipw[l], s->miph[l], &s->mippix[l]);
        galaxyshrink(s->mip[l - 1], s->mipw[l - 1], s->miph[l - 1], s->mip[l], s->mipw[l], s->miph[l]);
    }
    for (l = s->base; l < GALAXYMIPS; l++)
        if (s->mip[l])
            XRenderSetPictureFilter(dpy, s->mip[l], FilterBilinear, NULL, 0);
}

/* 每个窗口只截一次: 可见窗口截全尺寸, 其他 tag / 隐藏窗口直接截半尺寸 (它们从远处出现) */
static void
galaxycapture(GalaxyStar *s, Client *c, int full)
{
    GalaxyScene *r = &galaxyscene;
    XRenderPictureAttributes pa = {.subwindow_mode = IncludeInferiors};
    XRenderPictFormat *fmt;
    XWindowAttributes wa;
    XImage *img = c->preview.hidden_image;
    Picture src = 0;
    Pixmap tmp = 0;
    XRenderColor clear = {0, 0, 0, 0};
    int sw0, sh0;
    GC gc;

    if (s->hidden) {
        if (!img)
            return;  /* 绝不为了截图映射隐藏的真实窗口 */
        sw0 = img->width;
        sh0 = img->height;
        tmp = XCreatePixmap(dpy, root, sw0, sh0, img->depth);
        gc = XCreateGC(dpy, tmp, 0, NULL);
        XPutImage(dpy, tmp, gc, img, 0, 0, 0, 0, sw0, sh0);
        XFreeGC(dpy, gc);
        fmt = img->depth == 32 ? r->argb : XRenderFindStandardFormat(dpy, PictStandardRGB24);
        src = XRenderCreatePicture(dpy, tmp, fmt, 0, NULL);
    } else {
        if (!XGetWindowAttributes(dpy, c->win, &wa) || wa.map_state != IsViewable
                || !(fmt = XRenderFindVisualFormat(dpy, wa.visual)))
            return;
        sw0 = wa.width;
        sh0 = wa.height;
        src = XRenderCreatePicture(dpy, c->win, fmt, CPSubwindowMode, &pa);
    }
    if (!src || sw0 <= 0 || sh0 <= 0)
        goto done;
    s->base = full ? 0 : 1;
    s->mipw[s->base] = full ? sw0 : MAX(1, sw0 / 2);
    s->miph[s->base] = full ? sh0 : MAX(1, sh0 / 2);
    s->mip[s->base] = galaxyargb(s->mipw[s->base], s->miph[s->base], &s->mippix[s->base]);
    XRenderFillRectangle(dpy, PictOpSrc, s->mip[s->base], &clear, 0, 0, s->mipw[s->base], s->miph[s->base]);
    if (!full) {
        galaxyaffine(src, (double)sw0 / s->mipw[1], (double)sh0 / s->miph[1], 0, 0);
        XRenderSetPictureFilter(dpy, src, FilterBilinear, NULL, 0);
    }
    XRenderComposite(dpy, PictOpOver, src, None, s->mip[s->base], 0, 0, 0, 0, 0, 0, s->mipw[s->base], s->miph[s->base]);
    galaxybuildmips(s);
    s->snap = 1;
done:
    if (src)
        XRenderFreePicture(dpy, src);
    if (tmp)
        XFreePixmap(dpy, tmp);
}

static void
galaxyfreemip(GalaxyStar *s, int l)
{
    if (s->mip[l])
        XRenderFreePicture(dpy, s->mip[l]);
    if (s->mippix[l])
        XFreePixmap(dpy, s->mippix[l]);
    s->mip[l] = 0;
    s->mippix[l] = 0;
}

static void
galaxyfreestar(GalaxyStar *s)
{
    int l;

    for (l = 0; l < GALAXYMIPS; l++)
        galaxyfreemip(s, l);
}

/* 场景资源 (截图 / 精灵 / 数组); 停在壁纸或退出时释放 */
static void
galaxyfreescene(void)
{
    GalaxyScene *r = &galaxyscene;
    int i, j, k;

    for (i = 0; i < r->nstars; i++)
        galaxyfreestar(&r->stars[i]);
    if (r->titledraw)
        XftDrawDestroy(r->titledraw);
    if (r->titlecolorok)
        XftColorFree(dpy, DefaultVisual(dpy, screen), DefaultColormap(dpy, screen), &r->titlecolor);
    if (r->titlefont)
        XftFontClose(dpy, r->titlefont);
    if (r->queryfont)
        XftFontClose(dpy, r->queryfont);
    r->titledraw = NULL;
    r->titlefont = r->queryfont = NULL;
    r->titlecolorok = 0;
    for (i = 0; i < GalaxyShapes; i++)
        for (j = 0; j < GalaxyTints; j++)
            for (k = 0; k < GALAXYSPRITES; k++) {
                if (r->sprite[i][j][k])
                    XRenderFreePicture(dpy, r->sprite[i][j][k]);
                if (r->spritepix[i][j][k])
                    XFreePixmap(dpy, r->spritepix[i][j][k]);
                r->sprite[i][j][k] = 0;
                r->spritepix[i][j][k] = 0;
            }
    for (i = 0; i < GALAXYALPHAS; i++) {
        if (r->black[i])
            XRenderFreePicture(dpy, r->black[i]);
        r->black[i] = 0;
    }
    if (r->vignette)
        XRenderFreePicture(dpy, r->vignette);
    if (r->vignettepix)
        XFreePixmap(dpy, r->vignettepix);
    if (r->tilemask)
        XRenderFreePicture(dpy, r->tilemask);
    if (r->tilemaskpix)
        XFreePixmap(dpy, r->tilemaskpix);
    if (r->spinpic)
        XRenderFreePicture(dpy, r->spinpic);
    if (r->spinpix)
        XFreePixmap(dpy, r->spinpix);
    r->vignette = 0;
    r->vignettepix = 0;
    r->tilemask = 0;
    r->tilemaskpix = 0;
    r->spinpic = 0;
    r->spinpix = 0;
    free(r->stars);
    free(r->galaxies);
    free(r->dust);
    free(r->items);
    free(r->tgpos);
    free(r->tgplane);
    free(r->tpts);
    free(r->rpts);
    free(r->streakpts);
    free(r->streakz);
    free(r->popord);
    r->streakpts = NULL;
    r->streakz = NULL;
    r->popord = NULL;
    if (r->bandimg)
        XDestroyImage(r->bandimg);  /* 连同 bandbuf 一起释放 */
    else
        free(r->bandbuf);
    if (r->bandpic)
        XRenderFreePicture(dpy, r->bandpic);
    if (r->bandpix)
        XFreePixmap(dpy, r->bandpix);
    if (r->bandgc)
        XFreeGC(dpy, r->bandgc);
    free(r->banddirty);
    r->bandimg = NULL;
    r->bandbuf = r->banddirty = NULL;
    r->bandpic = 0;
    r->bandpix = 0;
    r->bandgc = 0;
    r->bandn = 0;
    r->stars = NULL;
    r->galaxies = NULL;
    r->dust = NULL;
    r->items = NULL;
    r->tgpos = NULL;
    r->tgplane = NULL;
    r->tpts = r->rpts = NULL;
    r->nstars = r->ntags = r->ndust = r->nitems = 0;
    r->hover = r->hovercore = -1;
}

static void
galaxyfree(void)
{
    GalaxyScene *r = &galaxyscene;
    int i;

    galaxyfreescene();
    for (i = 0; i < GALAXYALPHAS; i++)
        if (r->white[i])
            XRenderFreePicture(dpy, r->white[i]);
    if (r->overlaypic) XRenderFreePicture(dpy, r->overlaypic);
    if (r->overlay) XDestroyWindow(dpy, r->overlay);
    if (r->hand) XFreeCursor(dpy, r->hand);
    if (r->blankcursor) XFreeCursor(dpy, r->blankcursor);
    if (r->back) XRenderFreePicture(dpy, r->back);
    if (r->backpix) XFreePixmap(dpy, r->backpix);
    if (r->bg) XRenderFreePicture(dpy, r->bg);
    if (r->bgpix) XFreePixmap(dpy, r->bgpix);
    if (r->desktop) XRenderFreePicture(dpy, r->desktop);
    if (r->desktoppix) XFreePixmap(dpy, r->desktoppix);
    if (r->wallpaper) XRenderFreePicture(dpy, r->wallpaper);
    if (r->wallpix) XFreePixmap(dpy, r->wallpix);
    if (r->live) XRenderFreePicture(dpy, r->live);
    free(r->gaps);
    if (r->log)
        fclose(r->log);
    memset(r, 0, sizeof *r);
}

static int
galaxyxerror(Display *d, XErrorEvent *ee)
{
    /* 截图 / 合成过程中窗口可能随时消失; dwm 的 xerror 遇到 Render 错误会直接退出整个会话 */
    if (ee->request_code == galaxyscene.rendermajor || ee->error_code == BadWindow
            || ee->error_code == BadDrawable || ee->error_code == BadPixmap || ee->error_code == BadMatch
            || ee->request_code == X_CreatePixmap || ee->request_code == X_FreePixmap
            || ee->request_code == X_PutImage || ee->request_code == X_CreateGC
            || ee->request_code == X_GetWindowAttributes || ee->request_code == X_QueryTree
            || ee->request_code == X_GrabPointer || ee->request_code == X_GrabKeyboard) {
        galaxyscene.errors++;
        return 0;
    }
    return xerror(d, ee);
}

/* ---------- 场景构建 ---------- */

/* 星系群布局: 有窗口的星系黄金角分布在一个铺满屏幕的椭球里 (x/y 占满画面, z 拉开近 / 中 / 远三层);
 * 没有窗口的 tag 退到后景作为远处的深度参照, 不占主画面 */
/* 群轨道分配: 有窗口的 tag 先放内 / 中轨 (当前 tag 在内轨上靠镜头的一侧), 空 tag 放外轨; 同轨按平近点角均分 */
static void
galaxybuildlanes(int cur, int npop)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyCore *g;
    GalaxyMat world;
    GalaxyVec v;
    static const double offset[GALAXYLANES] = { 0, 2.2, 4.1 };
    int order[32], count[GALAXYLANES] = {0}, slot[32], n = 0, nmain = 0, lanes, i, j, k, best = 0;
    double score, bestscore = 1e18, d;

    /* 先排主星系 (当前 tag 第一), 再排空 tag */
    if (cur >= 0)
        order[n++] = cur;
    for (i = 0; i < r->ntags && n < 32; i++)
        if (i != cur && (r->galaxies[i].nstars || !npop))
            order[n++] = i;
    nmain = n;
    for (i = 0; i < r->ntags && n < 32; i++)
        if (i != cur && !(r->galaxies[i].nstars || !npop))
            order[n++] = i;
    /* 有窗口的星系轮流放进三条轨道 (铺满整个盘面), 空 tag 交替补在外轨和中轨 */
    lanes = MAX(1, MIN(GALAXYLANES, nmain));
    r->npop = 0;
    r->lanemask = 0;
    for (j = 0; j < n; j++) {
        g = &r->galaxies[order[j]];
        g->lane = j < nmain ? j % lanes : GALAXYLANES - 1 - (j - nmain) % 2;
        g->rank = j;
        slot[j] = count[g->lane]++;
        r->lanemask |= 1 << g->lane;
        if (g->nstars)
            r->popord[r->npop++] = order[j];
    }
    for (j = 0; j < n; j++) {
        g = &r->galaxies[order[j]];
        g->orbitphase = 2 * GALAXYPI * slot[j] / count[g->lane] + offset[g->lane];
    }
    if (cur < 0 || n < 1)
        return;
    /* 转动内轨的整体相位, 让当前 tag 在进入驻留时位于靠镜头的一侧 */
    world = galaxyworldat(GALAXYHOLD, GALAXYIEND + .5);
    g = &r->galaxies[cur];
    for (k = 0; k < 72; k++) {
        d = 2 * GALAXYPI * k / 72;
        v = galaxyapply(world, galaxylanepoint(g->lane, galaxykepler(galaxyanomaly(g, GALAXYIEND + .5, 0) + d,
                        galaxylanes[g->lane].e)));
        score = v.z + .5 * fabs(v.x);
        if (score < bestscore) {
            bestscore = score;
            best = k;
        }
    }
    for (j = 0; j < n; j++)
        if (r->galaxies[order[j]].lane == g->lane)
            r->galaxies[order[j]].orbitphase += 2 * GALAXYPI * best / 72;
}

static void
galaxybuildcores(void)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyCore *g;
    double F = r->cam.focal, spread, yy, rr, phi, rad;
    int i, k, n, idx, perm, npop = 0, pi = 0, ei = 0, main, cur = -1;

    for (i = 0; i < r->ntags; i++)
        npop += r->galaxies[i].nstars > 0;
    /* 当前 tag 的星系 (用户眼前的桌面) 占第一个位置 */
    for (i = 0; i < r->ntags && cur < 0; i++)
        if (r->savedtags & 1u << i && (r->galaxies[i].nstars || !npop))
            cur = i;
    if (cur >= 0)
        pi = 1;
    rad = .085 * r->vw * r->orbitscale * MAX(.75, MIN(1.5, 1.9 / sqrt(npop + .5)));
    for (i = 0; i < r->ntags; i++) {
        g = &r->galaxies[i];
        g->tag = i;
        main = g->nstars || !npop;
        idx = i == cur ? 0 : main ? pi++ : ei++;
        n = main ? (npop ? npop : r->ntags) : r->ntags - npop;
        if (main && n == 1) {
            g->home = galaxyv(0, 0, 0);
        } else if (main) {
            spread = MIN(1, .55 + n / 8.0);
            perm = ((n % 7 ? idx * 7 : idx) + n / 2) % n;   /* 第一个位置在垂直方向居中 */
            yy = 1 - 2 * (perm + .5) / n;
            rr = sqrt(MAX(0, 1 - yy * yy));
            phi = idx * 2.39996323 - GALAXYPI / 2 + .25;
            /* xz 近似圆形 (半径约 .42F): 绕 y 旋转时不会转出屏幕, 星系群约占屏宽 2/3;
             * 第一个位置 (当前 tag) 在靠近镜头的一侧 */
            g->home = galaxyv(cos(phi) * rr * .35 * r->vw * spread + (galaxyhash(i * 3 + 1) - .5) * .03 * r->vw,
                    yy * .25 * r->vh * spread,
                    sin(phi) * rr * .42 * F * spread + (galaxyhash(i * 3 + 2) - .5) * .08 * F);
        } else {
            phi = idx * 2.39996323 + 1.1;
            g->home = galaxyv(cos(phi) * .62 * r->vw, (galaxyhash(i * 3 + 1) - .5) * .6 * r->vh,
                    F * (.85 + .35 * galaxyhash(i * 3 + 2)) + sin(phi) * .3 * F);
        }
        g->rx = galaxyinclinations[i % LENGTH(galaxyinclinations)] * GALAXYPI / 180 * (1 + .12 * (galaxyhash(i + 40) - .5));
        g->ry = fmod(i * .9 + .3, 2 * GALAXYPI) - GALAXYPI;
        g->rz = .25 * sin(i * 1.3);
        g->phase = i * 1.7;
        g->speed = (.7 + .1 * (i % 3)) * (i % 2 ? -1 : 1);
        g->precess = .05 * (i % 2 ? 1 : -1);
        g->size = 34 * (1 + .08 * MIN(g->nstars, 6)) * (g->nstars ? 1 : .6);
        g->nrings = !g->nstars ? 0 : g->nstars <= 4 ? 1 : g->nstars <= 10 ? 2 : 3;
        g->radius = rad * (.92 + .08 * (i % 3));
        /* 同一星系的几条轨道环互相倾斜 (原子模型式), 每条环有自己的平面 */
        for (k = 0; k < GALAXYRINGS; k++) {
            g->ring[k] = galaxymul(galaxyrotz(k * GALAXYPI / 3 + .4 * galaxyhash(i * 7 + k)),
                    galaxyrotx(galaxyringtilt[k] * GALAXYPI / 180));
            g->ringr[k] = g->radius * (1 + .45 * k);
        }
        g->hover = 0;
    }
    galaxybuildlanes(cur, npop);
}

static void
galaxybuildorbits(void)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyStar *s;
    GalaxyCore *g;
    int i, slot[32] = {0}, idx, inring, perring;
    double F = r->cam.focal;

    for (i = 0; i < r->nstars; i++) {
        s = &r->stars[i];
        g = &r->galaxies[s->galaxy];
        idx = slot[s->galaxy]++;
        s->ring = idx % g->nrings;
        perring = (g->nstars - s->ring + g->nrings - 1) / g->nrings;
        inring = idx / g->nrings;
        s->radius = g->ringr[s->ring];
        s->angle = 2 * GALAXYPI * inring / MAX(1, perring) + s->ring * .5 + g->phase;
        /* 中间那条环反向运行, 相邻轨道互相穿插 */
        s->speed = g->speed / (1 + .3 * s->ring) * (s->ring == 1 ? -1 : 1);
        s->rock = 2 * GALAXYPI * galaxyhash(i + 100);
        s->delay = .12 * galaxyhash(i + 200);
        s->detach = s->current
            ? galaxyadd(galaxyscale(s->home, .88), galaxyv(0, 0, F * (.38 + .12 * galaxyhash(i + 300))))
            : galaxyadd(galaxyscale(s->home, .85), galaxyv(0, 0, F * (1.3 + .3 * galaxyhash(i + 400))));
        s->drx = .12 * (galaxyhash(i + 500) * 2 - 1);
        s->dry = .25 * (galaxyhash(i + 600) * 2 - 1);
        s->drz = .04 * (galaxyhash(i + 700) * 2 - 1);
        s->pos = s->home;
    }
}

/* 三层空间: 远景星空 (整个球壳) / 盘面尘带 (与群轨道同一平面, 随星系群转动) / 前景浮尘 (镜头与星系群之间) */
static void
galaxybuilddust(void)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyDust *d;
    double F = r->cam.focal, u, v, rad, th, ph;
    int i;

    for (i = 0; i < r->ndust; i++) {
        d = &r->dust[i];
        u = galaxyhash(i * 5 + 1000);
        v = galaxyhash(i * 5 + 1001);
        th = 2 * GALAXYPI * galaxyhash(i * 5 + 1002);
        ph = acos(2 * v - 1);
        d->disk = 0;
        d->tw = galaxyhash(i * 5 + 1005);
        d->boost = 0;
        if (u < .45) {          /* 远景星空 */
            rad = F * (3.5 + 2.5 * galaxyhash(i * 5 + 1003));
            d->pos = galaxyv(rad * sin(ph) * cos(th), rad * cos(ph) * .7, rad * sin(ph) * sin(th));
            d->size = 8 + 6 * galaxyhash(i * 5 + 1004);
            d->light = .35 + .25 * galaxyhash(i * 5 + 1004);
        } else if (u < .8) {    /* 盘面尘带: 半径 .12–.7 屏宽, 薄薄一层 */
            rad = r->vw * (.12 + .58 * sqrt(galaxyhash(i * 5 + 1003)));
            d->pos = galaxyv(rad * cos(th), (v - .5) * .035 * r->vw, rad * sin(th));
            d->disk = 1;
            d->size = 4.5 + 4 * galaxyhash(i * 5 + 1004);
            d->light = .45 + .35 * galaxyhash(i * 5 + 1004);
        } else {                /* 前景浮尘: 视差最大 */
            d->pos = galaxyv((galaxyhash(i * 5 + 1003) - .5) * 2.4 * F, (v - .5) * 1.4 * F,
                    -F * (.35 + .45 * galaxyhash(i * 5 + 1002)));
            d->size = 1.8 + 1.2 * galaxyhash(i * 5 + 1004);
            d->light = .55 + .25 * galaxyhash(i * 5 + 1004);
        }
    }
}

/* 动态壁纸 (linux-wallpaperengine / xwinwrap 等) 是最底层的全屏非托管窗口 */
static Window
galaxyfindwallpaper(void)
{
    Window d1, d2, *wins = NULL, found = None;
    XWindowAttributes wa;
    unsigned int i, n;

    if (!XQueryTree(dpy, root, &d1, &d2, &wins, &n))
        return None;
    for (i = 0; i < n; i++) {
        if (wintoclient(wins[i]))
            break;
        if (XGetWindowAttributes(dpy, wins[i], &wa) && wa.map_state == IsViewable && wa.class == InputOutput
                && wa.x <= 0 && wa.y <= 0 && wa.x + wa.width >= galaxyscene.w && wa.y + wa.height >= galaxyscene.h) {
            found = wins[i];
            break;
        }
    }
    if (wins)
        XFree(wins);
    return found;
}

static void
galaxycapturebackground(void)
{
    GalaxyScene *r = &galaxyscene;
    XRenderPictureAttributes pa = {.subwindow_mode = IncludeInferiors};
    XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy, DefaultVisual(dpy, screen));
    XRenderColor space = {2800, 3400, 5000, 65535};
    XWindowAttributes wa;
    Atom type;
    int format;
    unsigned long items, after;
    unsigned char *data = NULL;
    Pixmap source = None;
    Picture p;

    /* 当前合成后的整个桌面: 第一帧与真实桌面完全一致 */
    if ((p = XRenderCreatePicture(dpy, root, fmt, CPSubwindowMode, &pa))) {
        XRenderComposite(dpy, PictOpSrc, p, None, r->desktop, 0, 0, 0, 0, 0, 0, r->w, r->h);
        XRenderFreePicture(dpy, p);
    }
    XRenderFillRectangle(dpy, PictOpSrc, r->wallpaper, &space, 0, 0, r->w, r->h);
    if ((r->wallwin = galaxyfindwallpaper()) && XGetWindowAttributes(dpy, r->wallwin, &wa)
            && (fmt = XRenderFindVisualFormat(dpy, wa.visual))
            && (r->live = XRenderCreatePicture(dpy, r->wallwin, fmt, CPSubwindowMode, &pa))) {
        XRenderComposite(dpy, PictOpSrc, r->live, None, r->wallpaper, -wa.x, -wa.y, 0, 0, 0, 0, r->w, r->h);
        return;
    }
    r->wallwin = None;
    if (XGetWindowProperty(dpy, root, XInternAtom(dpy, "_XROOTPMAP_ID", False), 0, 1, False, XA_PIXMAP,
            &type, &format, &items, &after, &data) == Success && data && items)
        source = *(Pixmap *)data;
    if (data)
        XFree(data);
    if (source && (p = XRenderCreatePicture(dpy, source, XRenderFindVisualFormat(dpy, DefaultVisual(dpy, screen)), 0, NULL))) {
        XRenderComposite(dpy, PictOpSrc, p, None, r->wallpaper, 0, 0, 0, 0, 0, 0, r->w, r->h);
        XRenderFreePicture(dpy, p);
    }
}

static void
galaxylogstart(void)
{
    GalaxyScene *r = &galaxyscene;
    char path[512];
    const char *home = getenv("HOME");
    int i;

    /* 主显示器 :0 用 dwm-galaxy.log, 其他显示器 (嵌套 / 测试会话) 各用一份, 互不覆盖 */
    if (!strcmp(DisplayString(dpy), ":0"))
        snprintf(path, sizeof path, "%s/.cache/dwm-galaxy.log", home ? home : "/tmp");
    else
        snprintf(path, sizeof path, "%s/.cache/dwm-galaxy%s.log", home ? home : "/tmp", DisplayString(dpy));
    if (!(r->log = fopen(path, "w")))
        return;
    fprintf(r->log, "galaxy start: screen %dx%d tags %d windows %d dust %d trail %d focal %.0f timescale %.2f dpms %d wallpaper 0x%lx%s quiet %d fxgap %.2f\n",
            r->w, r->h, r->ntags, r->nstars, r->ndust, r->ntrail, r->cam.focal, r->tscale, r->dpms,
            r->wallwin, r->live ? " (live)" : "", r->quiet, r->fxgap);
    for (i = 0; i < r->ntags; i++)
        if (r->galaxies[i].nstars)
            fprintf(r->log, "galaxy %d: windows %d rings %d radius %.0f home %.0f %.0f %.0f\n", i + 1,
                    r->galaxies[i].nstars, r->galaxies[i].nrings, r->galaxies[i].radius,
                    r->galaxies[i].home.x, r->galaxies[i].home.y, r->galaxies[i].home.z);
    for (i = 0; i < r->nstars; i++)
        fprintf(r->log, "star %2d win 0x%08lx client %p tag %d ring %d %s%s%s snapshot %dx%d mip%d \"%s\"\n",
                i, r->stars[i].win, (void *)r->stars[i].c, r->stars[i].galaxy + 1, r->stars[i].ring,
                r->stars[i].current ? "current " : "", r->stars[i].hidden ? "hidden " : "",
                r->stars[i].focused ? "focused " : "", r->stars[i].snap ? r->stars[i].mipw[r->stars[i].base] : 0,
                r->stars[i].snap ? r->stars[i].miph[r->stars[i].base] : 0, r->stars[i].base, r->stars[i].title);
    fflush(r->log);
}

static int
galaxygapcmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* 一个阶段 (开场 / 驻留每 10 秒 / 坍缩 / 回程) 的帧率统计, 打印后清零 */
static void
galaxylogseg(const char *how)
{
    GalaxyScene *r = &galaxyscene;
    double now = galaxynow(), span = now - r->segstart, low = 0;

    if (r->log && r->frames >= 2) {
        if (r->ngaps) {
            qsort(r->gaps, r->ngaps, sizeof *r->gaps, galaxygapcmp);
            low = r->gaps[(size_t)(r->ngaps * .99)];
        }
        fprintf(r->log, "galaxy %s: at %.2fs frames %lu time %.3fs avg %.1f fps min %.1f fps 1%%low %.1f fps render avg %.2fms max %.2fms update %.2fms background %.2fms trails %.2fms items %.2fms present %.2fms XSync %.2fms bands %.2f+%.2fms (%.0f tiles in %.1f flushes) quality %d%s xerrors %lu\n",
                how, now, r->frames, span, r->frames / MAX(span, 1e-3),
                r->ngaps ? 1 / r->gaps[r->ngaps - 1] : 0, low > 0 ? 1 / low : 0,
                r->rendersum / r->frames * 1000, r->rendermax * 1000,
                r->phasecost[0] / r->frames * 1000, r->phasecost[1] / r->frames * 1000,
                r->phasecost[2] / r->frames * 1000, r->phasecost[3] / r->frames * 1000,
                r->phasecost[4] / r->frames * 1000, r->phasecost[5] / r->frames * 1000,
                r->bandraster / r->frames * 1000, r->bandflush / r->frames * 1000,
                (double)r->bandtiles / r->frames, (double)r->bandflushes / r->frames, r->quality,
                r->mode == GalaxyOrbit && now - r->lastinput > GALAXYIDLE ? " (idle)" : "", r->errors);
        fflush(r->log);
    }
    r->frames = r->ngaps = 0;
    r->rendersum = r->rendermax = 0;
    memset(r->phasecost, 0, sizeof r->phasecost);
    r->bandraster = r->bandflush = 0;
    r->bandtiles = r->bandflushes = 0;
    r->segstart = now;
}

static void
galaxylogfirst(void)
{
    GalaxyScene *r = &galaxyscene;
    int i;

    if (!r->log || r->nfirst < 2 || r->firstlogged)
        return;
    fprintf(r->log, "galaxy first frames (start ms / render ms):");
    for (i = 0; i < r->nfirst; i++)
        fprintf(r->log, " %.0f/%.1f", r->firstgap[i] * 1000, r->firstcost[i] * 1000);
    fprintf(r->log, "\n");
    r->firstlogged = 1;
}
