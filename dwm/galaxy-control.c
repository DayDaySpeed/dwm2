/* Super+Z 星系: 状态机. 开场 / 驻留 / 坍缩 / 回程 / 静止壁纸, 事件处理, 每帧 tick, 入口 galaxy().
 * 由 galaxy.c 按顺序 include. */

/* ---------- 状态机 ---------- */

static int
galaxyactive(void)
{
    return galaxyscene.mode != GalaxyOff;
}

static void
galaxysetcursor(int on)
{
    GalaxyScene *r = &galaxyscene;

    if (on == r->handon || !r->overlay)
        return;
    r->handon = on;
    if (on < 0 && r->blankcursor)
        XDefineCursor(dpy, r->overlay, r->blankcursor);
    else if (on > 0 && r->hand)
        XDefineCursor(dpy, r->overlay, r->hand);
    else
        XUndefineCursor(dpy, r->overlay);
}

static void
galaxymousewake(void)
{
    GalaxyScene *r = &galaxyscene;

    r->mouseawake = 1;
    r->mousevalid = 0;
    r->lastinput = r->lastmouse = r->lastpointer = galaxynow();
    galaxysetcursor(0);
    if (r->log) {
        fprintf(r->log, "galaxy mouse: wake\n");
        fflush(r->log);
    }
}

static void
galaxymousesleep(const char *reason)
{
    GalaxyScene *r = &galaxyscene;

    r->mouseawake = r->mousevalid = 0;
    r->dragging = 0;
    r->dragvyaw = r->dragvpitch = 0;
    r->dragtyaw = r->dragtpitch = 0;
    r->hover = r->hovercore = -1;
    r->tyaw = r->tpitch = 0;
    r->tzoom = 1;
    r->lastpointer = -1e9;
    galaxysetcursor(-1);
    if (r->log) {
        fprintf(r->log, "galaxy mouse: sleep (%s)\n", reason);
        fflush(r->log);
    }
}

/* 不再独占键盘鼠标 (锁屏程序仍能抓取), 键盘焦点交给遮罩, 鼠标事件直接发给最上层的遮罩 */
static void
galaxyrelease(void)
{
    GalaxyScene *r = &galaxyscene;

    if (r->grabptr)
        XUngrabPointer(dpy, CurrentTime);
    if (r->grabkbd)
        XUngrabKeyboard(dpy, CurrentTime);
    r->grabptr = r->grabkbd = 0;
    XSetInputFocus(dpy, r->overlay, RevertToPointerRoot, CurrentTime);
}

static void
galaxyend(int restore)
{
    GalaxyScene *r = &galaxyscene;
    Monitor *m, *tmon = r->tmon;
    unsigned int ttags = r->ttags;
    Window twin = r->twin;
    int tshow = r->tshow;
    Client *c;
    XEvent ev;

    if (!r->mode)
        return;
    galaxylogfirst();
    if (r->mode != GalaxyRest)
        galaxylogseg(galaxymodename[r->mode]);
    if (r->log)
        fprintf(r->log, "galaxy end: from %s restore %d tags 0x%x win 0x%lx xerrors %lu\n",
                galaxymodename[r->mode], restore, ttags, twin, r->errors);
    if (r->grabptr)
        XUngrabPointer(dpy, CurrentTime);
    if (r->grabkbd)
        XUngrabKeyboard(dpy, CurrentTime);
    XSetErrorHandler(galaxyxerror);
    galaxyfree();
    XSync(dpy, False);
    XSetErrorHandler(xerror);
    /* 遮罩消失时鼠标下的窗口会收到 EnterNotify, 丢掉它们, 焦点不跟着鼠标变 */
    while (XCheckMaskEvent(dpy, EnterWindowMask, &ev));
    if (!restore)
        return;
    c = twin ? wintoclient(twin) : NULL;
    if (c)
        tmon = c->mon;
    for (m = mons; m && m != tmon; m = m->next);
    if (m) {
        selmon = m;
        /* 星系自己已经把窗口飞回了原位, 让 picom 别再给切 tag / 恢复隐藏窗口加滑动或放大动画, 直接切换 */
        noanim(400);
        if (c && tshow && HIDDEN(c))
            show(c);  /* 点击了隐藏窗口的星体: 恢复它 */
        if (ttags && (m->tagset[m->seltags] & TAGMASK) != ttags)
            view(&(Arg){.ui = ttags});
    }
    c = twin ? wintoclient(twin) : NULL;
    focus(c && ISVISIBLE(c) && !HIDDEN(c) ? c : NULL);
}

static void
galaxycleanup(void)
{
    galaxyend(0);
    galaxyspacefree();
}

static void
galaxycancel(void)
{
    galaxyend(1);
}

/* 壁纸进 picom 看到的前缓冲: 画进已绑定为 GL 底层纹理的 r->back (与正常帧画卡片同一张 pixmap),
 * 清掉前景和光层, 泛光为 0, 再 galaxyglpresent. XRender 到遮罩窗口再 SwapBuffers 换上来的仍是上一张 GL 帧. */
static void
galaxypresentwall(void)
{
    GalaxyScene *r = &galaxyscene;
    XRenderColor clear = {0, 0, 0, 0};

    if (!galaxygl.ready || !galaxygl.win || !galaxygl.gback || !r->back || !r->wallpaper)
        return;
    if ((glXGetCurrentContext() != galaxygl.ctx || glXGetCurrentDrawable() != galaxygl.win)
            && !glXMakeCurrent(dpy, galaxygl.win, galaxygl.ctx))
        return;
    if (r->live)
        XRenderComposite(dpy, PictOpSrc, r->live, None, r->wallpaper, 0, 0, 0, 0, 0, 0, r->w, r->h);
    XRenderComposite(dpy, PictOpSrc, r->wallpaper, None, r->back, 0, 0, 0, 0, 0, 0, r->w, r->h);
    if (r->front)
        XRenderFillRectangle(dpy, PictOpSrc, r->front, &clear, 0, 0, r->w, r->h);
    galaxyglframe();
    galaxyglpresent(0, 0, 1);
}

/* 坍缩结束 (或坍缩中再按 Esc): 遮罩只显示壁纸, 释放场景资源, 等 Super+Z / 任意键恢复.
 * gback / backpix 留到真正退出: 壁纸走和正常帧一样的 GL 合成, 才能进前缓冲. */
static void
galaxyfinish(void)
{
    GalaxyScene *r = &galaxyscene;
    int i;

    galaxylogseg(galaxymodename[r->mode]);
    galaxyfreescene();
    for (i = 0; i < GALAXYALPHAS; i++) {
        if (r->white[i])
            XRenderFreePicture(dpy, r->white[i]);
        r->white[i] = 0;
    }
    /* 窗口背景可能是 desktoppix; 先拆掉引用再释放. 不释放 backpix, GLX pixmap 还绑着它. */
    XSetWindowBackgroundPixmap(dpy, r->overlay, None);
    if (r->desktop) XRenderFreePicture(dpy, r->desktop);
    if (r->desktoppix) XFreePixmap(dpy, r->desktoppix);
    if (r->bg) XRenderFreePicture(dpy, r->bg);
    if (r->bgpix) XFreePixmap(dpy, r->bgpix);
    r->desktop = r->bg = 0;
    r->desktoppix = r->bgpix = 0;
    r->mode = GalaxyRest;
    galaxysetcursor(0);
    galaxypresentwall();
    galaxyrelease();
    if (r->log)
        fprintf(r->log, "galaxy rest: wallpaper only, xerrors %lu\n", r->errors);
    XSync(dpy, False);
}

/* 开场播完: 停在星系轨道态, 不限时 */
static void
galaxyorbitstart(void)
{
    GalaxyScene *r = &galaxyscene;

    galaxylogfirst();
    galaxylogseg(r->warping ? "intro (warped)" : "intro");
    if (r->log && r->divenum)
        fprintf(r->log, "galaxy dive: frames %d render avg %.2fms max %.2fms\n",
                r->divenum, 1000 * r->divesum / r->divenum, 1000 * r->divemax);
    r->mode = GalaxyOrbit;
    r->warping = 0;
    r->lastinput = r->lastdpms = galaxynow();
    if (r->fakehour > 0)
        r->fakehour += r->lastinput;   /* 测试用的假整点: 从驻留开始计时 */
    if (!r->saver)
        galaxymousesleep("orbit start");
    galaxyrelease();
}

static void
galaxycollapsestart(void)
{
    GalaxyScene *r = &galaxyscene;

    galaxylogseg(galaxymodename[r->mode]);
    r->dragstar = r->dragarm = -1;
    r->cstage = r->stage;
    r->cworld = r->world;
    r->exitspin = 0;
    r->mode = GalaxyCollapse;
    r->cstart = galaxynow();
    r->hover = r->hovercore = -1;
    galaxysetcursor(0);
}

/* 开场中鼠标已唤醒后再次点击: 场景时钟加速, GALAXYWARP 秒内走到驻留态 (一切都是时间的纯函数, 不会跳帧) */
static void
galaxywarp(void)
{
    GalaxyScene *r = &galaxyscene;

    if (r->mode != GalaxyIntro || r->warping)
        return;
    r->warping = 1;
    r->wstart = galaxynow();
    r->wscene = r->scene;
}

/* 回程: star >= 0 时回到该窗口所在 tag 并聚焦它, tag >= 0 时只切到该 tag, 都为 -1 时回到开始前的状态 */
/* 切换后的状态栏 / 托盘: 直接从窗口取 (合成器重定向的窗口在遮罩下面也有内容), 回程最后淡入 */
static void
galaxycapturebars(void)
{
    GalaxyScene *r = &galaxyscene;
    XRenderPictureAttributes pa = {.subwindow_mode = IncludeInferiors};
    XWindowAttributes wa;
    XRenderPictFormat *fmt;
    Window wins[GALAXYBARS];
    Monitor *m;
    int i, n = 0;

    for (i = 0; i < r->nlandbar; i++)
        XRenderFreePicture(dpy, r->landbar[i]);
    r->nlandbar = 0;
    for (m = mons; m && n < GALAXYBARS - 1; m = m->next)
        if (m->showbar && m->barwin)
            wins[n++] = m->barwin;
    if (systray && systray->win && n < GALAXYBARS)
        wins[n++] = systray->win;
    for (i = 0; i < n; i++)
        if (XGetWindowAttributes(dpy, wins[i], &wa) && wa.map_state == IsViewable
                && (fmt = XRenderFindVisualFormat(dpy, wa.visual))
                && (r->landbar[r->nlandbar] = XRenderCreatePicture(dpy, wins[i], fmt, CPSubwindowMode, &pa))) {
            r->landbarx[r->nlandbar] = wa.x + wa.border_width;
            r->landbary[r->nlandbar] = wa.y + wa.border_width;
            r->landbarw[r->nlandbar] = wa.width;
            r->landbarh[r->nlandbar++] = wa.height;
        }
}

