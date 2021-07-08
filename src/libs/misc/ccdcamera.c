/* follows are generic camera control wrapper functions.
 * they work with either
 *   an OCAAS-compliant camera driver
 *   an auxcam user program.
 *   a camera from Finger Lakes on a parallel port
 *   a remote (or local) camera controlled by a TCP/IP daemon
 *
 * Only one may be used at a time. The choice is determined in setPathCCD().
 *   if auxcam is set, so be it;
 *   else open the path and try an FLI command, if works so be it;
 *   else try an OCAAS command, if works so be it;
 *   else failure.
 *
 * The auxcam program must support the following commands:
 *   -g x:y:w:h:bx:by:open	# test if given exp params are ok
 *   -x dur:x:y:w:h:bx:by:open	# perform specified exposure, pixels on stdout
 *   -k 			# kill current exp, if any
 *   -t temp			# set temp to given degrees C
 *   -T				# current temp on stdout in C
 *   -s open			# immediate shutter open or close
 *   -i				# 1-line camera id string on stdout
 *   -f				# max "w h bx by" on stdout
 *
 * The TCP/IP daemon must be "telserver-like" and support the commands found herein (see sendServerCommand calls)
 *
 * use this to log activities to stderr
#define	CMDTRACE
*/


#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <unistd.h>
#include <netdb.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/socket.h>

#include "ccdcamera.h"
#include "strops.h"
#include "fits.h"
#include "telenv.h"

#include "fli.h"


static int auxExecCmd (char *argv[], int sync, char *msg);
static void auxKill (void);
static int readPix (char *mem, int nbytes, int block, char *errmsg);
static int openCCD(char *errmsg);
static void closeCCD(void);

/* used for kernel driver mode */
static int ccd_fd = -1;		/* cam file descriptor */
static char *ccd_fn = "dev/ccdcamera";

/* used for auxcam user process mode */
static int aux_fd = -1;		/* stdout/err of auxcam process */
static int aux_pid = -1;	/* auxcam pid */
static char *aux_cmd;		/* name of process */
static CCDExpoParams expsav;	/* copy of exposure params to use */

/* used for CCDServer mode */
static int ccdServer = 0;
static int ccdserver_pixpipe; // like fli monitor... fd becomes readable when pixels ready
static int ccdserver_diepipe; // like fli monitor... kills support child in case parent dies
static int whoExposing; // If more than one "camera" app is controlling driver, only 1 can expose/cancel at a time
void freeBinBuffer(void);
char * getBinBuffer(void);
long getBinSize(void);
int getLastFailcode(void);
char *getLastFailMessage(void);
char * getReturnLine(int line);
int getNumReturnLines(void);
int sendServerCommand(char *retBuf, char *fmt, ...);
int initCCDServer(char *host, int port, char *errRet);
void Reconnect(void);
static int ccdserver_monitor(char *err);
static int areWeExposing(void);
#define CCDServerPP  1000000	/* exposure poll period, usecs */

/* used for FLI driver */
#define	FLIPP		500000	/* exposure poll period, usecs */
#define	FLINF		2	/* number of flushes before an exp */
static int fli_use;		/* use fli portion */
static flicam_t fli_cam;	/* one per vitual camera */
static flidev_t fli_dev;	/* one per phy camera */
static int fli_pixpipe;		/* fd becomes readable when pixels ready */
static int fli_diepipe;		/* kills support child in case parent dies */
static int fli_settemp;		/* target temp, C */
static int fli_tempset;		/* whether cooler is on */
static int fli_monitor(char *err);

/* check and save the given exposure parameters.
 * return 0 if ok, else set errmsg[] and return -1.
 */
int
setExpCCD (CCDExpoParams *expP, char *errmsg)
{

	if(ccdServer) {
	    /* save for startExpCCD */
	    expsav = *expP;
		
		return sendServerCommand(errmsg, "TestExpParams %d %d %d %d %d %d %d",
					expP->sx, expP->sy, expP->sw, expP->sh,
					expP->bx, expP->by, expP->shutter);
	}
	else if (aux_cmd) {
	    char *argv[10];
	    char bufs[10][128];
	    int n;

	    /* save for startExpCCD */
	    expsav = *expP;

	    n = 0;
	    sprintf (argv[n] = bufs[n], "%s", basenm(aux_cmd)); n++;
	    sprintf (argv[n] = bufs[n], "-g"); n++;
	    sprintf (argv[n] = bufs[n], "%d:%d:%d:%d:%d:%d:%d",
				expP->sx, expP->sy, expP->sw, expP->sh,
				expP->bx, expP->by, expP->shutter);
				n++;
	    argv[n] = NULL;

	    return (auxExecCmd (argv, 1, errmsg));

	} else if (fli_use) {
	    int ul_x, ul_y, lr_x, lr_y;
	    fliframe_t frametype;
	    long expms;

	    if (FLIGetVisibleArea (fli_cam, &ul_x, &ul_y, &lr_x, &lr_y) != 0) {
		sprintf (errmsg, "Can not get FLI visible area");
		return (-1);
	    }
	    ul_x += expP->sx;
	    ul_y += expP->sy;
	    lr_x = ul_x + expP->sw/expP->bx;
	    lr_y = ul_y + expP->sh/expP->by;
	    if (FLISetImageArea (fli_cam, ul_x, ul_y, lr_x, lr_y) != 0) {
		sprintf (errmsg, "Can not set FLI image area to %d %d %d %d",
							ul_x, ul_y, lr_x, lr_y);
		return (-1);
	    }

	    /* set shutter as desired, or insure closed if <= MIN_TIME */
	    switch (expP->shutter) {
	    case CCDSO_Closed: frametype = FLI_FRAME_TYPE_DARK; break;
	    case CCDSO_Open:   frametype = FLI_FRAME_TYPE_NORMAL; break;
	    default:
		sprintf(errmsg,"FLI supports only Open or Closed shutter mode");
		return (-1);
	    }
	    if (expP->duration <= FLI_MIN_EXPOSURE_TIME) {
		expms = FLI_MIN_EXPOSURE_TIME;
		frametype = FLI_FRAME_TYPE_DARK;
	    } else
		expms = expP->duration;;

	    if (FLISetFrameType (fli_cam, frametype) != 0) {
		sprintf (errmsg, "Can not set FLI frame type to %d",
								(int)frametype);
		return (-1);
	    }
	    if (FLISetExposureTime (fli_cam, expms) != 0) {
		sprintf(errmsg, "Can not set FLI exposure time to %ldms",expms);
		return (-1);
	    }

	    if (FLISetBitDepth (fli_cam, FLI_MODE_16BIT) != 0) {
		sprintf (errmsg, "Can not set FLI bit depth to 16");
		return (-1);
	    }
	    if (FLISetHBin (fli_cam, expP->bx) != 0) {
		sprintf(errmsg, "Can not set FLI Hbinning to %d", expP->bx);
		return (-1);
	    }
	    if (FLISetVBin (fli_cam, expP->by) != 0) {
		sprintf(errmsg, "Can not set FLI Vbinning to %d", expP->by);
		return (-1);
	    }
	    if (FLISetNFlushes (fli_cam, FLINF) != 0) {
		sprintf(errmsg, "Can not set FLI n flushes");
		return (-1);
	    }
	    return (0);

	} else {
	    int s;
	    if (openCCD (errmsg) < 0)
		return (-1);
	    s = ioctl (ccd_fd, CCD_SET_EXPO, expP);
	    if (s < 0) {
		switch (errno) {
		case ENXIO:
		    (void) sprintf (errmsg, "CCD exposure parameters error.");
		    break;
		default:
		    (void) sprintf (errmsg, "CCD error: %s",strerror(errno));
		    break;
		}
		closeCCD();
	    }

	    return (s);
	}
}

