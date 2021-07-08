/* handle the Focus channel */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <math.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include "P_.h"
#include "astro.h"
#include "circum.h"
#include "configfile.h"
#include "strops.h"
#include "telstatshm.h"
#include "running.h"
#include "misc.h"
#include "csimc.h"
#include "virmc.h"
#include "telenv.h"
#include "cliserv.h"

#include "teled.h"

/* the current activity, if any */
static void (*active_func) (int first, ...);

/* one of these... */
static void focus_poll(void);
static void focus_reset(int first);
static void focus_home(int first, ...);
static void focus_limits(int first, ...);
static void focus_stop(int first, ...);
static void focus_auto(int first, ...);
static void focus_offset(int first, ...);
static void focus_jog(int first, ...);

/* helped along by these... */
static void initCfg(void);
static void stopFocus(int fast);
static void readFocus (void);

static double OJOGF;

/* called when we receive a message from the Focus fifo.
 * if !msg just update things.
 */
/* ARGSUSED */
void
focus_msg (msg)
char *msg;
{
	char jog[10];

	/* do reset before checking for `have' to allow for new config file */
	if (msg && strncasecmp (msg, "reset", 5) == 0) {
	    focus_reset(1);
	    return;
	}

	if (!OMOT->have) {
	    if (msg)
		fifoWrite (Focus_Id, 0, "Ok, but focuser not really installed");
	    return;
	}

	/* setup? */
	if(!virtual_mode) {
		if (!MIPCFD(OMOT)) {
		    tdlog ("Focus command before initial Reset: %s", msg?msg:"(NULL)");
    		return;
		}
	}
	
	if (!msg)
	    focus_poll();
	else if (strncasecmp (msg, "home", 4) == 0)
	    focus_home(1);
	else if (strncasecmp (msg, "stop", 4) == 0)
	    focus_stop(1);
	else if (strncasecmp (msg, "limits", 6) == 0)
	    focus_limits(1);
	else if (sscanf (msg, "j%1[0+-]", jog) == 1)
	    focus_jog (1, jog[0]);
	else
	    focus_offset (1, atof(msg));
}

/* no new messages.
 * goose the current objective, if any.
 */
static void
focus_poll()
{
	if(virtual_mode) {
		MotorInfo *mip = OMOT;
		vmcService(mip->axis);
	}
	if (active_func)
	    (*active_func)(0);
	/* TODO: monitor while idle? */
}

/* stop and reread config files */
static void
focus_reset(int first)
{
	MotorInfo *mip = OMOT;
	int had = mip->have;

	initCfg();

	/* TODO: for some reason focus behaves badly if you just close/reopen.
	 * N.B. "had" relies on telstatshmp being zeroed when telescoped starts.
	 */
	if (mip->have) {
		if(virtual_mode) {
			if(vmcSetup(mip->axis,mip->maxvel,mip->maxacc,mip->step,mip->sign)) {
				mip->ishomed = 0;
			}
			vmcReset(mip->axis);
		} else {
		    if (!had) csiiOpen (mip);
	    	csiSetup(mip);
	    }
	    stopFocus(0);
	    readFocus ();
	    fifoWrite (Focus_Id, 0, "Reset complete");
	} else {
		if(!virtual_mode) {
		    if (had) csiiClose (mip);
		}
	    fifoWrite (Focus_Id, 0, "Not installed");
	}
}

/* seek the home position */
static void
focus_home(int first, ...)
{
	MotorInfo *mip = OMOT;
	double ugoal, unow;

	if (first) {
	    stopFocus(0);
	    if (axis_home (mip, Focus_Id, 1) < 0) {
		active_func = NULL;
		return;
	    }

	    /* new state */
	    active_func = focus_home;
	}

	switch (axis_home (mip, Focus_Id, 0)) {
	case -1:
	    stopFocus(1);
	    active_func = NULL;
	    return;
	case  1: 
	    break;
	case  0:
	    active_func = NULL;
	    fifoWrite (Focus_Id,1,"Homing complete.");
	    readFocus();
	    unow = mip->cpos*mip->step/(2*PI*mip->focscale);
	    mip->cvel = 0;
	    mip->homing = 0;
	    focus_offset (1, ugoal - unow);
	    break;
	}
}

static void
focus_limits(int first, ...)
{
	MotorInfo *mip = OMOT;

	/* maintain cpos and raw */
	readFocus();

	if (first) {
	    if (axis_limits (mip, Focus_Id, 1) < 0) {
		stopFocus(1);
		active_func = NULL;
		return;
	    }

	    /* new state */
	    active_func = focus_limits;
	}

	switch (axis_limits (mip, Focus_Id, 0)) {
	case -1:
	    stopFocus(1);
	    active_func = NULL;
	    return;
	case  1: 
	    break;
	case  0:
	    stopFocus(0);
	    active_func = NULL;
	    initCfg();		/* read new limits */
	    fifoWrite (Focus_Id, 0, "Limits found");
	    break;
	}
}

