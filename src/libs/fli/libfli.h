/*

  Copyright (c) 2000, 2002 Finger Lakes Instrumentation (FLI), LLC.
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:

        Redistributions of source code must retain the above copyright
        notice, this list of conditions and the following disclaimer.

        Redistributions in binary form must reproduce the above
        copyright notice, this list of conditions and the following
        disclaimer in the documentation and/or other materials
        provided with the distribution.

        Neither the name of Finger Lakes Instrumentation (FLI), LLC
        nor the names of its contributors may be used to endorse or
        promote products derived from this software without specific
        prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
  REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
  POSSIBILITY OF SUCH DAMAGE.

  ======================================================================

  Finger Lakes Instrumentation (FLI)
  web: http://www.fli-cam.com
  email: fli@rpa.net

*/

#ifndef _FLILIB_H_
#define _FLILIB_H_

/* === External interface === */

#include "fli.h"

/* === Internal details === */

typedef struct {
  int x;                        /* X coordinate */
  int y;                        /* Y coordinate */
} _point_t;

typedef struct {
  _point_t ul;                  /* Upper-left */
  _point_t lr;                  /* Lower-right */
} _area_t;

typedef struct fliport {
  struct fliport *next;
  char *filename;
  int fd;
  struct flidev *first_dev;
} _fliport_t;

typedef struct {
  u_int16_t modelid;
  char *model;
  _area_t array_area;
  _area_t visible_area;
} _flidevinfo_t;

typedef struct flidev {
  struct flidev *next;
  _fliport_t *port;
  int dev_offset;
  _flidevinfo_t caminfo;
  u_int16_t serialnum;
  u_int16_t firmwarerev;
} _flidev_t;

typedef struct {
  _flidev_t *dev;
  _area_t image;
  long exposure_time;
  int expdur;
  fliframe_t frametype;
  int hbin;
  int vbin;
  int nflushes;
  flibitdepth_t bitdepth;
} _flicam_t;

#define CAMERA_OFFSET 1
#define MAX_DEVICES 16

#define X_FLUSHBIN 4
#define Y_FLUSHBIN 4

/* === Command and Data Word Formats === */

#define C_ADDRESS(addr, ext) (0x8000 | ((addr << 8) & 0x0f00) | (ext & 0x00ff))

#define C_RESTCFG(gain, chnl, res) \
  (0x9000 | ((gain << 8) & 0x0f00) | ((chnl << 4) & 0x00f0) | ((res & 0x000f)))

#define C_SHUTTER(open, dmult) \
  (0xa000 | (dmult & 0x07ff) | ((open << 11) & 0x0800))

#define C_SEND(x) (0xb000 | ((x) & 0x0fff))
#define C_FLUSH(x) (0xc000 | ((x) & 0x0fff))
#define C_VSKIP(x) (0xd000 | ((x) & 0x0fff))
#define C_HSKIP(x) (0xe000 | ((x) & 0x0fff))
#define C_TEMP(x) (0xf000 | ((x) & 0x0fff))
#define D_XROWOFF(x) (0x0000 | ((x) & 0x0fff))
#define D_XROWWID(x) (0x1000 | ((x) & 0x0fff))
#define D_XFLBIN(x) (0x2000 | ((x) & 0x0fff))
#define D_YFLBIN(x) (0x3000 | ((x) & 0x0fff))
#define D_XBIN(x) (0x4000 | ((x) & 0x0fff))
#define D_YBIN(x) (0x5000 | ((x) & 0x0fff))
#define D_EXPDUR(x) (0x6000 | ((x) & 0x0fff))
#define D_RESERVE(x) (0x7000 | ((x) & 0x0fff))

/* === Extended Parameter Fields for Querying Camera === */

#define EPARAM_ECHO (0x00)
#define EPARAM_CCDID (0x01)
#define EPARAM_FIRM (0x02)
#define EPARAM_SNHIGH (0x03)
#define EPARAM_SNLOW (0x04)
#define EPARAM_SIGGAIN (0x05)
#define EPARAM_DEVICE (0x06)

#define DEVICE_CAMERA (1)

#define MAX_EXPDUR 0xfff
#define MAX_EXPMUL 0x7ff

/* === Known Devices === */

const _flidevinfo_t knowndev[] = {
  /* id model           array_area              visible_area */
  {1,  "KAF-0260C0-2",  {{0, 0}, {534,  520}},  {{12, 4},  {524,  516}}},
  {2,  "KAF-0400C0-2",  {{0, 0}, {796,  520}},  {{14, 4},  {782,  516}}},
  {3,  "KAF-1000C0-2",  {{0, 0}, {1042, 1032}}, {{8,  4},  {1032, 1028}}},
  {4,  "KAF-1300C0-2",  {{0, 0}, {1304, 1028}}, {{4,  2},  {1284, 1026}}},
  {5,  "KAF-1400C0-2",  {{0, 0}, {796,  520}},  {{14, 14}, {782,  526}}},
  {6,  "KAF-1600C0-2",  {{0, 0}, {1564, 1032}}, {{14, 4},  {1550, 1028}}},
  {7,  "KAF-4200C0-2",  {{0, 0}, {2060, 2048}}, {{25, 2},  {2057, 2046}}},
  {8,  "SITe-502S",     {{0, 0}, {527,  512}},  {{15, 0},  {527,  512}}},
  {9,  "TK-1024",       {{0, 0}, {1124, 1024}}, {{50, 0},  {1074, 1024}}},
  {10, "TK-512",        {{0, 0}, {563,  512}},  {{51, 0},  {563,  512}}},
  {11, "SI-003A",       {{0, 0}, {1056, 1024}}, {{16, 0},  {1040, 1024}}},
  {12, "KAF-6300",      {{0, 0}, {3100, 2056}}, {{16, 4},  {3088, 2052}}},
  {0,  "Unknown Model", {{0, 0}, {0,    0}},    {{0,  0},  {0,    0}}}
};

#endif /* _FLILIB_H_ */
