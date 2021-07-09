/* main dispatch and execution functions for the mount itself. */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "P_.h"
#include "astro.h"
#include "circum.h"
#include "cliserv.h"
#include "configfile.h"
#include "csimc.h"
#include "misc.h"
#include "running.h"
#include "strops.h"
#include "telenv.h"
#include "telstatshm.h"
#include "virmc.h"

#include "teled.h"

/* handy loop "for each motor" we deal with here */
#define FEM(p) for ((p) = HMOT; (p) <= RMOT; (p)++)
#define NMOT (TEL_RM - TEL_HM + 1)

/* the current activity, if any */
static void (*active_func)(int first, ...);

/* one of these... */
static void tel_poll(void);
static void tel_reset(int first);
static void tel_home(int first, ...);
static void tel_limits(int first, ...);
void tel_stow(int first, ...);
static void tel_radecep(int first, ...);
static void tel_radeceod(int first, ...);
static void tel_op(int first, ...);
static void tel_altaz(int first, ...);
static void tel_hadec(int first, ...);
static void tel_stop(int first, ...);
static void tel_jog(int first, char jog_dir[]);
static void offsetTracking(int first, double harcsecs, double darcsecs, int report);

/* helped along by these... */
static int dbformat(char *msg, Obj *op, double *drap, double *ddecp);
static void initCfg(void);
static void hd2xyr(double ha, double dec, double *xp, double *yp, double *rp);
static void xyr2altaz(double x, double y, double r, double *alt, double *az);
static void readRaw(void);
static void mkCook(void);
static void dummyTarg(void);
static void stopTel(int fast);
static int onTarget(MotorInfo **mipp);
static int atTarget(void);
static int trackObj(Obj *op, int first);
static void findAxes(Now *np, Obj *op, double *xp, double *yp, double *rp);
static int chkLimits(int wrapok, double *xp, double *yp, double *rp);
static void jogTrack(int first, char dircode);
static void jogSlew(int first, char dircode);
static int checkAxes(void);
static char *sayWhere(double alt, double az);

/* config entries */
static double TRACKACC;    /* tracking accuracy, rads. 0 means 1 enc step*/
static double ACQUIREACC;  /* acquire accuracy, rads. 0 means 1 enc step*/
static double ACQUIREDELT; /* how far moved in 1sec before settled */
static double FGUIDEVEL;   /* fine jogging motion rate, rads/sec */
static double CGUIDEVEL;   /* coarse jogging motion rate, rads/sec */
static int TRACKINT;       /* tracking interval for each e/mtrack, secs */

#define PPTRACK 60 /* number of positions to e/mtrack */

/* offsets to apply to target object location, if any */
static double r_offset; /* delta ra to be added */
static double d_offset; /* delta dec to be added */

#define MAXJITTER 10.0 /* max clock vs host difference */
static double strack;  /* when current e/mtrack started */

int tel_ishomed(void);

/* called when we receive a message from the Tel fifo.
 * as well as regularly with !msg just to update things.
 */
/* ARGSUSED */
void tel_msg(msg) char *msg;
{
    double a, b, c;
    int i;
    char jog_dir[8];
    Obj o;

    /* dispatch -- stop by default */

    if (!msg)
        tel_poll();
    else if (strncasecmp(msg, "reset", 5) == 0)
        tel_reset(1);
    else if (strncasecmp(msg, "home", 4) == 0)
        tel_home(1, msg);
    else if (strncasecmp(msg, "limits", 6) == 0)
        tel_limits(1, msg);
    else if (strncasecmp(msg, "stow", 4) == 0)
        tel_stow(1, msg);
    else if (sscanf(msg, "RA:%lf Dec:%lf Epoch:%lf", &a, &b, &c) == 3)
        tel_radecep(1, a, b, c);
    else if (sscanf(msg, "RA:%lf Dec:%lf", &a, &b) == 2)
        tel_radeceod(1, a, b);
    else if (dbformat(msg, &o, &a, &b) == 0)
        tel_op(1, &o, a, b);
    else if (sscanf(msg, "Alt:%lf Az:%lf", &a, &b) == 2)
        tel_altaz(1, a, b);
    else if (sscanf(msg, "HA:%lf Dec:%lf", &a, &b) == 2)
        tel_hadec(1, a, b);
    else if (sscanf(msg, "j%7[NSEWnsew0]", jog_dir) == 1)
        tel_jog(1, jog_dir);
    else if (sscanf(msg, "Offset %lf,%lf", &a, &b) == 2)
        offsetTracking(1, a, b, 1);
    else
    {
        /* User-issued stop */
        tel_stop(1);
    }
}

/* no new messages.
 * goose the current objective, if any, else just update cooked position.
 */
static void tel_poll()
{
    if (virtual_mode)
    {
        MotorInfo *mip;
        FEM(mip)
        {
            if (mip->have)
                vmcService(mip->axis);
        }
    }
    if (active_func)
        (*active_func)(0);
    else
    {
        /* idle -- just update */
        readRaw();
        mkCook();
        dummyTarg();
    }
}

/* stop and reread config files */
static void tel_reset(int first)
{
    MotorInfo *mip;

    FEM(mip)
    {
        if (virtual_mode)
        {
            vmcReset(mip->axis);
        }
        else
        {
            if (mip->have)
                csiiClose(mip);
        }
    }

    initCfg();
    init_cfg();

    FEM(mip)
    {
        if (virtual_mode)
        {
            if (vmcSetup(mip->axis, mip->maxvel, mip->maxacc, mip->step, mip->esign))
            {
                mip->ishomed = 0;
            }
        }
        else
        {
            if (mip->have)
            {
                csiiOpen(mip);
                csiSetup(mip);
            }
        }
    }

    stopTel(0);
    active_func = NULL;
    fifoWrite(Tel_Id, 0, "Reset complete");
}

/* seek telescope axis home positions.. all or as per HDR */
static void tel_home(int first, ...)
{
    static int want[NMOT];
    static int nwant;
    MotorInfo *mip;
    int i;

    /* maintain just cpos and raw */
    readRaw();

    if (first)
    {
        char *msg;

        /* get the whole command */
        va_list ap;
        va_start(ap, first);
        msg = va_arg(ap, char *);
        va_end(ap);

        /* start fresh */
        stopTel(0);
        memset((void *)want, 0, sizeof(want));

        /* find which axes to do, or all if none specified */
        nwant = 0;
        if (strchr(msg, 'H'))
        {
            nwant++;
            want[TEL_HM] = 1;
        }
        if (strchr(msg, 'D'))
        {
            nwant++;
            want[TEL_DM] = 1;
        }
        if (strchr(msg, 'R'))
        {
            nwant++;
            want[TEL_RM] = 1;
        }
        if (!nwant)
        {
            nwant = 3;
            want[TEL_HM] = want[TEL_DM] = want[TEL_RM] = 1;
        }

        FEM(mip)
        {
            i = mip - &telstatshmp->minfo[0];
            if (want[i])
            {
                switch (axis_home(mip, Tel_Id, 1))
                {
                case -1:
                    /* abort all axes if any fail */
                    stopTel(1);
                    active_func = NULL;
                    return;
                case 1:
                    continue;
                case 0:
                    want[i] = 0;
                    nwant--;
                    break;
                }
            }
        }

        /* if get here, set new state */
        active_func = tel_home;
        telstatshmp->telstate = TS_HOMING;
        telstatshmp->telstateidx++;
    }

    /* continue to seek home on each axis still not done */
    FEM(mip)
    {
        i = mip - &telstatshmp->minfo[0];
        if (want[i])
        {
            switch (axis_home(mip, Tel_Id, 0))
            {
            case -1:
                /* abort all axes if any fail */
                stopTel(1);
                active_func = NULL;
                return;
            case 1:
                continue;
            case 0:
                fifoWrite(Tel_Id, 1, "Axis %d: home complete", mip->axis);
                want[i] = 0;
                nwant--;
                break;
            }
        }
    }

    /* really done when none left */
    if (!nwant)
    {
        telstatshmp->telstate = TS_STOPPED;
        telstatshmp->telstateidx++;
        active_func = NULL;
        fifoWrite(Tel_Id, 0, "Scope homing complete");
    }
}