static void
focus_stop(int first, ...)
{
	MotorInfo *mip = OMOT;
	int cfd = MIPCFD(mip);

	/* stay current */
	readFocus();

	if (first) {
	    /* issue stop */
	    stopFocus(0);
	    active_func = focus_stop;
	}

	/* wait for really stopped */
	if(virtual_mode) {
		if(vmcGetVelocity(mip->axis) != 0) return;
	} else {
		if (csi_rix (cfd, "=mvel;") != 0) return;
	}
	
	/* if get here, it has really stopped */
	active_func = NULL;
	readFocus();
	fifoWrite (Focus_Id, 0, "Stop complete");
}

/* handle a relative focus move, in microns */
static void
focus_offset(int first, ...)
{
	static int rawgoal;
	MotorInfo *mip = OMOT;
	int cfd = MIPCFD(mip);
	
	
	/* maintain current info */
	readFocus();

	if (first) {
	    va_list ap;
	    double delta, goal;
		char buf[128];
		
	    // make sure we're homed to begin with
	    if(axisHomedCheck(mip, buf)) {
		    active_func = NULL;
		    stopFocus(0);
		    fifoWrite (Focus_Id, -1, "Focus error: %s", buf);
            return;
		}	
	
	    /* fetch offset, in microns, canonical direction */
	    va_start (ap, first);
	    delta = va_arg (ap, double);
	    va_end (ap);

	    /* compute goal, in rads from home; check against limits */
	    goal = mip->cpos + (2*PI)*delta*mip->focscale/mip->step;
	    if (goal > mip->poslim) {
		fifoWrite (Focus_Id, -1, "Move is beyond positive limit");
		active_func = NULL;
		return;
	    }
	    if (goal < mip->neglim) {
		fifoWrite (Focus_Id, -2, "Move is beyond negative limit");
		active_func = NULL;
		return;
	    }

	    /* ok, go for the gold, er, goal */
	    rawgoal = (int)floor(mip->sign*mip->step*goal/(2*PI) + 0.5);
	    if(virtual_mode) {
	    	vmcSetTargetPosition(mip->axis, rawgoal);
	    } else {
	    	csi_w (cfd, "mtpos=%d;", rawgoal);
	    }
	    mip->cvel = mip->maxvel;
	    mip->dpos = goal;
	    active_func = focus_offset;
	}

	/* done when we reach goal */
	if (mip->raw == rawgoal) {
	    active_func = NULL;
	    stopFocus(0);
	    fifoWrite (Focus_Id, 0, "Focus offset complete");
	}
}
	
/* handle a joystick jog command */
static void
focus_jog(int first, ...)
{
	MotorInfo *mip = OMOT;
	int cfd = MIPCFD(mip);
	char buf[1024];

	/* maintain current info */
	readFocus();
	mip->dpos = mip->cpos;	/* just for looks */

	if (first) {
	    va_list ap;
	    char dircode;

	    /* fetch offset, in microns, canonical direction */
	    va_start (ap, first);
	    //dircode = va_arg (ap, char);
	    dircode = va_arg (ap, int); // char is promoted to int, so pass int...
	    va_end (ap);

	    /* crack the code */
	    switch (dircode) {
	    case '0':	/* stop */
		focus_stop (1);		/* gentle and reports accurately */
		return;

	    case '+':	/* go canonical positive */
		if (mip->cpos >= mip->poslim) {
		    fifoWrite (Focus_Id, -4, "At positive limit");
		    return;
		}
		if(virtual_mode) {
			vmcJog(mip->axis,(long)(mip->sign*MAXVELStp(mip)*OJOGF));
		} else {
			csi_w (cfd, "mtvel=%.0f;", mip->sign*MAXVELStp(mip)*OJOGF);
		}
		mip->cvel = mip->maxvel*OJOGF;
		active_func = focus_jog;
		fifoWrite (Focus_Id, 1, "Paddle command in");
		break;

	    case '-':	/* go canonical negative */
		if (mip->cpos <= mip->neglim) {
		    fifoWrite (Focus_Id, -5, "At negative limit");
		    return;
		}
		if(virtual_mode) {
			vmcJog(mip->axis,0 - (long) (mip->sign*MAXVELStp(mip)*OJOGF));
		} else {
			csi_w (cfd, "mtvel=%.0f;", -mip->sign*MAXVELStp(mip)*OJOGF);
		}
		mip->cvel = -mip->maxvel*OJOGF;
		active_func = focus_jog;
		fifoWrite (Focus_Id, 2, "Paddle command out");
		break;

	    default:
		tdlog ("focus_jog(): bogus dircode: %c 0x%x", dircode, dircode);
		active_func = NULL;
		return;
	    }
	}

	/* this is under user control -- about all we can do is watch for lim */
	if (axisLimitCheck (mip, buf) < 0) {
	    stopFocus(1);
	    active_func = NULL;
	    fifoWrite (Focus_Id, -7, "%s", buf);
	}
}



