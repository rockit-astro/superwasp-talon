/* update the display based on shared memory */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>

#include <Xm/Xm.h>
#include <Xm/ToggleB.h>
#include <Xm/RowColumn.h>
#include <X11/keysym.h>

#include "P_.h"
#include "astro.h"
#include "circum.h"
#include "strops.h"
#include "configfile.h"
#include "misc.h"
#include "telstatshm.h"
#include "xtools.h"

#include "xobs.h"
#include "widgets.h"

#define	SLOW_DT		(5./SPD)	/* days between "slow" updates */
#define	FAST_DT		(.2/SPD)	/* days between "fast" updates */
#define	COAST_DT	(2./SPD)	/* days for fast after busy */
#define	MAXTEMPERR	2		/* camtemp error to show WARN */

static void curPos (void);
static void noPos (void);
static void curTarg (void);
static void noTarg (void);
static void computeSunMoon(void);
static void showTime (void);
static void showSunMoon (void);
static void showScope(void);
static void showHL(void);
static void showDome(void);

static char blank[] = " ";
static int batchison;

/* update the display.
 * called periodically and on specific impulses.
 * if force, redraw everything, else just what seems timely.
 */
void
updateStatus(int force)
{

	static double last_slow, last_fast;
	static double last_tbusy, last_dbusy, last_obusy;
	static double last_ibusy, last_wbusy;
	Now *np = &telstatshmp->now;
	int doslow = force || mjd > last_slow + SLOW_DT;
	int dofast = force || mjd > last_fast + FAST_DT;
	TelState ts = telstatshmp->telstate;
	DomeState ds = telstatshmp->domestate;
	DShState ss = telstatshmp->shutterstate;
	int busy;

	/* do once per swing through */
	batchison = batchIsOn();
	soundIsOn();

	/* always do these at least occasionally */
	if (doslow) {
	    computeSunMoon();
	    showSunMoon();
	    last_slow = mjd;
	}

	/* always do these at least more often */
	if (dofast) {
	    showTime();
	    batchUpdate();
	    last_fast = mjd;
	}

	/* do these at least occasionally or especially often when busy */

	busy = ts==TS_SLEWING || ts==TS_HUNTING || ts==TS_LIMITING;
	if (doslow || busy || mjd < last_tbusy + COAST_DT) {
	    showSkyMap();
	    if (busy)
		last_tbusy = mjd;
	}

	busy = ds==DS_ROTATING || ds==DS_HOMING ||
					    ss==SH_OPENING || ss==SH_CLOSING;
	if (doslow || busy || mjd < last_dbusy + COAST_DT) {
	    showDome();
	    if (busy)
		last_dbusy = mjd;
	}

	busy = IMOT->cvel != 0;
	if (doslow || busy || mjd < last_ibusy + COAST_DT) {
	    if (busy)
		last_ibusy = mjd;
	}

	/* always be very responsive to the scope */
	showScope();
	showHL();
}

static void
curPos ()
{
	char buf[32];

	fs_sexa (buf, radhr(telstatshmp->CJ2kRA), 2, 36000);
	wtprintf (g_w[PCRA_W], "%s", buf);

	fs_sexa (buf, raddeg(telstatshmp->CJ2kDec), 4, 3600);
	wtprintf (g_w[PCDEC_W], "%s", buf);

	fs_sexa (buf, radhr(telstatshmp->CAHA), 2, 36000);
	wtprintf (g_w[PCHA_W], "%s", buf);

	fs_sexa (buf, raddeg(telstatshmp->Calt), 4, 3600);
	wtprintf (g_w[PCALT_W], "%s", buf);

	fs_sexa (buf, raddeg(telstatshmp->Caz), 4, 3600);
	wtprintf (g_w[PCAZ_W], "%s", buf);
}

static void
noPos ()
{
	wtprintf (g_w[PCRA_W], blank);
	wtprintf (g_w[PCDEC_W], blank);
	wtprintf (g_w[PCHA_W], blank);
	wtprintf (g_w[PCALT_W], blank);
	wtprintf (g_w[PCAZ_W], blank);
	wtprintf (g_w[PCDAZ_W], blank);
}