/* find limit positions and H/D motor steps and signs.
 * N.B. set tax->h*lim as soon as we know TEL_HM limits.
 */
static void tel_limits(int first, ...)
{
    static int ishomed[NMOT];
    static int want[NMOT];
    static int nwant;
    MotorInfo *mip;
    int i;

    /* stay up to date */
    readRaw();
    mkCook();

    if (first)
    {
        char *msg;

        /* get the whole command */
        va_list ap;
        va_start(ap, first);
        msg = va_arg(ap, char *);
        va_end(ap);

        /* start fresh */
        stopTel(0);
        memset((void *)want, 0, sizeof(want));

        /* find which axes to do, or all if none specified */
        nwant = 0;
        if (strchr(msg, 'H'))
        {
            nwant++;
            want[TEL_HM] = 1;
        }
        if (strchr(msg, 'D'))
        {
            nwant++;
            want[TEL_DM] = 1;
        }
        if (strchr(msg, 'R'))
        {
            nwant++;
            want[TEL_RM] = 1;
        }
        if (!nwant)
        {
            nwant = 3;
            want[TEL_HM] = want[TEL_DM] = want[TEL_RM] = 1;
        }

        FEM(mip)
        {
            i = mip - &telstatshmp->minfo[0];

            /* Something in the limiting procedure resets this bit
             * Record the initial state so we can fix it afterwards
             */
            ishomed[i] = mip->ishomed;

            if (want[i])
            {
                switch (axis_limits(mip, Tel_Id, 1))
                {
                case -1:
                    /* abort all axes if any fail */
                    stopTel(1);
                    active_func = NULL;
                    return;
                case 1:
                    continue;
                case 0:
                    want[i] = 0;
                    nwant--;
                    break;
                }
            }
        }

        /* new state */
        active_func = tel_limits;
        telstatshmp->telstate = TS_LIMITING;
        telstatshmp->telstateidx++;
    }

    /* continue to seek limits on each axis still not done */
    FEM(mip)
    {
        i = mip - &telstatshmp->minfo[0];
        if (want[i])
        {
            switch (axis_limits(mip, Tel_Id, 0))
            {
            case -1:
                /* abort all axes if any fail */
                stopTel(1);
                active_func = NULL;
                return;
            case 1:
                continue;
            case 0:
                mip->cvel = 0;
                want[i] = 0;
                nwant--;
                break;
            }
        }
    }

    /* really done when none left */
    if (!nwant)
    {
        stopTel(0);

        initCfg(); /* read new limits */
        active_func = NULL;
        fifoWrite(Tel_Id, 0, "All Scope limits are complete.");

        /* restore the inital home state */
        FEM(mip)
        {
            i = mip - &telstatshmp->minfo[0];
            mip->ishomed = ishomed[i];
        }

        /* N.B. save TEL_HM limits in tax */
        telstatshmp->tax.hneglim = HMOT->neglim;
        telstatshmp->tax.hposlim = HMOT->poslim;
    }
}

/* Place the telescope in STOW position */
void tel_stow(int first, ...)
{
    char buf[128];

    allstop();

    fifoWrite(Tel_Id, 0, "Telescope stow underway");
    tel_altaz(1, STOWALT, STOWAZ);
}

/* handle tracking an astrometric position */
static void tel_radecep(int first, ...)
{
    static Obj o;

    if (first)
    {
        Now *np = &telstatshmp->now;
        Obj newo, *op = &newo;
        double Mjd, ra, dec, ep;
        va_list ap;

        /* fetch values */
        va_start(ap, first);
        ra = va_arg(ap, double);
        dec = va_arg(ap, double);
        ep = va_arg(ap, double);
        va_end(ap);

        /* fill in op */
        memset((void *)op, 0, sizeof(*op));
        op->f_RA = ra;
        op->f_dec = dec;
        year_mjd(ep, &Mjd);
        op->f_epoch = Mjd;
        op->o_type = FIXED;
        strcpy(op->o_name, "<Anon>");

        /* this is the new target */
        o = *op;
        active_func = tel_radecep;
        telstatshmp->telstate = TS_HUNTING;
        telstatshmp->telstateidx++;
        telstatshmp->jogging_ison = 0;
        r_offset = d_offset = 0;

        obj_cir(np, op); /* just for sayWhere */
    }

    if (trackObj(&o, first) < 0)
        active_func = NULL;
}

/* handle tracking an apparent position */
static void tel_radeceod(int first, ...)
{
    static Obj o;

    if (first)
    {
        Now *np = &telstatshmp->now;
        Obj newo, *op = &newo;
        double ra, dec;
        va_list ap;

        /* fetch values */
        va_start(ap, first);
        ra = va_arg(ap, double);
        dec = va_arg(ap, double);
        va_end(ap);

        /* fill in op */
        ap_as(np, J2000, &ra, &dec);
        memset((void *)op, 0, sizeof(*op));
        op->f_RA = ra;
        op->f_dec = dec;
        op->f_epoch = J2000;
        op->o_type = FIXED;
        strcpy(op->o_name, "<Anon>");

        /* this is the new target */
        o = *op;
        active_func = tel_radeceod;
        telstatshmp->telstate = TS_HUNTING;
        telstatshmp->telstateidx++;
        telstatshmp->jogging_ison = 0;
        r_offset = d_offset = 0;

        obj_cir(np, op); /* just for sayWhere */
    }

    if (trackObj(&o, first) < 0)
        active_func = NULL;
}

/* handle tracking an object */
static void tel_op(int first, ...)
{
    static Obj o;

    if (first)
    {
        Now *np = &telstatshmp->now;
        Obj *op;
        va_list ap;

        va_start(ap, first);
        op = va_arg(ap, Obj *);
        r_offset = va_arg(ap, double);
        d_offset = va_arg(ap, double);
        va_end(ap);

        /* this is the new target */
        o = *op;
        active_func = tel_op;
        telstatshmp->telstate = TS_HUNTING;
        telstatshmp->telstateidx++;
        telstatshmp->jogging_ison = 0;

        obj_cir(np, op); /* just for sayWhere */
    }

    if (trackObj(&o, first) < 0)
        active_func = NULL;
}

