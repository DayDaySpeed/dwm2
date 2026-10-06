/* Super+A 窗口总览 (overview): 当前屏的所有窗口 (含其他 tag 和隐藏的) 排成网格, 点选进入.
 * 独立的 2D 特效: 自己的遮罩、截图、渲染和事件处理, 由 dwm.c 在 config.h 之后 #include.
 *   打开: 当前桌面的窗口从原位置滑进格子, 其他 tag / 隐藏的窗口在格子里淡入, 背景压暗并稍稍后退
 *   停留: 悬停 / 键盘选中的卡片浮起并亮起光框, 卡片下方是标题; 打字按标题过滤; 截图约 2 秒刷新一轮
 *   进入 (排开): 选中的卡片原地放大到真实窗口, 周围的卡片被它挤开、由近及远滑出屏幕, 同 tag 的窗口滑到各自位置
 *   关闭: 当前桌面的窗口滑回原位, 其余卡片缩小淡出
 * 遮罩只盖发起的那块屏, 其他屏照常显示. 动画期间不移动 / 映射 / 聚焦任何真实窗口; 选中后才在遮罩下切 tag. */

#include <ctype.h>

#define OVERVIEWFPS      120.0
#define OVERVIEWOPEN     .42       /* 打开: 窗口滑进网格 */
#define OVERVIEWLAND     .50       /* 进入: 选中的卡片放大落位, 周围的被挤开 */
#define OVERVIEWCLOSE    .38       /* 关闭: 滑回原位 */
#define OVERVIEWMIPS     6
#define OVERVIEWALPHAS   64
#define OVERVIEWBARS     8
#define OVERVIEWHUDGAPS  64
#define OVERVIEWKEYIDLE  8.0       /* 键盘最近这么多秒内操作过, 高亮跟随键盘选中而不是鼠标悬停 */
#define OVERVIEWREFRESH  2.0       /* 停留时每张截图的刷新间隔 */
#define OVERVIEWBUDGET   (320L << 20)   /* 全尺寸截图的显存预算, 超出的截半尺寸 */

enum { OverviewOff, OverviewOpen, OverviewIdle, OverviewLand, OverviewClose };
static const char *overviewmodename[] = { "off", "open", "idle", "land", "close" };

typedef struct {
    Window win;
    Client *c;                  /* 窗口关闭后置空 */
    char title[64];
    int valid, hidden, current, focused, urgent, snap, base, kmatch, land, back, hit;
    int w, h;                   /* 截图时的窗口大小 */
    double hx, hy;              /* 真实窗口的中心 (root 坐标) */
    double gx, gy, gs;          /* 网格里的中心和缩放 */
    double lx, ly, lw, lh;      /* 落点: 目标 tag 里真实窗口的中心和大小 */
    double x, y, sw, sh;        /* 当前帧: 中心和屏幕上的宽高 */
    double alpha, vis, light, hover, z;
    double fx, fy, fw, fh, fvis, flight, fhover;   /* 进入 / 关闭开始时冻结的状态 */
    double dx, dy, push, delay; /* 被挤开: 方向、距离和出发时刻 */
    double died, refreshat;
    double bx0, by0, bx1, by1;  /* 屏幕上的范围 (点击和光框), root 坐标 */
    Pixmap mippix[OVERVIEWMIPS];
    Picture mip[OVERVIEWMIPS];
    int mipw[OVERVIEWMIPS], miph[OVERVIEWMIPS];
} OverviewCard;

typedef struct {
    int mode, n, grabkbd, grabptr, rendermajor, handon, lead, fulldesk;
    OverviewCard *cards;
    int *order;                 /* 绘制顺序 (从下到上) */
    Monitor *mon, *tmon;
    unsigned int savedtags, ttags;
    Window savedwin, twin;
    int vx, vy, vw, vh;         /* 遮罩覆盖的屏 (root 坐标) */
    Window overlay, wallwin;
    Cursor hand;
    XRenderPictFormat *argb;
    Picture overlaypic, back, desktop, wallpaper, halo, vignette;
    Pixmap backpix, desktoppix, wallpix, halopix, vignettepix;
    Picture white[OVERVIEWALPHAS], black[OVERVIEWALPHAS];
    XftFont *titlefont, *queryfont;
    XftDraw *draw;
    XftColor textcolor;
    int textcolorok;
    /* 状态栏 / 托盘: 进入别的 tag 后实时截取的样子, 最后淡入 */
    Picture bars[OVERVIEWBARS];
    int barx[OVERVIEWBARS], bary[OVERVIEWBARS], barw[OVERVIEWBARS], barh[OVERVIEWBARS], nbars;
    /* 时间 (overviewnow, 秒) / 鼠标 / 键盘 */
    struct timespec start;
    double last, rstart, lastkey;
    double mx, my;
    int mousevalid, hover, ksel, kqlen, kn;
    char kq[64];
    /* 背景: 亮度 / 暗角 / 开始时截的桌面 / 结束时交叉淡入的桌面 / 状态栏 */
    double bright, vign, desk, deskover, bar, rbright, rvign, rdesk;
    /* 统计 */
    double gaps[OVERVIEWHUDGAPS], segstart, seggapmax, segrender, segrendermax, lastcost;
    int ngaps, frames, segframes, refreshi, refreshn;
    double refreshsum;
    unsigned long errors;
    FILE *log;
} OverviewScene;

static OverviewScene overviewscene;
static int overviewhud;         /* F12 帧率面板, 进程内保持 */

/* ---------- 小工具 ---------- */

static double overviewclamp(double x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static double overviewphase(double t, double a, double b) { return overviewclamp((t - a) / (b - a)); }
static double overviewsmooth(double x) { x = overviewclamp(x); return x * x * (3 - 2 * x); }
static double overviewmix(double a, double b, double t) { return a + (b - a) * t; }
static double overviewoutcubic(double x) { x = 1 - overviewclamp(x); return 1 - x * x * x; }

static double
overviewinoutcubic(double x)
{
    x = overviewclamp(x);
    return x < .5 ? 4 * x * x * x : 1 - pow(-2 * x + 2, 3) / 2;
}

/* 滑行: 速度 12x(1-x)^2, 从 0 平滑加速, 1/3 处最快, 到终点平滑减到 0 (前快后柔, 没有长尾巴) */
static double
overviewglide(double x)
{
    x = overviewclamp(x);
    return 1 - (1 - x) * (1 - x) * (1 - x) * (1 + 3 * x);
}

/* 指数平滑: 与帧率无关 */
static double
overviewfollow(double cur, double target, double dt, double tau)
{
    return cur + (target - cur) * (1 - exp(-dt / tau));
}

static double
overviewnow(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - overviewscene.start.tv_sec) + (t.tv_nsec - overviewscene.start.tv_nsec) / 1e9;
}

static int
overviewactive(void)
{
    return overviewscene.mode != OverviewOff;
}

/* 截图 / 合成过程中窗口可能随时消失; dwm 的 xerror 遇到 Render 错误会直接退出整个会话 */
static int
overviewxerror(Display *d, XErrorEvent *ee)
{
    if (ee->request_code == overviewscene.rendermajor || ee->error_code == BadWindow
            || ee->error_code == BadDrawable || ee->error_code == BadPixmap || ee->error_code == BadMatch
            || ee->request_code == X_CreatePixmap || ee->request_code == X_FreePixmap
            || ee->request_code == X_PutImage || ee->request_code == X_CreateGC
            || ee->request_code == X_GetWindowAttributes || ee->request_code == X_QueryTree
            || ee->request_code == X_GrabPointer || ee->request_code == X_GrabKeyboard) {
        overviewscene.errors++;
        return 0;
    }
    return xerror(d, ee);
}

static void
overviewaffine(Picture p, double sx, double sy, double tx, double ty)
{
    XTransform tr = {{
        {XDoubleToFixed(sx), 0, XDoubleToFixed(tx)},
        {0, XDoubleToFixed(sy), XDoubleToFixed(ty)},
        {0, 0, XDoubleToFixed(1)}}};
    XRenderSetPictureTransform(dpy, p, &tr);
}

static Picture
overviewargbpic(int w, int h, Pixmap *pix)
{
    *pix = XCreatePixmap(dpy, root, MAX(1, w), MAX(1, h), 32);
    return XRenderCreatePicture(dpy, *pix, overviewscene.argb, 0, NULL);
}

static Picture
overviewopaquepic(int w, int h, Pixmap *pix)
{
    *pix = XCreatePixmap(dpy, root, MAX(1, w), MAX(1, h), DefaultDepth(dpy, screen));
    return XRenderCreatePicture(dpy, *pix, XRenderFindVisualFormat(dpy, DefaultVisual(dpy, screen)), 0, NULL);
}