static void
curTarg ()
{
	char buf[32];
	double tmp;

	/* target */

	fs_sexa (buf, radhr(telstatshmp->DJ2kRA), 2, 36000);
	wtprintf (g_w[PTRA_W], "%s", buf);

	fs_sexa (buf, raddeg(telstatshmp->DJ2kDec), 4, 3600);
	wtprintf (g_w[PTDEC_W], "%s", buf);

	fs_sexa (buf, radhr(telstatshmp->DAHA), 2, 36000);
	wtprintf (g_w[PTHA_W], "%s", buf);

	fs_sexa (buf, raddeg(telstatshmp->Dalt), 4, 3600);
	wtprintf (g_w[PTALT_W], "%s", buf);

	fs_sexa (buf, raddeg(telstatshmp->Daz), 4, 3600);
	wtprintf (g_w[PTAZ_W], "%s", buf);

	/* differences */

	tmp = delra (telstatshmp->CJ2kRA - telstatshmp->DJ2kRA);
	fs_sexa (buf, radhr(tmp), 2, 36000);
	wtprintf (g_w[PDRA_W], "%s", buf);

	tmp = telstatshmp->CJ2kDec - telstatshmp->DJ2kDec;
	fs_sexa (buf, raddeg(tmp), 4, 3600);
	wtprintf (g_w[PDDEC_W], "%s", buf);

	tmp = delra (telstatshmp->CAHA - telstatshmp->DAHA);
	fs_sexa (buf, radhr(tmp), 2, 36000);
	wtprintf (g_w[PDHA_W], "%s", buf);

	tmp = telstatshmp->Calt - telstatshmp->Dalt;
	fs_sexa (buf, raddeg(tmp), 4, 3600);
	wtprintf (g_w[PDALT_W], "%s", buf);

	tmp = telstatshmp->Caz - telstatshmp->Daz;
	fs_sexa (buf, raddeg(tmp), 4, 3600);
	wtprintf (g_w[PDAZ_W], "%s", buf);
}

static void
noTarg ()
{
	wtprintf (g_w[PTRA_W], blank);
	wtprintf (g_w[PTDEC_W], blank);
	wtprintf (g_w[PTHA_W], blank);
	wtprintf (g_w[PTALT_W], blank);
	wtprintf (g_w[PTAZ_W], blank);

	wtprintf (g_w[PDRA_W], blank);
	wtprintf (g_w[PDDEC_W], blank);
	wtprintf (g_w[PDHA_W], blank);
	wtprintf (g_w[PDALT_W], blank);
	wtprintf (g_w[PDAZ_W], blank);
}

static void
showTime()
{
	Now *np = &telstatshmp->now;
	double dawn, dusk;
	double tmp, lst;
	double d;
	int m, y;
	int rsstatus;
	char buf[64];

	tmp = utc_now (np);
	fs_sexa (buf, tmp, 2, 3600);
	wtprintf (g_w[IUT_W], "  %s", buf);

	tmp -= tz;
	range (&tmp, 24.0);
	fs_sexa (buf, tmp, 2, 3600);
	wtprintf (g_w[ILT_W], "  %s", buf);

	mjd_cal (mjd, &m, &d, &y);
	wtprintf (g_w[IUTD_W], "%2d-%s-%d", (int)d, monthName(m), y);

	wtprintf (g_w[IJD_W], "%11.3f", telstatshmp->now.n_mjd+MJD0);

	now_lst (np, &lst);
	fs_sexa (buf, lst, 2, 3600);
	wtprintf (g_w[ILST_W], "  %s", buf);

	twilight_cir (np, SUNDOWN, &dawn, &dusk,&rsstatus);
	dawn = mjd_hr (dawn);
	dusk = mjd_hr (dusk);
	fs_sexa (buf, dawn, 2, 60);
	wtprintf (g_w[IDAWN_W], "  %s UT", buf);
	fs_sexa (buf, dusk, 2, 60);
	wtprintf (g_w[IDUSK_W], "  %s UT", buf);
	tmp = mjd_hr(telstatshmp->now.n_mjd);

	if (tmp > dusk || tmp < dawn) {
	    setColor (g_w[IDAWN_W], XmNbackground, uneditableColor);
	    setColor (g_w[IDUSK_W], XmNbackground, uneditableColor);
	} else {
	    setColor (g_w[IDAWN_W], XmNbackground, ltcolors[LTACTIVE]);
	    setColor (g_w[IDUSK_W], XmNbackground, ltcolors[LTACTIVE]);
	}
}