/* handle slewing to a horizon location */
static void tel_altaz(int first, ...)
{

    if (first)
    {
        Now *np = &telstatshmp->now;
        double pa, ra, lst, ha, dec;
        double alt, az;
        MotorInfo *mip;
        double x, y, r;
        va_list ap;

        /* gather target values */
        va_start(ap, first);
        alt = va_arg(ap, double);
        az = va_arg(ap, double);
        va_end(ap);

        /* find target axis positions once */
        aa_hadec(lat, alt, az, &ha, &dec);
        telstatshmp->jogging_ison = 0;
        r_offset = d_offset = 0;
        hd2xyr(ha, dec, &x, &y, &r);
        if (chkLimits(1, &x, &y, &r) < 0)
        {
            active_func = NULL;
            ;
            return; /* Tel_Id already informed */
        }

        /* set new state */
        telstatshmp->telstate = TS_SLEWING;
        telstatshmp->telstateidx++;
        active_func = tel_altaz;

        /* set new raw destination */
        HMOT->dpos = x;
        DMOT->dpos = y;
        RMOT->dpos = r;

        /* and new cooked destination just for prying eyes */
        telstatshmp->Dalt = alt;
        telstatshmp->Daz = az;
        telstatshmp->DAHA = ha;
        telstatshmp->DADec = dec;
        tel_hadec2PA(ha, dec, &telstatshmp->tax, lat, &pa);
        telstatshmp->DPA = pa;
        now_lst(np, &lst);
        ra = hrrad(lst) - ha;
        range(&ra, 2 * PI);
        telstatshmp->DARA = ra;
        ap_as(np, J2000, &ra, &dec);
        telstatshmp->DJ2kRA = ra;
        telstatshmp->DJ2kDec = dec;

        /* issue move command to each axis */
        FEM(mip)
        {
            if (mip->have)
            {

                // make sure we're homed to begin with
                char buf[128];
                if (axisHomedCheck(mip, buf))
                {
                    active_func = NULL;
                    stopTel(0);
                    fifoWrite(Tel_Id, -1, "Error: %s", buf);
                    return;
                }

                if (virtual_mode)
                {
                    vmcSetTargetPosition(mip->axis, mip->sign * mip->step * mip->dpos / (2 * PI));
                }
                else
                {
                    if (mip->haveenc)
                    {
                        csi_w(MIPCFD(mip), "etpos=%.0f;", mip->esign * mip->estep * mip->dpos / (2 * PI));
                    }
                    else
                    {
                        csi_w(MIPCFD(mip), "mtpos=%.0f;", mip->sign * mip->step * mip->dpos / (2 * PI));
                    }
                }
            }
        }
    }

    /* stay up to date */
    readRaw();
    mkCook();

    if (checkAxes() < 0)
    {
        stopTel(1);
        active_func = NULL;
    }

    if (atTarget() == 0)
    {
        stopTel(0);
        fifoWrite(Tel_Id, 0, "Slew complete");
        active_func = NULL;
    }
}

/* handle slewing to an equatorial location */
static void tel_hadec(int first, ...)
{
    if (first)
    {
        Now *np = &telstatshmp->now;
        double alt, az, pa, ra, lst, ha, dec;
        MotorInfo *mip;
        double x, y, r;
        va_list ap;

        /* gather params */
        va_start(ap, first);
        ha = va_arg(ap, double);
        dec = va_arg(ap, double);
        va_end(ap);

        /* find target axis positions once */
        telstatshmp->jogging_ison = 0;
        r_offset = d_offset = 0;
        hd2xyr(ha, dec, &x, &y, &r);
        if (chkLimits(1, &x, &y, &r) < 0)
        {
            active_func = NULL;
            ;
            return; /* Tel_Id already informed */
        }

        /* set new state */
        telstatshmp->telstate = TS_SLEWING;
        telstatshmp->telstateidx++;
        active_func = tel_hadec;

        /* set raw destination */
        HMOT->dpos = x;
        DMOT->dpos = y;
        RMOT->dpos = r;

        /* and cooked desination, just for enquiring minds */
        telstatshmp->DAHA = ha;
        telstatshmp->DADec = dec;
        tel_hadec2PA(ha, dec, &telstatshmp->tax, lat, &pa);
        hadec_aa(lat, ha, dec, &alt, &az);
        telstatshmp->DPA = pa;
        telstatshmp->Dalt = alt;
        telstatshmp->Daz = az;
        now_lst(np, &lst);
        ra = hrrad(lst) - ha;
        range(&ra, 2 * PI);
        telstatshmp->DARA = ra;
        ap_as(np, J2000, &ra, &dec);
        telstatshmp->DJ2kRA = ra;
        telstatshmp->DJ2kDec = dec;

        /* issue move command to each axis */
        FEM(mip)
        {
            if (mip->have)
            {

                // make sure we're homed to begin with
                char buf[128];
                if (axisHomedCheck(mip, buf))
                {
                    active_func = NULL;
                    stopTel(0);
                    fifoWrite(Tel_Id, -1, "Error: %s", buf);
                    return;
                }

                if (virtual_mode)
                {
                    vmcSetTargetPosition(mip->axis, mip->sign * mip->step * mip->dpos / (2 * PI));
                }
                else
                {
                    if (mip->haveenc)
                    {
                        csi_w(MIPCFD(mip), "etpos=%.0f;", mip->esign * mip->estep * mip->dpos / (2 * PI));
                    }
                    else
                    {
                        csi_w(MIPCFD(mip), "mtpos=%.0f;", mip->sign * mip->step * mip->dpos / (2 * PI));
                    }
                }
            }
        }
    }

    /* stay up to date */
    readRaw();
    mkCook();

    if (checkAxes() < 0)
    {
        stopTel(1);
        active_func = NULL;
    }

    if (atTarget() == 0)
    {
        stopTel(0);
        fifoWrite(Tel_Id, 0, "Slew complete");
        active_func = NULL;
    }
}

/* politely stop all axes */
static void tel_stop(int first, ...)
{
    MotorInfo *mip;

    if (first)
    {
        /* issue stops */
        stopTel(0);
        active_func = tel_stop;
    }

    /* wait for all to be stopped */
    FEM(mip)
    {
        if (mip->have)
        {
            if (virtual_mode)
            {
                if (vmcGetVelocity(mip->axis) != 0)
                    return;
            }
            else
            {
                if (csi_rix(MIPCFD(mip), "=mvel;") != 0)
                    return;
            }
        }
    }

    /* if get here, everything has stopped */
    telstatshmp->telstate = TS_STOPPED;
    telstatshmp->telstateidx++;
    active_func = NULL;
    fifoWrite(Tel_Id, 0, "Stop complete");
    readRaw();
}

/* respond to a request for jogging.
 */
static void tel_jog(int first, char jog_dir[])
{
    if (telstatshmp->telstate == TS_TRACKING)
        jogTrack(first, jog_dir[0]);
    else
        jogSlew(first, jog_dir[0]);
}

/* aux support functions */

/* dig out a message with a db line in it.
 * may optionally be preceded by "dRA:x dDec:y #".
 * return 0 if ok, else -1.
 * N.B. always set dra/ddec.
 */
static int dbformat(char *msg, Obj *op, double *drap, double *ddecp)
{
    if (sscanf(msg, "dRA:%lf dDec:%lf #", drap, ddecp) == 2)
        msg = strchr(msg, '#') + 1;
    else
        *drap = *ddecp = 0.0;

    return (db_crack_line(msg, op, NULL));
}