/* 进入窗口 / tag: 在遮罩下面先把 dwm 切到目标状态 (恢复隐藏窗口、切 tag、排好位置), 读出每个窗口真实的落点.
 * 抓住服务器做: 切换中途 (例如浮动窗口被 raise 到遮罩之上) 不会被合成器画出来. pick 为空时进入 tag */
static void
galaxylandprepare(Client *pick, int tag)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyStar *s;
    Monitor *m = pick ? pick->mon : r->savedmon;
    unsigned int want = pick ? pick->tags & TAGMASK : tag >= 0 ? 1u << tag : 0;
    Client *c;
    int i;

    noanim((long)(1000 * GALAXYLAND) + 600);
    XGrabServer(dpy);
    selmon = m;
    if (pick && HIDDEN(pick))
        show(pick);
    if (want && !(pick && ISVISIBLE(pick)) && (m->tagset[m->seltags] & TAGMASK) != want)
        view(&(Arg){.ui = want});
    XRaiseWindow(dpy, r->overlay);
    XSync(dpy, False);
    XUngrabServer(dpy);
    r->tmon = m;
    r->ttags = m->tagset[m->seltags] & TAGMASK;
    r->twin = pick ? pick->win : None;
    r->tshow = 0;
    for (i = 0; i < r->nstars; i++) {
        s = &r->stars[i];
        c = s->valid ? wintoclient(s->win) : NULL;
        s->land = c && ISVISIBLE(c) && !HIDDEN(c) && !s->died && s->snap;
        if (s->land) {
            s->lpos = galaxyv(c->x + c->bw + c->w * .5 - r->vx - r->vw * .5, c->y + c->bw + c->h * .5 - r->vy - r->vh * .5, 0);
            s->lw = MAX(1, c->w);
            s->lh = MAX(1, c->h);
        }
    }
    galaxycapturebars();
    if (r->log)
        fprintf(r->log, "galaxy land: tags 0x%x win 0x%lx bars %d\n", r->ttags, r->twin, r->nlandbar), fflush(r->log);
}

static void
galaxyreturnstart(int tag, int star)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyStar *s;
    GalaxyCore *g;
    int i;

    if (r->mode == GalaxyOff || r->mode == GalaxyRest || r->mode == GalaxyReturn)
        return;
    r->dragstar = r->dragarm = -1;
    galaxylogseg(galaxymodename[r->mode]);
    r->rkind = star >= 0 || tag >= 0 ? GalaxyLand : GalaxyFlyHome;
    r->rstar = star;
    r->rcore = tag;
    if (r->rkind == GalaxyLand)
        galaxylandprepare(star >= 0 && r->stars[star].valid ? wintoclient(r->stars[star].win) : NULL, star >= 0 ? -1 : tag);
    /* Super+Z: 结束后可见的窗口飞回原位置. 点击不使用这条轨迹. */
    for (i = 0; i < r->nstars; i++) {
        s = &r->stars[i];
        if (s->mon != r->tmon)
            s->back = s->shown;
        else
            s->back = (!s->hidden || i == star) && (s->global || (s->tags & r->ttags));
        if (s->died)
            s->back = 0;        /* 已经化作流星的窗口不再飞回 */
        s->rpos = s->pos;
        s->rorient = s->orient;
        s->rsize = s->size;
        s->rvis = s->vis;
        s->rtint = s->tint;
        s->rglow = s->glow;
        s->rbright = s->p.ok ? s->brightness : 1;
        s->rkw = s->kw;
        s->rkh = s->kh;
        if (s->died)
            s->land = 0;
    }
    for (i = 0; i < r->ntags; i++) {
        g = &r->galaxies[i];
        g->nova = g->bridge = 0;    /* 驻留特效不带进回程 */
        g->rpos = g->pos;
        g->ralpha = g->alpha;
    }
    r->fulldesk = r->tmon == r->savedmon && r->ttags == r->savedtags && (star < 0 || !r->stars[star].hidden);
    if (r->moved) {     /* 拖动换过 tag: 开始时截的桌面已经不对了, 最后淡入实时的状态栏 */
        r->fulldesk = 0;
        if (r->rkind == GalaxyFlyHome)
            galaxycapturebars();
    }
    if (r->rkind == GalaxyLand)     /* 开始时截的桌面里, 焦点窗口和现在可能不同: 只在没换 tag、选中的就是原焦点时交叉淡入 */
        r->fulldesk = r->fulldesk && (star < 0 ? r->savedwin == None || tag < 0 : r->stars[star].win == r->savedwin);
    if (r->tmon && r->tmon->showbar) {
        r->barx = r->tmon->mx;
        r->bary = r->tmon->by;
        r->barw = r->tmon->mw;
        r->barh = bh;
    }
    r->rcdist = r->cam.dist;
    r->rcx = r->cam.rx * 180 / GALAXYPI;
    r->rcy = r->cam.ry * 180 / GALAXYPI;
    r->rcz = r->cam.rz * 180 / GALAXYPI;
    r->rcampos = r->cam.pos;
    r->rctarget = r->cam.target;
    r->rstreak = r->streakalpha;
    r->rsun = r->sunalpha;
    r->rring = r->ringalpha;
    r->rcluster = r->clusteralpha;
    r->rdust = r->dustfade;
    r->rbright = r->bright;
    r->rspace = r->space;
    r->rvign = r->vign;
    r->rdesk = r->desk;
    r->mode = GalaxyReturn;
    r->rstart = galaxynow();
    r->hover = r->hovercore = -1;
    galaxysetcursor(0);
    if (r->log)
        fprintf(r->log, "galaxy return: to tags 0x%x win 0x%lx fulldesk %d\n", r->ttags, r->twin, r->fulldesk);
}

/* 鼠标下最前面的窗口星或星系核心 (按上一帧画出的位置, 从近到远找) */
static void
galaxypick(double x, double y, int *star, int *core)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyStar *s;
    GalaxyCore *g;
    int i;

    *star = *core = -1;
    for (i = r->nitems - 1; i >= 0; i--) {
        if (r->items[i].kind == GalaxyStarItem) {
            s = &r->stars[r->items[i].index];
            if (s->hit && x >= s->bx0 && x <= s->bx1 && y >= s->by0 && y <= s->by1) {
                *star = r->items[i].index;
                return;
            }
        } else if (r->items[i].kind == GalaxyCoreItem) {
            g = &r->galaxies[r->items[i].index];
            if (g->hit && hypot(x - g->hx, y - g->hy) <= g->hr) {
                *core = r->items[i].index;
                return;
            }
        }
    }
}

/* 停在壁纸 / 驻留时: 动态壁纸继续播放, 新窗口抢走的键盘焦点还给遮罩 */
static void
galaxyrest(void)
{
    GalaxyScene *r = &galaxyscene;
    Window focused;
    int revert;

    if (r->mode == GalaxyRest && r->live) {
        galaxypresentwall();
        XSync(dpy, False);
    }
    XGetInputFocus(dpy, &focused, &revert);
    if (focused != r->overlay)
        XSetInputFocus(dpy, r->overlay, RevertToPointerRoot, CurrentTime);
}

/* 安静模式跟随省电模式 (bin/powersave.sh 打开时会创建 $XDG_CACHE_HOME/powersave/on, 拔电时自动打开) */
static int
galaxyquiet(void)
{
    char path[512];
    const char *cache = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");

    if (cache && *cache)
        snprintf(path, sizeof path, "%s/powersave/on", cache);
    else
        snprintf(path, sizeof path, "%s/.cache/powersave/on", home ? home : "");
    return access(path, F_OK) == 0;
}

/* 遮罩视口中心所在显示器的刷新率 (XRandR 当前模式的 dotClock / (hTotal * vTotal)); 查不到时用 GALAXYORBITFPS */
static double
galaxyrefresh(void)
{
    GalaxyScene *r = &galaxyscene;
    XRRScreenResources *res;
    XRRCrtcInfo *ci;
    double hz = 0;
    int i, j, cx = r->vx + r->vw / 2, cy = r->vy + r->vh / 2;

    if (!(res = XRRGetScreenResourcesCurrent(dpy, root)))
        return GALAXYORBITFPS;
    for (i = 0; i < res->ncrtc && hz <= 0; i++) {
        if (!(ci = XRRGetCrtcInfo(dpy, res, res->crtcs[i])))
            continue;
        if (ci->mode && cx >= ci->x && cx < ci->x + (int)ci->width && cy >= ci->y && cy < ci->y + (int)ci->height)
            for (j = 0; j < res->nmode; j++)
                if (res->modes[j].id == ci->mode && res->modes[j].hTotal && res->modes[j].vTotal)
                    hz = (double)res->modes[j].dotClock / ((double)res->modes[j].hTotal * res->modes[j].vTotal);
        XRRFreeCrtcInfo(ci);
    }
    XRRFreeScreenResources(res);
    return hz > 1 ? MAX(GALAXYORBITFPS, MIN(360, hz)) : GALAXYORBITFPS;
}

static double
galaxyfps(double now)
{
    GalaxyScene *r = &galaxyscene;
    double orbit = r->refresh > 1 ? r->refresh : GALAXYORBITFPS;

    if (r->mode != GalaxyOrbit)
        return GALAXYFPS;
    if (r->quiet || r->saver)
        return MIN(GALAXY_QUIETFPS, orbit);
    return now - r->lastinput > GALAXYIDLE ? GALAXYIDLEFPS : orbit;
}

/* ---------- 键盘导航: 方向键选星, Enter 跳转, 打字按标题过滤, Tab 在匹配项之间切换 ---------- */

static int
galaxykeyactive(void)
{
    GalaxyScene *r = &galaxyscene;

    return r->kqlen > 0 || galaxynow() - r->lastkey < 8;
}

/* 可以被键盘选中的星: 在画面上, 而且匹配过滤词 */
static int
galaxykeyok(int i)
{
    GalaxyStar *s = &galaxyscene.stars[i];

    return s->p.ok && s->alpha > .1 && s->kmatch && s->p.x > 0 && s->p.y > 0
        && s->p.x < galaxyscene.w && s->p.y < galaxyscene.h;
}