static void
computeSunMoon()
{
	Now *np = &telstatshmp->now;

	(void) memset ((void *)&moonobj, 0, sizeof(moonobj));
	moonobj.o_type = PLANET;
	moonobj.pl.pl_code = MOON;
	(void) obj_cir (np, &moonobj);

	(void) memset ((void *)&sunobj, 0, sizeof(sunobj));
	sunobj.o_type = PLANET;
	sunobj.pl.pl_code = SUN;
	(void) obj_cir (np, &sunobj);
}

static void
showSunMoon ()
{
	Widget w;
	Obj *op;

	op = &moonobj;
	w = g_w[IMOON_W];
	wtprintf (w, "%2.0f%% %2s %+.0f", op->s_phase, cardDirName(op->s_az),
							    raddeg(op->s_alt));
	setColor (w, XmNbackground, op->s_alt > 0 ? ltcolors[LTACTIVE]
						  : uneditableColor);

	op = &sunobj;
	w = g_w[ISUN_W];
	wtprintf (w, "   %3s %+.0f", cardDirName(op->s_az), raddeg(op->s_alt));
	setColor (w, XmNbackground, op->s_alt > 0 ? ltcolors[LTACTIVE]
						  : uneditableColor);
}

static void
showScope()
{
	/* batch status */
	setLt (g_w[SBLT_W], batchison ? LTOK : LTIDLE);
	if (!xobs_alone) {
	    /* track state if passive */
	    int now = XmToggleButtonGetState(g_w[CBATCH_W]);
	    if (now != batchison)
		XmToggleButtonSetState (g_w[CBATCH_W], batchison, False);
	}

	switch (telstatshmp->telstate) {
	case TS_STOPPED:
	    setLt(g_w[STLT_W],LTIDLE); setLt(g_w[SSLT_W],LTIDLE);
	    curPos();
	    noTarg();
	    break;

	case TS_SLEWING:
	    setLt(g_w[STLT_W],LTIDLE); setLt(g_w[SSLT_W],LTOK);
	    curPos();
	    curTarg();
	    break;

	case TS_HUNTING:
	    setLt(g_w[STLT_W],LTACTIVE); setLt(g_w[SSLT_W],LTOK);
	    curPos();
	    curTarg();
	    break;

	case TS_TRACKING:
	    setLt(g_w[STLT_W],LTOK); setLt(g_w[SSLT_W],LTIDLE);
	    curPos();
	    curTarg();
	    break;

	case TS_HOMING:
	    setLt(g_w[STLT_W],LTIDLE); setLt(g_w[SSLT_W],LTIDLE);
	    noPos();
	    noTarg();
	    break;

	case TS_LIMITING:
	    setLt(g_w[STLT_W],LTIDLE); setLt(g_w[SSLT_W],LTIDLE);
	    curPos();
	    noTarg();
	    break;
	}
}

static void
showHL()
{
	setLt(g_w[SHLT_W], ANY_HOMING   ? LTACTIVE : LTIDLE);
	setLt(g_w[SLLT_W], ANY_LIMITING ? LTACTIVE : LTIDLE);
}