/* start an exposure, as previously defined via setExpCCD().
 * we return immediately but leave things open so ccd_fd or aux_fd can be used.
 * return 0 if ok, else set errmsg[] and return -1.
 */
int
startExpCCD (char *errmsg)
{
    char dummy[1];

	if(ccdServer) {
		int rt = sendServerCommand(errmsg, "StartExpose %d %d %d %d %d %d %d %d",
				    expsav.duration, expsav.sx, expsav.sy, expsav.sw, expsav.sh,
				    expsav.bx, expsav.by, expsav.shutter);
		if(rt == 0) {
			rt = ccdserver_monitor(errmsg);
		}
		return rt;
	}
	else if (aux_cmd) {
	    char *argv[10];
	    char bufs[10][128];
	    int n;

	    n = 0;
	    sprintf (argv[n] = bufs[n], "%s", basenm(aux_cmd)); n++;
	    sprintf (argv[n] = bufs[n], "-x"); n++;
	    sprintf (argv[n] = bufs[n], "%d:%d:%d:%d:%d:%d:%d:%d",
		    expsav.duration, expsav.sx, expsav.sy, expsav.sw, expsav.sh,
		    expsav.bx, expsav.by, expsav.shutter); n++;
	    argv[n] = NULL;

	    return (auxExecCmd (argv, 0, errmsg));
	    
	} else if (fli_use) {
	    if (FLIExposeFrame (fli_cam) != 0) {
		sprintf (errmsg, "Can not start FLI expose");
		return (-1);
	    }
	    if (fli_monitor(errmsg) < 0)
		return (-1);	
	} else {
	    if (openCCD (errmsg) < 0)
		return (-1);

	    /* make sure it's nonblocking, so the subsequent read just
	     * starts it
	     */
	    if (fcntl (ccd_fd, F_SETFL, O_NONBLOCK) < 0) {
		(void) sprintf (errmsg, "startExpCCD() fcntl: %s",
							    strerror (errno));
		closeCCD();
		return (-1);
	    }

	    /* start the exposure by starting a read.
	     * we need not supply a buffer since ccd_fd is open for
	     * nonblocking i/o and the first call always just starts the i/o.
	     * but we do have to supply a positive count so the read calls the
	     * driver at all.
	     */
	    if (read (ccd_fd, dummy, 1) < 0 && errno != EAGAIN) {
		(void) sprintf (errmsg, "startExpCCD(): %s", strerror(errno));
		closeCCD();
		return (-1);
	    }
	}

	return (0);
}

/* set up for a new drift scan.
 * return 0 if ok else -1 with errmsg.
 * N.B. leave camera open of ok.
 */
int
setupDriftScan (CCDDriftScan *dsip, char *errmsg)
{
	if(ccdServer) {
		/* Not sure we CAN support this externally...
		int rt =  sendServerCommand(errmsg, "SetDriftScan");
		if(rt == 0) {
			rt = ccdserver_monitor(errmsg);
		}
		return rt;
		*/
	    strcpy (errmsg, "Drift scanning not supported");
	    return (-1);
	}
	else if (aux_cmd) {
	    strcpy (errmsg, "Drift scanning not supported");
	    return (-1);
	} else if (fli_use) {
	    sprintf (errmsg, "FLI does not support drift scan");
	    return (-1);
	} else {
	    if (openCCD (errmsg) < 0)
		return (-1);
	    if (ioctl (ccd_fd, CCD_DRIFT_SCAN, dsip) < 0) {
		strcpy (errmsg, strerror(errno));
		closeCCD();
		return (-1);
	    }
	    return (0);
	}
}

/* abort a current exposure, if any.
 */
void
abortExpCCD()
{
	if(ccdServer) {
		char buf[256];

		if (ccdserver_pixpipe > 0) {
			close (ccdserver_pixpipe);
			ccdserver_pixpipe = 0;
			close (ccdserver_diepipe);
			ccdserver_diepipe = 0;
		}

		if(areWeExposing()) {
			(void) sendServerCommand(buf, "CancelExposure");
			whoExposing = 0;
		}
	}
	else if (aux_cmd) {
	    char *argv[10];
	    char buf[10][32];
	    char msg[1024];
	    int n;

	    n = 0;
	    sprintf (argv[n] = buf[n], "%s", basenm(aux_cmd)); n++;
	    sprintf (argv[n] = buf[n], "-k"); n++;
	    argv[n] = NULL;

	    (void) auxExecCmd (argv, 1, msg);
	    auxKill();
	} else if (fli_use) {
	    if (fli_pixpipe > 0) {
		close (fli_pixpipe);
		fli_pixpipe = 0;
		close (fli_diepipe);
		fli_diepipe = 0;
	    } else
		(void) FLICancelExposure(fli_cam);
	} else {
	    /* just closing the driver will force an exposure abort if needed */
	    closeCCD();
	}
}

/* after calling startExpCCD() or setupDriftScan(), call this to obtain a
 * handle which is suitable for use with select(2) to wait for pixels to be
 * ready. return file descriptor if ok, else fill errmsg[] and return -1.
 */
int
selectHandleCCD (char *errmsg)
{
	if(ccdServer) {
	    if (ccdserver_pixpipe)
			return (ccdserver_pixpipe);
	    else {
			sprintf (errmsg, "CCD Server not exposing");
		return (-1);
	    }
	}
	else if (aux_cmd) {
	    if (aux_fd < 0) {
		strcpy (errmsg, "selectHandleCCD() before startExpCCD()");
		return (-1);
	    } else 
		return (aux_fd);
	} else if (fli_use) {
	    if (fli_pixpipe)
		return (fli_pixpipe);
	    else {
		sprintf (errmsg, "FLI not exposing");
		return (-1);
	    }
	} else {
	    if (openCCD (errmsg) < 0)
		return(-1);
	    return (ccd_fd);
	}
}

/* after calling startExpCCD() call this to read the pixels and close camera.
 * if the pixels are not yet ready, return will be -1 and errno EAGAIN.
 * return 0 if ok, else set errmsg[] and return -1.
 * N.B. must supply enough mem for entire read.
 */
int
readPixelNBCCD (char *mem, int nbytes, char *errmsg)
{
	return (readPix (mem, nbytes, 0, errmsg));
}