/* 标题里是否含有过滤词 (ASCII 不区分大小写) */
static int
galaxykeymatch(const char *title, const char *q)
{
    size_t i, j, n = strlen(q), m = strlen(title);

    for (i = 0; n && i + n <= m; i++) {
        for (j = 0; j < n && tolower((unsigned char)title[i + j]) == tolower((unsigned char)q[j]); j++);
        if (j == n)
            return 1;
    }
    return !n;
}

/* 离屏幕中心最近的可选星 (没有选中时的起点; 优先当前焦点窗口) */
static int
galaxykeystart(void)
{
    GalaxyScene *r = &galaxyscene;
    double best = 1e18, d;
    int i, pick = -1;

    for (i = 0; i < r->nstars; i++) {
        if (!galaxykeyok(i))
            continue;
        if (r->stars[i].focused && !r->kqlen)
            return i;
        d = hypot(r->stars[i].p.x - r->vx - r->vw * .5, r->stars[i].p.y - r->vy - r->vh * .5);
        if (d < best) {
            best = d;
            pick = i;
        }
    }
    return pick;
}

static void
galaxykeyfilter(void)
{
    GalaxyScene *r = &galaxyscene;
    int i;

    r->kq[r->kqlen] = 0;
    for (i = r->kn = 0; i < r->nstars; i++)
        r->kn += r->stars[i].kmatch = galaxykeymatch(r->stars[i].title, r->kq);
    if (r->ksel < 0 || r->ksel >= r->nstars || !galaxykeyok(r->ksel))
        r->ksel = galaxykeystart();
}

static void
galaxykeyclear(void)
{
    GalaxyScene *r = &galaxyscene;

    r->kqlen = 0;
    r->ksel = -1;
    r->lastkey = -1e9;
    galaxykeyfilter();
    r->ksel = -1;
}

/* 方向键: 选中该方向 60° 锥形内最近的星 (距离 + 偏离方向的惩罚) */
static void
galaxykeymove(double dx, double dy)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyStar *a, *b;
    double best = 1e18, along, perp, cost;
    int i, pick = -1;

    if (r->ksel < 0 || !galaxykeyok(r->ksel)) {
        r->ksel = galaxykeystart();
        return;
    }
    a = &r->stars[r->ksel];
    for (i = 0; i < r->nstars; i++) {
        if (i == r->ksel || !galaxykeyok(i))
            continue;
        b = &r->stars[i];
        along = (b->p.x - a->p.x) * dx + (b->p.y - a->p.y) * dy;
        perp = fabs((b->p.x - a->p.x) * dy - (b->p.y - a->p.y) * dx);
        if (along <= 0 || perp > along * 1.73)
            continue;
        cost = along + 2 * perp;
        if (cost < best) {
            best = cost;
            pick = i;
        }
    }
    if (pick >= 0)
        r->ksel = pick;
}

/* Tab / Shift+Tab: 按屏幕位置 (从左到右, 从上到下) 在可选的星之间循环 */
static void
galaxykeycycle(int dir)
{
    GalaxyScene *r = &galaxyscene;
    double key, cur = -1e18, best = dir > 0 ? 1e18 : -1e18, wrap = dir > 0 ? 1e18 : -1e18;
    int i, pick = -1, first = -1;

    if (r->ksel >= 0 && galaxykeyok(r->ksel))
        cur = r->stars[r->ksel].p.x * 4096 + r->stars[r->ksel].p.y;
    for (i = 0; i < r->nstars; i++) {
        if (!galaxykeyok(i) || i == r->ksel)
            continue;
        key = r->stars[i].p.x * 4096 + r->stars[i].p.y;
        if (dir > 0 ? key > cur && key < best : key < cur && key > best) {
            best = key;
            pick = i;
        }
        if (dir > 0 ? key < wrap : key > wrap) {
            wrap = key;
            first = i;
        }
    }
    if (pick < 0)
        pick = first;
    if (pick >= 0)
        r->ksel = pick;
}

/* 驻留态的普通按键 (Esc / Super+Z 之外) */
static void
galaxykey(XEvent *e, KeySym sym)
{
    GalaxyScene *r = &galaxyscene;
    char buf[8];
    KeySym ks;
    int n;

    if (CLEANMASK(e->xkey.state) & (MODKEY | ControlMask | Mod1Mask))
        return;     /* 带 Super / Ctrl / Alt 的组合键不当作输入 */
    r->lastkey = galaxynow();
    switch (sym) {
    case XK_Left:  galaxykeymove(-1, 0); return;
    case XK_Right: galaxykeymove(1, 0); return;
    case XK_Up:    galaxykeymove(0, -1); return;
    case XK_Down:  galaxykeymove(0, 1); return;
    case XK_Tab:   galaxykeycycle(e->xkey.state & ShiftMask ? -1 : 1); return;
    case XK_ISO_Left_Tab: galaxykeycycle(-1); return;
    case XK_Return: case XK_KP_Enter:
        if (r->ksel >= 0 && galaxykeyok(r->ksel))
            galaxyreturnstart(-1, r->ksel);
        return;
    case XK_BackSpace:
        if (r->kqlen) {
            r->kqlen--;
            galaxykeyfilter();
        }
        return;
    }
    n = XLookupString(&e->xkey, buf, sizeof buf, &ks, NULL);
    if (n == 1 && buf[0] >= 0x20 && buf[0] < 0x7f && r->kqlen < (int)sizeof r->kq - 1) {
        r->kq[r->kqlen++] = buf[0];
        r->ksel = -1;       /* 过滤词变了: 选中跳到第一个匹配 */
        galaxykeyfilter();
    }
}

/* 窗口星的基本信息 (位置以视口中心为原点). cur: 没有 tag 的窗口 (全局窗口等) 归到当前 tag */
static void
galaxyinitstar(GalaxyStar *s, Client *c, Monitor *m, unsigned int cur)
{
    GalaxyScene *r = &galaxyscene;
    int tag = c->isglobal || !(c->tags & TAGMASK) ? (int)cur : __builtin_ctz(c->tags & TAGMASK);

    s->win = c->win;
    s->c = c;
    s->mon = m;
    s->tags = c->tags & TAGMASK;
    s->valid = 1;
    snprintf(s->title, sizeof s->title, "%.63s", c->name);
    s->galaxy = MIN(tag, r->ntags - 1);
    s->hidden = HIDDEN(c);
    s->global = c->isglobal;
    s->shown = ISVISIBLE(c) && !s->hidden;
    s->current = s->shown;   /* 任何显示器上正显示的窗口都从原位置起飞 (多显示器时不会在另一块屏上凭空消失) */
    s->focused = c == selmon->sel;
    s->w = MAX(1, c->w);
    s->h = MAX(1, c->h);
    s->kmatch = 1;
    s->kw = s->kh = 1;
    s->home = galaxyv(c->x + c->bw + c->w * .5 - r->vx - r->vw * .5, c->y + c->bw + c->h * .5 - r->vy - r->vh * .5, 0);
}

/* ---------- 天象: 桌面事件 (新窗口诞生新星 / 关窗化作流星 / 通知彗星 / 整点报时) ---------- */

/* 新映射的托管窗口先排队, 0.6s 后 (窗口画好了) 再截图诞生 */
static void
galaxybirthqueue(Window w)
{
    GalaxyScene *r = &galaxyscene;
    int i;

    for (i = 0; i < r->nbirth; i++)
        if (r->birthwin[i] == w)
            return;
    for (i = 0; i < r->nstars; i++)
        if (r->stars[i].win == w && r->stars[i].valid)
            return;
    if (r->nbirth >= GALAXYSPARE)
        return;
    r->birthwin[r->nbirth] = w;
    r->birthat[r->nbirth++] = galaxynow();
}

static void
galaxybirthdrop(int i)
{
    GalaxyScene *r = &galaxyscene;

    memmove(r->birthwin + i, r->birthwin + i + 1, (r->nbirth - i - 1) * sizeof *r->birthwin);
    memmove(r->birthat + i, r->birthat + i + 1, (r->nbirth - i - 1) * sizeof *r->birthat);
    r->nbirth--;
}

/* 在所在 tag 的星系里诞生一颗新星: 放在星最少的那条环上、当前最大的空隙中间 (同一条环上的星角速度相同, 空隙一直保持) */
/* 把星放进它所属星系的轨道: 星最少的那条环上、当前最大的空隙中间 (同一条环上的星角速度相同, 空隙一直保持).
 * self: 这颗星在数组里的下标 (统计时跳过自己). 返回所在的环 */
static int
galaxyplacestar(GalaxyStar *s, int self)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyCore *g = &r->galaxies[s->galaxy];
    GalaxyStar *o;
    double ang[64], best = -1, mid, a, b;
    int i, j, n, k, ring = 0, cnt[GALAXYRINGS] = {0};

    if (!g->nrings)
        g->nrings = 1;      /* 空星系: 轨道环随 fill 从核心长出来 */
    for (i = 0; i < r->nstars; i++)
        if (i != self && r->stars[i].galaxy == s->galaxy && !r->stars[i].died)
            cnt[r->stars[i].ring]++;
    for (k = 1; k < g->nrings; k++)
        if (cnt[k] < cnt[ring])
            ring = k;
    s->ring = ring;
    s->radius = g->ringr[ring];
    s->speed = g->speed / (1 + .3 * ring) * (ring == 1 ? -1 : 1);
    for (i = n = 0; i < r->nstars && n < 64; i++) {
        o = &r->stars[i];
        if (i != self && o->galaxy == s->galaxy && o->ring == ring && !o->died)
            ang[n++] = fmod(fmod(o->angle, 2 * GALAXYPI) + 2 * GALAXYPI, 2 * GALAXYPI);
    }
    for (i = 1; i < n; i++)
        for (j = i; j > 0 && ang[j] < ang[j - 1]; j--) {
            a = ang[j]; ang[j] = ang[j - 1]; ang[j - 1] = a;
        }
    mid = 2 * GALAXYPI * galaxyhash(self * 13 + 7);
    for (i = 0; i < n; i++) {
        a = ang[i];
        b = i + 1 < n ? ang[i + 1] : ang[0] + 2 * GALAXYPI;
        if (b - a > best) {
            best = b - a;
            mid = (a + b) * .5;
        }
    }
    s->angle = mid;
    return ring;
}

