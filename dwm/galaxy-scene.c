/* Super+Z 星系: 场景. world / 群轨道 / 动作 / 导演镜头 / 开场节拍 / 驻留特效状态 / 每帧 update.
 * 由 galaxy.c 按顺序 include, 与其余部分是同一个编译单元. */

/* ---------- 世界: 全局旋转 / 星系 / 星体 (都是 (stage, motion) 的纯函数, 尾迹直接回溯时间求值) ---------- */

static double
galaxyramp(double x, double r)
{
    return x <= 0 ? 0 : x < r ? x * x / (2 * r) : x - r / 2;
}

/* 星系群整体绕竖直轴的缓慢漂移: 开场 .09 rad/s, 进入驻留后 3s 内降到 .025 rad/s (导演镜头负责主要的视角变化) */
static double
galaxydrift(double motion)
{
    double x = galaxyclamp((motion - GALAXYIEND) / 3);

    return .09 * galaxyramp(motion - 1, .8)
        - .065 * (3 * (x * x * x - x * x * x * x / 2) + MAX(0, motion - GALAXYIEND - 3));
}

/* 高速旋转结束后, 整个轨道盘面在俯冲前立起再回落; 两端的一阶速度均为 0。 */
static double
galaxyintrotilt(double motion)
{
    return .96 * galaxybeatw()
        * galaxysmoothstep(galaxyphase(motion, 3.25, 3.58))
        * (1 - galaxysmoothstep(galaxyphase(motion, 3.65, 3.95)));
}

static GalaxyMat
galaxyworldat(double stage, double motion)
{
    double u = galaxyphase(stage, 2.2, 3.3);
    /* 2.2s 静止 -> 加速 -> 约 2.85s 峰值 (≈1.5 转/秒) -> 3.3s 减速到几乎停止, 之后保持缓慢漂移 */
    double theta = galaxydrift(motion) + 2 * GALAXYPI * galaxyeaseinoutcubic(pow(u, 1.25));
    double tilt = .1 * galaxysmoothstep(galaxyphase(stage, 1, 2.2)) + .2 * sin(GALAXYPI * u)
        - .1 * galaxysmoothstep(galaxyphase(stage, 4.8, 5.5)) + .06 * galaxyscene.holdw * sin(2 * GALAXYPI * motion / 40);
    return galaxymul(galaxyrotx(tilt + galaxyintrotilt(motion)),
            galaxymul(galaxyroty(theta), galaxyrotz(.05 * sin(GALAXYPI * u))));
}

static double
galaxyexitangle(double elapsed)
{
    /* 不满一圈的同向弧线, 以平缓的速度曲线进入收束. */
    return 1.15 * GALAXYPI * galaxysmoothstep(galaxyphase(elapsed, .15, GALAXYEXITSTART));
}

/* 开普勒方程 E - e sinE = M (Newton 迭代) */
static double
galaxykepler(double m, double e)
{
    double E = m + e * sin(m);
    int i;

    for (i = 0; i < 4; i++)
        E -= (E - e * sin(E) - m) / (1 - e * cos(E));
    return E;
}

/* 椭圆群轨道上偏近点角 E 处的点. 轨道在水平盘面 (xz) 内, 焦点在原点 (中心光源), 再按升交点 / 倾角 / 近点角摆放 */
static GalaxyVec
galaxylanepoint(int lane, double E)
{
    const GalaxyLane *l = &galaxylanes[lane];
    double a = l->a * galaxyscene.vw, b = a * sqrt(1 - l->e * l->e), d2r = GALAXYPI / 180;
    GalaxyMat m = galaxymul(galaxyroty(l->node * d2r), galaxymul(galaxyrotx(l->inc * d2r), galaxyroty(l->peri * d2r)));

    return galaxyapply(m, galaxyv(a * (cos(E) - l->e), 0, b * sin(E)));
}

/* 核心在 motion 时刻的平近点角: 匀速增长, 位置由开普勒方程给出, 近点快远点慢 */
static double
galaxyanomaly(GalaxyCore *g, double motion, double spin)
{
    const GalaxyLane *l = &galaxylanes[g->lane];

    return g->orbitphase + l->dir * (2 * GALAXYPI * galaxyscene.tscale / l->period * (motion - 2.8) + spin);
}

/* 驻留后的周期事件: 进入驻留 offset 真实秒后开始, 每 every 真实秒一次.
 * 返回 1 表示已开始, *k 是第几次, *local 是本次开始后的真实秒数 (motion 的纯函数) */
static int
galaxycycle(double motion, double offset, double every, int *k, double *local)
{
    double ts = galaxyscene.tscale, gap = galaxyscene.fxgap > 0 ? galaxyscene.fxgap : 1;
    double x = motion - GALAXYIEND - offset * gap / ts, e = every * gap / ts;

    if (x < 0)
        return 0;
    *k = (int)floor(x / e);
    *local = (x - *k * e) * ts;
    return 1;
}

/* 闪光包络: attack 秒内平滑亮起, 之后按 decay 指数衰减. 不能从 0 一帧跳到最亮, 否则画面会「跳一下」 */
static double
galaxyflash(double t, double attack, double decay)
{
    if (t <= 0)
        return 0;
    return t < attack ? galaxysmoothstep(t / attack) : exp(-decay * (t - attack));
}

/* 星系翻转: 驻留后每 5s 轮到一个有窗口的星系, 用 3.2s 绕自身轨道平面的 x 轴翻转一整圈 */
static double
galaxyflipat(GalaxyCore *g, double motion)
{
    GalaxyScene *r = &galaxyscene;
    double local;

    /* 翻转的星系在事件开始时选定 (galaxyupdateholdfx), 这里只按开始时刻算角度; 转完一圈 (2π) 与 0 等价 */
    if (r->flipg != g->tag || (local = (motion - r->flipstart) * r->tscale) <= 0)
        return 0;
    return 2 * GALAXYPI * galaxyeaseinoutcubic(local / 3.2);
}
/* 轨道呼吸: 局部轨道环缓慢胀缩, 相邻星系节奏错开. 环和环上的星体共用这个系数 */
static double
galaxybreathe(GalaxyCore *g, double motion)
{
    return 1 + .07 * galaxysmoothstep(galaxyphase(motion, GALAXYIEND, GALAXYIEND + 3))
        * sin(2 * GALAXYPI * motion * galaxyscene.tscale / 9 + g->phase);
}

/* 旋涡扭转: 高速旋转时内圈比外圈多转, 星系群和群轨道被拧成螺旋, 减速时解开 (stage 的纯函数) */
static GalaxyVec
galaxytwist(GalaxyVec v, double stage)
{
    double u = galaxyphase(stage, 2.2, 3.3), a;

    if (u <= 0 || u >= 1)
        return v;
    a = 1.8 * pow(sin(GALAXYPI * u), 2) * (1 - MIN(1, sqrt(v.x * v.x + v.z * v.z) / (.5 * galaxyscene.vw)));
    return galaxyapply(galaxyroty(a), v);
}

static void
galaxycoreat(GalaxyCore *g, double stage, double motion, GalaxyMat world, GalaxyVec *pos, GalaxyMat *plane)
{
    GalaxyScene *r = &galaxyscene;
    double emerge = galaxyeaseoutcubic(galaxyphase(stage, .6, 1));
    double expand = galaxyeaseinoutcubic(galaxyphase(stage, 1.5, 2.2));
    double ready = galaxyeaseinoutcubic(galaxyphase(stage, 1.65, 2.8));
    double spin = r->exitspin;
    double side = g->tag % 2 ? 1 : -1;
    double merge = galaxyeaseinoutcubic(galaxyphase(stage, 5 + .03 * (g->tag % 4), 5.45));
    GalaxyVec p, ctrl, orbit;

    p = galaxyv(g->home.x * galaxymix(.62, 1, expand), g->home.y * galaxymix(.62, 1, expand),
            g->home.z * galaxymix(.45, 1, expand) + (1 - emerge) * 1.2 * r->cam.focal);
    if (r->mode == GalaxyCollapse && motion < r->motion)
        spin = galaxyexitangle(r->celapsed - (r->motion - motion) * r->tscale);
    orbit = galaxylanepoint(g->lane, galaxykepler(galaxyanomaly(g, motion, spin), galaxylanes[g->lane].e));
    p = galaxylerp(p, orbit, ready);
    p = galaxyadd(p, galaxyv(0, (1 - ready) * .012 * r->vh * sin(.9 * motion + g->phase),
                (1 - ready) * .02 * r->cam.focal * sin(.6 * motion + 1.3 * g->phase)));
    if (merge > 0) {
        /* 星系核心沿各自方向的 3D 曲线汇聚到中心 */
        ctrl = galaxyadd(galaxyscale(galaxyapply(galaxyroty(1.1 * side), p), .8), galaxyv(0, -.2 * r->vh * side, 0));
        p = galaxybezier(p, ctrl, galaxyv(0, 0, 0), merge);
    }
    *pos = galaxyapply(world, galaxytwist(p, stage));
    *plane = galaxymul(world, galaxymul(galaxyeuler(g->rx, g->ry + g->precess * motion, g->rz),
                galaxyrotx(galaxyflipat(g, motion))));
}