static void
initCfg()
{
#define NOCFG   (sizeof(ocfg)/sizeof(ocfg[0]))
#define NHCFG   (sizeof(hcfg)/sizeof(hcfg[0]))

	static int OHAVE, OHASLIM, OAXIS;
	static int OSTEP, OSIGN, OPOSSIDE, OHOMELOW;
	static double OMAXVEL, OMAXACC, OSLIMACC, OSCALE;

	static CfgEntry ocfg[] = {
	    {"OAXIS",		CFG_INT, &OAXIS}, 
	    {"OHAVE",		CFG_INT, &OHAVE},
	    {"OHASLIM",		CFG_INT, &OHASLIM},
	    {"OPOSSIDE",	CFG_INT, &OPOSSIDE},
	    {"OHOMELOW",	CFG_INT, &OHOMELOW},
	    {"OSTEP",		CFG_INT, &OSTEP},
	    {"OSIGN",		CFG_INT, &OSIGN},
	    {"OMAXVEL",		CFG_DBL, &OMAXVEL},
	    {"OMAXACC",		CFG_DBL, &OMAXACC},
	    {"OSLIMACC",	CFG_DBL, &OSLIMACC},
	    {"OSCALE",		CFG_DBL, &OSCALE},
	    {"OJOGF",		CFG_DBL, &OJOGF},
	};

	static double OPOSLIM, ONEGLIM;

	static CfgEntry hcfg[] = {
	    {"OPOSLIM",		CFG_DBL, &OPOSLIM}, 
	    {"ONEGLIM",		CFG_DBL, &ONEGLIM}, 
	};

	MotorInfo *mip = OMOT;
	int n;
		
	n = readCfgFile (1, ocfn, ocfg, NOCFG);
	if (n != NOCFG) {
	    cfgFileError (ocfn, n, (CfgPrFp)tdlog, ocfg, NOCFG);
	    die();
	}
		
	n = readCfgFile (1, hcfn, hcfg, NHCFG);
	if (n != NHCFG) {
	    cfgFileError (hcfn, n, (CfgPrFp)tdlog, hcfg, NHCFG);
	    die();
	}

	memset ((void *)mip, 0, sizeof(*mip));

	mip->axis = OAXIS;
	mip->have = OHAVE;
	mip->haveenc = 0;
	mip->enchome = 0;
	mip->havelim = OHASLIM;
	mip->posside = OPOSSIDE ? 1 : 0;
	mip->homelow = OHOMELOW ? 1 : 0;
	mip->step = OSTEP;

	if (abs(OSIGN) != 1) {
	    tdlog ("OSIGN must be +-1\n");
	    die();
	}
	mip->sign = OSIGN;

	mip->limmarg = 0;
	mip->maxvel = fabs(OMAXVEL);
	mip->maxacc = OMAXACC;
	mip->slimacc = OSLIMACC;
	mip->poslim = OPOSLIM;
	mip->neglim = ONEGLIM;

	mip->focscale = OSCALE;

#undef NOCFG
#undef NHCFG
}

static void
stopFocus(int fast)
{
	MotorInfo *mip = OMOT;

	if(virtual_mode) {
		vmcStop(mip->axis);
	} else {
		csiStop (mip, fast);
	}

	OMOT->homing = 0;
	OMOT->limiting = 0;
	OMOT->cvel = 0;
	
	//STO: 20010523 Focus stop (red light) visual bug due to position mismatch on stop
	OMOT->dpos = OMOT->cpos;
}

/* read the raw value */
static void
readFocus ()
{
	MotorInfo *mip = OMOT;

	if (!mip->have)
	    return;

	if (virtual_mode) {
	    mip->raw = vmc_rix (mip->axis, "=mpos;");
	    mip->cpos = (2*PI) * mip->sign * mip->raw / mip->step;
	} else {
	    mip->raw = csi_rix (MIPSFD(mip), "=mpos;");
	    mip->cpos = (2*PI) * mip->sign * mip->raw / mip->step;
	}
}

/* For RCS Only -- Do Not Edit */
static char *rcsid[2] = {(char *)rcsid, "@(#) $RCSfile: focus.c,v $ $Date: 2006/01/13 20:43:23 $ $Revision: 1.1.1.1 $ $Name:  $"};