/* 在所在 tag 的星系里诞生一颗新星 */
static void
galaxyaddstar(Client *c)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyStar *s;
    double now = galaxynow();
    int ring;

    if (r->nstars >= r->maxstars) {
        if (r->log)
            fprintf(r->log, "galaxy birth: no spare slot for 0x%lx\n", c->win), fflush(r->log);
        return;
    }
    s = &r->stars[r->nstars];
    memset(s, 0, sizeof *s);
    galaxyinitstar(s, c, c->mon, r->savedtags ? (unsigned int)__builtin_ctz(r->savedtags) : 0);
    s->focused = 0;
    ring = galaxyplacestar(s, r->nstars);
    s->rock = 2 * GALAXYPI * galaxyhash(r->nstars + 100);
    s->delay = .12 * galaxyhash(r->nstars + 200);
    s->detach = s->pos = s->home;
    galaxycapture(s, c, 1);
    s->born = now;
    r->galaxies[s->galaxy].nstars++;
    r->nstars++;
    r->hushuntil = now + 3;
    galaxyheatpids();
    if (r->kqlen)
        galaxykeyfilter();
    if (r->log)
        fprintf(r->log, "galaxy birth: tag %d ring %d win 0x%lx %s\n", s->galaxy + 1, ring, s->win, s->snap ? "captured" : "no snapshot"),
            fflush(r->log);
}

/* 窗口的真实位置可能变了 (拖动换 tag 后同 tag 的窗口重新平铺): 回程的落点改成现在的位置 */
static void
galaxyrefreshhomes(void)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyStar *s;
    Client *c;
    int i;

    for (i = 0; i < r->nstars; i++) {
        s = &r->stars[i];
        if (!(c = s->valid ? wintoclient(s->win) : NULL))
            continue;
        s->tags = c->tags & TAGMASK;
        s->shown = ISVISIBLE(c) && !HIDDEN(c);
        s->lw = MAX(1, c->w);  /* 回程按现在的大小落位 (galaxyupdatereturn 里拉伸卡片) */
        s->lh = MAX(1, c->h);
        s->home = galaxyv(c->x + c->bw + c->w * .5 - r->vx - r->vw * .5, c->y + c->bw + c->h * .5 - r->vy - r->vh * .5, 0);
    }
}

/* 拖动卡片松手在另一个核心上: 把窗口移到那个 tag (在遮罩下切好), 星体换到新星系的轨道, 从松手处飞进去 */
static void
galaxymovestar(int i, int tag)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyStar *s = &r->stars[i];
    Client *c = s->valid ? wintoclient(s->win) : NULL;
    double now = galaxynow();

    s->moveat = now;
    s->movefrom = r->dragpos;
    if (!c || c->isglobal || tag < 0 || tag >= r->ntags || tag == s->galaxy || !(1u << tag & TAGMASK)) {
        if (r->log)
            fprintf(r->log, "galaxy drag: star %d back to orbit\n", i), fflush(r->log);
        return;
    }
    noanim(1500);
    XGrabServer(dpy);
    c->tags = 1u << tag;
    if (c == c->mon->sel)
        focus(NULL);
    arrange(c->mon);
    XRaiseWindow(dpy, r->overlay);
    XSync(dpy, False);
    XUngrabServer(dpy);
    r->galaxies[s->galaxy].nstars--;
    s->galaxy = tag;
    galaxyplacestar(s, i);
    r->galaxies[tag].nstars++;
    r->galaxies[tag].flashat = now;
    r->moved = 1;
    r->hushuntil = now + 2.5;
    galaxyrefreshhomes();
    if (r->log)
        fprintf(r->log, "galaxy move: win 0x%lx -> tag %d\n", s->win, tag + 1), fflush(r->log);
}

/* 中键点卡片: 关闭窗口 (同 killclient: 先请求关闭, 不支持的强制断开); 销毁后化作流星 */
static void
galaxyclosestar(int i)
{
    GalaxyScene *r = &galaxyscene;
    Client *c = r->stars[i].valid ? wintoclient(r->stars[i].win) : NULL;

    if (!c)
        return;
    if (r->log)
        fprintf(r->log, "galaxy close: win 0x%lx\n", c->win), fflush(r->log);
    if (!sendevent(c->win, wmatom[WMDelete], NoEventMask, wmatom[WMDelete], CurrentTime, 0, 0, 0)) {
        XGrabServer(dpy);
        XSetErrorHandler(xerrordummy);
        XSetCloseDownMode(dpy, DestroyAll);
        XKillClient(dpy, c->win);
        XSync(dpy, False);
        XSetErrorHandler(galaxyxerror);
        XUngrabServer(dpy);
    }
}

/* 屏幕坐标下的核心 (不管窗口卡片); 拖动时找松手的目标 */
static int
galaxypickcore(double x, double y)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyCore *g;
    int i, best = -1;
    double d, bd = 1e18;

    for (i = 0; i < r->ntags; i++) {
        g = &r->galaxies[i];
        if (!g->hit || (d = hypot(x - g->hx, y - g->hy)) > MAX(g->hr, 40) || d >= bd)
            continue;
        bd = d;
        best = i;
    }
    return best;
}

/* 屏幕坐标反投影到镜头空间深度 z 的平面上 */
static GalaxyVec
galaxyunproject(double x, double y, double z)
{
    GalaxyScene *r = &galaxyscene;
    double k = z / r->cam.focal;

    return galaxyadd(r->cam.pos, galaxyapply(r->cam.rot,
                galaxyv((x - r->vx - r->vw * .5) * k, (y - r->vy - r->vh * .5) * k, z)));
}

/* 关闭的窗口: 卡片缩成光点, 沿轨道切线加速飞出, 拖着长尾消失 */
static void
galaxykillstar(int i)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyStar *s = &r->stars[i];
    GalaxyVec out;
    double F = r->cam.focal;

    s->died = galaxynow();
    s->dpos = s->pos;
    s->dvel = s->vel;
    if (galaxylen(s->dvel) < .12 * F) {
        out = galaxysub(s->pos, r->galaxies[s->galaxy].pos);
        out = galaxylen(out) > 1 ? galaxynormalize(out) : galaxyv(1, 0, 0);
        s->dvel = galaxyadd(s->dvel, galaxyscale(out, .12 * F));
    }
    if (r->hover == i)
        r->hover = -1;
    if (r->ksel == i)
        r->ksel = -1;
    r->hushuntil = s->died + 3;
    if (r->log)
        fprintf(r->log, "galaxy death: star %d tag %d win 0x%lx\n", i, s->galaxy + 1, s->win), fflush(r->log);
}

/* bin/galaxynote.sh (dunst 规则) 设置 _DWM_GALAXY=note:<应用>: <摘要>; 去掉换行, 按 UTF-8 字符边界截断 */
static void
galaxynotequeue(const char *text)
{
    GalaxyScene *r = &galaxyscene;
    char *t, buf[160];
    size_t n;

    if (r->nnote >= GALAXYNOTES || !*text)
        return;
    t = r->note[r->nnote];
    n = MIN(strlen(text), sizeof r->note[0] - 1);
    while (n > 0 && (text[n] & 0xc0) == 0x80)  /* 不把多字节字符截断在中间 */
        n--;
    memcpy(t, text, n);
    t[n] = 0;
    for (; *t; t++)
        if (*t == '\n' || *t == '\t' || *t == '\r')
            *t = ' ';
    r->noteat[r->nnote++] = 0;
    if (r->log)
        fprintf(r->log, "galaxy note: queued \"%s\"\n", galaxynotetext(r->note[r->nnote - 1], buf, sizeof buf)), fflush(r->log);
}

/* 每帧: 处理待诞生的窗口, 推进彗星队列, 检查整点, 天象发生后随机特效让位 3s */
static void
galaxyupdateevents(double now, double dt)
{
    GalaxyScene *r = &galaxyscene;
    Client *c;
    struct tm tm;
    time_t tt;
    char buf[160];
    int i, active = 0;
    double last = -1e9;

    if (r->mode != GalaxyOrbit)
        return;
    for (i = 0; i < r->nbirth; i++)
        if (now - r->birthat[i] >= .6) {
            c = wintoclient(r->birthwin[i]);
            if (c && !c->isscratchpad)
                galaxyaddstar(c);
            galaxybirthdrop(i--);
        }
    /* 彗星: 6s 一颗, 同时最多 2 颗, 两颗之间至少隔 1.5s */
    for (i = 0; i < r->nnote; i++)
        if (r->noteat[i] > 0 && now - r->noteat[i] >= 6.5) {
            memmove(r->note + i, r->note + i + 1, (r->nnote - i - 1) * sizeof r->note[0]);
            memmove(r->noteat + i, r->noteat + i + 1, (r->nnote - i - 1) * sizeof *r->noteat);
            r->nnote--;
            i--;
        }
    for (i = 0; i < r->nnote; i++)
        if (r->noteat[i] > 0) {
            active++;
            last = MAX(last, r->noteat[i]);
        }
    for (i = 0; i < r->nnote && active < 2 && now - last >= 1.5; i++)
        if (r->noteat[i] == 0) {
            r->noteat[i] = last = now;
            active++;
            r->hushuntil = now + 3;
            if (r->log)
                fprintf(r->log, "galaxy note: comet \"%s\"\n", galaxynotetext(r->note[i], buf, sizeof buf)), fflush(r->log);
        }
    /* 整点报时 (GALAXY_FAKEHOUR=秒: 驻留这么多秒后假装到了整点, 测试用) */
    tt = time(NULL);
    localtime_r(&tt, &tm);
    if (r->fakehour > 0 && now >= r->fakehour) {
        r->fakehour = 0;
        r->lasthour = (tm.tm_hour + 23) % 24;
    }
    if (r->lasthour >= 0 && tm.tm_hour != r->lasthour) {
        r->chimeat = now;
        r->chimehour = tm.tm_hour;
        r->hushuntil = now + 4;
        if (r->log)
            fprintf(r->log, "galaxy chime: %02d:00\n", tm.tm_hour), fflush(r->log);
    }
    r->lasthour = tm.tm_hour;
    r->calm = galaxyfollow(r->calm, now < r->hushuntil ? .3 : 1, dt, .5);
}