/* 星体当前的轨道角 */
static double
galaxyorbitangle(GalaxyStar *s, double stage, double motion)
{
    double c = galaxyeaseincubic(galaxyphase(stage, 4.8 + s->delay * .5, 5.2 + s->delay * .5));
    return s->angle + s->speed * (motion - .6) + 2.4 * c * (s->speed < 0 ? -1 : 1);
}

/* ringplane: 星体所在轨道环的平面 (星系轨道平面 x 环自身的倾斜), 星体严格在画出的环上运行 */
static void
galaxystarat(GalaxyStar *s, double stage, double motion, GalaxyMat world, GalaxyVec gpos, GalaxyMat ringplane,
        GalaxyVec *pos, GalaxyMat *orient)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyCore *g = &r->galaxies[s->galaxy];
    double F = r->cam.focal, dir = s->speed < 0 ? -1 : 1;
    double c = galaxyeaseincubic(galaxyphase(stage, 4.8 + s->delay * .5, 5.2 + s->delay * .5));
    double rad = s->radius * (1 - c) * galaxybreathe(g, motion) * galaxyeaseoutcubic(g->fill), a = galaxyorbitangle(s, stage, motion);
    double u1 = s->current ? galaxyeaseinoutcubic(galaxyphase(stage, .1, .58)) : 1;
    double u2 = galaxyeaseinoutcubic(galaxyphase(stage, .6 + s->delay, 1.5));
    GalaxyVec orbit, start, ctrl, d;
    GalaxyMat od, oo;

    orbit = galaxyadd(gpos, galaxyapply(ringplane, galaxyv(cos(a) * rad, sin(a) * rad, 0)));
    start = s->current ? galaxylerp(s->home, s->detach, u1) : s->detach;
    od = galaxyeuler(s->drx * u1, s->dry * u1, s->drz * u1);
    if (u2 <= 0) {
        *pos = start;
        *orient = od;
        return;
    }
    d = galaxysub(orbit, s->detach);
    ctrl = galaxyadd(galaxyscale(galaxyadd(s->detach, orbit), .5), galaxyv(-d.y * .25 * dir, d.x * .25 * dir, -.35 * F));
    *pos = galaxybezier(s->detach, ctrl, orbit, u2);
    oo = galaxymul(world, galaxyeuler(.55 * g->rx + .25 * sin(a), .55 * (g->ry + g->precess * motion) + .3 * cos(a), .4 * g->rz));
    *orient = galaxyblend(od, oo, u2);
}

static void
galaxyupdatecores(double stage, double motion, double dt)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyCore *g, *h;
    GalaxyVec d;
    double appear = r->variant == GalaxyBang
        ? MAX(galaxyeaseoutcubic(galaxyphase(stage, .6, 1.1)), galaxybeatw() * galaxyeaseoutcubic(galaxyphase(r->iclock, .32, .6)))
        : galaxyeaseoutcubic(galaxyphase(stage, .6, 1.1));
    double absorb = galaxyeaseinoutcubic(galaxyphase(stage, 4.9, 5.3));
    double merge = galaxyeaseincubic(galaxyphase(stage, 5.05, 5.45));
    double th = .09 * r->vw, flare[32] = {0}, occult[32] = {0}, dist, f, E, dz, radius, strength;
    int i, j, back, front, pair = -1;

    for (i = 0; i < r->ntags; i++) {
        g = &r->galaxies[i];
        galaxycoreat(g, stage, motion, r->world, &g->pos, &g->plane);
        if (r->variant == GalaxyBang)   /* 开场 C: 核心从爆心飞到原定位置 */
            g->pos = galaxylerp(r->bang, g->pos, 1 - galaxybeatw() * (1 - galaxyeaseoutcubic(galaxyphase(r->iclock, .34, 1.2))));
        g->anomaly = galaxyanomaly(g, motion, r->exitspin);
        E = galaxykepler(g->anomaly, galaxylanes[g->lane].e);
        /* 近点甩掠: 经过近点时更亮更大 (速度变化由开普勒运动自带) */
        g->peri = r->holdw * galaxysmoothstep((cos(E) - .6) / .4);
        g->flip = sin(.5 * galaxyflipat(g, motion));
        g->ripple = .6 * r->rippleamp * exp(-pow((galaxylen(g->pos) - r->ripple) / (.09 * r->vw), 2));
        g->nudge = galaxyv(0, 0, 0);
        g->fill = galaxyfollow(g->fill, g->nstars > 0, dt, .6);
        g->alpha = appear * (1 - .9 * merge) * galaxymix(.55, 1, g->fill) * (1 + .35 * absorb * g->fill);
        g->hover = galaxyfollow(g->hover, r->hovercore == i, dt, .12);
    }
    /* 交会: 不同轨道上的两个核心在 3D 中靠近时同时闪光, 并沿连线互相吸引一点 */
    for (i = 0; i < r->ntags && i < 32; i++)
        for (j = i + 1; j < r->ntags && j < 32; j++) {
            g = &r->galaxies[i];
            h = &r->galaxies[j];
            if (g->lane == h->lane || g->alpha < .1 || h->alpha < .1)
                continue;
            d = galaxysub(h->pos, g->pos);
            dist = galaxylen(d);
            f = r->holdw * galaxyclamp(1 - dist / th);
            if (f <= 0)
                continue;
            d = galaxyscale(d, .012 * r->vw * f / MAX(dist, 1));
            g->nudge = galaxyadd(g->nudge, d);
            h->nudge = galaxysub(h->nudge, d);
            flare[i] = MAX(flare[i], f);
            flare[j] = MAX(flare[j], f);
        }
    for (i = 0; i < r->ntags; i++) {
        g = &r->galaxies[i];
        g->pos = galaxyadd(g->pos, g->nudge);
        g->flare = galaxyfollow(g->flare, i < 32 ? flare[i] : 0, dt, .2);
        g->p = galaxyproject(g->pos);
    }
    /* 交会掩食: 只在投影光晕重叠、世界位置接近且镜头深度明确分层时压低后方核心。
     * 核心本身仍按 camera-space z 排序, 这只改变现有光晕的强度。 */
    if (r->mode == GalaxyOrbit)
        for (i = 0; i < r->ntags && i < 32; i++)
            for (j = i + 1; j < r->ntags && j < 32; j++) {
                g = &r->galaxies[i];
                h = &r->galaxies[j];
                if (g->lane == h->lane || !g->p.ok || !h->p.ok || g->alpha < .2 || h->alpha < .2)
                    continue;
                dz = fabs(g->p.z - h->p.z);
                radius = 2.4 * (g->size * g->p.scale + h->size * h->p.scale);
                dist = hypot(g->p.x - h->p.x, g->p.y - h->p.y);
                if (radius <= 0 || dist >= radius || dz <= .025 * r->cam.focal)
                    continue;
                strength = r->holdw * galaxysmoothstep(1 - dist / radius)
                    * galaxysmoothstep((dz - .025 * r->cam.focal) / (.075 * r->cam.focal))
                    * galaxysmoothstep(1.8 - galaxylen(galaxysub(g->pos, h->pos)) / th);
                back = g->p.z > h->p.z ? i : j;
                front = back == i ? j : i;
                if (strength > occult[back]) {
                    occult[back] = strength;
                    if (strength > .25)
                        pair = back * 32 + front;
                }
            }
    for (i = 0; i < r->ntags; i++)
        r->galaxies[i].eclipse = galaxyfollow(r->galaxies[i].eclipse, i < 32 ? occult[i] : 0, dt, .12);
    if (pair != r->eclipsepair) {
        if (pair >= 0 && r->log) {
            fprintf(r->log, "galaxy eclipse: front tag %d behind tag %d at %.1fs\n", pair % 32 + 1, pair / 32 + 1, motion);
            fflush(r->log);
        }
        r->eclipsepair = pair;
    }
}

