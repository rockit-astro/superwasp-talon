#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>

#include <Xm/Label.h>
#include <Xm/PushB.h>
#include <Xm/RowColumn.h>
#include <Xm/Form.h>

#include "P_.h"
#include "astro.h"
#include "circum.h"
#include "catalogs.h"
#include "strops.h"
#include "configfile.h"
#include "misc.h"
#include "telenv.h"
#include "telstatshm.h"
#include "xtools.h"
#include "xobs.h"
#include "widgets.h"

#include "clicommands.h"
#include "clitypes.h"
#include "cli.h"
#include "cliutil.h"

static int replycommand (struct cliclient *c, int cmd, short rv, char *msg, char *errstr);
static void cmd_err (char *errstr, const char *fmt, ...);

extern char cli_tmp_image[16384];

static int current_tel_cmd;    /* current command on telescope */
static int current_dome_cmd;   /* current command on dome */

static struct cliclient *current_tel_client;
static struct cliclient *current_dome_client;

void initCliCommand (void) {
  current_tel_cmd = 0;
  current_dome_cmd = 0;
  current_tel_client = (struct cliclient *) NULL;
  current_dome_client = (struct cliclient *) NULL;
}

void clientDisconnect (struct cliclient *c) {
  if(c == current_tel_client) {
    current_tel_client = (struct cliclient *) NULL;
    current_tel_cmd = 0;
  }
  if(c == current_dome_client) {
    current_dome_client = (struct cliclient *) NULL;
    current_dome_cmd = 0;
  }
}

