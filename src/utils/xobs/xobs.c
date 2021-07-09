/* main() for xobs.
 * (C) 1998 Elwood Charles Downey
 * we offer direct control, as well as organized delegation to supporting
 * processes.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <X11/Shell.h>
#include <Xm/BulletinB.h>
#include <Xm/CascadeB.h>
#include <Xm/DrawingA.h>
#include <Xm/FileSB.h>
#include <Xm/Form.h>
#include <Xm/Frame.h>
#include <Xm/Label.h>
#include <Xm/MainW.h>
#include <Xm/MessageB.h>
#include <Xm/PushB.h>
#include <Xm/RowColumn.h>
#include <Xm/ScrollBar.h>
#include <Xm/SelectioB.h>
#include <Xm/Separator.h>
#include <Xm/TextF.h>
#include <Xm/ToggleB.h>
#include <Xm/Xm.h>

#include "P_.h"
#include "astro.h"
#include "circum.h"
#include "cliserv.h"
#include "configfile.h"
#include "misc.h"
#include "running.h"
#include "strops.h"
#include "telenv.h"
#include "telstatshm.h"
#include "xobs.h"
#include "xtools.h"

/* global data */
Widget toplevel_w;
XtAppContext app;
char myclass[] = "XObs";
TelStatShm *telstatshmp;
Obj sunobj, moonobj;
int xobs_alone;

#define SHMPOLL_PERIOD 100 /* statshm polling period, ms */

static void chkDaemon(char *name, char *fifo, int required, int to);
static void initShm(void);
static void onsig(int sn);
static void periodic_check(void);

static char *progname;

static XrmOptionDescRec options[] = {
    {"-q", ".quiet", XrmoptionIsArg, NULL},
};

int main(int ac, char *av[])
{
    int i;
    char *telescoped = "telescoped";

    progname = basenm(av[0]);

    /* connect to log file */
    telOELog(progname);

    /* see whether we are alone */
    xobs_alone = lock_running(progname) == 0;

    /* connect to X server */
    toplevel_w = XtVaAppInitialize(&app, myclass, options, XtNumber(options), &ac, av, fallbacks, XmNallowShellResize,
                                   False, XmNiconName, myclass, NULL);

    /* secret switch: turn off confirms and tips if silent */
    if (getXRes(toplevel_w, "quiet", NULL))
    {
        tip_seton(0);
        rusure_seton(0);
    }

    /* handle some signals */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, onsig);
    signal(SIGINT, onsig);
    signal(SIGQUIT, onsig);
    signal(SIGBUS, onsig);
    signal(SIGSEGV, onsig);
    signal(SIGHUP, onsig);

    /* init stuff */
    for (i = 1; i < ac; i++)
        if (strcmp(av[i], "-v") == 0)
            telescoped = "telescoped -v";

    chkDaemon(telescoped, "Tel", 1, 60); /*long for csimcd stale socket*/
    initCfg();
    initShm();
    mkGUI();

    /* connect fifos if alone and no telrun running */
    if (xobs_alone)
        initPipes();

    /* start a periodic timer */
    periodic_check();

    /* up */
    XtRealizeWidget(toplevel_w);

    if (!xobs_alone)
    {
        msg("Another xobs is running -- this one will remain forever passive.");
        guiSensitive(0);
    }

    /* go */
    msg("Welcome, telescope user.");
    XtAppMainLoop(app);

    printf("%s: XtAppMainLoop() returned ?!\n", progname);
    return (1); /* for lint */
}

void die()
{
    unlock_running(progname, 0);
    exit(0);
}

static void initShm()
{
    int shmid;
    long addr;

    shmid = shmget(TELSTATSHMKEY, sizeof(TelStatShm), 0);
    if (shmid < 0)
    {
        perror("shmget TELSTATSHMKEY");
        unlock_running(progname, 0);
        exit(1);
    }

    addr = (long)shmat(shmid, (void *)0, 0);
    if (addr == -1)
    {
        perror("shmat TELSTATSHMKEY");
        unlock_running(progname, 0);
        exit(1);
    }

    telstatshmp = (TelStatShm *)addr;
}

/* start the given daemon on the given channel if not already running.
 * wait for connect to work up to 'to' secs.
 * die if required, else ignore.
 * N.B. this is *not* where we build the permanent fifo connection.
 */
static void chkDaemon(char *dname, char *fifo, int required, int to)
{
    char buf[1024];
    int fd[2];
    int i;

    /* ok if responding to lock */
    if (testlock_running(dname) == 0)
        return;

    /* nope. execute it, via rund */
    sprintf(buf, "rund %s", dname);
    if (system(buf) != 0)
    {
        if (required)
        {
            daemonLog("Can not %s\n", buf);
            exit(1);
        }
        else
            return;
    }

    /* give it a few seconds to build the fifo */
    for (i = 0; i < to; i++)
    {
        sleep(1);
        if (cli_conn(fifo, fd, buf) == 0)
        {
            /* ok, it's running, that's all we need to know */
            (void)close(fd[0]);
            (void)close(fd[1]);
            return;
        }
    }

    /* no can do if get here */
    if (required)
    {
        daemonLog("Can not connect to %s's fifos: %s\n", dname, buf);
        exit(1);
    }
}

static void onsig(int sn)
{
    die();
}

static void periodic_check()
{
    updateStatus(0);
    XtAppAddTimeOut(app, SHMPOLL_PERIOD, (XtTimerCallbackProc)periodic_check, 0);
}