/* 客户端算好的 ARGB (预乘) 图上传成 Picture; 会释放 data */
static Picture
overviewupload(int w, int h, unsigned int *data, Pixmap *pix)
{
    Picture p = overviewargbpic(w, h, pix);
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

static Picture
overviewwhite(double a)
{
    return overviewscene.white[(int)lround(overviewclamp(a) * (OVERVIEWALPHAS - 1))];
}

static Picture
overviewblack(double a)
{
    return overviewscene.black[(int)lround(overviewclamp(a) * (OVERVIEWALPHAS - 1))];
}

static void
overviewsetcursor(int on)
{
    OverviewScene *r = &overviewscene;

    if (on == r->handon || !r->overlay)
        return;
    r->handon = on;
    if (on && r->hand)
        XDefineCursor(dpy, r->overlay, r->hand);
    else
        XUndefineCursor(dpy, r->overlay);
}

/* ---------- 日志: ~/.cache/dwm-overview.log (测试显示各用一份) ---------- */

static void
overviewlogstart(void)
{
    OverviewScene *r = &overviewscene;
    const char *home = getenv("HOME");
    char path[512];
    OverviewCard *s;
    int i;

    if (!strcmp(DisplayString(dpy), ":0"))
        snprintf(path, sizeof path, "%s/.cache/dwm-overview.log", home ? home : "/tmp");
    else
        snprintf(path, sizeof path, "%s/.cache/dwm-overview%s.log", home ? home : "/tmp", DisplayString(dpy));
    if (!(r->log = fopen(path, "w")))
        return;
    fprintf(r->log, "overview start: monitor %dx%d+%d+%d windows %d\n", r->vw, r->vh, r->vx, r->vy, r->n);
    for (i = 0; i < r->n; i++) {
        s = &r->cards[i];
        fprintf(r->log, "card %2d win 0x%08lx %s%s%s snapshot %dx%d mip%d grid %.2f \"%s\"\n", i, s->win,
                s->current ? "current " : "", s->hidden ? "hidden " : "", s->focused ? "focused " : "",
                s->snap ? s->mipw[s->base] : 0, s->snap ? s->miph[s->base] : 0, s->base, s->gs, s->title);
    }
    fflush(r->log);
}

/* 一段 (打开 / 停留 / 进入 / 关闭) 结束: 帧数、帧率、最慢一帧、渲染耗时 */
static void
overviewlogseg(void)
{
    OverviewScene *r = &overviewscene;
    double now = overviewnow(), t = now - r->segstart;

    if (r->log && r->segframes)
        fprintf(r->log, "overview %s: at %.2fs frames %d time %.3fs avg %.1f fps min %.1f fps render avg %.2fms max %.2fms xerrors %lu\n",
                overviewmodename[r->mode], now, r->segframes, t, t > 0 ? r->segframes / t : 0,
                r->seggapmax > 0 ? 1 / r->seggapmax : 0, 1000 * r->segrender / r->segframes, 1000 * r->segrendermax, r->errors);
    if (r->log)
        fflush(r->log);
    r->segstart = now;
    r->segframes = 0;
    r->seggapmax = r->segrender = r->segrendermax = 0;
}

/* ---------- 截图 ---------- */

/* src 缩小到 dst (每级 2 倍, 双线性正好是 2x2 平均) */
static void
overviewshrink(Picture src, int sw0, int sh0, Picture dst, int dw, int dh)
{
    overviewaffine(src, (double)sw0 / dw, (double)sh0 / dh, 0, 0);
    XRenderSetPictureFilter(dpy, src, FilterBilinear, NULL, 0);
    XRenderComposite(dpy, PictOpSrc, src, None, dst, 0, 0, 0, 0, 0, 0, dw, dh);
}

static void
overviewbuildmips(OverviewCard *s)
{
    int l;

    for (l = s->base + 1; l < OVERVIEWMIPS; l++) {
        if (s->mipw[l - 1] < 16 || s->miph[l - 1] < 16)
            break;
        s->mipw[l] = MAX(1, s->mipw[l - 1] / 2);
        s->miph[l] = MAX(1, s->miph[l - 1] / 2);
        s->mip[l] = overviewargbpic(s->mipw[l], s->miph[l], &s->mippix[l]);
        overviewshrink(s->mip[l - 1], s->mipw[l - 1], s->miph[l - 1], s->mip[l], s->mipw[l], s->miph[l]);
    }
    for (l = s->base; l < OVERVIEWMIPS; l++)
        if (s->mip[l]) {
            overviewaffine(s->mip[l], 1, 1, 0, 0);
            XRenderSetPictureFilter(dpy, s->mip[l], FilterBilinear, NULL, 0);
        }
}

static void
overviewfreemips(OverviewCard *s)
{
    int l;

    for (l = 0; l < OVERVIEWMIPS; l++) {
        if (s->mip[l])
            XRenderFreePicture(dpy, s->mip[l]);
        if (s->mippix[l])
            XFreePixmap(dpy, s->mippix[l]);
        s->mip[l] = 0;
        s->mippix[l] = 0;
    }
}

/* 截一张窗口: 可见窗口直接从窗口取 (合成器重定向的窗口在其他 tag 的屏外位置也有内容),
 * 隐藏窗口用 dwm 隐藏时缓存的图. full 为 0 时截半尺寸 */
static void
overviewcapture(OverviewCard *s, Client *c, int full)
{
    OverviewScene *r = &overviewscene;
    XRenderPictureAttributes pa = {.subwindow_mode = IncludeInferiors};
    XRenderColor clear = {0, 0, 0, 0};
    XRenderPictFormat *fmt;
    XWindowAttributes wa;
    XImage *img = c->preview.hidden_image;
    Picture src = 0;
    Pixmap tmp = 0;
    int sw0, sh0;
    GC gc;

    if (s->hidden) {
        if (!img)
            return;     /* 绝不为了截图映射隐藏的真实窗口 */
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
    s->mip[s->base] = overviewargbpic(s->mipw[s->base], s->miph[s->base], &s->mippix[s->base]);
    XRenderFillRectangle(dpy, PictOpSrc, s->mip[s->base], &clear, 0, 0, s->mipw[s->base], s->miph[s->base]);
    if (!full) {
        overviewaffine(src, (double)sw0 / s->mipw[1], (double)sh0 / s->miph[1], 0, 0);
        XRenderSetPictureFilter(dpy, src, FilterBilinear, NULL, 0);
    }
    XRenderComposite(dpy, PictOpOver, src, None, s->mip[s->base], 0, 0, 0, 0, 0, 0, s->mipw[s->base], s->miph[s->base]);
    overviewbuildmips(s);
    s->snap = 1;
done:
    if (src)
        XRenderFreePicture(dpy, src);
    if (tmp)
        XFreePixmap(dpy, tmp);
}

/* 停留时刷新一张截图: 窗口当前内容合成进已有的 mip, 再逐级缩小; 窗口大小变了就整套重建 (格子里的大小不变).
 * 只刷映射着的窗口 (隐藏的跳过). 返回是否刷新了 */
static int
overviewrefresh(OverviewCard *s)
{
    XRenderPictureAttributes pa = {.subwindow_mode = IncludeInferiors};
    XRenderColor clear = {0, 0, 0, 0};
    XRenderPictFormat *fmt;
    XWindowAttributes wa;
    Client *c = s->valid ? wintoclient(s->win) : NULL;
    Picture src;
    int l, full = s->base == 0;

    if (!c || HIDDEN(c) || !s->snap || s->died || !XGetWindowAttributes(dpy, s->win, &wa) || wa.map_state != IsViewable
            || !(fmt = XRenderFindVisualFormat(dpy, wa.visual)))
        return 0;
    if (wa.width != s->w || wa.height != s->h) {
        s->gs *= MIN((double)s->w / MAX(1, wa.width), (double)s->h / MAX(1, wa.height));
        overviewfreemips(s);
        s->snap = 0;
        s->w = MAX(1, wa.width);
        s->h = MAX(1, wa.height);
        s->hidden = 0;
        overviewcapture(s, c, full);
        return 1;
    }
    if (!(src = XRenderCreatePicture(dpy, s->win, fmt, CPSubwindowMode, &pa)))
        return 0;
    if (!full) {
        overviewaffine(src, (double)s->w / s->mipw[1], (double)s->h / s->miph[1], 0, 0);
        XRenderSetPictureFilter(dpy, src, FilterBilinear, NULL, 0);
    }
    XRenderFillRectangle(dpy, PictOpSrc, s->mip[s->base], &clear, 0, 0, s->mipw[s->base], s->miph[s->base]);
    XRenderComposite(dpy, PictOpOver, src, None, s->mip[s->base], 0, 0, 0, 0, 0, 0, s->mipw[s->base], s->miph[s->base]);
    XRenderFreePicture(dpy, src);
    for (l = s->base + 1; l < OVERVIEWMIPS && s->mip[l]; l++)
        overviewshrink(s->mip[l - 1], s->mipw[l - 1], s->miph[l - 1], s->mip[l], s->mipw[l], s->miph[l]);
    overviewaffine(s->mip[s->base], 1, 1, 0, 0);
    return 1;
}

/* 每帧最多刷一张, 轮流来; 只刷看得清的卡片 */
static void
overviewrefreshstep(double now)
{
    OverviewScene *r = &overviewscene;
    OverviewCard *s;
    double t0;
    int k, i;

    if (r->mode != OverviewIdle)
        return;
    for (k = 0; k < r->n; k++) {
        i = (r->refreshi + k) % r->n;
        s = &r->cards[i];
        if (now - s->refreshat < OVERVIEWREFRESH || s->vis < .3)
            continue;
        r->refreshi = i + 1;
        s->refreshat = now;
        t0 = overviewnow();
        if (overviewrefresh(s)) {
            r->refreshn++;
            r->refreshsum += overviewnow() - t0;
        }
        return;
    }
}

/* 动态壁纸 (linux-wallpaperengine / xwinwrap 等) 是最底层的全屏非托管窗口 */
static Window
overviewfindwallpaper(void)
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
                && wa.x <= 0 && wa.y <= 0 && wa.x + wa.width >= sw && wa.y + wa.height >= sh) {
            found = wins[i];
            break;
        }
    }
    if (wins)
        XFree(wins);
    return found;
}

