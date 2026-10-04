/* Super+Z 3D 工作空间星系. 由 dwm.c 在 config.h 与 Pertag 之后 #include.
 *
 * 每个窗口在开始时截图一次 (XRender Picture + mipmap), 之后只在离屏 3D 场景里绘制:
 *   世界坐标 (x 右, y 下, z 远离镜头) -> 镜头变换 -> 透视投影 -> 按镜头空间 z 排序 -> XRender 合成 -> 全屏遮罩窗口
 * Tag = 星系核心, 窗口 = 沿 3D 轨道环绕核心运行的星体, 所有 tag 组成星系群.
 * 流程: 开场 (约 5.7s) -> 停在星系轨道态 (不限时, 镜头缓慢巡航, 鼠标视差 / 滚轮推拉 / 点击窗口星跳转)
 *       -> Esc: 坍缩成一个光点, 停在纯壁纸 (再按 Super+Z 恢复)
 *       -> Super+Z / 点击: 回程, 窗口星飞回原位置变回截图, 露出真实桌面
 * 动画期间不移动 / 隐藏 / 映射 / 重叠 / 聚焦任何真实窗口; 只在结束时恢复 (或按点击切换) tag 和焦点. */

#include <X11/extensions/dpms.h>

#define RELAXMIPS     6
#define RELAXALPHAS   256
#define RELAXTRAIL    12
#define RELAXBUCKETS  8
#define RELAXSPRITES  3
#define RELAXGAPS     4096
#define RELAXRINGS    3
#define RELAXSEG      72
#define RELAXFPS      120.0
#define RELAXORBITFPS 60.0
#define RELAXIDLEFPS  30.0
#define RELAXIDLE     90.0      /* 驻留时多久无输入后降到 RELAXIDLEFPS (秒) */
#define RELAXINTRO    1.35      /* 开场时间拉伸: 1 场景秒 = 1.35 真实秒 */
#define RELAXHOLD     4.2       /* 开场在场景时间 4.2s 进入驻留轨道态 */
#define RELAXEXIT     4.75      /* Esc 坍缩从这里接着播放到 RELAXEND (4.2~4.8 之间的曲线是平的) */
#define RELAXEND      6.0
#define RELAXWARP     .6        /* 开场中按键: 快进到驻留态的真实时长 */
#define RELAXRETURN   1.6       /* 回程的真实时长 */
#define RELAXCRUISE   18.0      /* 驻留时巡航扫掠的周期 (场景秒) */
#define RELAXPI       3.14159265358979323846

enum { RelaxOff, RelaxIntro, RelaxOrbit, RelaxCollapse, RelaxReturn, RelaxRest };
enum { RelaxDustItem, RelaxCoreItem, RelaxStarItem, RelaxRingItem };
enum { RelaxHalo, RelaxDisc, RelaxShapes };
enum { RelaxWarm, RelaxCool, RelaxTints };

static const char *relaxmodename[] = { "off", "intro", "orbit", "collapse", "return", "rest" };

typedef struct { double x, y, z; } RelaxVec;
typedef struct { double m[3][3]; } RelaxMat;
typedef struct { double w, x, y, z; } RelaxQuat;
typedef struct { double t, v; } RelaxKey;

typedef struct {
    double x, y, z, scale;      /* 屏幕坐标, 镜头空间深度, 透视缩放 */
    int ok;
} RelaxProj;

typedef struct {
    RelaxVec pos, target;
    double rx, ry, rz;          /* pitch / yaw / roll */
    double fov, focal, near, far, dist;
    RelaxMat rot;               /* 镜头 -> 世界 (镜头的朝向) */
    RelaxMat view;              /* 世界 -> 镜头 */
} RelaxCamera;

typedef struct {
    int tag, nstars, nrings, hit;
    RelaxVec home;              /* 星系群坐标系中的位置 */
    RelaxVec pos, rpos;         /* 当前世界坐标 / 回程开始时冻结的位置 */
    double rx, ry, rz;          /* 轨道平面: 倾角 / 偏航 / 翻滚 */
    double radius, speed, phase, precess, size, alpha, ralpha, hover;
    RelaxMat ring[RELAXRINGS];  /* 每条轨道环相对星系轨道平面的姿态 (环与环互相倾斜) */
    double ringr[RELAXRINGS], ringz[RELAXRINGS][2];
    int ringn[RELAXRINGS][2];   /* 后半圈 / 前半圈的线段数 */
    RelaxMat plane;             /* 当前轨道平面 (含全局旋转) */
    double hx, hy, hr;          /* 屏幕上的点击范围 */
    RelaxProj p;
} RelaxGalaxy;

typedef struct {
    Window win;
    Client *c;                  /* 只用于调试日志, 窗口关闭后置空 */
    Monitor *mon;
    unsigned int tags;
    char title[64];
    int galaxy, ring, valid, focused, current, hidden, global, shown, back, base, snap, hit;
    int w, h;                   /* 真实窗口大小, 也是卡片的世界尺寸 */
    Pixmap mippix[RELAXMIPS];
    Picture mip[RELAXMIPS];
    int mipw[RELAXMIPS], miph[RELAXMIPS];
    RelaxVec home, detach;      /* 桌面平面 z=0 上的起点 / 脱离桌面后的位置 */
    double drx, dry, drz;       /* 脱离桌面时的卡片旋转 */
    double radius, angle, speed, rock, delay;
    RelaxVec pos, vel;
    RelaxMat orient;
    double size, brightness, alpha, vis, tint, glow, lod, hover;
    /* 回程开始时冻结的状态 */
    RelaxVec rpos;
    RelaxMat rorient;
    double rsize, rvis, rtint, rglow, rbright;
    double bx0, by0, bx1, by1;  /* 屏幕上的点击范围 */
    RelaxProj p;
} RelaxStar;

typedef struct {
    RelaxVec pos;
    double size, light;
    RelaxProj p;
} RelaxDust;

typedef struct { int kind, index; double z; } RelaxItem;

typedef struct {
    int mode, grabkbd, grabptr, w, h, ntags, nstars, ndust, nitems, ntrail, triscap, rendermajor;
    int warping, dpms, dpmsoff, hover, hovercore, handon, fulldesk;
    double tscale, starscale, orbitscale, glowscale;
    /* 时钟: scene 是场景时间, stage 驱动关键帧曲线 (驻留时停住), motion 驱动轨道运动 (一直走) */
    double last, scene, stage, motion, holdw;
    double wstart, wscene, cstart, rstart, lastinput, lastdpms;
    /* 驻留交互: 鼠标视差 / 滚轮推拉 (t 开头是目标值, 每帧平滑逼近) */
    double mx, my, tyaw, tpitch, pyaw, ppitch, tzoom, zoom;
    /* 每帧由 update 算出, render 只读这些 */
    double bright, vign, desk, deskover, bar, reveal, ringalpha, trailgain, dustfade, central, pulse;
    /* 回程开始时冻结的全局状态 */
    double rcdist, rcx, rcy, rcz, rring, rdust, rbright, rvign, rdesk;
    RelaxVec rcampos;
    Window overlay, wallwin;
    Cursor hand;
    Pixmap backpix, desktoppix, wallpix, vignettepix, bgpix;
    Picture back, desktop, wallpaper, live, overlaypic, vignette, bg;
    double bgkey[4], bglast[4];     /* 背景缓存: 驻留时壁纸亮度 / 暗角不变, 每帧只复制一次 */
    int bgok;
    Pixmap spritepix[RelaxShapes][RelaxTints][RELAXSPRITES];
    Picture sprite[RelaxShapes][RelaxTints][RELAXSPRITES];
    Picture white[RELAXALPHAS], black[RELAXALPHAS];
    XRenderPictFormat *argb, *a8;
    RelaxGalaxy *galaxies;
    RelaxStar *stars;
    RelaxDust *dust;
    RelaxItem *items;
    RelaxVec *tgpos;
    RelaxMat *tgplane;
    RelaxProj *tpts, *rpts;
    XTriangle *tris[RELAXBUCKETS];
    int ntris[RELAXBUCKETS];
    RelaxCamera cam;
    RelaxMat world;
    struct timespec start;
    /* 开始前的 dwm 状态, 以及结束时要切换到的状态 (默认就是开始前的状态, 点击窗口星时改成该窗口) */
    Monitor *savedmon, *tmon;
    unsigned int savedtags, ttags;
    Window savedwin, twin;
    int tshow, barx, bary, barw, barh;
    /* 统计 (按阶段) */
    FILE *log;
    unsigned long frames, errors, ngaps;
    double segstart, rendersum, rendermax, *gaps;
    double firstgap[24], firstcost[24];    /* 开场前 24 帧的时刻和渲染耗时 */
    int nfirst, firstlogged;
} RelaxScene;

static RelaxScene relaxscene;
static const int relaxspritesize[RELAXSPRITES] = { 128, 32, 8 };

/* 时间轴上的关键帧曲线 (单调分段三次 Hermite), 用于镜头和形态变化.
 * 时间是场景时间; 驻留态停在 RELAXHOLD, 坍缩从 RELAXEXIT 接着播放, 所以 4.2~4.8 的值必须相同 */
static const RelaxKey relaxcamdist[] = {
    {0, 1}, {.15, 1}, {.6, 1.05}, {1, 1.12}, {1.5, 1.22}, {2.2, 1.5}, {2.6, 1.46},
    {2.9, 1.38}, {3.3, 1.3}, {3.8, 1.21}, {4.2, 1.18}, {4.8, 1.18}, {5.5, 1.2}, {6, 1.2}
};
static const RelaxKey relaxcamyaw[] = {   /* 度 */
    {0, 0}, {.15, 0}, {.6, -3}, {1, -6}, {1.5, -10}, {2.2, -12}, {2.6, -2},
    {2.9, 10}, {3.3, 20}, {3.8, 14}, {4.2, 10}, {4.8, 10}, {5.5, 0}, {6, 0}
};
static const RelaxKey relaxcampitch[] = { /* 负值: 镜头在上方俯视, 驻留时轨道环显出椭圆 */
    {0, 0}, {.15, 0}, {.6, 2}, {1, 5}, {1.5, 8}, {2.2, 8}, {2.6, 4},
    {2.9, 0}, {3.3, -5}, {3.8, -7}, {4.2, -7}, {4.8, -7}, {5.5, 0}, {6, 0}
};
static const RelaxKey relaxcamroll[] = {
    {0, 0}, {2.2, 0}, {2.6, .8}, {2.9, 1.6}, {3.3, 2}, {4.2, 1}, {4.8, 1}, {5.5, 0}
};
static const RelaxKey relaxbright[] = {   /* 壁纸亮度: 冻结时 70%, 星系阶段沉入深空, 结尾 70% -> 100% */
    {0, 1}, {.15, .7}, {.6, .7}, {1.5, .3}, {4.8, .3}, {5.5, .7}, {5.7, .7}, {6, 1}
};
static const RelaxKey relaxvignettekeys[] = {
    {.15, 0}, {1.5, .75}, {4.8, .75}, {5.6, .25}, {6, 0}
};
static const RelaxKey relaxcardvis[] = {  /* 截图可见度 (再乘深度 LOD): 截图 -> 小卡 -> 发光面板, 坍缩时变成光点 */
    {1, 1}, {1.5, .85}, {2.2, .7}, {3, .62}, {3.6, .7}, {4.2, .8}, {4.8, .8}, {5.05, 0}
};
static const RelaxKey relaxcardsize[] = {
    {.15, 1}, {.6, .74}, {1, .46}, {1.5, .3}, {2.2, .2}, {3.3, .15}, {4.2, .17}, {4.8, .17}, {5.2, .08}
};
static const RelaxKey relaxcardtint[] = { /* 卡片自身发光: 截图 -> 发光的小面板 */
    {1.2, 0}, {2.2, .22}, {3, .42}, {3.6, .3}, {4.2, .14}, {4.8, .14}, {5, .5}
};
static const RelaxKey relaxstarglow[] = {
    {.8, 0}, {1.5, .22}, {2.2, .45}, {3, .75}, {3.6, 1}
};
static const RelaxKey relaxtrailgain[] = {
    {1.4, 0}, {1.9, .35}, {2.3, .6}, {2.7, 1}, {3.1, .9}, {3.5, .55}, {4.2, .5},
    {4.8, .5}, {5.05, .35}, {5.35, .2}, {5.5, 0}
};
static const RelaxKey relaxringkeys[] = { /* 轨道环的透明度: 高速旋转时稍明显, 驻留时淡 */
    {1, 0}, {1.8, .14}, {2.4, .24}, {3.3, .24}, {4.2, .2}, {4.8, .2}, {5.1, 0}
};
static const double relaxinclinations[] = { 8, 28, -36, 54, -22, 42, -50, 16, -62 };
static const double relaxringtilt[RELAXRINGS] = { 0, 38, -32 };

/* ---------- 数学 ---------- */

