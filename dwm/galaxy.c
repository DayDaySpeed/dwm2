/* Super+Z 3D 工作空间星系. 由 dwm.c 在 config.h 与 Pertag 之后 #include.
 *
 * 每个窗口在开始时截图一次 (XRender Picture + mipmap), 之后只在离屏 3D 场景里绘制:
 *   世界坐标 (x 右, y 下, z 远离镜头) -> 镜头变换 -> 透视投影 -> 按镜头空间 z 排序 -> XRender 合成 -> 全屏遮罩窗口
 * Tag = 星系核心, 窗口 = 沿 3D 轨道环绕核心运行的星体, 所有 tag 组成星系群.
 * 流程: 开场 (约 7.7s: 起飞 / 跃迁 / 点火 / 螺旋旋转 / 俯冲铺满全屏 / 点名 / 弧线回缩) -> 停在星系轨道态 (不限时, tag 核心沿开普勒椭圆群轨道公转, 导演镜头轮换机位, 鼠标视差 / 滚轮推拉 / 点击窗口星跳转)
 *       -> Esc: 星系群沿轨道划过一段弧线后坍缩成一个光点, 停在纯壁纸 (再按 Super+Z 恢复)
 *       -> Super+Z: 回程, 窗口星飞回原位置变回截图, 露出真实桌面
 *       -> 点击窗口星: 卡片朝镜头前推后跳到该窗口; 点击核心: 核心亮起后切到该 tag
 * 动画期间不移动 / 隐藏 / 映射 / 重叠 / 聚焦任何真实窗口; 只在结束时恢复 (或按点击切换) tag 和焦点. */

#include <X11/extensions/dpms.h>

#define RELAXMIPS     6
#define RELAXALPHAS   256
#define RELAXTRAIL    12
#define RELAXBTILE    128       /* 光带画布按 128x128 分块上传 */
#define RELAXSPRITES  3
#define RELAXGAPS     4096
#define RELAXRINGS    3
#define RELAXSEG      72
#define RELAXARCS     8         /* 每段 9 条线, 独立参加深度排序 */
#define RELAXARCSEG   (RELAXSEG / RELAXARCS)
#define RELAXTILES    4         /* 大截图的透视画面拆成 4x4 仿射块 */
#define RELAXFPS      120.0
#define RELAXORBITFPS 60.0
#define RELAXIDLEFPS  30.0
#define RELAXIDLE     90.0      /* 驻留时多久无输入后降到 RELAXIDLEFPS (秒) */
#define RELAXINTRO    1.35      /* 开场时间拉伸: 1 场景秒 = 1.35 真实秒 */
#define RELAXHOLD     4.2       /* 关键帧曲线的驻留点: stage 停在这里 */
#define RELAXIEND     5.7       /* 开场时钟走到这里进入驻留 (旋转后还有俯冲 / 点名 / 回缩, 真实约 7.7s) */
#define RELAXEXIT     4.75      /* Esc 坍缩从这里接着播放到 RELAXEND (4.2~4.8 之间的曲线是平的) */
#define RELAXEND      6.0
#define RELAXWARP     .6        /* 开场中按键: 快进到驻留态的真实时长 */
#define RELAXRETURN   1.6       /* Super+Z 飞回原位的真实时长 */
#define RELAXPICK     .42       /* 点击窗口: 选中前推 */
#define RELAXCORE     .30       /* 点击星系核心: 亮起后淡出 */
#define RELAXSHOT     9.0       /* 导演镜头: 每个机位停留 (真实秒) */
#define RELAXSHOTMIX  4.0       /* 机位之间的过渡 (真实秒), 巡游之间是一次飞越 */
#define RELAXDIAG     30.0      /* 驻留时镜头翻滚 (度): 压扁的轨道盘面沿屏幕对角线铺开, 星轨更舒展; 卡片朝向镜头, 仍是正的 */
#define RELAXSTREAK   24        /* 每个星系核心身后的长曝光星轨分段数 */
#define RELAXSTREAKM  1.5       /* 星轨覆盖的平近点角 (rad): 近点处自然拉长, 远点处变短 */
#define RELAXCOLLAPSE 2.35      /* Esc 后完整的环绕 / 坍缩演出 (真实秒) */
#define RELAXEXITSTART 1.25     /* 退场从环绕切换到向中心收束的时刻 */
#define RELAXPREP     .45       /* 开场按 Esc 时平滑进入星系群形态 */
#define RELAXLANES    3
#define RELAXPI       3.14159265358979323846

enum { RelaxOff, RelaxIntro, RelaxOrbit, RelaxCollapse, RelaxReturn, RelaxRest };
enum { RelaxFlyHome, RelaxPickStar, RelaxPickCore };
enum { RelaxDustItem, RelaxCoreItem, RelaxStarItem, RelaxRingItem, RelaxClusterItem, RelaxStreakItem, RelaxSunItem };
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
    int tag, nstars, nrings, hit, lane;
    RelaxVec home;              /* 星系群坐标系中的位置 */
    RelaxVec pos, rpos;         /* 当前世界坐标 / 回程开始时冻结的位置 */
    double rx, ry, rz;          /* 轨道平面: 倾角 / 偏航 / 翻滚 */
    double radius, speed, phase, orbitphase, precess, size, alpha, ralpha, hover;
    double anomaly, peri, flare, ripple, flip;  /* 平近点角 / 近点亮度 / 交会闪光 / 涟漪 / 翻转中 */
    double ignite, callout, streakreveal;       /* 开场: 点火闪光 / 逐个点名 / 星轨拉出进度 */
    double nova, bridge;                        /* 驻留: 超新星爆闪 / 光桥到达时的闪光 */
    int rank;                   /* 开场点火的先后顺序 */
    RelaxVec nudge;             /* 交会时互相吸引的表现层偏移 */
    RelaxMat ring[RELAXRINGS];  /* 每条轨道环相对星系轨道平面的姿态 (环与环互相倾斜) */
    double ringr[RELAXRINGS], ringz[RELAXRINGS][RELAXARCS];
    int ringn[RELAXRINGS][RELAXARCS];
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
    double size, brightness, alpha, vis, tint, glow, lod, hover, flipcard, constel;
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
    int disk;                   /* 盘面尘带: 跟随星系群一起转 */
    double tw, boost;           /* 远景星点的闪烁相位; 超新星附近的提亮 */
    RelaxProj p;
} RelaxDust;

typedef struct { int kind, index; double z; } RelaxItem;

typedef struct {
    int mode, grabkbd, grabptr, w, h, ntags, nstars, ndust, nitems, ntrail, rendermajor;
    int warping, dpms, dpmsoff, hover, hovercore, handon, fulldesk;
    int rkind, rstar, rcore;    /* 回程种类: 飞回原位 / 点中的窗口 / 点中的核心 */
    double rcoresize;
    double tscale, starscale, orbitscale, glowscale;
    /* 时钟: scene 是场景时间, stage 驱动关键帧曲线 (驻留时停住), motion 驱动轨道运动 (一直走) */
    double last, scene, stage, motion, holdw;
    double wstart, wscene, cstart, cstage, celapsed, rstart, lastinput, lastdpms;
    double exitspin, clusteralpha, rcluster, streakalpha, rstreak, sunalpha, rsun, sunpulse, ripple, rippleamp;
    RelaxMat cworld;            /* Esc 时冻结星系群朝向, 避免叠加全局自转 */
    int nlanes, lanemask, npop, *popord;
    RelaxProj clusterpts[RELAXLANES][RELAXSEG + 1];
    double clusterz[RELAXLANES][RELAXARCS], clusterr[RELAXLANES][RELAXSEG + 1];
    int clustern[RELAXLANES][RELAXARCS];
    RelaxProj *streakpts, sunp;  /* 每个核心 RELAXSTREAK + 1 个点 */
    double *streakz;
    /* 导演镜头: dclock 只在无交互时走; 交互 (移动 / 滚轮 / 悬停) 让 dspeed 降到 0, 镜头停在当前机位 */
    double dclock, dspeed, dfit, lastpointer, dtyaw[2];
    int dshot, dtg[2], lastflip, lastripple, lastnova, lastcomet;
    int bridgek, bri, brj;      /* 当前这次核心光桥连接的两个核心 */
    /* 开场节拍: iclock 是开场时钟 (场景秒, 坍缩时冻结), beatfade 在快进 / 坍缩时把节拍效果淡掉 */
    double iclock, beatfade, warpfx, lanereveal[RELAXLANES];
    double divesum, divemax;    /* 俯冲段 (开场时钟 3.2–5.0) 的渲染耗时统计 */
    int divenum;
    RelaxVec dtour[2];          /* 巡游机位: 当前 / 下一个机位跟踪的核心 (平滑后的位置) */
    /* 驻留交互: 鼠标视差 / 滚轮推拉 (t 开头是目标值, 每帧平滑逼近) */
    double mx, my, tyaw, tpitch, pyaw, ppitch, tzoom, zoom;
    /* 每帧由 update 算出, render 只读这些 */
    double bright, vign, desk, deskover, bar, reveal, ringalpha, trailgain, dustfade, central, pulse;
    /* 回程开始时冻结的全局状态 */
    double rcdist, rcx, rcy, rcz, rring, rdust, rbright, rvign, rdesk;
    RelaxVec rcampos, rctarget;
    Window overlay, wallwin;
    Cursor hand;
    Pixmap backpix, desktoppix, wallpix, vignettepix, bgpix, tilemaskpix, spinpix;
    Picture back, desktop, wallpaper, live, overlaypic, vignette, bg, tilemask, spinpic;
    int spinw, spinh, projslow;   /* projslow: 整幅透视采样明显慢于仿射, 大卡片改走半分辨率 */
    double probeaffine, probeproj;
    XftFont *titlefont;
    XftDraw *titledraw;
    XftColor titlecolor;
    int titlecolorok;
    double bgkey[4], bglast[4];     /* 背景缓存: 驻留时壁纸亮度 / 暗角不变, 每帧只复制一次 */
    int bgok;
    Pixmap spritepix[RelaxShapes][RelaxTints][RELAXSPRITES];
    Picture sprite[RelaxShapes][RelaxTints][RELAXSPRITES];
    Picture white[RELAXALPHAS], black[RELAXALPHAS];
    XRenderPictFormat *argb, *a8, *a1;
    RelaxGalaxy *galaxies;
    RelaxStar *stars;
    RelaxDust *dust;
    RelaxItem *items;
    RelaxVec *tgpos;
    RelaxMat *tgplane;
    RelaxProj *tpts, *rpts;
    /* 光带画布: NVIDIA 上 XRenderCompositeTriangles 在 CPU 上逐个三角形栅格化 (每个约 7µs, 驻留每帧可达 30ms),
     * 改为进程内软件画 a8 抗锯齿光带, 只上传有内容的 128x128 块, 再用 GPU 合成 */
    unsigned char *bandbuf, *banddirty;
    XImage *bandimg;
    Pixmap bandpix;
    Picture bandpic;
    GC bandgc;
    int bandtw, bandth, bandn;
    double bandraster, bandflush;   /* 统计: 软件画光带 / 上传合成的累计耗时 */
    unsigned long bandtiles, bandflushes;
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
    double phasecost[6], qualityavg, qualitylast, qualityvisual;
    int quality, qualitybad, qualitygood;
    double firstgap[24], firstcost[24];    /* 开场前 24 帧的时刻和渲染耗时 */
    int nfirst, firstlogged;
} RelaxScene;

static RelaxScene relaxscene;
static double relaxnow(void);
static const int relaxspritesize[RELAXSPRITES] = { 128, 32, 8 };

/* 时间轴上的关键帧曲线 (单调分段三次 Hermite), 用于镜头和形态变化.
 * 时间是场景时间; 驻留态停在 RELAXHOLD, 坍缩从 RELAXEXIT 接着播放, 所以 4.2~4.8 的值必须相同 */
static const RelaxKey relaxcamdist[] = {
    {0, 1}, {.15, 1}, {.6, 1.05}, {1, 1.12}, {1.5, 1.22}, {2.2, 1.5}, {2.6, 1.46},
    {2.9, 1.38}, {3.3, 1.3}, {3.8, 1}, {4.2, .72}, {4.8, .72}, {5.5, 1.2}, {6, 1.2}
};
static const RelaxKey relaxcamyaw[] = {   /* 度 */
    {0, 0}, {.15, 0}, {.6, -3}, {1, -6}, {1.5, -10}, {2.2, -12}, {2.6, -2},
    {2.9, 10}, {3.3, 20}, {3.8, 6}, {4.2, -4}, {4.8, -4}, {5.5, 0}, {6, 0}
};
static const RelaxKey relaxcampitch[] = { /* 负值: 镜头在上方; 群轨道在水平盘面内, 驻留时低角度斜视 */
    {0, 0}, {.15, 0}, {.6, 2}, {1, 5}, {1.5, 8}, {2.2, 8}, {2.6, 4},
    {2.9, 0}, {3.3, -8}, {3.8, -12}, {4.2, -14}, {4.8, -14}, {5.5, 0}, {6, 0}
};
static const RelaxKey relaxcamroll[] = {
    {0, 0}, {2.2, 0}, {2.6, 2}, {2.9, 5}, {3.3, 3}, {4.2, RELAXDIAG}, {4.8, RELAXDIAG}, {5.5, 0}
};
/* 旋转之后的节拍镜头 (时间轴是开场时钟): 俯冲进星系群内部铺满全屏 -> 逐个点名 -> 沿弧线拉回,
 * 终点与驻留第一个机位 (wide) 的起点相同 */
static const RelaxKey relaxbeatdist[] = {
    {3.2, 1.32}, {3.6, .75}, {3.95, .42}, {4.5, .4}, {5.2, .8}, {5.45, .76}, {5.7, .72}
};
static const RelaxKey relaxbeatpitch[] = {
    {3.2, -7}, {3.6, -20}, {3.95, -34}, {4.5, -32}, {5.2, -17}, {5.7, -14}
};
static const RelaxKey relaxbeatyaw[] = {
    {3.2, 18}, {3.95, 34}, {4.5, 66}, {5.2, 8}, {5.7, -4}
};
static const RelaxKey relaxbeatroll[] = {
    {3.2, 2.5}, {3.95, -3}, {4.5, -4}, {5.2, RELAXDIAG * .75}, {5.7, RELAXDIAG}
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
    {1, 0}, {1.8, .14}, {2.4, .24}, {3.3, .24}, {4.2, .13}, {4.8, .13}, {5.1, 0}
};
static const double relaxinclinations[] = { 8, 28, -36, 54, -22, 42, -50, 16, -62 };
static const double relaxringtilt[RELAXRINGS] = { 0, 38, -32 };
/* 三条开普勒椭圆群轨道 (共用焦点 = 中心光源): 半长轴 (屏宽倍数) / 偏心率 / 倾角 / 升交点 / 近点角 (度) / 周期 (真实秒) / 方向 */
typedef struct { double a, e, inc, node, peri, period, dir; } RelaxLane;
static const RelaxLane relaxlanes[RELAXLANES] = {
    { .22, .35,   8,   0,  40, 30,  1 },
    { .33, .45, -10,  70, 165, 44, -1 },
    { .45, .52,  14, -40, 285, 60,  1 },
};

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