/* 这块屏现在的样子 (第一帧与真实桌面完全一致) 和壁纸 (背景), 都是屏内坐标 */
static void
overviewcapturebackground(void)
{
    OverviewScene *r = &overviewscene;
    XRenderPictureAttributes pa = {.subwindow_mode = IncludeInferiors};
    XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy, DefaultVisual(dpy, screen));
    XRenderColor dark = {2800, 3400, 5000, 65535};
    XWindowAttributes wa;
    Atom type;
    int format;
    unsigned long items, after;
    unsigned char *data = NULL;
    Pixmap source = None;
    Picture p;

    if ((p = XRenderCreatePicture(dpy, root, fmt, CPSubwindowMode, &pa))) {
        XRenderComposite(dpy, PictOpSrc, p, None, r->desktop, r->vx, r->vy, 0, 0, 0, 0, r->vw, r->vh);
        XRenderFreePicture(dpy, p);
    }
    XRenderFillRectangle(dpy, PictOpSrc, r->wallpaper, &dark, 0, 0, r->vw, r->vh);
    if ((r->wallwin = overviewfindwallpaper()) && XGetWindowAttributes(dpy, r->wallwin, &wa)
            && (fmt = XRenderFindVisualFormat(dpy, wa.visual))
            && (p = XRenderCreatePicture(dpy, r->wallwin, fmt, CPSubwindowMode, &pa))) {
        XRenderComposite(dpy, PictOpSrc, p, None, r->wallpaper, r->vx - wa.x, r->vy - wa.y, 0, 0, 0, 0, r->vw, r->vh);
        XRenderFreePicture(dpy, p);
        return;
    }
    if (XGetWindowProperty(dpy, root, XInternAtom(dpy, "_XROOTPMAP_ID", False), 0, 1, False, XA_PIXMAP,
            &type, &format, &items, &after, &data) == Success && data && items)
        source = *(Pixmap *)data;
    if (data)
        XFree(data);
    if (source && (p = XRenderCreatePicture(dpy, source, XRenderFindVisualFormat(dpy, DefaultVisual(dpy, screen)), 0, NULL))) {
        XRenderComposite(dpy, PictOpSrc, p, None, r->wallpaper, r->vx, r->vy, 0, 0, 0, 0, r->vw, r->vh);
        XRenderFreePicture(dpy, p);
    }
}

/* 切换后的状态栏 / 托盘: 直接从窗口取 (合成器重定向的窗口在遮罩下面也有内容), 最后淡入 */
static void
overviewcapturebars(void)
{
    OverviewScene *r = &overviewscene;
    XRenderPictureAttributes pa = {.subwindow_mode = IncludeInferiors};
    XWindowAttributes wa;
    XRenderPictFormat *fmt;
    Window wins[OVERVIEWBARS];
    Monitor *m;
    int i, n = 0;

    for (i = 0; i < r->nbars; i++)
        XRenderFreePicture(dpy, r->bars[i]);
    r->nbars = 0;
    for (m = mons; m && n < OVERVIEWBARS - 1; m = m->next)
        if (m->showbar && m->barwin)
            wins[n++] = m->barwin;
    if (systray && systray->win && n < OVERVIEWBARS)
        wins[n++] = systray->win;
    for (i = 0; i < n; i++)
        if (XGetWindowAttributes(dpy, wins[i], &wa) && wa.map_state == IsViewable
                && (fmt = XRenderFindVisualFormat(dpy, wa.visual))
                && (r->bars[r->nbars] = XRenderCreatePicture(dpy, wins[i], fmt, CPSubwindowMode, &pa))) {
            r->barx[r->nbars] = wa.x + wa.border_width;
            r->bary[r->nbars] = wa.y + wa.border_width;
            r->barw[r->nbars] = wa.width;
            r->barh[r->nbars++] = wa.height;
        }
}

/* 暗角 (边缘渐暗) 和紧急窗口的红色光晕: 小图, 绘制时拉伸 */
static void
overviewbuildsprites(void)
{
    OverviewScene *r = &overviewscene;
    unsigned int *data;
    double x, y, d, a;
    int w = 64, h = 36, n = 64, i, j;

    if ((data = malloc(w * h * 4))) {
        for (j = 0; j < h; j++)
            for (i = 0; i < w; i++) {
                x = (i + .5) / w * 2 - 1;
                y = (j + .5) / h * 2 - 1;
                a = .85 * overviewsmooth((sqrt(x * x + y * y) - .35) / .8);
                data[j * w + i] = (unsigned int)(a * 255 + .5) << 24;
            }
        r->vignette = overviewupload(w, h, data, &r->vignettepix);
        overviewaffine(r->vignette, (double)w / r->vw, (double)h / r->vh, 0, 0);
    }
    if ((data = malloc(n * n * 4))) {
        for (j = 0; j < n; j++)
            for (i = 0; i < n; i++) {
                x = (i + .5) / n * 2 - 1;
                y = (j + .5) / n * 2 - 1;
                d = sqrt(x * x + y * y);
                a = (.55 * exp(-d * d / .0288) + .3 * exp(-d * d / .1568) + .15 * exp(-d * d / .5))
                    * (1 - overviewsmooth((d - .75) / .25));
                a = overviewclamp(a);
                data[j * n + i] = (unsigned int)(a * 255 + .5) << 24 | (unsigned int)(a * 255 + .5) << 16
                    | (unsigned int)(a * .32 * 255 + .5) << 8 | (unsigned int)(a * .24 * 255 + .5);
            }
        r->halo = overviewupload(n, n, data, &r->halopix);
    }
}

/* ---------- 网格 ---------- */

/* 标题放不下时按 UTF-8 字符截断并加省略号 */
static void
overviewfittext(XftFont *font, char *text, size_t size, int maxw)
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

/* 列数取能放下的最小正方形, 每格按窗口比例缩放 (不放大), 各行居中, 标题在卡片下方 */
static void
overviewlayout(void)
{
    OverviewScene *r = &overviewscene;
    Monitor *m = r->mon;
    OverviewCard *s;
    int n = r->n, cols, rows, row, i, j, k;
    double outer = .045 * r->vw, gap = .02 * r->vw, title = (r->titlefont ? r->titlefont->height : 16) + 16;
    double cw, ch, x, y, roww, rowh, total = 0, rowh_[64] = {0};

    if (n <= 0)
        return;
    for (cols = 1; cols * cols < n; cols++);
    rows = (cols - 1) * cols >= n ? cols - 1 : cols;
    cw = (m->ww - 2 * outer - (cols - 1) * gap) / cols;
    ch = (m->wh - 2 * outer - (rows - 1) * gap) / rows - title;
    for (i = 0; i < n; i++) {
        s = &r->cards[i];
        s->gs = MIN(1, MIN(cw / s->w, ch / s->h));
    }
    for (row = 0; row < rows && row < 64; row++) {
        for (j = 0, rowh = 0; j < cols && row * cols + j < n; j++)
            rowh = MAX(rowh, r->cards[row * cols + j].h * r->cards[row * cols + j].gs);
        rowh_[row] = rowh + title;
        total += rowh_[row] + (row ? gap : 0);
    }
    y = m->wy + (m->wh - total) * .5;
    for (row = 0; row < rows && row < 64; row++) {
        for (j = 0, roww = 0; j < cols && row * cols + j < n; j++)
            roww += r->cards[row * cols + j].w * r->cards[row * cols + j].gs + (j ? gap : 0);
        x = m->wx + (m->ww - roww) * .5;
        for (j = 0; j < cols && (k = row * cols + j) < n; j++) {
            s = &r->cards[k];
            s->gx = x + s->w * s->gs * .5;
            s->gy = y + (rowh_[row] - title) * .5;
            x += s->w * s->gs + gap;
            overviewfittext(r->titlefont, s->title, sizeof s->title, (int)MAX(160, s->w * s->gs));
        }
        y += rowh_[row] + gap;
    }
}

/* ---------- 键盘: 方向键选卡片, 打字按标题过滤, Tab 在匹配项之间切换 ---------- */

static int
overviewkeyactive(void)
{
    OverviewScene *r = &overviewscene;

    return r->kqlen > 0 || overviewnow() - r->lastkey < OVERVIEWKEYIDLE;
}

/* 可以被选中的卡片: 看得见而且匹配过滤词 */
static int
overviewkeyok(int i)
{
    OverviewCard *s = &overviewscene.cards[i];

    return s->alpha > .1 && s->kmatch && !s->died;
}

/* 当前高亮的卡片: 键盘最近操作过就用键盘选中的, 否则用鼠标悬停的 */
static int
overviewsel(void)
{
    OverviewScene *r = &overviewscene;

    if (overviewkeyactive() && r->ksel >= 0 && r->ksel < r->n && overviewkeyok(r->ksel))
        return r->ksel;
    return r->hover >= 0 && r->hover < r->n ? r->hover : -1;
}

/* 标题里是否含有过滤词 (ASCII 不区分大小写) */
static int
overviewkeymatch(const char *title, const char *q)
{
    size_t i, j, n = strlen(q), m = strlen(title);

    for (i = 0; n && i + n <= m; i++) {
        for (j = 0; j < n && tolower((unsigned char)title[i + j]) == tolower((unsigned char)q[j]); j++);
        if (j == n)
            return 1;
    }
    return !n;
}

/* 没有选中时的起点: 优先当前焦点窗口, 否则离屏幕中心最近的 */
static int
overviewkeystart(void)
{
    OverviewScene *r = &overviewscene;
    double best = 1e18, d;
    int i, pick = -1;

    for (i = 0; i < r->n; i++) {
        if (!overviewkeyok(i))
            continue;
        if (r->cards[i].focused && !r->kqlen)
            return i;
        d = hypot(r->cards[i].gx - r->vx - r->vw * .5, r->cards[i].gy - r->vy - r->vh * .5);
        if (d < best) {
            best = d;
            pick = i;
        }
    }
    return pick;
}

static void
overviewkeyfilter(void)
{
    OverviewScene *r = &overviewscene;
    int i;

    r->kq[r->kqlen] = 0;
    for (i = r->kn = 0; i < r->n; i++)
        r->kn += r->cards[i].kmatch = overviewkeymatch(r->cards[i].title, r->kq);
    if (r->ksel < 0 || r->ksel >= r->n || !overviewkeyok(r->ksel))
        r->ksel = overviewkeystart();
}