/* GALAXY_DUMP: 假时钟走到列出的时刻时, 把这一帧 (back buffer) 存成 PPM, 给 tests/galaxy/golden.sh 做标准帧比较 */
static void
galaxydumpframe(void)
{
    GalaxyScene *r = &galaxyscene;
    const char *dir = getenv("GALAXY_DUMPDIR");
    char path[512];
    XImage *img;
    FILE *f;
    int x, y;
    unsigned long px;

    if (r->dumpi >= r->ndump || galaxynow() + 1e-9 < r->dumpt[r->dumpi])
        return;
    snprintf(path, sizeof path, "%s/%s-%05.2f.ppm", dir && *dir ? dir : "/tmp",
            galaxyvariantname[r->variant], r->dumpt[r->dumpi]);
    for (x = (int)strlen(dir && *dir ? dir : "/tmp") + 1; path[x]; x++)
        if (path[x] == ' ')
            path[x] = '_';
    r->dumpi++;
    if (!(img = XGetImage(dpy, r->backpix, r->vx, r->vy, r->vw, r->vh, AllPlanes, ZPixmap)))
        return;
    if ((f = fopen(path, "wb"))) {
        fprintf(f, "P6\n%d %d\n255\n", r->vw, r->vh);
        for (y = 0; y < r->vh; y++)
            for (x = 0; x < r->vw; x++) {
                px = XGetPixel(img, x, y);
                fputc(px >> 16 & 255, f);
                fputc(px >> 8 & 255, f);
                fputc(px & 255, f);
            }
        fclose(f);
        if (r->log)
            fprintf(r->log, "galaxy dump: %s\n", path), fflush(r->log);
    }
    XDestroyImage(img);
}

static void
galaxytick(void)
{
    GalaxyScene *r = &galaxyscene;
    double now, dt, u = 0, begin, cost;
    CARD16 level;
    BOOL on;
    int star, core;

    if (r->fakestep > 0)
        r->fakeclock += r->fakestep;
    now = galaxynow();
    if (r->mode == GalaxyOrbit && r->mouseawake && now - r->lastmouse >= GALAXYMOUSEIDLE)
        galaxymousesleep("idle");
    if (r->mode == GalaxyRest) {
        if (r->live && now - r->last >= 1 / 60.0) {
            r->last = now;
            galaxyrest();
        }
        return;
    }
    if (now - r->last < 1 / galaxyfps(now) - .0005)
        return;
    dt = r->last < 0 ? 0 : MIN(now - r->last, .1);
    r->pdt = dt;
    if (r->mode == GalaxyOrbit && r->dpms && now - r->lastdpms >= 1) {
        /* 屏幕关闭 (DPMS) 时暂停渲染 */
        r->lastdpms = now;
        r->dpmsoff = DPMSInfo(dpy, &level, &on) && on && level != DPMSModeOn;
    }
    if (r->mode == GalaxyOrbit && r->dpmsoff) {
        r->last = now;
        return;
    }
    switch (r->mode) {
    case GalaxyIntro:
        if (r->warping) {
            u = (now - r->wstart) / GALAXYWARP;
            r->scene = r->wscene + (GALAXYIEND - r->wscene) * galaxyeaseinoutcubic(u);
            r->beatfade = 1 - galaxysmoothstep(u * 2);   /* 快进跳过俯冲等节拍, 不在 0.6s 内闪一遍 */
            if (u >= 1)
                r->scene = GALAXYIEND;
        } else {
            r->scene += dt / r->tscale;
        }
        r->stage = galaxyintrostage(MIN(r->scene, GALAXYIEND));
        r->iclock = r->scene;
        r->motion = r->scene;
        break;
    case GalaxyOrbit:
        /* 交互让位: 镜头停住的同时轨道运动放慢到 30%, 窗口星容易点中 */
        r->scene += dt / r->tscale * galaxymix(.3, 1, r->dspeed);
        r->stage = GALAXYHOLD;
        r->motion = r->scene;
        break;
    case GalaxyCollapse:
        r->scene += dt / r->tscale;
        u = now - r->cstart;
        r->celapsed = u;
        if (u < GALAXYPREP)
            r->stage = galaxymix(r->cstage, GALAXYHOLD, galaxyeaseinoutcubic(u / GALAXYPREP));
        else if (u < GALAXYEXITSTART)
            r->stage = GALAXYHOLD;
        else
            r->stage = GALAXYEXIT + (GALAXYEND - GALAXYEXIT) * galaxyphase(u, GALAXYEXITSTART, GALAXYCOLLAPSE);
        r->motion = r->scene;
        r->exitspin = galaxyexitangle(u);
        r->beatfade = MIN(r->beatfade, 1 - galaxysmoothstep(u));   /* 开场节拍 (iclock 已冻结) 用 1s 平滑淡出 */
        if (u >= MAX(GALAXYCOLLAPSE, galaxygl.gwall ? GALAXYSHOCK + GALAXYSHOCKT + GALAXYAFTER : 0)) {
            galaxyfinish();
            return;
        }
        break;
    case GalaxyReturn:
        u = (now - r->rstart) / (r->rkind == GalaxyLand ? GALAXYLAND : GALAXYRETURN);
        if (u >= 1) {
            galaxyend(1);
            return;
        }
        break;
    }
    if (r->frames && r->ngaps < GALAXYGAPS)
        r->gaps[r->ngaps++] = now - r->last;
    r->last = now;
    begin = galaxynow();
    if (r->mode == GalaxyOrbit && r->saver) {
        /* 屏保: 指针静止不动, 不当作悬停 (否则导演镜头会一直停住) */
        r->hover = r->hovercore = -1;
    } else if (r->mode == GalaxyOrbit && r->dragging) {
        r->hover = r->hovercore = -1;
        galaxysetcursor(0);
    } else if (r->mode == GalaxyOrbit && r->dragstar >= 0) {
        /* 拖着卡片: 只找松手的目标核心 */
        r->hover = -1;
        r->hovercore = galaxypickcore(r->mx, r->my);
        galaxysetcursor(1);
    } else if (r->mode == GalaxyOrbit && r->mouseawake && r->mousevalid) {
        galaxypick(r->mx, r->my, &star, &core);
        r->hover = star;
        r->hovercore = core;
        galaxysetcursor(star >= 0 || core >= 0);
    } else if (r->mode == GalaxyOrbit && galaxykeyactive()) {
        /* 键盘导航中: 选中的星当作悬停 (显示标题, 镜头停住) */
        r->hover = r->ksel;
        r->hovercore = -1;
        if (r->mouseawake)
            galaxysetcursor(0);
    } else if (r->mode == GalaxyOrbit) {
        r->hover = r->hovercore = -1;
        galaxysetcursor(r->mouseawake ? 0 : -1);
    }
    if (r->mode == GalaxyReturn) {
        r->retu = u;
        if (r->rkind == GalaxyFlyHome)
            galaxyupdatereturn(u);
        else
            galaxyupdateland(u);
    } else {
        galaxyupdateevents(now, dt);
        galaxyupdatescene(r->stage, r->motion, dt);
        galaxyheatstep(now);
    }
    r->phasecost[0] += galaxynow() - begin;
    galaxyspacestep();
    galaxyrender();
    galaxydumpframe();
    cost = galaxynow() - begin;
    r->lastcost = cost;
    if (r->mode == GalaxyOrbit && now - r->lastinput < GALAXYIDLE) {
        r->qualityavg = r->qualityavg ? galaxymix(r->qualityavg, cost, .045) : cost;
        r->qualitybad = r->qualityavg > .0168 ? r->qualitybad + 1 : 0;
        r->qualitygood = r->qualityavg < .0135 ? r->qualitygood + 1 : 0;
        if (now - r->qualitylast > 2.5) {
            if (r->qualitybad >= 45 && r->quality < 3) {
                r->quality++;
                r->qualitylast = now;
                r->qualitybad = r->qualitygood = 0;
            } else if (r->qualitygood >= 150 && r->quality > 0) {
                r->quality--;
                r->qualitylast = now;
                r->qualitybad = r->qualitygood = 0;
            }
        }
    }
    r->qualityvisual = galaxyfollow(r->qualityvisual, r->quality, dt, .35);
    if (r->mode == GalaxyIntro && r->iclock >= 3.2 && r->iclock < 5) {
        r->divesum += cost;
        r->divemax = MAX(r->divemax, cost);
        r->divenum++;
    }
    if (r->mode == GalaxyIntro && r->nfirst < (int)LENGTH(r->firstcost)) {
        r->firstgap[r->nfirst] = now;   /* 相对按下 Super+Z 后时钟起点的时刻 */
        r->firstcost[r->nfirst++] = cost;
    }
    r->rendersum += cost;
    r->rendermax = MAX(r->rendermax, cost);
    r->frames++;
    if (r->mode == GalaxyIntro && r->scene >= GALAXYIEND)
        galaxyorbitstart();
    else if (r->mode == GalaxyOrbit && now - r->segstart >= 10)
        galaxylogseg("orbit");
}

/* select() 的超时 (微秒), -1 表示没有动画, 一直等待 X 事件 */
static long
galaxytimeout(void)
{
    GalaxyScene *r = &galaxyscene;
    double now, left;

    if (r->mode == GalaxyRest)
        return r->live ? 16666 : -1;
    now = galaxynow();
    if (r->mode == GalaxyOrbit && r->dpmsoff)
        return 1000000;
    left = 1 / galaxyfps(now) - (now - r->last);
    return left > 0 ? (long)(left * 1e6) : 0;
}