/* after calling startExpCCD() call this to read the pixels and close camera.
 * if the pixels are not yet ready, we block until they are.
 * return 0 if ok, else set errmsg[] and return -1.
 * N.B. must supply enough mem for entire read.
 */
int
readPixelCCD (char *mem, int nbytes, char *errmsg)
{
	return (readPix (mem, nbytes, 1, errmsg));
}

/* set up the cooler according to tp.
 * return 0 if ok, else set errmsg[] and return -1.
 * leave camera closed when finished if it was when we were called.
 */
int
setTempCCD (CCDTempInfo *tp, char *errmsg)
{
	if(ccdServer) {
		int rt;
	    if(tp->s == CCDTS_OFF) {
	    	rt = sendServerCommand(errmsg,"SetTemp OFF");
	    } else {
		    rt = sendServerCommand(errmsg,"SetTemp %d", tp->t);
		}
		return rt;
	} else if (aux_cmd) {
	    char *argv[10];
	    char buf[10][32];
	    int n;

	    n = 0;
	    sprintf (argv[n] = buf[n], "%s", basenm(aux_cmd)); n++;
	    sprintf (argv[n] = buf[n], "-t"); n++;
	    // STO 10-22-02: Support "OFF" as well
	    if(tp->s == CCDTS_OFF) {
	    	sprintf(argv[n] = buf[n],"%s","OFF"); n++;
	    } else {
		    sprintf (argv[n] = buf[n], "%d", tp->t); n++;
		}
	    argv[n] = NULL;

	    return (auxExecCmd (argv, 1, errmsg));
	} else if (fli_use) {
	    if (tp->s == CCDTS_SET) {
		if (FLISetTemperature (fli_dev, (double)tp->t) != 0) {
		    sprintf (errmsg, "Can not set FLI temp to %d", tp->t);
		    return (-1);
		}
		fli_settemp = tp->t;
		fli_tempset = 1;
	    } else
		fli_tempset = 0;
	    return (0);
	} else {
	    int wasclosed = ccd_fd < 0;
	    int s;

	    if (openCCD (errmsg) < 0)
		return(-1);
	    s = ioctl (ccd_fd, CCD_SET_TEMP, tp);
	    if (s < 0)
		strcpy (errmsg, "Cooler command error");
	    if (wasclosed)
		closeCCD();
	    return (s);
	}
}

/* fetch the camera temp, in C.
 * return 0 if ok, else set errmsg[] and return -1.
 * leave camera closed when finished if it was when we were called.
 */
int
getTempCCD (CCDTempInfo *tp, char *errmsg)
{
	if(ccdServer || aux_cmd) {
		char *p;
		
		if(ccdServer) {
			if(sendServerCommand(errmsg,"GetTemp") < 0) {
				return -1;
			}
			
		} else if (aux_cmd) {
		    char *argv[10];
		    char buf[10][32];
	    	int n;
	
		    n = 0;
		    sprintf (argv[n] = buf[n], "%s", basenm(aux_cmd)); n++;
		    sprintf (argv[n] = buf[n], "-T"); n++;
		    argv[n] = NULL;

		    if (auxExecCmd (argv, 1, errmsg) < 0)
			return (-1);
		}
		
		// Aux cmd and CCDServer share this...
		// STO 10-22-02: Extend this to support return of status string also
		p = strtok(errmsg," ");
		if(!p) p = errmsg;
	    tp->t = atoi(p);
	    p = strtok(NULL," \r\n\0");
	    if(p) {
	    	if(!strcasecmp(p,"ERR")) {
	    		tp->s = CCDTS_ERR;
	    	}
	    	else if(!strcasecmp(p,"UNDER")) {
	    		tp->s = CCDTS_UNDER;
	    	}
	    	else if(!strcasecmp(p,"OVER")) {
	    		tp->s = CCDTS_OVER;
	    	}
	    	else if(!strcasecmp(p,"OFF")) {
	    		tp->s = CCDTS_OFF;
	    	}
	    	else if(!strcasecmp(p,"RDN")) {
	    		tp->s = CCDTS_RDN;
	    	}
	    	else if(!strcasecmp(p,"RUP")) {
	    		tp->s = CCDTS_RUP;
	    	}
	    	else if(!strcasecmp(p,"STUCK")) {
	    		tp->s = CCDTS_STUCK;
	    	}
	    	else if(!strcasecmp(p,"MAX")) {
	    		tp->s = CCDTS_MAX;
	    	}
	    	else if(!strcasecmp(p,"AMB")) {
	    		tp->s = CCDTS_AMB;
	    	}
	    	else if(!strcasecmp(p,"COOLING")) {
	    		// this generic return will assume we are ramping down...
	    		tp->s = CCDTS_RDN;
	    	}
			else {
				tp->s = CCDTS_AT; 	// default / old version behavior
			}	    	
	    }
	    else{
	      tp->s = CCDTS_AT;	// Default / old version behavior
		}
	    return (0);

	} else if (fli_use) {
	    double t;

	    if (FLIGetTemperature (fli_dev, &t) != 0) {
		sprintf (errmsg, "Can not get FLI temp");
		return (-1);
	    }
	    tp->t = (int)floor(t+.5);

	    if (fli_tempset) {
		tp->s = tp->t < fli_settemp ? CCDTS_UNDER :
			tp->t > fli_settemp ? CCDTS_RDN :
			CCDTS_AT;
	    } else {
		tp->s = CCDTS_OFF;
	    }
	    return (0);

	} else {
	    int wasclosed = ccd_fd < 0;
	    int s;

	    if (openCCD (errmsg) < 0)
		return (-1);
	    s = ioctl (ccd_fd, CCD_GET_TEMP, tp);
	    if (s < 0)
		strcpy (errmsg, "Error fetching camera temperature");
	    if (wasclosed)
		closeCCD();
	    return (s);
	}
}

/* immediate shutter control: open or close.
 * return 0 if ok, else set errmsg[] and return -1.
 */
int
setShutterNow (int open, char *errmsg)
{
	if(ccdServer) {
		return sendServerCommand(errmsg,"SetShutterNow %d",open);
	}
	else if (aux_cmd) {
	    char *argv[10];
	    char buf[10][32];
	    int n;

	    n = 0;
	    sprintf (argv[n] = buf[n], "%s", basenm(aux_cmd)); n++;
	    sprintf (argv[n] = buf[n], "-s"); n++;
	    sprintf (argv[n] = buf[n], "%d", open); n++;
	    argv[n] = NULL;

	    return (auxExecCmd (argv, 1, errmsg));
	} if (fli_use) {
	    sprintf (errmsg, "FLI does not support direct shutter control");
	    return (-1);
	} else {
	    int wasclosed = ccd_fd < 0;
	    int s;

	    if (openCCD (errmsg) < 0)
		return (-1);
	    s = ioctl (ccd_fd, CCD_SET_SHTR, open);
	    if (s < 0)
		strcpy (errmsg, "Shutter error");
	    if (wasclosed)
		closeCCD();
	    return (s);
	}
}

/* fetch the camera ID string.
 * return 0 if ok, else set errmsg[] and return -1.
 * leave camera closed when finished if it was when we were called.
 */