static void
overviewkeyclear(void)
{
    OverviewScene *r = &overviewscene;

    r->kqlen = 0;
    r->ksel = -1;
    r->lastkey = -1e9;
    overviewkeyfilter();
    r->ksel = -1;
}

/* 方向键: 选中该方向 60° 锥形内最近的卡片 (距离 + 偏离方向的惩罚), 按网格位置算 */
static void
overviewkeymove(double dx, double dy)
{
    OverviewScene *r = &overviewscene;
    OverviewCard *a, *b;
    double best = 1e18, along, perp, cost;
    int i, pick = -1;

    if (r->ksel < 0 || !overviewkeyok(r->ksel)) {
        r->ksel = r->hover >= 0 && overviewkeyok(r->hover) ? r->hover : overviewkeystart();
        return;
    }
    a = &r->cards[r->ksel];
    for (i = 0; i < r->n; i++) {
        if (i == r->ksel || !overviewkeyok(i))
            continue;
        b = &r->cards[i];
        along = (b->gx - a->gx) * dx + (b->gy - a->gy) * dy;
        perp = fabs((b->gx - a->gx) * dy - (b->gy - a->gy) * dx);
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

/* Tab / Shift+Tab: 按阅读顺序 (从上到下, 从左到右) 在可选的卡片之间循环 */
static void
overviewkeycycle(int dir)
{
    OverviewScene *r = &overviewscene;
    double key, cur = -1e18, best = dir > 0 ? 1e18 : -1e18, wrap = dir > 0 ? 1e18 : -1e18;
    int i, pick = -1, first = -1;

    if (r->ksel >= 0 && overviewkeyok(r->ksel))
        cur = r->cards[r->ksel].gy * 16384 + r->cards[r->ksel].gx;
    for (i = 0; i < r->n; i++) {
        if (!overviewkeyok(i) || i == r->ksel)
            continue;
        key = r->cards[i].gy * 16384 + r->cards[i].gx;
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

/* ---------- 状态机 ---------- */

static void
overviewend(int restore)
{
    OverviewScene *r = &overviewscene;
    Monitor *m, *tmon = r->tmon;
    unsigned int ttags = r->ttags;
    Window twin = r->twin;
    Client *c;
    XEvent ev;
    int i;

    if (!r->mode)
        return;
    overviewlogseg();
    if (r->log)
        fprintf(r->log, "overview end: from %s restore %d tags 0x%x win 0x%lx xerrors %lu\n",
                overviewmodename[r->mode], restore, ttags, twin, r->errors);
    if (r->grabptr)
        XUngrabPointer(dpy, CurrentTime);
    if (r->grabkbd)
        XUngrabKeyboard(dpy, CurrentTime);
    XSetErrorHandler(overviewxerror);
    for (i = 0; r->cards && i < r->n; i++)
        overviewfreemips(&r->cards[i]);
    for (i = 0; i < r->nbars; i++)
        XRenderFreePicture(dpy, r->bars[i]);
    for (i = 0; i < OVERVIEWALPHAS; i++) {
        if (r->white[i])
            XRenderFreePicture(dpy, r->white[i]);
        if (r->black[i])
            XRenderFreePicture(dpy, r->black[i]);
    }
    if (r->draw)
        XftDrawDestroy(r->draw);
    if (r->textcolorok)
        XftColorFree(dpy, DefaultVisual(dpy, screen), DefaultColormap(dpy, screen), &r->textcolor);
    if (r->titlefont)
        XftFontClose(dpy, r->titlefont);
    if (r->queryfont)
        XftFontClose(dpy, r->queryfont);
    if (r->overlaypic) XRenderFreePicture(dpy, r->overlaypic);
    if (r->overlay) XDestroyWindow(dpy, r->overlay);
    if (r->hand) XFreeCursor(dpy, r->hand);
    if (r->back) XRenderFreePicture(dpy, r->back);
    if (r->backpix) XFreePixmap(dpy, r->backpix);
    if (r->desktop) XRenderFreePicture(dpy, r->desktop);
    if (r->desktoppix) XFreePixmap(dpy, r->desktoppix);
    if (r->wallpaper) XRenderFreePicture(dpy, r->wallpaper);
    if (r->wallpix) XFreePixmap(dpy, r->wallpix);
    if (r->halo) XRenderFreePicture(dpy, r->halo);
    if (r->halopix) XFreePixmap(dpy, r->halopix);
    if (r->vignette) XRenderFreePicture(dpy, r->vignette);
    if (r->vignettepix) XFreePixmap(dpy, r->vignettepix);
    if (r->log)
        fclose(r->log);
    free(r->cards);
    free(r->order);
    memset(r, 0, sizeof *r);
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
        noanim(400);    /* 窗口已经由动画放到了位, 切 tag 时 picom 不再加动画 */
        if (ttags && (m->tagset[m->seltags] & TAGMASK) != ttags)
            view(&(Arg){.ui = ttags});
    }
    c = twin ? wintoclient(twin) : NULL;
    focus(c && ISVISIBLE(c) && !HIDDEN(c) ? c : NULL);
}

static void
overviewcleanup(void)
{
    overviewend(0);
}

/* 进入窗口: 在遮罩下面先把 dwm 切到目标状态 (恢复隐藏窗口、切 tag、排好位置), 读出每个窗口真实的落点.
 * 抓住服务器做: 切换中途 (例如窗口被 raise 到遮罩之上) 不会被合成器画出来 */
static void
overviewlandprepare(Client *pick)
{
    OverviewScene *r = &overviewscene;
    OverviewCard *s;
    Monitor *m = pick->mon;
    Client *c;
    int i;

    noanim((long)(1000 * OVERVIEWLAND) + 600);
    XGrabServer(dpy);
    selmon = m;
    if (HIDDEN(pick))
        show(pick);
    if (!ISVISIBLE(pick) && (pick->tags & TAGMASK))
        view(&(Arg){.ui = pick->tags & TAGMASK});
    XRaiseWindow(dpy, r->overlay);
    XSync(dpy, False);
    XUngrabServer(dpy);
    r->tmon = m;
    r->ttags = m->tagset[m->seltags] & TAGMASK;
    r->twin = pick->win;
    for (i = 0; i < r->n; i++) {
        s = &r->cards[i];
        c = s->valid ? wintoclient(s->win) : NULL;
        s->land = c && c->mon == r->mon && ISVISIBLE(c) && !HIDDEN(c) && !s->died && s->snap;
        if (s->land) {
            s->lx = c->x + c->bw + c->w * .5;
            s->ly = c->y + c->bw + c->h * .5;
            s->lw = MAX(1, c->w);
            s->lh = MAX(1, c->h);
        }
    }
    overviewcapturebars();
    if (r->log)
        fprintf(r->log, "overview land: tags 0x%x win 0x%lx bars %d\n", r->ttags, r->twin, r->nbars), fflush(r->log);
}

/* 被挤开: 方向从选中的卡片指向这张卡片, 距离是沿这个方向完全离开屏幕所需的最短路程;
 * 离选中的卡片越近越先动 (像被它的边缘推开) */
static void
overviewpushsetup(void)
{
    OverviewScene *r = &overviewscene;
    OverviewCard *s, *L = r->lead >= 0 ? &r->cards[r->lead] : NULL;
    double ox = L ? L->fx : r->vx + r->vw * .5, oy = L ? L->fy : r->vy + r->vh * .5;
    double d, maxd = 1, tx, ty, hw, hh;
    int i;

    for (i = 0; i < r->n; i++) {
        s = &r->cards[i];
        if (s->land)
            continue;
        s->dx = s->fx - ox;
        s->dy = s->fy - oy;
        if (hypot(s->dx, s->dy) < 1) {     /* 与选中的卡片重合: 改为离开屏幕中心, 再不行就向上 */
            s->dx = s->fx - (r->vx + r->vw * .5);
            s->dy = s->fy - (r->vy + r->vh * .5);
        }
        if ((d = hypot(s->dx, s->dy)) < 1) {
            s->dx = 0;
            s->dy = -1;
            d = 0;
        } else {
            s->dx /= d;
            s->dy /= d;
        }
        s->delay = hypot(s->fx - ox, s->fy - oy);
        maxd = MAX(maxd, s->delay);
        hw = s->fw * .5;
        hh = s->fh * .5;
        tx = s->dx > 1e-6 ? (r->vx + r->vw - (s->fx - hw)) / s->dx
            : s->dx < -1e-6 ? (s->fx + hw - r->vx) / -s->dx : 1e9;
        ty = s->dy > 1e-6 ? (r->vy + r->vh - (s->fy - hh)) / s->dy
            : s->dy < -1e-6 ? (s->fy + hh - r->vy) / -s->dy : 1e9;
        s->push = MAX(0, MIN(tx, ty)) + 40;
    }
    for (i = 0; i < r->n; i++)
        if (!r->cards[i].land)
            r->cards[i].delay = .22 * r->cards[i].delay / maxd;
}

/* 选中 pick 进入 (pick < 0 或窗口已关闭时关闭总览, 回到原来的样子) */
static void
overviewleave(int pick)
{
    OverviewScene *r = &overviewscene;
    OverviewCard *s;
    Client *c = pick >= 0 && pick < r->n && r->cards[pick].valid ? wintoclient(r->cards[pick].win) : NULL;
    int i;

    if (r->mode != OverviewOpen && r->mode != OverviewIdle)
        return;
    overviewlogseg();
    for (i = 0; i < r->n; i++) {
        s = &r->cards[i];
        s->fx = s->x;
        s->fy = s->y;
        s->fw = s->sw;
        s->fh = s->sh;
        s->fvis = s->vis;
        s->flight = s->light;
        s->fhover = s->hover;
        s->land = 0;
        s->back = s->current && s->valid && !s->died && s->snap;
    }
    r->rbright = r->bright;
    r->rvign = r->vign;
    r->rdesk = r->desk;
    if (c) {
        overviewlandprepare(c);
        r->lead = r->cards[pick].land ? pick : -1;
        r->mode = OverviewLand;
        overviewpushsetup();
    } else {
        r->lead = -1;
        r->tmon = r->mon;
        r->ttags = r->savedtags;
        r->twin = r->savedwin;
        r->mode = OverviewClose;
        overviewcapturebars();
        if (r->log)
            fprintf(r->log, "overview close\n"), fflush(r->log);
    }
    /* 回到原 tag 且焦点不变、也没有窗口关闭: 最后交叉淡入开始时截的桌面, 结束画面与真实桌面一致 */
    r->fulldesk = r->tmon == r->mon && r->ttags == r->savedtags && r->twin == r->savedwin && !(c && r->cards[pick].hidden);
    for (i = 0; i < r->n; i++)
        if (r->cards[i].died || !r->cards[i].valid)
            r->fulldesk = 0;
    r->rstart = overviewnow();
    r->hover = -1;
    overviewsetcursor(0);
}

/* ---------- 每帧的状态 ---------- */

static int
overviewzcmp(const void *a, const void *b)
{
    double za = overviewscene.cards[*(const int *)a].z, zb = overviewscene.cards[*(const int *)b].z;

    return za < zb ? -1 : za > zb;
}

static void
overviewsort(void)
{
    OverviewScene *r = &overviewscene;
    int i;

    for (i = 0; i < r->n; i++)
        r->order[i] = i;
    qsort(r->order, r->n, sizeof *r->order, overviewzcmp);
}

/* 打开 / 停留 */
static void
overviewupdategrid(double t, double dt)
{
    OverviewScene *r = &overviewscene;
    OverviewCard *s;
    Client *c;
    double open = overviewoutcubic(overviewphase(t, .03, OVERVIEWOPEN)), fadein = overviewsmooth(overviewphase(t, .06, OVERVIEWOPEN));
    double k;
    int i, sel = overviewsel();

    for (i = 0; i < r->n; i++) {
        s = &r->cards[i];
        c = s->valid ? wintoclient(s->win) : NULL;
        s->urgent = c && c->isurgent && !s->died;
        s->hover = overviewfollow(s->hover, i == sel, dt, .07);
        if (s->current) {   /* 当前桌面上的窗口: 第一帧与真实窗口重合, 滑进格子 */
            s->x = overviewmix(s->hx, s->gx, open);
            s->y = overviewmix(s->hy, s->gy, open);
            k = overviewmix(1, s->gs, open);
            s->alpha = 1;
        } else {            /* 其他 tag / 隐藏的窗口: 在格子里由小变大淡入 */
            s->x = s->gx;
            s->y = s->gy;
            k = s->gs * overviewmix(.9, 1, open);
            s->alpha = fadein;
        }
        k *= 1 + .045 * s->hover;
        s->sw = s->w * k;
        s->sh = s->h * k;
        if (s->died)        /* 总览中关闭的窗口: 缩小淡出 */
            s->alpha *= 1 - overviewsmooth((overviewnow() - s->died) / .3);
        s->vis = s->alpha * (r->kqlen && !s->kmatch ? .3 : 1);
        s->light = sel >= 0 && i != sel ? overviewmix(1, .8, open) : 1;
        s->z = s->hover;    /* 高亮的卡片画在最上面 */
    }
    overviewsort();
    r->bright = overviewmix(1, .42, open);
    r->vign = .55 * open;
    r->desk = 1 - overviewsmooth(overviewphase(t, 0, .1));
    r->deskover = r->bar = 0;
}

/* 进入 (排开): 选中的卡片沿直线放大到真实窗口, 同 tag 的窗口稍晚滑到各自位置, 其余卡片被挤出屏幕.
 * 结束时卡片与真实窗口逐像素重合, 去掉遮罩时看不出切换 */
static void
overviewupdateland(double u)
{
    OverviewScene *r = &overviewscene;
    OverviewCard *s;
    double v, m;
    int i;

    for (i = 0; i < r->n; i++) {
        s = &r->cards[i];
        s->hover = 0;
        if (s->land) {
            v = overviewglide(i == r->lead ? u : overviewphase(u, .08, 1));
            s->x = overviewmix(s->fx, s->lx, v);
            s->y = overviewmix(s->fy, s->ly, v);
            s->sw = overviewmix(s->fw, s->lw, v);
            s->sh = overviewmix(s->fh, s->lh, v);
            s->alpha = 1;
            s->vis = overviewmix(s->fvis, 1, overviewsmooth(overviewphase(u, 0, .35)));
            s->light = overviewmix(s->flight, 1, v);
            if (i == r->lead)   /* 光框跟着卡片放大并淡出 */
                s->hover = s->fhover * (1 - overviewsmooth(overviewphase(u, 0, .4)));
            s->z = i == r->lead ? 2 : 1;
        } else {
            m = overviewglide(overviewphase(u, s->delay, s->delay + .6));
            s->x = s->fx + s->dx * s->push * m;
            s->y = s->fy + s->dy * s->push * m;
            s->sw = s->fw;
            s->sh = s->fh;
            s->alpha = 1 - overviewsmooth(overviewphase(u, s->delay + .35, s->delay + .65));
            s->vis = s->fvis * s->alpha;
            s->light = s->flight;
            s->z = 0;
        }
    }
    overviewsort();
    r->bright = overviewmix(r->rbright, 1, overviewinoutcubic(overviewphase(u, 0, .8)));
    r->vign = r->rvign * (1 - overviewsmooth(overviewphase(u, 0, .7)));
    r->desk = r->rdesk * (1 - overviewsmooth(overviewphase(u, 0, .3)));
    /* 回到原 tag: 最后交叉淡入开始时截的桌面; 换了 tag: 淡入实时截取的状态栏 */
    r->deskover = r->fulldesk ? overviewsmooth(overviewphase(u, .8, 1)) : 0;
    r->bar = overviewsmooth(overviewphase(u, .6, 1));
}

/* 关闭: 当前桌面的窗口沿直线滑回原位, 其余卡片原地缩小淡出 */
static void
overviewupdateclose(double u)
{
    OverviewScene *r = &overviewscene;
    OverviewCard *s;
    double v = overviewglide(u), away = overviewoutcubic(overviewphase(u, 0, .6));
    int i;

    for (i = 0; i < r->n; i++) {
        s = &r->cards[i];
        s->hover = s->fhover * (1 - overviewsmooth(overviewphase(u, 0, .4)));
        if (s->back) {
            s->x = overviewmix(s->fx, s->hx, v);
            s->y = overviewmix(s->fy, s->hy, v);
            s->sw = overviewmix(s->fw, s->w, v);
            s->sh = overviewmix(s->fh, s->h, v);
            s->alpha = 1;
            s->vis = overviewmix(s->fvis, 1, overviewsmooth(overviewphase(u, 0, .5)));
            s->light = overviewmix(s->flight, 1, v);
            s->z = 1;
        } else {
            s->x = s->fx;
            s->y = s->fy;
            s->sw = s->fw * (1 - .12 * away);
            s->sh = s->fh * (1 - .12 * away);
            s->alpha = 1 - away;
            s->vis = s->fvis * s->alpha;
            s->light = s->flight;
            s->z = 0;
        }
    }
    overviewsort();
    r->bright = overviewmix(r->rbright, 1, overviewinoutcubic(overviewphase(u, 0, .85)));
    r->vign = r->rvign * (1 - overviewsmooth(overviewphase(u, 0, .7)));
    r->desk = r->rdesk * (1 - overviewsmooth(overviewphase(u, 0, .3)));
    r->deskover = r->fulldesk ? overviewsmooth(overviewphase(u, .75, 1)) : 0;
    r->bar = r->fulldesk ? 0 : overviewsmooth(overviewphase(u, .6, 1));
}

/* 鼠标下最上面的卡片 */
static int
overviewpick(double x, double y)
{
    OverviewScene *r = &overviewscene;
    OverviewCard *s;
    int i;

    for (i = r->n - 1; i >= 0; i--) {
        s = &r->cards[r->order[i]];
        if (s->hit && !s->died && x >= s->bx0 && x < s->bx1 && y >= s->by0 && y < s->by1)
            return r->order[i];
    }
    return -1;
}

/* ---------- 绘制 (back 缓冲是屏内坐标, 卡片是 root 坐标) ---------- */

/* 带透明度的文字 (先画一层暗影保证在亮处也看得清). Xft 的颜色按预乘处理 */
static void
overviewtext(XftFont *font, double x, double y, const char *text, double a, int center)
{
    OverviewScene *r = &overviewscene;
    XRenderColor rc;
    XftColor col;
    XGlyphInfo ext;
    int len = strlen(text), k;

    if (!r->draw || !font || a < .02 || !len)
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
        if (!XftColorAllocValue(dpy, DefaultVisual(dpy, screen), DefaultColormap(dpy, screen), &rc, &col))
            return;
        XftDrawStringUtf8(r->draw, &col, font, (int)x + (k ? 0 : 1), (int)y + (k ? 0 : 2), (XftChar8 *)text, len);
        XftColorFree(dpy, DefaultVisual(dpy, screen), DefaultColormap(dpy, screen), &col);
    }
}

/* 一张卡片: 截图按当前大小缩放 (双线性, 亚像素位置), 不透明度 vis, light < 1 时压暗.
 * 只有位置和大小都对齐到像素时才直接拷贝: 收尾时如果提前按取整位置拷贝, 最后几帧会跳 1px、清晰度突变 */
static void
overviewdrawcard(OverviewCard *s)
{
    OverviewScene *r = &overviewscene;
    double left = s->x - s->sw * .5 - r->vx, top = s->y - s->sh * .5 - r->vy;
    int lvl, x0, y0, x1, y1, exact;
    Picture mask;

    if (!s->snap || s->vis < .004 || s->sw < 1 || s->sh < 1
            || left + s->sw < 0 || top + s->sh < 0 || left > r->vw || top > r->vh)
        return;
    if (s->vis > .3) {
        s->bx0 = left + r->vx;
        s->by0 = top + r->vy;
        s->bx1 = s->bx0 + s->sw;
        s->by1 = s->by0 + s->sh;
        s->hit = 1;
    }
    /* 源像素 / 屏幕像素不超过 2 */
    for (lvl = s->base; lvl < OVERVIEWMIPS - 1 && s->mip[lvl + 1] && s->mipw[lvl] > 2 * s->sw; lvl++);
    exact = fabs(s->sw - s->mipw[lvl]) < .02 && fabs(s->sh - s->miph[lvl]) < .02
        && fabs(left - lround(left)) < .02 && fabs(top - lround(top)) < .02;
    mask = s->vis > .996 ? None : overviewwhite(s->vis);
    if (exact) {
        x0 = (int)lround(left);
        y0 = (int)lround(top);
        x1 = x0 + s->mipw[lvl];
        y1 = y0 + s->miph[lvl];
        XRenderSetPictureFilter(dpy, s->mip[lvl], FilterNearest, NULL, 0);
    } else {
        /* 目标 -> 源: src = (dst - left) * 源 / 屏; 合成从整像素 x0 开始, 平移补上小数部分 */
        double kx = s->mipw[lvl] / s->sw, ky = s->miph[lvl] / s->sh;

        x0 = (int)floor(left);
        y0 = (int)floor(top);
        x1 = (int)ceil(left + s->sw);
        y1 = (int)ceil(top + s->sh);
        overviewaffine(s->mip[lvl], kx, ky, (x0 - left) * kx, (y0 - top) * ky);
    }
    XRenderComposite(dpy, PictOpOver, s->mip[lvl], mask, r->back, 0, 0, 0, 0, x0, y0, x1 - x0, y1 - y0);
    if (s->light < .996)    /* 压暗: 截图本身 (同样的变换) 当蒙版, 只暗卡片的形状 */
        XRenderComposite(dpy, PictOpOver, overviewblack((1 - s->light) * s->vis), s->mip[lvl], r->back,
                0, 0, 0, 0, x0, y0, x1 - x0, y1 - y0);
    overviewaffine(s->mip[lvl], 1, 1, 0, 0);
    XRenderSetPictureFilter(dpy, s->mip[lvl], FilterBilinear, NULL, 0);
}

/* 光框: 离卡片 1px 的一圈选中色实线, 外面三圈渐淡的光晕, 每圈 2px */
static void
overviewdrawframe(OverviewCard *s, double a)
{
    OverviewScene *r = &overviewscene;
    XRenderColor base = scheme[SchemeSel][ColBorder].color, c;
    static const double ring[] = {.8, .2, .11, .05};
    XRectangle rc[4];
    int k, x0, y0, x1, y1, t = 2;

    for (k = 0; k < (int)LENGTH(ring); k++) {
        x0 = (int)lround(s->bx0 - r->vx) - 1 - t * (k + 1);
        y0 = (int)lround(s->by0 - r->vy) - 1 - t * (k + 1);
        x1 = (int)lround(s->bx1 - r->vx) + 1 + t * (k + 1);
        y1 = (int)lround(s->by1 - r->vy) + 1 + t * (k + 1);
        c.alpha = (unsigned short)(65535 * overviewclamp(ring[k] * a));
        c.red = (unsigned short)(base.red / 65535.0 * c.alpha);
        c.green = (unsigned short)(base.green / 65535.0 * c.alpha);
        c.blue = (unsigned short)(base.blue / 65535.0 * c.alpha);
        rc[0] = (XRectangle){x0, y0, x1 - x0, t};
        rc[1] = (XRectangle){x0, y1 - t, x1 - x0, t};
        rc[2] = (XRectangle){x0, y0 + t, t, y1 - y0 - 2 * t};
        rc[3] = (XRectangle){x1 - t, y0 + t, t, y1 - y0 - 2 * t};
        XRenderFillRectangles(dpy, PictOpOver, r->back, &c, rc, 4);
    }
}

/* 请求关注的窗口: 卡片背后脉动的红光 */
static void
overviewdrawhalo(OverviewCard *s)
{
    OverviewScene *r = &overviewscene;
    double p = .5 + .5 * sin(2 * M_PI * overviewnow() / 1.1), rad = .7 * hypot(s->sw, s->sh) * (1 + .06 * p);
    double x = s->x - r->vx - rad, y = s->y - r->vy - rad, k;
    int x0 = (int)floor(x), y0 = (int)floor(y), d = (int)ceil(2 * rad) + 2;

    if (!r->halo || rad < 2)
        return;
    k = 64 / (2 * rad);
    overviewaffine(r->halo, k, k, (x0 - x) * k, (y0 - y) * k);
    XRenderComposite(dpy, PictOpOver, r->halo, overviewwhite((.3 + .3 * p) * s->alpha), r->back, 0, 0, 0, 0, x0, y0, d, d);
}

/* 过滤词: 屏幕下方居中的半透明框, 「过滤: 词 · N 个匹配 · 操作提示」 */
static void
overviewdrawquery(void)
{
    OverviewScene *r = &overviewscene;
    XGlyphInfo ext;
    XRenderColor shade = {0x0600, 0x0a00, 0x1600, 0xd800}, edge = {0x5000, 0x6800, 0x9000, 0x9000};
    char text[160];
    int x, y, w, h, len;

    if ((r->mode != OverviewIdle && r->mode != OverviewOpen) || !r->kqlen || !r->draw || !r->queryfont || !r->textcolorok)
        return;
    r->kq[r->kqlen] = 0;
    len = snprintf(text, sizeof text, "过滤: %s    ·    %d 个匹配%s", r->kq, r->kn,
            r->kn ? "    ·    Enter 进入    Tab 切换    Esc 清空" : "    ·    Esc 清空");
    XftTextExtentsUtf8(dpy, r->queryfont, (XftChar8 *)text, len, &ext);
    w = MIN(r->vw - 32, ext.xOff + 48);
    h = r->queryfont->height + 26;
    x = (r->vw - w) / 2;
    y = r->vh - h - r->vh / 9;
    XRenderFillRectangle(dpy, PictOpOver, r->back, &edge, x - 1, y - 1, w + 2, h + 2);
    XRenderFillRectangle(dpy, PictOpOver, r->back, &shade, x, y, w, h);
    XftDrawStringUtf8(r->draw, &r->textcolor, r->queryfont, x + 24, y + 13 + r->queryfont->ascent, (XftChar8 *)text, len);
}

/* F12 帧率面板: 右上角, 实时帧率 / 渲染耗时 / 截图刷新 */
static void
overviewdrawhud(void)
{
    OverviewScene *r = &overviewscene;
    XRenderColor shade = {0x0400, 0x0600, 0x0c00, 0xc800};
    char line[2][128];
    double sum = 0;
    int i, n = MIN(r->ngaps, OVERVIEWHUDGAPS), k, w = 0, lh, x, y;
    XGlyphInfo ext;

    if (!overviewhud || !r->draw || !r->titlefont || !r->textcolorok)
        return;
    for (i = 0; i < n; i++)
        sum += r->gaps[i];
    snprintf(line[0], sizeof line[0], "%.0f fps  ·  渲染 %.1f ms  ·  %s", sum > 0 ? n / sum : 0, r->lastcost * 1000,
            overviewmodename[r->mode]);
    snprintf(line[1], sizeof line[1], "截图刷新 %d 张 / 平均 %.2f ms", r->refreshn, r->refreshn ? r->refreshsum / r->refreshn * 1000 : 0);
    for (k = 0; k < 2; k++) {
        XftTextExtentsUtf8(dpy, r->titlefont, (XftChar8 *)line[k], strlen(line[k]), &ext);
        w = MAX(w, ext.xOff);
    }
    lh = r->titlefont->height + 2;
    w += 24;
    x = r->vw - w - 24;
    y = 24;
    XRenderFillRectangle(dpy, PictOpOver, r->back, &shade, x, y, w, lh * 2 + 14);
    for (k = 0; k < 2; k++)
        XftDrawStringUtf8(r->draw, &r->textcolor, r->titlefont, x + 12, y + 7 + k * lh + r->titlefont->ascent,
                (XftChar8 *)line[k], strlen(line[k]));
}

/* 结尾盖在卡片之上: 回到原样时交叉淡入开始时截的桌面; 否则淡入真实窗口的边框和切换后的状态栏 */
static void
overviewdrawfront(void)
{
    OverviewScene *r = &overviewscene;
    OverviewCard *s;
    Client *c;
    XRenderColor bc;
    XRectangle rc[4];
    int i, bw, x0, y0, x1, y1;

    if (r->deskover > .004)
        XRenderComposite(dpy, PictOpOver, r->desktop, overviewwhite(r->deskover), r->back, 0, 0, 0, 0, 0, 0, r->vw, r->vh);
    if ((r->mode != OverviewLand && r->mode != OverviewClose) || r->bar < .004 || r->fulldesk)
        return;
    for (i = 0; i < r->n; i++) {
        s = &r->cards[i];
        c = s->valid ? wintoclient(s->win) : NULL;
        if (!c || !(r->mode == OverviewLand ? s->land : s->back) || !s->hit || (bw = c->bw) <= 0 || s->alpha < .5)
            continue;
        bc = scheme[c->win == r->twin ? SchemeSel : SchemeNorm][ColBorder].color;
        bc.red = (unsigned short)(bc.red * r->bar);
        bc.green = (unsigned short)(bc.green * r->bar);
        bc.blue = (unsigned short)(bc.blue * r->bar);
        bc.alpha = (unsigned short)(65535 * r->bar);
        x0 = (int)lround(s->bx0 - r->vx) - bw;
        y0 = (int)lround(s->by0 - r->vy) - bw;
        x1 = (int)lround(s->bx1 - r->vx) + bw;
        y1 = (int)lround(s->by1 - r->vy) + bw;
        rc[0] = (XRectangle){x0, y0, x1 - x0, bw};
        rc[1] = (XRectangle){x0, y1 - bw, x1 - x0, bw};
        rc[2] = (XRectangle){x0, y0 + bw, bw, y1 - y0 - 2 * bw};
        rc[3] = (XRectangle){x1 - bw, y0 + bw, bw, y1 - y0 - 2 * bw};
        XRenderFillRectangles(dpy, PictOpOver, r->back, &bc, rc, 4);
    }
    for (i = 0; i < r->nbars; i++)
        XRenderComposite(dpy, PictOpOver, r->bars[i], overviewwhite(r->bar), r->back, 0, 0, 0, 0,
                r->barx[i] - r->vx, r->bary[i] - r->vy, r->barw[i], r->barh[i]);
}

static void
overviewrender(void)
{
    OverviewScene *r = &overviewscene;
    OverviewCard *s;
    XRenderColor shade = {0, 0, 0, 0};
    double depth = 1 - .035 * overviewclamp((1 - r->bright) / .58), fade = 1, t;
    int i, titles = r->mode == OverviewOpen || r->mode == OverviewIdle || r->mode == OverviewLand;

    /* 背景: 壁纸以屏幕中心稍稍后退 (缩小) 并压暗; 开始时截的桌面在第一帧盖在上面, 很快淡出.
     * 阈值要足够小: 太早切回原图, 边缘会跳半个像素 */
    if (depth < .99999) {
        overviewaffine(r->wallpaper, 1 / depth, 1 / depth, (1 - 1 / depth) * r->vw * .5, (1 - 1 / depth) * r->vh * .5);
        XRenderSetPictureFilter(dpy, r->wallpaper, FilterBilinear, NULL, 0);
    }
    XRenderComposite(dpy, PictOpSrc, r->wallpaper, None, r->back, 0, 0, 0, 0, 0, 0, r->vw, r->vh);
    if (depth < .99999) {
        overviewaffine(r->wallpaper, 1, 1, 0, 0);
        XRenderSetPictureFilter(dpy, r->wallpaper, FilterNearest, NULL, 0);
    }
    if (r->desk > .004)
        XRenderComposite(dpy, PictOpOver, r->desktop, overviewwhite(r->desk), r->back, 0, 0, 0, 0, 0, 0, r->vw, r->vh);
    shade.alpha = (unsigned short)(65535 * overviewclamp(1 - r->bright));
    if (shade.alpha)
        XRenderFillRectangle(dpy, PictOpOver, r->back, &shade, 0, 0, r->vw, r->vh);
    if (r->vignette && r->vign > .004)
        XRenderComposite(dpy, PictOpOver, r->vignette, overviewwhite(r->vign), r->back, 0, 0, 0, 0, 0, 0, r->vw, r->vh);
    for (i = 0; i < r->n; i++)
        r->cards[i].hit = 0;
    for (i = 0; i < r->n; i++) {
        s = &r->cards[r->order[i]];
        if (s->alpha < .002)
            continue;
        if (s->urgent && r->mode != OverviewLand)
            overviewdrawhalo(s);
        overviewdrawcard(s);
        if (s->hover > .01 && s->hit && r->mode != OverviewClose)
            overviewdrawframe(s, s->hover * s->alpha);
    }
    /* 标题: 卡片下方居中, 高亮的更亮; 进入时在前 20% 淡出 */
    if (r->mode == OverviewLand)
        fade = 1 - overviewsmooth((overviewnow() - r->rstart) / (.2 * OVERVIEWLAND));
    for (i = 0; titles && fade > .01 && i < r->n; i++) {
        s = &r->cards[i];
        t = overviewclamp(1 - r->desk) * s->vis * (.55 + .45 * s->hover) * fade;
        if (s->hit && s->title[0] && r->titlefont)
            overviewtext(r->titlefont, (s->bx0 + s->bx1) * .5 - r->vx, s->by1 - r->vy + 10 + r->titlefont->ascent, s->title, t, 1);
    }
    overviewdrawquery();
    overviewdrawfront();
    overviewdrawhud();
    XRenderComposite(dpy, PictOpSrc, r->back, None, r->overlaypic, 0, 0, 0, 0, 0, 0, r->vw, r->vh);
    XSync(dpy, False);
}

/* ---------- 每帧 / 事件 ---------- */

static void
overviewtick(void)
{
    OverviewScene *r = &overviewscene;
    double now = overviewnow(), dt, u = 0, gap, begin, cost;
    int sel;

    if (!r->mode)
        return;
    if (r->mode == OverviewOpen && now >= OVERVIEWOPEN) {
        overviewlogseg();
        r->mode = OverviewIdle;
    }
    if (r->mode == OverviewLand || r->mode == OverviewClose) {
        u = (now - r->rstart) / (r->mode == OverviewLand ? OVERVIEWLAND : OVERVIEWCLOSE);
        if (u >= 1) {
            overviewend(1);
            return;
        }
    }
    gap = r->last >= 0 ? now - r->last : 0;
    dt = MIN(gap, .1);
    if (r->last >= 0) {
        r->gaps[r->ngaps++ % OVERVIEWHUDGAPS] = gap;
        r->seggapmax = MAX(r->seggapmax, gap);
    }
    r->last = now;
    begin = overviewnow();
    if ((r->mode == OverviewOpen || r->mode == OverviewIdle) && r->mousevalid) {
        r->hover = overviewpick(r->mx, r->my);
        overviewsetcursor(r->hover >= 0);
    }
    if (r->mode == OverviewLand)
        overviewupdateland(u);
    else if (r->mode == OverviewClose)
        overviewupdateclose(u);
    else
        overviewupdategrid(now, dt);
    overviewrender();
    if (r->ksel >= 0 && !overviewkeyok(r->ksel) && (sel = overviewkeystart()) >= 0)
        r->ksel = sel;      /* 选中的窗口关闭了: 换一个 */
    cost = overviewnow() - begin;
    overviewrefreshstep(now);
    r->lastcost = cost;
    r->segrender += cost;
    r->segrendermax = MAX(r->segrendermax, cost);
    r->segframes++;
    r->frames++;
    /* 进入 / 关闭中的慢帧: 帧间隔超过 1.5 帧或渲染超过 6ms 时记下进度, 找卡顿出在哪一段 */
    if (r->log && (r->mode == OverviewLand || r->mode == OverviewClose) && (gap > 1.5 / OVERVIEWFPS || cost > .006))
        fprintf(r->log, "overview slow frame: u %.2f gap %.1fms render %.1fms\n", u, 1000 * gap, 1000 * cost), fflush(r->log);
}

/* select() 的超时 (微秒) */
static long
overviewtimeout(void)
{
    OverviewScene *r = &overviewscene;
    double left = 1 / OVERVIEWFPS - (overviewnow() - r->last);

    return left > 0 ? (long)(left * 1e6) : 0;
}

/* 按键: Super+A / Enter 进入高亮的窗口 (没有高亮时 Super+A 关闭), Esc 先清空过滤词再关闭,
 * 方向键 / Tab / Super+Tab 选卡片, 打字过滤, F12 帧率面板. 进入 / 关闭中按 Esc 或 Super+A 立即结束 */
static void
overviewkey(XEvent *e, KeySym sym)
{
    OverviewScene *r = &overviewscene;
    int mod = CLEANMASK(e->xkey.state) == MODKEY, sel = overviewsel(), n;
    char buf[8];
    KeySym ks;

    if (r->mode == OverviewLand || r->mode == OverviewClose) {
        if (sym == XK_Escape || (mod && sym == XK_a))
            overviewend(1);
        return;
    }
    if (sym == XK_F12) {
        overviewhud = !overviewhud;
        return;
    }
    if (mod && sym == XK_a) {
        overviewleave(sel);
        return;
    }
    if (mod && sym == XK_Tab) {
        r->lastkey = overviewnow();
        overviewkeycycle(1);
        return;
    }
    if (sym == XK_Escape) {
        if (r->kqlen)
            overviewkeyclear();
        else
            overviewleave(-1);
        return;
    }
    if (sym == XK_Return || sym == XK_KP_Enter) {
        if (sel >= 0)
            overviewleave(sel);
        return;
    }
    if (CLEANMASK(e->xkey.state) & (MODKEY | ControlMask | Mod1Mask))
        return;     /* 带 Super / Ctrl / Alt 的组合键不当作输入 */
    r->lastkey = overviewnow();
    switch (sym) {
    case XK_Left:  overviewkeymove(-1, 0); return;
    case XK_Right: overviewkeymove(1, 0); return;
    case XK_Up:    overviewkeymove(0, -1); return;
    case XK_Down:  overviewkeymove(0, 1); return;
    case XK_Tab:   overviewkeycycle(e->xkey.state & ShiftMask ? -1 : 1); return;
    case XK_ISO_Left_Tab: overviewkeycycle(-1); return;
    case XK_BackSpace:
        if (r->kqlen) {
            r->kqlen--;
            overviewkeyfilter();
        }
        return;
    }
    n = XLookupString(&e->xkey, buf, sizeof buf, &ks, NULL);
    if (n == 1 && buf[0] >= 0x20 && buf[0] < 0x7f && r->kqlen < (int)sizeof r->kq - 1) {
        r->kq[r->kqlen++] = buf[0];
        r->ksel = -1;       /* 过滤词变了: 选中跳到第一个匹配 */
        overviewkeyfilter();
    }
}

/* 动画期间 dwm 主循环先把事件交给这里; 返回 1 表示已处理, 不再交给 dwm */
static int
overviewevent(XEvent *e)
{
    OverviewScene *r = &overviewscene;
    KeySym sym;
    Window w;
    int i;

    if (!r->mode)
        return 0;
    switch (e->type) {
    case KeyPress:
        sym = XLookupKeysym(&e->xkey, 0);
        if (!IsModifierKey(sym))
            overviewkey(e, sym);
        return 1;
    case ButtonPress:
        /* 点卡片进入它, 点空白处关闭 */
        if ((r->mode == OverviewOpen || r->mode == OverviewIdle) && e->xbutton.button == Button1)
            overviewleave(overviewpick(e->xbutton.x_root, e->xbutton.y_root));
        return 1;
    case MotionNotify:
        r->mx = e->xmotion.x_root;
        r->my = e->xmotion.y_root;
        r->mousevalid = 1;
        return 1;
    case ButtonRelease: case KeyRelease: case EnterNotify: case LeaveNotify:
        return 1;
    case Expose:
        return e->xexpose.window == r->overlay;     /* 每帧都整幅重画 */
    case DestroyNotify:
    case UnmapNotify:
        /* 窗口在总览中关闭: 卡片缩小淡出 (进入 / 关闭中照旧显示到动画结束) */
        w = e->type == DestroyNotify ? e->xdestroywindow.window : e->xunmap.window;
        for (i = 0; i < r->n; i++)
            if (r->cards[i].win == w && r->cards[i].valid) {
                r->cards[i].valid = 0;
                r->cards[i].c = NULL;
                if ((r->mode == OverviewOpen || r->mode == OverviewIdle) && !r->cards[i].died)
                    r->cards[i].died = overviewnow();
                if (r->log)
                    fprintf(r->log, "card %d window 0x%lx gone\n", i, w), fflush(r->log);
            }
        if (e->type == DestroyNotify && w == r->twin && r->mode == OverviewLand)
            r->twin = None;
        return 0;
    }
    return 0;
}

/* dwm 处理完事件之后: 新映射 / 重新排列的窗口不能盖到遮罩上面 */
static void
overviewpost(XEvent *e)
{
    OverviewScene *r = &overviewscene;

    if (!r->mode)
        return;
    XSetErrorHandler(overviewxerror);
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
}

/* Super+A: 打开窗口总览 (已打开时立即结束) */
static void
overview(const Arg *arg)
{
    OverviewScene *r = &overviewscene;
    XSetWindowAttributes wa;
    XClassHint cls = {"dwm-overview", "dwm-overview"};
    XRenderColor color;
    OverviewCard *s;
    Client *c;
    Window dw;
    int i, n = 0, ev, er, dx, dy, pass, full;
    unsigned int mask;
    long budget = OVERVIEWBUDGET, bytes;

    if (r->mode) {
        overviewend(1);
        return;
    }
    memset(r, 0, sizeof *r);
    clock_gettime(CLOCK_MONOTONIC, &r->start);
    r->mon = r->tmon = selmon;
    r->vx = selmon->mx;
    r->vy = selmon->my;
    r->vw = selmon->mw;
    r->vh = selmon->mh;
    r->savedtags = r->ttags = selmon->tagset[selmon->seltags] & TAGMASK;
    r->savedwin = r->twin = selmon->sel ? selmon->sel->win : None;
    r->hover = r->ksel = r->lead = -1;
    r->lastkey = -1e9;
    r->last = -1;
    XQueryExtension(dpy, "RENDER", &r->rendermajor, &ev, &er);
    XSetErrorHandler(overviewxerror);
    r->argb = XRenderFindStandardFormat(dpy, PictStandardARGB32);
    for (c = selmon->clients; c; c = c->next)
        if (!c->isscratchpad)
            n++;
    r->cards = calloc(MAX(1, n), sizeof *r->cards);
    r->order = calloc(MAX(1, n), sizeof *r->order);
    if (!r->cards || !r->order || !r->argb)
        goto fail;
    if (!(r->back = overviewopaquepic(r->vw, r->vh, &r->backpix))
            || !(r->desktop = overviewopaquepic(r->vw, r->vh, &r->desktoppix))
            || !(r->wallpaper = overviewopaquepic(r->vw, r->vh, &r->wallpix)))
        goto fail;
    /* Xft 不会逐字回退到别的字体: 选一个同时含中文和拉丁字母的 (窗口标题常有中文) */
    r->titlefont = XftFontOpenName(dpy, screen, "sans:lang=zh-cn:size=11");
    r->queryfont = XftFontOpenName(dpy, screen, "sans:lang=zh-cn:size=16");
    if ((r->draw = XftDrawCreate(dpy, r->backpix, DefaultVisual(dpy, screen), DefaultColormap(dpy, screen)))) {
        XRenderColor white = {0xe900, 0xf600, 0xffff, 0xffff};
        r->textcolorok = XftColorAllocValue(dpy, DefaultVisual(dpy, screen), DefaultColormap(dpy, screen), &white, &r->textcolor);
    }
    for (i = 0; i < OVERVIEWALPHAS; i++) {
        color.alpha = color.red = color.green = color.blue = (unsigned short)(65535L * i / (OVERVIEWALPHAS - 1));
        r->white[i] = XRenderCreateSolidFill(dpy, &color);
        color.red = color.green = color.blue = 0;
        r->black[i] = XRenderCreateSolidFill(dpy, &color);
    }
    overviewbuildsprites();

    for (c = selmon->clients; c; c = c->next) {
        if (c->isscratchpad)
            continue;
        s = &r->cards[r->n++];
        s->win = c->win;
        s->c = c;
        snprintf(s->title, sizeof s->title, "%.63s", c->name);
        s->valid = s->kmatch = 1;
        s->hidden = HIDDEN(c);
        s->current = ISVISIBLE(c) && !s->hidden;
        s->focused = c == selmon->sel;
        s->w = MAX(1, c->w);
        s->h = MAX(1, c->h);
        s->hx = c->x + c->bw + c->w * .5;
        s->hy = c->y + c->bw + c->h * .5;
        /* 隐藏窗口没有缓存的截图时 (dwm 重启前就隐藏了等) 临时映射截一张 */
        if (s->hidden && !c->preview.hidden_image)
            c->preview.hidden_image = capturehidden(c);
    }
    XGrabServer(dpy);
    overviewcapturebackground();
    /* 每个窗口只截一次. 当前桌面的窗口优先截全尺寸 (第一帧与真实窗口重合), 其余在预算内也截全尺寸 (进入时 1:1 显示), 超出的截半尺寸 */
    for (pass = 0; pass < 2; pass++)
        for (i = 0; i < r->n; i++) {
            s = &r->cards[i];
            if (s->current != !pass)
                continue;
            bytes = (long)s->w * s->h * 16 / 3;   /* 4 字节 x (1 + mip 链约 1/3) */
            full = pass ? budget >= bytes : budget > 0;
            if (full)
                budget -= bytes;
            overviewcapture(s, s->c, full);
        }
    XSync(dpy, False);
    XUngrabServer(dpy);
    overviewlayout();
    if (XQueryPointer(dpy, root, &dw, &dw, &dx, &dy, &ev, &er, &mask)) {
        r->mx = dx;
        r->my = dy;
    }

    /* 遮罩只盖这块屏, 背景就是刚截的桌面, 映射瞬间不会闪 */
    wa.override_redirect = True;
    wa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
    wa.background_pixmap = r->desktoppix;
    r->overlay = XCreateWindow(dpy, root, r->vx, r->vy, r->vw, r->vh, 0, DefaultDepth(dpy, screen), InputOutput,
            DefaultVisual(dpy, screen), CWOverrideRedirect | CWEventMask | CWBackPixmap, &wa);
    XSetClassHint(dpy, r->overlay, &cls);
    XStoreName(dpy, r->overlay, "dwm-overview");
    r->hand = XCreateFontCursor(dpy, XC_hand2);
    if (!(r->overlaypic = XRenderCreatePicture(dpy, r->overlay, XRenderFindVisualFormat(dpy, DefaultVisual(dpy, screen)), 0, NULL)))
        goto fail;
    XMapRaised(dpy, r->overlay);
    r->mode = OverviewOpen;
    overviewlogstart();
    /* 别的程序可能正短暂持有抓取 (菜单刚关闭等): 最多重试约 0.5s */
    for (i = 0; i < 50 && XGrabKeyboard(dpy, r->overlay, False, GrabModeAsync, GrabModeAsync, CurrentTime) != GrabSuccess; i++)
        nanosleep(&(struct timespec){0, 10000000}, NULL);
    if (i == 50)
        goto fail;
    r->grabkbd = 1;
    for (i = 0; i < 50 && XGrabPointer(dpy, r->overlay, False, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                GrabModeAsync, GrabModeAsync, None, None, CurrentTime) != GrabSuccess; i++)
        nanosleep(&(struct timespec){0, 10000000}, NULL);
    if (i == 50)
        goto fail;
    r->grabptr = 1;
    XSync(dpy, False);
    r->segstart = overviewnow();
    overviewtick();
    return;
fail:
    if (r->log)
        fprintf(r->log, "overview start failed (keyboard %d pointer %d)\n", r->grabkbd, r->grabptr);
    r->mode = OverviewOpen;
    overviewend(1);
}
