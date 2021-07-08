#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include <Xm/Xm.h>

#include "P_.h"
#include "astro.h"
#include "circum.h"
#include "telstatshm.h"
#include "configfile.h"

#include "xobs.h"

/* variables set from the config files -- see initCfg() */
double MINALT;
double MAXHA;
double MAXDEC;
double SUNDOWN;
double STOWALT;
double STOWAZ;
double SERVICEALT;
double SERVICEAZ;
int OffTargPitch;
int OffTargDuration;
int OffTargPercent;
int OnTargPitch;
int OnTargDuration;
int OnTargPercent;
int BeepPeriod;
char BANNER[80];

double FLATTAZ;          /* telescope azimuth for dome flat */
double FLATTALT;         /* telescope altitude for dome flat */
double FLATDAZ;          /* dome azimuth for dome flat */
double DOMETOL;

int MAXFLINT;

char icfn[] = "archive/config/filter.cfg";
FilterInfo *filtinfo;
int nfilt;
int deffilt;

static char tscfn[] = "archive/config/telsched.cfg";
static char dfn[] = "archive/config/dome.cfg";
static char tcfn[] = "archive/config/telescoped.cfg";

void
initCfg()
{
#define NTCFG   (sizeof(tcfg)/sizeof(tcfg[0]))

	static CfgEntry tcfg[] = {
	    {"MAXFLINT",	CFG_INT, &MAXFLINT},
	};

#define NTSCFG   (sizeof(tscfg)/sizeof(tscfg[0]))

	static CfgEntry tscfg[] = {
	    {"MINALT",		CFG_DBL, &MINALT},
	    {"MAXHA",		CFG_DBL, &MAXHA},
	    {"MAXDEC",		CFG_DBL, &MAXDEC},
	    {"SUNDOWN",		CFG_DBL, &SUNDOWN},
	    {"STOWALT",		CFG_DBL, &STOWALT},
	    {"STOWAZ",		CFG_DBL, &STOWAZ},
	    {"SERVICEALT",	CFG_DBL, &SERVICEALT},
	    {"SERVICEAZ",	CFG_DBL, &SERVICEAZ},
	    {"OffTargPitch",	CFG_INT, &OffTargPitch},
	    {"OffTargDuration",	CFG_INT, &OffTargDuration},
	    {"OffTargPercent",	CFG_INT, &OffTargPercent},
	    {"OnTargPitch",	CFG_INT, &OnTargPitch},
	    {"OnTargDuration",	CFG_INT, &OnTargDuration},
	    {"OnTargPercent",	CFG_INT, &OnTargPercent},
	    {"BeepPeriod",	CFG_INT, &BeepPeriod},
	    {"BANNER",		CFG_STR, BANNER, sizeof(BANNER)},
	};

#define NDCFG   (sizeof(dcfg)/sizeof(dcfg[0]))

	static CfgEntry dcfg[] = {
	    {"FLATTAZ",  CFG_DBL, &FLATTAZ},
            {"FLATTALT", CFG_DBL, &FLATTALT},
            {"FLATDAZ",  CFG_DBL, &FLATDAZ},
	    {"DOMETOL",  CFG_DBL, &DOMETOL},
	};

	char buf[1024];
	int n;

	/* read stuff from telescoped.cfg */
	n = readCfgFile (1, tcfn, tcfg, NTCFG);
	if (n != NTCFG) {
	    cfgFileError (tcfn, n, NULL, tcfg, NTCFG);
	    die();
	}

	/* read stuff from telsched.cfg */
	n = readCfgFile (1, tscfn, tscfg, NTSCFG);
	if (n != NTSCFG) {
	    cfgFileError (tscfn, n, NULL, tscfg, NTSCFG);
	    die();
	}

	/* from dome.cfg */
	n = readCfgFile (1, dfn, dcfg, NDCFG);
	if (n != NDCFG) {
	    cfgFileError (dfn, n, NULL, dcfg, NDCFG);
	    die();
	}

	/* get fresh filter info from filter.cfg -- always 1 even if faked */
	if (filtinfo) {
	    free ((void *)filtinfo);
	    filtinfo = NULL;
	}
	nfilt = readFilterCfg (1, icfn, &filtinfo, &deffilt, buf);
	if (nfilt < 0) {
	    fprintf (stderr, "%s: %s\n", icfn, buf);
	    die();
	}
}