int
getIDCCD (char buf[], char *errmsg)
{
	if(ccdServer || aux_cmd) {
	    int n;
		
		if(ccdServer) {
			if(sendServerCommand(errmsg,"GetIDString") < 0) {
				return -1;
			}
		} else if (aux_cmd) {
		    char *argv[10];
		    char cbuf[10][32];

		    n = 0;
		    sprintf (argv[n] = cbuf[n], "%s", basenm(aux_cmd)); n++;
	    	sprintf (argv[n] = cbuf[n], "-i"); n++;
		    argv[n] = NULL;

		    if (auxExecCmd (argv, 1, errmsg) < 0)
			return (-1);
		}
	    n = strlen(errmsg);
	    if (errmsg[n-1] == '\n')
		errmsg[n-1] = '\0';
	    strcpy (buf, errmsg);
	    return (0);
	} if (fli_use) {
	    int rev;
	    char model[100];

	    if (FLIGetFirmwareRev (fli_dev, &rev) != 0) {
		strcpy (errmsg, "Can not get FLI rev");
		return(-1);
	    }
	    if (FLIGetModel (fli_dev, model, sizeof(model)) != 0) {
		strcpy (errmsg, "Can not get FLI model");
		return(-1);
	    }
	    sprintf (buf, "FLI %s Rev %d", model, rev);
	    return (0);
	} else {
	    int wasclosed = ccd_fd < 0;
	    int s;

	    if (openCCD (errmsg) < 0)
		return (-1);
	    s = ioctl (ccd_fd, CCD_GET_ID, buf);
	    if (s < 0)
		strcpy (errmsg, "Can not get camera ID string");
	    if (wasclosed)
		closeCCD();
	    return (s);
	}
}

/* fetch the max camera settings.
 * return 0 if ok, else set errmsg[] and return -1.
 * leave camera closed when finished if it was when we were called.
 */
int
getSizeCCD (CCDExpoParams *cep, char *errmsg)
{	
	if(ccdServer || aux_cmd) {
		if(ccdServer) {
			if(sendServerCommand(errmsg,"GetMaxSize") < 0) {
				return -1;
			}
		} else if (aux_cmd) {
		    char *argv[10];
		    char buf[10][32];
	    	int n;

		    n = 0;
		    sprintf (argv[n] = buf[n], "%s", basenm(aux_cmd)); n++;
	    	sprintf (argv[n] = buf[n], "-f"); n++;
		    argv[n] = NULL;

		    if (auxExecCmd (argv, 1, errmsg) < 0)
			return (-1);
		}
		// CCD and Aux share this
	    if (sscanf (errmsg, "%d %d %d %d", &cep->sw, &cep->sh, &cep->bx,
							    &cep->by) != 4)
		return (-1);
	    return (0);
	} else if (fli_use) {
	    int ul_x, ul_y, lr_x, lr_y;

	    if (FLIGetVisibleArea (fli_cam, &ul_x, &ul_y, &lr_x, &lr_y) != 0) {
		sprintf (errmsg, "Can not get FLI size");
		return (-1);
	    }
	    cep->sw = lr_x - ul_x;
	    cep->sh = lr_y - ul_y;
	    cep->bx = 10;			/* ?? */
	    cep->by = 10;			/* ?? */
	    return (0);
	} else {
	    int wasclosed = ccd_fd < 0;
	    int s;

	    if (openCCD (errmsg) < 0)
		return (-1);
	    s = ioctl (ccd_fd, CCD_GET_SIZE, cep);
	    if (s < 0)
		strcpy (errmsg, "Can not get camera size info");
	    if (wasclosed)
		closeCCD();
	    return (s);
	}
}

/* set a path to a driver or auxcam program.
 * N.B. path[] memory must be persistent.
 * return 0 if tests ok, else -1.
 */
int
setPathCCD (char *path, int auxcam, char *msg)
{
	fliport_t fli_port;
	char *p;

	/* last-ditch default */
	if (path && !path[0])
	    strcpy (path, "dev/ccdcamera");

	// if path looks like a host:port name, then it must be a CCD Server
	if((p = strchr(path,':'))) {
		ccdServer = 1; // we must be a ccd server!
		*p++ = 0;
		/*return*/initCCDServer(path, atoi(p), msg);
		return 0; // don't return error... it may just be offline, and our reconnect will pick it up if it comes back later.
	} else {
		ccdServer = 0; // let auxcam setting determine what we are...
	}
	
	telfixpath (path, path);
	
	aux_cmd = NULL; // default to null, will set if we set path

	if (auxcam) {
	    char *argv[10];
	    char buf[10][32];
	    int n;

	    n = 0;
	    aux_cmd = path;
	    sprintf (argv[n] = buf[n], "%s", basenm(aux_cmd)); n++;
	    sprintf (argv[n] = buf[n], "-f"); n++;
	    argv[n] = NULL;

	    return (auxExecCmd (argv, 1, msg));
	} else if (fli_use || FLIInit (path, &fli_port) == 0) {
	    if (fli_use) {
		/* being reopened */
		if (fli_pixpipe) {
		    close (fli_pixpipe);
		    fli_pixpipe = 0;
		    close (fli_diepipe);
		    fli_diepipe = 0;
		}
		FLIClose (fli_cam);
		FLIExit();
		if (FLIInit (path, &fli_port) != 0) {
		    sprintf (msg, "Can not init FLI port");
		    return (-1);
		}
		fli_use = 0;
	    }

	    if (FLIOpen (fli_port, NULL, &fli_cam) < 0) {
		sprintf (msg, "Can not get FLI cam");
		return (-1);
	    }
	    if (FLIGetDevice (fli_cam, &fli_dev) < 0) {
		sprintf (msg, "Can not get FLI dev");
		return (-1);
	    }
	    fli_use = 1;
	} else {
	    /* OCAAS interface */
	    int wasclosed = ccd_fd < 0;

	    ccd_fn = path;
	    if (openCCD (msg) < 0)
		return (-1);
	    if (wasclosed)
		closeCCD();
	}

	return (0);
}

/* exec aux_cmd with the given args and capture its stdout/err.
 * if pipe/fork/exec fails return -1 and fill msg with strerror.
 * if <sync> then wait and return -(exit status) and any output in msg,
 * else set aux_fd to a file des to aux_cmd and set aux_pid and return 0.
 */
