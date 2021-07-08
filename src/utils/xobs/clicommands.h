#ifndef __CLICOMMANDS_H__
#define __CLICOMMANDS_H__

/* Command numbers */
#define TCSCMD_SLEW_OBJECT   1
#define TCSCMD_SLEW_RADEC    2
#define TCSCMD_SLEW_ALTAZ    3

#define TCSCMD_TRACK_OBJECT  4
#define TCSCMD_TRACK_RADEC   5
#define TCSCMD_TRACK_ALTAZ   6

#define TCSCMD_OFFSET_RADEC  7
#define TCSCMD_OFFSET_ALTAZ  8

#define TCSCMD_ENGMODE       9
#define TCSCMD_STOP_TEL     10  /* stop telescope */

#define TCSCMD_RASTER       11  /* raster on/off */

#define TCSCMD_DOME         12  /* dome open/close */
#define TCSCMD_STOP_DOME    13  /* stop dome */

#define TCSCMD_EXPOSE       14
#define TCSCMD_STOP_CAM     15
#define TCSCMD_SCRATCH      16
#define TCSCMD_LAST_SKY     17  /* get skylevel of last integration */

#define TCSCMD_LIGHTS       18

#define TCSCMD_SET_ALARM    19
#define TCSCMD_HOME         20  /* zeroset axes */

#define TCSCMD_ABORT       100 /* abort running command */

/* Return codes */
#define TCSRET_WXBUTOK  -5
#define TCSRET_WXALERT  -4
#define TCSRET_BUSY     -3
#define TCSRET_ABORTED  -2
#define TCSRET_USAGE    -1
#define TCSRET_FAILED    0
#define TCSRET_ACK       1
#define TCSRET_FINISHED  2

#endif  /* __CLICOMMANDS_H__ */