/* 轨道环拆为短弧, 用每段的镜头深度与窗口和核心一起排序. */
static void
galaxyupdaterings(double shrink)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyCore *g;
    GalaxyProj *pts;
    GalaxyMat m;
    double th, rad, z;
    int i, k, j, arc;

    for (i = 0; i < r->ntags; i++) {
        g = &r->galaxies[i];
        for (k = 0; k < g->nrings; k++) {
            memset(g->ringn[k], 0, sizeof g->ringn[k]);
            memset(g->ringz[k], 0, sizeof g->ringz[k]);
            if (r->ringalpha < .003 || !g->p.ok)
                continue;
            m = galaxymul(g->plane, g->ring[k]);
            rad = g->ringr[k] * (1 - shrink) * galaxybreathe(g, r->motion) * galaxyeaseoutcubic(g->fill);
            pts = r->rpts + (i * GALAXYRINGS + k) * (GALAXYSEG + 1);
            for (j = 0; j <= GALAXYSEG; j++) {
                th = 2 * GALAXYPI * j / GALAXYSEG;
                pts[j] = galaxyproject(galaxyadd(g->pos, galaxyapply(m, galaxyv(cos(th) * rad, sin(th) * rad, 0))));
            }
            for (j = 0; j < GALAXYSEG; j++) {
                if (!pts[j].ok || !pts[j + 1].ok)
                    continue;
                z = (pts[j].z + pts[j + 1].z) * .5;
                arc = j / GALAXYARCSEG;
                g->ringz[k][arc] += z;
                g->ringn[k][arc]++;
            }
            for (arc = 0; arc < GALAXYARCS; arc++)
                if (g->ringn[k][arc])
                    g->ringz[k][arc] /= g->ringn[k][arc];
        }
    }
}

/* 星系群椭圆轨道 (淡淡的底线) + 每个核心身后的长曝光星轨 + 中心光源 */
static void
galaxyupdatecluster(double shrink)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyProj *pts;
    GalaxyCore *g;
    GalaxyVec v;
    double E, z[3], m;
    int lane, j, arc, i, n[3];

    for (lane = 0; lane < GALAXYLANES; lane++) {
        pts = r->clusterpts[lane];
        memset(r->clustern[lane], 0, sizeof r->clustern[lane]);
        memset(r->clusterz[lane], 0, sizeof r->clusterz[lane]);
        if (r->clusteralpha < .003 || !(r->lanemask & 1 << lane))
            continue;
        for (j = 0; j <= GALAXYSEG; j++) {
            v = galaxyscale(galaxylanepoint(lane, 2 * GALAXYPI * j / GALAXYSEG), 1 - shrink);
            r->clusterr[lane][j] = galaxylen(v);
            pts[j] = galaxyproject(galaxyapply(r->world, galaxytwist(v, r->stage)));
        }
        for (j = 0; j < GALAXYSEG; j++) {
            if (!pts[j].ok || !pts[j + 1].ok)
                continue;
            arc = j / GALAXYARCSEG;
            r->clusterz[lane][arc] += (pts[j].z + pts[j + 1].z) * .5;
            r->clustern[lane][arc]++;
        }
        for (arc = 0; arc < GALAXYARCS; arc++)
            if (r->clustern[lane][arc])
                r->clusterz[lane][arc] /= r->clustern[lane][arc];
    }
    /* 星轨: 沿当前椭圆往回取一段平近点角. 时间均匀, 所以近点处拉得长, 远点处缩得短 */
    for (i = 0; i < r->ntags; i++) {
        g = &r->galaxies[i];
        pts = r->streakpts + i * (GALAXYSTREAK + 1);
        for (j = 0; j < 3; j++) {
            r->streakz[i * 3 + j] = -1;
            z[j] = n[j] = 0;
        }
        if (r->streakalpha < .003 || g->alpha < .01 || !g->p.ok)
            continue;
        pts[0] = g->p;
        for (j = 1; j <= GALAXYSTREAK; j++) {
            m = g->anomaly - galaxylanes[g->lane].dir * GALAXYSTREAKM * j / GALAXYSTREAK;
            E = galaxykepler(m, galaxylanes[g->lane].e);
            pts[j] = galaxyproject(galaxyapply(r->world, galaxytwist(galaxyscale(galaxylanepoint(g->lane, E), 1 - shrink), r->stage)));
        }
        for (j = 0; j < GALAXYSTREAK; j++)
            if (pts[j].ok && pts[j + 1].ok) {
                z[j * 3 / GALAXYSTREAK] += (pts[j].z + pts[j + 1].z) * .5;
                n[j * 3 / GALAXYSTREAK]++;
            }
        for (j = 0; j < 3; j++)
            if (n[j])
                r->streakz[i * 3 + j] = z[j] / n[j];
    }
    r->sunp = galaxyproject(galaxyv(0, 0, 0));
}

/* 盘面尘带按开普勒角速度绕中心流动: 周期正比于半径的 1.5 次方, 内快外慢 (.3 屏宽处约 50s 一圈) */
static GalaxyVec
galaxydustat(GalaxyDust *d, double motion)
{
    GalaxyScene *r = &galaxyscene;
    double rad, th;

    if (!d->disk)
        return d->pos;
    rad = MAX(1, hypot(d->pos.x, d->pos.z));
    th = 2 * GALAXYPI * motion * r->tscale / (50 * pow(rad / (.3 * r->vw), 1.5));
    return galaxyapply(r->world, galaxyv(d->pos.x * cos(th) - d->pos.z * sin(th), d->pos.y, d->pos.x * sin(th) + d->pos.z * cos(th)));
}