static int
auxExecCmd (char *argv[], int sync, char *msg)
{
	int pid, fd;
	int p[2];
	int i;

#ifdef CMDTRACE
	char *retbuf = msg;
	for (i = 0; argv[i]; i++)
	    fprintf (stderr, "auxExecCmd argv[%d] = '%s'\n", i, argv[i]);
#endif

	if (pipe(p) < 0) {
	    strcpy (msg, strerror(errno));
	    return (-1);
	}

	pid = fork();
	if (pid < 0) {
	    /* failed */
	    close (p[0]);
	    close (p[1]);
	    strcpy (msg, strerror(errno));
	    return (-1);
	}

	if (pid == 0) {
	    /* child */
	    close (p[0]);
	    close (0);
	    dup2 (p[1], 1);
	    close (p[1]);
	    for (i = 2; i < 100; i++)
		close (i);
	    execvp (aux_cmd, argv);
	    exit (errno);
	}

	/* parent continues.. */
	close (p[1]);
	fd = p[0];
	if (sync)  {
	    /* read response, gather exit status */
	    while ((i = read (fd, msg, 1024)) > 0)
		msg += i;
	    *msg = '\0';
	    close (fd);
	    wait (&i);
	    if (WIFEXITED(i))
		i = -WEXITSTATUS(i);
	    else
		i = -(32 + WTERMSIG(i));
	    if (i != 0 && strlen(msg) == 0)
		sprintf (msg, "Error from %s: %s", aux_cmd, strerror(-i));
	} else {
	    /* let process run, store info for later use, presumably readPix */
	    aux_fd = fd;
	    aux_pid = pid;
	    i = 0;
	}

#ifdef CMDTRACE
	    fprintf (stderr, "auxExecCmd returns %d\n", i);
	    if(sync) fprintf(stderr,"  return buffer: %s\n",retbuf);
#endif
	return (i);
}

static void
auxKill ()
{
	if (aux_pid > 0) {
	    kill (aux_pid, SIGKILL);
	    aux_pid = -1;
	}
	wait (NULL);
	if (aux_fd >= 0) {
	    close (aux_fd);
	    aux_fd = -1;
	}
}

/* read nbytes worth the pixels into mem[].
 * if `block' we wait even if none are ready now, else only wait if some are
 * ready on the first read.
 */
static int
readPix (char *mem, int nbytes, int block, char *errmsg)
{
	int fflags = block ? 0 : O_NONBLOCK;

	if(ccdServer) {
		// do same way as FLI pipe...
	    struct timeval tv, *tvp;
	    size_t bytesgrabbed;
	    fd_set rs;
	    char *memend;

	    if (!ccdserver_pixpipe) {
			strcpy (errmsg, "CCD Server not exposing");
			return (-1);
	    }

	    /* block or just check fli_pipe */
	    FD_ZERO(&rs);
	    FD_SET(ccdserver_pixpipe, &rs);
	    if (block) {
			tvp = NULL;
	    } else {
			tv.tv_sec = 0;
			tv.tv_usec = 0;
			tvp = &tv;
	    }
	    switch (select (ccdserver_pixpipe+1, &rs, NULL, NULL, tvp)) {
		    case 1:	/* ready */
				break;
		    case 0:	/* timed out -- must be non-blocking */
				sprintf (errmsg, "CCD Server is Exposing");
				return (-1);
		    case -1:
	    	default:
				sprintf (errmsg, "CCD Server %s", strerror(errno));
				return (-1);
	    }

	    close (ccdserver_pixpipe);
	    ccdserver_pixpipe = 0;
	    close (ccdserver_diepipe);
	    ccdserver_diepipe = 0;

		if(sendServerCommand(errmsg,"GetPixels") < 0) {
			return -1;
		}

		bytesgrabbed = getBinBuffer() ? getBinSize() : 0;
		
	    if (nbytes > bytesgrabbed) {
			sprintf (errmsg, "CCD Server %d bytes short", nbytes-bytesgrabbed);
			freeBinBuffer();
			return (-1);
	    }
	
		memcpy(mem,getBinBuffer(),nbytes);
		freeBinBuffer();
	
	    /* byte swap FITS to our internal format */
	    for (memend = mem+nbytes; mem < memend; ) {
			char tmp = *mem++;
			mem[-1] = *mem;
			*mem++ = tmp;
	    }
		
		return(0);
	
	} else if (aux_cmd) {
	    int ntot, nread;
	    char *memend;

	    if (aux_fd < 0) {
		(void) strcpy (errmsg, "camera not open");
		return (-1);
	    }

	    if (fcntl (aux_fd, F_SETFL, fflags) < 0) {
		(void) sprintf (errmsg, "fcntl(%d): %s",fflags,strerror(errno));
		auxKill();
		return (-1);
	    }

	    /* first pixel is just "heads up" */
	    nread = read (aux_fd, mem, 1);
	    if (nread < 0) {
		if (!block && errno == EAGAIN) {
		    return (0);
		} else {
		    strcpy (errmsg, strerror(errno));
		    auxKill();
		    return (-1);
		}
	    }

	    for (ntot = 0; ntot < nbytes; ntot += nread) {
		nread = read (aux_fd, mem+ntot, nbytes-ntot);
		if (nread <= 0) {
		    if (nread < 0)
			strcpy (errmsg, strerror(errno));
		    else {
			/* if smallish # bytes probably an error report */
			if (ntot < 100)
			    sprintf (errmsg, "%.*s", ntot-1, mem); /* no \n */
			else
			    sprintf (errmsg, "Unexpected EOF after %d bytes",
									ntot);
		    }
		    auxKill();
		    return (-1);
		}
	    }

	    /* byte swap FITS to our internal format */
	    for (memend = mem+nbytes; mem < memend; ) {
		char tmp = *mem++;
		mem[-1] = *mem;
		*mem++ = tmp;
	    }

	    /* mop up */
	    auxKill();
	    return (0);

	} else if (fli_use) {
	    struct timeval tv, *tvp;
	    size_t bytesgrabbed;
	    fd_set rs;

	    if (!fli_pixpipe) {
		strcpy (errmsg, "FLI not exposing");
		/* TODO cleanup */
		return (-1);
	    }

	    /* block or just check fli_pipe */
	    FD_ZERO(&rs);
	    FD_SET(fli_pixpipe, &rs);
	    if (block)
		tvp = NULL;
	    else {
		tv.tv_sec = 0;
		tv.tv_usec = 0;
		tvp = &tv;
	    }
	    switch (select (fli_pixpipe+1, &rs, NULL, NULL, tvp)) {
	    case 1:	/* ready */
		break;
	    case 0:	/* timed out -- must be non-blocking */
		sprintf (errmsg, "FLI is Exposing");
		return (-1);
	    case -1:
	    default:
		sprintf (errmsg, "FLI %s", strerror(errno));
		return (-1);
	    }

	    close (fli_pixpipe);
	    fli_pixpipe = 0;
	    close (fli_diepipe);
	    fli_diepipe = 0;

		if (FLIReadFrame(fli_cam, mem, nbytes, &bytesgrabbed) != 0) {
		sprintf (errmsg, "FLI error reading pixels");
		return (-1);
	    }
	    if (nbytes != bytesgrabbed) {
		sprintf (errmsg, "FLI %d bytes short", nbytes-bytesgrabbed);
		return (-1);
	    }

	    return (0);
	} else {
	    int nread;
	    int s;

	    if (ccd_fd < 0) {
		(void) strcpy (errmsg, "camera not open");
		return (-1);
	    }
	    if (fcntl (ccd_fd, F_SETFL, fflags) < 0) {
		(void) sprintf (errmsg, "fcntl(%d): %s",fflags,strerror(errno));
		closeCCD();
		return (-1);
	    }

	    /* must read all pixels in one shot from driver */
	    s = 0;
	    nread = read (ccd_fd, mem, nbytes);
	    if (nread != nbytes) {
		if (nread < 0)
		    (void) sprintf (errmsg, "%s", strerror (errno));
		else
		    (void) sprintf (errmsg, "%d but expected %d",nread,nbytes);
		s = -1;
	    }

	    closeCCD();
	    return (s);
	}
}