int docommand (struct cliclient *c, unsigned char cmd, int paylen,
	       unsigned char *paybuf, char *errstr) {
  char errbuf[ERRSTR_LEN] = { '\0' }, *optr;
  int rlen, errlen;
  short rv;
 
  struct cli_object obj;
  float off[2];
  char objname[1024], abuf[32], bbuf[32];
  int dtmp;
  char *ctmp;
  unsigned char haxes;

  int *telcmd = (int *) NULL;
  struct cliclient **telclient = (struct cliclient **) NULL;

  errbuf[0] = '\0';

  /* Special handling of abort */
  if(cmd == TCSCMD_ABORT) {
    /* Abort last command */
    switch(c->cmd) {
    case TCSCMD_SLEW_OBJECT:
    case TCSCMD_SLEW_RADEC:
    case TCSCMD_SLEW_ALTAZ:
    case TCSCMD_TRACK_OBJECT:
    case TCSCMD_TRACK_RADEC:
    case TCSCMD_TRACK_ALTAZ:
    case TCSCMD_OFFSET_RADEC:
    case TCSCMD_OFFSET_ALTAZ:
    case TCSCMD_HOME:
      /* Change to a stop */
      cmd = TCSCMD_STOP_TEL;
      break;
    case TCSCMD_DOME:
      /* Change to a stop */
      cmd = TCSCMD_STOP_DOME;
      break;
    case TCSCMD_ENGMODE:
    case TCSCMD_SET_ALARM:
      /* Ignore */
      rv = TCSRET_FAILED;
      goto doreply;
    case TCSCMD_STOP_TEL:
    case TCSCMD_STOP_DOME:
      /* Try it again */
      cmd = c->cmd;
      break;
    case 0:
      /* Nothing */
      cmd_err(errbuf, "no command is running, cannot abort");
      rv = TCSRET_FAILED;
      goto doreply;
    default:
      /* Unknown command */
      cmd_err(errbuf, "unknown command, cannot abort");
      rv = TCSRET_FAILED;
      goto doreply;
    }
  }

  /* Check that device is available and set telcmd */
  if(cmd > 0 && (cmd <= TCSCMD_STOP_TEL || cmd == TCSCMD_HOME)) {
    /* Telescope */
    if(current_tel_cmd && c != current_tel_client) {
      cmd_err(errbuf, "telescope in use by another client");
      rv = TCSRET_BUSY;
      goto doreply;
    }

    telcmd = &current_tel_cmd;
    telclient = &current_tel_client;
  }
  else if(cmd == TCSCMD_STOP_DOME || cmd == TCSCMD_DOME ||
	  cmd == TCSCMD_SET_ALARM) {
    /* Dome */
    if(current_dome_cmd && c != current_dome_client) {
      cmd_err(errbuf, "dome in use by another client");
      rv = TCSRET_BUSY;
      goto doreply;
    }

    telcmd = &current_dome_cmd;
    telclient = &current_dome_client;
  }

  /* Normal handling of commands */
  switch(cmd) {
  case TCSCMD_SLEW_OBJECT:
    if(paylen < 1) {
      cmd_err(errbuf, "missing argument");
      rv = TCSRET_USAGE;
    }
    else if(paylen >= sizeof(objname)-1) {
      cmd_err(errbuf, "object name too long");
      rv = TCSRET_USAGE;
    }
    else {
      memcpy(objname, paybuf, paylen);
      objname[paylen] = '\0';

      if(!strcasecmp(objname, "stow")) {
	if(fifoMsg(Tel_Id, "Alt:%.5f Az:%.6f", STOWALT, STOWAZ) == -1) {
	  cmd_err(errbuf, "telescope not available");
	  rv = TCSRET_FAILED;
	}
	else {
	  msg("CLI: Stowing telescope");
	  
	  rv = TCSRET_ACK;
	  current_tel_cmd = cmd;
	}
      }
      else if(!strcasecmp(objname, "zenith") ||
	      !strcasecmp(objname, "service")) {
	if(fifoMsg(Tel_Id, "Alt:%.5f Az:%.6f", M_PI / 2, 0.0) == -1) {
	  cmd_err(errbuf, "telescope not available");
	  rv = TCSRET_FAILED;
	}
	else {
	  msg("CLI: Slewing to zenith");

	  rv = TCSRET_ACK;
	  current_tel_cmd = cmd;
	}
      }
      else {
	/* lookup object */
	Now *np = &telstatshmp->now;
	char catdir[1024], buf[1024];
	Obj o;

	telfixpath(catdir, "archive/catalogs");
	if(searchDirectory(catdir, objname, &o, buf) < 0) {
	  /* unknown object */
	  cmd_err(errbuf, "unknown object: %s", objname);
	  rv = TCSRET_FAILED;
	}
	else {
	  /* good object */
	  
	  /* compute @ EOD */
	  (void) obj_cir(np, &o);
	  
	  if(fifoMsg(Tel_Id, "Alt:%.5f Az:%.6f", o.s_alt, o.s_az) == -1) {
	    cmd_err(errbuf, "telescope not available");
	    rv = TCSRET_FAILED;
	  }
	  else {
	    fs_sexa(abuf, raddeg(o.s_alt), 3, 3600);
	    fs_sexa(bbuf, raddeg(o.s_az), 3, 3600);
	    msg("CLI: Slewing to object %s at Alt %s Az %s", objname, abuf, bbuf);
	    
	    rv = TCSRET_ACK;
	    current_tel_cmd = cmd;
	  }
	}
      }
    }

    break;
  case TCSCMD_SLEW_RADEC:
    if(paylen != sizeof(struct cli_object)) {
      cmd_err(errbuf, "incorrect payload size, expected %d, got %d",
	      sizeof(struct cli_object), paylen);
      rv = TCSRET_USAGE;
    }
    else {
      Now *np = &telstatshmp->now;
      Obj o;
      double ra, dec, ha;

      memcpy(&obj, paybuf, sizeof(obj));

      ra = obj.ra;
      dec = obj.dec;

      memset(&o, 0, sizeof(o));
      o.o_type = FIXED;

      if(obj.coordtype == 'A') {
	radec2ha (np, ra, dec, &ha);
	ap_as(np, J2000, &ra, &dec);
	o.f_RA = obj.ra;
	o.f_dec = obj.dec;
	o.f_epoch = J2000;
	obj_cir (np, &o);
      }
      else {
	Now n = *np;

	n.n_epoch = obj.equinox;
	radec2ha (np, ra, dec, &ha);
	o.f_RA = obj.ra;
	o.f_dec = obj.dec;
	o.f_epoch = obj.equinox;
	obj_cir (np, &o);
      }

      if(fifoMsg(Tel_Id, "Alt:%.5f Az:%.6f", o.s_alt, o.s_az) == -1) {
	cmd_err(errbuf, "telescope not available");
	rv = TCSRET_FAILED;
      }
      else {
	fs_sexa(abuf, raddeg(o.s_alt), 3, 3600);
	fs_sexa(bbuf, raddeg(o.s_az), 3, 3600);
	msg("CLI: Slewing to Alt %s Az %s", abuf, bbuf);
	
	rv = TCSRET_ACK;
	current_tel_cmd = cmd;
      }
    }
   
    break;
  case TCSCMD_SLEW_ALTAZ:
    if(paylen != sizeof(off)) {
      cmd_err(errbuf, "incorrect payload size, expected %d, got %d",
	      sizeof(off), paylen);
      rv = TCSRET_USAGE;
    }
    else {
      memcpy(off, paybuf, sizeof(off));

      if(fifoMsg(Tel_Id, "Alt:%.5f Az:%.6f", off[0], off[1]) == -1) {
	cmd_err(errbuf, "telescope not available");
	rv = TCSRET_FAILED;
      }
      else {
	fs_sexa(abuf, raddeg(off[0]), 3, 3600);
	fs_sexa(bbuf, raddeg(off[1]), 3, 3600);
	msg("CLI: Slewing to Alt %s Az %s", abuf, bbuf);
	
	rv = TCSRET_ACK;
	current_tel_cmd = cmd;
      }
    }
   
    break;
  case TCSCMD_TRACK_OBJECT:
    if(paylen < 1) {
      cmd_err(errbuf, "missing argument");
      rv = TCSRET_USAGE;
    }
    else if(paylen >= sizeof(objname)-1) {
      cmd_err(errbuf, "object name too long");
      rv = TCSRET_USAGE;
    }
    else {
      int altaz = 0, ok = 1;
      double ra, dec, alt = 0, az = 0;

      memcpy(objname, paybuf, paylen);
      objname[paylen] = '\0';

      if(!strcasecmp(objname, "stow")) {
	alt = STOWALT;
	az = STOWAZ;
	altaz = 1;
      }
      else if(!strcasecmp(objname, "zenith") ||
	      !strcasecmp(objname, "service")) {
	alt = M_PI / 2;
	az = 0.0;
	altaz = 1;
      }
      else {
	/* lookup object */
	Now *np = &telstatshmp->now;
	char catdir[1024], buf[1024];
	Obj o;
	
	telfixpath(catdir, "archive/catalogs");
	if(searchDirectory(catdir, objname, &o, buf) < 0) {
	  /* unknown object */
	  cmd_err(errbuf, "unknown object: %s", objname);
	  rv = TCSRET_FAILED;
	  ok = 0;
	}
	else {
	  /* good object */
	  
	  /* compute @ EOD */
	  (void) obj_cir(np, &o);
	  
	  ra = o.s_ra;
	  dec = o.s_dec;
	}
      }

      if(ok) {
	if(altaz) {
	  Now *np = &telstatshmp->now;
	  double ha, lst;
	  
	  unrefract(temp, pressure, alt, &alt);
	  aa_hadec(lat, alt, az, &ha, &dec);
	  now_lst(np, &lst);
	  ra = hrrad(lst) - ha;
	  range(&ra, 2*PI);
	}
	
	if(fifoMsg(Tel_Id, "RA:%.6f Dec:%.6f", ra, dec) == -1) {
	  cmd_err(errbuf, "telescope not available");
	  rv = TCSRET_FAILED;
	}
	else {
	  fs_sexa(abuf, radhr(ra), 3, 3600);
	  fs_sexa(bbuf, raddeg(dec), 3, 3600);
	  msg("CLI: Track object %s at %s %s", objname, abuf, bbuf);
	  
	  rv = TCSRET_ACK;
	  current_tel_cmd = cmd;
	}
      }
    }
  
    break;
  case TCSCMD_TRACK_RADEC:
    if(paylen != sizeof(struct cli_object)) {
      cmd_err(errbuf, "incorrect payload size, expected %d, got %d",
	      sizeof(struct cli_object), paylen);
      rv = TCSRET_USAGE;
    }
    else {
      memcpy(&obj, paybuf, sizeof(obj));

      fs_sexa(abuf, radhr(obj.ra), 3, 3600);
      fs_sexa(bbuf, raddeg(obj.dec), 3, 3600);

      if(obj.coordtype == 'A') {
	if(fifoMsg(Tel_Id, "RA:%.6f Dec:%.6f", obj.ra, obj.dec) == -1) {
	  cmd_err(errbuf, "telescope not available");
	  rv = TCSRET_FAILED;
	}
	else {
	  msg("CLI: Track %s %s", abuf, bbuf);
	  rv = TCSRET_ACK;
	  current_tel_cmd = cmd;
	}
      }
      else {
	if(fifoMsg(Tel_Id, "RA:%.6f Dec:%.6f Epoch:%g",
		   obj.ra, obj.dec, obj.equinox) == -1) {
	  cmd_err(errbuf, "telescope not available");
	  rv = TCSRET_FAILED;
	}
	else {
	  msg("CLI: Track %s %s %c%.1f", abuf, bbuf, obj.coordtype, obj.equinox);

	  rv = TCSRET_ACK;
	  current_tel_cmd = cmd;
	}
      }
    }
   
    break;
  case TCSCMD_TRACK_ALTAZ:
    if(paylen != sizeof(off)) {
      cmd_err(errbuf, "incorrect payload size, expected %d, got %d",
	      sizeof(off), paylen);
      rv = TCSRET_USAGE;
    }
    else {
      Now *np = &telstatshmp->now;
      double alt, az, ra, dec, ha, lst;

      memcpy(off, paybuf, sizeof(off));

      alt = off[0];
      az = off[1];

      unrefract(temp, pressure, alt, &alt);
      aa_hadec(lat, alt, az, &ha, &dec);
      now_lst(np, &lst);
      ra = hrrad(lst) - ha;
      range(&ra, 2*PI);

      if(fifoMsg(Tel_Id, "RA:%.6f Dec:%.6f", ra, dec) == -1) {
	cmd_err(errbuf, "telescope not available");
	rv = TCSRET_FAILED;
      }
      else {
	fs_sexa(abuf, radhr(ra), 3, 3600);
	fs_sexa(bbuf, raddeg(dec), 3, 3600);
	msg("CLI: Track %s %s", abuf, bbuf);
	
	rv = TCSRET_ACK;
	current_tel_cmd = cmd;
      }
    }
   
    break;
  case TCSCMD_OFFSET_RADEC:
    if(paylen != sizeof(off)) {
      cmd_err(errbuf, "incorrect payload size, expected %d, got %d",
	      sizeof(off), paylen);
      rv = TCSRET_USAGE;
    }
    else {
      double ra, dec, ha;

      memcpy(off, paybuf, sizeof(off));

      /* Check telescope state */
      if(telstatshmp->telstate == TS_HUNTING ||
	 telstatshmp->telstate == TS_TRACKING) {
	/* Calculate new desired position from current one */
	ra = telstatshmp->CARA + off[0];
	dec = telstatshmp->CADec + off[1];

	if(dec < -M_PI/2) {
	  dec = -M_PI - dec;
	  ra += M_PI;
	}
	if(dec > M_PI/2) {
	  dec = M_PI - dec;
	  ra += M_PI;
	}
	
	if(ra < 0)
	  ra += 2*M_PI;
	if(ra > 2*M_PI)
	  ra -= 2*M_PI;

	/* Send the telescope there */
	fs_sexa(abuf, radhr(ra), 3, 3600);
	fs_sexa(bbuf, raddeg(dec), 3, 3600);

	if(fifoMsg(Tel_Id, "RA:%.6f Dec:%.6f", ra, dec) == -1) {
	  cmd_err(errbuf, "telescope not available");
	  rv = TCSRET_FAILED;
	}
	else {
	  msg("CLI: Offset to %s %s", abuf, bbuf);

	  rv = TCSRET_ACK;
	  current_tel_cmd = cmd;
	}
      }
      else {
	Now *np = &telstatshmp->now;
	Now n = *np;
	Obj o;

	memset(&o, 0, sizeof(o));
	o.o_type = FIXED;

	/* Calculate new desired position from current one */
	ra = telstatshmp->CJ2kRA + off[0];
	dec = telstatshmp->CJ2kDec + off[1];
	
	if(dec < -M_PI/2) {
	  dec = -M_PI - dec;
	  ra += M_PI;
	}
	if(dec > M_PI/2) {
	  dec = M_PI - dec;
	  ra += M_PI;
	}

	if(ra < 0)
	  ra += 2*M_PI;
	if(ra > 2*M_PI)
	  ra -= 2*M_PI;

	/* Convert to alt/az */
	n.n_epoch = J2000;
	radec2ha (np, ra, dec, &ha);
	o.f_RA = ra;
	o.f_dec = dec;
	o.f_epoch = J2000;
	obj_cir (np, &o);

	/* Send the telescope there */
	if(fifoMsg(Tel_Id, "Alt:%.5f Az:%.6f", o.s_alt, o.s_az) == -1) {
	  cmd_err(errbuf, "telescope not available");
	  rv = TCSRET_FAILED;
	}
	else {
	  fs_sexa(abuf, raddeg(o.s_alt), 3, 3600);
	  fs_sexa(bbuf, raddeg(o.s_az), 3, 3600);
	  msg("CLI: Offset to Alt %s Az %s", abuf, bbuf);
	  
	  rv = TCSRET_ACK;
	  current_tel_cmd = cmd;
	}
      }
    }

    break;
  case TCSCMD_OFFSET_ALTAZ:
    if(paylen != sizeof(off)) {
      cmd_err(errbuf, "incorrect payload size, expected %d, got %d",
	      sizeof(off), paylen);
      rv = TCSRET_USAGE;
    }
    else {
      double alt, az;

      memcpy(off, paybuf, sizeof(off));

      /* Calculate new desired position from current one */
      alt = telstatshmp->Calt + off[0];
      az = telstatshmp->Caz + off[1];

      if(alt < -M_PI/2) {
	alt = -M_PI - alt;
	az += M_PI;
      }
      if(alt > M_PI/2) {
	alt = M_PI - alt;
	az += M_PI;
      }

      if(az < 0)
	az += 2*M_PI;
      if(az > 2*M_PI)
	az -= 2*M_PI;
      
      /* Check telescope state */
      if(telstatshmp->telstate == TS_HUNTING ||
	 telstatshmp->telstate == TS_TRACKING) {
	Now *np = &telstatshmp->now;
	double ra, dec, ha, lst;

	unrefract(temp, pressure, alt, &alt);
	aa_hadec(lat, alt, az, &ha, &dec);
	now_lst(np, &lst);
	ra = hrrad(lst) - ha;
	range(&ra, 2*PI);
	
	if(fifoMsg(Tel_Id, "RA:%.6f Dec:%.6f", ra, dec) == -1) {
	  cmd_err(errbuf, "telescope not available");
	  rv = TCSRET_FAILED;
	}
	else {
	  fs_sexa(abuf, radhr(ra), 3, 3600);
	  fs_sexa(bbuf, raddeg(dec), 3, 3600);
	  msg("CLI: Offset to %s %s", abuf, bbuf);

	  rv = TCSRET_ACK;
	  current_tel_cmd = cmd;
	}
      }
      else {
	if(fifoMsg(Tel_Id, "Alt:%.5f Az:%.6f", alt, az) == -1) {
	  cmd_err(errbuf, "telescope not available");
	  rv = TCSRET_FAILED;
	}
	else {
	  fs_sexa(abuf, raddeg(alt), 3, 3600);
	  fs_sexa(bbuf, raddeg(az), 3, 3600);
	  msg("CLI: Offset to Alt %s Az %s", abuf, bbuf);
	  
	  rv = TCSRET_ACK;
	  current_tel_cmd = cmd;
	}
      }
    }

    break;
  case TCSCMD_STOP_TEL:

    if(fifoMsg(Tel_Id, "Stop") == -1) {
      cmd_err(errbuf, "telescope not available");
      rv = TCSRET_FAILED;
    }
    else {
      msg("CLI: Stop telescope");
      
      rv = TCSRET_ACK;
      current_tel_cmd = cmd;
    }

    break;
  case TCSCMD_ENGMODE:
    if(paylen != sizeof(int)) {
      cmd_err(errbuf, "incorrect payload size, expected %d, got %d",
	      sizeof(int), paylen);
      rv = TCSRET_USAGE;
    }
    else {
      memcpy(&dtmp, paybuf, sizeof(dtmp));

      if(fifoMsg(Tel_Id, "engmode %d", dtmp) == -1) {
	cmd_err(errbuf, "telescope not available");
	rv = TCSRET_FAILED;
      }
      else {
	msg("CLI: engmode %d", dtmp);
	
	rv = TCSRET_ACK;
	current_tel_cmd = cmd;
      }
    }

    break;
  case TCSCMD_DOME:
    if(paylen != sizeof(int)) {
      cmd_err(errbuf, "incorrect payload size, expected %d, got %d",
	      sizeof(int), paylen);
      rv = TCSRET_USAGE;
    }
    else {
      memcpy(&dtmp, paybuf, sizeof(dtmp));

      if(dtmp == 1) {
	if(fifoMsg(Dome_Id, "open") == -1) {
	  cmd_err(errbuf, "dome not available");
	  rv = TCSRET_FAILED;
	}
	else {
	  msg("CLI: Opening dome");
	  
	  rv = TCSRET_ACK;
	  current_dome_cmd = cmd;
	}
      }
      else if(dtmp == 0) {
        if(fifoMsg(Dome_Id, "close") == -1) {
	  cmd_err(errbuf, "dome not available");
	  rv = TCSRET_FAILED;
	}
	else {
	  msg("CLI: Closing dome");
	  
	  rv = TCSRET_ACK;
	  current_dome_cmd = cmd;
	}
      }
      else {
	cmd_err(errbuf, "invalid dome parameter: %d", dtmp);
	rv = TCSRET_USAGE;
      }
    }

    break;
  case TCSCMD_STOP_DOME:
    
    if(fifoMsg(Dome_Id, "Stop") == -1) {
      cmd_err(errbuf, "dome not available");
      rv = TCSRET_FAILED;
    }
    else {
      msg("CLI: Stop dome");
      
      rv = TCSRET_ACK;
      current_dome_cmd = cmd;
    }

    break;
  case TCSCMD_SET_ALARM:
    if(paylen != sizeof(int)) {
      cmd_err(errbuf, "incorrect payload size, expected %d, got %d",
	      sizeof(int), paylen);
      rv = TCSRET_USAGE;
    }
    else {
      memcpy(&dtmp, paybuf, sizeof(dtmp));

      if(fifoMsg(Dome_Id, "alarmset %d", dtmp) == -1) {
	cmd_err(errbuf, "dome not available");
	rv = TCSRET_FAILED;
      }
      else {
	msg("CLI: Dome alarm %d", dtmp);
	
	rv = TCSRET_ACK;
	current_dome_cmd = cmd;
      }
    }

    break;
  case TCSCMD_HOME:
    if(paylen != sizeof(unsigned char)) {
      cmd_err(errbuf, "incorrect payload size, expected %d, got %d",
	      sizeof(unsigned char), paylen);
      rv = TCSRET_USAGE;
    }
    else {
      char axcodes[4];
      int naxcodes = 0;

      memcpy(&haxes, paybuf, sizeof(haxes));

      /* Form message */
      if(haxes & CLI_HOME_HA)
	axcodes[naxcodes++] = 'H';
      if(haxes & CLI_HOME_DEC)
	axcodes[naxcodes++] = 'D';
      if(haxes & CLI_HOME_ROT)
	axcodes[naxcodes++] = 'R';

      axcodes[naxcodes++] = '\0';  /* null-terminate */

      msg("%d %s", haxes, axcodes);

      if(fifoMsg(Tel_Id, "home %s", axcodes) == -1) {
	cmd_err(errbuf, "telescope not available");
	rv = TCSRET_FAILED;
      }
      else {
	msg("CLI: Home %s", axcodes);
	
	rv = TCSRET_ACK;
	current_tel_cmd = cmd;
      }
    }

    break;
  default:
    cmd_err(errbuf, "unknown command: %d", cmd);
    rv = TCSRET_USAGE;
  }

  /* Send reply */
 doreply:
  errlen = strlen(errbuf);

  rlen = 1 + sizeof(short);
  rlen += sizeof(int) + errlen;

  if(c->outrem < rlen) {
    report_err(errstr, "no buffer space for reply");
    goto error;
  }

  optr = &(c->outbuf[c->outoff]);

  optr[0] = cmd;
  memcpy(&(optr[1]), &rv, sizeof(short));
  memcpy(&(optr[1+sizeof(short)]), &errlen, sizeof(int));
  memcpy(&(optr[1+sizeof(short)+sizeof(int)]), errbuf, errlen);

  if(rv != TCSRET_ACK && cmd != TCSCMD_ABORT) {
    if(telcmd)
      *telcmd = 0;
    if(telclient)
      *telclient = (struct cliclient *) NULL;

    c->cmd = 0;
  }
  else {
    if(telclient)
      *telclient = c;

    c->cmd = cmd;
  }

  c->outoff += rlen;
  c->outrem -= rlen;

  return(0);

 error:
  return(1);
}