/* 动画期间 dwm 主循环先把事件交给这里; 返回 1 表示已处理, 不再交给 dwm */
static int
galaxyevent(XEvent *e)
{
    GalaxyScene *r = &galaxyscene;
    KeySym sym;
    Window w;
    int i, esc, superz, star, core;

    if (!r->mode)
        return 0;
    if (r->saver && (r->mode == GalaxyIntro || r->mode == GalaxyOrbit)) {
        /* 屏保模式: 任何按键、点击, 或鼠标移动超过 8px, 都走回程回到桌面 */
        if ((e->type == KeyPress && !IsModifierKey(XLookupKeysym(&e->xkey, 0))) || e->type == ButtonPress
                || (e->type == MotionNotify && hypot(e->xmotion.x_root - r->saverx, e->xmotion.y_root - r->savery) > 8)) {
            if (r->log)
                fprintf(r->log, "galaxy saver: input, back to desktop\n");
            galaxyreturnstart(-1, -1);
            return 1;
        }
        if (e->type == KeyPress || e->type == KeyRelease || e->type == ButtonPress || e->type == ButtonRelease
                || e->type == MotionNotify)
            return 1;
    }
    switch (e->type) {
    case KeyPress:
        sym = XLookupKeysym(&e->xkey, 0);
        if (IsModifierKey(sym))
            return 1;
        r->lastinput = galaxynow();
        esc = sym == XK_Escape;
        superz = sym == XK_z && CLEANMASK(e->xkey.state) == MODKEY;
        switch (r->mode) {
        case GalaxyIntro:
            if (esc)
                galaxycollapsestart();
            else if (superz)
                galaxyreturnstart(-1, -1);
            /* 开场是完整演出，普通按键不改变时间轴。需要快进时可先唤醒鼠标再点击。 */
            break;
        case GalaxyOrbit:
            if (superz)
                galaxyreturnstart(-1, -1);
            else if (esc && (r->kqlen || (r->ksel >= 0 && galaxykeyactive())))
                galaxykeyclear();       /* 先清空过滤词和选中 */
            else if (esc)
                galaxycollapsestart();  /* 鼠标醒着也直接退出, 不再只休眠 */
            else
                galaxykey(e, sym);
            break;
        case GalaxyCollapse:
            if (esc)
                galaxyfinish();
            else if (superz)
                galaxycancel();
            break;
        case GalaxyReturn:
            if (esc || superz)
                galaxyend(1);
            break;
        case GalaxyRest:
            galaxycancel();
            break;
        }
        return 1;
    case ButtonPress:
        switch (r->mode) {
        case GalaxyIntro:
            if (!r->mouseawake) {
                if (e->xbutton.button == Button1)
                    galaxymousewake();
                break;
            }
            r->lastinput = galaxynow();
            if (e->xbutton.button <= Button3)
                galaxywarp();
            break;
        case GalaxyOrbit:
            if (!r->mouseawake) {
                if (e->xbutton.button == Button1)
                    galaxymousewake();
                break;
            }
            r->lastinput = r->lastmouse = galaxynow();
            r->mx = e->xbutton.x_root;
            r->my = e->xbutton.y_root;
            /* 默认就是最近 (zoom 1), 向上滚到底不再变化, 向下滚逐级拉远 */
            if (e->xbutton.button == Button4) {
                r->tzoom = MAX(1, r->tzoom * .9);
                r->lastpointer = galaxynow();
            } else if (e->xbutton.button == Button5) {
                r->tzoom = MIN(1.8, r->tzoom / .9);
                r->lastpointer = galaxynow();
            } else if (e->xbutton.button == Button1) {
                /* 按在卡片上先不进入: 松手时没拖动才算点击, 拖动超过 8px 就是把窗口拖到别的核心 */
                r->mousevalid = 1;
                galaxypick(r->mx, r->my, &star, &core);
                if (r->trace && r->log)
                    fprintf(r->log, "galaxy press: %.0f,%.0f star %d core %d\n", r->mx, r->my, star, core), fflush(r->log);
                if (star >= 0) {
                    r->dragarm = star;
                    r->dragx0 = r->mx;
                    r->dragy0 = r->my;
                } else if (core >= 0) {
                    galaxyreturnstart(core, -1);
                }
            } else if (e->xbutton.button == Button2) {
                galaxypick(r->mx, r->my, &star, &core);
                if (star >= 0)
                    galaxyclosestar(star);
            } else if (e->xbutton.button == Button3) {
                r->dragging = 1;
                r->dragx = r->mx;
                r->dragy = r->my;
                r->dragtime = r->lastmouse;
                r->dragvyaw = r->dragvpitch = 0;
                r->mousevalid = 0;
                r->hover = r->hovercore = -1;
                galaxysetcursor(0);
                if (r->log) {
                    fprintf(r->log, "galaxy mouse: drag start\n");
                    fflush(r->log);
                }
            }
            if (r->log && (e->xbutton.button == Button4 || e->xbutton.button == Button5)) {
                fprintf(r->log, "galaxy mouse: zoom %.2f\n", r->tzoom);
                fflush(r->log);
            }
            break;
        case GalaxyRest:
            galaxycancel();
            break;
        }
        return 1;
    case MotionNotify:
        if ((r->mode == GalaxyIntro || r->mode == GalaxyOrbit) && !r->mouseawake)
            return 1;
        r->lastinput = r->lastpointer = galaxynow();
        if (r->mode == GalaxyOrbit) {
            r->lastmouse = r->lastinput;
            r->mousevalid = !r->dragging;
        }
        r->lastkey = -1e9;      /* 鼠标一动, 悬停交还给鼠标 */
        r->mx = e->xmotion.x_root;
        r->my = e->xmotion.y_root;
        if (r->mode == GalaxyOrbit && r->dragarm >= 0 && r->dragarm < r->nstars
                && hypot(r->mx - r->dragx0, r->my - r->dragy0) > 8) {
            r->dragstar = r->dragarm;
            r->dragarm = -1;
            r->dragz = r->stars[r->dragstar].p.ok ? r->stars[r->dragstar].p.z : r->cam.focal;
            if (r->log)
                fprintf(r->log, "galaxy drag: star %d \"%s\"\n", r->dragstar, r->stars[r->dragstar].title), fflush(r->log);
        }
        if (r->mode == GalaxyOrbit && r->dragstar >= 0) {
            r->dragpos = galaxyunproject(r->mx, r->my, r->dragz);
            r->mousevalid = 0;
            return 1;
        }
        if (r->mode == GalaxyOrbit && r->dragging) {
            double dt = r->lastmouse - r->dragtime;
            double dyaw = (r->mx - r->dragx) * 145.0 / r->vw;
            double dpitch = (r->my - r->dragy) * 105.0 / r->vh;
            r->dragtyaw = MAX(-60, MIN(60, r->dragtyaw + dyaw));
            r->dragtpitch = MAX(-32, MIN(32, r->dragtpitch + dpitch));
            if (dt > .004 && dt < .12) {
                r->dragvyaw = MAX(-45, MIN(45, dyaw / dt));
                r->dragvpitch = MAX(-40, MIN(40, dpitch / dt));
            }
            r->dragx = r->mx;
            r->dragy = r->my;
            r->dragtime = r->lastmouse;
            return 1;
        }
        /* 鼠标视差: 镜头随指针小幅偏航 / 俯仰 (幅度小, 瞄准窗口星时目标不会明显跑开) */
        r->tyaw = ((r->mx - r->vx) / r->vw - .5) * 2 * 4;
        r->tpitch = ((r->my - r->vy) / r->vh - .5) * 2 * 2.5;
        return 1;
    case ButtonRelease:
        if (r->mode == GalaxyOrbit && e->xbutton.button == Button1 && r->dragstar >= 0) {
            i = r->dragstar;
            r->dragstar = -1;
            r->mousevalid = 1;
            galaxymovestar(i, galaxypickcore(e->xbutton.x_root, e->xbutton.y_root));
        } else if (r->mode == GalaxyOrbit && e->xbutton.button == Button1 && r->dragarm >= 0) {
            i = r->dragarm;
            r->dragarm = -1;
            galaxyreturnstart(-1, i);
        }
        if (r->mode == GalaxyOrbit && r->dragging && e->xbutton.button == Button3) {
            r->dragging = 0;
            if (galaxynow() - r->dragtime > .1)
                r->dragvyaw = r->dragvpitch = 0;
            r->lastmouse = r->lastpointer = galaxynow();
            if (r->log) {
                fprintf(r->log, "galaxy mouse: drag end yaw %.2f pitch %.2f\n", r->dragtyaw, r->dragtpitch);
                fflush(r->log);
            }
        }
        return 1;
    case KeyRelease: case EnterNotify: case LeaveNotify:
        return 1;
    case Expose:
        if (e->xexpose.window != r->overlay)
            return 0;
        if (r->mode == GalaxyRest)
            galaxypresentwall();
        return 1;
    case DestroyNotify:
    case UnmapNotify:
        /* 窗口在动画中关闭: 星体标记失效, 缓存的截图继续显示到动画结束.
         * 经 root 的 SubstructureNotify 收到时 xany.window 是 root, 关闭的窗口在 xdestroywindow / xunmap.window */
        w = e->type == DestroyNotify ? e->xdestroywindow.window : e->xunmap.window;
        for (i = 0; i < r->nbirth; i++)
            if (r->birthwin[i] == w)
                galaxybirthdrop(i--);
        for (i = 0; i < r->nstars; i++)
            if (r->stars[i].win == w && r->stars[i].valid) {
                r->stars[i].valid = 0;
                r->stars[i].c = NULL;
                if (r->log)
                    fprintf(r->log, "star %d window 0x%lx gone during animation\n", i, r->stars[i].win), fflush(r->log);
                /* 开场 / 驻留中关闭的窗口化作流星飞走 (坍缩 / 回程中照旧显示到动画结束) */
                if ((r->mode == GalaxyIntro || r->mode == GalaxyOrbit) && !r->stars[i].died && r->stars[i].alpha > .05)
                    galaxykillstar(i);
            }
        if (e->type == DestroyNotify && e->xdestroywindow.window == r->twin && r->mode == GalaxyReturn)
            r->twin = None;
        if (e->type == DestroyNotify && e->xdestroywindow.window == r->wallwin && r->wallwin) {
            r->wallwin = None;
            if (r->live)
                XRenderFreePicture(dpy, r->live);
            r->live = 0;
        }
        return 0;
    }
    return 0;
}

/* dwm 处理完事件之后: 新映射 / 重排的托管窗口不能盖过遮罩, 错误处理器可能被 unmanage 等改回.
 * 非托管的 override-redirect 窗口 (锁屏, 通知) 不去压它, 驻留态可能持续很久, 锁屏必须在最上面 */
static void
galaxypost(XEvent *e)
{
    GalaxyScene *r = &galaxyscene;

    if (!r->mode)
        return;
    XSetErrorHandler(galaxyxerror);
    if (e->xany.window == r->overlay)
        return;
    switch (e->type) {
    case MapNotify:
        if (!e->xmap.override_redirect)
            XRaiseWindow(dpy, r->overlay);
        if ((r->mode == GalaxyIntro || r->mode == GalaxyOrbit) && wintoclient(e->xmap.window))
            galaxybirthqueue(e->xmap.window);     /* 星系运行中新开的窗口: 诞生一颗新星 */
        break;
    case MapRequest: case ConfigureRequest: case ClientMessage:
        XRaiseWindow(dpy, r->overlay);
        break;
    case ConfigureNotify:
        if (e->xconfigure.window != root && wintoclient(e->xconfigure.window))
            XRaiseWindow(dpy, r->overlay);
        break;
    case CirculateNotify:
        if (wintoclient(e->xcirculate.window))
            XRaiseWindow(dpy, r->overlay);
        break;
    }
    if ((r->mode == GalaxyRest || r->mode == GalaxyOrbit)
            && (e->type == MapRequest || e->type == ClientMessage || e->type == DestroyNotify || e->type == UnmapNotify))
        galaxyrest();
}