/* build and load an e/mtrack sequence for op.
 * time starts at np. it is ok to modify np->n_mjd.
 * N.B. we assume clocks have been set to 0
 */
static void buildTrack(Now *np, Obj *op)
{
    double *x, *y, *r;
    double *xyr[NMOT];
    double mjd0;
    MotorInfo *mip;
    int i;

    /* malloc each then store so we can effectively access them via a mip */
    x = (double *)malloc(PPTRACK * sizeof(double));
    y = (double *)malloc(PPTRACK * sizeof(double));
    r = (double *)malloc(PPTRACK * sizeof(double));
    xyr[TEL_HM] = x;
    xyr[TEL_DM] = y;
    xyr[TEL_RM] = r;

    /* build list of PPTRACK values beginning at mjd */
    mjd0 = mjd;
    for (i = 0; i < PPTRACK; i++)
    {
        mjd = mjd0 + i * TRACKINT / (PPTRACK * SPD);
        findAxes(np, op, &x[i], &y[i], &r[i]);
        (void)chkLimits(1, &x[i], &y[i], &r[i]); /* let limit protect */
    }

    /* send to each controller */
    FEM(mip)
    {
        double scale;
        double *xyrp;
        int cfd;

        if (!mip->have)
            continue;

        if (virtual_mode)
        {

            xyrp = xyr[mip - telstatshmp->minfo];
            //	    tdlog ("Creating track profile:");
            vmcSetTrackPath(mip->axis, PPTRACK, 0, 1000.0 * TRACKINT / PPTRACK + 0.5, xyrp);
        }
        else
        {

            xyrp = xyr[mip - telstatshmp->minfo];
            cfd = MIPCFD(mip);
            //	    tdlog ("Creating track profile:");
            if (mip->haveenc)
            {
                scale = mip->esign * mip->estep / (2 * PI);
                csi_w(cfd, "etrack");
                //		printf ("etrack");
            }
            else
            {
                scale = mip->sign * mip->step / (2 * PI);
                csi_w(cfd, "mtrack");
                //		printf ("mtrack");
            }
            csi_w(cfd, "(0,%.0f", 1000. * TRACKINT / PPTRACK + .5);
            //	    printf ("(0,%.0f", 1000.*TRACKINT/PPTRACK+.5);

            /* TODO: pack into longer commands */
            for (i = 0; i < PPTRACK; i++)
            {
                csi_w(cfd, ",%.0f", scale * xyrp[i] + .5);
                //		printf (",%.0f", scale*xyrp[i]+.5);
            }
            csi_w(cfd, ");");
            //	    printf (");\n");

        } // !virtual_mode
    }
    fflush(stdout);

    /* done */
    free((void *)x);
    free((void *)y);
    free((void *)r);
}

/* if first or TRACKINT has expired and needs refreshed compute and load a new
 *   tracking profile.
 * also always handle jogginf, limit checks, telstat info, whether on track.
 * return -1 when tracking is just not possible, 0 when ok to keep trying.
 */
static int trackObj(Obj *op, int first)
{
    Now *np = &telstatshmp->now; /* pointer to live one */
    Now now = telstatshmp->now;  /* stable and changeable copy */
    double ra, dec, lst, ha;
    double x, y, r;
    int clocknow;
    MotorInfo *mip;

    /* download tracking profile if new or expired */
    if (first || mjd > strack + TRACKINT / SPD)
    {
        /* sync all clocks to 0 */
        /* N.B. use MIPSFD to insure precedes main loop clock reads */
        FEM(mip)
        {
            if (mip->have)
            {
                if (virtual_mode)
                {
                    vmcResetClock(mip->axis);
                }
                else
                {
                    csi_w(MIPSFD(mip), "clock=0;");
                }
            }
        }

        /* record when this TRACKINT began */
        strack = now.n_mjd;

        /* set all timeouts to TRACKINT */
        FEM(mip)
        {
            if (mip->have)
            {
                if (virtual_mode)
                {
                    vmcSetTimeout(mip->axis, TRACKINT * 1000);
                }
                else
                {
                    csi_w(MIPSFD(mip), "timeout=%d;", TRACKINT * 1000);
                }
            }
        }

        /* if just starting, reset any lingering track offset */
        if (first)
        {
            FEM(mip)
            {
                if (mip->have)
                {
                    // make sure we're homed to begin with
                    char buf[128];
                    if (axisHomedCheck(mip, buf))
                    {
                        active_func = NULL;
                        stopTel(0);
                        fifoWrite(Tel_Id, -1, "Error: %s", buf);
                        return -1;
                    }
                    if (virtual_mode)
                    {
                        vmcSetTrackingOffset(mip->axis, 0);
                    }
                    else
                    {
                        csi_w(MIPSFD(mip), "toffset=0;");
                    }
                }
            }
        }

        /* now build and install tracking profiles */
        buildTrack(&now, op);
    }

    /* quick, get current value of typical clock.
     * use this to compute desired to avoid host computer time jitter
     */
    mip = HMOT->have ? HMOT : DMOT; /* surely we have one ! */
    if (virtual_mode)
    {
        clocknow = vmcGetClock(mip->axis);
    }
    else
    {
        clocknow = csi_rix(MIPSFD(mip), "=clock;");
    }

    /* update actual position info */
    readRaw();
    mkCook();

    /* check axes */
    if (checkAxes() < 0)
    {
        stopTel(1);
        return (-1);
    }

    /* find desired topocentric apparent place and axes @ clocknow */
    now.n_mjd = strack + clocknow / (SPD * 1000.);
    x = fabs(mjd - now.n_mjd) * SPD;
    if (x > MAXJITTER)
    {
        fifoWrite(Tel_Id, -5, "Motion controller clock drift exceeds %g sec: %g", MAXJITTER, x);
        fifoWrite(Tel_Id, -5, "clocknow=%d. strack=%g", clocknow, strack);
        stopTel(0);
        return (-1);
    }
    findAxes(&now, op, &x, &y, &r);
    if (chkLimits(1, &x, &y, &r) < 0)
    {
        stopTel(0);
        return (-1);
    }
    telstatshmp->Dalt = op->s_alt;
    telstatshmp->Daz = op->s_az;
    telstatshmp->DARA = ra = op->s_ra;
    telstatshmp->DADec = dec = op->s_dec;
    now_lst(&now, &lst);
    ha = hrrad(lst) - ra;
    haRange(&ha);
    telstatshmp->DAHA = ha;
    ap_as(&now, J2000, &ra, &dec);
    telstatshmp->DJ2kRA = ra;
    telstatshmp->DJ2kDec = dec;
    HMOT->dpos = x;
    DMOT->dpos = y;
    RMOT->dpos = r;

    /* check progress, revert to hunting if lose track */
    switch (telstatshmp->telstate)
    {
    case TS_HUNTING:
        if (atTarget() == 0)
        {
            fifoWrite(Tel_Id, 3, "All axes have tracking lock");
            fifoWrite(Tel_Id, 0, "Now tracking");
            telstatshmp->telstate = TS_TRACKING;
            telstatshmp->telstateidx++;
        }
        break;
    case TS_TRACKING:
        if (!telstatshmp->jogging_ison && onTarget(&mip) < 0)
        {
            fifoWrite(Tel_Id, 4, "Axis %d lost tracking lock", mip->axis);
            telstatshmp->telstate = TS_HUNTING;
            telstatshmp->telstateidx++;
        }
        break;

    default:
        break;
    }

    /* ok */
    return (0);
}

