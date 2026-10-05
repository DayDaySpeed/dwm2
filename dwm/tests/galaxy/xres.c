/* dwm (由 _NET_SUPPORTING_WM_CHECK 窗口确定) 在 X 服务器上占用的资源数.
 * 系统没有 libXRes, 直接发 X-Resource 扩展请求 (QueryClients=1, QueryClientResources=2, QueryClientPixmapBytes=3) */
#include <X11/Xlibint.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <string.h>

typedef struct { CARD8 reqType, minor; CARD16 length; } ReqNoArg;
typedef struct { CARD8 reqType, minor; CARD16 length; CARD32 xid; } ReqXid;

int
main(void)
{
    Display *d = XOpenDisplay(NULL);
    int op, ev, er, f, i, n;
    Atom t;
    unsigned long ni, ba;
    unsigned char *p = NULL;
    Window w = 0;
    xGenericReply rep;
    CARD32 *buf;
    long tot = 0, win = 0, pix = 0, pic = 0, gc = 0;
    unsigned long bytes = 0;

    if (!d || !XQueryExtension(d, "X-Resource", &op, &ev, &er))
        return 1;
    if (XGetWindowProperty(d, DefaultRootWindow(d), XInternAtom(d, "_NET_SUPPORTING_WM_CHECK", 0), 0, 1, 0, XA_WINDOW,
                &t, &f, &ni, &ba, &p) == Success && p)
        w = *(Window *)p;
    /* QueryClients */
    LockDisplay(d);
    {
        ReqNoArg *req = (ReqNoArg *)_XGetRequest(d, 1, sizeof *req);
        req->reqType = op; req->minor = 1; req->length = 1;
    }
    if (!_XReply(d, (xReply *)&rep, 0, xFalse)) { UnlockDisplay(d); return 1; }
    n = rep.data00;
    buf = Xmalloc(n * 8);
    _XRead(d, (char *)buf, n * 8);
    UnlockDisplay(d);
    for (i = 0; i < n; i++) {
        CARD32 base = buf[2 * i], mask = buf[2 * i + 1];
        if ((w & ~mask) != base)
            continue;
        LockDisplay(d);
        {
            ReqXid *req = (ReqXid *)_XGetRequest(d, 2, sizeof *req);
            req->reqType = op; req->minor = 2; req->length = 2; req->xid = base;
        }
        if (_XReply(d, (xReply *)&rep, 0, xFalse)) {
            int nt = rep.data00, k;
            CARD32 *ty = Xmalloc(nt * 8);
            _XRead(d, (char *)ty, nt * 8);
            UnlockDisplay(d);
            for (k = 0; k < nt; k++) {
                char *nm = XGetAtomName(d, ty[2 * k]);
                tot += ty[2 * k + 1];
                if (!strcmp(nm, "WINDOW")) win = ty[2 * k + 1];
                else if (!strcmp(nm, "PIXMAP")) pix = ty[2 * k + 1];
                else if (!strcmp(nm, "PICTURE")) pic = ty[2 * k + 1];
                else if (!strcmp(nm, "GC")) gc = ty[2 * k + 1];
                XFree(nm);
            }
            Xfree(ty);
        } else
            UnlockDisplay(d);
        LockDisplay(d);
        {
            ReqXid *req = (ReqXid *)_XGetRequest(d, 3, sizeof *req);
            req->reqType = op; req->minor = 3; req->length = 2; req->xid = base;
        }
        if (_XReply(d, (xReply *)&rep, 0, xFalse))
            bytes = rep.data00;
        UnlockDisplay(d);
        printf("windows %ld pixmaps %ld pictures %ld gcs %ld total %ld pixmap_bytes %lu\n", win, pix, pic, gc, tot, bytes);
    }
    return 0;
}