/* 开场节拍效果的强度 (开场 / 坍缩中有效, 快进或坍缩时随 beatfade 淡出) */
static double
relaxbeatw(void)
{
    RelaxScene *r = &relaxscene;

    return r->mode == RelaxIntro || r->mode == RelaxCollapse ? r->beatfade : 0;
}

/* 开场时钟 -> 关键帧 stage: 旋转结束 (3.3) 后平滑停在 3.45, 留出俯冲 / 点名的时间, 4.5 起再落定到 RELAXHOLD (C1 连续) */
static double
relaxintrostage(double scene)
{
    double x;

    if (scene <= 3.3)
        return scene;
    if (scene < 3.6) {
        x = scene - 3.3;
        return 3.3 + x - x * x / .6;
    }
    return 3.45 + (RELAXHOLD - 3.45) * relaxsmoothstep(relaxphase(scene, 4.5, RELAXIEND));
}

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

/* 贴近镜头时淡出 (.45F -> .25F): 物体穿过镜头附近时不会突然出现或消失. 卡片在 .25F 处被裁掉, 正好已经完全透明 */
static double
relaxnearfade(double z)
{
    return relaxsmoothstep((z - relaxscene.cam.focal * .25) / (relaxscene.cam.focal * .2));
}

static double
relaxdepthblur(double z)
{
    return relaxclamp((z / relaxscene.cam.dist - 1.05) / .5);
}

/* 镜头绕星系群中心运动: 距离 + 偏航 / 俯仰 / 翻滚 (度) */
static void
relaxsetcameraat(RelaxVec target, double dist, double pitch, double yaw, double roll)
{
    RelaxCamera *cam = &relaxscene.cam;
    double d2r = RELAXPI / 180;
    RelaxMat orbit;

    cam->dist = dist;
    cam->rx = pitch * d2r;
    cam->ry = yaw * d2r;
    cam->rz = roll * d2r;
    cam->target = target;
    orbit = relaxmul(relaxroty(cam->ry), relaxrotx(cam->rx));
    cam->pos = relaxadd(cam->target, relaxapply(orbit, relaxv(0, 0, -cam->dist)));
    cam->rot = relaxmul(orbit, relaxrotz(cam->rz));
    cam->view = relaxtranspose(cam->rot);
}

/* ---------- 世界: 全局旋转 / 星系 / 星体 (都是 (stage, motion) 的纯函数, 尾迹直接回溯时间求值) ---------- */

static double
relaxramp(double x, double r)
{
    return x <= 0 ? 0 : x < r ? x * x / (2 * r) : x - r / 2;
}

/* 星系群整体绕竖直轴的缓慢漂移: 开场 .09 rad/s, 进入驻留后 3s 内降到 .025 rad/s (导演镜头负责主要的视角变化) */
static double
relaxdrift(double motion)
{
    double x = relaxclamp((motion - RELAXIEND) / 3);

    return .09 * relaxramp(motion - 1, .8)
        - .065 * (3 * (x * x * x - x * x * x * x / 2) + MAX(0, motion - RELAXIEND - 3));
}

static RelaxMat
relaxworldat(double stage, double motion)
{
    double u = relaxphase(stage, 2.2, 3.3);
    /* 2.2s 静止 -> 加速 -> 约 2.85s 峰值 (≈1.5 转/秒) -> 3.3s 减速到几乎停止, 之后保持缓慢漂移 */
    double theta = relaxdrift(motion) + 2 * RELAXPI * relaxeaseinoutcubic(pow(u, 1.25));
    double tilt = .1 * relaxsmoothstep(relaxphase(stage, 1, 2.2)) + .2 * sin(RELAXPI * u)
        - .1 * relaxsmoothstep(relaxphase(stage, 4.8, 5.5)) + .06 * relaxscene.holdw * sin(2 * RELAXPI * motion / 40);
    return relaxmul(relaxrotx(tilt), relaxmul(relaxroty(theta), relaxrotz(.05 * sin(RELAXPI * u))));
}

static double
relaxexitangle(double elapsed)
{
    /* 不满一圈的同向弧线, 以平缓的速度曲线进入收束. */
    return 1.15 * RELAXPI * relaxsmoothstep(relaxphase(elapsed, .15, RELAXEXITSTART));
}

/* 开普勒方程 E - e sinE = M (Newton 迭代) */
static double
relaxkepler(double m, double e)
{
    double E = m + e * sin(m);
    int i;

    for (i = 0; i < 4; i++)
        E -= (E - e * sin(E) - m) / (1 - e * cos(E));
    return E;
}

/* 椭圆群轨道上偏近点角 E 处的点. 轨道在水平盘面 (xz) 内, 焦点在原点 (中心光源), 再按升交点 / 倾角 / 近点角摆放 */
static RelaxVec
relaxlanepoint(int lane, double E)
{
    const RelaxLane *l = &relaxlanes[lane];
    double a = l->a * relaxscene.w, b = a * sqrt(1 - l->e * l->e), d2r = RELAXPI / 180;
    RelaxMat m = relaxmul(relaxroty(l->node * d2r), relaxmul(relaxrotx(l->inc * d2r), relaxroty(l->peri * d2r)));

    return relaxapply(m, relaxv(a * (cos(E) - l->e), 0, b * sin(E)));
}

/* 核心在 motion 时刻的平近点角: 匀速增长, 位置由开普勒方程给出, 近点快远点慢 */
static double
relaxanomaly(RelaxGalaxy *g, double motion, double spin)
{
    const RelaxLane *l = &relaxlanes[g->lane];

    return g->orbitphase + l->dir * (2 * RELAXPI * relaxscene.tscale / l->period * (motion - 2.8) + spin);
}

/* 驻留后的周期事件: 进入驻留 offset 真实秒后开始, 每 every 真实秒一次.
 * 返回 1 表示已开始, *k 是第几次, *local 是本次开始后的真实秒数 (motion 的纯函数) */
static int
relaxcycle(double motion, double offset, double every, int *k, double *local)
{
    double ts = relaxscene.tscale, x = motion - RELAXIEND - offset / ts, e = every / ts;

    if (x < 0)
        return 0;
    *k = (int)floor(x / e);
    *local = (x - *k * e) * ts;
    return 1;
}

/* 闪光包络: attack 秒内平滑亮起, 之后按 decay 指数衰减. 不能从 0 一帧跳到最亮, 否则画面会「跳一下」 */
static double
relaxflash(double t, double attack, double decay)
{
    if (t <= 0)
        return 0;
    return t < attack ? relaxsmoothstep(t / attack) : exp(-decay * (t - attack));
}

/* 星系翻转: 驻留后每 5s 轮到一个有窗口的星系, 用 3.2s 绕自身轨道平面的 x 轴翻转一整圈 */
static double
relaxflipat(RelaxGalaxy *g, double motion)
{
    RelaxScene *r = &relaxscene;
    double local;
    int k;

    if (r->npop < 1 || !relaxcycle(motion, 1.5, 5, &k, &local) || r->popord[k % r->npop] != g->tag)
        return 0;
    return 2 * RELAXPI * relaxeaseinoutcubic(local / 3.2);
}

/* 轨道呼吸: 局部轨道环缓慢胀缩, 相邻星系节奏错开. 环和环上的星体共用这个系数 */
static double
relaxbreathe(RelaxGalaxy *g, double motion)
{
    return 1 + .07 * relaxsmoothstep(relaxphase(motion, RELAXIEND, RELAXIEND + 3))
        * sin(2 * RELAXPI * motion * relaxscene.tscale / 9 + g->phase);
}

/* 旋涡扭转: 高速旋转时内圈比外圈多转, 星系群和群轨道被拧成螺旋, 减速时解开 (stage 的纯函数) */
static RelaxVec
relaxtwist(RelaxVec v, double stage)
{
    double u = relaxphase(stage, 2.2, 3.3), a;

    if (u <= 0 || u >= 1)
        return v;
    a = 1.8 * pow(sin(RELAXPI * u), 2) * (1 - MIN(1, sqrt(v.x * v.x + v.z * v.z) / (.5 * relaxscene.w)));
    return relaxapply(relaxroty(a), v);
}

static void
relaxgalaxyat(RelaxGalaxy *g, double stage, double motion, RelaxMat world, RelaxVec *pos, RelaxMat *plane)
{
    RelaxScene *r = &relaxscene;
    double emerge = relaxeaseoutcubic(relaxphase(stage, .6, 1));
    double expand = relaxeaseinoutcubic(relaxphase(stage, 1.5, 2.2));
    double ready = relaxeaseinoutcubic(relaxphase(stage, 1.65, 2.8));
    double spin = r->exitspin;
    double side = g->tag % 2 ? 1 : -1;
    double merge = relaxeaseinoutcubic(relaxphase(stage, 5 + .03 * (g->tag % 4), 5.45));
    RelaxVec p, ctrl, orbit;

    p = relaxv(g->home.x * relaxmix(.62, 1, expand), g->home.y * relaxmix(.62, 1, expand),
            g->home.z * relaxmix(.45, 1, expand) + (1 - emerge) * 1.2 * r->cam.focal);
    if (r->mode == RelaxCollapse && motion < r->motion)
        spin = relaxexitangle(r->celapsed - (r->motion - motion) * r->tscale);
    orbit = relaxlanepoint(g->lane, relaxkepler(relaxanomaly(g, motion, spin), relaxlanes[g->lane].e));
    p = relaxlerp(p, orbit, ready);
    p = relaxadd(p, relaxv(0, (1 - ready) * .012 * r->h * sin(.9 * motion + g->phase),
                (1 - ready) * .02 * r->cam.focal * sin(.6 * motion + 1.3 * g->phase)));
    if (merge > 0) {
        /* 星系核心沿各自方向的 3D 曲线汇聚到中心 */
        ctrl = relaxadd(relaxscale(relaxapply(relaxroty(1.1 * side), p), .8), relaxv(0, -.2 * r->h * side, 0));
        p = relaxbezier(p, ctrl, relaxv(0, 0, 0), merge);
    }
    *pos = relaxapply(world, relaxtwist(p, stage));
    *plane = relaxmul(world, relaxmul(relaxeuler(g->rx, g->ry + g->precess * motion, g->rz),
                relaxrotx(relaxflipat(g, motion))));
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
    double rad = s->radius * (1 - c) * relaxbreathe(g, motion), a = relaxorbitangle(s, stage, motion);
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
    RelaxGalaxy *g, *h;
    RelaxVec d;
    double appear = relaxeaseoutcubic(relaxphase(stage, .6, 1.1));
    double absorb = relaxeaseinoutcubic(relaxphase(stage, 4.9, 5.3));
    double merge = relaxeaseincubic(relaxphase(stage, 5.05, 5.45));
    double th = .09 * r->w, flare[32] = {0}, dist, f, E;
    int i, j;

    for (i = 0; i < r->ntags; i++) {
        g = &r->galaxies[i];
        relaxgalaxyat(g, stage, motion, r->world, &g->pos, &g->plane);
        g->anomaly = relaxanomaly(g, motion, r->exitspin);
        E = relaxkepler(g->anomaly, relaxlanes[g->lane].e);
        /* 近点甩掠: 经过近点时更亮更大 (速度变化由开普勒运动自带) */
        g->peri = r->holdw * relaxsmoothstep((cos(E) - .6) / .4);
        g->flip = sin(.5 * relaxflipat(g, motion));
        g->ripple = .6 * r->rippleamp * exp(-pow((relaxlen(g->pos) - r->ripple) / (.09 * r->w), 2));
        g->nudge = relaxv(0, 0, 0);
        g->alpha = appear * (1 - .9 * merge) * (g->nstars ? 1 : .55) * (1 + .35 * absorb * (g->nstars > 0));
        g->hover = relaxfollow(g->hover, r->hovercore == i, dt, .12);
    }
    /* 交会: 不同轨道上的两个核心在 3D 中靠近时同时闪光, 并沿连线互相吸引一点 */
    for (i = 0; i < r->ntags && i < 32; i++)
        for (j = i + 1; j < r->ntags && j < 32; j++) {
            g = &r->galaxies[i];
            h = &r->galaxies[j];
            if (g->lane == h->lane || g->alpha < .1 || h->alpha < .1)
                continue;
            d = relaxsub(h->pos, g->pos);
            dist = relaxlen(d);
            f = r->holdw * relaxclamp(1 - dist / th);
            if (f <= 0)
                continue;
            d = relaxscale(d, .012 * r->w * f / MAX(dist, 1));
            g->nudge = relaxadd(g->nudge, d);
            h->nudge = relaxsub(h->nudge, d);
            flare[i] = MAX(flare[i], f);
            flare[j] = MAX(flare[j], f);
        }
    for (i = 0; i < r->ntags; i++) {
        g = &r->galaxies[i];
        g->pos = relaxadd(g->pos, g->nudge);
        g->flare = relaxfollow(g->flare, i < 32 ? flare[i] : 0, dt, .2);
        g->p = relaxproject(g->pos);
    }
}

/* 轨道环拆为短弧, 用每段的镜头深度与窗口和核心一起排序. */
static void
relaxupdaterings(double shrink)
{
    RelaxScene *r = &relaxscene;
    RelaxGalaxy *g;
    RelaxProj *pts;
    RelaxMat m;
    double th, rad, z;
    int i, k, j, arc;

    for (i = 0; i < r->ntags; i++) {
        g = &r->galaxies[i];
        for (k = 0; k < g->nrings; k++) {
            memset(g->ringn[k], 0, sizeof g->ringn[k]);
            memset(g->ringz[k], 0, sizeof g->ringz[k]);
            if (r->ringalpha < .003 || !g->p.ok)
                continue;
            m = relaxmul(g->plane, g->ring[k]);
            rad = g->ringr[k] * (1 - shrink) * relaxbreathe(g, r->motion);
            pts = r->rpts + (i * RELAXRINGS + k) * (RELAXSEG + 1);
            for (j = 0; j <= RELAXSEG; j++) {
                th = 2 * RELAXPI * j / RELAXSEG;
                pts[j] = relaxproject(relaxadd(g->pos, relaxapply(m, relaxv(cos(th) * rad, sin(th) * rad, 0))));
            }
            for (j = 0; j < RELAXSEG; j++) {
                if (!pts[j].ok || !pts[j + 1].ok)
                    continue;
                z = (pts[j].z + pts[j + 1].z) * .5;
                arc = j / RELAXARCSEG;
                g->ringz[k][arc] += z;
                g->ringn[k][arc]++;
            }
            for (arc = 0; arc < RELAXARCS; arc++)
                if (g->ringn[k][arc])
                    g->ringz[k][arc] /= g->ringn[k][arc];
        }
    }
}