void check_tel_reply (int rv, char *buf) {
  char errstr[ERRSTR_LEN];
  short cret;

  buf = sstrip(buf);

  if(current_tel_cmd > 0) {
#ifdef CLI_DEBUG
    msg("DEBUG: check_tel_reply: %d %s", rv, buf);
#endif

    /* Check return value */
    if(rv < 0) {
      /* Failed, notify user */
	cret = TCSRET_FAILED;

      if(replycommand(current_tel_client, current_tel_cmd, cret, buf, errstr))
	fatal(1, "replycommand: %s", errstr);

      current_tel_cmd = 0;
      current_tel_client = (struct cliclient *) NULL;
    }
    else if(rv == 0) {
      /* Check for special cases where we get multiple replies */
      if((current_tel_cmd == TCSCMD_SLEW_OBJECT ||
	  current_tel_cmd == TCSCMD_SLEW_RADEC ||
	  current_tel_cmd == TCSCMD_SLEW_ALTAZ ||
	  current_tel_cmd == TCSCMD_OFFSET_RADEC ||
	  current_tel_cmd == TCSCMD_OFFSET_ALTAZ) && !strncasecmp(buf, "stop", 4))
	/* Not ours */
	;
      else {
	/* Completed */
#ifdef CLI_DEBUG
	msg("DEBUG: sending reply");
#endif

	if(replycommand(current_tel_client, current_tel_cmd, TCSRET_FINISHED, buf, errstr))
	  fatal(1, "replycommand: %s", errstr);
	
	current_tel_cmd = 0;
	current_tel_client = (struct cliclient *) NULL;
      }
    }
    /* else still going */
  }
}