/* open the CCD camera with O_NONBLOCK and save filedes in ccd_fd.
 * return 0 if ok, else fill errmsg[] and return -1.
 */
static int
openCCD(char *errmsg)
{
	if (ccd_fd >= 0)
	    return (0);

	ccd_fd = telopen (ccd_fn, O_RDWR | O_NONBLOCK);
	if (ccd_fd < 0) {
	    switch (errno) {
	    case ENXIO:
		strcpy (errmsg, "Camera not responding.");
		break;
	    case ENODEV:
		strcpy (errmsg, "Camera not present.");
		break;
	    case EBUSY:
		strcpy (errmsg, "Camera is currently busy.");
		break;
	    default:
		sprintf (errmsg, "%s: %s", ccd_fn, strerror (errno));
		break;
	    }
	    return (-1);
	}
	return (0);
}

static void
closeCCD()
{
	if (ccd_fd >= 0) {
	    (void) close (ccd_fd);
	    ccd_fd = -1;
	}
}

/* create a child helper process that monitors an exposure. set fli_pixpipe
 *   to a file descriptor from which the parent (us) will read EOF when the
 *   current exposure completes. set fli_diepipe to a fd from which the child
 *   will read EOF if we die or intentionally wish to cancel exposure.
 * return 0 if process is underway ok, else -1.
 */
static int
fli_monitor(err)
char *err;
{
	int pixp[2], diep[2];
	int pid;

	if (pipe(pixp) < 0 || pipe(diep)) {
	    sprintf (err, "FLI pipe: %s", strerror(errno));
	    return (-1);
	}

	signal (SIGCHLD, SIG_IGN);	/* no zombies */
	pid = fork();
	if (pid < 0) {
	    sprintf (err, "FLI fork: %s", strerror(errno));
	    close (pixp[0]);
	    close (pixp[1]);
	    close (diep[0]);
	    close (diep[1]);
	    return(-1);
	}

	if (pid) {
	    /* parent */
	    fli_pixpipe = pixp[0];
	    close (pixp[1]);
	    fli_diepipe = diep[1];
	    close (diep[0]);
	    return (0);
	}

	/* child .. poll for exposure or cancel if EOF on diepipe */
	close (pixp[0]);
	close (diep[1]);
	while (1) {
	    struct timeval tv;
	    fd_set rs;
	    long left;

	    tv.tv_sec = FLIPP/1000000;
	    tv.tv_usec = FLIPP%1000000;

	    FD_ZERO(&rs);
	    FD_SET(diep[0], &rs);

	    switch (select (diep[0]+1, &rs, NULL, NULL, &tv)) {
	    case 0: 	/* timed out.. time to poll camera */
		if (FLIGetExposureStatus (fli_cam, &left) != 0)
		    _exit(1);
		if (!left)
		    _exit(0);
		break;
	    case 1:	/* parent died or gave up */
		(void) FLICancelExposure(fli_cam);
		_exit(0);
		break;
	    default:	/* local trouble */
		(void) FLICancelExposure(fli_cam);
		_exit(1);
		break;
	    }
	}
}

// =============================================================================================

/*
	CCD Server support
	S. Ohmert 10-25-02
	
	As a better alternative to the script-based auxcam, this will support
	TCP/IP based communication with a server that controls the camera.
	The server is assumed to be a daemon running that will accept the commands
	given and return information according to the basic "telserver" concept.
	Commands are ascii, typically terminated by \r\n.
	Return blocks are preceded by *>>>\r\n and concluded with *<<<\r\n
	Return data is found in the lines between.
	
*/

typedef int Boolean;
#define FALSE (0)
#define TRUE (!FALSE)

typedef int	SOCKET;
#define SOCKET_ERROR -1
#define INVALID_SOCKET -1
typedef struct sockaddr SOCKADDR;

// local variables
static int		command_timeout = 20;		// timeout for inactivity
static SOCKET 	sockfd = INVALID_SOCKET;    // fd of server
static SOCKET 	sockfd2 = INVALID_SOCKET;    // second connection used by monitor
static Boolean errorExit;
// Buffers used when reading a return block
// maximum size of an input line
#define MAXLINE				1024
#define MAX_BLOCK_LINES		64
static char blockLine[2][MAX_BLOCK_LINES][MAXLINE];
static int lineNum = 0;
static long binSize = 0;
static char *pBinBuffer = NULL;
static int failcode;
static int failLine;

#define TIMEOUT_ERROR -100
#define BLOCK_SYNCH_ERROR -101

// local prototypes
static int connectToServer(SOCKET insock, char *host, int port, char *errRet);
static int getReturnBlock(SOCKET sockin);
static int readBinary(SOCKET sockin, char *pBuf, int numBytes);
static int readLine(SOCKET sockin, char *buf, int maxLine);
static void myrecv(SOCKET fd, char *buf, int len);
static void mysend(SOCKET fd, char *buf, int len);
static int sendServerCommand2(SOCKET sockin, char *retBuf, char *cmd);

// keep for reconnect
static char lastHost[256];
static int lastPort;

//
// Initialize the connection to the ccd server.
// returns 0 on success, or -1 on failure
// Reason for error returned via errRet
//
int initCCDServer(char *host, int port, char *errRet)
{
	int rt;
	
	if(sockfd != INVALID_SOCKET
	&& sockfd2 != INVALID_SOCKET
	&& !strcasecmp(host,lastHost)
	&& port == lastPort) {
		return 0; // already connected
	}
	
	if(sockfd != INVALID_SOCKET) {
		close(sockfd);
		sockfd = INVALID_SOCKET;
	}
	if(sockfd2 != INVALID_SOCKET) {
		close(sockfd2);
		sockfd2 = INVALID_SOCKET;
	}
	
	errorExit = 0;
		
	strcpy(lastHost,host);
	lastPort = port;

	/* create socket */
	if ((sockfd = socket (AF_INET, SOCK_STREAM, 0)) < 0) {
		sprintf(errRet,"Error creating socket: %s\n",strerror(errno));		
	    return -1;
	}
	
	if ((sockfd2 = socket (AF_INET, SOCK_STREAM, 0)) < 0) {
		close(sockfd);
		sockfd = INVALID_SOCKET;
		sprintf(errRet,"Error creating socket: %s\n",strerror(errno));		
	    return -1;
	}

	rt = connectToServer(sockfd, host, port, errRet);
	if(!rt) rt = connectToServer(sockfd2, host, port, errRet);
	if(rt < 0) {
		if(sockfd != INVALID_SOCKET) {
			close(sockfd);
			sockfd = INVALID_SOCKET;
		}
		if(sockfd2 != INVALID_SOCKET) {
			close(sockfd2);
			sockfd2 = INVALID_SOCKET;
		}
	}
	
	return rt;
}

