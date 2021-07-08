/* quick indices into the fifos[] array, below.
 * N.B. order must match the arrays in fifos.c and control.c.
 */

 /* Set to 1 if we have a windscreen (i.e. JSF) */
#ifndef WINDSCREEN
#define WINDSCREEN 0
#endif


typedef enum {
#if WINDSCREEN
    Tel_Id=0, Filter_Id, Focus_Id, Dome_Id, Lights_Id, Cam_Id, Screen_Id,
#else
    Tel_Id=0, Filter_Id, Focus_Id, Dome_Id, Lights_Id, Cam_Id,
#endif
    N_Id
} FifoId;

/* xobs.c */
extern Widget toplevel_w;
extern TelStatShm *telstatshmp;
extern XtAppContext app;
extern char myclass[];
extern Obj sunobj, moonobj;
extern int xobs_alone;
extern void die (void);

/* autofocus.c */
extern void afoc_manage (void);
extern void afoc_foc_cb (int code, char msg[]);
extern void afoc_cam_cb (int code, char msg[]);
extern void afoc_initCfg (void);

/* batch.c */
extern int batchIsOn(void);
extern void batchOn(void);
extern void batchOff(void);
extern void batchUpdate();
extern void batchCB (Widget w, XtPointer client, XtPointer call);

/* calaxes.c */
extern void axes_manage (void);
extern int axes_xephemSet (char *buf);

/* config.c */
extern void initCfg (void);
extern char icfn[];
extern FilterInfo *filtinfo;
extern int nfilt;
extern int deffilt;
extern double SUNDOWN;
extern double DOMETOL, FLATTAZ, FLATTALT, FLATDAZ;
extern double STOWALT, STOWAZ;
extern double SERVICEALT, SERVICEAZ;
extern double MAXHA, MINALT, MAXDEC;
extern int OffTargPitch;
extern int OffTargDuration;
extern int OffTargPercent;
extern int OnTargPitch;
extern int OnTargDuration;
extern int OnTargPercent;
extern int BeepPeriod;
extern char BANNER[80];
extern int MAXFLINT;

/* control.c */
extern void g_stop (Widget w, XtPointer client, XtPointer call);
extern void g_exit (Widget w, XtPointer client, XtPointer call);
extern void g_init (Widget w, XtPointer client, XtPointer call);
extern void g_home (Widget w, XtPointer client, XtPointer call);
extern void g_limit (Widget w, XtPointer client, XtPointer call);
extern void g_focus (Widget w, XtPointer client, XtPointer call);
extern void g_calib (Widget w, XtPointer client, XtPointer call);
extern void g_paddle (Widget w, XtPointer client, XtPointer call);
extern void g_confirm (Widget w, XtPointer client, XtPointer call);

/* dome.c */
extern void domeOpenCB (Widget w, XtPointer client, XtPointer call);
extern void domeCloseCB (Widget w, XtPointer client, XtPointer call);
extern void domeAutoCB (Widget w, XtPointer client, XtPointer call);
extern void domeGotoCB (Widget w, XtPointer client, XtPointer call);

/* fifos.c */
extern int fifoMsg (FifoId fid, char *fmt, ...);
extern int fifoRead (FifoId fid, char buf[], int buflen);
extern void resetSW (void);
extern void stop_all_devices(void);
extern void initPipes(void);
extern void closePipes(void);

/* gui.c */
typedef enum {
    LTIDLE, LTOK, LTACTIVE, LTWARN,
    LTN
} LtState;
extern Pixel ltcolors[LTN];
extern Pixel editableColor;
extern Pixel uneditableColor;
extern void mkGUI (char *version);
extern void fillFilterMenu(void);
extern int setColor (Widget w, char *resource, Pixel newp);
extern void nyi (void);
extern String fallbacks[];
extern void setLt (Widget w, LtState s);
extern Widget mkLight (Widget p_w);
extern void msg (char *fmt, ...);
extern void rmsg (char *line);
extern void guiSensitive (int whether);
extern void showEngmode ();

/* paddle.c */
extern void pad_manage (void);
extern void pad_reset (void);

/* query.c */
extern void query (Widget tw, char *msg, char *label0, char *label1, char
    *label2, void (*func0)(), void (*func1)(), void (*func2)());
extern int rusure (Widget tw, char *msg);
extern int rusure_geton (void);
extern void rusure_seton (int whether);

/* scope.c */
extern void s_stow (Widget w, XtPointer client, XtPointer call);
extern void s_service (Widget w, XtPointer client, XtPointer call);
extern void s_here (Widget w, XtPointer client, XtPointer call);
extern void s_lookup (Widget w, XtPointer client, XtPointer call);
extern void s_track (Widget w, XtPointer client, XtPointer call);
extern void s_goto (Widget w, XtPointer client, XtPointer call);
extern void s_edit (Widget w, XtPointer client, XtPointer call);

/* sound.c */
extern int soundIsOn (void);
extern void soundCB (Widget w, XtPointer client, XtPointer call);

/* skymap.c */
extern Widget mkSky (Widget p_w);
extern void showSkyMap (void);

/* telrun.c */
extern int startTelrun (void);
extern void stopTelrun(void);
extern int chkTelrun(void);
extern void monitorTelrun(int whether);

/* tips.c */
extern void wtip (Widget w, char *tip);
extern int tip_geton(void);
extern void tip_seton(int whether);

/* update.c */
extern void updateStatus(int force);

/* xephem.c */
extern void initXEphem(void);

/* cli.c */
extern void initCli(void);
extern void cleanupCli(void);

/* clicommands.c */
extern void check_tel_reply(int, char *);
extern void check_dome_reply(int, char *);
extern void check_cam_reply(int, char *);
extern void check_lights_reply(int, char *);
extern void cli_move_telescope(void);
extern void cli_move_dome(void);
extern void cli_move_cam(void);
extern void cli_move_lights(void);
extern void cli_wxalert(int);

/* For RCS Only -- Do Not Edit
 * @(#) $RCSfile: xobs.h,v $ $Date: 2006/01/13 20:42:48 $ $Revision: 1.1.1.1 $ $Name:  $
 */