static void
galaxyupdatestars(double stage, double motion, double dt)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyStar *s;
    GalaxyCore *g;
    GalaxyVec prev;
    GalaxyMat bb;
    double size = GALAXYCURVE(galaxycardsize, stage), vis = GALAXYCURVE(galaxycardvis, stage);
    double tint = GALAXYCURVE(galaxycardtint, stage), glow = GALAXYCURVE(galaxystarglow, stage);
    double hb = galaxysmoothstep(galaxyphase(stage, 3.3, 4.2)), collapse, a, t, fly;
    GalaxyVec dir;

    /* 俯冲起卡片就转为朝向镜头: 平行于画面的卡片走仿射采样, 近在眼前也不卡 */
    hb = MAX(hb, galaxybeatw() * galaxysmoothstep(galaxyphase(r->iclock, 3.1, 3.5)));
    int i;

    for (i = 0; i < r->nstars; i++) {
        s = &r->stars[i];
        g = &r->galaxies[s->galaxy];
        prev = s->pos;
        galaxystarat(s, stage, motion, r->world, g->pos, galaxymul(g->plane, g->ring[s->ring]), &s->pos, &s->orient);
        if (hb > 0) {
            /* 驻留时卡片大体朝向镜头 (截图可辨认), 随轨道位置摆动 ±26° / ±16°, 保留立体感 */
            a = galaxyorbitangle(s, stage, motion);
            bb = galaxymul(r->cam.rot, galaxyeuler(.28 * sin(a + s->rock), .45 * cos(a), 0));
            s->orient = galaxyblend(s->orient, bb, hb);
        }
        s->hover = galaxyfollow(s->hover, r->hover == i, dt, .12);
        if (s->hover > .001)  /* 悬停: 稍微靠近镜头 */
            s->pos = galaxyadd(s->pos, galaxyscale(galaxynormalize(galaxysub(r->cam.pos, s->pos)), .06 * r->cam.focal * s->hover));
        if (g->callout > .001)  /* 点名: 环上的卡片沿轨道半径向外弹一下 */
            s->pos = galaxyadd(s->pos, galaxyscale(galaxynormalize(galaxysub(s->pos, g->pos)), .18 * s->radius * g->callout));
        if (s->flipcard > 0)    /* 翻面亮相: 绕自身竖轴转一圈 */
            s->orient = galaxymul(s->orient, galaxyroty(2 * GALAXYPI * s->flipcard));
        s->vel = dt > 0 ? galaxyscale(galaxysub(s->pos, prev), 1 / dt) : galaxyv(0, 0, 0);
        if (s->died > 0) {
            /* 关闭的窗口: 沿关闭时的速度方向 (轨道切线) 滑行, 0.35s 后加速飞出 */
            t = galaxynow() - s->died;
            fly = MAX(0, t - .35);
            dir = galaxynormalize(s->dvel);
            s->pos = galaxyadd(galaxyadd(s->dpos, galaxyscale(s->dvel, t)), galaxyscale(dir, .9 * r->cam.focal * fly * fly));
            s->vel = galaxyadd(s->dvel, galaxyscale(dir, 1.8 * r->cam.focal * fly));
            s->hover = 0;
        }
        /* 首帧必须和桌面截图逐像素对齐; 聚焦放大等卡片离开桌面后才开始. */
        s->size = size * (1 + (s->focused ? .08 * galaxysmoothstep(galaxyphase(stage, .12, .75)) : 0))
            * (1 + .42 * s->hover);
        collapse = galaxyphase(stage, 4.8 + s->delay * .5, 5.2 + s->delay * .5);
        /* 当前桌面的窗口从第一帧起就画在自己原来的位置上 (与真实窗口逐像素重合), 背后的桌面截图先淡出再开始移动,
         * 移动中的窗口不会和静止的桌面截图叠成重影 */
        s->alpha = (s->current ? 1 : galaxyeaseoutquart(galaxyphase(stage, .6, 1)))
            * (1 - galaxyeaseincubic(galaxyphase(collapse, .7, 1)));
        s->p = galaxyproject(s->pos);
        if (s->p.ok && stage > 1)    /* 开场最初几帧卡片与真实窗口重合, 不能淡 */
            s->alpha *= galaxynearfade(s->p.z);
        if ((r->mode == GalaxyOrbit || (r->mode == GalaxyIntro && r->iclock > 3)) && s->p.ok) {
            /* 驻留时卡片有尺寸上限; 俯冲时卡片近在眼前, 也限制在 700px 以内 */
            double screen = MAX(s->w, s->h) * s->size * s->p.scale;
            double cap = r->mode == GalaxyOrbit ? galaxymix(460, 600, s->hover) : 700;
            if (screen > cap)
                s->size *= cap / screen;
        }
        s->brightness = s->p.ok ? galaxydepthlight(s->p.z) * (s->focused ? 1.1 : 1) * (1 + .25 * s->hover) : 0;
        if (s->current)  /* 脱离桌面前与背景一起变暗 (100% -> 70%), 之后过渡到深度亮度 */
            s->brightness = galaxymix(GALAXYCURVE(galaxybright, stage), s->brightness, galaxysmoothstep(galaxyphase(stage, .1, .58)));
        /* 深度 LOD: 投影后的卡片足够大才显示截图面板, 远处只剩光点 */
        s->lod = s->p.ok ? galaxysmoothstep((MAX(s->w, s->h) * s->size * s->p.scale - 52) / 58) : 0;
        s->vis = MAX(vis * s->lod, .85 * s->hover * (vis > .05)) * s->alpha;
        s->tint = (r->mode == GalaxyOrbit ? 0 : tint + .1 * s->hover) * s->alpha * MIN(1, s->vis / .3);
        s->glow = glow * s->alpha * MIN(1, s->brightness);
        if (s->flipcard > 0 && s->flipcard < 1) {   /* 翻面时一道扫过的高光 */
            s->tint += .5 * sin(GALAXYPI * s->flipcard) * s->alpha;
            s->glow += .6 * sin(GALAXYPI * s->flipcard) * s->alpha;
        }
        s->brightness *= 1 + .3 * g->callout + .35 * s->constel;
        if (s->born > 0) {
            /* 新星诞生 (2s): 先是一点光亮起, 光晕扩开, 卡片再从 0 展开 */
            t = (galaxynow() - s->born) / 2;
            if (t >= 1) {
                s->born = 0;
                if (r->log)
                    fprintf(r->log, "galaxy birth: star %d settled at %.0f,%.0f%s\n", i, s->p.x, s->p.y, s->p.ok ? "" : " (behind camera)"),
                        fflush(r->log);
            } else {
                s->size *= MAX(.02, galaxyeaseoutback(galaxyphase(t, .3, 1)));
                s->vis *= galaxysmoothstep(galaxyphase(t, .35, .9));
                s->glow = MAX(s->glow, s->alpha * 1.6 * galaxyflash(2 * t, .2, 2.2));
                s->brightness *= 1 + .5 * (1 - galaxysmoothstep(t));
            }
        }
        if (s->died > 0) {
            /* 化作流星: 卡片 0.4s 内缩成光点, 光点拖着长尾飞走 (尾巴在 galaxyrenderevents 里画) */
            t = (galaxynow() - s->died) / 1.6;
            fly = galaxyeaseincubic(galaxyphase(t, 0, .25));
            s->size *= 1 - .92 * fly;
            s->vis *= 1 - fly;
            s->tint = 0;
            s->glow = s->alpha * (1 + 1.5 * galaxyflash(t * 1.6, .15, 3)) * (1 - galaxysmoothstep(galaxyphase(t, .7, 1)));
            s->alpha *= t >= 1 ? 0 : 1;
            if (t >= 1)
                s->vis = s->glow = 0;
        }
        if (r->kqlen) {     /* 键盘过滤: 不匹配的卡片和光点淡下去, 匹配的提亮 */
            if (s->kmatch) {
                s->brightness *= 1.15;
            } else {
                s->vis *= .25;
                s->glow *= .3;
                s->brightness *= .5;
            }
        }
    }
}

static int
galaxyitemcmp(const void *pa, const void *pb)
{
    const GalaxyItem *a = pa, *b = pb;
    return a->z > b->z ? -1 : a->z < b->z;
}

/* 画家算法: 每帧按镜头空间 z 从远到近排序, 轨道短弧与核心和窗口穿插. */
static void
galaxysortdepth(void)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyCore *g;
    int i, k, arc, n = 0;

    for (i = 0; i < r->ndust; i++)
        if (r->dust[i].p.ok)
            r->items[n++] = (GalaxyItem){GalaxyDustItem, i, r->dust[i].p.z};
    for (i = 0; i < r->ntags; i++) {
        g = &r->galaxies[i];
        if (g->p.ok && g->alpha > .002)
            r->items[n++] = (GalaxyItem){GalaxyCoreItem, i, g->p.z};
        if (r->ringalpha * g->alpha < .003)
            continue;
        for (k = 0; k < g->nrings; k++)
            for (arc = 0; arc < GALAXYARCS; arc++)
                if (g->ringn[k][arc])
                    r->items[n++] = (GalaxyItem){GalaxyRingItem, (i * GALAXYRINGS + k) * GALAXYARCS + arc, g->ringz[k][arc]};
    }
    for (i = 0; i < GALAXYLANES; i++)
        for (arc = 0; arc < GALAXYARCS; arc++)
            if (r->clustern[i][arc])
                r->items[n++] = (GalaxyItem){GalaxyClusterItem, i * GALAXYARCS + arc, r->clusterz[i][arc]};
    for (i = 0; i < r->ntags * 3; i++)
        if (r->streakz[i] > 0)
            r->items[n++] = (GalaxyItem){GalaxyStreakItem, i, r->streakz[i]};
    if (r->sunp.ok && r->sunalpha > .003)
        r->items[n++] = (GalaxyItem){GalaxySunItem, 0, r->sunp.z};
    for (i = 0; i < r->nstars; i++)
        if (r->stars[i].p.ok && r->stars[i].alpha > .002)
            r->items[n++] = (GalaxyItem){GalaxyStarItem, i, r->stars[i].p.z};
    r->nitems = n;
    qsort(r->items, n, sizeof *r->items, galaxyitemcmp);
}

/* 开场 / 驻留 / 坍缩: 整个场景由 (stage, motion) 决定; dt 是真实时间, 只用于平滑交互 */
/* ---------- 导演镜头: 驻留时按机位轮换, 交互时停住让位 ---------- */

typedef struct { double pitch, yaw, dist, roll; } GalaxyShot;
enum { GalaxyShotWide, GalaxyShotEdge, GalaxyShotBelow, GalaxyShotTour, GalaxyShotCross, GalaxyShotTop };
static const char *galaxyshotname[] = { "wide", "edge", "below", "tour", "cross", "top" };
/* 全景与巡游交替: 巡游时镜头飞到某个星系身边绕着它转, 两次巡游之间飞越到下一个星系 */
static const int galaxyshotseq[] = {
    GalaxyShotWide, GalaxyShotTour, GalaxyShotTour, GalaxyShotEdge, GalaxyShotTour, GalaxyShotBelow,
    GalaxyShotTour, GalaxyShotCross, GalaxyShotTour, GalaxyShotTop, GalaxyShotTour
};

/* 机位在其开始后 t 真实秒的参数 (t 可略超出 [0, GALAXYSHOT], 用于过渡段). 角度单位: 度.
 * slot: 巡游机位用哪个跟踪槽 (0 当前 / 1 下一个) */
static GalaxyShot
galaxyshotat(int shot, double t, int slot)
{
    GalaxyScene *r = &galaxyscene;
    double x = t / GALAXYSHOT, breathe = 1 + .12 * sin(2 * GALAXYPI * r->dclock / 20);

    switch (shot) {
    case GalaxyShotEdge:     /* 贴近盘面侧掠: 椭圆压成细线, 核心前后遮挡 */
        return (GalaxyShot){-4, -30 + 60 * x, .68 * breathe, GALAXYDIAG + 2};
    case GalaxyShotBelow:    /* 从盘面下方仰视 */
        return (GalaxyShot){11, 34 - 34 * x, .7 * breathe, GALAXYDIAG - 4};
    case GalaxyShotTour:     /* 巡游: 在核心的轨道外侧, 绕着它慢慢转, 远处是整个星系群 */
        return (GalaxyShot){-17 + 7 * sin(2 * GALAXYPI * t / 14), r->dtyaw[slot] + 30 + 35 * x, GALAXY_TOURDIST, GALAXYDIAG * .6};
    case GalaxyShotCross:    /* 镜头从盘面上方推过盘面到下方 */
        return (GalaxyShot){-20 + 32 * galaxysmoothstep(x), 48 - 18 * x, (.76 - .08 * x) * breathe, GALAXYDIAG - 2};
    case GalaxyShotTop:      /* 高空俯瞰 */
        return (GalaxyShot){-34, 8 + 26 * x, .9 * breathe, GALAXYDIAG};
    default:                /* 低角度斜视全景 */
        return (GalaxyShot){-14, -4 + 28 * x, .72 * breathe, GALAXYDIAG};
    }
}