/* compute axes for op at np, including fixed schedule offsets if any.
 * return 0 if ok, -1 if exceeds limits
 * N.B. o_type of *op may be different upon return.
 */
static void findAxes(Now *np, Obj *op, double *xp, double *yp, double *rp)
{
    double ha, dec;
    Obj fobj;

    if (r_offset || d_offset)
    {
        /* find offsets to op as a fixed object */
        double ra, dec;

        epoch = J2000;
        obj_cir(np, op);
        ra = op->s_ra;
        dec = op->s_dec;

        /* apply offsets */
        ra += r_offset;
        dec += d_offset;

        op = &fobj;
        op->o_type = FIXED;
        op->f_RA = ra;
        op->f_dec = dec;
        op->f_epoch = J2000;
    }

    epoch = EOD;
    obj_cir(np, op);
    aa_hadec(lat, op->s_alt, op->s_az, &ha, &dec);
    hd2xyr(ha, dec, xp, yp, rp);
}

/* convert an ha/dec to scope x/y/r, allowing for mesh corrections.
 * in many ways, this is the reverse of mkCook().
 */
static void hd2xyr(double ha, double dec, double *xp, double *yp, double *rp)
{
    TelAxes *tap = &telstatshmp->tax;
    double mdha, mddec;
    double x, y, r;

    tel_mount_cor(ha, dec, &mdha, &mddec);
    ha += mdha;
    dec += mddec;
    hdRange(&ha, &dec);

    tel_hadec2xy(ha, dec, tap, &x, &y);
    tel_ideal2realxy(tap, &x, &y);
    if (RMOT->have)
    {
        Now *np = &telstatshmp->now;
        tel_hadec2PA(ha, dec, tap, lat, &r);
        r += tap->R0 * RMOT->sign;
    }
    else
        r = 0;

    *xp = x;
    *yp = y;
    *rp = r;
}

static void xyr2altaz(double x, double y, double r, double *alt, double *az)
{
    Now *np = &telstatshmp->now;
    TelAxes *tap = &telstatshmp->tax;
    double ha, dec, mdha, mddec;

    /* back out non-ideal axes info */
    tel_realxy2ideal(tap, &x, &y);

    /* convert encoders to apparent ha/dec */
    tel_xy2hadec(x, y, tap, &ha, &dec);

    /* back out the mesh corrections */
    tel_mount_cor(ha, dec, &mdha, &mddec);
    ha -= mdha;
    dec -= mddec;
    hdRange(&ha, &dec);

    /* find horizon coords */
    hadec_aa(lat, ha, dec, alt, az);
}

/* using the raw values, compute the astro position.
 * in many ways, this is the reverse of hd2xyz().
 */
static void mkCook()
{
    Now *np = &telstatshmp->now;
    TelAxes *tap = &telstatshmp->tax;
    double lst, ra, ha, dec, alt, az;
    double mdha, mddec;
    double x, y, r;

    /* handy axis values */
    x = HMOT->cpos;
    y = DMOT->cpos;
    r = RMOT->cpos;

    /* back out non-ideal axes info */
    tel_realxy2ideal(tap, &x, &y);

    /* convert encoders to apparent ha/dec */
    tel_xy2hadec(x, y, tap, &ha, &dec);

    /* back out the mesh corrections */
    tel_mount_cor(ha, dec, &mdha, &mddec);
    telstatshmp->mdha = mdha;
    telstatshmp->mddec = mddec;
    ha -= mdha;
    dec -= mddec;
    hdRange(&ha, &dec);

    /* find horizon coords */
    hadec_aa(lat, ha, dec, &alt, &az);
    telstatshmp->Calt = alt;
    telstatshmp->Caz = az;

    /* find apparent equatorial coords */
    unrefract(pressure, temp, alt, &alt);
    aa_hadec(lat, alt, az, &ha, &dec);
    now_lst(np, &lst);
    lst = hrrad(lst);
    ra = lst - ha;
    range(&ra, 2 * PI);
    telstatshmp->CARA = ra;
    telstatshmp->CAHA = ha;
    telstatshmp->CADec = dec;
    telstatshmp->Clst = lst;

    /* find J2000 astrometric equatorial coords */
    ap_as(np, J2000, &ra, &dec);
    telstatshmp->CJ2kRA = ra;
    telstatshmp->CJ2kDec = dec;

    /* find position angle */
    tel_hadec2PA(ha, dec, tap, lat, &r);
    telstatshmp->CPA = r;
}

/* read the raw values */
static void readRaw()
{
    MotorInfo *mip;

    FEM(mip)
    {
        if (!mip->have)
            continue;

        if (virtual_mode)
        {
            mip->raw = vmcGetPosition(mip->axis);
            mip->cpos = (2 * PI) * mip->sign * mip->raw / mip->step;
        }
        else
        {
            if (mip->haveenc)
            {
                double draw;
                int raw;

                /* just change by half-step if encoder changed by 1 */
                raw = csi_rix(MIPSFD(mip), "=epos;");
                draw = abs(raw - mip->raw) == 1 ? (raw + mip->raw) / 2.0 : raw;
                mip->raw = raw;
                mip->cpos = (2 * PI) * mip->esign * draw / mip->estep;
            }
            else
            {
                mip->raw = csi_rix(MIPSFD(mip), "=mpos;");
                mip->cpos = (2 * PI) * mip->sign * mip->raw / mip->step;
            }
        }
    }
}

/* issue a stop to all telescope axes */
static void stopTel(int fast)
{
    MotorInfo *mip;

    FEM(mip)
    {
        if (mip->have)
        {
            if (virtual_mode)
            {
                vmcStop(mip->axis);
            }
            else
            {
                int cfd = MIPCFD(mip);
                csi_intr(cfd);
                csi_w(MIPSFD(mip), "mtvel=0;");
            }
            mip->cvel = 0;
            mip->limiting = 0;
            mip->homing = 0;
        }
    }

    telstatshmp->jogging_ison = 0;
    telstatshmp->telstate = TS_STOPPED; /* well, soon anyway */
    telstatshmp->telstateidx++;
}

/* return 0 if all axes are within acceptable margin of desired, else -1 if
 * if any are out of range and set *mipp to offending axis.
 * N.B. use this only while tracking; use atTarget() when first acquiring.
 */
static int onTarget(MotorInfo **mipp)
{
    MotorInfo *mip;

    FEM(mip)
    {
        double trackacc;

        if (!mip->have)
            continue;

        /* tolerance: "0" means +-1 enc tick */
        trackacc = TRACKACC == 0.0 ? 1.5 * (2 * PI) / (mip->haveenc ? mip->estep : mip->step) : TRACKACC;

        if (delra(mip->cpos - mip->dpos) > trackacc)
        {
            *mipp = mip;
            return (-1);
        }
    }

    /* all ok */
    return (0);
}

/* return 0 if all axes are within ACQUIREACC, else -1.
 * N.B. use this when first acquiring; use onTarget() while tracking.
 */
