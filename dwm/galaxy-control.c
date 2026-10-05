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
            show(c);  /* 点击了隐藏窗口的星体: 恢复它 (与 Super+A 预览选中隐藏窗口一致) */
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
}

static void
galaxycancel(void)
{
    galaxyend(1);
}

/* 坍缩结束 (或坍缩中再按 Esc): 遮罩只显示壁纸, 释放全部星系资源, 等 Super+Z / 任意键恢复 */
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
    XSetWindowBackgroundPixmap(dpy, r->overlay, None);
    if (r->desktop) XRenderFreePicture(dpy, r->desktop);
    if (r->desktoppix) XFreePixmap(dpy, r->desktoppix);
    if (r->back) XRenderFreePicture(dpy, r->back);
    if (r->backpix) XFreePixmap(dpy, r->backpix);
    if (r->bg) XRenderFreePicture(dpy, r->bg);
    if (r->bgpix) XFreePixmap(dpy, r->bgpix);
    r->desktop = r->back = r->bg = 0;
    r->desktoppix = r->backpix = r->bgpix = 0;
    r->mode = GalaxyRest;
    galaxysetcursor(0);
    if (r->live)
        XRenderComposite(dpy, PictOpSrc, r->live, None, r->wallpaper, 0, 0, 0, 0, 0, 0, r->w, r->h);
    XRenderComposite(dpy, PictOpSrc, r->wallpaper, None, r->overlaypic, 0, 0, 0, 0, 0, 0, r->w, r->h);
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
    if (!r->saver)
        galaxymousesleep("orbit start");
    galaxyrelease();
}

static void
galaxycollapsestart(void)
{
    GalaxyScene *r = &galaxyscene;

    galaxylogseg(galaxymodename[r->mode]);
    r->cstage = r->stage;
    r->cworld = r->world;
    r->exitspin = 0;
    r->mode = GalaxyCollapse;
    r->cstart = galaxynow();
    r->hover = r->hovercore = -1;
    galaxysetcursor(0);
}

/* 开场中按键 / 点击: 场景时钟加速, GALAXYWARP 秒内走到驻留态 (一切都是时间的纯函数, 不会跳帧) */
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
static void
galaxyreturnstart(int tag, int star)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyStar *s;
    GalaxyCore *g;
    int i;

    if (r->mode == GalaxyOff || r->mode == GalaxyRest || r->mode == GalaxyReturn)
        return;
    galaxylogseg(galaxymodename[r->mode]);
    r->rkind = star >= 0 ? GalaxyPickStar : tag >= 0 ? GalaxyPickCore : GalaxyFlyHome;
    r->rstar = star;
    r->rcore = tag;
    if (r->rkind == GalaxyPickCore && tag >= 0 && tag < r->ntags)
        r->rcoresize = r->galaxies[tag].size;
    if (star >= 0) {
        s = &r->stars[star];
        r->tmon = s->mon;
        r->ttags = 1u << s->galaxy;
        r->twin = s->valid ? s->win : None;
        r->tshow = 1;
    } else if (tag >= 0) {
        r->tmon = r->savedmon;
        r->ttags = 1u << tag;
        r->twin = None;
        r->tshow = 0;
    }
    /* Super+Z: 结束后可见的窗口飞回原位置. 点击不使用这条轨迹. */
    for (i = 0; i < r->nstars; i++) {
        s = &r->stars[i];
        if (s->mon != r->tmon)
            s->back = s->shown;
        else
            s->back = (!s->hidden || i == star) && (s->global || (s->tags & r->ttags));
        s->rpos = s->pos;
        s->rorient = s->orient;
        s->rsize = s->size;
        s->rvis = s->vis;
        s->rtint = s->tint;
        s->rglow = s->glow;
        s->rbright = s->p.ok ? s->brightness : 1;
    }
    for (i = 0; i < r->ntags; i++) {
        g = &r->galaxies[i];
        g->nova = g->bridge = 0;    /* 驻留特效不带进回程 */
        g->rpos = g->pos;
        g->ralpha = g->alpha;
    }
    r->fulldesk = r->tmon == r->savedmon && r->ttags == r->savedtags && (star < 0 || !r->stars[star].hidden);
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
        XRenderComposite(dpy, PictOpSrc, r->live, None, r->wallpaper, 0, 0, 0, 0, 0, 0, r->w, r->h);
        XRenderComposite(dpy, PictOpSrc, r->wallpaper, None, r->overlaypic, 0, 0, 0, 0, 0, 0, r->w, r->h);
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