/* 巡游的下一站: 离当前位置最近、最近几次没去过的星系 (有窗口的优先), 镜头不会横穿整个星系群 */
static int
galaxytourpick(GalaxyVec from, int not)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyCore *g;
    double best = 1e18, d;
    int i, j, pick = -1;

    for (i = 0; i < r->ntags; i++) {
        g = &r->galaxies[i];
        if ((r->npop && !g->nstars) || i == not)
            continue;
        d = galaxylen(galaxysub(g->pos, from));
        for (j = 0; j < (int)LENGTH(r->tourhist); j++)
            if (r->tourhist[j] == i)
                d = d * 3 + r->w;   /* 最近去过: 尽量不选 */
        if (d < best) {
            best = d;
            pick = i;
        }
    }
    if (pick < 0)
        pick = not >= 0 ? not : 0;
    memmove(r->tourhist + 1, r->tourhist, sizeof r->tourhist - sizeof *r->tourhist);
    r->tourhist[0] = pick;
    return pick;
}

static void
galaxytourtrack(int slot, double dt, int snap)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyCore *g;
    double ty;

    if (r->dtg[slot] < 0 || r->dtg[slot] >= r->ntags)
        return;
    g = &r->galaxies[r->dtg[slot]];
    ty = atan2(-g->pos.x, -g->pos.z) * 180 / GALAXYPI;
    if (snap) {
        r->dtour[slot] = g->pos;
        r->dtyaw[slot] = ty;
        return;
    }
    /* 只跟随位置; 视角 (dtyaw) 在开始巡游时定下, 不跟着星系绕中心转, 否则星系过近点时镜头会被甩着转 */
    r->dtour[slot] = galaxylerp(r->dtour[slot], g->pos, 1 - exp(-dt * r->dspeed / .45));
}

static void
galaxyupdatecamera(double stage, double motion, double dt)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyShot a, b;
    GalaxyCore *g;
    GalaxyVec ta = galaxyv(0, 0, 0), tb = galaxyv(0, 0, 0), target;
    double hw = r->holdw, dw = hw, period = GALAXYSHOT + GALAXYSHOTMIX, x, mix, wf, dev, fit, dist, ya, yb, yd;
    double kd, kp, ky, kr, bw, cp, cy, s = r->iclock;
    int k, sa, sb, i, active, n = LENGTH(galaxyshotseq);

    /* 基础镜头: 关键帧 (stage); 旋转之后换成节拍镜头 (开场时钟), 快进 / 坍缩时由 beatfade 淡回关键帧 */
    kd = GALAXYCURVE(galaxycamdist, stage);
    kp = GALAXYCURVE(galaxycampitch, stage);
    ky = GALAXYCURVE(galaxycamyaw, stage);
    kr = GALAXYCURVE(galaxycamroll, stage);
    bw = (r->mode == GalaxyIntro || r->mode == GalaxyCollapse) && s < GALAXYIEND
        ? galaxysmoothstep(galaxyphase(s, 3.2, 3.5)) * r->beatfade : 0;
    if (bw > 0) {
        kd = galaxymix(kd, GALAXYCURVE(galaxybeatdist, s), bw);
        kp = galaxymix(kp, GALAXYCURVE(galaxybeatpitch, s), bw);
        ky = galaxymix(ky, GALAXYCURVE(galaxybeatyaw, s), bw);
        kr = galaxymix(kr, GALAXYCURVE(galaxybeatroll, s), bw);
    }
    if (r->mode == GalaxyIntro || r->mode == GalaxyCollapse)
        /* 起飞时轻推一下; 超空间跃迁时镜头前冲 */
        kd *= 1 - r->beatfade * (.03 * sin(GALAXYPI * galaxyphase(s, .1, .6)) + .08 * r->warpfx);

    if (r->mode == GalaxyCollapse)
        dw *= 1 - galaxysmoothstep(r->celapsed / 1.1);
    /* 交互让位: 最近 4s 动过鼠标 / 滚轮, 或指针停在星体 / 核心上时, 导演时钟平滑减速到停 */
    if (r->mode == GalaxyOrbit && r->mouseawake && !r->dragging) {
        r->dragtyaw = MAX(-60, MIN(60, r->dragtyaw + r->dragvyaw * dt));
        r->dragtpitch = MAX(-32, MIN(32, r->dragtpitch + r->dragvpitch * dt));
        r->dragvyaw *= exp(-dt / .18);
        r->dragvpitch *= exp(-dt / .18);
    }
    if (r->mode == GalaxyOrbit) {
        double follow = 1 - exp(-dt / (r->mouseawake ? .14 : .85));
        r->dragyaw += MAX(-70 * dt, MIN(70 * dt, (r->dragtyaw - r->dragyaw) * follow));
        r->dragpitch += MAX(-50 * dt, MIN(50 * dt, (r->dragtpitch - r->dragpitch) * follow));
    }
    active = r->dragging || r->last - r->lastpointer < 4 || r->hover >= 0 || r->hovercore >= 0;
    r->dspeed = galaxyfollow(r->dspeed, !active, dt, .6);
    if (r->mode == GalaxyOrbit)
        r->dclock += dt * r->dspeed;
    k = (int)floor(r->dclock / period);
    x = r->dclock - k * period;
    sa = galaxyshotseq[k % n];
    sb = galaxyshotseq[(k + 1) % n];
    if (k != r->dshot) {
        /* 进入新机位: 原来「下一个」的跟踪槽变成「当前」 */
        if (r->dshot >= 0 && k == r->dshot + 1) {
            r->dtg[0] = r->dtg[1];
            r->dtour[0] = r->dtour[1];
            r->dtyaw[0] = r->dtyaw[1];
            r->dyoff[0] = r->dyoff[1];
        } else {
            r->dtg[0] = sa == GalaxyShotTour ? galaxytourpick(galaxyv(0, 0, 0), -1) : -1;
            galaxytourtrack(0, dt, 1);
            r->dyoff[0] = 0;
        }
        r->dtg[1] = sb == GalaxyShotTour
            ? galaxytourpick(r->dtg[0] >= 0 ? r->galaxies[r->dtg[0]].pos : galaxyv(0, 0, 0), r->dtg[0]) : -1;
        galaxytourtrack(1, dt, 1);
        /* 机位切换时镜头最多转 35°: 下一个机位的 yaw 整体平移到离当前机位结束时的 yaw 不远的地方 */
        ya = galaxyshotat(sa, period, 0).yaw + r->dyoff[0];
        yb = galaxyshotat(sb, 0, 1).yaw;
        yd = remainder(ya - yb, 360);
        r->dyoff[1] = yd - MAX(-35, MIN(35, yd));
        r->dshot = k;
        if (r->log && r->mode == GalaxyOrbit)
            fprintf(r->log, "galaxy shot: %s (tag %d) at %.1fs\n", galaxyshotname[sa],
                    sa == GalaxyShotTour ? r->dtg[0] + 1 : 0, motion);
    }
    if (r->dspeed > .01) {
        galaxytourtrack(0, dt, 0);
        galaxytourtrack(1, dt, 0);
    }
    a = galaxyshotat(sa, x, 0);
    b = galaxyshotat(sb, x - period, 1);
    a.yaw += r->dyoff[0];
    b.yaw += r->dyoff[1];
    mix = galaxyeaseinoutcubic((x - GALAXYSHOT) / GALAXYSHOTMIX);
    b.yaw = a.yaw + remainder(b.yaw - a.yaw, 360);   /* 机位之间 yaw 走短弧 */
    if (sa == GalaxyShotTour && r->dtg[0] >= 0)
        ta = r->dtour[0];
    if (sb == GalaxyShotTour && r->dtg[1] >= 0)
        tb = r->dtour[1];
    target = galaxyscale(galaxylerp(ta, tb, mix), dw);
    wf = (sa == GalaxyShotTour) * (1 - mix) + (sb == GalaxyShotTour) * mix;
    /* 自动取景: 只在全景机位里让核心贴近屏幕边缘. 偏移按滚轮缩放归一, 不会把用户的缩放抵消掉 */
    if (r->mode == GalaxyOrbit && r->dspeed > .5 && wf < .5) {
        dev = 0;
        for (i = 0; i < r->ntags; i++) {
            g = &r->galaxies[i];
            if (!g->p.ok || g->alpha < .1)
                continue;
            dev = MAX(dev, (fabs(g->p.x - r->vx - r->vw * .5) + g->size * g->p.scale) / (r->vw * .5));
            dev = MAX(dev, (fabs(g->p.y - r->vy - r->vh * .5) + g->size * g->p.scale) / (r->vh * .5));
        }
        if (dev > 0) {
            dev *= galaxymix(1, r->zoom, hw);
            fit = MAX(.8, MIN(1.25, r->dfit * dev / .98));
            r->dfit = galaxyfollow(r->dfit, fit, dt, 4);   /* 慢慢调, 不让画面「呼吸」得太明显 */
        }
    }
    dist = galaxymix(a.dist * galaxymix(r->dfit, 1, sa == GalaxyShotTour), b.dist * galaxymix(r->dfit, 1, sb == GalaxyShotTour), mix);
    if (sa == GalaxyShotTour || sb == GalaxyShotTour)
        dist += .22 * sin(GALAXYPI * mix);   /* 飞越: 先拉远再推近 */
    cp = galaxymix(kp, galaxymix(a.pitch, b.pitch, mix), dw) + hw * (r->ppitch + r->dragpitch);
    cy = galaxymix(ky, galaxymix(a.yaw, b.yaw, mix), dw) + hw * (r->pyaw + r->dragyaw);
    if (r->mode == GalaxyOrbit)
        cp = MAX(-48, MIN(32, cp));
    galaxysetcameraat(target,
            MAX(.35, galaxymix(kd, dist, dw)) * r->cam.focal * galaxymix(1, r->zoom, hw),
            cp, cy,
            galaxymix(kr, galaxymix(a.roll, b.roll, mix), dw));
    /* GALAXY_TRACE=1: 每帧记录镜头, 用来检查镜头运动是否平滑 (dwm/tests/galaxy/camtrace.py 分析) */
    if (r->trace && r->log && r->mode == GalaxyOrbit)
        fprintf(r->log, "cam %.4f %d %d %.3f %.4f %.3f %.3f %.3f %.1f %.1f %.1f\n", r->last, sa, sb, mix,
                r->cam.dist / r->cam.focal, r->cam.rx * 180 / GALAXYPI, r->cam.ry * 180 / GALAXYPI, r->cam.rz * 180 / GALAXYPI,
                r->cam.target.x, r->cam.target.y, r->cam.target.z);
}