static double relaxclamp(double x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static double relaxmix(double a, double b, double t) { return a + (b - a) * t; }
static double relaxphase(double t, double a, double b) { return relaxclamp((t - a) / (b - a)); }
static double relaxeaseincubic(double x) { x = relaxclamp(x); return x * x * x; }
static double relaxeaseoutcubic(double x) { x = 1 - relaxclamp(x); return 1 - x * x * x; }
static double relaxeaseoutquart(double x) { x = 1 - relaxclamp(x); return 1 - x * x * x * x; }
static double relaxsmoothstep(double x) { x = relaxclamp(x); return x * x * (3 - 2 * x); }

static double
relaxeaseinoutcubic(double x)
{
    x = relaxclamp(x);
    return x < .5 ? 4 * x * x * x : 1 - pow(-2 * x + 2, 3) / 2;
}

/* 指数平滑: 与帧率无关 */
static double
relaxfollow(double cur, double target, double dt, double tau)
{
    return cur + (target - cur) * (1 - exp(-dt / tau));
}

static double
relaxtangent(const RelaxKey *k, int n, int i)
{
    double a, b;

    if (i == 0 || i == n - 1)
        return 0;
    a = (k[i].v - k[i-1].v) / (k[i].t - k[i-1].t);
    b = (k[i+1].v - k[i].v) / (k[i+1].t - k[i].t);
    return a * b <= 0 ? 0 : 2 * a * b / (a + b);
}

static double
relaxcurve(const RelaxKey *k, int n, double t)
{
    double h, u, u2, u3, m0, m1;
    int i;

    if (t <= k[0].t)
        return k[0].v;
    if (t >= k[n-1].t)
        return k[n-1].v;
    for (i = 0; t > k[i+1].t; i++);
    h = k[i+1].t - k[i].t;
    u = (t - k[i].t) / h;
    u2 = u * u;
    u3 = u2 * u;
    m0 = relaxtangent(k, n, i) * h;
    m1 = relaxtangent(k, n, i + 1) * h;
    return (2*u3 - 3*u2 + 1) * k[i].v + (u3 - 2*u2 + u) * m0
        + (-2*u3 + 3*u2) * k[i+1].v + (u3 - u2) * m1;
}
#define RELAXCURVE(k, t) relaxcurve(k, LENGTH(k), t)

/* 可复现的伪随机数 [0, 1) */
static double
relaxhash(unsigned int x)
{
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return (x & 0xffffff) / 16777216.0;
}

static RelaxVec relaxv(double x, double y, double z) { return (RelaxVec){x, y, z}; }
static RelaxVec relaxadd(RelaxVec a, RelaxVec b) { return relaxv(a.x + b.x, a.y + b.y, a.z + b.z); }
static RelaxVec relaxsub(RelaxVec a, RelaxVec b) { return relaxv(a.x - b.x, a.y - b.y, a.z - b.z); }
static RelaxVec relaxscale(RelaxVec a, double s) { return relaxv(a.x * s, a.y * s, a.z * s); }
static double relaxdot(RelaxVec a, RelaxVec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static double relaxlen(RelaxVec a) { return sqrt(relaxdot(a, a)); }

static RelaxVec
relaxcross(RelaxVec a, RelaxVec b)
{
    return relaxv(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

static RelaxVec
relaxnormalize(RelaxVec a)
{
    double l = relaxlen(a);
    return l > 1e-12 ? relaxscale(a, 1 / l) : relaxv(0, 0, 1);
}

static RelaxVec
relaxlerp(RelaxVec a, RelaxVec b, double t)
{
    return relaxadd(a, relaxscale(relaxsub(b, a), t));
}

/* 二次 Bezier: 3D 曲线轨迹 */
static RelaxVec
relaxbezier(RelaxVec a, RelaxVec c, RelaxVec b, double t)
{
    return relaxlerp(relaxlerp(a, c, t), relaxlerp(c, b, t), t);
}

static RelaxMat
relaxrotx(double a)
{
    double c = cos(a), s = sin(a);
    return (RelaxMat){{{1, 0, 0}, {0, c, -s}, {0, s, c}}};
}

static RelaxMat
relaxroty(double a)
{
    double c = cos(a), s = sin(a);
    return (RelaxMat){{{c, 0, s}, {0, 1, 0}, {-s, 0, c}}};
}

static RelaxMat
relaxrotz(double a)
{
    double c = cos(a), s = sin(a);
    return (RelaxMat){{{c, -s, 0}, {s, c, 0}, {0, 0, 1}}};
}

static RelaxMat
relaxmul(RelaxMat a, RelaxMat b)
{
    RelaxMat r;
    int i, j;

    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
    return r;
}

static RelaxVec
relaxapply(RelaxMat m, RelaxVec v)
{
    return relaxv(m.m[0][0] * v.x + m.m[0][1] * v.y + m.m[0][2] * v.z,
                  m.m[1][0] * v.x + m.m[1][1] * v.y + m.m[1][2] * v.z,
                  m.m[2][0] * v.x + m.m[2][1] * v.y + m.m[2][2] * v.z);
}

static RelaxMat
relaxtranspose(RelaxMat a)
{
    RelaxMat r;
    int i, j;

    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            r.m[i][j] = a.m[j][i];
    return r;
}

/* yaw (y) -> pitch (x) -> roll (z), 俯仰角不会接近 90°, 不会出现万向节锁 */
static RelaxMat
relaxeuler(double rx, double ry, double rz)
{
    return relaxmul(relaxroty(ry), relaxmul(relaxrotx(rx), relaxrotz(rz)));
}

static RelaxQuat
relaxquat(RelaxMat a)
{
    double (*m)[3] = a.m, t = m[0][0] + m[1][1] + m[2][2], s;

    if (t > 0) {
        s = sqrt(t + 1) * 2;
        return (RelaxQuat){.25 * s, (m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s};
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        s = sqrt(1 + m[0][0] - m[1][1] - m[2][2]) * 2;
        return (RelaxQuat){(m[2][1] - m[1][2]) / s, .25 * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s};
    } else if (m[1][1] > m[2][2]) {
        s = sqrt(1 + m[1][1] - m[0][0] - m[2][2]) * 2;
        return (RelaxQuat){(m[0][2] - m[2][0]) / s, (m[0][1] + m[1][0]) / s, .25 * s, (m[1][2] + m[2][1]) / s};
    }
    s = sqrt(1 + m[2][2] - m[0][0] - m[1][1]) * 2;
    return (RelaxQuat){(m[1][0] - m[0][1]) / s, (m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, .25 * s};
}

static RelaxMat
relaxquatmat(RelaxQuat q)
{
    double w = q.w, x = q.x, y = q.y, z = q.z;

    return (RelaxMat){{
        {1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
        {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
        {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}}};
}

/* 两个朝向之间的插值: 四元数球面插值, 任意角度差都稳定 */
static RelaxMat
relaxblend(RelaxMat a, RelaxMat b, double t)
{
    RelaxQuat qa = relaxquat(a), qb = relaxquat(b), q;
    double d = qa.w * qb.w + qa.x * qb.x + qa.y * qb.y + qa.z * qb.z, th, sa, sb, l;

    if (t <= 0)
        return a;
    if (t >= 1)
        return b;
    if (d < 0) {
        qb = (RelaxQuat){-qb.w, -qb.x, -qb.y, -qb.z};
        d = -d;
    }
    if (d > .9995) {
        sa = 1 - t;
        sb = t;
    } else {
        th = acos(d);
        sa = sin((1 - t) * th) / sin(th);
        sb = sin(t * th) / sin(th);
    }
    q = (RelaxQuat){sa * qa.w + sb * qb.w, sa * qa.x + sb * qb.x, sa * qa.y + sb * qb.y, sa * qa.z + sb * qb.z};
    l = sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    return relaxquatmat((RelaxQuat){q.w / l, q.x / l, q.y / l, q.z / l});
}

/* ---------- 镜头与投影 ---------- */

static RelaxProj
relaxproject(RelaxVec v)
{
    RelaxCamera *cam = &relaxscene.cam;
    RelaxProj p = {0};
    RelaxVec c = relaxapply(cam->view, relaxsub(v, cam->pos));

    if (c.z < cam->near || c.z > cam->far)
        return p;
    p.scale = cam->focal / c.z;
    p.x = relaxscene.w * .5 + c.x * p.scale;
    p.y = relaxscene.h * .5 + c.y * p.scale;
    p.z = c.z;
    p.ok = 1;
    return p;
}

/* 深度决定亮度和模糊: 近处更亮更清晰, 远处更暗更模糊 */
static double
relaxdepthlight(double z)
{
    double l = 1.22 - .5 * z / relaxscene.cam.dist;
    return l < .25 ? .25 : l > 1.15 ? 1.15 : l;
}

static double
relaxdepthblur(double z)
{
    return relaxclamp((z / relaxscene.cam.dist - 1.05) / .5);
}

/* 镜头绕星系群中心运动: 距离 + 偏航 / 俯仰 / 翻滚 (度) */
static void
relaxsetcamera(double dist, double pitch, double yaw, double roll)
{
    RelaxCamera *cam = &relaxscene.cam;
    double d2r = RELAXPI / 180;
    RelaxMat orbit;

    cam->dist = dist;
    cam->rx = pitch * d2r;
    cam->ry = yaw * d2r;
    cam->rz = roll * d2r;
    cam->target = relaxv(0, 0, 0);
    orbit = relaxmul(relaxroty(cam->ry), relaxrotx(cam->rx));
    cam->pos = relaxadd(cam->target, relaxapply(orbit, relaxv(0, 0, -cam->dist)));
    cam->rot = relaxmul(orbit, relaxrotz(cam->rz));
    cam->view = relaxtranspose(cam->rot);
}

/* 关键帧镜头 + 驻留时的缓慢巡航 + 鼠标视差 + 滚轮推拉 (后三者乘 holdw, 开场和坍缩时为 0) */
static void
relaxupdatecamera(double stage, double motion)
{
    RelaxScene *r = &relaxscene;
    double hw = r->holdw, tau = 2 * RELAXPI;

    relaxsetcamera(RELAXCURVE(relaxcamdist, stage) * r->cam.focal
                * (1 + .04 * hw * sin(tau * motion / 27)) * relaxmix(1, r->zoom, hw),
            RELAXCURVE(relaxcampitch, stage) + hw * (6 * sin(tau * motion / 23 + 1) + r->ppitch),
            RELAXCURVE(relaxcamyaw, stage) + hw * (18 * sin(tau * motion / 35) + r->pyaw),
            RELAXCURVE(relaxcamroll, stage));
}

/* ---------- 世界: 全局旋转 / 星系 / 星体 (都是 (stage, motion) 的纯函数, 尾迹直接回溯时间求值) ---------- */

static double
relaxramp(double x, double r)
{
    return x <= 0 ? 0 : x < r ? x * x / (2 * r) : x - r / 2;
}

/* 驻留时每 RELAXCRUISE 秒一次巡航扫掠: 整个星系群再转 100°, 反复展示近大远小 / 遮挡 / 轨道侧面 */
static double
relaxcruise(double motion)
{
    double x = motion - (RELAXHOLD + 6), k;

    if (x <= 0)
        return 0;
    k = floor(x / RELAXCRUISE);
    return 1.75 * (k + relaxeaseinoutcubic((x - k * RELAXCRUISE) / 2.4));
}

static RelaxMat
relaxworldat(double stage, double motion)
{
    double u = relaxphase(stage, 2.2, 3.3);
    /* 2.2s 静止 -> 加速 -> 约 2.85s 峰值 (≈1.5 转/秒) -> 3.3s 减速到几乎停止, 之后保持缓慢漂移和周期巡航 */
    double theta = .09 * relaxramp(motion - 1, .8) + 2 * RELAXPI * relaxeaseinoutcubic(pow(u, 1.25)) + relaxcruise(motion);
    double tilt = .1 * relaxsmoothstep(relaxphase(stage, 1, 2.2)) + .2 * sin(RELAXPI * u)
        - .1 * relaxsmoothstep(relaxphase(stage, 4.8, 5.5)) + .1 * relaxscene.holdw * sin(2 * RELAXPI * motion / 40);
    return relaxmul(relaxrotx(tilt), relaxmul(relaxroty(theta), relaxrotz(.05 * sin(RELAXPI * u))));
}

static void
relaxgalaxyat(RelaxGalaxy *g, double stage, double motion, RelaxMat world, RelaxVec *pos, RelaxMat *plane)
{
    RelaxScene *r = &relaxscene;
    double emerge = relaxeaseoutcubic(relaxphase(stage, .6, 1));
    double expand = relaxeaseinoutcubic(relaxphase(stage, 1.5, 2.2));
    double side = g->tag % 2 ? 1 : -1;
    double merge = relaxeaseinoutcubic(relaxphase(stage, 5 + .03 * (g->tag % 4), 5.45));
    RelaxVec p, ctrl;

    p = relaxv(g->home.x * relaxmix(.62, 1, expand), g->home.y * relaxmix(.62, 1, expand),
            g->home.z * relaxmix(.45, 1, expand) + (1 - emerge) * 1.2 * r->cam.focal);
    p = relaxadd(p, relaxv(0, .012 * r->h * sin(.9 * motion + g->phase), .02 * r->cam.focal * sin(.6 * motion + 1.3 * g->phase)));
    if (merge > 0) {
        /* 星系核心沿各自方向的 3D 曲线汇聚到中心 */
        ctrl = relaxadd(relaxscale(relaxapply(relaxroty(1.1 * side), p), .8), relaxv(0, -.2 * r->h * side, 0));
        p = relaxbezier(p, ctrl, relaxv(0, 0, 0), merge);
    }
    *pos = relaxapply(world, p);
    *plane = relaxmul(world, relaxeuler(g->rx, g->ry + g->precess * motion, g->rz));
}

/* 星体当前的轨道角 */
static double
relaxorbitangle(RelaxStar *s, double stage, double motion)
{
    double c = relaxeaseincubic(relaxphase(stage, 4.8 + s->delay * .5, 5.2 + s->delay * .5));
    return s->angle + s->speed * (motion - .6) + 2.4 * c * (s->speed < 0 ? -1 : 1);
}

/* ringplane: 星体所在轨道环的平面 (星系轨道平面 x 环自身的倾斜), 星体严格在画出的环上运行 */
static void
relaxstarat(RelaxStar *s, double stage, double motion, RelaxMat world, RelaxVec gpos, RelaxMat ringplane,
        RelaxVec *pos, RelaxMat *orient)
{
    RelaxScene *r = &relaxscene;
    RelaxGalaxy *g = &r->galaxies[s->galaxy];
    double F = r->cam.focal, dir = s->speed < 0 ? -1 : 1;
    double c = relaxeaseincubic(relaxphase(stage, 4.8 + s->delay * .5, 5.2 + s->delay * .5));
    double rad = s->radius * (1 - c), a = relaxorbitangle(s, stage, motion);
    double u1 = s->current ? relaxeaseinoutcubic(relaxphase(stage, .1, .58)) : 1;
    double u2 = relaxeaseinoutcubic(relaxphase(stage, .6 + s->delay, 1.5));
    RelaxVec orbit, start, ctrl, d;
    RelaxMat od, oo;

    orbit = relaxadd(gpos, relaxapply(ringplane, relaxv(cos(a) * rad, sin(a) * rad, 0)));
    start = s->current ? relaxlerp(s->home, s->detach, u1) : s->detach;
    od = relaxeuler(s->drx * u1, s->dry * u1, s->drz * u1);
    if (u2 <= 0) {
        *pos = start;
        *orient = od;
        return;
    }
    d = relaxsub(orbit, s->detach);
    ctrl = relaxadd(relaxscale(relaxadd(s->detach, orbit), .5), relaxv(-d.y * .25 * dir, d.x * .25 * dir, -.35 * F));
    *pos = relaxbezier(s->detach, ctrl, orbit, u2);
    oo = relaxmul(world, relaxeuler(.55 * g->rx + .25 * sin(a), .55 * (g->ry + g->precess * motion) + .3 * cos(a), .4 * g->rz));
    *orient = relaxblend(od, oo, u2);
}

static void
relaxupdategalaxies(double stage, double motion, double dt)
{
    RelaxScene *r = &relaxscene;
    RelaxGalaxy *g;
    double appear = relaxeaseoutcubic(relaxphase(stage, .6, 1.1));
    double absorb = relaxeaseinoutcubic(relaxphase(stage, 4.9, 5.3));
    double merge = relaxeaseincubic(relaxphase(stage, 5.05, 5.45));
    int i;

    for (i = 0; i < r->ntags; i++) {
        g = &r->galaxies[i];
        relaxgalaxyat(g, stage, motion, r->world, &g->pos, &g->plane);
        g->alpha = appear * (1 - .9 * merge) * (g->nstars ? 1 : .55) * (1 + .35 * absorb * (g->nstars > 0));
        g->hover = relaxfollow(g->hover, r->hovercore == i, dt, .12);
        g->p = relaxproject(g->pos);
    }
}

/* 轨道环: 每条环采样 RELAXSEG 段投影到屏幕; 按镜头空间深度分成后半圈 / 前半圈, 分别参与深度排序 */
static void
relaxupdaterings(double shrink)
{
    RelaxScene *r = &relaxscene;
    RelaxGalaxy *g;
    RelaxProj *pts;
    RelaxMat m;
    double th, rad, z;
    int i, k, j, half;

    for (i = 0; i < r->ntags; i++) {
        g = &r->galaxies[i];
        for (k = 0; k < g->nrings; k++) {
            g->ringn[k][0] = g->ringn[k][1] = 0;
            g->ringz[k][0] = g->ringz[k][1] = 0;
            if (r->ringalpha < .003 || !g->p.ok)
                continue;
            m = relaxmul(g->plane, g->ring[k]);
            rad = g->ringr[k] * (1 - shrink);
            pts = r->rpts + (i * RELAXRINGS + k) * (RELAXSEG + 1);
            for (j = 0; j <= RELAXSEG; j++) {
                th = 2 * RELAXPI * j / RELAXSEG;
                pts[j] = relaxproject(relaxadd(g->pos, relaxapply(m, relaxv(cos(th) * rad, sin(th) * rad, 0))));
            }
            for (j = 0; j < RELAXSEG; j++) {
                if (!pts[j].ok || !pts[j + 1].ok)
                    continue;
                z = (pts[j].z + pts[j + 1].z) * .5;
                half = z < g->p.z;
                g->ringz[k][half] += z;
                g->ringn[k][half]++;
            }
            for (half = 0; half < 2; half++)
                if (g->ringn[k][half])
                    g->ringz[k][half] /= g->ringn[k][half];
        }
    }
}

static void
relaxupdatestars(double stage, double motion, double dt)
{
    RelaxScene *r = &relaxscene;
    RelaxStar *s;
    RelaxGalaxy *g;
    RelaxVec prev;
    RelaxMat bb;
    double size = RELAXCURVE(relaxcardsize, stage), vis = RELAXCURVE(relaxcardvis, stage);
    double tint = RELAXCURVE(relaxcardtint, stage), glow = RELAXCURVE(relaxstarglow, stage);
    double hb = relaxsmoothstep(relaxphase(stage, 3.3, 4.2)), collapse, a;
    int i;

    for (i = 0; i < r->nstars; i++) {
        s = &r->stars[i];
        g = &r->galaxies[s->galaxy];
        prev = s->pos;
        relaxstarat(s, stage, motion, r->world, g->pos, relaxmul(g->plane, g->ring[s->ring]), &s->pos, &s->orient);
        if (hb > 0) {
            /* 驻留时卡片大体朝向镜头 (截图可辨认), 随轨道位置摆动 ±26° / ±16°, 保留立体感 */
            a = relaxorbitangle(s, stage, motion);
            bb = relaxmul(r->cam.rot, relaxeuler(.28 * sin(a + s->rock), .45 * cos(a), 0));
            s->orient = relaxblend(s->orient, bb, hb);
        }
        s->hover = relaxfollow(s->hover, r->hover == i, dt, .12);
        if (s->hover > .001)  /* 悬停: 稍微靠近镜头 */
            s->pos = relaxadd(s->pos, relaxscale(relaxnormalize(relaxsub(r->cam.pos, s->pos)), .06 * r->cam.focal * s->hover));
        s->vel = dt > 0 ? relaxscale(relaxsub(s->pos, prev), 1 / dt) : relaxv(0, 0, 0);
        s->size = size * (s->focused ? 1.08 : 1) * (1 + .12 * s->hover);
        collapse = relaxphase(stage, 4.8 + s->delay * .5, 5.2 + s->delay * .5);
        /* 当前桌面的窗口从第一帧起就画在自己原来的位置上 (与真实窗口逐像素重合), 背后的桌面截图先淡出再开始移动,
         * 移动中的窗口不会和静止的桌面截图叠成重影 */
        s->alpha = (s->current ? 1 : relaxeaseoutquart(relaxphase(stage, .6, 1)))
            * (1 - relaxeaseincubic(relaxphase(collapse, .7, 1)));
        s->p = relaxproject(s->pos);
        s->brightness = s->p.ok ? relaxdepthlight(s->p.z) * (s->focused ? 1.1 : 1) * (1 + .25 * s->hover) : 0;
        if (s->current)  /* 脱离桌面前与背景一起变暗 (100% -> 70%), 之后过渡到深度亮度 */
            s->brightness = relaxmix(RELAXCURVE(relaxbright, stage), s->brightness, relaxsmoothstep(relaxphase(stage, .1, .58)));
        /* 深度 LOD: 投影后的卡片足够大才显示截图面板, 远处只剩光点 */
        s->lod = s->p.ok ? relaxsmoothstep((s->w * s->size * s->p.scale - 40) / 110) : 0;
        s->vis = MAX(vis * s->lod, .85 * s->hover * (vis > .05)) * s->alpha;
        s->tint = (tint + .1 * s->hover) * s->alpha * MIN(1, s->vis / .3);  /* 发光随面板一起消失, 不留白色方块 */
        s->glow = glow * s->alpha * MIN(1, s->brightness);
    }
}

static int
relaxitemcmp(const void *pa, const void *pb)
{
    const RelaxItem *a = pa, *b = pb;
    return a->z > b->z ? -1 : a->z < b->z;
}

/* 画家算法: 每帧按镜头空间 z 从远到近排序 (轨道环的前后半圈各是一个对象, 能与核心和星体互相遮挡) */
static void
relaxsortdepth(void)
{
    RelaxScene *r = &relaxscene;
    RelaxGalaxy *g;
    int i, k, half, n = 0;

    for (i = 0; i < r->ndust; i++)
        if (r->dust[i].p.ok)
            r->items[n++] = (RelaxItem){RelaxDustItem, i, r->dust[i].p.z};
    for (i = 0; i < r->ntags; i++) {
        g = &r->galaxies[i];
        if (g->p.ok && g->alpha > .002)
            r->items[n++] = (RelaxItem){RelaxCoreItem, i, g->p.z};
        if (r->ringalpha * g->alpha < .003)
            continue;
        for (k = 0; k < g->nrings; k++)
            for (half = 0; half < 2; half++)
                if (g->ringn[k][half])
                    r->items[n++] = (RelaxItem){RelaxRingItem, (i * RELAXRINGS + k) * 2 + half, g->ringz[k][half]};
    }
    for (i = 0; i < r->nstars; i++)
        if (r->stars[i].p.ok && r->stars[i].alpha > .002)
            r->items[n++] = (RelaxItem){RelaxStarItem, i, r->stars[i].p.z};
    r->nitems = n;
    qsort(r->items, n, sizeof *r->items, relaxitemcmp);
}

/* 开场 / 驻留 / 坍缩: 整个场景由 (stage, motion) 决定; dt 是真实时间, 只用于平滑交互 */
static void
relaxupdatescene(double stage, double motion, double dt)
{
    RelaxScene *r = &relaxscene;
    int orbit = r->mode == RelaxOrbit, i;

    r->holdw = relaxsmoothstep(relaxphase(motion, RELAXHOLD - .5, RELAXHOLD + 2.5))
        * (1 - relaxsmoothstep(relaxphase(stage, RELAXEXIT, RELAXEXIT + .5)));
    r->pyaw = relaxfollow(r->pyaw, orbit ? r->tyaw : 0, dt, .35);
    r->ppitch = relaxfollow(r->ppitch, orbit ? r->tpitch : 0, dt, .35);
    r->zoom = relaxfollow(r->zoom, orbit ? r->tzoom : 1, dt, .25);
    relaxupdatecamera(stage, motion);
    r->world = relaxworldat(stage, motion);
    relaxupdategalaxies(stage, motion, dt);
    r->ringalpha = RELAXCURVE(relaxringkeys, stage);
    relaxupdaterings(relaxeaseincubic(relaxphase(stage, 4.8, 5.3)));
    relaxupdatestars(stage, motion, dt);
    for (i = 0; i < r->ndust; i++)
        r->dust[i].p = relaxproject(r->dust[i].pos);
    r->bright = RELAXCURVE(relaxbright, stage);
    r->vign = RELAXCURVE(relaxvignettekeys, stage);
    r->desk = 1 - relaxsmoothstep(relaxphase(stage, 0, .1));
    r->deskover = r->bar = 0;
    r->reveal = relaxeaseinoutcubic(relaxphase(stage, 5.7, 6));
    r->trailgain = RELAXCURVE(relaxtrailgain, stage);
    r->dustfade = relaxeaseoutcubic(relaxphase(stage, .5, 1.3)) * (1 - relaxeaseinoutcubic(relaxphase(stage, 5, 5.7)));
    r->central = relaxeaseoutcubic(relaxphase(stage, 5.15, 5.45)) * (1 - relaxeaseinoutcubic(relaxphase(stage, 5.7, 6)));
    r->pulse = sin(RELAXPI * relaxphase(stage, 5.5, 5.7));
    relaxsortdepth();
}

/* 回程: 从冻结的画面出发, 目标 tag 的窗口沿 3D 曲线飞回原位置变回截图, 其他星体退向深处, 镜头回正 */
static void
relaxupdatereturn(double u)
{
    RelaxScene *r = &relaxscene;
    RelaxStar *s;
    RelaxGalaxy *g;
    RelaxVec ctrl, d;
    RelaxMat id = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    double F = r->cam.focal, e = relaxeaseinoutcubic(relaxphase(u, 0, .8)), fade = 1 - relaxsmoothstep(relaxphase(u, 0, .45));
    double v, away;
    int i;

    relaxsetcamera(relaxmix(r->rcdist, F, e), relaxmix(r->rcx, 0, e), relaxmix(r->rcy, 0, e), relaxmix(r->rcz, 0, e));
    for (i = 0; i < r->ntags; i++) {
        g = &r->galaxies[i];
        g->pos = g->rpos;
        g->alpha = g->ralpha * fade;
        g->p = relaxproject(g->pos);
    }
    r->ringalpha = r->rring * fade;
    relaxupdaterings(0);
    for (i = 0; i < r->nstars; i++) {
        s = &r->stars[i];
        if (s->back) {
            v = relaxeaseinoutcubic(relaxphase(u, s->delay, .8 + s->delay * .5));
            d = relaxsub(s->home, s->rpos);
            /* 控制点: 中点向镜头拉近, 并沿 (行进方向 x 视线) 侧偏, 轨迹是一条 3D 弧线 */
            ctrl = relaxadd(relaxscale(relaxadd(s->rpos, s->home), .5),
                    relaxadd(relaxscale(relaxcross(d, relaxv(0, 0, 1)), -.15), relaxv(0, 0, -.1 * F)));
            s->pos = relaxbezier(s->rpos, ctrl, s->home, v);
            s->orient = relaxblend(s->rorient, id, v);
            s->size = relaxmix(s->rsize, 1, v);
            s->alpha = 1;
            s->vis = relaxmix(s->rvis, 1, relaxsmoothstep(relaxphase(u, 0, .5)));
            s->tint = s->rtint * (1 - v);
            s->glow = s->rglow * (1 - relaxsmoothstep(relaxphase(u, 0, .6)));
            s->brightness = relaxmix(s->rbright, 1, v);
        } else {
            away = relaxeaseincubic(relaxphase(u, 0, .6));
            s->pos = relaxadd(s->rpos, relaxscale(relaxnormalize(relaxsub(s->rpos, r->rcampos)), 2 * F * away));
            s->orient = s->rorient;
            s->size = s->rsize;
            s->alpha = 1 - relaxsmoothstep(relaxphase(u, 0, .55));
            s->vis = s->rvis * s->alpha;
            s->tint = s->rtint * s->alpha;
            s->glow = s->rglow * s->alpha;
            s->brightness = s->rbright;
        }
        s->p = relaxproject(s->pos);
    }
    for (i = 0; i < r->ndust; i++)
        r->dust[i].p = relaxproject(r->dust[i].pos);
    r->dustfade = r->rdust * fade;
    r->bright = relaxmix(r->rbright, 1, relaxeaseinoutcubic(relaxphase(u, .1, .85)));
    r->vign = r->rvign * (1 - relaxsmoothstep(relaxphase(u, 0, .7)));
    r->desk = r->rdesk * (1 - relaxsmoothstep(relaxphase(u, 0, .3)));
    /* 最后 20%: 回到原 tag 时交叉淡入开始时截的桌面 (结束画面与真实桌面一致), 否则只淡入状态栏 */
    r->deskover = r->fulldesk ? relaxsmoothstep(relaxphase(u, .8, 1)) : 0;
    r->bar = r->fulldesk ? 0 : relaxsmoothstep(relaxphase(u, .75, 1));
    r->reveal = r->trailgain = r->central = r->pulse = 0;
    relaxsortdepth();
}

/* ---------- XRender 绘制 ---------- */

static Picture relaxwhite(double a) { return relaxscene.white[(int)(relaxclamp(a) * (RELAXALPHAS - 1) + .5)]; }
static Picture relaxblack(double a) { return relaxscene.black[(int)(relaxclamp(a) * (RELAXALPHAS - 1) + .5)]; }

static void
relaxaffine(Picture p, double sx, double sy, double tx, double ty)
{
    XTransform tr = {{
        {XDoubleToFixed(sx), 0, XDoubleToFixed(tx)},
        {0, XDoubleToFixed(sy), XDoubleToFixed(ty)},
        {0, 0, XDoubleToFixed(1)}}};
    XRenderSetPictureTransform(dpy, p, &tr);
}

/* 光点精灵: 亚像素位置用变换的平移表示, 慢速运动不会逐像素跳动 */
static void
relaxsprite(int shape, int tint, double x, double y, double radius, double alpha)
{
    RelaxScene *r = &relaxscene;
    double d = radius * 2, s;
    int lvl, x0, y0, size;

    if (alpha < 1.0 / 255 || radius < .25 || x + radius < 0 || y + radius < 0
            || x - radius > r->w || y - radius > r->h)
        return;
    lvl = d > 24 ? 0 : d > 6 ? 1 : 2;
    s = relaxspritesize[lvl] / d;
    x0 = (int)floor(x - radius) - 1;
    y0 = (int)floor(y - radius) - 1;
    size = (int)ceil(d) + 3;
    relaxaffine(r->sprite[shape][tint][lvl], s, s, -(x - radius - x0) * s, -(y - radius - y0) * s);
    XRenderComposite(dpy, PictOpOver, r->sprite[shape][tint][lvl], relaxwhite(alpha), r->back,
            0, 0, 0, 0, x0, y0, size, size);
}

static void
relaxinvert(double m[3][3], double o[3][3])
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
relaxsquaretoquad(double q[4][2], double m[3][3])
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
relaxhomography(Picture p, double q[4][2], int bw, int bh, int srcw, int srch)
{
    double m[3][3], inv[3][3], k = 0;
    XTransform tr;
    int i, j;

    if (!relaxsquaretoquad(q, m))
        return 0;
    relaxinvert(m, inv);
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
relaxfillquad(Picture color, double q[4][2], int ox, int oy)
{
    RelaxScene *r = &relaxscene;
    XTriangle t[2];
    XPointFixed p[4];
    int i;

    for (i = 0; i < 4; i++)
        p[i] = (XPointFixed){XDoubleToFixed(q[i][0] + ox), XDoubleToFixed(q[i][1] + oy)};
    t[0] = (XTriangle){p[0], p[1], p[2]};
    t[1] = (XTriangle){p[0], p[2], p[3]};
    XRenderCompositeTriangles(dpy, PictOpOver, color, r->back, r->a8, 0, 0, t, 2);
}

static void
relaxrenderwindow(RelaxStar *s, double vis, double tint, double light)
{
    RelaxScene *r = &relaxscene;
    static const double sx[4] = {-1, 1, 1, -1}, sy[4] = {-1, -1, 1, 1};
    double q[4][2], minx = 1e9, miny = 1e9, maxx = -1e9, maxy = -1e9, edge, hw, hh, dark;
    RelaxVec corner, normal, tocam;
    RelaxProj p;
    int i, lvl, x0, y0, x1, y1, cx, cy;

    if (!s->snap || (vis < .004 && tint < .004))
        return;
    hw = s->w * .5 * s->size;
    hh = s->h * .5 * s->size;
    for (i = 0; i < 4; i++) {
        corner = relaxadd(s->pos, relaxapply(s->orient, relaxv(sx[i] * hw, sy[i] * hh, 0)));
        p = relaxproject(corner);
        if (!p.ok || p.z < r->cam.focal * .25)
            return;
        q[i][0] = p.x;
        q[i][1] = p.y;
        minx = MIN(minx, p.x); maxx = MAX(maxx, p.x);
        miny = MIN(miny, p.y); maxy = MAX(maxy, p.y);
    }
    if (maxx < 0 || maxy < 0 || minx > r->w || miny > r->h || maxx - minx < 1 || maxy - miny < 1)
        return;
    if (vis > .3) {
        s->bx0 = minx; s->by0 = miny;
        s->bx1 = maxx; s->by1 = maxy;
        s->hit = 1;
    }
    /* 卡片背面朝向镜头时更暗 (单应变换自动镜像截图) */
    normal = relaxapply(s->orient, relaxv(0, 0, -1));
    tocam = relaxsub(r->cam.pos, s->pos);
    if (relaxdot(normal, tocam) < 0)
        light *= .4;
    /* 选择 mip: 源像素 / 屏幕像素 不超过 2, 远处再降一级 (景深模糊) */
    edge = MAX(hypot(q[1][0] - q[0][0], q[1][1] - q[0][1]), hypot(q[3][0] - q[0][0], q[3][1] - q[0][1]) * s->w / MAX(1, s->h));
    for (lvl = s->base; lvl < RELAXMIPS - 1 && s->mip[lvl + 1] && s->mipw[lvl] > 2 * edge; lvl++);
    if (relaxdepthblur(s->p.z) > .55 && lvl < RELAXMIPS - 1 && s->mip[lvl + 1])
        lvl++;
    x0 = (int)floor(minx);
    y0 = (int)floor(miny);
    x1 = (int)ceil(maxx) + 1;
    y1 = (int)ceil(maxy) + 1;
    for (i = 0; i < 4; i++) {
        q[i][0] -= x0;
        q[i][1] -= y0;
    }
    if (!relaxhomography(s->mip[lvl], q, x1 - x0, y1 - y0, s->mipw[lvl], s->miph[lvl]))
        return;
    cx = MAX(0, x0);
    cy = MAX(0, y0);
    x1 = MIN(r->w, x1);
    y1 = MIN(r->h, y1);
    if (x1 <= cx || y1 <= cy)
        return;
    /* 截图只做一次透视采样 (软件渲染时透视采样约是仿射的 8 倍开销); 景深变暗 / 面板发光是盖在同一四边形上的纯色 */
    if (vis >= .004)
        XRenderComposite(dpy, PictOpOver, s->mip[lvl], relaxwhite(vis), r->back,
                cx - x0, cy - y0, 0, 0, cx, cy, x1 - cx, y1 - cy);
    dark = (1 - MIN(1, light)) * vis;
    if (dark >= .004)
        relaxfillquad(relaxblack(dark), q, x0, y0);
    if (tint >= .004)
        relaxfillquad(relaxwhite(tint * MIN(1, light * 1.1)), q, x0, y0);
}

static void
relaxrenderglow(int tint, RelaxProj p, double radius, double alpha, double blur, double halo, double outer)
{
    double rr = radius * p.scale;

    if (outer > 0)
        relaxsprite(RelaxHalo, tint, p.x, p.y, rr * 7 * (1 + .3 * blur), outer * alpha);
    relaxsprite(RelaxHalo, tint, p.x, p.y, rr * (3 + .8 * blur), halo * alpha);
    relaxsprite(RelaxDisc, tint, p.x, p.y, MAX(.7, rr), alpha * (1 - .5 * blur));
}

/* 细光带 (轨道环 / 尾迹): 按透明度分桶, 每桶一次 XRenderCompositeTriangles (a8 遮罩, 重叠处不会叠亮) */
static void
relaxband(RelaxProj *pa, RelaxProj *pb, double a, double hw)
{
    RelaxScene *r = &relaxscene;
    double len, nx, ny;
    XTriangle *tri;
    int b = (int)(a / .25 * RELAXBUCKETS + .5);

    if (b < 1)
        return;
    b = MIN(b, RELAXBUCKETS) - 1;
    len = hypot(pb->x - pa->x, pb->y - pa->y);
    if (len < .3 || r->ntris[b] + 2 > r->triscap)
        return;
    nx = -(pb->y - pa->y) / len * hw;
    ny = (pb->x - pa->x) / len * hw;
    tri = &r->tris[b][r->ntris[b]];
    tri[0].p1 = (XPointFixed){XDoubleToFixed(pa->x + nx), XDoubleToFixed(pa->y + ny)};
    tri[0].p2 = (XPointFixed){XDoubleToFixed(pa->x - nx), XDoubleToFixed(pa->y - ny)};
    tri[0].p3 = (XPointFixed){XDoubleToFixed(pb->x - nx), XDoubleToFixed(pb->y - ny)};
    tri[1].p1 = tri[0].p1;
    tri[1].p2 = tri[0].p3;
    tri[1].p3 = (XPointFixed){XDoubleToFixed(pb->x + nx), XDoubleToFixed(pb->y + ny)};
    r->ntris[b] += 2;
}

static void
relaxflushbands(void)
{
    RelaxScene *r = &relaxscene;
    int b;

    for (b = 0; b < RELAXBUCKETS; b++)
        if (r->ntris[b]) {
            XRenderCompositeTriangles(dpy, PictOpOver, relaxwhite(.25 * (b + 1) / RELAXBUCKETS), r->back,
                    r->a8, 0, 0, r->tris[b], r->ntris[b]);
            r->ntris[b] = 0;
        }
}

/* 一条轨道环的后半圈 (half=0) 或前半圈 (half=1): 透视下正面是圆, 斜看是椭圆, 侧看接近一条线;
 * 线宽随透视缩放, 亮度随深度, 前半圈更亮 */
static void
relaxrenderring(int index)
{
    RelaxScene *r = &relaxscene;
    int gi = index / (2 * RELAXRINGS), k = index / 2 % RELAXRINGS, half = index % 2, j;
    RelaxGalaxy *g = &r->galaxies[gi];
    RelaxProj *pts = r->rpts + (gi * RELAXRINGS + k) * (RELAXSEG + 1);
    double base = r->ringalpha * MIN(1, g->alpha) * (half ? 1.25 : .6) * (1 + .6 * g->hover), z;

    for (j = 0; j < RELAXSEG; j++) {
        if (!pts[j].ok || !pts[j + 1].ok)
            continue;
        z = (pts[j].z + pts[j + 1].z) * .5;
        if ((z < g->p.z) != half)
            continue;
        relaxband(&pts[j], &pts[j + 1], base * relaxdepthlight(z),
                MAX(.75, (pts[j].scale + pts[j + 1].scale) * .5 * r->starscale));
    }
    relaxflushbands();
}

static void
relaxrenderstar(RelaxStar *s)
{
    RelaxScene *r = &relaxscene;
    double blur = relaxdepthblur(s->p.z), rad, a;

    /* 截图面板背后的柔光, 让面板像发光体而不是贴图 */
    if (s->vis > .02 && s->glow > .01)
        relaxsprite(RelaxHalo, RelaxCool, s->p.x, s->p.y, .62 * hypot(s->w, s->h) * s->size * s->p.scale,
                .16 * r->glowscale * s->glow * MIN(1, s->vis * 1.3));
    relaxrenderwindow(s, s->vis, s->tint, s->brightness);
    a = s->glow * (1 - .7 * MIN(1, s->vis * 1.5));
    rad = 8.5 * r->starscale * (s->focused ? 1.18 : 1) * (1 + .3 * s->hover);
    if (a > .002)
        relaxrenderglow(RelaxCool, s->p, rad, a, blur, .5 * r->glowscale * (1 + .5 * s->hover), 0);
    if (!s->hit && (a > .05 || s->vis > .05)) {
        rad = MAX(16, 3 * rad * s->p.scale);
        s->bx0 = s->p.x - rad; s->by0 = s->p.y - rad;
        s->bx1 = s->p.x + rad; s->by1 = s->p.y + rad;
        s->hit = 1;
    }
}

static void
relaxrenderitems(void)
{
    RelaxScene *r = &relaxscene;
    RelaxGalaxy *g;
    RelaxDust *d;
    double a;
    int i;

    for (i = 0; i < r->nstars; i++)
        r->stars[i].hit = 0;
    for (i = 0; i < r->ntags; i++)
        r->galaxies[i].hit = 0;
    for (i = 0; i < r->nitems; i++) {
        switch (r->items[i].kind) {
        case RelaxDustItem:
            d = &r->dust[r->items[i].index];
            a = d->light * r->dustfade * relaxsmoothstep(relaxphase(d->p.z, r->cam.near * 1.5, r->cam.near * 3));
            relaxsprite(RelaxHalo, RelaxCool, d->p.x, d->p.y, MAX(.6, d->size * d->p.scale), a);
            break;
        case RelaxCoreItem:
            g = &r->galaxies[r->items[i].index];
            relaxrenderglow(RelaxWarm, g->p, g->size * (1 + .15 * g->hover), MIN(1, g->alpha * relaxdepthlight(g->p.z)),
                    relaxdepthblur(g->p.z), .55 * (1 + .4 * g->hover), .22 * r->glowscale);
            if (g->alpha > .2) {
                g->hx = g->p.x;
                g->hy = g->p.y;
                g->hr = MAX(18, 1.3 * g->size * g->p.scale);
                g->hit = 1;
            }
            break;
        case RelaxStarItem:
            relaxrenderstar(&r->stars[r->items[i].index]);
            break;
        case RelaxRingItem:
            relaxrenderring(r->items[i].index);
            break;
        }
    }
}

/* 低透明度的尾迹: 回溯时间求出星体之前的 3D 位置, 用当前镜头投影.
 * 高速旋转时是短促的流光, 驻留时拉长成沿轨道的彗星弧, 指示运动方向 */
static void
relaxrendertrails(void)
{
    RelaxScene *r = &relaxscene;
    double gain = r->trailgain, dt, tk, mk, rate, a, speed;
    int i, k, n = r->ntrail, stride = r->ntrail + 1;
    RelaxProj *pa, *pb;
    RelaxGalaxy *g;
    RelaxMat world, orient;
    RelaxVec pos;

    if (gain < .01 || n < 1)
        return;
    /* 尾迹覆盖的总时长: 高速旋转时 0.12s (短促流光), 驻留时 1s (沿轨道的彗星弧) */
    dt = relaxmix(.12, 1, relaxsmoothstep(relaxphase(r->stage, 3.3, 4.2))) / n;
    rate = r->mode == RelaxOrbit ? 0 : 1;   /* 驻留时 stage 停住, 只有 motion 在走 */
    for (k = 0; k <= n; k++) {
        tk = r->stage - k * dt * rate;
        mk = r->motion - k * dt;
        world = relaxworldat(tk, mk);
        for (i = 0; i < r->ntags; i++) {
            relaxgalaxyat(&r->galaxies[i], tk, mk, world, &r->tgpos[i], &r->tgplane[i]);
            r->tpts[(r->nstars + i) * stride + k] = r->stage >= 4.9 ? relaxproject(r->tgpos[i]) : (RelaxProj){0};
        }
        for (i = 0; i < r->nstars; i++) {
            g = &r->galaxies[r->stars[i].galaxy];
            relaxstarat(&r->stars[i], tk, mk, world, r->tgpos[r->stars[i].galaxy],
                    relaxmul(r->tgplane[r->stars[i].galaxy], g->ring[r->stars[i].ring]), &pos, &orient);
            r->tpts[i * stride + k] = relaxproject(pos);
        }
    }
    for (i = 0; i < r->nstars + r->ntags; i++) {
        if (i < r->nstars) {
            speed = relaxlen(r->stars[i].vel) / r->cam.focal;
            a = gain * r->stars[i].alpha * MIN(1, r->stars[i].brightness) * (.12 + .2 * relaxclamp(3 * speed));
        } else {
            a = .16 * gain * r->galaxies[i - r->nstars].alpha;
        }
        for (k = 0; k < n; k++) {
            pa = &r->tpts[i * stride + k];
            pb = &r->tpts[i * stride + k + 1];
            if (!pa->ok || !pb->ok)
                break;
            relaxband(pa, pb, a * (1 - (double)k / n), MAX(.45, .9 * pa->scale * r->starscale));
        }
    }
    relaxflushbands();
}

static void
relaxrenderbackground(void)
{
    RelaxScene *r = &relaxscene;
    XRenderColor shade = {0, 0, 0, 0};
    double key[4] = {r->bright, r->vign, r->desk, r->live ? r->reveal : 0};

    /* 参数与上一帧相同 (驻留态): 直接用缓存, 省掉全屏暗角缩放和填充 */
    if (r->bg && r->bgok && !memcmp(key, r->bgkey, sizeof key)) {
        XRenderComposite(dpy, PictOpSrc, r->bg, None, r->back, 0, 0, 0, 0, 0, 0, r->w, r->h);
        return;
    }
    XRenderComposite(dpy, PictOpSrc, r->wallpaper, None, r->back, 0, 0, 0, 0, 0, 0, r->w, r->h);
    if (r->live && r->reveal > 0)
        XRenderComposite(dpy, PictOpOver, r->live, relaxwhite(r->reveal), r->back, 0, 0, 0, 0, 0, 0, r->w, r->h);
    if (r->desktop && r->desk > 0)
        XRenderComposite(dpy, PictOpOver, r->desktop, relaxwhite(r->desk), r->back, 0, 0, 0, 0, 0, 0, r->w, r->h);
    shade.alpha = (unsigned short)(65535 * relaxclamp(1 - r->bright));
    if (shade.alpha)
        XRenderFillRectangle(dpy, PictOpOver, r->back, &shade, 0, 0, r->w, r->h);
    if (r->vignette && r->vign > .004)
        XRenderComposite(dpy, PictOpOver, r->vignette, relaxwhite(r->vign), r->back, 0, 0, 0, 0, 0, 0, r->w, r->h);
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
relaxrenderfront(void)
{
    RelaxScene *r = &relaxscene;

    if (!r->desktop)
        return;
    if (r->deskover > .004)
        XRenderComposite(dpy, PictOpOver, r->desktop, relaxwhite(r->deskover), r->back, 0, 0, 0, 0, 0, 0, r->w, r->h);
    if (r->bar > .004 && r->barw > 0 && r->barh > 0)
        XRenderComposite(dpy, PictOpOver, r->desktop, relaxwhite(r->bar), r->back,
                r->barx, r->bary, 0, 0, r->barx, r->bary, r->barw, r->barh);
}

/* 所有窗口星体回归核心后, 中心只剩一个柔和光点: Stellar Pulse */
static void
relaxrendercentral(void)
{
    RelaxScene *r = &relaxscene;
    double pulse = r->pulse;
    RelaxProj p;

    if (r->central < .004)
        return;
    p = relaxproject(relaxv(0, 0, 0));
    if (!p.ok)
        return;
    relaxrenderglow(RelaxWarm, p, 15 * (1 + .12 * pulse), r->central, 0, .45 * (1 + .6 * pulse), .14 * (1 + .6 * pulse));
}

static void
relaxpresent(void)
{
    RelaxScene *r = &relaxscene;
    XRenderComposite(dpy, PictOpSrc, r->back, None, r->overlaypic, 0, 0, 0, 0, 0, 0, r->w, r->h);
}

static double
relaxnow(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec - relaxscene.start.tv_sec + (now.tv_nsec - relaxscene.start.tv_nsec) / 1e9;
}

static void
relaxrender(void)
{
    relaxrenderbackground();
    relaxrendertrails();
    relaxrenderitems();
    relaxrendercentral();
    relaxrenderfront();
    relaxpresent();
    /* 等服务器画完这一帧: 既是帧时间的真实测量, 也避免请求堆积 */
    XSync(dpy, False);
}

/* ---------- 资源 ---------- */

static Picture
relaxargb(int w, int h, Pixmap *pix)
{
    *pix = XCreatePixmap(dpy, root, MAX(1, w), MAX(1, h), 32);
    return XRenderCreatePicture(dpy, *pix, relaxscene.argb, 0, NULL);
}

static Picture
relaxopaque(int w, int h, Pixmap *pix)
{
    *pix = XCreatePixmap(dpy, root, w, h, DefaultDepth(dpy, screen));
    return XRenderCreatePicture(dpy, *pix, XRenderFindVisualFormat(dpy, DefaultVisual(dpy, screen)), 0, NULL);
}

/* 客户端算好的 ARGB 图上传成 Picture (只在开始时调用) */
static Picture
relaxupload(int w, int h, unsigned int *data, Pixmap *pix)
{
    Picture p = relaxargb(w, h, pix);
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
relaxbuildsprites(void)
{
    RelaxScene *r = &relaxscene;
    static const double tints[RelaxTints][3] = {{1, .95, .87}, {.86, .91, 1}};
    unsigned int *data;
    double x, y, d, a, c;
    int shape, tint, lvl, n, i, j;

    for (shape = 0; shape < RelaxShapes; shape++)
        for (tint = 0; tint < RelaxTints; tint++)
            for (lvl = 0; lvl < RELAXSPRITES; lvl++) {
                n = relaxspritesize[lvl];
                if (!(data = malloc(n * n * 4)))
                    continue;
                for (j = 0; j < n; j++)
                    for (i = 0; i < n; i++) {
                        x = (i + .5) / n * 2 - 1;
                        y = (j + .5) / n * 2 - 1;
                        d = sqrt(x * x + y * y);
                        if (shape == RelaxHalo)  /* 多层高斯叠加的柔光, 没有硬边 */
                            a = (.55 * exp(-d * d / .0288) + .3 * exp(-d * d / .1568) + .15 * exp(-d * d / .5))
                                * (1 - relaxsmoothstep((d - .75) / .25));
                        else
                            a = 1 - relaxsmoothstep((d - .4) / .6);
                        a = relaxclamp(a);
                        c = a * 255;
                        data[j * n + i] = (unsigned int)(c + .5) << 24
                            | (unsigned int)(c * tints[tint][0] + .5) << 16
                            | (unsigned int)(c * tints[tint][1] + .5) << 8
                            | (unsigned int)(c * tints[tint][2] + .5);
                    }
                r->sprite[shape][tint][lvl] = relaxupload(n, n, data, &r->spritepix[shape][tint][lvl]);
            }
}

static void
relaxbuildvignette(void)
{
    RelaxScene *r = &relaxscene;
    int w = 64, h = 36, i, j;
    unsigned int *data = malloc(w * h * 4);
    double x, y, a;

    if (!data)
        return;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) {
            x = (i + .5) / w * 2 - 1;
            y = (j + .5) / h * 2 - 1;
            a = .85 * relaxsmoothstep((sqrt(x * x + y * y) - .35) / .8);
            data[j * w + i] = (unsigned int)(a * 255 + .5) << 24;
        }
    r->vignette = relaxupload(w, h, data, &r->vignettepix);
    relaxaffine(r->vignette, (double)w / r->w, (double)h / r->h, 0, 0);
}

/* src 缩小到 dst (每级 2 倍, 双线性正好是 2x2 平均) */
static void
relaxshrink(Picture src, int sw0, int sh0, Picture dst, int dw, int dh)
{
    relaxaffine(src, (double)sw0 / dw, (double)sh0 / dh, 0, 0);
    XRenderSetPictureFilter(dpy, src, FilterBilinear, NULL, 0);
    XRenderComposite(dpy, PictOpSrc, src, None, dst, 0, 0, 0, 0, 0, 0, dw, dh);
}

static void
relaxbuildmips(RelaxStar *s)
{
    int l;

    for (l = s->base + 1; l < RELAXMIPS; l++) {
        s->mipw[l] = MAX(1, s->mipw[l - 1] / 2);
        s->miph[l] = MAX(1, s->miph[l - 1] / 2);
        if (s->mipw[l - 1] < 16 || s->miph[l - 1] < 16)
            break;
        s->mip[l] = relaxargb(s->mipw[l], s->miph[l], &s->mippix[l]);
        relaxshrink(s->mip[l - 1], s->mipw[l - 1], s->miph[l - 1], s->mip[l], s->mipw[l], s->miph[l]);
    }
    for (l = s->base; l < RELAXMIPS; l++)
        if (s->mip[l])
            XRenderSetPictureFilter(dpy, s->mip[l], FilterBilinear, NULL, 0);
}

/* 每个窗口只截一次: 可见窗口截全尺寸, 其他 tag / 隐藏窗口直接截半尺寸 (它们从远处出现) */
static void
relaxcapture(RelaxStar *s, Client *c, int full)
{
    RelaxScene *r = &relaxscene;
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
    s->mip[s->base] = relaxargb(s->mipw[s->base], s->miph[s->base], &s->mippix[s->base]);
    XRenderFillRectangle(dpy, PictOpSrc, s->mip[s->base], &clear, 0, 0, s->mipw[s->base], s->miph[s->base]);
    if (!full) {
        relaxaffine(src, (double)sw0 / s->mipw[1], (double)sh0 / s->miph[1], 0, 0);
        XRenderSetPictureFilter(dpy, src, FilterBilinear, NULL, 0);
    }
    XRenderComposite(dpy, PictOpOver, src, None, s->mip[s->base], 0, 0, 0, 0, 0, 0, s->mipw[s->base], s->miph[s->base]);
    relaxbuildmips(s);
    s->snap = 1;
done:
    if (src)
        XRenderFreePicture(dpy, src);
    if (tmp)
        XFreePixmap(dpy, tmp);
}

static void
relaxfreemip(RelaxStar *s, int l)
{
    if (s->mip[l])
        XRenderFreePicture(dpy, s->mip[l]);
    if (s->mippix[l])
        XFreePixmap(dpy, s->mippix[l]);
    s->mip[l] = 0;
    s->mippix[l] = 0;
}

static void
relaxfreestar(RelaxStar *s)
{
    int l;

    for (l = 0; l < RELAXMIPS; l++)
        relaxfreemip(s, l);
}

/* 场景资源 (截图 / 精灵 / 数组); 停在壁纸或退出时释放 */
static void
relaxfreescene(void)
{
    RelaxScene *r = &relaxscene;
    int i, j, k;

    for (i = 0; i < r->nstars; i++)
        relaxfreestar(&r->stars[i]);
    for (i = 0; i < RelaxShapes; i++)
        for (j = 0; j < RelaxTints; j++)
            for (k = 0; k < RELAXSPRITES; k++) {
                if (r->sprite[i][j][k])
                    XRenderFreePicture(dpy, r->sprite[i][j][k]);
                if (r->spritepix[i][j][k])
                    XFreePixmap(dpy, r->spritepix[i][j][k]);
                r->sprite[i][j][k] = 0;
                r->spritepix[i][j][k] = 0;
            }
    for (i = 0; i < RELAXALPHAS; i++) {
        if (r->black[i])
            XRenderFreePicture(dpy, r->black[i]);
        r->black[i] = 0;
    }
    if (r->vignette)
        XRenderFreePicture(dpy, r->vignette);
    if (r->vignettepix)
        XFreePixmap(dpy, r->vignettepix);
    r->vignette = 0;
    r->vignettepix = 0;
    free(r->stars);
    free(r->galaxies);
    free(r->dust);
    free(r->items);
    free(r->tgpos);
    free(r->tgplane);
    free(r->tpts);
    free(r->rpts);
    for (i = 0; i < RELAXBUCKETS; i++) {
        free(r->tris[i]);
        r->tris[i] = NULL;
    }
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
relaxfree(void)
{
    RelaxScene *r = &relaxscene;
    int i;

    relaxfreescene();
    for (i = 0; i < RELAXALPHAS; i++)
        if (r->white[i])
            XRenderFreePicture(dpy, r->white[i]);
    if (r->overlaypic) XRenderFreePicture(dpy, r->overlaypic);
    if (r->overlay) XDestroyWindow(dpy, r->overlay);
    if (r->hand) XFreeCursor(dpy, r->hand);
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
relaxxerror(Display *d, XErrorEvent *ee)
{
    /* 截图 / 合成过程中窗口可能随时消失; dwm 的 xerror 遇到 Render 错误会直接退出整个会话 */
    if (ee->request_code == relaxscene.rendermajor || ee->error_code == BadWindow
            || ee->error_code == BadDrawable || ee->error_code == BadPixmap || ee->error_code == BadMatch
            || ee->request_code == X_CreatePixmap || ee->request_code == X_FreePixmap
            || ee->request_code == X_PutImage || ee->request_code == X_CreateGC
            || ee->request_code == X_GetWindowAttributes || ee->request_code == X_QueryTree
            || ee->request_code == X_GrabPointer || ee->request_code == X_GrabKeyboard) {
        relaxscene.errors++;
        return 0;
    }
    return xerror(d, ee);
}

/* ---------- 场景构建 ---------- */

/* 星系群布局: 有窗口的星系黄金角分布在一个铺满屏幕的椭球里 (x/y 占满画面, z 拉开近 / 中 / 远三层);
 * 没有窗口的 tag 退到后景作为远处的深度参照, 不占主画面 */
static void
relaxbuildgalaxies(void)
{
    RelaxScene *r = &relaxscene;
    RelaxGalaxy *g;
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
    rad = .085 * r->w * r->orbitscale * MAX(.75, MIN(1.5, 1.9 / sqrt(npop + .5)));
    for (i = 0; i < r->ntags; i++) {
        g = &r->galaxies[i];
        g->tag = i;
        main = g->nstars || !npop;
        idx = i == cur ? 0 : main ? pi++ : ei++;
        n = main ? (npop ? npop : r->ntags) : r->ntags - npop;
        if (main && n == 1) {
            g->home = relaxv(0, 0, 0);
        } else if (main) {
            spread = MIN(1, .55 + n / 8.0);
            perm = ((n % 7 ? idx * 7 : idx) + n / 2) % n;   /* 第一个位置在垂直方向居中 */
            yy = 1 - 2 * (perm + .5) / n;
            rr = sqrt(MAX(0, 1 - yy * yy));
            phi = idx * 2.39996323 - RELAXPI / 2 + .25;
            /* xz 近似圆形 (半径约 .42F): 绕 y 旋转时不会转出屏幕, 星系群约占屏宽 2/3;
             * 第一个位置 (当前 tag) 在靠近镜头的一侧 */
            g->home = relaxv(cos(phi) * rr * .35 * r->w * spread + (relaxhash(i * 3 + 1) - .5) * .03 * r->w,
                    yy * .25 * r->h * spread,
                    sin(phi) * rr * .42 * F * spread + (relaxhash(i * 3 + 2) - .5) * .08 * F);
        } else {
            phi = idx * 2.39996323 + 1.1;
            g->home = relaxv(cos(phi) * .62 * r->w, (relaxhash(i * 3 + 1) - .5) * .6 * r->h,
                    F * (.85 + .35 * relaxhash(i * 3 + 2)) + sin(phi) * .3 * F);
        }
        g->rx = relaxinclinations[i % LENGTH(relaxinclinations)] * RELAXPI / 180 * (1 + .12 * (relaxhash(i + 40) - .5));
        g->ry = fmod(i * .9 + .3, 2 * RELAXPI) - RELAXPI;
        g->rz = .25 * sin(i * 1.3);
        g->phase = i * 1.7;
        g->speed = (.7 + .1 * (i % 3)) * (i % 2 ? -1 : 1);
        g->precess = .05 * (i % 2 ? 1 : -1);
        g->size = 34 * (1 + .08 * MIN(g->nstars, 6)) * (g->nstars ? 1 : .6);
        g->nrings = !g->nstars ? 0 : g->nstars <= 4 ? 1 : g->nstars <= 10 ? 2 : 3;
        g->radius = rad * (.92 + .08 * (i % 3));
        /* 同一星系的几条轨道环互相倾斜 (原子模型式), 每条环有自己的平面 */
        for (k = 0; k < RELAXRINGS; k++) {
            g->ring[k] = relaxmul(relaxrotz(k * RELAXPI / 3 + .4 * relaxhash(i * 7 + k)),
                    relaxrotx(relaxringtilt[k] * RELAXPI / 180));
            g->ringr[k] = g->radius * (1 + .45 * k);
        }
        g->hover = 0;
    }
}

static void
relaxbuildorbits(void)
{
    RelaxScene *r = &relaxscene;
    RelaxStar *s;
    RelaxGalaxy *g;
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
        s->angle = 2 * RELAXPI * inring / MAX(1, perring) + s->ring * .5 + g->phase;
        /* 中间那条环反向运行, 相邻轨道互相穿插 */
        s->speed = g->speed / (1 + .3 * s->ring) * (s->ring == 1 ? -1 : 1);
        s->rock = 2 * RELAXPI * relaxhash(i + 100);
        s->delay = .12 * relaxhash(i + 200);
        s->detach = s->current
            ? relaxadd(relaxscale(s->home, .88), relaxv(0, 0, F * (.38 + .12 * relaxhash(i + 300))))
            : relaxadd(relaxscale(s->home, .85), relaxv(0, 0, F * (1.3 + .3 * relaxhash(i + 400))));
        s->drx = .12 * (relaxhash(i + 500) * 2 - 1);
        s->dry = .25 * (relaxhash(i + 600) * 2 - 1);
        s->drz = .04 * (relaxhash(i + 700) * 2 - 1);
        s->pos = s->home;
    }
}

static void
relaxbuilddust(void)
{
    RelaxScene *r = &relaxscene;
    RelaxDust *d;
    double F = r->cam.focal, u, v, rad, th, ph;
    int i;

    for (i = 0; i < r->ndust; i++) {
        d = &r->dust[i];
        u = relaxhash(i * 5 + 1000);
        v = relaxhash(i * 5 + 1001);
        th = 2 * RELAXPI * relaxhash(i * 5 + 1002);
        ph = acos(2 * v - 1);
        if (u < .5) {           /* 远景 */
            rad = F * (3.5 + 2.5 * relaxhash(i * 5 + 1003));
            d->size = 10 + 5 * relaxhash(i * 5 + 1004);
            d->light = .4 + .2 * relaxhash(i * 5 + 1004);
        } else if (u < .85) {   /* 中景 */
            rad = F * (1.6 + 1.2 * relaxhash(i * 5 + 1003));
            d->size = 5.5 + 2 * relaxhash(i * 5 + 1004);
            d->light = .5 + .2 * relaxhash(i * 5 + 1004);
        } else {                /* 前景: 镜头与星系群之间, 视差最大 */
            d->pos = relaxv((relaxhash(i * 5 + 1003) - .5) * 2.2 * F, (v - .5) * 1.3 * F,
                    -F * (.45 + .4 * relaxhash(i * 5 + 1002)));
            d->size = 1.8 + .8 * relaxhash(i * 5 + 1004);
            d->light = .6 + .25 * relaxhash(i * 5 + 1004);
            continue;
        }
        d->pos = relaxv(rad * sin(ph) * cos(th), rad * cos(ph) * .6, rad * sin(ph) * sin(th));
    }
}

/* 动态壁纸 (linux-wallpaperengine / xwinwrap 等) 是最底层的全屏非托管窗口 */
static Window
relaxfindwallpaper(void)
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
                && wa.x <= 0 && wa.y <= 0 && wa.x + wa.width >= relaxscene.w && wa.y + wa.height >= relaxscene.h) {
            found = wins[i];
            break;
        }
    }
    if (wins)
        XFree(wins);
    return found;
}

static void
relaxcapturebackground(void)
{
    RelaxScene *r = &relaxscene;
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
    if ((r->wallwin = relaxfindwallpaper()) && XGetWindowAttributes(dpy, r->wallwin, &wa)
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
relaxlogstart(void)
{
    RelaxScene *r = &relaxscene;
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
    fprintf(r->log, "galaxy start: screen %dx%d tags %d windows %d dust %d trail %d focal %.0f timescale %.2f dpms %d wallpaper 0x%lx%s\n",
            r->w, r->h, r->ntags, r->nstars, r->ndust, r->ntrail, r->cam.focal, r->tscale, r->dpms,
            r->wallwin, r->live ? " (live)" : "");
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
relaxgapcmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* 一个阶段 (开场 / 驻留每 10 秒 / 坍缩 / 回程) 的帧率统计, 打印后清零 */
static void
relaxlogseg(const char *how)
{
    RelaxScene *r = &relaxscene;
    double now = relaxnow(), span = now - r->segstart, low = 0;

    if (r->log && r->frames >= 2) {
        if (r->ngaps) {
            qsort(r->gaps, r->ngaps, sizeof *r->gaps, relaxgapcmp);
            low = r->gaps[(size_t)(r->ngaps * .99)];
        }
        fprintf(r->log, "galaxy %s: at %.2fs frames %lu time %.3fs avg %.1f fps min %.1f fps 1%%low %.1f fps render avg %.2fms max %.2fms%s xerrors %lu\n",
                how, now, r->frames, span, r->frames / MAX(span, 1e-3),
                r->ngaps ? 1 / r->gaps[r->ngaps - 1] : 0, low > 0 ? 1 / low : 0,
                r->rendersum / r->frames * 1000, r->rendermax * 1000,
                r->mode == RelaxOrbit && now - r->lastinput > RELAXIDLE ? " (idle)" : "", r->errors);
        fflush(r->log);
    }
    r->frames = r->ngaps = 0;
    r->rendersum = r->rendermax = 0;
    r->segstart = now;
}

static void
relaxlogfirst(void)
{
    RelaxScene *r = &relaxscene;
    int i;

    if (!r->log || r->nfirst < 2 || r->firstlogged)
        return;
    fprintf(r->log, "galaxy first frames (start ms / render ms):");
    for (i = 0; i < r->nfirst; i++)
        fprintf(r->log, " %.0f/%.1f", r->firstgap[i] * 1000, r->firstcost[i] * 1000);
    fprintf(r->log, "\n");
    r->firstlogged = 1;
}

/* ---------- 状态机 ---------- */

static int
relaxactive(void)
{
    return relaxscene.mode != RelaxOff;
}

static void
relaxsetcursor(int on)
{
    RelaxScene *r = &relaxscene;

    if (on == r->handon || !r->overlay)
        return;
    r->handon = on;
    if (on && r->hand)
        XDefineCursor(dpy, r->overlay, r->hand);
    else
        XUndefineCursor(dpy, r->overlay);
}

/* 不再独占键盘鼠标 (锁屏程序仍能抓取), 键盘焦点交给遮罩, 鼠标事件直接发给最上层的遮罩 */
static void
relaxrelease(void)
{
    RelaxScene *r = &relaxscene;

    if (r->grabptr)
        XUngrabPointer(dpy, CurrentTime);
    if (r->grabkbd)
        XUngrabKeyboard(dpy, CurrentTime);
    r->grabptr = r->grabkbd = 0;
    XSetInputFocus(dpy, r->overlay, RevertToPointerRoot, CurrentTime);
}

static void
relaxend(int restore)
{
    RelaxScene *r = &relaxscene;
    Monitor *m, *tmon = r->tmon;
    unsigned int ttags = r->ttags;
    Window twin = r->twin;
    int tshow = r->tshow;
    Client *c;
    XEvent ev;

    if (!r->mode)
        return;
    relaxlogfirst();
    if (r->mode != RelaxRest)
        relaxlogseg(relaxmodename[r->mode]);
    if (r->log)
        fprintf(r->log, "galaxy end: from %s restore %d tags 0x%x win 0x%lx xerrors %lu\n",
                relaxmodename[r->mode], restore, ttags, twin, r->errors);
    if (r->grabptr)
        XUngrabPointer(dpy, CurrentTime);
    if (r->grabkbd)
        XUngrabKeyboard(dpy, CurrentTime);
    XSetErrorHandler(relaxxerror);
    relaxfree();
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
        if (c && tshow && HIDDEN(c))
            show(c);  /* 点击了隐藏窗口的星体: 恢复它 (与 Super+A 预览选中隐藏窗口一致) */
        if (ttags && (m->tagset[m->seltags] & TAGMASK) != ttags)
            view(&(Arg){.ui = ttags});
    }
    c = twin ? wintoclient(twin) : NULL;
    focus(c && ISVISIBLE(c) && !HIDDEN(c) ? c : NULL);
}

static void
relaxcleanup(void)
{
    relaxend(0);
}

static void
relaxcancel(void)
{
    relaxend(1);
}

/* 坍缩结束 (或坍缩中再按 Esc): 遮罩只显示壁纸, 释放全部星系资源, 等 Super+Z / 任意键恢复 */
static void
relaxfinish(void)
{
    RelaxScene *r = &relaxscene;
    int i;

    relaxlogseg(relaxmodename[r->mode]);
    relaxfreescene();
    for (i = 0; i < RELAXALPHAS; i++) {
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
    r->mode = RelaxRest;
    relaxsetcursor(0);
    if (r->live)
        XRenderComposite(dpy, PictOpSrc, r->live, None, r->wallpaper, 0, 0, 0, 0, 0, 0, r->w, r->h);
    XRenderComposite(dpy, PictOpSrc, r->wallpaper, None, r->overlaypic, 0, 0, 0, 0, 0, 0, r->w, r->h);
    relaxrelease();
    if (r->log)
        fprintf(r->log, "galaxy rest: wallpaper only, xerrors %lu\n", r->errors);
    XSync(dpy, False);
}

/* 开场播完: 停在星系轨道态, 不限时 */
static void
relaxorbitstart(void)
{
    RelaxScene *r = &relaxscene;

    relaxlogfirst();
    relaxlogseg(r->warping ? "intro (warped)" : "intro");
    r->mode = RelaxOrbit;
    r->warping = 0;
    r->lastinput = r->lastdpms = relaxnow();
    relaxrelease();
}

static void
relaxcollapsestart(void)
{
    RelaxScene *r = &relaxscene;

    relaxlogseg(relaxmodename[r->mode]);
    r->mode = RelaxCollapse;
    r->cstart = relaxnow();
    r->hover = r->hovercore = -1;
    relaxsetcursor(0);
}

/* 开场中按键 / 点击: 场景时钟加速, RELAXWARP 秒内走到驻留态 (一切都是时间的纯函数, 不会跳帧) */
static void
relaxwarp(void)
{
    RelaxScene *r = &relaxscene;

    if (r->mode != RelaxIntro || r->warping)
        return;
    r->warping = 1;
    r->wstart = relaxnow();
    r->wscene = r->scene;
}

/* 回程: star >= 0 时回到该窗口所在 tag 并聚焦它, tag >= 0 时只切到该 tag, 都为 -1 时回到开始前的状态 */
static void
relaxreturnstart(int tag, int star)
{
    RelaxScene *r = &relaxscene;
    RelaxStar *s;
    RelaxGalaxy *g;
    int i;

    if (r->mode == RelaxOff || r->mode == RelaxRest || r->mode == RelaxReturn)
        return;
    relaxlogseg(relaxmodename[r->mode]);
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
    /* 回程结束后可见的窗口飞回原位置, 其余的退向深处 */
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
    r->rcx = r->cam.rx * 180 / RELAXPI;
    r->rcy = r->cam.ry * 180 / RELAXPI;
    r->rcz = r->cam.rz * 180 / RELAXPI;
    r->rcampos = r->cam.pos;
    r->rring = r->ringalpha;
    r->rdust = r->dustfade;
    r->rbright = r->bright;
    r->rvign = r->vign;
    r->rdesk = r->desk;
    r->mode = RelaxReturn;
    r->rstart = relaxnow();
    r->hover = r->hovercore = -1;
    relaxsetcursor(0);
    if (r->log)
        fprintf(r->log, "galaxy return: to tags 0x%x win 0x%lx fulldesk %d\n", r->ttags, r->twin, r->fulldesk);
}

/* 鼠标下最前面的窗口星或星系核心 (按上一帧画出的位置, 从近到远找) */
static void
relaxpick(double x, double y, int *star, int *core)
{
    RelaxScene *r = &relaxscene;
    RelaxStar *s;
    RelaxGalaxy *g;
    int i;

    *star = *core = -1;
    for (i = r->nitems - 1; i >= 0; i--) {
        if (r->items[i].kind == RelaxStarItem) {
            s = &r->stars[r->items[i].index];
            if (s->hit && x >= s->bx0 && x <= s->bx1 && y >= s->by0 && y <= s->by1) {
                *star = r->items[i].index;
                return;
            }
        } else if (r->items[i].kind == RelaxCoreItem) {
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
relaxrest(void)
{
    RelaxScene *r = &relaxscene;
    Window focused;
    int revert;

    if (r->mode == RelaxRest && r->live) {
        XRenderComposite(dpy, PictOpSrc, r->live, None, r->wallpaper, 0, 0, 0, 0, 0, 0, r->w, r->h);
        XRenderComposite(dpy, PictOpSrc, r->wallpaper, None, r->overlaypic, 0, 0, 0, 0, 0, 0, r->w, r->h);
        XSync(dpy, False);
    }
    XGetInputFocus(dpy, &focused, &revert);
    if (focused != r->overlay)
        XSetInputFocus(dpy, r->overlay, RevertToPointerRoot, CurrentTime);
}

static double
relaxfps(double now)
{
    RelaxScene *r = &relaxscene;

    if (r->mode != RelaxOrbit)
        return RELAXFPS;
    return now - r->lastinput > RELAXIDLE ? RELAXIDLEFPS : RELAXORBITFPS;
}

static void
relaxtick(void)
{
    RelaxScene *r = &relaxscene;
    double now, dt, u = 0, begin, cost;
    CARD16 level;
    BOOL on;
    int star, core;

    now = relaxnow();
    if (r->mode == RelaxRest) {
        if (r->live && now - r->last >= 1 / 60.0) {
            r->last = now;
            relaxrest();
        }
        return;
    }
    if (now - r->last < 1 / relaxfps(now) - .0005)
        return;
    dt = r->last < 0 ? 0 : MIN(now - r->last, .1);
    if (r->mode == RelaxOrbit && r->dpms && now - r->lastdpms >= 1) {
        /* 屏幕关闭 (DPMS) 时暂停渲染 */
        r->lastdpms = now;
        r->dpmsoff = DPMSInfo(dpy, &level, &on) && on && level != DPMSModeOn;
    }
    if (r->mode == RelaxOrbit && r->dpmsoff) {
        r->last = now;
        return;
    }
    switch (r->mode) {
    case RelaxIntro:
        if (r->warping) {
            u = (now - r->wstart) / RELAXWARP;
            r->scene = r->wscene + (RELAXHOLD - r->wscene) * relaxeaseinoutcubic(u);
            if (u >= 1)
                r->scene = RELAXHOLD;
        } else {
            r->scene += dt / r->tscale;
        }
        r->stage = MIN(r->scene, RELAXHOLD);
        r->motion = r->scene;
        break;
    case RelaxOrbit:
        r->scene += dt / r->tscale;
        r->stage = RELAXHOLD;
        r->motion = r->scene;
        break;
    case RelaxCollapse:
        r->scene += dt / r->tscale;
        r->stage = RELAXEXIT + (now - r->cstart) / r->tscale;
        r->motion = r->scene;
        if (r->stage >= RELAXEND) {
            relaxfinish();
            return;
        }
        break;
    case RelaxReturn:
        u = (now - r->rstart) / RELAXRETURN;
        if (u >= 1) {
            relaxend(1);
            return;
        }
        break;
    }
    if (r->frames && r->ngaps < RELAXGAPS)
        r->gaps[r->ngaps++] = now - r->last;
    r->last = now;
    begin = relaxnow();
    if (r->mode == RelaxOrbit) {
        relaxpick(r->mx, r->my, &star, &core);
        r->hover = star;
        r->hovercore = core;
        relaxsetcursor(star >= 0 || core >= 0);
    }
    if (r->mode == RelaxReturn)
        relaxupdatereturn(u);
    else
        relaxupdatescene(r->stage, r->motion, dt);
    relaxrender();
    cost = relaxnow() - begin;
    if (r->mode == RelaxIntro && r->nfirst < (int)LENGTH(r->firstcost)) {
        r->firstgap[r->nfirst] = now;   /* 相对按下 Super+Z 后时钟起点的时刻 */
        r->firstcost[r->nfirst++] = cost;
    }
    r->rendersum += cost;
    r->rendermax = MAX(r->rendermax, cost);
    r->frames++;
    if (r->mode == RelaxIntro && r->scene >= RELAXHOLD)
        relaxorbitstart();
    else if (r->mode == RelaxOrbit && now - r->segstart >= 10)
        relaxlogseg("orbit");
}

/* select() 的超时 (微秒), -1 表示没有动画, 一直等待 X 事件 */
static long
relaxtimeout(void)
{
    RelaxScene *r = &relaxscene;
    double now, left;

    if (r->mode == RelaxRest)
        return r->live ? 16666 : -1;
    now = relaxnow();
    if (r->mode == RelaxOrbit && r->dpmsoff)
        return 1000000;
    left = 1 / relaxfps(now) - (now - r->last);
    return left > 0 ? (long)(left * 1e6) : 0;
}

/* 动画期间 dwm 主循环先把事件交给这里; 返回 1 表示已处理, 不再交给 dwm */
static int
relaxevent(XEvent *e)
{
    RelaxScene *r = &relaxscene;
    KeySym sym;
    Window w;
    int i, esc, superz, star, core;

    if (!r->mode)
        return 0;
    switch (e->type) {
    case KeyPress:
        sym = XLookupKeysym(&e->xkey, 0);
        if (IsModifierKey(sym))
            return 1;
        r->lastinput = relaxnow();
        esc = sym == XK_Escape;
        superz = sym == XK_z && CLEANMASK(e->xkey.state) == MODKEY;
        switch (r->mode) {
        case RelaxIntro:
            if (esc)
                relaxcancel();
            else if (superz)
                relaxreturnstart(-1, -1);
            else
                relaxwarp();
            break;
        case RelaxOrbit:
            if (esc)
                relaxcollapsestart();
            else if (superz)
                relaxreturnstart(-1, -1);
            break;
        case RelaxCollapse:
            if (esc)
                relaxfinish();
            else if (superz)
                relaxcancel();
            break;
        case RelaxReturn:
            if (esc || superz)
                relaxend(1);
            break;
        case RelaxRest:
            relaxcancel();
            break;
        }
        return 1;
    case ButtonPress:
        r->lastinput = relaxnow();
        r->mx = e->xbutton.x_root;
        r->my = e->xbutton.y_root;
        switch (r->mode) {
        case RelaxIntro:
            if (e->xbutton.button <= Button3)
                relaxwarp();
            break;
        case RelaxOrbit:
            if (e->xbutton.button == Button4) {
                r->tzoom = MAX(.7, r->tzoom * .9);
            } else if (e->xbutton.button == Button5) {
                r->tzoom = MIN(1.4, r->tzoom / .9);
            } else if (e->xbutton.button == Button1) {
                relaxpick(r->mx, r->my, &star, &core);
                if (star >= 0)
                    relaxreturnstart(-1, star);
                else if (core >= 0)
                    relaxreturnstart(core, -1);
            }
            break;
        case RelaxRest:
            relaxcancel();
            break;
        }
        return 1;
    case MotionNotify:
        r->lastinput = relaxnow();
        r->mx = e->xmotion.x_root;
        r->my = e->xmotion.y_root;
        /* 鼠标视差: 镜头随指针小幅偏航 / 俯仰 (幅度小, 瞄准窗口星时目标不会明显跑开) */
        r->tyaw = (r->mx / r->w - .5) * 2 * 4;
        r->tpitch = (r->my / r->h - .5) * 2 * 2.5;
        return 1;
    case KeyRelease: case ButtonRelease: case EnterNotify: case LeaveNotify:
        return 1;
    case Expose:
        if (e->xexpose.window != r->overlay)
            return 0;
        if (r->mode == RelaxRest)
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
        if (e->type == DestroyNotify && e->xdestroywindow.window == r->twin && r->mode == RelaxReturn)
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
relaxpost(XEvent *e)
{
    RelaxScene *r = &relaxscene;

    if (!r->mode)
        return;
    XSetErrorHandler(relaxxerror);
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
    if ((r->mode == RelaxRest || r->mode == RelaxOrbit)
            && (e->type == MapRequest || e->type == ClientMessage || e->type == DestroyNotify || e->type == UnmapNotify))
        relaxrest();
}

static void
relax(const Arg *arg)
{
    RelaxScene *r = &relaxscene;
    XSetWindowAttributes wa;
    XClassHint cls = {"dwm-galaxy", "dwm-galaxy"};
    XRenderColor color;
    Monitor *m;
    Client *c;
    RelaxStar *s;
    Window dw;
    struct timespec t0, t1, t2;
    int i, count = 0, ev, er, tag, full, pass, dx, dy;
    unsigned int cur, mask;
    long budget = 320L << 20, bytes;

    if (r->mode) {
        relaxcancel();
        return;
    }
    clock_gettime(CLOCK_MONOTONIC, &t0);
    memset(r, 0, sizeof *r);
    r->w = sw;
    r->h = sh;
    r->ntags = MIN((int)LENGTH(tags), 31);
    r->hover = r->hovercore = -1;
    r->zoom = r->tzoom = 1;
    r->bglast[0] = -1;
    XQueryExtension(dpy, "RENDER", &r->rendermajor, &ev, &er);
    r->dpms = DPMSQueryExtension(dpy, &ev, &er) && DPMSCapable(dpy);
    XSetErrorHandler(relaxxerror);
    r->argb = XRenderFindStandardFormat(dpy, PictStandardARGB32);
    r->a8 = XRenderFindStandardFormat(dpy, PictStandardA8);
    for (m = mons; m; m = m->next)
        for (c = m->clients; c; c = c->next)
            if (!c->isscratchpad)
                count++;
    r->nstars = count;
    r->tscale = count ? RELAXINTRO : 1;   /* 没有窗口: 同样的视觉语言, 更短的开场 */
    r->starscale = MAX(.65, MIN(1.1, 1.15 - .012 * count));
    r->orbitscale = MAX(.7, MIN(1, 1.05 - .008 * count));
    r->glowscale = MAX(.55, MIN(1, 1.1 - .015 * count));
    r->ndust = MAX(28, MIN(84, 84 - (int)(1.5 * count)));
    r->ntrail = count > 24 ? 6 : RELAXTRAIL;
    r->cam.fov = 62 * RELAXPI / 180;
    r->cam.focal = r->w * .5 / tan(r->cam.fov / 2);
    r->cam.near = r->cam.focal * .12;
    r->cam.far = r->cam.focal * 14;
    r->triscap = MAX((count + r->ntags) * r->ntrail * 2, RELAXSEG * 2) + 2;
    r->galaxies = calloc(r->ntags, sizeof *r->galaxies);
    r->stars = calloc(MAX(1, count), sizeof *r->stars);
    r->dust = calloc(r->ndust, sizeof *r->dust);
    r->items = calloc(r->ndust + r->ntags * (1 + 2 * RELAXRINGS) + count + 1, sizeof *r->items);
    r->tgpos = calloc(r->ntags, sizeof *r->tgpos);
    r->tgplane = calloc(r->ntags, sizeof *r->tgplane);
    r->tpts = calloc((count + r->ntags) * (r->ntrail + 1), sizeof *r->tpts);
    r->rpts = calloc(r->ntags * RELAXRINGS * (RELAXSEG + 1), sizeof *r->rpts);
    r->gaps = calloc(RELAXGAPS, sizeof *r->gaps);
    for (i = 0; i < RELAXBUCKETS; i++)
        if (!(r->tris[i] = calloc(r->triscap, sizeof(XTriangle))))
            goto fail;
    if (!r->galaxies || !r->stars || !r->dust || !r->items || !r->tgpos || !r->tgplane || !r->tpts || !r->rpts
            || !r->gaps || !r->argb || !r->a8)
        goto fail;
    r->savedmon = r->tmon = selmon;
    r->savedtags = r->ttags = selmon->tagset[selmon->seltags] & TAGMASK;
    r->savedwin = r->twin = selmon->sel ? selmon->sel->win : None;
    cur = r->savedtags ? (unsigned int)__builtin_ctz(r->savedtags) : 0;

    if (!(r->back = relaxopaque(r->w, r->h, &r->backpix)) || !(r->desktop = relaxopaque(r->w, r->h, &r->desktoppix))
            || !(r->wallpaper = relaxopaque(r->w, r->h, &r->wallpix)) || !(r->bg = relaxopaque(r->w, r->h, &r->bgpix)))
        goto fail;
    for (i = 0; i < RELAXALPHAS; i++) {
        color.alpha = color.red = color.green = color.blue = (unsigned short)(65535L * i / (RELAXALPHAS - 1));
        r->white[i] = XRenderCreateSolidFill(dpy, &color);
        color.red = color.green = color.blue = 0;
        r->black[i] = XRenderCreateSolidFill(dpy, &color);
    }
    relaxbuildsprites();
    relaxbuildvignette();

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
            s->current = m == selmon && s->shown;
            s->focused = c == selmon->sel;
            s->w = MAX(1, c->w);
            s->h = MAX(1, c->h);
            s->home = relaxv(c->x + c->bw + c->w * .5 - r->w * .5, c->y + c->bw + c->h * .5 - r->h * .5, 0);
            r->galaxies[s->galaxy].nstars++;
        }
    XGrabServer(dpy);
    relaxcapturebackground();
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
            relaxcapture(s, s->c, full);
        }
    XSync(dpy, False);
    XUngrabServer(dpy);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    relaxbuildgalaxies();
    relaxbuildorbits();
    relaxbuilddust();
    if (XQueryPointer(dpy, root, &dw, &dw, &dx, &dy, &ev, &er, &mask)) {
        r->mx = dx;
        r->my = dy;
        r->tyaw = (r->mx / r->w - .5) * 2 * 4;
        r->tpitch = (r->my / r->h - .5) * 2 * 2.5;
    }

    /* 遮罩的背景就是刚截的桌面, 映射瞬间不会闪黑 */
    wa.override_redirect = True;
    wa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | PointerMotionMask;
    wa.background_pixmap = r->desktoppix;
    r->overlay = XCreateWindow(dpy, root, 0, 0, r->w, r->h, 0, DefaultDepth(dpy, screen), InputOutput,
            DefaultVisual(dpy, screen), CWOverrideRedirect | CWEventMask | CWBackPixmap, &wa);
    XSetClassHint(dpy, r->overlay, &cls);
    XStoreName(dpy, r->overlay, "dwm-galaxy");
    r->hand = XCreateFontCursor(dpy, XC_hand2);
    r->overlaypic = XRenderCreatePicture(dpy, r->overlay, XRenderFindVisualFormat(dpy, DefaultVisual(dpy, screen)), 0, NULL);
    if (!r->overlaypic)
        goto fail;
    XMapRaised(dpy, r->overlay);
    r->mode = RelaxIntro;
    if (XGrabKeyboard(dpy, r->overlay, False, GrabModeAsync, GrabModeAsync, CurrentTime) != GrabSuccess)
        goto fail;
    r->grabkbd = 1;
    if (XGrabPointer(dpy, r->overlay, False, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                GrabModeAsync, GrabModeAsync, None, None, CurrentTime) != GrabSuccess)
        goto fail;
    r->grabptr = 1;
    relaxlogstart();
    XSync(dpy, False);
    clock_gettime(CLOCK_MONOTONIC, &r->start);
    r->last = -1;
    r->segstart = 0;
    relaxtick();
    if (r->log) {
        clock_gettime(CLOCK_MONOTONIC, &t2);
        fprintf(r->log, "galaxy setup: grab+capture %.1fms, key -> first frame %.1fms\n",
                (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6,
                (t2.tv_sec - t0.tv_sec) * 1e3 + (t2.tv_nsec - t0.tv_nsec) / 1e6);
        fflush(r->log);
    }
    return;
fail:
    if (r->log)
        fprintf(r->log, "galaxy start failed\n");
    r->mode = RelaxIntro;
    relaxend(1);
}
