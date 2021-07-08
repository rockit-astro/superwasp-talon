/*

  Copyright (c) 2000 Finger Lakes Instrumentation (FLI), LLC.
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

#ifndef _FLI_H_
#define _FLI_H_

typedef void *fliport_t;
typedef void *flidev_t;
typedef void *flicam_t;

typedef enum {
  FLI_FRAME_TYPE_NORMAL = 0,
  FLI_FRAME_TYPE_DARK
} fliframe_t;

typedef enum {
  FLI_MODE_8BIT = 0,
  FLI_MODE_16BIT
} flibitdepth_t;


#define FLI_MIN_EXPOSURE_TIME 8
#define FLI_MAX_EXPOSURE_TIME 68669153

#define FLI_MIN_HBIN 1
#define FLI_MAX_HBIN 4095

#define FLI_MIN_VBIN 1
#define FLI_MAX_VBIN 4095

#define FLI_MIN_FLUSHES 1
#define FLI_MAX_FLUSHES 4095

#define FLI_MIN_TEMP -55
#define FLI_MAX_TEMP 45

#define FLI_MIN_ROWS 1
#define FLI_MAX_ROWS 4095

#define FLI_MIN_REPEAT 1
#define FLI_MAX_REPEAT 65535

/* Function prototypes */

int FLICancelExposure(flicam_t cam);
int FLIClose(flicam_t cam);
int FLIExit(void);
int FLIExposeFrame(flicam_t cam);
int FLIFlushRow(flicam_t cam, int rows, int repeat);
int FLIGetArrayArea(flicam_t cam, int *ul_x, int *ul_y,
                    int *lr_x, int *lr_y);
int FLIGetDevice(flicam_t cam, flidev_t *dev);
int FLIGetFirmwareRev(flicam_t cam, int *firmrev);
int FLIGetModel(flicam_t cam, char *model, size_t len);
int FLIGetSerialNum(flicam_t cam, int *serialnum);
int FLIGetExposureStatus(flicam_t cam, long *timeleft);
int FLIGetLibVersion(char *ver, size_t len);
int FLIGetNextDevice(fliport_t port, flidev_t dev, flidev_t *nextdev);
int FLIGetTemperature(flicam_t cam, double *temperature);
int FLIGetVisibleArea(flicam_t cam, int *ul_x, int *ul_y,
                      int *lr_x, int *lr_y);
int FLIGrabFrame(flicam_t cam, void *buff, size_t buffsize,
                 size_t *bytesgrabbed);
int FLIGrabRow(flicam_t cam, void *buff, size_t buffsize);
int FLIReadFrame(flicam_t cam, void *buff, size_t buffsize,
                 size_t *bytesgrabbed);
int FLIInit(char *file, fliport_t *handle);
int FLIOpen(fliport_t port, flidev_t dev, flicam_t *cam);
int FLISetDevice(flicam_t cam, flidev_t dev);
int FLISetExposureTime(flicam_t cam, long exptime);
int FLISetFrameType(flicam_t cam, fliframe_t frametype);
int FLISetHBin(flicam_t cam, int hbin);
int FLISetImageArea(flicam_t cam, int ul_x, int ul_y,
                    int lr_x, int lr_y);
int FLISetBitDepth(flicam_t cam, flibitdepth_t bitdepth);
int FLISetNFlushes(flicam_t cam, int nflushes);
int FLISetTemperature(flicam_t cam, double temperature);
int FLISetVBin(flicam_t cam, int vbin);

#endif /* _FLI_H_ */