/* 星系翻转 / 涟漪各记一行日志, 便于核对画面 */
static void
galaxylogactions(double motion)
{
    GalaxyScene *r = &galaxyscene;
    double local;
    int k;

    if (!r->log || r->mode != GalaxyOrbit)
        return;
    if (galaxycycle(motion, 4, 6, &k, &local) && k != r->lastripple) {
        r->lastripple = k;
        fprintf(r->log, "galaxy ripple at %.1fs\n", motion);
    }
    if (!r->quiet && galaxycycle(motion, 6, 15, &k, &local) && k != r->lastcomet) {
        r->lastcomet = k;
        fprintf(r->log, "galaxy comet at %.1fs\n", motion);
    }
}

/* 涟漪: 驻留后每 6s 从中心光源发出一圈亮波, 以 .4 屏宽/秒沿轨道向外扩散, 扫过的核心短暂亮起 */
static void
galaxyupdateripple(double motion)
{
    GalaxyScene *r = &galaxyscene;
    double local;   /* 本次涟漪发出后的真实秒数 */
    int k;

    r->ripple = r->rippleamp = r->sunpulse = 0;
    if (!galaxycycle(motion, 4, 6, &k, &local))
        return;
    r->ripple = .4 * r->vw * local;
    r->rippleamp = galaxyclamp(1 - r->ripple / (.65 * r->vw));
    r->sunpulse = .7 * galaxyflash(local, .6, 2);
}

/* 局部轨道环的描绘进度: 像光笔一样从起点沿环画出一整圈 */
static double
galaxyringreveal(GalaxyCore *g)
{
    double s = galaxyscene.iclock;

    return 1 - galaxybeatw() * (1 - galaxyeaseinoutcubic(galaxyphase(s, 1 + .05 * g->rank, 1.9 + .05 * g->rank)));
}

/* 开场节拍: 跃迁 / 轨道描绘 / 点火 / 点名 / 星轨拉出 / 卡片翻面 / 转速峰值爆闪 / 落定涟漪 */
static void
galaxyupdatebeats(double motion)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyCore *g;
    double f = galaxybeatw(), s = r->iclock, t, x;
    int i, k;

    r->warpfx = f * (pow(sin(GALAXYPI * galaxyphase(s, .55, 1.35)), 2) + .5 * pow(sin(GALAXYPI * galaxyphase(s, 3.3, 3.95)), 2));
    if (r->variant == GalaxyGate)   /* 星门: 接近时只有淡淡的拉丝, 穿过星门的一刻光线从环心涌出 */
        r->warpfx = f * (.25 * pow(sin(GALAXYPI * galaxyphase(s, .55, 1.2)), 2) + 1.3 * pow(sin(GALAXYPI * galaxyphase(s, 1.12, 1.65)), 2)
                + .5 * pow(sin(GALAXYPI * galaxyphase(s, 3.3, 3.95)), 2));
    for (i = 0; i < GALAXYLANES; i++)
        r->lanereveal[i] = 1 - f * (1 - galaxyeaseinoutcubic(galaxyphase(s, 1.5 + .12 * i, 2.3 + .12 * i)));
    for (i = 0; i < r->ntags; i++) {
        g = &r->galaxies[i];
        g->ignite = f * exp(-pow((s - galaxyignitet(g)) / .1, 2));
        g->streakreveal = 1 - f * (1 - galaxyeaseinoutcubic(galaxyphase(s, 4.6 + .05 * g->rank, 5.4 + .05 * g->rank)));
        g->callout = 0;
    }
    /* 铺满全屏时, 有窗口的星系依次点名 */
    for (k = 0; k < r->npop; k++) {
        t = 3.95 + k * .6 / r->npop;
        r->galaxies[r->popord[k]].callout = f * exp(-pow((s - t) / .09, 2));
    }
    for (i = 0; i < r->nstars; i++) {
        t = 4.9 + .45 * i / MAX(1, r->nstars);
        r->stars[i].flipcard = f * galaxyeaseinoutcubic(galaxyphase(s, t, t + .5));
    }
    r->sunpulse += f * 1.5 * exp(-pow((s - 2.85) / .12, 2));
    /* 回缩落定的一刻, 中心光源发出一圈大涟漪 (之后接驻留的周期涟漪) */
    x = (motion - GALAXYIEND + .25) * r->tscale;
    if (x >= 0 && x < 2.5 && r->mode != GalaxyCollapse) {
        t = 1.3 * galaxyclamp(1 - .45 * x / .75);
        if (t > r->rippleamp) {
            r->ripple = .45 * r->vw * x;
            r->rippleamp = t;
        }
        r->sunpulse = MAX(r->sunpulse, galaxyflash(x, .6, 2));
    }
}

/* 驻留特效的状态 (强度乘 holdw): 超新星 / 星座连线 / 核心光桥 / 超新星附近的尘埃提亮 */
static int
galaxyonview(GalaxyProj p)
{
    GalaxyScene *r = &galaxyscene;

    return p.ok && p.x > r->vx && p.x < r->vx + r->vw && p.y > r->vy && p.y < r->vy + r->vh;
}

/* 驻留特效的目标星系: 画面里看得见、离镜头不太近 (近处翻转或爆闪太刺眼)、不是正在 / 即将巡游的那个.
 * populated: 只选有窗口的; not: 再排除一个. 候选里用 seed 做确定性的轮换, 没有合适的时放宽条件 */
static int
galaxyfxpick(int populated, int not, unsigned int seed)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyCore *g;
    double best = 2, h;
    int i, pass, pick = -1, onscreen, toured;

    /* 逐轮放宽: 0 画面里 + 非巡游 + 不重复上次; 1 非巡游 + 不重复 (镜头背后的也行); 2 非巡游; 3 任意 */
    for (pass = 0; pass < 4 && pick < 0; pass++)
        for (i = 0; i < r->ntags; i++) {
            g = &r->galaxies[i];
            if (g->alpha < .1 || (populated && r->npop && !g->nstars))
                continue;
            onscreen = g->p.ok && g->p.x > r->vx + .08 * r->vw && g->p.x < r->vx + .92 * r->vw
                && g->p.y > r->vy + .08 * r->vh && g->p.y < r->vy + .92 * r->vh && g->p.z > .6 * r->cam.focal;
            toured = i == r->dtg[0] || i == r->dtg[1];
            if ((pass == 0 && !onscreen) || (pass < 3 && toured) || (pass < 2 && i == not))
                continue;
            h = galaxyhash(seed * 131 + i * 17);
            if (h < best) {
                best = h;
                pick = i;
            }
        }
    return pick;
}