static int atTarget()
{
    static double mjd0;
    Now *np = &telstatshmp->now;
    MotorInfo *mip;

    static double last_delmax = 0;
    double delpos, delmax = 0;

    FEM(mip)
    {
        double trackacc;

        if (!mip->have)
            continue;

        /* tolerance: "0" means +-1 enc tick */
        trackacc = ACQUIREACC == 0.0 ? 1.5 * (2 * PI) / (mip->haveenc ? mip->estep : mip->step) : ACQUIREACC;

        delpos = delra(mip->cpos - mip->dpos);

        if (delpos > trackacc)
        {
            mjd0 = 0;
            return (-1);
        }

        delpos = fabs(delpos);

        if (delpos > delmax)
            delmax = delpos;
    }

    /* if get here, all axes are within ACQUIREACC this time
     * but still doesn't count until/unless it stays on for a second.
     */
    if (!mjd0)
    {
        mjd0 = mjd;
        last_delmax = delmax;
        return (-1);
    }
    if (mjd >= mjd0 + 1. / SPD)
    {
        tdlog("Hunt %f %f", fabs(last_delmax - delmax) * 3600 * 180 / PI, ACQUIREDELT * 3600 * 180 / PI);
        if (fabs(last_delmax - delmax) > ACQUIREDELT)
        {
            mjd0 = mjd;
            last_delmax = delmax;
            return (-1);
        }
        else
            return (0);
    }
    return (-1);
}

/* check each canonical axis value for being beyond the hardware limit.
 * if wrapok, wrap the input values whole revolutions to accommodate limit.
 * if find any trouble, send failed message to Tel_Id and return -1.
 * else (all ok) return 0.
 */
static int chkLimits(int wrapok, double *xp, double *yp, double *rp)
{
    double *valp[NMOT];
    MotorInfo *mip;
    char str[64];
    double poslim, neglim, alt, az;

    /* store so we can effectively access them via a mip */
    valp[TEL_HM] = xp;
    valp[TEL_DM] = yp;
    valp[TEL_RM] = rp;

    FEM(mip)
    {
        double *vp = valp[mip - telstatshmp->minfo];
        double v = *vp;

        if (!mip->have)
            continue;

        poslim = mip->poslim;
        neglim = mip->neglim;

        while (v <= neglim)
        {
            if (!wrapok)
            {
                fs_sexa(str, raddeg(v), 4, 3600);
                fifoWrite(Tel_Id, -2, "Axis %d: %s hits negative limit", mip->axis, str);
                return (-1);
            }
            v += 2 * PI;
        }

        while (v >= poslim)
        {
            if (!wrapok)
            {
                fs_sexa(str, raddeg(v), 4, 3600);
                fifoWrite(Tel_Id, -3, "Axis %d: %s hits positive limit", mip->axis, str);
                return (-1);
            }
            v -= 2 * PI;
        }

        /* double-check */
        if (v <= neglim || v >= poslim)
        {
            fs_sexa(str, raddeg(v), 4, 3600);
            fifoWrite(Tel_Id, -4, "Axis %d: %s trapped within limits gap", mip->axis, str);
            return (-1);
        }

        /* pass back possibly updated */
        *vp = v;
    }

    /* if get here, all ok */
    return (0);
}

/* set all desireds to currents */
static void dummyTarg()
{
    HMOT->dpos = HMOT->cpos;
    DMOT->dpos = DMOT->cpos;
    RMOT->dpos = RMOT->cpos;

    telstatshmp->DJ2kRA = telstatshmp->CJ2kRA;
    telstatshmp->DJ2kDec = telstatshmp->CJ2kDec;
    telstatshmp->DARA = telstatshmp->CARA;
    telstatshmp->DADec = telstatshmp->CADec;
    telstatshmp->DAHA = telstatshmp->CAHA;
    telstatshmp->Dalt = telstatshmp->Calt;
    telstatshmp->Daz = telstatshmp->Caz;
    telstatshmp->DPA = telstatshmp->CPA;
}

/* called when get a j* jog command while TRACKING.
 * we do not set active_func so we do not look at first.
 */
static void jogTrack(int first, char dircode)
{
    MotorInfo *mip = NULL;
    double gvel = 0;
    double scale;
    int stpv;

    /* establish axis and vel */
    switch (dircode)
    {
    case 'N':
        mip = DMOT;
        gvel = CGUIDEVEL;
        break;
    case 'n':
        mip = DMOT;
        gvel = FGUIDEVEL;
        break;
    case 'S':
        mip = DMOT;
        gvel = -CGUIDEVEL;
        break;
    case 's':
        mip = DMOT;
        gvel = -FGUIDEVEL;
        break;
    case 'E':
        mip = HMOT;
        gvel = CGUIDEVEL;
        break;
    case 'e':
        mip = HMOT;
        gvel = FGUIDEVEL;
        break;
    case 'W':
        mip = HMOT;
        gvel = -CGUIDEVEL;
        break;
    case 'w':
        mip = HMOT;
        gvel = -FGUIDEVEL;
        break;
    case '0':
        if (!virtual_mode)
        {
            mip = HMOT;
            if (mip->have)
                csi_intr(MIPCFD(mip)); /* kill while() */
            mip = DMOT;
            if (mip->have)
                csi_intr(MIPCFD(mip)); /* kill while() */

            tdlog("HA, Dec axis offset = 0");
            return;
        }
    }

    /* sanity checks */
    if (!mip)
    {
        tdlog("Bogus jog direction code '%c'", dircode);
        return;
    }

    if (!mip->have)
    {
        tdlog("No axis to move '%c'", dircode);
        return;
    }

    /* ok, issue the jog */
    if (mip->haveenc)
        scale = mip->esign * mip->estep / (2 * PI);
    else
        scale = mip->sign * mip->step / (2 * PI);
    stpv = floor(gvel * scale + 0.5);

    if (virtual_mode)
        vmcSetTrackingOffset(mip->axis, stpv);
    else
    {
        csi_w(MIPCFD(mip), "while(1) {toffset += %d/5; pause(200);}", stpv);
        tdlog("%s axis offset now %d vel %f", mip == HMOT ? "HA" : "Dec", stpv, 3600 * 180 * stpv / (PI * scale));
    }

    telstatshmp->jogging_ison = 1;
}

/* called when get a j* command while in any state other than TRACKING.
 * we do set not active_func so we do not look at first.
 */
