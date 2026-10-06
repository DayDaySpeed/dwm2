/* CPU 色温: 星的颜色跟随窗口进程的 CPU 占用 (冷蓝 -> 暖白 -> 橙红).
 * 窗口的进程号来自 _NET_WM_PID; 占用按进程树汇总 (终端里跑的程序算在终端窗口上).
 * /proc 每帧最多读 40 个条目, 一轮扫完 (约 2s 一轮) 再汇总, 不会让某一帧卡住 */
#include <dirent.h>
#include <fcntl.h>

#define GALAXYHEATBATCH 40

static void
galaxyheatpids(void)
{
    GalaxyScene *r = &galaxyscene;
    Atom pidatom = XInternAtom(dpy, "_NET_WM_PID", False), type;
    unsigned long n, after;
    unsigned char *p;
    int i, format;

    for (i = 0; i < r->nstars; i++) {
        p = NULL;
        r->stars[i].pid = 0;
        if (XGetWindowProperty(dpy, r->stars[i].win, pidatom, 0, 1, False, XA_CARDINAL,
                &type, &format, &n, &after, &p) == Success && p) {
            if (n == 1 && format == 32)
                r->stars[i].pid = (int)*(unsigned long *)p;
            XFree(p);
        }
    }
}

/* /proc/<pid>/stat: 括号里的进程名可能含空格, 从最后一个 ')' 之后解析 ppid 和 utime + stime + cutime + cstime */
static int
galaxyheatread(const char *pid, struct GalaxyProc *out)
{
    char path[64], buf[512], *q;
    unsigned long long ut, st;
    long long cut, cst;
    int fd, n, ppid;

    snprintf(path, sizeof path, "/proc/%s/stat", pid);
    if ((fd = open(path, O_RDONLY)) < 0)
        return 0;
    n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = 0;
    if (!(q = strrchr(buf, ')'))
            || sscanf(q + 1, " %*c %d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu %lld %lld", &ppid, &ut, &st, &cut, &cst) != 5)
        return 0;
    out->pid = atoi(pid);
    out->ppid = ppid;
    out->t = ut + st + (unsigned long long)MAX(0, cut) + (unsigned long long)MAX(0, cst);
    return 1;
}

static int
galaxyproccmp(const void *a, const void *b)
{
    return ((const struct GalaxyProc *)a)->pid - ((const struct GalaxyProc *)b)->pid;
}

/* 一轮扫完: 每个进程沿 ppid 往上找, 遇到窗口的进程号就把时间加到那个窗口上 */
static void
galaxyheatsum(double now)
{
    GalaxyScene *r = &galaxyscene;
    struct GalaxyProc key, *pp;
    unsigned long long total[256] = {0};
    double span = now - r->heatlast, hz = sysconf(_SC_CLK_TCK);
    int i, j, depth, pid, n = MIN(r->nstars, 256);

    qsort(r->procs, r->nprocs, sizeof *r->procs, galaxyproccmp);
    for (i = 0; i < r->nprocs; i++)
        for (pid = r->procs[i].pid, depth = 0; pid > 1 && depth < 32; depth++) {
            for (j = 0; j < n; j++)
                if (r->stars[j].pid == pid)
                    total[j] += r->procs[i].t;
            key.pid = pid;
            if (!(pp = bsearch(&key, r->procs, r->nprocs, sizeof *r->procs, galaxyproccmp)))
                break;
            pid = pp->ppid;
        }
    for (j = 0; j < n; j++) {
        if (r->stars[j].pid && r->stars[j].cpuprev && r->heatlast > 0 && span > .2 && hz > 0)
            r->stars[j].heatt = MAX(0, MIN(4, (double)(total[j] - MIN(total[j], r->stars[j].cpuprev)) / hz / span));
        r->stars[j].cpuprev = total[j];
    }
    r->heatlast = now;
}

/* 每帧推进一点扫描; 驻留时才扫, 安静模式不扫 */
static void
galaxyheatstep(double now)
{
    GalaxyScene *r = &galaxyscene;
    struct dirent *de;
    double t0;
    int n = 0;

    if (r->quiet || r->mode != GalaxyOrbit || r->fakestep > 0)
        return;
    t0 = galaxynow();
    if (!r->heatdir) {
        if (now - r->heatstart < 2)
            return;
        if (!(r->heatdir = opendir("/proc")))
            return;
        r->heatstart = now;
        r->nprocs = 0;
    }
    while (n < GALAXYHEATBATCH && (de = readdir((DIR *)r->heatdir))) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9')
            continue;
        n++;
        if (r->nprocs == r->cprocs) {
            struct GalaxyProc *grown = realloc(r->procs, (r->cprocs ? r->cprocs * 2 : 512) * sizeof *r->procs);
            if (!grown)
                break;
            r->procs = grown;
            r->cprocs = r->cprocs ? r->cprocs * 2 : 512;
        }
        r->nprocs += galaxyheatread(de->d_name, &r->procs[r->nprocs]);
    }
    if (n < GALAXYHEATBATCH) {     /* 读到目录末尾: 汇总 */
        closedir((DIR *)r->heatdir);
        r->heatdir = NULL;
        galaxyheatsum(now);
    }
    r->heatcost += galaxynow() - t0;
}

static void
galaxyheatfree(void)
{
    GalaxyScene *r = &galaxyscene;

    if (r->heatdir)
        closedir((DIR *)r->heatdir);
    free(r->procs);
    r->heatdir = NULL;
    r->procs = NULL;
    r->nprocs = r->cprocs = 0;
}