/* 星系群椭圆轨道 (淡淡的底线) + 每个核心身后的长曝光星轨 + 中心光源 */
static void
relaxupdatecluster(double shrink)
{
    RelaxScene *r = &relaxscene;
    RelaxProj *pts;
    RelaxGalaxy *g;
    RelaxVec v;
    double E, z[3], m;
    int lane, j, arc, i, n[3];

    for (lane = 0; lane < RELAXLANES; lane++) {
        pts = r->clusterpts[lane];
        memset(r->clustern[lane], 0, sizeof r->clustern[lane]);
        memset(r->clusterz[lane], 0, sizeof r->clusterz[lane]);
        if (r->clusteralpha < .003 || !(r->lanemask & 1 << lane))
            continue;
        for (j = 0; j <= RELAXSEG; j++) {
            v = relaxscale(relaxlanepoint(lane, 2 * RELAXPI * j / RELAXSEG), 1 - shrink);
            r->clusterr[lane][j] = relaxlen(v);
            pts[j] = relaxproject(relaxapply(r->world, relaxtwist(v, r->stage)));
        }
        for (j = 0; j < RELAXSEG; j++) {
            if (!pts[j].ok || !pts[j + 1].ok)
                continue;
            arc = j / RELAXARCSEG;
            r->clusterz[lane][arc] += (pts[j].z + pts[j + 1].z) * .5;
            r->clustern[lane][arc]++;
        }
        for (arc = 0; arc < RELAXARCS; arc++)
            if (r->clustern[lane][arc])
                r->clusterz[lane][arc] /= r->clustern[lane][arc];
    }
    /* 星轨: 沿当前椭圆往回取一段平近点角. 时间均匀, 所以近点处拉得长, 远点处缩得短 */
    for (i = 0; i < r->ntags; i++) {
        g = &r->galaxies[i];
        pts = r->streakpts + i * (RELAXSTREAK + 1);
        for (j = 0; j < 3; j++) {
            r->streakz[i * 3 + j] = -1;
            z[j] = n[j] = 0;
        }
        if (r->streakalpha < .003 || g->alpha < .01 || !g->p.ok)
            continue;
        pts[0] = g->p;
        for (j = 1; j <= RELAXSTREAK; j++) {
            m = g->anomaly - relaxlanes[g->lane].dir * RELAXSTREAKM * j / RELAXSTREAK;
            E = relaxkepler(m, relaxlanes[g->lane].e);
            pts[j] = relaxproject(relaxapply(r->world, relaxtwist(relaxscale(relaxlanepoint(g->lane, E), 1 - shrink), r->stage)));
        }
        for (j = 0; j < RELAXSTREAK; j++)
            if (pts[j].ok && pts[j + 1].ok) {
                z[j * 3 / RELAXSTREAK] += (pts[j].z + pts[j + 1].z) * .5;
                n[j * 3 / RELAXSTREAK]++;
            }
        for (j = 0; j < 3; j++)
            if (n[j])
                r->streakz[i * 3 + j] = z[j] / n[j];
    }
    r->sunp = relaxproject(relaxv(0, 0, 0));
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

    /* 俯冲起卡片就转为朝向镜头: 平行于画面的卡片走仿射采样, 近在眼前也不卡 */
    hb = MAX(hb, relaxbeatw() * relaxsmoothstep(relaxphase(r->iclock, 3.1, 3.5)));
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
        if (g->callout > .001)  /* 点名: 环上的卡片沿轨道半径向外弹一下 */
            s->pos = relaxadd(s->pos, relaxscale(relaxnormalize(relaxsub(s->pos, g->pos)), .18 * s->radius * g->callout));
        if (s->flipcard > 0)    /* 翻面亮相: 绕自身竖轴转一圈 */
            s->orient = relaxmul(s->orient, relaxroty(2 * RELAXPI * s->flipcard));
        s->vel = dt > 0 ? relaxscale(relaxsub(s->pos, prev), 1 / dt) : relaxv(0, 0, 0);
        /* 首帧必须和桌面截图逐像素对齐; 聚焦放大等卡片离开桌面后才开始. */
        s->size = size * (1 + (s->focused ? .08 * relaxsmoothstep(relaxphase(stage, .12, .75)) : 0))
            * (1 + .42 * s->hover);
        collapse = relaxphase(stage, 4.8 + s->delay * .5, 5.2 + s->delay * .5);
        /* 当前桌面的窗口从第一帧起就画在自己原来的位置上 (与真实窗口逐像素重合), 背后的桌面截图先淡出再开始移动,
         * 移动中的窗口不会和静止的桌面截图叠成重影 */
        s->alpha = (s->current ? 1 : relaxeaseoutquart(relaxphase(stage, .6, 1)))
            * (1 - relaxeaseincubic(relaxphase(collapse, .7, 1)));
        s->p = relaxproject(s->pos);
        if (s->p.ok && stage > 1)    /* 开场最初几帧卡片与真实窗口重合, 不能淡 */
            s->alpha *= relaxnearfade(s->p.z);
        if ((r->mode == RelaxOrbit || (r->mode == RelaxIntro && r->iclock > 3)) && s->p.ok) {
            /* 驻留时卡片有尺寸上限; 俯冲时卡片近在眼前, 也限制在 700px 以内 */
            double screen = MAX(s->w, s->h) * s->size * s->p.scale;
            double cap = r->mode == RelaxOrbit ? relaxmix(460, 600, s->hover) : 700;
            if (screen > cap)
                s->size *= cap / screen;
        }
        s->brightness = s->p.ok ? relaxdepthlight(s->p.z) * (s->focused ? 1.1 : 1) * (1 + .25 * s->hover) : 0;
        if (s->current)  /* 脱离桌面前与背景一起变暗 (100% -> 70%), 之后过渡到深度亮度 */
            s->brightness = relaxmix(RELAXCURVE(relaxbright, stage), s->brightness, relaxsmoothstep(relaxphase(stage, .1, .58)));
        /* 深度 LOD: 投影后的卡片足够大才显示截图面板, 远处只剩光点 */
        s->lod = s->p.ok ? relaxsmoothstep((MAX(s->w, s->h) * s->size * s->p.scale - 52) / 58) : 0;
        s->vis = MAX(vis * s->lod, .85 * s->hover * (vis > .05)) * s->alpha;
        s->tint = (r->mode == RelaxOrbit ? 0 : tint + .1 * s->hover) * s->alpha * MIN(1, s->vis / .3);
        s->glow = glow * s->alpha * MIN(1, s->brightness);
        if (s->flipcard > 0 && s->flipcard < 1) {   /* 翻面时一道扫过的高光 */
            s->tint += .5 * sin(RELAXPI * s->flipcard) * s->alpha;
            s->glow += .6 * sin(RELAXPI * s->flipcard) * s->alpha;
        }
        s->brightness *= 1 + .3 * g->callout + .35 * s->constel;
    }
}

static int
relaxitemcmp(const void *pa, const void *pb)
{
    const RelaxItem *a = pa, *b = pb;
    return a->z > b->z ? -1 : a->z < b->z;
}

/* 画家算法: 每帧按镜头空间 z 从远到近排序, 轨道短弧与核心和窗口穿插. */
static void
relaxsortdepth(void)
{
    RelaxScene *r = &relaxscene;
    RelaxGalaxy *g;
    int i, k, arc, n = 0;

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
            for (arc = 0; arc < RELAXARCS; arc++)
                if (g->ringn[k][arc])
                    r->items[n++] = (RelaxItem){RelaxRingItem, (i * RELAXRINGS + k) * RELAXARCS + arc, g->ringz[k][arc]};
    }
    for (i = 0; i < RELAXLANES; i++)
        for (arc = 0; arc < RELAXARCS; arc++)
            if (r->clustern[i][arc])
                r->items[n++] = (RelaxItem){RelaxClusterItem, i * RELAXARCS + arc, r->clusterz[i][arc]};
    for (i = 0; i < r->ntags * 3; i++)
        if (r->streakz[i] > 0)
            r->items[n++] = (RelaxItem){RelaxStreakItem, i, r->streakz[i]};
    if (r->sunp.ok && r->sunalpha > .003)
        r->items[n++] = (RelaxItem){RelaxSunItem, 0, r->sunp.z};
    for (i = 0; i < r->nstars; i++)
        if (r->stars[i].p.ok && r->stars[i].alpha > .002)
            r->items[n++] = (RelaxItem){RelaxStarItem, i, r->stars[i].p.z};
    r->nitems = n;
    qsort(r->items, n, sizeof *r->items, relaxitemcmp);
}

/* 开场 / 驻留 / 坍缩: 整个场景由 (stage, motion) 决定; dt 是真实时间, 只用于平滑交互 */
/* ---------- 导演镜头: 驻留时按机位轮换, 交互时停住让位 ---------- */

typedef struct { double pitch, yaw, dist, roll; } RelaxShot;
enum { RelaxShotWide, RelaxShotEdge, RelaxShotBelow, RelaxShotTour, RelaxShotCross, RelaxShotTop };
static const char *relaxshotname[] = { "wide", "edge", "below", "tour", "cross", "top" };
/* 全景与巡游交替: 巡游时镜头飞到某个星系身边绕着它转, 两次巡游之间飞越到下一个星系 */
static const int relaxshotseq[] = {
    RelaxShotWide, RelaxShotTour, RelaxShotTour, RelaxShotEdge, RelaxShotTour, RelaxShotBelow,
    RelaxShotTour, RelaxShotCross, RelaxShotTour, RelaxShotTop, RelaxShotTour
};

/* 机位在其开始后 t 真实秒的参数 (t 可略超出 [0, RELAXSHOT], 用于过渡段). 角度单位: 度.
 * slot: 巡游机位用哪个跟踪槽 (0 当前 / 1 下一个) */
static RelaxShot
relaxshotat(int shot, double t, int slot)
{
    RelaxScene *r = &relaxscene;
    double x = t / RELAXSHOT, breathe = 1 + .12 * sin(2 * RELAXPI * r->dclock / 20);

    switch (shot) {
    case RelaxShotEdge:     /* 贴近盘面侧掠: 椭圆压成细线, 核心前后遮挡 */
        return (RelaxShot){-4, -30 + 60 * x, .68 * breathe, RELAXDIAG + 2};
    case RelaxShotBelow:    /* 从盘面下方仰视 */
        return (RelaxShot){11, 34 - 34 * x, .7 * breathe, RELAXDIAG - 4};
    case RelaxShotTour:     /* 巡游: 在核心的轨道外侧, 绕着它慢慢转, 远处是整个星系群 */
        return (RelaxShot){-17 + 7 * sin(2 * RELAXPI * t / 14), r->dtyaw[slot] + 30 + 35 * x, .5, RELAXDIAG * .6};
    case RelaxShotCross:    /* 镜头从盘面上方推过盘面到下方 */
        return (RelaxShot){-20 + 32 * relaxsmoothstep(x), 48 - 18 * x, (.76 - .08 * x) * breathe, RELAXDIAG - 2};
    case RelaxShotTop:      /* 高空俯瞰 */
        return (RelaxShot){-34, 8 + 26 * x, .9 * breathe, RELAXDIAG};
    default:                /* 低角度斜视全景 */
        return (RelaxShot){-14, -4 + 28 * x, .72 * breathe, RELAXDIAG};
    }
}

/* 第 k 个机位 (若是巡游) 轮到的星系: 依次轮到有窗口的星系, 没有窗口时轮到全部核心 */
static int
relaxtourgalaxy(int k)
{
    RelaxScene *r = &relaxscene;
    int n = LENGTH(relaxshotseq), per = 0, before = 0, i, idx;

    for (i = 0; i < n; i++) {
        per += relaxshotseq[i] == RelaxShotTour;
        before += i < k % n && relaxshotseq[i] == RelaxShotTour;
    }
    idx = k / n * per + before;
    return r->npop ? r->popord[idx % r->npop] : idx % MAX(1, r->ntags);
}

/* 巡游跟踪槽: 平滑跟随核心位置, yaw 取核心的径向方向 (镜头在轨道外侧) */
static void
relaxtourtrack(int slot, double dt, int snap)
{
    RelaxScene *r = &relaxscene;
    RelaxGalaxy *g;
    double ty;

    if (r->dtg[slot] < 0 || r->dtg[slot] >= r->ntags)
        return;
    g = &r->galaxies[r->dtg[slot]];
    ty = atan2(-g->pos.x, -g->pos.z) * 180 / RELAXPI;
    if (snap) {
        r->dtour[slot] = g->pos;
        r->dtyaw[slot] = ty;
        return;
    }
    r->dtour[slot] = relaxlerp(r->dtour[slot], g->pos, 1 - exp(-dt * r->dspeed / .45));
    r->dtyaw[slot] = relaxfollow(r->dtyaw[slot], r->dtyaw[slot] + remainder(ty - r->dtyaw[slot], 360), dt * r->dspeed, 1.2);
}