static void jogSlew(int first, char dircode)
{
    MotorInfo *mip = 0;
    char *msg = NULL;

    /* TODO: slave the rotator */

    /* not really used, but shm will show */
    telstatshmp->jdha = telstatshmp->jddec = 0;

    switch (dircode)
    {
    case 'N':
        mip = DMOT;
        mip->cvel = mip->maxvel;
        msg = "up, fast";
        break;
    case 'n':
        mip = DMOT;
        mip->cvel = CGUIDEVEL;
        msg = "up, slow";
        break;
    case 'S':
        mip = DMOT;
        mip->cvel = -mip->maxvel;
        msg = "down, fast";
        break;
    case 's':
        mip = DMOT;
        mip->cvel = -CGUIDEVEL;
        msg = "down, slow";
        break;
    case 'E':
        mip = HMOT;
        mip->cvel = mip->maxvel;
        msg = "CCW, fast";
        break;
    case 'e':
        mip = HMOT;
        mip->cvel = CGUIDEVEL;
        msg = "CCW, slow";
        break;
    case 'W':
        mip = HMOT;
        mip->cvel = -mip->maxvel;
        msg = "CW, fast";
        break;
    case 'w':
        mip = HMOT;
        mip->cvel = -CGUIDEVEL;
        msg = "CW, slow";
        break;
    case '0': /* stop here */
        stopTel(0);
        fifoWrite(Tel_Id, 0, "Paddle command stop");
        telstatshmp->jogging_ison = 0;
        return;
    }

    /* sanity checks */
    if (!mip)
    {
        tdlog("Bogus jog direction code '%c'", dircode);
        return;
    }
    if (!mip->have)
    {
        tdlog("No axis to move %c", dircode);
        return;
    }

    /* ok, issue the jog */
    if (virtual_mode)
    {
        vmcJog(mip->axis, CVELStp(mip));
    }
    else
    {
        csi_w(MIPCFD(mip), "clock=0;");
        csi_w(MIPCFD(mip), "timeout=300000;");
        csi_w(MIPCFD(mip), "mtvel=%d;", CVELStp(mip));
    }
    telstatshmp->telstate = TS_SLEWING;
    telstatshmp->telstateidx++;
    fifoWrite(Tel_Id, 5, "Paddle command %s", msg);
    telstatshmp->jogging_ison = 1;
}

/** Apply an absolute tracking offset in arcseconds to each axis */
static void offsetTracking(int first, double harcsecs, double darcsecs, int report)
{
    long hcounts, dcounts;

    (void)first; // not used in this context

    if (telstatshmp->telstate != TS_TRACKING)
    {
        if (report)
            fifoWrite(Tel_Id, -1, "Telescope is not tracking -- offset ignored");

        return;
    }

    hcounts = (long)(harcsecs * (HMOT->estep * HMOT->esign) /
                     1296000.0); // divide by 360, then by 60 then by 60 == steps per arcsecond
    dcounts = (long)(darcsecs * (DMOT->estep * DMOT->esign) /
                     1296000.0); // divide by 360, then by 60 then by 60 == steps per arcsecond

    /* okay, issue the offsets */
    if (virtual_mode)
    {
        vmcSetTrackingOffset(HMOT->axis, hcounts);
        vmcSetTrackingOffset(DMOT->axis, dcounts);
    }
    else
    {
        csi_w(MIPCFD(HMOT), "toffset = %d;", hcounts);
        csi_w(MIPCFD(DMOT), "toffset = %d;", dcounts);
    }

    // Turn on jogging ... this produces the offset and also serves as a flag that we have done this
    telstatshmp->jogging_ison = 1;

    if (report)
        fifoWrite(Tel_Id, 0, "Tracking offset by %3.3f x %3.3f arcseconds (%ld x %ld steps)", harcsecs, darcsecs,
                  hcounts, dcounts);
}