void check_dome_reply (int rv, char *buf) {
  char errstr[ERRSTR_LEN];
  short cret;

  buf = sstrip(buf);
 
  if(current_dome_cmd > 0) {
#ifdef CLI_DEBUG
    msg("DEBUG: check_dome_reply: %d %s", rv, buf);
#endif

    /* Check return value */
    if(rv < 0) {
      /* Failed, notify user */
	cret = TCSRET_FAILED;

      if(replycommand(current_dome_client, current_dome_cmd, cret, buf, errstr))
	fatal(1, "replycommand: %s", errstr);

      current_dome_cmd = 0;
      current_dome_client = (struct cliclient *) NULL;
    }
    else if(rv == 0) {
      /* Completed */
      if(replycommand(current_dome_client, current_dome_cmd, TCSRET_FINISHED, buf, errstr))
	fatal(1, "replycommand: %s", errstr);

      current_dome_cmd = 0;
      current_dome_client = (struct cliclient *) NULL;
    }
    /* else still going */
  }
}

/* Called by GUI to notify of telescope movement */

void cli_move_telescope (void) {
  char errstr[ERRSTR_LEN];

  if(current_tel_cmd > 0) {
#ifdef CLI_DEBUG
    msg("DEBUG: aborting command %d due to GUI interaction", current_tel_cmd);
#endif    

    if(replycommand(current_tel_client, current_tel_cmd,
		    TCSRET_FAILED, "GUI override", errstr))
      fatal(1, "replycommand: %s", errstr);

    current_tel_cmd = 0;
    current_tel_client = (struct cliclient *) NULL;
  }
}