static void
relaxupdatecamera(double stage, double motion, double dt)
{
    RelaxScene *r = &relaxscene;
    RelaxShot a, b;
    RelaxGalaxy *g;
    RelaxVec ta = relaxv(0, 0, 0), tb = relaxv(0, 0, 0), target;
    double hw = r->holdw, dw = hw, period = RELAXSHOT + RELAXSHOTMIX, x, mix, wf, dev, fit, dist;
    double kd, kp, ky, kr, bw, s = r->iclock;
    int k, sa, sb, i, active, n = LENGTH(relaxshotseq);

    /* 基础镜头: 关键帧 (stage); 旋转之后换成节拍镜头 (开场时钟), 快进 / 坍缩时由 beatfade 淡回关键帧 */
    kd = RELAXCURVE(relaxcamdist, stage);
    kp = RELAXCURVE(relaxcampitch, stage);
    ky = RELAXCURVE(relaxcamyaw, stage);
    kr = RELAXCURVE(relaxcamroll, stage);
    bw = (r->mode == RelaxIntro || r->mode == RelaxCollapse) && s < RELAXIEND
        ? relaxsmoothstep(relaxphase(s, 3.2, 3.5)) * r->beatfade : 0;
    if (bw > 0) {
        kd = relaxmix(kd, RELAXCURVE(relaxbeatdist, s), bw);
        kp = relaxmix(kp, RELAXCURVE(relaxbeatpitch, s), bw);
        ky = relaxmix(ky, RELAXCURVE(relaxbeatyaw, s), bw);
        kr = relaxmix(kr, RELAXCURVE(relaxbeatroll, s), bw);
    }
    if (r->mode == RelaxIntro || r->mode == RelaxCollapse)
        /* 起飞时轻推一下; 超空间跃迁时镜头前冲 */
        kd *= 1 - r->beatfade * (.03 * sin(RELAXPI * relaxphase(s, .1, .6)) + .08 * r->warpfx);

    if (r->mode == RelaxCollapse)
        dw *= 1 - relaxsmoothstep(r->celapsed / 1.1);
    /* 交互让位: 最近 4s 动过鼠标 / 滚轮, 或指针停在星体 / 核心上时, 导演时钟平滑减速到停 */
    active = r->last - r->lastpointer < 4 || r->hover >= 0 || r->hovercore >= 0;
    r->dspeed = relaxfollow(r->dspeed, !active, dt, .6);
    if (r->mode == RelaxOrbit)
        r->dclock += dt * r->dspeed;
    k = (int)floor(r->dclock / period);
    x = r->dclock - k * period;
    sa = relaxshotseq[k % n];
    sb = relaxshotseq[(k + 1) % n];
    if (k != r->dshot) {
        /* 进入新机位: 原来「下一个」的跟踪槽变成「当前」 */
        if (r->dshot >= 0 && k == r->dshot + 1) {
            r->dtg[0] = r->dtg[1];
            r->dtour[0] = r->dtour[1];
            r->dtyaw[0] = r->dtyaw[1];
        } else {
            r->dtg[0] = sa == RelaxShotTour ? relaxtourgalaxy(k) : -1;
            relaxtourtrack(0, dt, 1);
        }
        r->dtg[1] = sb == RelaxShotTour ? relaxtourgalaxy(k + 1) : -1;
        relaxtourtrack(1, dt, 1);
        r->dshot = k;
        if (r->log && r->mode == RelaxOrbit)
            fprintf(r->log, "galaxy shot: %s (tag %d) at %.1fs\n", relaxshotname[sa],
                    sa == RelaxShotTour ? r->dtg[0] + 1 : 0, motion);
    }
    if (r->dspeed > .01) {
        relaxtourtrack(0, dt, 0);
        relaxtourtrack(1, dt, 0);
    }
    a = relaxshotat(sa, x, 0);
    b = relaxshotat(sb, x - period, 1);
    mix = relaxeaseinoutcubic((x - RELAXSHOT) / RELAXSHOTMIX);
    b.yaw = a.yaw + remainder(b.yaw - a.yaw, 360);   /* 机位之间 yaw 走短弧 */
    if (sa == RelaxShotTour && r->dtg[0] >= 0)
        ta = r->dtour[0];
    if (sb == RelaxShotTour && r->dtg[1] >= 0)
        tb = r->dtour[1];
    target = relaxscale(relaxlerp(ta, tb, mix), dw);
    wf = (sa == RelaxShotTour) * (1 - mix) + (sb == RelaxShotTour) * mix;
    /* 自动取景: 只在全景机位里让核心贴近屏幕边缘. 偏移按滚轮缩放归一, 不会把用户的缩放抵消掉 */
    if (r->mode == RelaxOrbit && r->dspeed > .5 && wf < .5) {
        dev = 0;
        for (i = 0; i < r->ntags; i++) {
            g = &r->galaxies[i];
            if (!g->p.ok || g->alpha < .1)
                continue;
            dev = MAX(dev, (fabs(g->p.x - r->w * .5) + g->size * g->p.scale) / (r->w * .5));
            dev = MAX(dev, (fabs(g->p.y - r->h * .5) + g->size * g->p.scale) / (r->h * .5));
        }
        if (dev > 0) {
            dev *= relaxmix(1, r->zoom, hw);
            fit = MAX(.8, MIN(1.25, r->dfit * dev / .98));
            r->dfit = relaxfollow(r->dfit, fit, dt, 1.5);
        }
    }
    dist = relaxmix(a.dist * relaxmix(r->dfit, 1, sa == RelaxShotTour), b.dist * relaxmix(r->dfit, 1, sb == RelaxShotTour), mix);
    if (sa == RelaxShotTour || sb == RelaxShotTour)
        dist += .35 * sin(RELAXPI * mix);   /* 飞越: 先拉远再推近 */
    relaxsetcameraat(target,
            MAX(.35, relaxmix(kd, dist, dw)) * r->cam.focal * relaxmix(1, r->zoom, hw),
            relaxmix(kp, relaxmix(a.pitch, b.pitch, mix), dw) + hw * r->ppitch,
            relaxmix(ky, relaxmix(a.yaw, b.yaw, mix), dw) + hw * r->pyaw,
            relaxmix(kr, relaxmix(a.roll, b.roll, mix), dw));
}

/* 星系翻转 / 涟漪各记一行日志, 便于核对画面 */
static void
relaxlogactions(double motion)
{
    RelaxScene *r = &relaxscene;
    double local;
    int k;

    if (!r->log || r->mode != RelaxOrbit)
        return;
    if (r->npop >= 1 && relaxcycle(motion, 1.5, 5, &k, &local) && k != r->lastflip) {
        r->lastflip = k;
        fprintf(r->log, "galaxy flip: tag %d at %.1fs\n", r->popord[k % r->npop] + 1, motion);
    }
    if (relaxcycle(motion, 4, 6, &k, &local) && k != r->lastripple) {
        r->lastripple = k;
        fprintf(r->log, "galaxy ripple at %.1fs\n", motion);
    }
    if (relaxcycle(motion, 9, 20, &k, &local) && k != r->lastnova) {
        r->lastnova = k;
        fprintf(r->log, "galaxy supernova: tag %d at %.1fs\n", (k * 5 + 3) % r->ntags + 1, motion);
    }
    if (relaxcycle(motion, 6, 15, &k, &local) && k != r->lastcomet) {
        r->lastcomet = k;
        fprintf(r->log, "galaxy comet at %.1fs\n", motion);
    }
}

/* 涟漪: 驻留后每 6s 从中心光源发出一圈亮波, 以 .4 屏宽/秒沿轨道向外扩散, 扫过的核心短暂亮起 */
static void
relaxupdateripple(double motion)
{
    RelaxScene *r = &relaxscene;
    double local;   /* 本次涟漪发出后的真实秒数 */
    int k;

    r->ripple = r->rippleamp = r->sunpulse = 0;
    if (!relaxcycle(motion, 4, 6, &k, &local))
        return;
    r->ripple = .4 * r->w * local;
    r->rippleamp = relaxclamp(1 - r->ripple / (.65 * r->w));
    r->sunpulse = .7 * relaxflash(local, .6, 2);
}

/* 局部轨道环的描绘进度: 像光笔一样从起点沿环画出一整圈 */
static double
relaxringreveal(RelaxGalaxy *g)
{
    double s = relaxscene.iclock;

    return 1 - relaxbeatw() * (1 - relaxeaseinoutcubic(relaxphase(s, 1 + .05 * g->rank, 1.9 + .05 * g->rank)));
}

/* 开场节拍: 跃迁 / 轨道描绘 / 点火 / 点名 / 星轨拉出 / 卡片翻面 / 转速峰值爆闪 / 落定涟漪 */
static void
relaxupdatebeats(double motion)
{
    RelaxScene *r = &relaxscene;
    RelaxGalaxy *g;
    double f = relaxbeatw(), s = r->iclock, t, x;
    int i, k;

    r->warpfx = f * (pow(sin(RELAXPI * relaxphase(s, .55, 1.35)), 2) + .5 * pow(sin(RELAXPI * relaxphase(s, 3.3, 3.95)), 2));
    for (i = 0; i < RELAXLANES; i++)
        r->lanereveal[i] = 1 - f * (1 - relaxeaseinoutcubic(relaxphase(s, 1.5 + .12 * i, 2.3 + .12 * i)));
    for (i = 0; i < r->ntags; i++) {
        g = &r->galaxies[i];
        g->ignite = f * exp(-pow((s - .62 - .06 * g->rank) / .1, 2));
        g->streakreveal = 1 - f * (1 - relaxeaseinoutcubic(relaxphase(s, 4.6 + .05 * g->rank, 5.4 + .05 * g->rank)));
        g->callout = 0;
    }
    /* 铺满全屏时, 有窗口的星系依次点名 */
    for (k = 0; k < r->npop; k++) {
        t = 3.95 + k * .6 / r->npop;
        r->galaxies[r->popord[k]].callout = f * exp(-pow((s - t) / .09, 2));
    }
    for (i = 0; i < r->nstars; i++) {
        t = 4.9 + .45 * i / MAX(1, r->nstars);
        r->stars[i].flipcard = f * relaxeaseinoutcubic(relaxphase(s, t, t + .5));
    }
    r->sunpulse += f * 1.5 * exp(-pow((s - 2.85) / .12, 2));
    /* 回缩落定的一刻, 中心光源发出一圈大涟漪 (之后接驻留的周期涟漪) */
    x = (motion - RELAXIEND + .25) * r->tscale;
    if (x >= 0 && x < 2.5 && r->mode != RelaxCollapse) {
        t = 1.3 * relaxclamp(1 - .45 * r->w * x / (.75 * r->w));
        if (t > r->rippleamp) {
            r->ripple = .45 * r->w * x;
            r->rippleamp = t;
        }
        r->sunpulse = MAX(r->sunpulse, relaxflash(x, .6, 2));
    }
}

/* 驻留特效的状态 (强度乘 holdw): 超新星 / 星座连线 / 核心光桥 / 超新星附近的尘埃提亮 */
static void
relaxupdateholdfx(double motion)
{
    RelaxScene *r = &relaxscene;
    RelaxGalaxy *g, *h;
    RelaxDust *d;
    double f = r->holdw, local, env, best, dist;
    int i, j, k, gi = -1;

    for (i = 0; i < r->ntags; i++)
        r->galaxies[i].nova = r->galaxies[i].bridge = 0;
    for (i = 0; i < r->nstars; i++)
        r->stars[i].constel = 0;
    if (f < .01 || r->ntags < 1)
        return;
    /* 超新星: 每 20s 轮到一个核心 */
    if (relaxcycle(motion, 9, 20, &k, &local) && local < 3) {
        gi = (k * 5 + 3) % r->ntags;
        r->galaxies[gi].nova = f * relaxflash(local, .4, 1.6);
    }
    for (i = 0; i < r->ndust; i++) {
        d = &r->dust[i];
        d->boost = 0;
        if (gi >= 0 && d->disk) {
            dist = relaxlen(relaxsub(relaxapply(r->world, d->pos), r->galaxies[gi].pos));
            d->boost = r->galaxies[gi].nova * relaxclamp(1 - dist / (.25 * r->w));
        }
    }
    /* 星座连线: 每 4s 轮到一个有窗口的星系 */
    if (r->npop && relaxcycle(motion, 2, 4, &k, &local)) {
        gi = r->popord[k % r->npop];
        env = relaxsmoothstep(local / .4) * (1 - relaxsmoothstep((local - 1.6) / 1));
        for (i = 0; i < r->nstars; i++)
            if (r->stars[i].galaxy == gi)
                r->stars[i].constel = f * env;
    }
    /* 核心光桥: 每 7s 一次, 连接不同轨道上相距最近的两个核心 */
    if (relaxcycle(motion, 3, 7, &k, &local)) {
        if (k != r->bridgek) {
            r->bridgek = k;
            r->bri = r->brj = -1;
            best = 1e18;
            for (i = 0; i < r->ntags; i++)
                for (j = i + 1; j < r->ntags; j++) {
                    g = &r->galaxies[i];
                    h = &r->galaxies[j];
                    if (g->lane == h->lane || g->alpha < .1 || h->alpha < .1 || !g->p.ok || !h->p.ok)
                        continue;
                    /* 加一点随机, 不总是同一对 */
                    dist = relaxlen(relaxsub(g->pos, h->pos)) * (1 + .6 * relaxhash(k * 31 + i * 7 + j));
                    if (dist < best) {
                        best = dist;
                        r->bri = k % 2 ? j : i;
                        r->brj = k % 2 ? i : j;
                    }
                }
        }
        if (r->brj >= 0 && local > 1.15)
            r->galaxies[r->brj].bridge = .7 * f * relaxflash(local - .9, .4, 2.5);
    }
}