//
// Connect the socket to the server
//
int connectToServer(SOCKET insock, char *host, int port, char *errRet)
{
	struct sockaddr_in cli_socket;
	struct hostent *hp;
	int len;

	/* get host name running server */
	if (!(hp = gethostbyname(host))) {
		sprintf(errRet,"Error resolving host: %s\n",strerror(errno));		
	    return -1;
	}
		
	/* connect to the server */
	len = sizeof(cli_socket);
	memset (&cli_socket, 0, len);
	cli_socket.sin_family = AF_INET;
	cli_socket.sin_addr.s_addr =
			    ((struct in_addr *)(hp->h_addr_list[0]))->s_addr;
	cli_socket.sin_port = htons(port);
	if (connect (insock, (struct sockaddr *)&cli_socket, len) < 0) {
		sprintf(errRet,"Unable to connect with CCD Server: %s\n",strerror(errno));
	    return -1;
	}
		
	/* ready */
	return 0;
}

//
// Reconnect if we lose connection
//
void Reconnect(void)
{
	char msg[256];

	if(sockfd != INVALID_SOCKET) {
		close(sockfd);
		sockfd = INVALID_SOCKET;
	}
	if(sockfd2 != INVALID_SOCKET) {
		close(sockfd2);
		sockfd2 = INVALID_SOCKET;
	}
		
	if(initCCDServer(lastHost,lastPort,msg) < 0) {
		fprintf(stderr,"Unable to reconnect to %s:%d [%s]\n",lastHost,lastPort,msg);
	}
}

//
// Send a command to the CCD Server and return response via return value and retBuf
// formatted command is in format and args
//
int sendServerCommand(char *retBuf, char *fmt, ...)
{
	char buf[8192];
	va_list ap;
	int rt;
		
	/* format the message */
	va_start (ap, fmt);
	vsprintf (buf, fmt, ap);
	va_end (ap);

#if CMDTRACE	
	fprintf(stderr,"Sending Command: %s\n",buf);
#endif
	rt = sendServerCommand2(sockfd, retBuf, buf);
	if(errorExit) {
		Reconnect();
		rt = sendServerCommand2(sockfd, retBuf, buf);
	}
	return rt;
}
// send to a specific socket connection -- (monitor program creates asynch link...)
int sendServerCommand2(SOCKET sockin, char *retBuf, char *cmd)
{
	if(!errorExit) mysend(sockin, cmd, strlen(cmd));
	if(!errorExit) mysend(sockin, "\r\n", 2);
	if(!errorExit) {
		getReturnBlock(sockin);
		if(getLastFailcode()) {
			sprintf(retBuf, "%s",getLastFailMessage());
			return -1;
		}
		if(getNumReturnLines()) {
			// return the first line of the response.  Mindful of which return block to read.
			strcpy(retBuf,blockLine[sockin == sockfd ? 0 : 1][1]);
		} else strcpy(retBuf,"");
		return 0;		
	} else {
		sprintf(retBuf,"Communications Error");
		return -1;
	}
}

//
// Get the number of lines in the most recently returned block
//
int getNumReturnLines(void)
{
	return lineNum;
}

//
// Read the return lines from the most recent return block
// Line numbers are 0-based
// Only reads from "main" (sockfd) block.
//
char * getReturnLine(int line)
{
	if(line < 0 || line >= lineNum-1) {
		return "";
	}
	return blockLine[0][line+1];
}		

//
// Get the failure code of the last return block
//
int getLastFailcode(void)
{
	return failcode;
}

//
// Get the last failure message
//
char *getLastFailMessage(void)
{
	char buf[MAXLINE];
	char *p;
	
	strcpy(buf, blockLine[0][failLine]);
	p = strchr(buf,':');
	if(p) p++;
	else p = buf;
		
	if(!failcode) return "";
	return p;
}

//
// Return a pointer to the binary buffer
//
char * getBinBuffer()
{
	return pBinBuffer;
}

//
// return the size of the binary buffer
//
long getBinSize()
{
	return binSize;
}

//
// Free the binary buffer if it exists
//
void freeBinBuffer()
{
	if(pBinBuffer) free(pBinBuffer);
	pBinBuffer = NULL;
	binSize = 0;
}

//
// Monitor function -- essentially same as fli_monitor
//
/* create a child helper process that monitors an exposure. set ccdserver_pixpipe
 *   to a file descriptor from which the parent (us) will read EOF when the
 *   current exposure completes. set ccdserver_diepipe to a fd from which the child
 *   will read EOF if we die or intentionally wish to cancel exposure.
 * return 0 if process is underway ok, else -1.
 */
static int ccdserver_monitor(char *err)
{
	int pixp[2], diep[2];
	int pid;

	if (pipe(pixp) < 0 || pipe(diep)) {
	    sprintf (err, "CCD Server pipe: %s", strerror(errno));
	    return (-1);
	}

	signal (SIGCHLD, SIG_IGN);	/* no zombies */
	pid = fork();
	if (pid < 0) {
	    sprintf (err, "CCD Server fork: %s", strerror(errno));
	    close (pixp[0]);
	    close (pixp[1]);
	    close (diep[0]);
	    close (diep[1]);
	    return(-1);
	}

	if (pid) {
	    /* parent */
	    ccdserver_pixpipe = pixp[0];
	    close (pixp[1]);
	    ccdserver_diepipe = diep[1];
	    close (diep[0]);

	    // mark us as in control of this exposure
	    whoExposing = sockfd;

	    return (0);
	}

	/* child .. poll for exposure or cancel if EOF on diepipe */
	close (pixp[0]);
	close (diep[1]);
	
	// mark us as in control of this exposure
	whoExposing = sockfd;
	
	while (1) {
	    struct timeval tv;
	    fd_set rs;
	    char buf[256];
	    char *p;

	    tv.tv_sec = CCDServerPP/1000000;
	    tv.tv_usec = CCDServerPP%1000000;

	    FD_ZERO(&rs);
	    FD_SET(diep[0], &rs);

	    switch (select (diep[0]+1, &rs, NULL, NULL, &tv)) {
		    case 0: 	/* timed out.. time to poll camera */
		    	// ArePixelsReady will return non-zero if TRUE
		    	if(sendServerCommand2(sockfd2,buf,"ArePixelsReady") < 0) {
					_exit(1);
				}
				p = strtok(buf," \r\n\0");
				if(atoi(p))	{
					_exit(0); // yep, come and get 'em
				}		    	
				break;
	    	case 1:	/* parent died or gave up */
//				sendServerCommand2(sockfd2,buf,"CancelExposure");
				_exit(0);
				break;
		    default:	/* local trouble */
//				sendServerCommand2(sockfd2,buf,"CancelExposure");
				_exit(1);
				break;
	    }
	}
}

//
// See if we are the ones exposing, and therefore have the right to cancel
//
static int areWeExposing()
{
	return(whoExposing == sockfd);
}