/* 比较一次仿射和一次透视, 决定大卡片旋转时走哪条路径. 在遮罩映射前做, 用户看不到. */
static void
galaxyprobeprojective(void)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyStar *s, *best = NULL;
    double q[4][2];
    struct timespec a, b;
    int i, lvl, w, h, pass;

    for (i = 0; i < r->nstars; i++) {
        s = &r->stars[i];
        if (!s->snap || !s->mip[s->base])
            continue;
        if (!best || (long)s->mipw[s->base] * s->miph[s->base]
                > (long)best->mipw[best->base] * best->miph[best->base])
            best = s;
    }
    if (!best)
        return;
    lvl = best->base;
    w = MIN(best->mipw[lvl], 1280);
    h = MIN(best->miph[lvl], 720);
    if (w < 32 || h < 32)
        return;
    XRenderSetPictureFilter(dpy, best->mip[lvl], FilterBilinear, NULL, 0);
    XSync(dpy, False);
    galaxyaffine(best->mip[lvl], (double)best->mipw[lvl] / w, (double)best->miph[lvl] / h, 0, 0);
    XRenderComposite(dpy, PictOpSrc, best->mip[lvl], None, r->back, 0, 0, 0, 0, 0, 0, w, h);
    XSync(dpy, False);
    clock_gettime(CLOCK_MONOTONIC, &a);
    XRenderComposite(dpy, PictOpSrc, best->mip[lvl], None, r->back, 0, 0, 0, 0, 0, 0, w, h);
    XSync(dpy, False);
    clock_gettime(CLOCK_MONOTONIC, &b);
    r->probeaffine = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
    q[0][0] = 0;
    q[0][1] = 0;
    q[1][0] = w;
    q[1][1] = h * .06;
    q[2][0] = w * .94;
    q[2][1] = h;
    q[3][0] = w * .05;
    q[3][1] = h * .93;
    if (!galaxyhomography(best->mip[lvl], q, w, h, best->mipw[lvl], best->miph[lvl]))
        return;
    for (pass = 0; pass < 2; pass++) {
        clock_gettime(CLOCK_MONOTONIC, &a);
        XRenderComposite(dpy, PictOpSrc, best->mip[lvl], None, r->back, 0, 0, 0, 0, 0, 0, w, h);
        XSync(dpy, False);
        clock_gettime(CLOCK_MONOTONIC, &b);
        r->probeproj = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
    }
    /* 第二次才算数: 第一次含着色器 / 纹理上传.
     * 只在透视像软件采样那样慢几倍时才降到半分辨率, 避免 GPU 上为了几毫秒损失清晰度. */
    r->projslow = r->probeproj > .012 && r->probeproj > r->probeaffine * 4;
    if (r->projslow) {
        r->spinw = MAX(1, (r->w + 1) / 2);
        r->spinh = MAX(1, (r->h + 1) / 2);
        r->spinpic = galaxyargb(r->spinw, r->spinh, &r->spinpix);
        if (!r->spinpic)
            r->projslow = 0;
        else
            XRenderSetPictureFilter(dpy, r->spinpic, FilterBilinear, NULL, 0);
    }
    /* 当前桌面的大截图开场会整幅拷贝, 先做一遍, 避免前几帧卡在第一次上传. */
    for (i = 0; i < r->nstars; i++) {
        s = &r->stars[i];
        if (!s->current || !s->snap || !s->mip[s->base] || s->mipw[s->base] < 800)
            continue;
        w = MIN(s->mipw[s->base], r->w);
        h = MIN(s->miph[s->base], r->h);
        galaxyaffine(s->mip[s->base], 1, 1, 0, 0);
        XRenderSetPictureFilter(dpy, s->mip[s->base], FilterNearest, NULL, 0);
        XRenderComposite(dpy, PictOpSrc, s->mip[s->base], None, r->back, 0, 0, 0, 0, 0, 0, w, h);
        XRenderSetPictureFilter(dpy, s->mip[s->base], FilterBilinear, NULL, 0);
    }
    XSync(dpy, False);
}