static void
galaxyupdateholdfx(double motion)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyCore *g, *h;
    GalaxyDust *d;
    double f = r->holdw * r->calm, local, env, best, dist;  /* 天象发生时随机特效让位 */
    int i, j, k, gi = -1;

    for (i = 0; i < r->ntags; i++)
        r->galaxies[i].nova = r->galaxies[i].bridge = 0;
    for (i = 0; i < r->nstars; i++)
        r->stars[i].constel = 0;
    if (f < .01 || r->ntags < 1)
        return;
    /* 星系翻转: 每 5s 一次, 开始时选定星系并记下开始时刻 (galaxyflipat 按它算角度) */
    if (r->npop && galaxycycle(motion, 1.5, 5, &k, &local) && k != r->flipk) {
        r->flipk = k;
        r->flipg = galaxyfxpick(1, r->flipg, k * 3 + 1);
        r->flipstart = motion - local / r->tscale;
        if (r->log && r->mode == GalaxyOrbit && r->flipg >= 0)
            fprintf(r->log, "galaxy flip: tag %d at %.1fs\n", r->flipg + 1, motion);
    }
    /* 超新星: 每 20s 一次, 选画面里看得见、不在镜头跟前的核心 */
    if (galaxycycle(motion, 9, 20, &k, &local) && local < 3) {
        if (k != r->novak) {
            r->novak = k;
            r->novag = galaxyfxpick(0, r->flipg, k * 7 + 5);
            if (r->log && r->mode == GalaxyOrbit && r->novag >= 0)
                fprintf(r->log, "galaxy supernova: tag %d at %.1fs\n", r->novag + 1, motion);
        }
        gi = r->novag;
        if (gi >= 0)
            r->galaxies[gi].nova = f * galaxyflash(local, .4, 1.6);
    }
    for (i = 0; i < r->ndust; i++) {
        d = &r->dust[i];
        d->boost = 0;
        if (gi >= 0 && d->disk) {
            dist = galaxylen(galaxysub(galaxydustat(d, motion), r->galaxies[gi].pos));
            d->boost = r->galaxies[gi].nova * galaxyclamp(1 - dist / (.25 * r->vw));
        }
    }
    /* 星座连线: 每 4s 一次; 正在巡游时连镜头跟前的那个星系 (近处看得清), 否则选画面里的 */
    if (r->npop && galaxycycle(motion, 2, 4, &k, &local)) {
        if (k != r->constk) {
            r->constk = k;
            r->constg = r->dtg[0] >= 0 && r->galaxies[r->dtg[0]].nstars ? r->dtg[0] : galaxyfxpick(1, r->constg, k * 5 + 2);
        }
        gi = r->constg;
        env = galaxysmoothstep(local / .4) * (1 - galaxysmoothstep((local - 1.6) / 1));
        for (i = 0; i < r->nstars; i++)
            if (r->stars[i].galaxy == gi && !r->stars[i].died)
                r->stars[i].constel = f * env;
    }
    /* 核心光桥: 每 7s 一次, 连接不同轨道上相距最近的两个核心 */
    if (galaxycycle(motion, 3, 7, &k, &local)) {
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
                    /* 加一点随机, 不总是同一对; 两端都在画面里的优先 */
                    dist = galaxylen(galaxysub(g->pos, h->pos)) * (1 + .6 * galaxyhash(k * 31 + i * 7 + j))
                        * (galaxyonview(g->p) && galaxyonview(h->p) ? 1 : 6);
                    if (dist < best) {
                        best = dist;
                        r->bri = k % 2 ? j : i;
                        r->brj = k % 2 ? i : j;
                    }
                }
        }
        if (r->brj >= 0 && local > 1.15)
            r->galaxies[r->brj].bridge = .7 * f * galaxyflash(local - .9, .4, 2.5);
    }
}

static void
galaxyupdatescene(double stage, double motion, double dt)
{
    GalaxyScene *r = &galaxyscene;
    int orbit = r->mode == GalaxyOrbit, i;

    r->holdw = galaxysmoothstep(galaxyphase(motion, GALAXYIEND - .5, GALAXYIEND + 2.5))
        * (1 - galaxysmoothstep(galaxyphase(stage, GALAXYEXIT, GALAXYEXIT + .5)));
    r->pyaw = galaxyfollow(r->pyaw, orbit ? r->tyaw : 0, dt, .35);
    r->ppitch = galaxyfollow(r->ppitch, orbit ? r->tpitch : 0, dt, .35);
    r->zoom = galaxyfollow(r->zoom, orbit ? r->tzoom : 1, dt, .25);
    galaxyupdateripple(motion);
    if (r->chimeat > 0 && r->mode == GalaxyOrbit) {
        /* 整点: 双星同步爆闪, 一圈慢速大光波扫过所有轨道 (扫到的核心亮起) */
        double t = galaxynow() - r->chimeat;
        if (t < 5) {
            r->ripple = .95 * r->vw * galaxyeaseoutcubic(t / 5);
            r->rippleamp = 1.4 * (1 - galaxysmoothstep(t / 5));
        }
        r->sunpulse = MAX(r->sunpulse, 1.6 * galaxyflash(t, .3, 1.1));
    }
    r->rippleamp *= r->holdw;
    r->sunpulse *= r->holdw;
    galaxyupdatebeats(motion);
    galaxyupdatecamera(stage, motion, dt);
    galaxylogactions(motion);
    r->world = r->mode == GalaxyCollapse ? r->cworld : galaxyworldat(stage, motion);
    if (r->trace && r->log && r->mode == GalaxyIntro && r->iclock >= 3.2 && r->iclock <= 4.0)
        fprintf(r->log, "rise %.4f %.3f %.3f %.3f\n", r->iclock, galaxyintrotilt(motion) * 180 / GALAXYPI,
                r->cam.dist / r->cam.focal, r->cam.rx * 180 / GALAXYPI);
    galaxyupdatecores(stage, motion, dt);
    galaxyupdateholdfx(motion);
    r->ringalpha = GALAXYCURVE(galaxyringkeys, stage);
    galaxyupdaterings(galaxyeaseincubic(galaxyphase(stage, 4.8, 5.3)));
    /* 椭圆底线: 高速旋转时较明显, 驻留时退成淡线, 让位给星轨 */
    r->clusteralpha = .2 * galaxysmoothstep(galaxyphase(stage, 1.5, 2.3)) * galaxymix(1, .55, galaxysmoothstep(galaxyphase(stage, 3.3, 4.2)))
        * (1 - galaxysmoothstep(galaxyphase(stage, 5.1, 5.55)));
    r->streakalpha = .24 * galaxysmoothstep(galaxyphase(stage, 2.8, 3.8)) * (1 - galaxysmoothstep(galaxyphase(stage, 4.85, 5.1)));
    if (r->mode == GalaxyIntro)  /* 开场里星轨在回缩时才点亮 */
        r->streakalpha = .24 * galaxymix(galaxysmoothstep(galaxyphase(stage, 2.8, 3.8)),
                galaxysmoothstep(galaxyphase(r->iclock, 4.6, 5)), r->beatfade);
    r->sunalpha = galaxysmoothstep(galaxyphase(stage, 1.5, 2.3)) * (1 - galaxysmoothstep(galaxyphase(stage, 4.9, 5.2)));
    if (r->mode == GalaxyCollapse) {
        /* 环绕时保留群轨道和星轨, 淡出局部环和历史尾迹, 让运动方向一眼可辨. */
        r->ringalpha *= 1 - .65 * galaxysmoothstep(galaxyphase(r->celapsed, .1, .6));
        r->clusteralpha *= 1 - .25 * galaxysmoothstep(galaxyphase(r->celapsed, .4, 1.1));
    }
    galaxyupdatecluster(galaxyeaseinoutcubic(galaxyphase(stage, 4.85, 5.45)));
    galaxyupdatestars(stage, motion, dt);
    for (i = 0; i < r->ndust; i++)
        r->dust[i].p = galaxyproject(galaxydustat(&r->dust[i], motion));
    r->bright = GALAXYCURVE(galaxybright, stage);
    r->space = GALAXYCURVE(galaxyspacekeys, stage);
    if (r->variant == GalaxyShatter && r->mode == GalaxyIntro)  /* 碎块后面直接是深空 */
        r->space = MAX(r->space, galaxysmoothstep(galaxyphase(stage, .12, .3)));
    r->vign = GALAXYCURVE(galaxyvignettekeys, stage);
    r->desk = 1 - galaxysmoothstep(galaxyphase(stage, 0, .1));
    r->deskover = r->bar = 0;
    r->reveal = galaxyeaseinoutcubic(galaxyphase(stage, 5.7, 6));
    r->trailgain = MAX(GALAXYCURVE(galaxytrailgain, stage), .6 * r->warpfx);
    if (r->mode == GalaxyCollapse)
        r->trailgain *= 1 - galaxysmoothstep(galaxyphase(r->celapsed, 0, .35));
    r->dustfade = galaxyeaseoutcubic(galaxyphase(stage, .5, 1.3)) * (1 - galaxyeaseinoutcubic(galaxyphase(stage, 5, 5.7)));
    r->central = galaxyeaseoutcubic(galaxyphase(stage, 5.15, 5.45)) * (1 - galaxyeaseinoutcubic(galaxyphase(stage, 5.7, 6)));
    r->pulse = sin(GALAXYPI * galaxyphase(stage, 5.5, 5.7));
    galaxysortdepth();
}