static void
relaxupdatescene(double stage, double motion, double dt)
{
    RelaxScene *r = &relaxscene;
    int orbit = r->mode == RelaxOrbit, i;

    r->holdw = relaxsmoothstep(relaxphase(motion, RELAXIEND - .5, RELAXIEND + 2.5))
        * (1 - relaxsmoothstep(relaxphase(stage, RELAXEXIT, RELAXEXIT + .5)));
    r->pyaw = relaxfollow(r->pyaw, orbit ? r->tyaw : 0, dt, .35);
    r->ppitch = relaxfollow(r->ppitch, orbit ? r->tpitch : 0, dt, .35);
    r->zoom = relaxfollow(r->zoom, orbit ? r->tzoom : 1, dt, .25);
    relaxupdateripple(motion);
    r->rippleamp *= r->holdw;
    r->sunpulse *= r->holdw;
    relaxupdatebeats(motion);
    relaxupdatecamera(stage, motion, dt);
    relaxlogactions(motion);
    r->world = r->mode == RelaxCollapse ? r->cworld : relaxworldat(stage, motion);
    relaxupdategalaxies(stage, motion, dt);
    relaxupdateholdfx(motion);
    r->ringalpha = RELAXCURVE(relaxringkeys, stage);
    relaxupdaterings(relaxeaseincubic(relaxphase(stage, 4.8, 5.3)));
    /* 椭圆底线: 高速旋转时较明显, 驻留时退成淡线, 让位给星轨 */
    r->clusteralpha = .2 * relaxsmoothstep(relaxphase(stage, 1.5, 2.3)) * relaxmix(1, .55, relaxsmoothstep(relaxphase(stage, 3.3, 4.2)))
        * (1 - relaxsmoothstep(relaxphase(stage, 5.1, 5.55)));
    r->streakalpha = .24 * relaxsmoothstep(relaxphase(stage, 2.8, 3.8)) * (1 - relaxsmoothstep(relaxphase(stage, 4.85, 5.1)));
    if (r->mode == RelaxIntro)  /* 开场里星轨在回缩时才点亮 */
        r->streakalpha = .24 * relaxmix(relaxsmoothstep(relaxphase(stage, 2.8, 3.8)),
                relaxsmoothstep(relaxphase(r->iclock, 4.6, 5)), r->beatfade);
    r->sunalpha = relaxsmoothstep(relaxphase(stage, 1.5, 2.3)) * (1 - relaxsmoothstep(relaxphase(stage, 4.9, 5.2)));
    if (r->mode == RelaxCollapse) {
        /* 环绕时保留群轨道和星轨, 淡出局部环和历史尾迹, 让运动方向一眼可辨. */
        r->ringalpha *= 1 - .65 * relaxsmoothstep(relaxphase(r->celapsed, .1, .6));
        r->clusteralpha *= 1 - .25 * relaxsmoothstep(relaxphase(r->celapsed, .4, 1.1));
    }
    relaxupdatecluster(relaxeaseinoutcubic(relaxphase(stage, 4.85, 5.45)));
    relaxupdatestars(stage, motion, dt);
    for (i = 0; i < r->ndust; i++)
        r->dust[i].p = relaxproject(r->dust[i].disk ? relaxapply(r->world, r->dust[i].pos) : r->dust[i].pos);
    r->bright = RELAXCURVE(relaxbright, stage);
    r->vign = RELAXCURVE(relaxvignettekeys, stage);
    r->desk = 1 - relaxsmoothstep(relaxphase(stage, 0, .1));
    r->deskover = r->bar = 0;
    r->reveal = relaxeaseinoutcubic(relaxphase(stage, 5.7, 6));
    r->trailgain = MAX(RELAXCURVE(relaxtrailgain, stage), .6 * r->warpfx);
    if (r->mode == RelaxCollapse)
        r->trailgain *= 1 - relaxsmoothstep(relaxphase(r->celapsed, 0, .35));
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

    relaxsetcameraat(relaxscale(r->rctarget, 1 - e), relaxmix(r->rcdist, F, e), relaxmix(r->rcx, 0, e), relaxmix(r->rcy, 0, e), relaxmix(r->rcz, 0, e));
    for (i = 0; i < r->ntags; i++) {
        g = &r->galaxies[i];
        g->pos = g->rpos;
        g->alpha = g->ralpha * fade;
        g->p = relaxproject(g->pos);
    }
    r->ringalpha = r->rring * fade;
    relaxupdaterings(0);
    r->clusteralpha = r->rcluster * fade;
    r->streakalpha = r->rstreak * fade;
    r->sunalpha = r->rsun * fade;
    r->rippleamp = r->sunpulse = 0;
    relaxupdatecluster(0);
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
        r->dust[i].p = relaxproject(r->dust[i].disk ? relaxapply(r->world, r->dust[i].pos) : r->dust[i].pos);
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

/* 点击: 镜头停住. 窗口朝镜头拉近并正对镜头; 核心只变亮, 周围淡掉. 都不飞回桌面坐标. */
static void
relaxupdatepick(double u)
{
    RelaxScene *r = &relaxscene;
    RelaxStar *s, *pick = NULL;
    RelaxGalaxy *g;
    RelaxVec tocam;
    RelaxProj pj;
    double ack = relaxsmoothstep(relaxphase(u, 0, .12 / RELAXPICK));
    double push = relaxeaseinoutcubic(relaxphase(u, .12 / RELAXPICK, .38 / RELAXPICK));
    double ck = relaxsmoothstep(relaxphase(u, .38 / RELAXPICK, 1));
    double fade, dist, travel, endsize;
    int i, samering;

    if (r->rkind == RelaxPickCore) {
        ack = push = 0;
        ck = relaxeaseinoutcubic(u);
    }
    fade = r->rkind == RelaxPickCore ? ck : push;
    if (r->rkind == RelaxPickStar && r->rstar >= 0 && r->rstar < r->nstars)
        pick = &r->stars[r->rstar];
    for (i = 0; i < r->nstars; i++) {
        s = &r->stars[i];
        s->pos = s->rpos;
        s->orient = s->rorient;
        s->size = s->rsize;
        s->tint = s->hover = 0;
        if (s == pick) {
            s->brightness = s->rbright * (1 + .55 * MAX(ack, push));
            s->glow = MIN(2.4, s->rglow + 1.35 * MAX(ack, push));
            s->vis = s->rvis;
            s->alpha = 1;
            if (push > 0) {
                tocam = relaxsub(r->cam.pos, s->rpos);
                dist = relaxlen(tocam);
                travel = MAX(0, dist - r->cam.near * 2.5);
                s->pos = relaxadd(s->rpos, relaxscale(relaxnormalize(tocam), travel * push));
                s->orient = relaxblend(s->rorient, r->cam.rot, push);
                pj = relaxproject(s->pos);
                if (pj.ok && pj.scale > 1e-6) {
                    endsize = .75 * r->w / (MAX(s->w, s->h) * pj.scale);
                    endsize = MAX(.02, MIN(40, endsize));
                    s->size = relaxmix(s->rsize, endsize, push);
                }
            }
            if (!r->fulldesk)
                s->vis *= 1 - ck;
        } else {
            samering = pick && s->galaxy == pick->galaxy && s->ring == pick->ring;
            s->vis = s->rvis * (1 - MAX(samering ? ack * .75 : 0, fade));
            s->alpha = 1 - fade;
            s->glow = s->rglow * (1 - fade);
            s->brightness = s->rbright;
            if (fade > 0 && r->rkind == RelaxPickStar) {
                tocam = relaxsub(s->rpos, r->cam.pos);
                s->pos = relaxadd(s->rpos, relaxscale(relaxnormalize(tocam), 1.4 * r->cam.focal * fade));
            }
        }
        s->p = relaxproject(s->pos);
    }
    for (i = 0; i < r->ntags; i++) {
        g = &r->galaxies[i];
        g->pos = g->rpos;
        if (r->rkind == RelaxPickCore && i == r->rcore) {
            g->alpha = MIN(1, g->ralpha + .35 * ck);
            g->size = r->rcoresize * (1 + .7 * ck);
            g->hover = ck;
        } else {
            g->alpha = g->ralpha * (1 - fade);
            g->hover = 0;
        }
        g->p = relaxproject(g->pos);
    }
    r->ringalpha = r->rring * (1 - fade);
    relaxupdaterings(0);
    r->clusteralpha = r->rcluster * (1 - fade);
    r->streakalpha = r->rstreak * (1 - fade);
    r->sunalpha = r->rsun * (1 - fade);
    r->rippleamp = r->sunpulse = 0;
    relaxupdatecluster(0);
    for (i = 0; i < r->ndust; i++)
        r->dust[i].p = relaxproject(r->dust[i].disk ? relaxapply(r->world, r->dust[i].pos) : r->dust[i].pos);
    r->dustfade = r->rdust * (1 - fade);
    r->bright = r->rbright * (r->rkind == RelaxPickStar && r->fulldesk ? 1 : (1 - .85 * ck));
    r->vign = r->rvign;
    r->desk = r->rdesk;
    r->deskover = r->rkind == RelaxPickStar && r->fulldesk ? ck : 0;
    r->bar = r->reveal = r->trailgain = r->central = r->pulse = 0;
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

/* 四角几乎共面成矩形时返回屏幕上的外接矩形. 开场前几帧卡片还没转, 走这条路径. */
static int
relaxquadrect(double q[4][2], double eps, double *x, double *y, double *w, double *h)
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
relaxcopywindow(RelaxStar *s, int lvl, double left, double top)
{
    RelaxScene *r = &relaxscene;
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
    relaxaffine(s->mip[lvl], 1, 1, 0, 0);
    XRenderSetPictureFilter(dpy, s->mip[lvl], FilterNearest, NULL, 0);
    XRenderComposite(dpy, PictOpSrc, s->mip[lvl], None, r->back, sx, sy, 0, 0, dx, dy, dw, dh);
    XRenderSetPictureFilter(dpy, s->mip[lvl], FilterBilinear, NULL, 0);
}

/* 软件透视很慢时, 先画进半分辨率离屏图再放大. 一次采样, 目标像素约为整屏的四分之一. */
static int
relaxrenderhalf(RelaxStar *s, int lvl, double vis, double q[4][2], int x0, int y0, int x1, int y1)
{
    RelaxScene *r = &relaxscene;
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
    if (!relaxhomography(s->mip[lvl], hq, dw, dh, s->mipw[lvl], s->miph[lvl]))
        return 0;
    XRenderComposite(dpy, PictOpSrc, s->mip[lvl], None, r->spinpic, 0, 0, 0, 0, 0, 0, dw, dh);
    relaxaffine(r->spinpic, (double)dw / bw, (double)dh / bh, 0, 0);
    cx = MAX(0, x0);
    cy = MAX(0, y0);
    dx1 = MIN(r->w, x1);
    dy1 = MIN(r->h, y1);
    if (dx1 <= cx || dy1 <= cy)
        return 0;
    XRenderComposite(dpy, PictOpOver, r->spinpic, vis > .996 ? None : relaxwhite(vis), r->back,
            cx - x0, cy - y0, 0, 0, cx, cy, dx1 - cx, dy1 - cy);
    return 1;
}

/* 大卡片把透视面切成小块, 每块用仿射采样; A1 蒙版避免块与块之间的抗锯齿暗缝.
 * 只在单应矩阵放不下时使用: 分块在 GPU 上是十几次往返, 比一次透视采样更慢. */
static int
relaxrendertiled(RelaxStar *s, int lvl, double vis)
{
    RelaxScene *r = &relaxscene;
    RelaxProj p[4];
    RelaxVec corner;
    XTriangle tri[2];
    XTransform tr;
    XRenderColor clear = {0, 0, 0, 0};
    double q[4][2], dx0, dx1, dy0, dy1, det, sx, sy, minx, miny, maxx, maxy;
    double ax, bx, cx, ay, by, cy, u, v;
    int tx, ty, j, x0, y0, x1, y1, used = 0;

    if (!r->tilemask)
        return 0;
    for (ty = 0; ty < RELAXTILES; ty++)
        for (tx = 0; tx < RELAXTILES; tx++) {
            for (j = 0; j < 4; j++) {
                u = (tx + (j == 1 || j == 2)) / (double)RELAXTILES;
                v = (ty + (j >= 2)) / (double)RELAXTILES;
                corner = relaxadd(s->pos, relaxapply(s->orient,
                            relaxv((u - .5) * s->w * s->size, (v - .5) * s->h * s->size, 0)));
                p[j] = relaxproject(corner);
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
            sx = s->mipw[lvl] / (double)RELAXTILES;
            sy = s->miph[lvl] / (double)RELAXTILES;
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
            XRenderCompositeTriangles(dpy, PictOpOver, relaxwhite(vis), r->tilemask,
                    r->a1, 0, 0, tri, 2);
            XRenderSetPictureTransform(dpy, s->mip[lvl], &tr);
            XRenderComposite(dpy, PictOpOver, s->mip[lvl], r->tilemask, r->back,
                    x0, y0, x0, y0, x0, y0, x1 - x0, y1 - y0);
            used = 1;
        }
    return used;
}

static void
relaxrenderwindow(RelaxStar *s, double vis, double tint, double light)
{
    RelaxScene *r = &relaxscene;
    static const double sx[4] = {-1, 1, 1, -1}, sy[4] = {-1, -1, 1, 1};
    double q[4][2], minx = 1e9, miny = 1e9, maxx = -1e9, maxy = -1e9, edge, hw, hh, dark;
    double left, top, rw, rh, persp;
    RelaxVec corner, normal, tocam;
    RelaxProj p;
    Picture mask;
    int i, lvl, x0, y0, x1, y1, cx, cy, tiled, aligned, pixelcopy, drawn, opaque;

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
    persp = hypot(q[0][0] - q[1][0] + q[2][0] - q[3][0], q[0][1] - q[1][1] + q[2][1] - q[3][1]);
    /* 驻留时卡片朝向镜头, 透视误差很小: 一律走仿射, 不随尺寸在两条路径之间切换 (切换那一帧卡片形状会跳一下) */
    if ((r->mode == RelaxOrbit && (MAX(maxx - minx, maxy - miny) < 480 || persp < 6))
            || (r->mode != RelaxOrbit && r->iclock > 1 && persp < 4)) {
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
    normal = relaxapply(s->orient, relaxv(0, 0, -1));
    tocam = relaxsub(r->cam.pos, s->pos);
    if (relaxdot(normal, tocam) < 0)
        light *= .4;
    /* 选择 mip: 源像素 / 屏幕像素 不超过 2, 远处再降一级 (景深模糊) */
    edge = MAX(hypot(q[1][0] - q[0][0], q[1][1] - q[0][1]), hypot(q[3][0] - q[0][0], q[3][1] - q[0][1]) * s->w / MAX(1, s->h));
    for (lvl = s->base; lvl < RELAXMIPS - 1 && s->mip[lvl + 1] && s->mipw[lvl] > 2 * edge; lvl++);
    if (r->mode == RelaxOrbit) {
        int budget = edge < 150 ? 256 : 512;
        while (lvl < RELAXMIPS - 1 && s->mip[lvl + 1] && s->mipw[lvl] > budget)
            lvl++;
    }
    if (relaxdepthblur(s->p.z) > .55 && lvl < RELAXMIPS - 1 && s->mip[lvl + 1])
        lvl++;
    /* 开场仍与桌面截图像素重合时直接拷贝. 一开始就转的全屏卡如果走 4x4 分块,
     * 每帧十几次合成, 在 :0 上会从约 8ms 掉到 40ms 以上. */
    aligned = relaxquadrect(q, .75, &left, &top, &rw, &rh);
    persp = hypot(q[0][0] - q[1][0] + q[2][0] - q[3][0], q[0][1] - q[1][1] + q[2][1] - q[3][1]);
    opaque = vis > .996 && r->mode != RelaxOrbit;
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
        relaxcopywindow(s, lvl, left, top);
        drawn = 1;
    } else if (opaque && (maxx - minx) * (maxy - miny) > 450000 && persp > 1 && r->projslow) {
        drawn = relaxrenderhalf(s, lvl, vis, q, x0, y0, x1, y1);
    }
    if (!drawn) {
        if (!relaxhomography(s->mip[lvl], q, x1 - x0, y1 - y0, s->mipw[lvl], s->miph[lvl])) {
            if (vis < .004 || !relaxrendertiled(s, lvl, vis))
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
            mask = opaque ? None : relaxwhite(r->mode == RelaxOrbit ? vis * (.7 + .3 * MIN(1, light)) : vis);
            XRenderComposite(dpy, opaque && aligned ? PictOpSrc : PictOpOver, s->mip[lvl], mask, r->back,
                    cx - x0, cy - y0, 0, 0, cx, cy, x1 - cx, y1 - cy);
        }
    }
    dark = r->mode == RelaxOrbit ? 0 : (1 - MIN(1, light)) * vis;
    for (i = 0; i < 2; i++) {
        double a = i ? tint * MIN(1, light * 1.1) : dark;
        Picture color = i ? relaxwhite(a) : relaxblack(a);

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
            relaxfillquad(color, q, x0, y0);
    }
}

static void
relaxrenderglow(int tint, RelaxProj p, double radius, double alpha, double blur, double halo, double outer)
{
    double rr = MIN(radius * p.scale, 80);   /* 俯冲时核心近在眼前: 限制光晕半径, 避免整屏的大面积合成 */

    if (outer > 0)
        relaxsprite(RelaxHalo, tint, p.x, p.y, rr * 7 * (1 + .3 * blur), outer * alpha);
    relaxsprite(RelaxHalo, tint, p.x, p.y, rr * (3 + .8 * blur), halo * alpha);
    relaxsprite(RelaxDisc, tint, p.x, p.y, MAX(.7, rr), alpha * (1 - .5 * blur));
}

/* 细光带 (轨道环 / 尾迹 / 冲击环): 软件画进 a8 画布 (按距离算覆盖率的抗锯齿胶囊), 同一画布内取最大值, 重叠处不叠亮.
 * a 是白色的不透明度 (上限 .25), hw 是半宽 */
static void
relaxband(RelaxProj *pa, RelaxProj *pb, double a, double hw)
{
    RelaxScene *r = &relaxscene;
    float ax = pa->x, ay = pa->y, dx = pb->x - pa->x, dy = pb->y - pa->y, l2, ext, t, px, py, ex, ey, d, cov;
    int x0, y0, x1, y1, x, y, lo, hi, v, tx, ty, steep;
    unsigned char *row;

    double t0;

    if (!r->bandbuf || a < 1 / 64.0)
        return;
    t0 = relaxnow();
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
    for (ty = y0 / RELAXBTILE; ty <= y1 / RELAXBTILE; ty++)
        for (tx = x0 / RELAXBTILE; tx <= x1 / RELAXBTILE; tx++)
            if (!r->banddirty[ty * r->bandtw + tx]) {
                r->banddirty[ty * r->bandtw + tx] = 1;
                r->bandn++;
            }
    r->bandraster += relaxnow() - t0;
}

/* 画布上是否有尚未合成、且与这个屏幕矩形相交的光带 */
static int
relaxbandpending(double x0, double y0, double x1, double y1)
{
    RelaxScene *r = &relaxscene;
    int tx, ty, tx0, tx1, ty1;

    if (!r->bandn)
        return 0;
    tx0 = MAX(0, (int)floor(x0 / RELAXBTILE));
    ty = MAX(0, (int)floor(y0 / RELAXBTILE));
    tx1 = MIN(r->bandtw - 1, (int)floor(x1 / RELAXBTILE));
    ty1 = MIN(r->bandth - 1, (int)floor(y1 / RELAXBTILE));
    for (; ty <= ty1; ty++)
        for (tx = tx0; tx <= tx1; tx++)
            if (r->banddirty[ty * r->bandtw + tx])
                return 1;
    return 0;
}

/* 把画布上有内容的块 (同一行相邻的块合并) 上传并以白色合成到场景, 然后清空这些块 */
static void
relaxflushbands(void)
{
    RelaxScene *r = &relaxscene;
    int tx, ty, run, x, y, w, h, j;
    double t0;

    if (!r->bandn)
        return;
    t0 = relaxnow();
    for (ty = 0; ty < r->bandth; ty++)
        for (tx = 0; tx < r->bandtw; tx++) {
            if (!r->banddirty[ty * r->bandtw + tx])
                continue;
            for (run = 1; tx + run < r->bandtw && r->banddirty[ty * r->bandtw + tx + run]; run++);
            x = tx * RELAXBTILE;
            y = ty * RELAXBTILE;
            w = MIN(run * RELAXBTILE, r->w - x);
            h = MIN(RELAXBTILE, r->h - y);
            XPutImage(dpy, r->bandpix, r->bandgc, r->bandimg, x, y, x, y, w, h);
            XRenderComposite(dpy, PictOpOver, r->white[RELAXALPHAS - 1], r->bandpic, r->back, 0, 0, x, y, x, y, w, h);
            for (j = 0; j < h; j++)   /* XPutImage 已把数据拷进请求缓冲区, 可以马上清 */
                memset(r->bandbuf + (size_t)(y + j) * r->w + x, 0, w);
            memset(r->banddirty + ty * r->bandtw + tx, 0, run);
            r->bandtiles += run;
            tx += run - 1;
        }
    r->bandn = 0;
    r->bandflushes++;
    r->bandflush += relaxnow() - t0;
}

/* 局部轨道: 暗的外沿与清晰的细线叠在短弧内, 由画家算法处理穿插. */
static void
relaxrenderring(int index)
{
    RelaxScene *r = &relaxscene;
    int gi = index / (RELAXARCS * RELAXRINGS), k = index / RELAXARCS % RELAXRINGS;
    int arc = index % RELAXARCS, j;
    RelaxGalaxy *g = &r->galaxies[gi];
    RelaxProj *pts = r->rpts + (gi * RELAXRINGS + k) * (RELAXSEG + 1);
    double base = r->ringalpha * MIN(1, g->alpha) * (1 + .55 * g->hover + 3 * g->callout), z, depth, width;
    double reveal = relaxringreveal(g) * RELAXSEG;

    for (j = arc * RELAXARCSEG; j < (arc + 1) * RELAXARCSEG; j++) {
        if (!pts[j].ok || !pts[j + 1].ok || j >= reveal)
            continue;
        if (j + 1 > reveal)   /* 光笔笔尖 */
            relaxsprite(RelaxHalo, RelaxCool, pts[j].x, pts[j].y, 14 * r->starscale * MAX(.5, pts[j].scale), .5 * MIN(1, g->alpha));
        z = (pts[j].z + pts[j + 1].z) * .5;
        depth = relaxdepthlight(z) * relaxnearfade(z);
        width = MAX(.65, (pts[j].scale + pts[j + 1].scale) * .5 * r->starscale);
        relaxband(&pts[j], &pts[j + 1], base * depth * .24 * (1 - relaxclamp(r->qualityvisual - 2)), width * 3.2);
        relaxband(&pts[j], &pts[j + 1], base * depth * 1.25, width * .7);
    }
}

/* 椭圆群轨道底线: 很淡的细线, 涟漪经过时沿轨道亮起一圈 */
static void
relaxrendercluster(int index)
{
    RelaxScene *r = &relaxscene;
    int lane = index / RELAXARCS, arc = index % RELAXARCS, j;
    RelaxProj *pts = r->clusterpts[lane];
    double a, wave, width, rr;

    for (j = arc * RELAXARCSEG; j < (arc + 1) * RELAXARCSEG; j++) {
        if (!pts[j].ok || !pts[j + 1].ok || j >= r->lanereveal[lane] * RELAXSEG)
            continue;
        if (j + 1 > r->lanereveal[lane] * RELAXSEG)   /* 光笔笔尖 */
            relaxsprite(RelaxHalo, RelaxCool, pts[j].x, pts[j].y, 22 * r->starscale * MAX(.5, pts[j].scale), .6);
        a = r->clusteralpha * relaxdepthlight((pts[j].z + pts[j + 1].z) * .5) * relaxnearfade(pts[j].z);
        width = MAX(.6, .8 * (pts[j].scale + pts[j + 1].scale) * .5);
        wave = 0;
        if (r->rippleamp > .003) {
            rr = (r->clusterr[lane][j] + r->clusterr[lane][j + 1]) * .5;
            wave = r->rippleamp * exp(-pow((rr - r->ripple) / (.03 * r->w), 2));
        }
        relaxband(&pts[j], &pts[j + 1], a * (1 + 5 * wave), width * (.6 + .5 * wave));
        if (wave > .05)
            relaxband(&pts[j], &pts[j + 1], .12 * wave * relaxdepthlight(pts[j].z), width * 3.5);
    }
}

/* 长曝光星轨: 从核心往回渐隐, 线宽从核心处向尾端变细. index = 核心 * 3 + 按深度划分的段 */
static void
relaxrenderstreak(int index)
{
    RelaxScene *r = &relaxscene;
    RelaxGalaxy *g = &r->galaxies[index / 3];
    RelaxProj *pts = r->streakpts + index / 3 * (RELAXSTREAK + 1);
    int j0 = index % 3 * RELAXSTREAK / 3, j1 = (index % 3 + 1) * RELAXSTREAK / 3, j;
    double base = r->streakalpha * MIN(1, g->alpha) * (1 + .6 * g->peri + .5 * g->flare + .8 * g->ripple) * (1 + .4 * g->hover);
    double u, a, width;

    for (j = j0; j < j1; j++) {
        if (!pts[j].ok || !pts[j + 1].ok || j >= g->streakreveal * RELAXSTREAK)
            continue;
        u = (j + .5) / RELAXSTREAK;
        a = base * pow(1 - u, 1.6) * relaxdepthlight((pts[j].z + pts[j + 1].z) * .5) * relaxnearfade(pts[j].z);
        width = MAX(.6, (pts[j].scale + pts[j + 1].scale) * .5 * (3 - 2.3 * u));
        relaxband(&pts[j], &pts[j + 1], a, width);
        if (u < .5)
            relaxband(&pts[j], &pts[j + 1], a * .35 * (1 - u / .5) * (1 - relaxclamp(r->qualityvisual - 2)), width * 3.5);
    }
}

/* 中心光源: 三条椭圆的共同焦点, 发出涟漪时脉冲一次 */
static void
relaxrendersun(void)
{
    RelaxScene *r = &relaxscene;
    double a = r->sunalpha * (1 + 1.2 * r->sunpulse);

    relaxrenderglow(RelaxCool, r->sunp, 20 * r->starscale * (1 + .5 * r->sunpulse), MIN(1, .7 * a),
            relaxdepthblur(r->sunp.z), 1.1, .5 * r->glowscale * (1 + r->sunpulse));
}

static void
relaxrenderstar(RelaxStar *s)
{
    RelaxScene *r = &relaxscene;
    double blur = relaxdepthblur(s->p.z), rad, a;

    /* 截图面板背后的柔光, 让面板像发光体而不是贴图 */
    if (s->vis > .02 && s->glow > .01)
        relaxsprite(RelaxHalo, RelaxCool, s->p.x, s->p.y, .62 * hypot(s->w, s->h) * s->size * s->p.scale,
                .16 * r->glowscale * s->glow * MIN(1, s->vis * 1.3)
                * (r->mode == RelaxOrbit ? 1 - relaxclamp(r->qualityvisual - 1) : 1));
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
    double a, rad;
    int i;

    for (i = 0; i < r->nstars; i++)
        r->stars[i].hit = 0;
    for (i = 0; i < r->ntags; i++)
        r->galaxies[i].hit = 0;
    for (i = 0; i < r->nitems; i++) {
        if (r->items[i].kind == RelaxStarItem && r->bandn) {
            /* 只有卡片会遮挡: 卡片之前若有与它相交的光带, 先合成 (光点 / 光晕之间的先后无关紧要) */
            RelaxStar *st = &r->stars[r->items[i].index];
            double ext = .75 * MAX(st->w, st->h) * st->size * st->p.scale + 2;
            if (st->vis > .02 && relaxbandpending(st->p.x - ext, st->p.y - ext, st->p.x + ext, st->p.y + ext))
                relaxflushbands();
        }
        switch (r->items[i].kind) {
        case RelaxDustItem:
            d = &r->dust[r->items[i].index];
            a = d->light * r->dustfade * relaxsmoothstep(relaxphase(d->p.z, r->cam.near * 1.5, r->cam.near * 3));
            if (r->mode == RelaxOrbit) {
                if (r->items[i].index % 2)
                    a *= 1 - relaxclamp(r->qualityvisual);
                if (r->items[i].index % 3)
                    a *= 1 - relaxclamp(r->qualityvisual - 1);
            }
            rad = MAX(.6, d->size * d->p.scale);
            if (r->mode == RelaxOrbit && !d->disk && d->size >= 8) {
                /* 闪烁星空: 各自频率明暗起伏, 每颗约 20s 一次短暂闪亮 */
                double tt = r->motion * r->tscale, sp = fmod(tt / 20 + d->tw * 7, 1);
                a *= .55 + .7 * (.5 + .5 * sin(tt * (1.3 + 2.1 * d->tw) + d->tw * 40));
                if (sp < .04) {
                    a *= 1 + 2.5 * sin(RELAXPI * sp / .04);
                    rad *= 1 + .6 * sin(RELAXPI * sp / .04);
                }
            }
            a *= 1 + 2 * d->boost;
            if (a < .003)
                break;
            relaxsprite(RelaxHalo, RelaxCool, d->p.x, d->p.y, rad, MIN(1, a));
            if (r->warpfx > .01) {
                /* 超空间跃迁: 尘埃沿屏幕中心向外拉成光线 */
                RelaxProj q = d->p;
                q.x += (d->p.x - r->w * .5) * .25 * r->warpfx;
                q.y += (d->p.y - r->h * .5) * .25 * r->warpfx;
                relaxband(&d->p, &q, MIN(.25, a * 1.2 * r->warpfx), MAX(.5, .45 * d->size * d->p.scale));
            }
            break;
        case RelaxCoreItem:
            g = &r->galaxies[r->items[i].index];
            /* 近点 / 交会 / 涟漪 / 翻转时核心更亮, 光晕更大 */
            a = .35 * g->peri + .6 * g->flare + .7 * g->ripple + .25 * g->flip + 1.5 * g->ignite + 1.2 * g->callout
                + 3 * g->nova + 1.2 * g->bridge;
            relaxrenderglow(RelaxWarm, g->p, g->size * (1 + .15 * g->hover + .12 * g->peri + .2 * g->flare + .12 * g->ripple
                        + .4 * (g->ignite + g->callout) + .6 * g->nova + .2 * g->bridge),
                    MIN(1, g->alpha * relaxdepthlight(g->p.z) * (1 + a)) * relaxnearfade(g->p.z),
                    relaxdepthblur(g->p.z), .55 * (1 + .4 * g->hover + a), .22 * r->glowscale * (1 + 1.5 * a)
                    * (r->mode == RelaxOrbit ? 1 - relaxclamp(r->qualityvisual - 2) : 1));
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
        case RelaxClusterItem:
            relaxrendercluster(r->items[i].index);
            break;
        case RelaxStreakItem:
            relaxrenderstreak(r->items[i].index);
            break;
        case RelaxSunItem:
            relaxrendersun();
            break;
        }
    }
    relaxflushbands();
}

/* 冲击环: center 处 plane 的 xy 平面内的圆 (3D, 随透视变成椭圆) */
static void
relaxringfx(RelaxVec center, RelaxMat plane, double rad, double a, double width)
{
    RelaxProj pts[49];
    double th;
    int j;

    if (a < .004 || rad < 1)
        return;
    for (j = 0; j <= 48; j++) {
        th = 2 * RELAXPI * j / 48;
        pts[j] = relaxproject(relaxadd(center, relaxapply(plane, relaxv(cos(th) * rad, sin(th) * rad, 0))));
    }
    for (j = 0; j < 48; j++)
        if (pts[j].ok && pts[j + 1].ok) {
            relaxband(&pts[j], &pts[j + 1], a * relaxnearfade(pts[j].z), MAX(.8, width * pts[j].scale));
            relaxband(&pts[j], &pts[j + 1], a * .3 * relaxnearfade(pts[j].z), MAX(2, 4 * width * pts[j].scale));
        }
}

/* 开场的一次性光效: 起飞冲击波 / 核心点火冲击环 / 中心光源点火 / 转速峰值的盘面冲击环 */
static void
relaxrenderfx(void)
{
    RelaxScene *r = &relaxscene;
    RelaxGalaxy *g;
    RelaxProj pts[65], c = {0};
    RelaxMat disk;
    double f = relaxbeatw(), s = r->iclock, u, t;
    int i, j;

    if (f < .01)
        return;
    /* 起飞: 以焦点窗口为圆心, 屏幕空间的一圈光环 */
    u = relaxphase(s, .08, .7);
    if (u > 0 && u < 1) {
        c.x = r->w * .5;
        c.y = r->h * .5;
        for (i = 0; i < r->nstars; i++)
            if (r->stars[i].focused && r->stars[i].p.ok)
                c = r->stars[i].p;
        for (j = 0; j <= 64; j++) {
            t = 2 * RELAXPI * j / 64;
            pts[j] = (RelaxProj){c.x + cos(t) * .7 * r->w * relaxeaseoutcubic(u), c.y + sin(t) * .7 * r->w * relaxeaseoutcubic(u), 1, 1, 1};
        }
        for (j = 0; j < 64; j++) {
            relaxband(&pts[j], &pts[j + 1], f * .22 * pow(1 - u, 1.5), 1.5 + 2 * u);
            relaxband(&pts[j], &pts[j + 1], f * .07 * pow(1 - u, 1.5), 8 + 10 * u);
        }
    }
    /* 核心点火: 在各自轨道平面里扩散的冲击环 */
    for (i = 0; i < r->ntags; i++) {
        g = &r->galaxies[i];
        t = .62 + .06 * g->rank;
        u = relaxphase(s, t, t + .5);
        if (u > 0 && u < 1 && g->p.ok)
            relaxringfx(g->pos, g->plane, 2.4 * g->radius * relaxeaseoutcubic(u), f * .24 * pow(1 - u, 1.3), 1.4);
    }
    /* 中心光源点火, 以及转速峰值时: 盘面 (xz) 上的大冲击环 */
    disk = relaxmul(r->world, relaxrotx(RELAXPI / 2));
    u = relaxphase(s, 1.5, 2.1);
    if (u > 0 && u < 1)
        relaxringfx(relaxv(0, 0, 0), disk, .45 * r->w * relaxeaseoutcubic(u), f * .22 * (1 - u), 1.6);
    u = relaxphase(s, 2.85, 3.5);
    if (u > 0 && u < 1)
        relaxringfx(relaxv(0, 0, 0), disk, .7 * r->w * relaxeaseoutcubic(u), f * .26 * (1 - u), 2.2);
    relaxflushbands();
}

/* 屏幕空间光带的一个点 */
static RelaxProj
relaxsp(double x, double y)
{
    return (RelaxProj){x, y, 1, 1, 1};
}

/* 驻留特效: 轨道光流 / 星座连线 / 核心光桥 / 超新星冲击环 / 彗星 / 流星. 都是 motion 的函数, 强度乘 holdw */
static void
relaxrenderholdfx(void)
{
    RelaxScene *r = &relaxscene;
    RelaxGalaxy *g, *h;
    RelaxStar *st;
    RelaxProj pts[13], c, hp;
    RelaxMat m, disk;
    RelaxVec v, head, tail, A, B, perp;
    double f = r->holdw, t = r->motion * r->tscale, local, env, u, a, th, rad, ang, len, wave, phi, R;
    int i, j, k, l, n, order[32];

    if (f < .01 || (r->mode != RelaxOrbit && r->mode != RelaxCollapse))
        return;
    /* 轨道光流: 每条群椭圆 6 个光点, 每条局部环 2 个, 沿轨道流动, 带短尾 */
    for (l = 0; l < RELAXLANES; l++) {
        if (!(r->lanemask & 1 << l))
            continue;
        for (i = 0; i < 6; i++) {
            for (j = 0; j <= 4; j++) {
                th = 2 * RELAXPI * i / 6 + relaxlanes[l].dir * (.3 * t - .045 * j);
                v = relaxlanepoint(l, th);
                pts[j] = relaxproject(relaxapply(r->world, v));
            }
            wave = r->rippleamp * exp(-pow((relaxlen(v) - r->ripple) / (.04 * r->w), 2));
            for (j = 0; j < 4; j++)
                if (pts[j].ok && pts[j + 1].ok)
                    relaxband(&pts[j], &pts[j + 1], f * .2 * (1 + 2 * wave) * (1 - j / 4.0) * relaxnearfade(pts[j].z),
                            MAX(.6, 1.6 * pts[j].scale));
            if (pts[0].ok)
                relaxsprite(RelaxHalo, RelaxCool, pts[0].x, pts[0].y, MAX(3, 10 * pts[0].scale * r->starscale),
                        f * .5 * (1 + wave) * relaxnearfade(pts[0].z));
        }
    }
    for (i = 0; i < r->ntags; i++) {
        g = &r->galaxies[i];
        if (!g->nrings || g->alpha < .1 || !g->p.ok)
            continue;
        for (k = 0; k < g->nrings; k++) {
            m = relaxmul(g->plane, g->ring[k]);
            rad = g->ringr[k] * relaxbreathe(g, r->motion);
            for (l = 0; l < 2; l++) {
                for (j = 0; j <= 3; j++) {
                    th = (1.1 + .2 * k) * (k == 1 ? -1 : 1) * (t - .1 * j) + RELAXPI * l + g->phase;
                    pts[j] = relaxproject(relaxadd(g->pos, relaxapply(m, relaxv(cos(th) * rad, sin(th) * rad, 0))));
                }
                for (j = 0; j < 3; j++)
                    if (pts[j].ok && pts[j + 1].ok)
                        relaxband(&pts[j], &pts[j + 1], f * .18 * g->alpha * (1 - j / 3.0) * relaxnearfade(pts[j].z),
                                MAX(.6, 1.3 * pts[j].scale));
                if (pts[0].ok)
                    relaxsprite(RelaxHalo, RelaxWarm, pts[0].x, pts[0].y, MAX(2.5, 7 * pts[0].scale * r->starscale),
                            f * .45 * g->alpha * relaxnearfade(pts[0].z));
            }
        }
    }
    /* 星座连线: 按轨道角把这个星系的窗口卡片连起来, 首颗再连到核心 */
    if (r->npop && relaxcycle(r->motion, 2, 4, &k, &local)) {
        g = &r->galaxies[r->popord[k % r->npop]];
        env = f * relaxsmoothstep(local / .4) * (1 - relaxsmoothstep((local - 1.6) / 1));
        for (i = n = 0; i < r->nstars && n < 32; i++)
            if (r->stars[i].galaxy == g->tag && r->stars[i].p.ok)
                order[n++] = i;
        for (i = 1; i < n; i++)     /* 按轨道角插入排序 (每个星系的星不多) */
            for (j = i; j > 0 && fmod(relaxorbitangle(&r->stars[order[j]], RELAXHOLD, r->motion) + 100 * RELAXPI, 2 * RELAXPI)
                    < fmod(relaxorbitangle(&r->stars[order[j - 1]], RELAXHOLD, r->motion) + 100 * RELAXPI, 2 * RELAXPI); j--) {
                l = order[j]; order[j] = order[j - 1]; order[j - 1] = l;
            }
        if (env > .01 && n) {
            for (i = 0; i < n; i++) {
                st = &r->stars[order[i]];
                hp = r->stars[order[(i + 1) % n]].p;
                if (n > 1 && (n > 2 || i == 0))
                    relaxband(&st->p, &hp, .2 * env, 1.1);
                relaxsprite(RelaxHalo, RelaxCool, st->p.x, st->p.y, 12, .6 * env);
            }
            if (g->p.ok)
                relaxband(&r->stars[order[0]].p, &g->p, .14 * env, .9);
        }
    }
    /* 核心光桥: 一道光线连起两个核心, 一个亮点沿光线跑过去 */
    if (r->bri >= 0 && r->brj >= 0 && relaxcycle(r->motion, 3, 7, &k, &local) && k == r->bridgek) {
        g = &r->galaxies[r->bri];
        h = &r->galaxies[r->brj];
        env = f * relaxsmoothstep(local / .25) * (1 - relaxsmoothstep((local - 1.3) / .6));
        if (env > .01 && g->p.ok && h->p.ok) {
            relaxband(&g->p, &h->p, .14 * env, 1.2);
            relaxband(&g->p, &h->p, .05 * env, 5);
            u = relaxeaseinoutcubic(relaxphase(local, .25, 1.15));
            if (u > 0 && u < 1)
                relaxsprite(RelaxHalo, RelaxWarm, relaxmix(g->p.x, h->p.x, u), relaxmix(g->p.y, h->p.y, u), 18, .9 * env);
        }
    }
    /* 超新星: 核心所在轨道平面和盘面上各一圈冲击环 */
    if (relaxcycle(r->motion, 9, 20, &k, &local) && local < 2) {
        g = &r->galaxies[(k * 5 + 3) % r->ntags];
        disk = relaxmul(r->world, relaxrotx(RELAXPI / 2));
        u = relaxphase(local, 0, 1.6);
        if (u < 1 && g->p.ok)
            relaxringfx(g->pos, g->plane, 3 * MAX(g->radius, 60) * relaxeaseoutcubic(u), f * .26 * (1 - u), 1.8);
        u = relaxphase(local, .25, 2);
        if (u > 0 && u < 1 && g->p.ok)
            relaxringfx(g->pos, disk, .3 * r->w * relaxeaseoutcubic(u), f * .18 * (1 - u), 1.3);
    }
    /* 彗星: 每 15s 一颗, 从星系群外侧穿过盘面, 尾巴背向中心光源 */
    if (relaxcycle(r->motion, 6, 15, &k, &local) && local < 5) {
        u = local / 5;
        phi = 2 * RELAXPI * relaxhash(k * 3 + 5);
        R = .75 * r->w;
        perp = relaxv(-sin(phi) * .18 * r->w, 0, cos(phi) * .18 * r->w);
        A = relaxadd(relaxv(R * cos(phi), -.12 * r->w, R * sin(phi)), perp);
        B = relaxadd(relaxv(-.9 * R * cos(phi), .08 * r->w, -.9 * R * sin(phi)), perp);
        head = relaxapply(r->world, relaxlerp(A, B, u));
        tail = relaxscale(relaxnormalize(head), .2 * r->w);
        env = f * relaxsmoothstep(u / .1) * (1 - relaxsmoothstep((u - .85) / .15));
        for (j = 0; j <= 10; j++)
            pts[j] = relaxproject(relaxadd(head, relaxscale(tail, j / 10.0)));
        for (j = 0; j < 10; j++)
            if (pts[j].ok && pts[j + 1].ok) {
                a = env * pow(1 - j / 10.0, 1.3) * relaxnearfade(pts[j].z);
                relaxband(&pts[j], &pts[j + 1], .24 * a, MAX(.6, (2.6 - 2.2 * j / 10.0) * pts[j].scale));
                relaxband(&pts[j], &pts[j + 1], .07 * a, MAX(2, 7 * pts[j].scale));
            }
        if (pts[0].ok && env * relaxnearfade(pts[0].z) > .01)
            relaxrenderglow(RelaxCool, pts[0], 14, MIN(1, .9 * env) * relaxnearfade(pts[0].z), relaxdepthblur(pts[0].z), 1, .35);
    }
    /* 流星: 每 2.6s 一颗, 大致沿轨道盘面的对角线方向 (右上 -> 左下) 划过画面 */
    if (relaxcycle(r->motion, .8, 2.6, &k, &local) && local < 1.1) {
        u = local / 1.1;
        ang = (180 - RELAXDIAG + (relaxhash(k * 7 + 11) - .5) * 30) * RELAXPI / 180;
        len = r->w * (.5 + .3 * relaxhash(k * 7 + 13));
        /* 起点在画面上边或右边之外, 流星从边缘飞入, 不会在画面中间凭空出现 */
        c = relaxhash(k * 7 + 12) < .55
            ? relaxsp(r->w * (.3 + .7 * relaxhash(k * 7 + 14)), -.06 * r->h)
            : relaxsp(1.04 * r->w, r->h * (.05 + .45 * relaxhash(k * 7 + 14)));
        env = f * relaxsmoothstep(u / .2) * (1 - relaxsmoothstep((u - .75) / .25));
        hp = relaxsp(c.x + cos(ang) * len * relaxeaseoutcubic(u), c.y + sin(ang) * len * relaxeaseoutcubic(u));
        for (j = 0; j <= 12; j++)
            pts[j] = relaxsp(hp.x - cos(ang) * .22 * r->w * j / 12 * MIN(1, u * 3), hp.y - sin(ang) * .22 * r->w * j / 12 * MIN(1, u * 3));
        for (j = 0; j < 12; j++)
            relaxband(&pts[j], &pts[j + 1], .25 * env * pow(1 - j / 12.0, 1.5), 1.7 - 1.3 * j / 12);
        relaxsprite(RelaxHalo, RelaxCool, hp.x, hp.y, 12, .8 * env);
    }
    relaxflushbands();
}

static void
relaxrendertitle(void)
{
    RelaxScene *r = &relaxscene;
    RelaxStar *s;
    XGlyphInfo ext;
    XRenderColor shade = {0x0700, 0x0b00, 0x1700, 0xd000};
    int x, y, w, h, len;

    if (r->mode != RelaxOrbit || r->hover < 0 || r->hover >= r->nstars || !r->titledraw || !r->titlefont)
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
    if (r->mode == RelaxOrbit && r->qualityvisual >= 1.99)
        n = MAX(4, n / 2);
    /* 尾迹覆盖的总时长: 高速旋转时 0.12s (短促流光), 驻留时 1s (沿轨道的彗星弧) */
    dt = relaxmix(.12, 1, relaxsmoothstep(relaxphase(r->stage, 3.3, 4.2))) / r->ntrail;
    rate = r->mode == RelaxOrbit ? 0 : 1;   /* 驻留时 stage 停住, 只有 motion 在走 */
    for (k = 0; k <= n; k++) {
        if (k == 0) {
            for (i = 0; i < r->ntags; i++) {
                r->tgpos[i] = r->galaxies[i].pos;
                r->tgplane[i] = r->galaxies[i].plane;
                r->tpts[(r->nstars + i) * stride] = r->stage >= 2.8 ? r->galaxies[i].p : (RelaxProj){0};
            }
            for (i = 0; i < r->nstars; i++)
                r->tpts[i * stride] = r->stars[i].p;
            continue;
        }
        tk = r->mode == RelaxIntro ? relaxintrostage(MAX(0, MIN(r->scene, RELAXIEND) - k * dt)) : r->stage - k * dt * rate;
        mk = r->motion - k * dt;
        world = r->mode == RelaxCollapse ? r->cworld : relaxworldat(tk, mk);
        for (i = 0; i < r->ntags; i++) {
            relaxgalaxyat(&r->galaxies[i], tk, mk, world, &r->tgpos[i], &r->tgplane[i]);
            r->tpts[(r->nstars + i) * stride + k] = r->stage >= 2.8 ? relaxproject(r->tgpos[i]) : (RelaxProj){0};
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
            a = .16 * gain * r->galaxies[i - r->nstars].alpha * (1 - r->streakalpha / .24);  /* 驻留时由长曝光星轨取代 */
        }
        for (k = 0; k < n; k++) {
            pa = &r->tpts[i * stride + k];
            pb = &r->tpts[i * stride + k + 1];
            if (!pa->ok || !pb->ok)
                break;
            relaxband(pa, pb, a * (1 - (double)k / r->ntrail)
                    * (r->mode == RelaxOrbit && k >= r->ntrail / 2 ? 1 - relaxclamp(r->qualityvisual - 1) : 1),
                    MAX(.45, .9 * pa->scale * r->starscale));
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

static double relaxnow(void);

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
    RelaxScene *r = &relaxscene;
    double t = relaxnow(), next;
    relaxrenderbackground();
    next = relaxnow(); r->phasecost[1] += next - t; t = next;
    relaxrendertrails();
    next = relaxnow(); r->phasecost[2] += next - t; t = next;
    relaxrenderitems();
    relaxrenderholdfx();
    relaxrenderfx();
    relaxrendertitle();
    relaxrendercentral();
    relaxrenderfront();
    next = relaxnow(); r->phasecost[3] += next - t; t = next;
    relaxpresent();
    next = relaxnow(); r->phasecost[4] += next - t; t = next;
    /* 等服务器画完这一帧: 既是帧时间的真实测量, 也避免请求堆积 */
    XSync(dpy, False);
    r->phasecost[5] += relaxnow() - t;
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
    if (r->titledraw)
        XftDrawDestroy(r->titledraw);
    if (r->titlecolorok)
        XftColorFree(dpy, DefaultVisual(dpy, screen), DefaultColormap(dpy, screen), &r->titlecolor);
    if (r->titlefont)
        XftFontClose(dpy, r->titlefont);
    r->titledraw = NULL;
    r->titlefont = NULL;
    r->titlecolorok = 0;
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
/* 群轨道分配: 有窗口的 tag 先放内 / 中轨 (当前 tag 在内轨上靠镜头的一侧), 空 tag 放外轨; 同轨按平近点角均分 */
static void
relaxbuildlanes(int cur, int npop)
{
    RelaxScene *r = &relaxscene;
    RelaxGalaxy *g;
    RelaxMat world;
    RelaxVec v;
    static const double offset[RELAXLANES] = { 0, 2.2, 4.1 };
    int order[32], count[RELAXLANES] = {0}, slot[32], n = 0, nmain = 0, lanes, i, j, k, best = 0;
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
    lanes = MAX(1, MIN(RELAXLANES, nmain));
    r->npop = 0;
    r->lanemask = 0;
    for (j = 0; j < n; j++) {
        g = &r->galaxies[order[j]];
        g->lane = j < nmain ? j % lanes : RELAXLANES - 1 - (j - nmain) % 2;
        g->rank = j;
        slot[j] = count[g->lane]++;
        r->lanemask |= 1 << g->lane;
        if (g->nstars)
            r->popord[r->npop++] = order[j];
    }
    for (j = 0; j < n; j++) {
        g = &r->galaxies[order[j]];
        g->orbitphase = 2 * RELAXPI * slot[j] / count[g->lane] + offset[g->lane];
    }
    if (cur < 0 || n < 1)
        return;
    /* 转动内轨的整体相位, 让当前 tag 在进入驻留时位于靠镜头的一侧 */
    world = relaxworldat(RELAXHOLD, RELAXIEND + .5);
    g = &r->galaxies[cur];
    for (k = 0; k < 72; k++) {
        d = 2 * RELAXPI * k / 72;
        v = relaxapply(world, relaxlanepoint(g->lane, relaxkepler(relaxanomaly(g, RELAXIEND + .5, 0) + d,
                        relaxlanes[g->lane].e)));
        score = v.z + .5 * fabs(v.x);
        if (score < bestscore) {
            bestscore = score;
            best = k;
        }
    }
    for (j = 0; j < n; j++)
        if (r->galaxies[order[j]].lane == g->lane)
            r->galaxies[order[j]].orbitphase += 2 * RELAXPI * best / 72;
}

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
    relaxbuildlanes(cur, npop);
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

/* 三层空间: 远景星空 (整个球壳) / 盘面尘带 (与群轨道同一平面, 随星系群转动) / 前景浮尘 (镜头与星系群之间) */
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
        d->disk = 0;
        d->tw = relaxhash(i * 5 + 1005);
        d->boost = 0;
        if (u < .45) {          /* 远景星空 */
            rad = F * (3.5 + 2.5 * relaxhash(i * 5 + 1003));
            d->pos = relaxv(rad * sin(ph) * cos(th), rad * cos(ph) * .7, rad * sin(ph) * sin(th));
            d->size = 8 + 6 * relaxhash(i * 5 + 1004);
            d->light = .35 + .25 * relaxhash(i * 5 + 1004);
        } else if (u < .8) {    /* 盘面尘带: 半径 .12–.7 屏宽, 薄薄一层 */
            rad = r->w * (.12 + .58 * sqrt(relaxhash(i * 5 + 1003)));
            d->pos = relaxv(rad * cos(th), (v - .5) * .035 * r->w, rad * sin(th));
            d->disk = 1;
            d->size = 4.5 + 4 * relaxhash(i * 5 + 1004);
            d->light = .45 + .35 * relaxhash(i * 5 + 1004);
        } else {                /* 前景浮尘: 视差最大 */
            d->pos = relaxv((relaxhash(i * 5 + 1003) - .5) * 2.4 * F, (v - .5) * 1.4 * F,
                    -F * (.35 + .45 * relaxhash(i * 5 + 1002)));
            d->size = 1.8 + 1.2 * relaxhash(i * 5 + 1004);
            d->light = .55 + .25 * relaxhash(i * 5 + 1004);
        }
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
        fprintf(r->log, "galaxy %s: at %.2fs frames %lu time %.3fs avg %.1f fps min %.1f fps 1%%low %.1f fps render avg %.2fms max %.2fms update %.2fms background %.2fms trails %.2fms items %.2fms present %.2fms XSync %.2fms bands %.2f+%.2fms (%.0f tiles in %.1f flushes) quality %d%s xerrors %lu\n",
                how, now, r->frames, span, r->frames / MAX(span, 1e-3),
                r->ngaps ? 1 / r->gaps[r->ngaps - 1] : 0, low > 0 ? 1 / low : 0,
                r->rendersum / r->frames * 1000, r->rendermax * 1000,
                r->phasecost[0] / r->frames * 1000, r->phasecost[1] / r->frames * 1000,
                r->phasecost[2] / r->frames * 1000, r->phasecost[3] / r->frames * 1000,
                r->phasecost[4] / r->frames * 1000, r->phasecost[5] / r->frames * 1000,
                r->bandraster / r->frames * 1000, r->bandflush / r->frames * 1000,
                (double)r->bandtiles / r->frames, (double)r->bandflushes / r->frames, r->quality,
                r->mode == RelaxOrbit && now - r->lastinput > RELAXIDLE ? " (idle)" : "", r->errors);
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
    if (r->log && r->divenum)
        fprintf(r->log, "galaxy dive: frames %d render avg %.2fms max %.2fms\n",
                r->divenum, 1000 * r->divesum / r->divenum, 1000 * r->divemax);
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
    r->cstage = r->stage;
    r->cworld = r->world;
    r->exitspin = 0;
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
    r->rkind = star >= 0 ? RelaxPickStar : tag >= 0 ? RelaxPickCore : RelaxFlyHome;
    r->rstar = star;
    r->rcore = tag;
    if (r->rkind == RelaxPickCore && tag >= 0 && tag < r->ntags)
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
    r->rcx = r->cam.rx * 180 / RELAXPI;
    r->rcy = r->cam.ry * 180 / RELAXPI;
    r->rcz = r->cam.rz * 180 / RELAXPI;
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
            r->scene = r->wscene + (RELAXIEND - r->wscene) * relaxeaseinoutcubic(u);
            r->beatfade = 1 - relaxsmoothstep(u * 2);   /* 快进跳过俯冲等节拍, 不在 0.6s 内闪一遍 */
            if (u >= 1)
                r->scene = RELAXIEND;
        } else {
            r->scene += dt / r->tscale;
        }
        r->stage = relaxintrostage(MIN(r->scene, RELAXIEND));
        r->iclock = r->scene;
        r->motion = r->scene;
        break;
    case RelaxOrbit:
        /* 交互让位: 镜头停住的同时轨道运动放慢到 30%, 窗口星容易点中 */
        r->scene += dt / r->tscale * relaxmix(.3, 1, r->dspeed);
        r->stage = RELAXHOLD;
        r->motion = r->scene;
        break;
    case RelaxCollapse:
        r->scene += dt / r->tscale;
        u = now - r->cstart;
        r->celapsed = u;
        if (u < RELAXPREP)
            r->stage = relaxmix(r->cstage, RELAXHOLD, relaxeaseinoutcubic(u / RELAXPREP));
        else if (u < RELAXEXITSTART)
            r->stage = RELAXHOLD;
        else
            r->stage = RELAXEXIT + (RELAXEND - RELAXEXIT) * relaxphase(u, RELAXEXITSTART, RELAXCOLLAPSE);
        r->motion = r->scene;
        r->exitspin = relaxexitangle(u);
        r->beatfade = MIN(r->beatfade, 1 - relaxsmoothstep(u));   /* 开场节拍 (iclock 已冻结) 用 1s 平滑淡出 */
        if (u >= RELAXCOLLAPSE) {
            relaxfinish();
            return;
        }
        break;
    case RelaxReturn:
        u = (now - r->rstart) / (r->rkind == RelaxPickStar ? RELAXPICK
                : r->rkind == RelaxPickCore ? RELAXCORE : RELAXRETURN);
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
    if (r->mode == RelaxReturn) {
        if (r->rkind == RelaxFlyHome)
            relaxupdatereturn(u);
        else
            relaxupdatepick(u);
    } else
        relaxupdatescene(r->stage, r->motion, dt);
    r->phasecost[0] += relaxnow() - begin;
    relaxrender();
    cost = relaxnow() - begin;
    if (r->mode == RelaxOrbit && now - r->lastinput < RELAXIDLE) {
        r->qualityavg = r->qualityavg ? relaxmix(r->qualityavg, cost, .045) : cost;
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
    r->qualityvisual = relaxfollow(r->qualityvisual, r->quality, dt, .35);
    if (r->mode == RelaxIntro && r->iclock >= 3.2 && r->iclock < 5) {
        r->divesum += cost;
        r->divemax = MAX(r->divemax, cost);
        r->divenum++;
    }
    if (r->mode == RelaxIntro && r->nfirst < (int)LENGTH(r->firstcost)) {
        r->firstgap[r->nfirst] = now;   /* 相对按下 Super+Z 后时钟起点的时刻 */
        r->firstcost[r->nfirst++] = cost;
    }
    r->rendersum += cost;
    r->rendermax = MAX(r->rendermax, cost);
    r->frames++;
    if (r->mode == RelaxIntro && r->scene >= RELAXIEND)
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
                relaxcollapsestart();
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
            /* 默认就是最近 (zoom 1), 向上滚到底不再变化, 向下滚逐级拉远 */
            if (e->xbutton.button == Button4) {
                r->tzoom = MAX(1, r->tzoom * .9);
                r->lastpointer = relaxnow();
            } else if (e->xbutton.button == Button5) {
                r->tzoom = MIN(1.8, r->tzoom / .9);
                r->lastpointer = relaxnow();
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
        r->lastinput = r->lastpointer = relaxnow();
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

/* 比较一次仿射和一次透视, 决定大卡片旋转时走哪条路径. 在遮罩映射前做, 用户看不到. */
static void
relaxprobeprojective(void)
{
    RelaxScene *r = &relaxscene;
    RelaxStar *s, *best = NULL;
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
    relaxaffine(best->mip[lvl], (double)best->mipw[lvl] / w, (double)best->miph[lvl] / h, 0, 0);
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
    if (!relaxhomography(best->mip[lvl], q, w, h, best->mipw[lvl], best->miph[lvl]))
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
        r->spinpic = relaxargb(r->spinw, r->spinh, &r->spinpix);
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
        relaxaffine(s->mip[s->base], 1, 1, 0, 0);
        XRenderSetPictureFilter(dpy, s->mip[s->base], FilterNearest, NULL, 0);
        XRenderComposite(dpy, PictOpSrc, s->mip[s->base], None, r->back, 0, 0, 0, 0, 0, 0, w, h);
        XRenderSetPictureFilter(dpy, s->mip[s->base], FilterBilinear, NULL, 0);
    }
    XSync(dpy, False);
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
    r->dfit = r->dspeed = r->beatfade = 1;
    r->dshot = r->lastflip = r->lastripple = r->lastnova = r->lastcomet = -1;
    r->dtg[0] = r->dtg[1] = -1;
    r->bridgek = r->bri = r->brj = -1;
    r->lastpointer = -1e9;
    r->bglast[0] = -1;
    XQueryExtension(dpy, "RENDER", &r->rendermajor, &ev, &er);
    r->dpms = DPMSQueryExtension(dpy, &ev, &er) && DPMSCapable(dpy);
    XSetErrorHandler(relaxxerror);
    r->argb = XRenderFindStandardFormat(dpy, PictStandardARGB32);
    r->a8 = XRenderFindStandardFormat(dpy, PictStandardA8);
    r->a1 = XRenderFindStandardFormat(dpy, PictStandardA1);
    for (m = mons; m; m = m->next)
        for (c = m->clients; c; c = c->next)
            if (!c->isscratchpad)
                count++;
    r->nstars = count;
    r->tscale = count ? RELAXINTRO : 1;   /* 没有窗口: 同样的视觉语言, 更短的开场 */
    r->starscale = MAX(.65, MIN(1.1, 1.15 - .012 * count));
    r->orbitscale = MAX(.7, MIN(1, 1.05 - .008 * count));
    r->glowscale = MAX(.55, MIN(1, 1.1 - .015 * count));
    r->ndust = MAX(70, MIN(140, 140 - 2 * count));
    r->ntrail = count > 24 ? 6 : RELAXTRAIL;
    r->cam.fov = 62 * RELAXPI / 180;
    r->cam.focal = r->w * .5 / tan(r->cam.fov / 2);
    r->cam.near = r->cam.focal * .12;
    r->cam.far = r->cam.focal * 14;
    r->galaxies = calloc(r->ntags, sizeof *r->galaxies);
    r->stars = calloc(MAX(1, count), sizeof *r->stars);
    r->dust = calloc(r->ndust, sizeof *r->dust);
    r->items = calloc(r->ndust + r->ntags * (4 + RELAXARCS * RELAXRINGS) + count + RELAXARCS * RELAXLANES + 2, sizeof *r->items);
    r->tgpos = calloc(r->ntags, sizeof *r->tgpos);
    r->tgplane = calloc(r->ntags, sizeof *r->tgplane);
    r->tpts = calloc((count + r->ntags) * (r->ntrail + 1), sizeof *r->tpts);
    r->rpts = calloc(r->ntags * RELAXRINGS * (RELAXSEG + 1), sizeof *r->rpts);
    r->streakpts = calloc(r->ntags * (RELAXSTREAK + 1), sizeof *r->streakpts);
    r->streakz = calloc(r->ntags * 3, sizeof *r->streakz);
    r->popord = calloc(r->ntags, sizeof *r->popord);
    r->gaps = calloc(RELAXGAPS, sizeof *r->gaps);
    r->bandtw = (r->w + RELAXBTILE - 1) / RELAXBTILE;
    r->bandth = (r->h + RELAXBTILE - 1) / RELAXBTILE;
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

    if (!(r->back = relaxopaque(r->w, r->h, &r->backpix)) || !(r->desktop = relaxopaque(r->w, r->h, &r->desktoppix))
            || !(r->wallpaper = relaxopaque(r->w, r->h, &r->wallpix)) || !(r->bg = relaxopaque(r->w, r->h, &r->bgpix)))
        goto fail;
    r->titlefont = XftFontOpenName(dpy, screen, "sans:size=11");
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
    for (i = 0; i < RELAXALPHAS; i++) {
        color.alpha = color.red = color.green = color.blue = (unsigned short)(65535L * i / (RELAXALPHAS - 1));
        r->white[i] = XRenderCreateSolidFill(dpy, &color);
        color.red = color.green = color.blue = 0;
        r->black[i] = XRenderCreateSolidFill(dpy, &color);
    }
    r->tilemaskpix = XCreatePixmap(dpy, root, r->w, r->h, 8);
    if (!r->tilemaskpix || !(r->tilemask = XRenderCreatePicture(dpy, r->tilemaskpix, r->a8, 0, NULL)))
        goto fail;
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
    relaxprobeprojective();
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
    r->mode = RelaxIntro;
    relaxend(1);
}