// --------------------------------------------------------------

/* My send and receive */
static void mysend(SOCKET fd, char *buf, int len)
{
	int err;
	time_t startTime;
	
	errno = 0;

	if(!fd) return;

	startTime = time(NULL);

	do {

		err = send(fd,buf,len,(MSG_DONTWAIT | MSG_NOSIGNAL));
		if(err != 0) err = errno;
		
		if( (time(NULL) - startTime) > command_timeout) {
			fprintf(stderr,"Timeout on send for socket %d\n",fd);
			err = -1;
		}

	} while (err == EAGAIN || err == EWOULDBLOCK || err == EINTR);
	
	if(!err) return;

//	fprintf(stderr,"ccdcamera.mysend(): Error on send on socket %d -- [%s] buf [%s]\n",fd, strerror(errno), buf);
	
	errorExit = 1; // EXIT ON ERROR!!!	
}

static void myrecv(SOCKET fd, char *buf, int len)
{
	int rb;
	int err;

	fd_set rfds;
	struct timeval tv;
	if(!fd) return;

	FD_ZERO(&rfds);
	FD_SET(fd, &rfds);
	tv.tv_sec = command_timeout;
	tv.tv_usec = 0;

	err = select(fd+1, &rfds, NULL, NULL, &tv);
	if(err <= 0) {
		fprintf(stderr,"Timeout on recv for socket %d\n",fd);
		errorExit = 1; // EXIT ON ERROR!!!
		return;
	}

	rb = read(fd,buf,len);		// use read instead of recv... seems to be the CLOSE_WAIT fix!

	if(rb != len) {
		errorExit = 1; // EXIT ON ERROR!!!
	}
}

/*
 * Read a line from the client socket up to but not including the next newline into buffer and terminate.
 * return number of characters read, or -1 on error
 *
 */
static int readLine(SOCKET sockin, char *buf, int maxLine)
{
	int cnt = 0;
	char ch;

	while(cnt < maxLine) {
		myrecv(sockin,&ch,1);
		if(errorExit) {
			return -1;
		}
		if(ch >= ' ') {
			buf[cnt++] = ch;
		}
		else if(ch == '\n') {
			buf[cnt++] = 0;
			return cnt;
		}
	}	
	buf[maxLine-1] = 0;
	return cnt;
}

/*
 * Read numBytes of binary data from the client socket into pBuf
 * return number of bytes read, or -1 on error
 */
static int readBinary(SOCKET sockin, char *pBuf, int numBytes)
{
/*
	int chunk = 256;
	int remain = numBytes;
	char *p = pBuf;
	chunk = chunk < remain ? chunk : remain;
	while(remain) {
		myrecv(sockfd,p,chunk);
		p += chunk;
		remain -= chunk;
		if(remain < chunk) chunk = remain;
	}
	if(errorExit) return -1;
	return numBytes;
*/

	int remain = numBytes;
	char *p = pBuf;
	while(!errorExit && remain--) {
		myrecv(sockin,p++,1);
	}
	if(errorExit) return -1;
	return numBytes;
}

/*
 * Collect a return block, gathering into an array of strings
 * and possibly a binary block
 * Return the number of lines in block (not including terminators)
 *
 */
static int getReturnBlock(SOCKET sockin)
{
	int 	bytesRead;
	long	size;
	int 	who = sockin == sockfd ? 0 : 1;
		
	// read until we hit end of block
	// or out of array slots
	lineNum = 0;
	failcode = 0;
	while(1) {
		blockLine[who][lineNum][0] = 0;
		bytesRead = readLine(sockin, blockLine[who][lineNum], MAXLINE);
		if(bytesRead < 0) break;
		if(!strcmp(blockLine[who][lineNum],"*<<<")) {
			break; // done!
		}
		if(sscanf(blockLine[who][lineNum],"*FAILURE (%d)",&failcode) == 1) {
			failLine = lineNum;
#if CMDTRACE		
			fprintf(stderr,"Failure (%d) detected\n",failcode);
#endif			
		}		
		if(sscanf(blockLine[who][lineNum],"<BIN:%ld>",&size) == 1) {
			freeBinBuffer();
			pBinBuffer = (char *) malloc(size);
			if(pBinBuffer) {
				binSize = size;
				readBinary(sockin, pBinBuffer,size);
			}
		}
		if(lineNum < MAX_BLOCK_LINES-1) {
			lineNum++;
		}
	}
	if(lineNum >= MAX_BLOCK_LINES-1) {
		fprintf(stderr,"Block Line Overrun Detected\n");
	}
	if(errorExit) {
		fprintf(stderr,"Timed out reading return block\n");
		failcode = TIMEOUT_ERROR;
	}
	else if(strcmp(blockLine[who][0],"*>>>")) {
		fprintf(stderr,"Expected block start, got %s\n",blockLine[who][0]);
		failcode = BLOCK_SYNCH_ERROR;
	}
	if(failcode) lineNum = 0;	
	return lineNum-1; // number of block lines, not counting terminators (starting at blockLine[1])
}

/* Last sky level: passthrough only for ccdserver */
int lastSkyCCD (char *buf) {
  char *p;
  int status = -1, len;

  if(ccdServer) {
    if(sendServerCommand(buf,"LastSky") < 0) {
      return -1;
    }

    p = strstr(buf, "\r\n\0");
    if(p)
      *p = '\0';

    status = (int) strtol(buf, &p, 10);
    if(p == buf)
      return -1;

    len = strlen(p)+1;
    memmove(buf, p, len);
  }

  return(status);
}

/* Last file name: passthrough only for ccdserver */
int lastFileNameCCD (char *buf) {
  char *p;
  int status = -1, len;

  if(ccdServer) {
    if(sendServerCommand(buf,"LastFileName") < 0) {
      return -1;
    }

    p = strstr(buf, "\r\n\0");
    if(p)
      *p = '\0';

    status = (int) strtol(buf, &p, 10);
    if(p == buf)
      return -1;

    len = strlen(p)+1;
    memmove(buf, p, len);
  }

  return(status);
}

/* Last sky level: passthrough only for ccdserver */
int scratchOnCCD (char *buf) {
  char *p;
  int status = -1, len;

  if(ccdServer) {
    if(sendServerCommand(buf,"ScratchOn") < 0) {
      return -1;
    }

    p = strstr(buf, "\r\n\0");
    if(p)
      *p = '\0';

    status = (int) strtol(buf, &p, 10);
    if(p == buf)
      return -1;

    len = strlen(p)+1;
    memmove(buf, p, len);
  }

  return(status);
}

/* Last sky level: passthrough only for ccdserver */
int scratchOffCCD (char *buf) {
  char *p;
  int status = -1, len;

  if(ccdServer) {
    if(sendServerCommand(buf,"ScratchOff") < 0) {
      return -1;
    }

    p = strstr(buf, "\r\n\0");
    if(p)
      *p = '\0';

    status = (int) strtol(buf, &p, 10);
    if(p == buf)
      return -1;

    len = strlen(p)+1;
    memmove(buf, p, len);
  }

  return(status);
}