/* 回程: 从冻结的画面出发, 目标 tag 的窗口沿 3D 曲线飞回原位置变回截图, 其他星体退向深处, 镜头回正 */
static void
galaxyupdatereturn(double u)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyStar *s;
    GalaxyCore *g;
    GalaxyVec ctrl, d;
    GalaxyMat id = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    double F = r->cam.focal, e = galaxyeaseinoutcubic(galaxyphase(u, 0, .8)), fade = 1 - galaxysmoothstep(galaxyphase(u, 0, .45));
    double v, away;
    int i;

    galaxysetcameraat(galaxyscale(r->rctarget, 1 - e), galaxymix(r->rcdist, F, e), galaxymix(r->rcx, 0, e), galaxymix(r->rcy, 0, e), galaxymix(r->rcz, 0, e));
    for (i = 0; i < r->ntags; i++) {
        g = &r->galaxies[i];
        g->pos = g->rpos;
        g->alpha = g->ralpha * fade;
        g->p = galaxyproject(g->pos);
    }
    r->ringalpha = r->rring * fade;
    galaxyupdaterings(0);
    r->clusteralpha = r->rcluster * fade;
    r->streakalpha = r->rstreak * fade;
    r->sunalpha = r->rsun * fade;
    r->rippleamp = r->sunpulse = 0;
    galaxyupdatecluster(0);
    for (i = 0; i < r->nstars; i++) {
        s = &r->stars[i];
        if (s->back) {
            v = galaxyeaseinoutcubic(galaxyphase(u, s->delay, .8 + s->delay * .5));
            d = galaxysub(s->home, s->rpos);
            /* 控制点: 中点向镜头拉近, 并沿 (行进方向 x 视线) 侧偏, 轨迹是一条 3D 弧线 */
            ctrl = galaxyadd(galaxyscale(galaxyadd(s->rpos, s->home), .5),
                    galaxyadd(galaxyscale(galaxycross(d, galaxyv(0, 0, 1)), -.15), galaxyv(0, 0, -.1 * F)));
            s->pos = galaxybezier(s->rpos, ctrl, s->home, v);
            s->orient = galaxyblend(s->rorient, id, v);
            s->size = galaxymix(s->rsize, 1, v);
            s->alpha = 1;
            s->vis = galaxymix(s->rvis, 1, galaxysmoothstep(galaxyphase(u, 0, .5)));
            s->tint = s->rtint * (1 - v);
            s->glow = s->rglow * (1 - galaxysmoothstep(galaxyphase(u, 0, .6)));
            s->brightness = galaxymix(s->rbright, 1, v);
        } else {
            away = galaxyeaseincubic(galaxyphase(u, 0, .6));
            s->pos = galaxyadd(s->rpos, galaxyscale(galaxynormalize(galaxysub(s->rpos, r->rcampos)), 2 * F * away));
            s->orient = s->rorient;
            s->size = s->rsize;
            s->alpha = 1 - galaxysmoothstep(galaxyphase(u, 0, .55));
            s->vis = s->rvis * s->alpha;
            s->tint = s->rtint * s->alpha;
            s->glow = s->rglow * s->alpha;
            s->brightness = s->rbright;
        }
        s->p = galaxyproject(s->pos);
    }
    for (i = 0; i < r->ndust; i++)
        r->dust[i].p = galaxyproject(galaxydustat(&r->dust[i], r->motion));
    r->dustfade = r->rdust * fade;
    r->bright = galaxymix(r->rbright, 1, galaxyeaseinoutcubic(galaxyphase(u, .1, .85)));
    r->space = r->rspace * (1 - galaxysmoothstep(galaxyphase(u, 0, .75)));
    r->vign = r->rvign * (1 - galaxysmoothstep(galaxyphase(u, 0, .7)));
    r->desk = r->rdesk * (1 - galaxysmoothstep(galaxyphase(u, 0, .3)));
    /* 最后 20%: 回到原 tag 时交叉淡入开始时截的桌面 (结束画面与真实桌面一致), 否则只淡入状态栏 */
    r->deskover = r->fulldesk ? galaxysmoothstep(galaxyphase(u, .8, 1)) : 0;
    r->bar = r->fulldesk ? 0 : galaxysmoothstep(galaxyphase(u, .75, 1));
    r->reveal = r->trailgain = r->central = r->pulse = 0;
    galaxysortdepth();
}

/* 点击: 镜头停住. 窗口朝镜头拉近并正对镜头; 核心只变亮, 周围淡掉. 都不飞回桌面坐标. */
static void
galaxyupdatepick(double u)
{
    GalaxyScene *r = &galaxyscene;
    GalaxyStar *s, *pick = NULL;
    GalaxyCore *g;
    GalaxyVec tocam;
    GalaxyProj pj;
    double ack = galaxysmoothstep(galaxyphase(u, 0, .12 / GALAXYPICK));
    double push = galaxyeaseinoutcubic(galaxyphase(u, .12 / GALAXYPICK, .38 / GALAXYPICK));
    double ck = galaxysmoothstep(galaxyphase(u, .38 / GALAXYPICK, 1));
    double fade, dist, travel, endsize;
    int i, samering;

    if (r->rkind == GalaxyPickCore) {
        ack = push = 0;
        ck = galaxyeaseinoutcubic(u);
    }
    fade = r->rkind == GalaxyPickCore ? ck : push;
    if (r->rkind == GalaxyPickStar && r->rstar >= 0 && r->rstar < r->nstars)
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
                tocam = galaxysub(r->cam.pos, s->rpos);
                dist = galaxylen(tocam);
                travel = MAX(0, dist - r->cam.near * 2.5);
                s->pos = galaxyadd(s->rpos, galaxyscale(galaxynormalize(tocam), travel * push));
                s->orient = galaxyblend(s->rorient, r->cam.rot, push);
                pj = galaxyproject(s->pos);
                if (pj.ok && pj.scale > 1e-6) {
                    endsize = .75 * r->vw / (MAX(s->w, s->h) * pj.scale);
                    endsize = MAX(.02, MIN(40, endsize));
                    s->size = galaxymix(s->rsize, endsize, push);
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
            if (fade > 0 && r->rkind == GalaxyPickStar) {
                tocam = galaxysub(s->rpos, r->cam.pos);
                s->pos = galaxyadd(s->rpos, galaxyscale(galaxynormalize(tocam), 1.4 * r->cam.focal * fade));
            }
        }
        s->p = galaxyproject(s->pos);
    }
    for (i = 0; i < r->ntags; i++) {
        g = &r->galaxies[i];
        g->pos = g->rpos;
        if (r->rkind == GalaxyPickCore && i == r->rcore) {
            g->alpha = MIN(1, g->ralpha + .35 * ck);
            g->size = r->rcoresize * (1 + .7 * ck);
            g->hover = ck;
        } else {
            g->alpha = g->ralpha * (1 - fade);
            g->hover = 0;
        }
        g->p = galaxyproject(g->pos);
    }
    r->ringalpha = r->rring * (1 - fade);
    galaxyupdaterings(0);
    r->clusteralpha = r->rcluster * (1 - fade);
    r->streakalpha = r->rstreak * (1 - fade);
    r->sunalpha = r->rsun * (1 - fade);
    r->rippleamp = r->sunpulse = 0;
    galaxyupdatecluster(0);
    for (i = 0; i < r->ndust; i++)
        r->dust[i].p = galaxyproject(galaxydustat(&r->dust[i], r->motion));
    r->dustfade = r->rdust * (1 - fade);
    r->bright = r->rbright * (r->rkind == GalaxyPickStar && r->fulldesk ? 1 : (1 - .85 * ck));
    r->vign = r->rvign;
    r->desk = r->rdesk;
    r->deskover = r->rkind == GalaxyPickStar && r->fulldesk ? ck : 0;
    r->bar = r->reveal = r->trailgain = r->central = r->pulse = 0;
    galaxysortdepth();
}