/* reread the config files -- exit if trouble */
static void initCfg()
{
#define NTDCFG (sizeof(tdcfg) / sizeof(tdcfg[0]))
#define NHCFG (sizeof(hcfg) / sizeof(hcfg[0]))

    static double HMAXVEL, HMAXACC, HSLIMACC, HDAMP, HTRENCWT;
    static int HHAVE, HAXIS, HENCHOME, HPOSSIDE, HHOMELOW, HESTEP, HESIGN;

    static double DMAXVEL, DMAXACC, DSLIMACC, DDAMP, DTRENCWT;
    static int DHAVE, DAXIS, DENCHOME, DPOSSIDE, DHOMELOW, DESTEP, DESIGN;

    static double RMAXVEL, RMAXACC, RSLIMACC, RDAMP;
    static int RHAVE, RAXIS, RHASLIM, RPOSSIDE, RHOMELOW, RSTEP, RSIGN;

    static int GERMEQ;
    static int ZENFLIP;

    static CfgEntry tdcfg[] = {
        {"HHAVE", CFG_INT, &HHAVE},
        {"HAXIS", CFG_INT, &HAXIS},
        {"HHOMELOW", CFG_INT, &HHOMELOW},
        {"HPOSSIDE", CFG_INT, &HPOSSIDE},
        {"HESTEP", CFG_INT, &HESTEP},
        {"HESIGN", CFG_INT, &HESIGN},
        {"HMAXVEL", CFG_DBL, &HMAXVEL},
        {"HMAXACC", CFG_DBL, &HMAXACC},
        {"HSLIMACC", CFG_DBL, &HSLIMACC},

        {"DHAVE", CFG_INT, &DHAVE},
        {"DAXIS", CFG_INT, &DAXIS},
        {"DHOMELOW", CFG_INT, &DHOMELOW},
        {"DPOSSIDE", CFG_INT, &DPOSSIDE},
        {"DESTEP", CFG_INT, &DESTEP},
        {"DESIGN", CFG_INT, &DESIGN},
        {"DMAXVEL", CFG_DBL, &DMAXVEL},
        {"DMAXACC", CFG_DBL, &DMAXACC},
        {"DSLIMACC", CFG_DBL, &DSLIMACC},

        {"RHAVE", CFG_INT, &RHAVE},
        {"RAXIS", CFG_INT, &RAXIS},
        {"RHASLIM", CFG_INT, &RHASLIM},
        {"RHOMELOW", CFG_INT, &RHOMELOW},
        {"RPOSSIDE", CFG_INT, &RPOSSIDE},
        {"RSTEP", CFG_INT, &RSTEP},
        {"RSIGN", CFG_INT, &RSIGN},
        {"RMAXVEL", CFG_DBL, &RMAXVEL},
        {"RMAXACC", CFG_DBL, &RMAXACC},
        {"RSLIMACC", CFG_DBL, &RSLIMACC},

        {"TRACKINT", CFG_INT, &TRACKINT},
        {"GERMEQ", CFG_INT, &GERMEQ},
        {"ZENFLIP", CFG_INT, &ZENFLIP},
        {"TRACKACC", CFG_DBL, &TRACKACC},
        {"ACQUIREACC", CFG_DBL, &ACQUIREACC},
        {"ACQUIREDELT", CFG_DBL, &ACQUIREDELT},
        {"FGUIDEVEL", CFG_DBL, &FGUIDEVEL},
        {"CGUIDEVEL", CFG_DBL, &CGUIDEVEL},
    };

    static double HT, DT, XP, YC, NP, R0;
    static double HPOSLIM, HNEGLIM, DPOSLIM, DNEGLIM, RNEGLIM, RPOSLIM;
    static int HSTEP, HSIGN, DSTEP, DSIGN;

    static CfgEntry hcfg[] = {
        {"HT", CFG_DBL, &HT},
        {"DT", CFG_DBL, &DT},
        {"XP", CFG_DBL, &XP},
        {"YC", CFG_DBL, &YC},
        {"NP", CFG_DBL, &NP},
        {"R0", CFG_DBL, &R0},

        {"HPOSLIM", CFG_DBL, &HPOSLIM},
        {"HNEGLIM", CFG_DBL, &HNEGLIM},
        {"DPOSLIM", CFG_DBL, &DPOSLIM},
        {"DNEGLIM", CFG_DBL, &DNEGLIM},
        {"RNEGLIM", CFG_DBL, &RNEGLIM},
        {"RPOSLIM", CFG_DBL, &RPOSLIM},

        {"HSTEP", CFG_INT, &HSTEP},
        {"HSIGN", CFG_INT, &HSIGN},
        {"DSTEP", CFG_INT, &DSTEP},
        {"DSIGN", CFG_INT, &DSIGN},
    };

    static int LARGEXP;
    static CfgEntry hcfg2[] = {
        {"LARGEXP", CFG_INT, &LARGEXP},
    };

    MotorInfo *mip;
    TelAxes *tap;
    int n;

    /* read in everything */
    n = readCfgFile(1, tdcfn, tdcfg, NTDCFG);
    if (n != NTDCFG)
    {
        cfgFileError(tdcfn, n, (CfgPrFp)tdlog, tdcfg, NTDCFG);
        die();
    }
    n = readCfgFile(1, hcfn, hcfg, NHCFG);
    if (n != NHCFG)
    {
        cfgFileError(hcfn, n, (CfgPrFp)tdlog, hcfg, NHCFG);
        die();
    }

    // Added fix for RA home switch beign > 180 degrees from north
    LARGEXP = 0;
    (void)readCfgFile(1, hcfn, hcfg2, 1);
    if (LARGEXP)
    {
        HT -= (PI / 2);
        XP += (PI / 2);
    }

    /* misc checks */
    if (TRACKINT <= 0)
    {
        tdlog("TRACKINT must be > 0\n");
        die();
    }

    /* install H */

    mip = &telstatshmp->minfo[TEL_HM];
    memset((void *)mip, 0, sizeof(*mip));
    mip->axis = HAXIS;
    mip->have = HHAVE;
    mip->haveenc = 1;
    mip->enchome = HENCHOME;
    mip->havelim = 1;
    if (HPOSSIDE != 0 && HPOSSIDE != 1)
    {
        tdlog("HPOSSIDE must be 0 or 1\n");
        die();
    }
    mip->posside = HPOSSIDE;
    if (HHOMELOW != 0 && HHOMELOW != 1)
    {
        tdlog("HHOMELOW must be 0 or 1\n");
        die();
    }
    mip->homelow = HHOMELOW;
    mip->step = HSTEP;
    if (abs(HSIGN) != 1)
    {
        tdlog("HSIGN must be +-1\n");
        die();
    }
    mip->sign = HSIGN;
    mip->estep = HESTEP;
    if (abs(HESIGN) != 1)
    {
        tdlog("HESIGN must be +-1\n");
        die();
    }
    mip->esign = HESIGN;
    mip->limmarg = 0;
    if (HMAXVEL <= 0)
    {
        tdlog("HMAXVEL must be > 0\n");
        die();
    }
    mip->maxvel = HMAXVEL;
    mip->maxacc = HMAXACC;
    mip->slimacc = HSLIMACC;
    mip->poslim = HPOSLIM;
    mip->neglim = HNEGLIM;
    mip->trencwt = HTRENCWT;
    mip->df = HDAMP;

    /* install D */

    mip = &telstatshmp->minfo[TEL_DM];
    memset((void *)mip, 0, sizeof(*mip));
    mip->axis = DAXIS;
    mip->have = DHAVE;
    mip->haveenc = 1;
    mip->enchome = DENCHOME;
    mip->havelim = 1;
    if (DPOSSIDE != 0 && DPOSSIDE != 1)
    {
        tdlog("DPOSSIDE must be 0 or 1\n");
        die();
    }
    mip->posside = DPOSSIDE;
    if (DHOMELOW != 0 && DHOMELOW != 1)
    {
        tdlog("DHOMELOW must be 0 or 1\n");
        die();
    }
    mip->homelow = DHOMELOW;
    mip->step = DSTEP;
    if (abs(DSIGN) != 1)
    {
        tdlog("DSIGN must be +-1\n");
        die();
    }
    mip->sign = DSIGN;
    mip->estep = DESTEP;
    if (abs(DESIGN) != 1)
    {
        tdlog("DESIGN must be +-1\n");
        die();
    }
    mip->esign = DESIGN;
    mip->limmarg = 0;
    if (DMAXVEL <= 0)
    {
        tdlog("DMAXVEL must be > 0\n");
        die();
    }
    mip->maxvel = DMAXVEL;
    mip->maxacc = DMAXACC;
    mip->slimacc = DSLIMACC;
    mip->poslim = DPOSLIM;
    mip->neglim = DNEGLIM;
    mip->trencwt = DTRENCWT;
    mip->df = DDAMP;

    /* install R */

    mip = &telstatshmp->minfo[TEL_RM];
    memset((void *)mip, 0, sizeof(*mip));
    mip->axis = RAXIS;
    mip->have = RHAVE;
    mip->haveenc = 0;
    mip->enchome = 0;
    mip->havelim = RHASLIM;
    if (RPOSSIDE != 0 && RPOSSIDE != 1)
    {
        tdlog("RPOSSIDE must be 0 or 1\n");
        die();
    }
    mip->posside = RPOSSIDE;
    if (RHOMELOW != 0 && RHOMELOW != 1)
    {
        tdlog("RHOMELOW must be 0 or 1\n");
        die();
    }
    mip->homelow = RHOMELOW;
    mip->step = RSTEP;
    if (abs(RSIGN) != 1)
    {
        tdlog("RSIGN must be +-1\n");
        die();
    }
    mip->sign = RSIGN;
    mip->estep = RSTEP;
    mip->esign = RSIGN;
    mip->limmarg = 0;
    if (RMAXVEL <= 0)
    {
        tdlog("RMAXVEL must be > 0\n");
        die();
    }
    mip->maxvel = RMAXVEL;
    mip->maxacc = RMAXACC;
    mip->slimacc = RSLIMACC;
    mip->poslim = RPOSLIM;
    mip->neglim = RNEGLIM;
    mip->trencwt = 0;
    mip->df = RDAMP;

    tap = &telstatshmp->tax;
    memset((void *)tap, 0, sizeof(*tap));
    tap->GERMEQ = GERMEQ;
    tap->ZENFLIP = ZENFLIP;
    tap->HT = HT;
    tap->DT = DT;
    tap->XP = XP;
    tap->YC = YC;
    tap->NP = NP;
    tap->R0 = R0;

    tap->hneglim = telstatshmp->minfo[TEL_HM].neglim;
    tap->hposlim = telstatshmp->minfo[TEL_HM].poslim;

    telstatshmp->dt = 100; /* not critical */

    /* re-read the mesh  file */
    init_mount_cor();

#undef NTDCFG
#undef NHCFG
}

/* check for stuck axes.
 * return 0 if all ok, else -1
 */
static int checkAxes()
{
    MotorInfo *mip;
    char buf[1024];
    int nbad = 0;

    /* check all axes to learn more */
    FEM(mip)
    {
        if (axisLimitCheck(mip, buf) < 0)
        {
            fifoWrite(Tel_Id, -8, "%s", buf);
            nbad++;
        }
        else if (axisMotionCheck(mip, buf) < 0)
        {
            fifoWrite(Tel_Id, -9, "%s", buf);
            nbad++;
        }
    }

    return (nbad > 0 ? -1 : 0);
}

int tel_ishomed(void)
{
    MotorInfo *mip;
    int ok = 1;

    FEM(mip)
    {
        if (mip->have && !mip->ishomed)
        {
            ok = 0;
            break;
        }
    }

    return (ok);
}