static double
galaxyfps(double now)
{
    GalaxyScene *r = &galaxyscene;

    if (r->mode != GalaxyOrbit)
        return GALAXYFPS;
    if (r->quiet || r->saver)
        return MIN(GALAXY_QUIETFPS, GALAXYORBITFPS);
    return now - r->lastinput > GALAXYIDLE ? GALAXYIDLEFPS : GALAXYORBITFPS;
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

static void
galaxytick(void)
{
    GalaxyScene *r = &galaxyscene;
    double now, dt, u = 0, begin, cost;
    CARD16 level;
    BOOL on;
    int star, core;

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
        if (u >= GALAXYCOLLAPSE) {
            galaxyfinish();
            return;
        }
        break;
    case GalaxyReturn:
        u = (now - r->rstart) / (r->rkind == GalaxyPickStar ? GALAXYPICK
                : r->rkind == GalaxyPickCore ? GALAXYCORE : GALAXYRETURN);
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
        if (r->rkind == GalaxyFlyHome)
            galaxyupdatereturn(u);
        else
            galaxyupdatepick(u);
    } else
        galaxyupdatescene(r->stage, r->motion, dt);
    r->phasecost[0] += galaxynow() - begin;
    galaxyrender();
    cost = galaxynow() - begin;
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
            else
                galaxywarp();
            break;
        case GalaxyOrbit:
            if (superz)
                galaxyreturnstart(-1, -1);
            else if (esc && (r->kqlen || (r->ksel >= 0 && galaxykeyactive())))
                galaxykeyclear();       /* 先清空过滤词和选中 */
            else if (esc && r->mouseawake)
                galaxymousesleep("Esc");
            else if (esc)
                galaxycollapsestart();
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
            r->lastinput = galaxynow();
            if (e->xbutton.button <= Button3)
                galaxywarp();
            break;
        case GalaxyOrbit:
            if (!r->mouseawake) {
                if (e->xbutton.button == Button1) {
                    r->mouseawake = 1;
                    r->mousevalid = 0;
                    r->lastinput = r->lastmouse = r->lastpointer = galaxynow();
                    galaxysetcursor(0);
                    if (r->log) {
                        fprintf(r->log, "galaxy mouse: wake\n");
                        fflush(r->log);
                    }
                }
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
                r->mousevalid = 1;
                galaxypick(r->mx, r->my, &star, &core);
                if (star >= 0)
                    galaxyreturnstart(-1, star);
                else if (core >= 0)
                    galaxyreturnstart(core, -1);
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
        if (r->mode == GalaxyOrbit && !r->mouseawake)
            return 1;
        r->lastinput = r->lastpointer = galaxynow();
        if (r->mode == GalaxyOrbit) {
            r->lastmouse = r->lastinput;
            r->mousevalid = !r->dragging;
        }
        r->lastkey = -1e9;      /* 鼠标一动, 悬停交还给鼠标 */
        r->mx = e->xmotion.x_root;
        r->my = e->xmotion.y_root;
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
            XRenderComposite(dpy, PictOpSrc, r->wallpaper, None, r->overlaypic, 0, 0, 0, 0, 0, 0, r->w, r->h);
        return 1;
    case DestroyNotify:
    case UnmapNotify:
        /* 窗口在动画中关闭: 星体标记失效, 缓存的截图继续显示到动画结束.
         * 经 root 的 SubstructureNotify 收到时 xany.window 是 root, 关闭的窗口在 xdestroywindow / xunmap.window */
        w = e->type == DestroyNotify ? e->xdestroywindow.window : e->xunmap.window;
        for (i = 0; i < r->nstars; i++)
            if (r->stars[i].win == w && r->stars[i].valid) {
                r->stars[i].valid = 0;
                r->stars[i].c = NULL;
                if (r->log)
                    fprintf(r->log, "star %d window 0x%lx gone during animation\n", i, r->stars[i].win), fflush(r->log);
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
    int i, count = 0, ev, er, tag, full, pass, dx, dy;
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
    r->tscale = count ? GALAXYINTRO : 1;   /* 没有窗口: 同样的视觉语言, 更短的开场 */
    r->starscale = MAX(.65, MIN(1.1, 1.15 - .012 * count));
    r->orbitscale = MAX(.7, MIN(1, 1.05 - .008 * count));
    r->glowscale = MAX(.55, MIN(1, 1.1 - .015 * count));
    r->trace = getenv("GALAXY_TRACE") != NULL;
    r->quiet = galaxyquiet();
    r->fxgap = GALAXY_FXGAP * (r->quiet ? 2.5 : 1);
    r->ndust = MAX(70, MIN(140, 140 - 2 * count)) / (r->quiet ? 2 : 1);
    r->ntrail = count > 24 ? 6 : GALAXYTRAIL;
    r->cam.fov = 62 * GALAXYPI / 180;
    r->cam.focal = r->vw * .5 / tan(r->cam.fov / 2);
    r->cam.near = r->cam.focal * .12;
    r->cam.far = r->cam.focal * 14;
    r->galaxies = calloc(r->ntags, sizeof *r->galaxies);
    r->stars = calloc(MAX(1, count), sizeof *r->stars);
    r->dust = calloc(r->ndust, sizeof *r->dust);
    r->items = calloc(r->ndust + r->ntags * (4 + GALAXYARCS * GALAXYRINGS) + count + GALAXYARCS * GALAXYLANES + 2, sizeof *r->items);
    r->tgpos = calloc(r->ntags, sizeof *r->tgpos);
    r->tgplane = calloc(r->ntags, sizeof *r->tgplane);
    r->tpts = calloc((count + r->ntags) * (r->ntrail + 1), sizeof *r->tpts);
    r->rpts = calloc(r->ntags * GALAXYRINGS * (GALAXYSEG + 1), sizeof *r->rpts);
    r->streakpts = calloc(r->ntags * (GALAXYSTREAK + 1), sizeof *r->streakpts);
    r->streakz = calloc(r->ntags * 3, sizeof *r->streakz);
    r->popord = calloc(r->ntags, sizeof *r->popord);
    r->gaps = calloc(GALAXYGAPS, sizeof *r->gaps);
    r->bandtw = (r->w + GALAXYBTILE - 1) / GALAXYBTILE;
    r->bandth = (r->h + GALAXYBTILE - 1) / GALAXYBTILE;
    r->bandbuf = calloc((size_t)r->w * r->h, 1);
    r->banddirty = calloc(r->bandtw * r->bandth, 1);
    if (!r->bandbuf || !r->banddirty
            || !(r->bandimg = XCreateImage(dpy, DefaultVisual(dpy, screen), 8, ZPixmap, 0, (char *)r->bandbuf, r->w, r->h, 8, r->w)))
        goto fail;
    r->bandpix = XCreatePixmap(dpy, root, r->w, r->h, 8);
    r->bandpic = XRenderCreatePicture(dpy, r->bandpix, r->a8, 0, NULL);
    r->bandgc = XCreateGC(dpy, r->bandpix, 0, NULL);
    if (!r->galaxies || !r->stars || !r->dust || !r->items || !r->tgpos || !r->tgplane || !r->tpts || !r->rpts
            || !r->streakpts || !r->streakz || !r->popord || !r->gaps || !r->argb || !r->a8 || !r->a1)
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
    if (r->titlefont)
        r->titledraw = XftDrawCreate(dpy, r->backpix, DefaultVisual(dpy, screen), DefaultColormap(dpy, screen));
    if (r->titledraw) {
        XRenderColor white = {0xe900, 0xf600, 0xffff, 0xffff};
        r->titlecolorok = XftColorAllocValue(dpy, DefaultVisual(dpy, screen),
                DefaultColormap(dpy, screen), &white, &r->titlecolor);
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
    galaxybuildsprites();
    galaxybuildvignette();

    i = 0;
    for (m = mons; m; m = m->next)
        for (c = m->clients; c; c = c->next) {
            if (c->isscratchpad)
                continue;
            s = &r->stars[i++];
            s->win = c->win;
            s->c = c;
            s->mon = m;
            s->tags = c->tags & TAGMASK;
            s->valid = 1;
            snprintf(s->title, sizeof s->title, "%.63s", c->name);
            tag = c->isglobal || !(c->tags & TAGMASK) ? (int)cur : __builtin_ctz(c->tags & TAGMASK);
            s->galaxy = MIN(tag, r->ntags - 1);
            s->hidden = HIDDEN(c);
            s->global = c->isglobal;
            s->shown = ISVISIBLE(c) && !s->hidden;
            s->current = s->shown;   /* 任何显示器上正显示的窗口都从原位置起飞 (多显示器时不会在另一块屏上凭空消失) */
            s->focused = c == selmon->sel;
            s->w = MAX(1, c->w);
            s->h = MAX(1, c->h);
            s->kmatch = 1;
            s->home = galaxyv(c->x + c->bw + c->w * .5 - r->vx - r->vw * .5, c->y + c->bw + c->h * .5 - r->vy - r->vh * .5, 0);
            r->galaxies[s->galaxy].nstars++;
        }
    XGrabServer(dpy);
    galaxycapturebackground();
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
    clock_gettime(CLOCK_MONOTONIC, &t1);
    galaxyprobeprojective();
    galaxybuildcores();
    galaxybuildorbits();
    galaxybuilddust();
    if (XQueryPointer(dpy, root, &dw, &dw, &dx, &dy, &ev, &er, &mask)) {
        r->mx = dx;
        r->my = dy;
        r->tyaw = ((r->mx - r->vx) / r->vw - .5) * 2 * 4;
        r->tpitch = ((r->my - r->vy) / r->vh - .5) * 2 * 2.5;
    }

    /* 遮罩的背景就是刚截的桌面, 映射瞬间不会闪黑 */
    wa.override_redirect = True;
    wa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
    wa.background_pixmap = r->desktoppix;
    r->overlay = XCreateWindow(dpy, root, 0, 0, r->w, r->h, 0, DefaultDepth(dpy, screen), InputOutput,
            DefaultVisual(dpy, screen), CWOverrideRedirect | CWEventMask | CWBackPixmap, &wa);
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
    r->overlaypic = XRenderCreatePicture(dpy, r->overlay, XRenderFindVisualFormat(dpy, DefaultVisual(dpy, screen)), 0, NULL);
    if (!r->overlaypic)
        goto fail;
    XMapRaised(dpy, r->overlay);
    r->mode = GalaxyIntro;
    if (XGrabKeyboard(dpy, r->overlay, False, GrabModeAsync, GrabModeAsync, CurrentTime) != GrabSuccess)
        goto fail;
    r->grabkbd = 1;
    if (XGrabPointer(dpy, r->overlay, False, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                GrabModeAsync, GrabModeAsync, None, None, CurrentTime) != GrabSuccess)
        goto fail;
    r->grabptr = 1;
    galaxylogstart();
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

/* bin/galaxysaver.py 在 root 上设置 _DWM_GALAXY=saver: 无操作到时间, 以屏保模式进入星系 (星系已在运行时忽略) */
static void
galaxyproperty(XPropertyEvent *ev)
{
    static Atom atom;
    GalaxyScene *r = &galaxyscene;
    char value[32] = "";
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