/* Called by GUI to notify of dome movement */

void cli_move_dome (void) {
  char errstr[ERRSTR_LEN];

  if(current_dome_cmd > 0) {
#ifdef CLI_DEBUG
    msg("DEBUG: aborting command %d due to GUI interaction", current_dome_cmd);
#endif    

    if(replycommand(current_dome_client, current_dome_cmd,
		    TCSRET_FAILED, "GUI override", errstr))
      fatal(1, "replycommand: %s", errstr);

    current_dome_cmd = 0;
    current_dome_client = (struct cliclient *) NULL;
  }
}

static int replycommand (struct cliclient *c, int cmd, short rv, char *msg, char *errstr) {
  int errlen = 0, rlen;
  char *optr;

  if(!c)
    return(0);

  rlen = 1 + sizeof(short);
  errlen = strlen(msg);
  rlen += sizeof(int) + errlen;

  if(c->outrem < rlen) {
    report_err(errstr, "no buffer space for reply");
    goto error;
  }

  optr = &(c->outbuf[c->outoff]);

  optr[0] = cmd;
  memcpy(&(optr[1]), &rv, sizeof(short));
  memcpy(&(optr[1+sizeof(short)]), &errlen, sizeof(int));
  memcpy(&(optr[1+sizeof(short)+sizeof(int)]), msg, errlen);

  c->outoff += rlen;
  c->outrem -= rlen;

  cli_add_write(c);

  return(0);

 error:
  return(1);
}

static void cmd_err (char *errstr, const char *fmt, ...) {
  va_list ap;

  va_start(ap, fmt);
  (void) vsnprintf(errstr, ERRSTR_LEN, fmt, ap);
  va_end(ap);
}