static void
showDome()
{
	static int last_godome = -1, last_goshutter = -1;
	DomeState ds = telstatshmp->domestate;
	LtState domelt, sol, scl;
	int go;
	int domealarm = telstatshmp->domealarm;

	/* first check whether to allow operator access.
	 * N.B. must cooperate with batchOn/Off
	 */
	go = ds != DS_ABSENT && xobs_alone && !batchison;
	if (go != last_godome) {
	    if (go) {
		XtSetSensitive (g_w[DAUTO_W], True);
		XtSetSensitive (g_w[DAZ_W], True);
		XtVaSetValues (g_w[DAZ_W], XmNbackground, editableColor, NULL);
		XtSetSensitive (g_w[DAZL_W], True);
	    } else {
		XtSetSensitive (g_w[DAUTO_W], False);
		XtSetSensitive (g_w[DAZ_W], False);
		XtVaSetValues (g_w[DAZ_W], XmNbackground, uneditableColor,NULL);
		XtSetSensitive (g_w[DAZL_W], False);
	    }
	    last_godome = go;
	}
	go = telstatshmp->shutterstate != SH_ABSENT && xobs_alone && !batchison;
	if (go != last_goshutter) {
	    if (go) {
		XtSetSensitive (g_w[DOPEN_W], True);
		XtSetSensitive (g_w[DCLOSE_W], True);
	    } else {
		XtSetSensitive (g_w[DOPEN_W], False);
		XtSetSensitive (g_w[DCLOSE_W], False);
	    }
	    last_goshutter = go;
	}

	switch (ds) {
	case DS_ABSENT:
	    domelt = LTIDLE;
	    break;

	case DS_ROTATING:	/* FALLTHRU */
	case DS_HOMING:
	    domelt = LTACTIVE;
	    break;

	case DS_STOPPED:
	    if (delra(telstatshmp->dometaz - telstatshmp->domeaz) <= DOMETOL)
		domelt = LTOK;
	    else if (telstatshmp->shutterstate == SH_OPEN)
		domelt = LTWARN; /* open but in wrong position */
	    else
		domelt = LTIDLE;
	    break;

	default:
	    domelt = LTIDLE;
	    break;
	}
	setLt (g_w[DAZLT_W], domelt);

	switch (telstatshmp->shutterstate) {
	case SH_ABSENT:  sol = LTIDLE;   scl = LTIDLE;   break;
	case SH_IDLE:    sol = LTIDLE;   scl = LTIDLE;   break;
	case SH_OPENING: sol = LTACTIVE; scl = LTIDLE;   break;
	case SH_CLOSING: sol = LTIDLE;   scl = LTACTIVE; break;
	case SH_OPEN:    sol = LTOK;     scl = LTIDLE;   break;
	case SH_CLOSED:  sol = LTIDLE;   scl = LTOK;     break;
	default:	 sol = LTIDLE;   scl = LTIDLE;   break;
	}

	if(domealarm) {
	  if(sol != LTIDLE)
	    sol = LTWARN;
	  if(scl != LTIDLE)
	    scl = LTWARN;
	}

	setLt (g_w[DOLT_W], sol);
	setLt (g_w[DCLT_W], scl);

	XmToggleButtonSetState (g_w[DAUTO_W], telstatshmp->autodome, False);

	if (ds != DS_ABSENT) {
	    char buf[128];
	    double tmp;

	    if (ds != DS_HOMING) {
		fs_sexa (buf, raddeg(telstatshmp->domeaz), 4, 3600);
		wtprintf (g_w[PCDAZ_W], "%s", buf);
		if (!XtIsSensitive (g_w[DAZ_W]))
		    wtprintf (g_w[DAZ_W], "%.3s", buf);
	    } else
		wtprintf (g_w[PCDAZ_W], blank);

	    if (telstatshmp->autodome || ds == DS_ROTATING || ds == DS_HOMING) {
		fs_sexa (buf, raddeg(telstatshmp->dometaz), 4, 3600);
		wtprintf (g_w[PTDAZ_W], "%s", buf);
	    } else
		wtprintf (g_w[PTDAZ_W], blank);

	    if (telstatshmp->autodome || ds == DS_ROTATING) {
		tmp = delra (telstatshmp->domeaz - telstatshmp->dometaz);
		fs_sexa (buf, raddeg(tmp), 4, 3600);
		wtprintf (g_w[PDDAZ_W], "%s", buf);
	    } else
		wtprintf (g_w[PDDAZ_W], blank);
	}
}

/* For RCS Only -- Do Not Edit */
static char *rcsid[2] = {(char *)rcsid, "@(#) $RCSfile: update.c,v $ $Date: 2006/01/13 20:42:47 $ $Revision: 1.1.1.1 $ $Name:  $"};