static void
galaxy(const Arg *arg)
{
    GalaxyScene *r = &galaxyscene;
    XSetWindowAttributes wa;
    XClassHint cls = {"dwm-galaxy", "dwm-galaxy"};
    XRenderColor color;
    Monitor *m;
    Client *c;
    GalaxyStar *s;
    Window dw;
    struct timespec t0, t1, t2;
    const char *env;
    int i, count = 0, ev, er, full, pass, dx, dy;
    unsigned int cur, mask;
    long budget = 320L << 20, bytes;

    if (r->mode) {
        galaxycancel();
        return;
    }
    clock_gettime(CLOCK_MONOTONIC, &t0);
    memset(r, 0, sizeof *r);
    r->w = sw;
    r->h = sh;
    r->vx = selmon->mx;
    r->vy = selmon->my;
    r->vw = selmon->mw;
    r->vh = selmon->mh;
    r->ntags = MIN((int)LENGTH(tags), 31);
    r->hover = r->hovercore = -1;
    r->zoom = r->tzoom = 1;
    r->dfit = r->dspeed = r->beatfade = 1;
    r->ksel = -1;
    r->lastkey = -1e9;
    r->dshot = r->lastflip = r->lastripple = r->lastnova = r->lastcomet = -1;
    r->dtg[0] = r->dtg[1] = -1;
    r->tourhist[0] = r->tourhist[1] = r->tourhist[2] = -1;
    r->flipk = r->flipg = r->novak = r->novag = r->constk = r->constg = -1;
    r->bridgek = r->bri = r->brj = -1;
    r->eclipsepair = -1;
    r->lastpointer = -1e9;
    r->lasthour = r->pulsar = -1;
    r->dragstar = r->dragarm = -1;
    r->calm = 1;
    if ((env = getenv("GALAXY_FAKETIME")) && atof(env) > 0)     /* 测试: 确定性时钟, 每帧前进 1/帧率 秒 */
        r->fakestep = 1 / atof(env);
    for (env = getenv("GALAXY_DUMP"); env && *env && r->ndump < (int)LENGTH(r->dumpt); env = strchr(env, ',') ? strchr(env, ',') + 1 : "")
        r->dumpt[r->ndump++] = atof(env);
    /* 开场变体: 随机选一套, 不和上一次重复 */
    {
        static int lastvariant = -1;
        if ((env = getenv("GALAXY_VARIANT")) && *env >= 'A' && *env < 'A' + GalaxyVariants)
            r->variant = *env - 'A';
        else if (r->fakestep > 0)
            r->variant = GalaxyShatter;
        else if (lastvariant < 0)
            r->variant = (int)(t0.tv_nsec / 1000 % GalaxyVariants);
        else
            r->variant = (lastvariant + 1 + (int)(t0.tv_nsec / 1000 % (GalaxyVariants - 1))) % GalaxyVariants;
        lastvariant = r->variant;
    }
    if ((env = getenv("GALAXY_FAKEHOUR")))   /* 测试: 驻留这么多秒后假装到了整点 */
        r->fakehour = atof(env);
    r->bglast[0] = -1;
    XQueryExtension(dpy, "RENDER", &r->rendermajor, &ev, &er);
    r->dpms = DPMSQueryExtension(dpy, &ev, &er) && DPMSCapable(dpy);
    XSetErrorHandler(galaxyxerror);
    r->argb = XRenderFindStandardFormat(dpy, PictStandardARGB32);
    r->a8 = XRenderFindStandardFormat(dpy, PictStandardA8);
    r->a1 = XRenderFindStandardFormat(dpy, PictStandardA1);
    for (m = mons; m; m = m->next)
        for (c = m->clients; c; c = c->next)
            if (!c->isscratchpad)
                count++;
    r->nstars = count;
    r->maxstars = count + GALAXYSPARE;
    r->tscale = count ? GALAXYINTRO : 1;   /* 没有窗口: 同样的视觉语言, 更短的开场 */
    r->starscale = MAX(.65, MIN(1.1, 1.15 - .012 * count));
    r->orbitscale = MAX(.7, MIN(1, 1.05 - .008 * count));
    r->glowscale = MAX(.55, MIN(1, 1.1 - .015 * count));
    r->trace = getenv("GALAXY_TRACE") != NULL;
    r->quiet = galaxyquiet();
    r->refresh = galaxyrefresh();
    r->fxgap = GALAXY_FXGAP * (r->quiet ? 2.5 : 1);
    r->ndust = MAX(70, MIN(140, 140 - 2 * count)) / (r->quiet ? 2 : 1);
    r->ntrail = count > 24 ? 6 : GALAXYTRAIL;
    r->cam.fov = 62 * GALAXYPI / 180;
    r->cam.focal = r->vw * .5 / tan(r->cam.fov / 2);
    r->cam.near = r->cam.focal * .12;
    r->cam.far = r->cam.focal * 14;
    r->galaxies = calloc(r->ntags, sizeof *r->galaxies);
    r->stars = calloc(r->maxstars, sizeof *r->stars);
    r->dust = calloc(r->ndust, sizeof *r->dust);
    r->items = calloc(r->ndust + r->ntags * (4 + GALAXYARCS * GALAXYRINGS) + r->maxstars + GALAXYARCS * GALAXYLANES + 2, sizeof *r->items);
    r->tgpos = calloc(r->ntags, sizeof *r->tgpos);
    r->tgplane = calloc(r->ntags, sizeof *r->tgplane);
    r->tpts = calloc((r->maxstars + r->ntags) * (r->ntrail + 1), sizeof *r->tpts);
    r->rpts = calloc(r->ntags * GALAXYRINGS * (GALAXYSEG + 1), sizeof *r->rpts);
    r->streakpts = calloc(r->ntags * (GALAXYSTREAK + 1), sizeof *r->streakpts);
    r->streakz = calloc(r->ntags * 3, sizeof *r->streakz);
    r->popord = calloc(r->ntags, sizeof *r->popord);
    r->gaps = calloc(GALAXYGAPS, sizeof *r->gaps);
    r->parts = calloc(GALAXYPARTICLES, sizeof *r->parts);
    r->novaburst = r->bangburst = -1;
    r->bandcolor = 0xffffffffu;
    if (!r->galaxies || !r->stars || !r->dust || !r->items || !r->tgpos || !r->tgplane || !r->tpts || !r->rpts
            || !r->streakpts || !r->streakz || !r->popord || !r->gaps || !r->parts || !r->argb || !r->a8 || !r->a1)
        goto fail;
    r->savedmon = r->tmon = selmon;
    r->savedtags = r->ttags = selmon->tagset[selmon->seltags] & TAGMASK;
    r->savedwin = r->twin = selmon->sel ? selmon->sel->win : None;
    cur = r->savedtags ? (unsigned int)__builtin_ctz(r->savedtags) : 0;

    if (!(r->back = galaxyopaque(r->w, r->h, &r->backpix)) || !(r->desktop = galaxyopaque(r->w, r->h, &r->desktoppix))
            || !(r->wallpaper = galaxyopaque(r->w, r->h, &r->wallpix)) || !(r->bg = galaxyopaque(r->w, r->h, &r->bgpix)))
        goto fail;
    /* Xft 不会逐字回退到别的字体: 选一个同时含中文和拉丁字母的 (窗口标题常有中文) */
    r->titlefont = XftFontOpenName(dpy, screen, "sans:lang=zh-cn:size=11");
    r->queryfont = XftFontOpenName(dpy, screen, "sans:lang=zh-cn:size=16");
    r->notefont = XftFontOpenName(dpy, screen, "sans:lang=zh-cn:size=15");
    r->clockfont = XftFontOpenName(dpy, screen, "sans:lang=zh-cn:size=34:weight=light");
    /* 前景层: 32 位 ARGB pixmap, 文字和标签画在这里 (透明处露出下面的光和场景) */
    {
        XVisualInfo vinfo;
        if (!XMatchVisualInfo(dpy, screen, 32, TrueColor, &vinfo))
            goto fail;
        r->argbvisual = vinfo.visual;
        r->argbcmap = XCreateColormap(dpy, root, r->argbvisual, AllocNone);
        r->frontpix = XCreatePixmap(dpy, root, r->w, r->h, 32);
        if (!r->frontpix || !(r->front = XRenderCreatePicture(dpy, r->frontpix, r->argb, 0, NULL)))
            goto fail;
    }
    if (r->titlefont)
        r->titledraw = XftDrawCreate(dpy, r->frontpix, r->argbvisual, r->argbcmap);
    if (r->titledraw) {
        XRenderColor white = {0xe900, 0xf600, 0xffff, 0xffff};
        r->titlecolorok = XftColorAllocValue(dpy, r->argbvisual, r->argbcmap, &white, &r->titlecolor);
    }
    if (!r->titlecolorok && r->titledraw) {
        XftDrawDestroy(r->titledraw);
        r->titledraw = NULL;
    }
    for (i = 0; i < GALAXYALPHAS; i++) {
        color.alpha = color.red = color.green = color.blue = (unsigned short)(65535L * i / (GALAXYALPHAS - 1));
        r->white[i] = XRenderCreateSolidFill(dpy, &color);
        color.red = color.green = color.blue = 0;
        r->black[i] = XRenderCreateSolidFill(dpy, &color);
    }
    r->tilemaskpix = XCreatePixmap(dpy, root, r->w, r->h, 8);
    if (!r->tilemaskpix || !(r->tilemask = XRenderCreatePicture(dpy, r->tilemaskpix, r->a8, 0, NULL)))
        goto fail;
    galaxybuildvignette();

    i = 0;
    for (m = mons; m; m = m->next)
        for (c = m->clients; c; c = c->next) {
            if (c->isscratchpad)
                continue;
            s = &r->stars[i++];
            galaxyinitstar(s, c, m, cur);
            r->galaxies[s->galaxy].nstars++;
        }
    XGrabServer(dpy);
    galaxycapturebackground();
    galaxyspacebegin();
    if (r->variant == GalaxyShatter)
        galaxybuildshards();
    /* 每个窗口只截一次. 当前桌面的窗口优先截全尺寸, 其余窗口在预算内也截全尺寸 (回程 / 点击跳转时 1:1 显示), 超出的截半尺寸 */
    for (pass = 0; pass < 2; pass++)
        for (i = 0; i < r->nstars; i++) {
            s = &r->stars[i];
            if (s->current != !pass)
                continue;
            bytes = (long)s->w * s->h * 16 / 3;   /* 4 字节 x (1 + mip 链约 1/3) */
            full = pass ? budget >= bytes : budget > 0;
            if (full)
                budget -= bytes;
            galaxycapture(s, s->c, full);
        }
    XSync(dpy, False);
    XUngrabServer(dpy);
    galaxyheatpids();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    galaxyprobeprojective();
    galaxybuildcores();
    galaxybuildorbits();
    galaxybuilddust();
    for (i = 0; i < r->nstars; i++)     /* 开场 C 的爆心: 焦点窗口的中心 (没有焦点时是视口中心) */
        if (r->stars[i].focused)
            r->bang = r->stars[i].home;
    if (XQueryPointer(dpy, root, &dw, &dw, &dx, &dy, &ev, &er, &mask)) {
        r->mx = dx;
        r->my = dy;
        r->tyaw = ((r->mx - r->vx) / r->vw - .5) * 2 * 4;
        r->tpitch = ((r->my - r->vy) / r->vh - .5) * 2 * 2.5;
    }

    /* 遮罩用 OpenGL 的 visual (一般就是默认 visual); 背景就是刚截的桌面, 映射瞬间不会闪黑 */
    if (!galaxyglinit()) {
        galaxylogstart();
        if (r->log)
            fprintf(r->log, "galaxy start failed: opengl\n");
        goto fail;
    }
    wa.override_redirect = True;
    wa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
    wa.background_pixmap = r->desktoppix;
    wa.colormap = galaxygl.cmap;
    wa.border_pixel = 0;
    r->overlay = XCreateWindow(dpy, root, 0, 0, r->w, r->h, 0, galaxygl.vi->depth, InputOutput, galaxygl.vi->visual,
            CWOverrideRedirect | CWEventMask | CWColormap | CWBorderPixel
            | (galaxygl.vi->depth == DefaultDepth(dpy, screen) ? CWBackPixmap : 0), &wa);
    XSetClassHint(dpy, r->overlay, &cls);
    XStoreName(dpy, r->overlay, "dwm-galaxy");
    r->hand = XCreateFontCursor(dpy, XC_hand2);
    {
        static const char empty[1] = {0};
        XColor clear = {0};
        Pixmap bits = XCreateBitmapFromData(dpy, r->overlay, empty, 1, 1);
        if (bits) {
            r->blankcursor = XCreatePixmapCursor(dpy, bits, bits, &clear, &clear, 0, 0);
            XFreePixmap(dpy, bits);
        }
    }
    r->overlaypic = XRenderCreatePicture(dpy, r->overlay, XRenderFindVisualFormat(dpy, galaxygl.vi->visual), 0, NULL);
    if (!r->overlaypic)
        goto fail;
    XMapRaised(dpy, r->overlay);
    if (!galaxyglbegin(r->overlay, r->backpix, r->frontpix, r->wallpix, r->w, r->h)) {
        galaxylogstart();
        if (r->log)
            fprintf(r->log, "galaxy start failed: opengl\n");
        goto fail;
    }
    r->mode = GalaxyIntro;
    /* 别的程序可能正短暂持有抓取 (菜单刚关闭等): 最多重试约 0.5s, 失败时把原因写进日志 */
    for (i = 0; i < 50 && XGrabKeyboard(dpy, r->overlay, False, GrabModeAsync, GrabModeAsync, CurrentTime) != GrabSuccess; i++)
        nanosleep(&(struct timespec){0, 10000000}, NULL);
    if (i == 50) {
        galaxylogstart();
        if (r->log)
            fprintf(r->log, "galaxy start failed: keyboard grab\n");
        goto fail;
    }
    r->grabkbd = 1;
    for (i = 0; i < 50 && XGrabPointer(dpy, r->overlay, False, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                GrabModeAsync, GrabModeAsync, None, None, CurrentTime) != GrabSuccess; i++)
        nanosleep(&(struct timespec){0, 10000000}, NULL);
    if (i == 50) {
        galaxylogstart();
        if (r->log)
            fprintf(r->log, "galaxy start failed: pointer grab\n");
        goto fail;
    }
    r->grabptr = 1;
    galaxylogstart();
    if (r->log) {
        fprintf(r->log, "galaxy variant: %s\n", galaxyvariantname[r->variant]);
        fprintf(r->log, "galaxy gl: pixmap orientation base %s front %s (FBConfig %s)\n",
                galaxygl.yinv24 ? "direct" : "flipped", galaxygl.yinv32 ? "direct" : "flipped",
                galaxygl.yinv32config ? "direct" : "flipped");
    }
    galaxymousesleep("intro start");
    XSync(dpy, False);
    clock_gettime(CLOCK_MONOTONIC, &r->start);
    r->last = -1;
    r->segstart = 0;
    galaxytick();
    if (r->log) {
        clock_gettime(CLOCK_MONOTONIC, &t2);
        fprintf(r->log, "galaxy setup: grab+capture %.1fms, key -> first frame %.1fms, probe affine %.1fms proj %.1fms %s\n",
                (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6,
                (t2.tv_sec - t0.tv_sec) * 1e3 + (t2.tv_nsec - t0.tv_nsec) / 1e6,
                r->probeaffine * 1e3, r->probeproj * 1e3, r->projslow ? "half" : "direct");
        fflush(r->log);
    }
    return;
fail:
    if (r->log)
        fprintf(r->log, "galaxy start failed\n");
    r->mode = GalaxyIntro;
    galaxyend(1);
}

/* bin/galaxysaver.py 在 root 上设置 _DWM_GALAXY=saver: 无操作到时间, 以屏保模式进入星系 (星系已在运行时忽略);
 * bin/galaxynote.sh 设置 note:<文字>: 星系运行中的通知彗星 */
static void
galaxyproperty(XPropertyEvent *ev)
{
    static Atom atom;
    GalaxyScene *r = &galaxyscene;
    char value[256] = "";
    XTextProperty tp;

    if (!atom)
        atom = XInternAtom(dpy, "_DWM_GALAXY", False);
    if (ev->atom != atom)
        return;
    if (XGetTextProperty(dpy, root, &tp, atom) && tp.value) {
        snprintf(value, sizeof value, "%s", (char *)tp.value);
        XFree(tp.value);
    }
    XDeleteProperty(dpy, root, atom);
    if (!strncmp(value, "note:", 5)) {     /* 通知: 星系运行中化作一颗带标题的彗星, 没运行时忽略 */
        if (r->mode == GalaxyIntro || r->mode == GalaxyOrbit)
            galaxynotequeue(value + 5);
        return;
    }
    if (strcmp(value, "saver") || galaxyactive())
        return;
    galaxy(&(Arg){0});
    if (!galaxyactive())
        return;
    r->saver = 1;
    r->saverx = r->mx;
    r->savery = r->my;
    if (r->log)
        fprintf(r->log, "galaxy saver: started after idle\n");
}
