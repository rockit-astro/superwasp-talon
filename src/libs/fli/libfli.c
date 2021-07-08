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

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <asm/errno.h>
#include <limits.h>
#include <sys/ioctl.h>
#include "libfli.h"
#include "fli_ioctl.h"
#include <errno.h>

#ifdef _DEBUG_
#include <stdio.h>
#endif

/* === Macros === */

#ifndef MIN
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#endif

#ifndef MAX
#define MAX(x, y) ((x) > (y) ? (x) : (y))
#endif

#define FLI_IOCTL(fd, request...)               \
  do {                                          \
    errno = 0;                                  \
    if (ioctl(fd, ## request))                  \
      return (errno ? -errno : -EINVAL);        \
  } while (0);

#define FLI_LOCK(fd) FLI_IOCTL(fd, FLI_LOCK_PORT)
#define FLI_UNLOCK(fd) FLI_IOCTL(fd, FLI_UNLOCK_PORT)

#define DEFAULT_NUM_POINTERS 1024

# define STRINGIFY(x) _STRINGIFY(x)
# define _STRINGIFY(x) #x

/* === Variables === */
static const char* version = "FLI CCD Library for Linux version "
STRINGIFY(VERSION);

static _fliport_t *first_port = NULL;
static double temp_slope = (100.0 / 201.1);
static double temp_intercept = (-61.613);

static int DEFAULT_RTO = 50;
static int FLUSHROW_RTO = 100;
static int PROBE_RTO = 10;

static struct {
  void **pointers;
  int size;
  int count;
} allocated = {NULL, 0, 0};

/* === Static Functions === */

static __inline__ void *_xmalloc(size_t size)
{
  int i;
  void *ptr;

  if (allocated.size == 0)
  {
    if ((allocated.pointers =
         malloc(DEFAULT_NUM_POINTERS * sizeof(void **))) == NULL)
      return NULL;

    memset(allocated.pointers, 0, DEFAULT_NUM_POINTERS * sizeof(void **));

    allocated.size = DEFAULT_NUM_POINTERS;
  }

  if (allocated.count + 1 > allocated.size)
  {
    void **tmp;

    if ((tmp = realloc(allocated.pointers,
                       2 * allocated.size * sizeof(void **))) == NULL)
      return NULL;

    allocated.pointers = tmp;

    memset(allocated.pointers + allocated.size, 0,
           allocated.size * sizeof(void **));

    allocated.size *= 2;
  }

  if ((ptr = malloc(size)) == NULL)
    return NULL;

  for (i = 0; i < allocated.size; i++)
  {
    if (allocated.pointers[i] == NULL)
      break;
  }

  if (i == allocated.size)
  {
    /* This shouldn't happen */
    free(ptr);
    return NULL;
  }

  allocated.pointers[i] = ptr;
  allocated.count++;

  return ptr;
}

static void *xmalloc(size_t size)
{
  return _xmalloc(size);
}

static void *xcalloc(size_t nmemb, size_t size)
{
  void *ptr;

  if ((ptr = _xmalloc(nmemb * size)) == NULL)
    return NULL;

  memset(ptr, 0, nmemb * size);

  return ptr;
}

static void xfree(void *ptr)
{
  int i;

  for (i = 0; i < allocated.size; i++)
  {
    if (allocated.pointers[i] == ptr)
      break;
  }

  if (i != allocated.size)      /* ptr was found */
  {
    free(ptr);

    allocated.pointers[i] = NULL;
    allocated.count--;
  }

  return;
}

static int xfree_all(void)
{
  int i;
  int freed = 0;

  for (i = 0; i < allocated.size; i++)
  {
    if (allocated.pointers[i] != NULL)
    {
      free(allocated.pointers[i]);
      freed++;
    }
  }

  if (allocated.pointers != NULL)
    free(allocated.pointers);

  allocated.pointers = NULL;
  allocated.size = 0;
  allocated.count = 0;

  return freed;
}

static __inline__ int cam_parameters_not_ok(_flicam_t *flicam)
{
  if ((flicam->image.ul.x < flicam->dev->caminfo.array_area.ul.x) ||
      (flicam->image.ul.y < flicam->dev->caminfo.array_area.ul.y) ||
      (flicam->image.lr.x > flicam->dev->caminfo.array_area.lr.x) ||
      (flicam->image.lr.y > flicam->dev->caminfo.array_area.lr.y) ||
      (flicam->image.lr.x <= flicam->image.ul.x) ||
      (flicam->image.lr.y <= flicam->image.ul.y) ||
      (flicam->exposure_time < FLI_MIN_EXPOSURE_TIME) ||
      (flicam->exposure_time > FLI_MAX_EXPOSURE_TIME) ||
      (flicam->frametype < FLI_FRAME_TYPE_NORMAL) ||
      (flicam->frametype > FLI_FRAME_TYPE_DARK) ||
      (flicam->hbin < FLI_MIN_HBIN) ||
      (flicam->hbin > FLI_MAX_HBIN) ||
      (flicam->vbin < FLI_MIN_VBIN) ||
      (flicam->vbin > FLI_MAX_VBIN) ||
      (flicam->nflushes < FLI_MIN_FLUSHES) ||
      (flicam->nflushes > FLI_MAX_FLUSHES) ||
      (flicam->bitdepth < FLI_MODE_8BIT) ||
      (flicam->bitdepth > FLI_MODE_16BIT) ||

      (flicam->image.ul.y +
       (flicam->image.lr.y - flicam->image.ul.y) * flicam->vbin >
       flicam->dev->caminfo.array_area.lr.y -
       flicam->dev->caminfo.array_area.ul.y) ||

      (flicam->image.ul.x +
       (flicam->image.lr.x - flicam->image.ul.x) * flicam->hbin >
       flicam->dev->caminfo.array_area.lr.x -
       flicam->dev->caminfo.array_area.ul.x))
    return 1;
  else
    return 0;
}

static int _writecommand(int fd, u_int16_t command, u_int16_t *data)
{
  extern int errno;
  int retval;
  u_int16_t tmp;

  errno = 0;

  if (write(fd, &command, sizeof(command)) != sizeof(command))
  {
    if (errno == 0)
      errno = EIO;

    return -errno;
  }

  switch (command & 0xf000)
  {
    /* Commands that return data */
  case C_ADDRESS(0, 0):
  case C_SHUTTER(0, 0):
  case C_FLUSH(0):
  case C_TEMP(0):

    if (read(fd, &tmp, sizeof(tmp)) != sizeof(tmp))
    {
      if (errno == 0)
        errno = EIO;

      return -errno;
    }

    if ((tmp & 0xf000) == (command & 0xf000))
    {
      if (data != NULL)
        *data = tmp;

      retval = 0;
    }
    else
      retval = -EIO;
    break;

  case C_SEND(0):
    retval = 0;
    break;

  default:
    retval = -EINVAL;
  }

  return retval;
}

static __inline__ int WriteCommand(flidev_t dev, u_int16_t command,
                                   u_int16_t *data)
{
  _flidev_t *flidev = (_flidev_t *)dev;
  int retval;

  FLI_LOCK(flidev->port->fd);

  /* Set device to listen */
  if ((retval = _writecommand(flidev->port->fd, C_ADDRESS(flidev->dev_offset,
                                                          EPARAM_ECHO), NULL)))
  {
    FLI_UNLOCK(flidev->port->fd);
    return retval;
  }

  /* Write command */
  retval = _writecommand(flidev->port->fd, command, data);

  FLI_UNLOCK(flidev->port->fd);

  return retval;
}

static int _writedata(int fd, u_int16_t data)
{
  extern int errno;
  u_int16_t tmp;

  errno = 0;

  if (write(fd, &data, sizeof(data)) != sizeof(data))
  {
    if (errno == 0)
      errno = EIO;

    return -errno;
  }

  if (read(fd, &tmp, sizeof(tmp)) != sizeof(tmp))
  {
    if (errno == 0)
      errno = EIO;

    return -errno;
  }

  return (tmp != data) ? -EIO : 0;
}

static __inline__ int WriteData(flidev_t dev, u_int16_t data)
{
  _flidev_t *flidev = (_flidev_t *)dev;
  int retval;

  FLI_LOCK(flidev->port->fd);

  /* Set device to listen */
  if ((retval =
       _writecommand(flidev->port->fd,
                     C_ADDRESS(flidev->dev_offset, EPARAM_ECHO), NULL)))
  {
    FLI_UNLOCK(flidev->port->fd);
    return retval;
  }

  /* Write data */
  retval = _writedata(flidev->port->fd, data);

  FLI_UNLOCK(flidev->port->fd);

  return retval;
}

static __inline__ void freedevs(_flidev_t *first_dev)
{
  _flidev_t *d;

  /* Free all dev's along the given linked-list */
  for (d = first_dev; d != NULL; )
  {
    _flidev_t *next = d->next;

    xfree(d);
    d = next;
  }

  return;
}

static void getcaminfo(_flidev_t *flidev, u_int16_t modelid)
{
  int i;

  /* Get generic camera info (model etc.) */
  for (i = 0; knowndev[i].modelid != 0; i++)
    if (knowndev[i].modelid == modelid)
      break;

  memcpy(&flidev->caminfo, &knowndev[i], sizeof(_flidevinfo_t));

  /* Get camera specific info (serial number etc.) */
  if (knowndev[i].modelid != 0)
  {
    u_int16_t tmp;

    if (WriteCommand(flidev,
                     C_ADDRESS(flidev->dev_offset, EPARAM_SNHIGH), &tmp))
      return;

    flidev->serialnum = (tmp & 0xff) << 8;

    if (WriteCommand(flidev,
                     C_ADDRESS(flidev->dev_offset, EPARAM_SNLOW), &tmp))
      return;

    flidev->serialnum |= tmp & 0xff;

    if (WriteCommand(flidev,
                     C_ADDRESS(flidev->dev_offset, EPARAM_FIRM), &tmp))
      return;

    flidev->firmwarerev = tmp & 0xff;
  }

  return;
}

static int initport(_fliport_t *port, char *file)
{
  extern int errno;
  int i;
  size_t len;
  _flidev_t *dev, *prevdev;

  errno = 0;

  if ((port->fd >= 0) && close(port->fd))
  {
    if (errno == 0)
      errno = EIO;

    return -errno;
  }

  if ((port->fd = open(file, O_RDWR)) == -1)
  {
    port->fd = -1;
    if (errno == 0)
      errno = EIO;

    return -errno;
  }

  /* Initialize port parameters */
  if (port->filename != NULL)
    xfree(port->filename);

  len = strlen(file) + 1;

  if ((port->filename = xmalloc(len)) == NULL)
    return -ENOMEM;

  strncpy(port->filename, file, len);
  port->filename[len - 1] = '\0';

  if (port->first_dev != NULL)
    freedevs(port->first_dev);

  port->first_dev = NULL;

  /* Adjust read timeout for probing devices */
  FLI_IOCTL(port->fd, FLI_SET_RTO, &PROBE_RTO);

  /* Look for cameras attached to port */
  for (i = CAMERA_OFFSET; i <= MAX_DEVICES; i++)
  {
    u_int16_t tmp;

    /* Set device i to listen to following commands */
    if (_writecommand(port->fd, C_ADDRESS(i, EPARAM_ECHO), &tmp))
      continue;

    if (tmp != C_ADDRESS(i, EPARAM_ECHO))
      continue;                 /* Invalid response */

    /* Find out what kind of device we're talking to */
    if (_writecommand(port->fd, C_ADDRESS(i, EPARAM_DEVICE), &tmp))
      continue;

    if((tmp & 0x00ff) != DEVICE_CAMERA)
      continue;                 /* Not a camera */

    /* Get camera id */
    if (_writecommand(port->fd, C_ADDRESS(i, EPARAM_CCDID), &tmp))
      continue;

    /* Allocate structure for this device */
    if ((dev = xcalloc(1, sizeof(_flidev_t))) == NULL)
    {
      /* Free all dev's already allocated for this port */
      freedevs(port->first_dev);
      port->first_dev = NULL;

      xfree(port->filename);
      port->filename = NULL;

      return -ENOMEM;
    }

    /* Initialize device parameters */
    dev->dev_offset = i;
    dev->port = port;

    /* Call this _after_ dev->dev_offset and dev->port are set */
    getcaminfo(dev, tmp & 0x00ff);

    /* Add device to linked-list of devices */
    dev->next = port->first_dev;
    port->first_dev = dev;
  }

  if (port->first_dev == NULL)
  {
    /* No devices found */
    xfree(port->filename);
    port->filename = NULL;

    return -ENODEV;
  }

  /* Set read timeout to default value */
  FLI_IOCTL(port->fd, FLI_SET_RTO, &DEFAULT_RTO);

  /* Device linked-list is in reverse order so put in correct order  */
  prevdev = NULL;
  for (dev = port->first_dev; dev != NULL; )
  {
    _flidev_t *nextdev = dev->next;

    if (nextdev == NULL)
      port->first_dev = dev;

    dev->next = prevdev;
    prevdev = dev;
    dev = nextdev;
  }

  /* Add port to linked-list of ports */
  port->next = first_port;
  first_port = port;

  return 0;
}

static __inline__ void freeport(_fliport_t *port)
{
  _fliport_t *current, *prev;

  /* Find port in linked-list of ports */
  prev = first_port;
  for (current = first_port; current != NULL; current = current->next)
  {
    if (current == port)
      break;

    prev = current;
  }

  /* Remove port from linked-list of ports (if there) */
  if (current != NULL)
  {
    if (current == first_port)
      first_port = current->next;
    else
      prev->next = current->next;
  }

  if (port->filename != NULL)
  {
    xfree(port->filename);
    port->filename = NULL;
  }

  if (port->fd >= 0)
  {
    close(port->fd);
    port->fd = -1;
  }

  if (port->first_dev != NULL)
  {
    freedevs(port->first_dev);
    port->first_dev = NULL;
  }

  xfree(port);

  return;
}

/* === Global Functions === */

/**
   Get the current library version.  This function copies up to
   \texttt{len - 1} characters of the current library version string,
   and a terminating \texttt{NULL} character, into the buffer pointed
   to by \texttt{ver}.  If \texttt{len} is less than or equal to zero
   this function has no effect.

   @param ver Pointer to a character buffer where the library version
   string is to be placed.

   @param len Size in bytes of buffer pointed to by \texttt{ver}.

   @return Zero on success.
   @return Non-zero on failure.

*/
int FLIGetLibVersion(char *ver, size_t len)
{
  if (ver == NULL)
    return -EINVAL;

  if (len > 0)
  {
    strncpy(ver, version, len);
    ver[len - 1] = '\0';
  }

  return 0;
}

/**
   Initialize the library.  This function initializes the library and
   places a port handle in the location pointed to by \texttt{port}.
   The port handle is used in subsequent library calls and acts as an
   interface to the parallel port.  This function must be called
   before any other library functions are used.

   @param file Filename of character special file that corresponds to
   the major/minor number controlled by FLI device driver module
   (e.g. \texttt{/dev/ccd0}).

   @param port Pointer to where a port handle will be placed.

   @return Zero on success.
   @return Non-zero on failure.

   @see FLIExit

*/
int FLIInit(char *file, fliport_t *port)
{
  _fliport_t *fliport;
  int retval;

  if ((file == NULL) || (port == NULL))
    return -EINVAL;

  /* See if initializing or re-initializing */
  for (fliport = first_port; fliport != NULL; fliport = fliport->next)
    if (strcmp(file, fliport->filename) == 0)
      break;

  if (fliport != NULL)
    freeport(fliport);

  if ((fliport = xcalloc(1, sizeof(_fliport_t))) == NULL)
    return -ENOMEM;

  fliport->fd = -1;

  if ((retval = initport(fliport, file)))
  {
    freeport(fliport);
    return retval;
  }

  *port = (fliport_t *)fliport;

  return 0;
}

/**
   Get a handle to a virtual camera.  The caller of this function can
   optionally request a handle associated with the specific physical
   camera \texttt{dev}.  If \texttt{dev} has a \texttt{NULL} value,
   the default (first) camera is used.  An application may use any
   number of handles associated with the same physical camera.

   @param port Port handle from a previous call to \texttt{FLIInit}.

   @param dev Device handle to request a specific device, or
   \texttt{NULL} for the default (first) device.

   @param cam Pointer to where a camera handle will be placed.

   @return Zero on success.
   @return Non-zero on failure.

   @see FLIClose

*/
int FLIOpen(fliport_t port, flidev_t dev, flicam_t *cam)
{
  _fliport_t *fliport = (_fliport_t *)port;
  _flidev_t *flidev = (_flidev_t *)dev;
  _flicam_t *c;
  _flidev_t *d;

  if ((port == NULL) || (cam == NULL))
    return -EINVAL;

  /* Associate a device with this cam handle */
  if (flidev != NULL)           /* A specific device was requested */
  {
    for (d = fliport->first_dev; d != NULL; d = d->next)
      if (d == flidev)
        break;
  }
  else                          /* Use first device */
    d = fliport->first_dev;

  if (d == NULL)
    return -EINVAL;

  if ((c = xcalloc(1, sizeof(_flicam_t))) == NULL)
    return -ENOMEM;

  c->dev = d;
  c->frametype = FLI_FRAME_TYPE_NORMAL;
  c->hbin = 1;
  c->vbin = 1;
  c->nflushes = 1;
  c->bitdepth = FLI_MODE_16BIT;

  *cam = (flicam_t *)c;

  return 0;
}

/**
   Close/Release a camera handle.  This function invalidates the given
   camera handle and releases all resources associated with it.  It
   should be called when the camera handle \texttt{cam} will no longer
   be used.

   @param cam Camera handle to be closed.

   @return Zero on success.
   @return Non-zero on failure.

   @see FLIOpen

*/
int FLIClose(flicam_t cam)
{
  _flicam_t *flicam = (_flicam_t *)cam;

  if (flicam == NULL)
    return -EINVAL;

  xfree(flicam);

  return 0;
}

/**
   Exit the library.  This function releases all resources held by the
   library.  After calling this function, library functions will not
   operate until \texttt{FLIInit} is called.

   @return Zero on success.
   @return Non-zero on failure.

   @see FLIInit

*/
int FLIExit(void)
{
  _fliport_t *port;
  int freed;

  for (port = first_port; port != NULL; )
  {
    _fliport_t *next = port->next;

    freeport(port);
    port = next;
  }

  first_port = NULL;

  /* Free all memory still allocated */
  freed = xfree_all();

#ifdef _DEBUG_
  if (freed)
    fprintf(stderr, "Forced to free %i pointers\n", freed);
#endif

  return 0;
}

/**
   Get a handle to the next physical device.  This function places a
   handle to the physical device after \texttt{dev}, in the location
   pointed to by \texttt{nextdev}.  If \texttt{dev} is the last
   physical device, the next device becomes the first physical device.

   @param port Port handle from a previous call to \texttt{FLIInit}.

   @param dev Device handle of the current device.

   @param nextdev Pointer to where a handle to the next will be
   placed.

   @return Zero on success.
   @return Non-zero on failure.

   @see FLIGetDevice
   @see FLISetDevice

*/
int FLIGetNextDevice(fliport_t port, flidev_t dev, flidev_t *nextdev)
{
  _fliport_t *fliport = (_fliport_t *)port;
  _flidev_t *flidev = (_flidev_t *)dev;

  if ((port == NULL) || (nextdev == NULL))
    return -EINVAL;

  *nextdev = (flidev == NULL) ? fliport->first_dev : flidev->next;

  return 0;
}

/**
   Get a handle to the physical device for a virtual camera.  A handle
   to the physical device associated with the virtual camera
   \texttt{cam} is place in the location pointed to by \texttt{dev}.

   @param cam Camera handle to find physical device for.

   @param dev Pointer to where a device handle will be placed.

   @return Zero on success.
   @return Non-zero on failure.

   @see FLISetDevice
   @see FLIGetNextDevice

*/
int FLIGetDevice(flicam_t cam, flidev_t *dev)
{
  _flicam_t *flicam = (_flicam_t *)cam;

  if ((cam == NULL) || (dev == NULL))
    return -EINVAL;

  *dev = (flidev_t *)flicam->dev;

  return 0;
}

/**
   Set the physical device for a virtual camera.  This function sets
   the physical device associated with the virtual camera \texttt{cam}
   to \texttt{dev}.  Note that the previous image area setting for
   \texttt{cam} may not be valid for new device \texttt{dev} since
   \texttt{dev} may have a different array area and visible area.

   @param cam Camera handle to set physical device for.

   @param dev Device handle to associate \texttt{cam} with.

   @return Zero on success.
   @return Non-zero on failure.

   @see FLIGetDevice
   @see FLIGetNextDevice

*/
int FLISetDevice(flicam_t cam, flidev_t dev)
{
  _flicam_t *flicam = (_flicam_t *)cam;
  _flidev_t *flidev = (_flidev_t *)dev;
  _flidev_t *d;

  if ((cam == NULL) || (dev == NULL))
    return -EINVAL;

  /* Check that flidev is a valid device */
  for (d = flicam->dev->port->first_dev; d != NULL; d = d->next)
    if (d == flidev)
      break;

  if (d == NULL)
    return -EINVAL;

  flicam->dev = (_flidev_t *)flidev;

  return 0;
}

/**
   Get the model of a given physical camera device.  This function
   copies up to \texttt{len - 1} characters of the model string for
   physical device \texttt{dev}, and a terminating \texttt{NULL}
   character, into the buffer pointed to by \texttt{model}.  If
   \texttt{len} is less than or equal to zero this function has no
   effect.

   @param dev Physical camera device to find model of.

   @param model Pointer to a character buffer where the model string
   is to be placed.

   @param len Size in bytes of buffer pointed to by \texttt{model}.

   @return Zero on success.
   @return Non-zero on failure.

   @see FLIGetFirmwareRev
   @see FLIGetSerialNum

*/
int FLIGetModel(flidev_t dev, char *model, size_t len)
{
  _flidev_t *flidev = (_flidev_t *)dev;

  if ((dev == NULL) || (model == NULL))
    return -EINVAL;

  if (len > 0)
  {
    strncpy(model, flidev->caminfo.model, len);
    model[len - 1] = '\0';
  }

  return 0;
}

/**
   Get the array area of the given virtual camera.  This function
   finds the \emph{total} area of the CCD array for virtual camera
   \texttt{cam}.  This area is specified in terms of a upper-left
   point and a lower-right point.  The upper-left x-coordinate is
   placed in \texttt{ul_x}, the upper-left y-coordinate is placed in
   \texttt{ul_y}, the lower-right x-coordinate is placed in
   \texttt{lr_x}, and the lower-right y-coordinate is placed in
   \texttt{lr_y}.

   @param cam Virtual camera to find array area of.

   @param ul_x Pointer to where the upper-left x-coordinate is to be
   placed.

   @param ul_y Pointer to where the upper-left y-coordinate is to be
   placed.

   @param lr_x Pointer to where the lower-right x-coordinate is to be
   placed.

   @param lr_y Pointer to where the lower-right y-coordinate is to be
   placed.

   @return Zero on success.
   @return Non-zero on failure.

   @see FLIGetVisibleArea
   @see FLISetImageArea

*/
int FLIGetArrayArea(flicam_t cam, int *ul_x, int *ul_y,
                    int *lr_x, int *lr_y)
{
  _flicam_t *flicam = (_flicam_t *)cam;
  _flidev_t *flidev;

  if ((cam == NULL) || (ul_x == NULL) || (ul_y == NULL) || (lr_x == NULL)
      || (lr_y == NULL) || ((flidev = flicam->dev) == NULL))
    return -EINVAL;

  *ul_x = flidev->caminfo.array_area.ul.x;
  *ul_y = flidev->caminfo.array_area.ul.y;
  *lr_x = flidev->caminfo.array_area.lr.x;
  *lr_y = flidev->caminfo.array_area.lr.y;

  return 0;
}

/**
   Get the visible area of the given virtual camera.  This function
   finds the \emph{visible} area of the CCD array for virtual camera
   \texttt{cam}.  This area is specified in terms of a upper-left
   point and a lower-right point.  The upper-left x-coordinate is
   placed in \texttt{ul_x}, the upper-left y-coordinate is placed in
   \texttt{ul_y}, the lower-right x-coordinate is placed in
   \texttt{lr_x}, the lower-right y-coordinate is placed in
   \texttt{lr_y}.

   @param cam Virtual camera to find visible area of.

   @param ul_x Pointer to where the upper-left x-coordinate is to be
   placed.

   @param ul_y Pointer to where the upper-left y-coordinate is to be
   placed.

   @param lr_x Pointer to where the lower-right x-coordinate is to be
   placed.

   @param lr_y Pointer to where the lower-right y-coordinate is to be
   placed.

   @return Zero on success.
   @return Non-zero on failure.

   @see FLISetImageArea
   @see FLIGetArrayArea

*/
int FLIGetVisibleArea(flicam_t cam, int *ul_x, int *ul_y,
                      int *lr_x, int *lr_y)
{
  _flicam_t *flicam = (_flicam_t *)cam;
  _flidev_t *flidev;

  if ((cam == NULL) || (ul_x == NULL) || (ul_y == NULL) || (lr_x == NULL)
      || (lr_y == NULL) || ((flidev = flicam->dev) == NULL))
    return -EINVAL;

  *ul_x = flidev->caminfo.visible_area.ul.x;
  *ul_y = flidev->caminfo.visible_area.ul.y;
  *lr_x = flidev->caminfo.visible_area.lr.x;
  *lr_y = flidev->caminfo.visible_area.lr.y;

  return 0;
}

/**
   Get the serial number of a given physical camera device.  This
   function copies the serial number for physical device \texttt{dev}
   into the location pointed to by \texttt{serialnum}.

   @param dev Physical camera device to find serial number of.

   @param serialnum Pointer to a integer location where the serial
   number is to be placed.

   @return Zero on success.
   @return Non-zero on failure.

   @see FLIGetModel
   @see FLIGetFirmwareRev

*/
int FLIGetSerialNum(flidev_t dev, int *serialnum)
{
  _flidev_t *flidev = (_flidev_t *)dev;

  if ((dev == NULL) || (serialnum == NULL))
    return -EINVAL;

  *serialnum = flidev->serialnum;

  return 0;
}

/**
   Get the firmware revision number of a given physical camera device.
   This function copies the firmware revision number for physical
   device \texttt{dev} into the location pointed to by
   \texttt{firmrev}.

   @param dev Physical camera device to find firmware revision number
   of.

   @param firmrev Pointer to a integer location where the firmware
   revision number is to be placed.

   @return Zero on success.
   @return Non-zero on failure.

   @see FLIGetModel
   @see FLIGetSerialNum

*/
int FLIGetFirmwareRev(flidev_t dev, int *firmrev)
{
  _flidev_t *flidev = (_flidev_t *)dev;

  if ((dev == NULL) || (firmrev == NULL))
    return -EINVAL;

  *firmrev = flidev->firmwarerev;

  return 0;
}

/**
   Expose a frame.  This function exposes a frame according to the
   settings (image area, exposure time, bit depth, etc.) of virtual
   camera \texttt{cam}.  The settings of \texttt{cam} must be valid
   for the physical camera device \texttt{cam} represents.  They are
   set by calling the appropriate set library function.  This function
   returns after the exposure has started.

   @param cam Virtual camera to expose frame of.

   @return Zero on success.
   @return Non-zero on failure.

   @see FLIGrabFrame
   @see FLICancelExposure
   @see FLIGetExposureStatus
   @see FLISetExposureTime
   @see FLISetFrameType
   @see FLISetImageArea
   @see FLISetHBin
   @see FLISetVBin
   @see FLISetNFlushes
   @see FLISetBitDepth

*/
int FLIExposeFrame(flicam_t cam)
{
  _flicam_t *flicam = (_flicam_t *)cam;
  _flidev_t *flidev;
  u_int16_t expdur = 0, expmul = 0;
  long ticks;
  int i;
  int min_remainder = INT_MAX;
  int retval;
  int dark;
  int res;

  if ((cam == NULL) || ((flidev = flicam->dev) == NULL))
    return -EINVAL;

  /* Check that cam parameters are okay */
  if (cam_parameters_not_ok(flicam))
    return -EINVAL;

  ticks = (long)(((double)flicam->exposure_time) / (8.192 /* msec */));

  /* Exposure time should be at least flicam->exposure_time */
  if ((double)ticks * 8.192 < (double)flicam->exposure_time)
    ticks++;

#ifdef _DEBUG_
  fprintf(stderr, "Exposure time %li (msec) = %li tick(s)\n",
          flicam->exposure_time, ticks);
#endif

  /* Find appropriate exposure multiplier */
  for (i = 1; i <= MIN(MAX(1, ticks/2), MAX_EXPMUL); i++)
  {
    int remainder = ticks % i;

    if ((ticks/i <= MAX_EXPMUL) && (remainder < min_remainder))
    {
      expdur = i;
      min_remainder = remainder;
      if (remainder == 0)
        break;
    }
  }

  expmul = ticks/expdur;

#ifdef _DEBUG_
  fprintf(stderr, "Exposure time %li (msec) => %i duration, "
          "%i multiplier = %i tick(s)\n",
          flicam->exposure_time, expdur, expmul, expdur * expmul);
#endif

  /* === Send Parameters for Scan === */

#ifdef _DEBUG_
  fprintf(stderr, "Setting X offset %i\n", flicam->image.ul.x);
#endif

  /* Set x offset */
  if ((retval = WriteData(flidev, D_XROWOFF(flicam->image.ul.x))))
    return retval;

#ifdef _DEBUG_
  fprintf(stderr, "Setting Row width %i\n",
          flidev->caminfo.array_area.lr.x - flidev->caminfo.array_area.ul.x);
#endif

  /* Set row width */
  if ((retval = WriteData(flidev,
                          D_XROWWID(flidev->caminfo.array_area.lr.x -
                                    flidev->caminfo.array_area.ul.x))))
    return retval;

#ifdef _DEBUG_
  fprintf(stderr, "Setting X flush bin factor %i\n", X_FLUSHBIN);
#endif

  /* Set x flush bin factor */
  if ((retval = WriteData(flidev, D_XFLBIN(X_FLUSHBIN))))
    return retval;

#ifdef _DEBUG_
  fprintf(stderr, "Setting Y flush bin factor %i\n", Y_FLUSHBIN);
#endif

  /* Set y flush bin factor */
  if ((retval = WriteData(flidev, D_YFLBIN(Y_FLUSHBIN))))
    return retval;

#ifdef _DEBUG_
  fprintf(stderr, "Setting horizontal bin factor %i\n", flicam->hbin);
#endif

  /* Set horizontal bin factor */
  if ((retval = WriteData(flidev, D_XBIN(flicam->hbin))))
    return retval;

#ifdef _DEBUG_
  fprintf(stderr, "Setting vertical bin factor %i\n", flicam->vbin);
#endif

  /* Set vertical bin factor */
  if ((retval = WriteData(flidev, D_YBIN(flicam->vbin))))
    return retval;

#ifdef _DEBUG_
  fprintf(stderr, "Setting Exposure duration %i\n", expdur);
#endif

  /* Set exposure duration */
  if ((retval = WriteData(flidev, D_EXPDUR(expdur))))
    return retval;

  res = (flicam->bitdepth == FLI_MODE_8BIT ? 7 : 15);
  if ((retval = WriteData(flidev, C_RESTCFG(0, 0 , res))))
    return retval;

#ifdef _DEBUG_
  fprintf(stderr, "Flushing Array\n");
#endif

  /* Flush entire array */
  if ((retval = FLIFlushRow(flicam, flidev->caminfo.array_area.lr.y -
                            flidev->caminfo.array_area.ul.y, flicam->nflushes)))
    return retval;

#ifdef _DEBUG_
  fprintf(stderr, "Exposing %s frame for %i multiple(s)\n",
          (flicam->frametype == FLI_FRAME_TYPE_DARK ? "dark" : "normal"),
          expmul);
#endif

  /* Expose frame*/
  dark = (flicam->frametype == FLI_FRAME_TYPE_DARK) ? 0 : 1;
  if ((retval = WriteCommand(flidev, C_SHUTTER(dark, expmul), NULL)))
    return retval;

#ifdef _DEBUG_
  fprintf(stderr, "Done %s\n", __PRETTY_FUNCTION__);
#endif

  flicam->expdur = expdur;

  return 0;
}

/**
   Cancel an exposure.  This function cancels an exposure in progress
   by closing the shutter.

   @param cam Virtual camera to cancel exposure of.

   @return Zero on success.
   @return Non-zero on failure.

   @see FLIExposeFrame
   @see FLIGetExposureStatus
   @see FLISetExposureTime

*/
int FLICancelExposure(flicam_t cam)
{
  _flicam_t *flicam = (_flicam_t *)cam;
  _flidev_t *flidev;
  int retval;

  if ((cam == NULL) || ((flidev = flicam->dev) == NULL))
    return -EINVAL;

  retval = WriteCommand(flidev, C_SHUTTER(0, 0), NULL);

  return retval;
}

/**
   Find the remaining exposure time.  This functions places the
   remaining exposure time (in milliseconds) in the location pointed
   to by \texttt{timeleft}.

   @param cam Virtual camera to find remaining exposure time of.

   @param timeleft Pointer to where the remaining exposure time in
   msec will be placed.

   @return Zero on success.
   @return Non-zero on failure.

   @see FLIExposeFrame
   @see FLICancelExposure
   @see FLISetExposureTime

*/
int FLIGetExposureStatus(flicam_t cam, long *timeleft)
{
  _flicam_t *flicam = (_flicam_t *)cam;
  _flidev_t *flidev;
  int retval;
  u_int16_t tmp;

  if ((cam == NULL) || (timeleft == NULL) ||
      ((flidev = flicam->dev) == NULL))
    return -EINVAL;

  if ((retval = WriteCommand(flidev, C_SHUTTER(1, 0), &tmp)))
    return retval;

  *timeleft = (long)((double)(tmp & 0x07ff) *
                     ((double)flicam->expdur * 8.192));

  return 0;
}

/**
   Grab a row of an image.  This function grabs the next available row
   of the image from virtual camera device \texttt{cam}.  The row of
   width \texttt{width} is placed in the buffer pointed to by
   \texttt{buff}.  The size of the buffer pointed to by \texttt{buff}
   must take into account the bit depth of the image, meaning the
   buffer size must be at least \texttt{width} bytes for an 8-bit
   image, and at least 2*\texttt{width} for a 16-bit image.

   @param cam Virtual camera whose image to grab the next available
   row from.

   @param buff Pointer to where the next available row will be placed.

   @param width Row width in pixels.

   @return Zero on success.
   @return Non-zero on failure.

   @see FLIGrabFrame

*/
int FLIGrabRow(flicam_t cam, void *buff, size_t width)
{
  _flicam_t *flicam = (_flicam_t *)cam;
  _flidev_t *flidev;
  _fliport_t *fliport;
  extern int errno;
  int retval;
  ssize_t read_count = width;
  u_int16_t tmp;

  if ((cam == NULL) || ((flidev = flicam->dev) == NULL) ||
      ((fliport = flidev->port) == NULL))
    return -EINVAL;

  /* Check that cam parameters are okay */
  if (cam_parameters_not_ok(flicam))
    return -EINVAL;

  errno = 0;

  /* Distinguish between 8 and 16-bit mode */
  if (flicam->bitdepth == FLI_MODE_16BIT)
    read_count = width * 2;

  FLI_LOCK(fliport->fd);

  /* Request row */
  if ((retval = WriteCommand(flidev, C_SEND(width), NULL)))
    return retval;

  if (read(fliport->fd, buff, read_count) != read_count)
  {
    if (errno == 0)
      errno = EIO;

    FLI_UNLOCK(fliport->fd);
    return -errno;
  }

  if (read(fliport->fd, &tmp, sizeof(tmp)) != sizeof(tmp))
  {
    if (errno == 0)
      errno = EIO;

    FLI_UNLOCK(fliport->fd);
    return -errno;
  }

  FLI_UNLOCK(fliport->fd);

  if (tmp != C_SEND(width))
    return -EIO;

  /* Adjust range of each data element */
  switch (flicam->bitdepth)
  {
    size_t i;
    u_int8_t *buff_8bit;
    u_int16_t *buff_16bit;

  case FLI_MODE_8BIT:
    buff_8bit = (u_int8_t *)buff;
    for (i = 0; i < read_count/sizeof(u_int8_t); i++)
      buff_8bit[i] += 0x80;
    break;

  case FLI_MODE_16BIT:
    buff_16bit = (u_int16_t *)buff;
    for (i = 0; i < read_count/sizeof(u_int16_t); i++)
      buff_16bit[i] += 0x8000;
    break;

  default:
    return -EINVAL;
  }

  return 0;
}

/**
   Grab a frame.  This function grabs a complete frame (image)
   according to the settings (image area, exposure time, bit depth,
   etc.) of virtual camera \texttt{cam}.  Up to \texttt{buffsize}
   bytes of the grabbed image are placed into the buffer pointed to by
   \texttt{buff}, and the actual number of bytes grabbed is placed in
   the location pointed to by \texttt{bytesgrabbed}.  The settings of
   \texttt{cam} must be valid for the physical camera device
   \texttt{cam} represents.  They are set by calling the appropriate
   set library function.  This function returns after the frame has
   been exposed and the image is acquired.  This includes starting the
   exposure, waiting until the exposure is complete, flushing rows
   above the image area, grabbing all rows of the image, and flushing
   any remaining rows below the image.

   @param cam Virtual camera to grab frame of.

   @param buff Pointer to where the grabbed frame will be placed.

   @param buffsize The size in bytes of the buffer pointed to by
   \texttt{buff}.

   @param bytesgrabbed Pointer to where the actual number of bytes
   grabbed will be placed.

   @return Zero on success.
   @return Non-zero on failure.

   @see FLISetExposureTime
   @see FLISetFrameType
   @see FLISetImageArea
   @see FLISetHBin
   @see FLISetVBin
   @see FLISetNFlushes
   @see FLISetBitDepth
   @see FLIExposeFrame
   @see FLIGetExposureStatus
   @see FLIFlushRow
   @see FLIGrabRow

*/
int FLIGrabFrame(flicam_t cam, void *buff, size_t buffsize,
                 size_t *bytesgrabbed)
{
  _flicam_t *flicam = (_flicam_t *)cam;
  _flidev_t *flidev;
  size_t row;
  long exptimeleft;             /* in msec */
  size_t row_width;             /* in pixels */
  size_t row_size;              /* in bytes */
  int retval;

  *bytesgrabbed = 0;

  if ((cam == NULL) || ((flidev = flicam->dev) == NULL))
    return -EINVAL;

  /* Check that cam parameters are okay */
  if (cam_parameters_not_ok(flicam))
    return -EINVAL;

  /* Expose frame */
  if ((retval = FLIExposeFrame(flicam)))
    return retval;

  /* Wait for expose to complete */
  for (exptimeleft = flicam->exposure_time; exptimeleft > 0; )
  {
    usleep(exptimeleft * 1000); /* exptimeleft is in msec */

    if ((retval = FLIGetExposureStatus(flicam, &exptimeleft)))
      return retval;
  }

#ifdef _DEBUG_
  fprintf(stderr, "Flushing %i rows above\n",
          flicam->image.ul.y - flidev->caminfo.array_area.ul.y);
#endif

  /* Flush rows above image */
  if ((retval = FLIFlushRow(flicam, flicam->image.ul.y -
                            flidev->caminfo.array_area.ul.y, 1)))
    return retval;

  row_width = flicam->image.lr.x - flicam->image.ul.x;
  row_size =  row_width * (flicam->bitdepth == FLI_MODE_8BIT ? 1 : 2);

#ifdef _DEBUG_
  fprintf(stderr, "Grabbing rows of image\n");
#endif

  /* Grab rows of image */
  for (row = 0; row < (size_t)(flicam->image.lr.y - flicam->image.ul.y); row++)
  {
    if ((row + 1) * row_size > buffsize)
      break;

    if ((retval = FLIGrabRow(cam, buff + row * row_size, row_width)))
      return retval;

    *bytesgrabbed += row_size;
  }

#ifdef _DEBUG_
  fprintf(stderr, "Flushing %i rows below\n", flidev->caminfo.array_area.lr.y
          - flicam->image.ul.y - flicam->image.lr.y);
#endif

  /* Flush rows below image */
  if ((retval = FLIFlushRow(flicam, flidev->caminfo.array_area.lr.y -
                            (flicam->image.ul.y + (flicam->image.lr.y -
                                                   flicam->image.ul.y) *
                             flicam->vbin), 1)))
    return retval;

  return 0;
}

/**

	NOTE: This function not included as part of 2.4 library distribution
	It is required by Talon, and was part of 2.2 distribution.
	Function body updated to reflect 2.4 methodology as expressed in FLIGrabFrame (read process)

   Read a frame. This function is to be called after as exposure is
   complete, as indicated by FLIGetExposureStatus() returning 0 time left,
   according to the settings (image area, exposure time, bit depth,
   etc.) of virtual camera \texttt{cam}.  Up to \texttt{buffsize}
   bytes of the grabbed image are placed into the buffer pointed to by
   \texttt{buff}, and the actual number of bytes grabbed is placed in
   the location pointed to by \texttt{bytesgrabbed}.  The settings of
   \texttt{cam} must be valid for the physical camera device
   \texttt{cam} represents.  They are set by calling the appropriate
   set library function. This function handles flushing rows
   above the image area, grabbing all rows of the image, and flushing
   any remaining rows below the image.

   @param cam Virtual camera to grab frame of.

   @param buff Pointer to where the grabbed frame will be placed.

   @param buffsize The size in bytes of the buffer pointed to by
   \texttt{buff}.

   @param bytesgrabbed Pointer to where the actual number of bytes
   grabbed will be placed.

   @return Zero on success.
   @return Non-zero on failure.

   @see FLISetExposureTime
   @see FLISetFrameType
   @see FLISetImageArea
   @see FLISetHBin
   @see FLISetVBin
   @see FLISetNFlushes
   @see FLISetBitDepth
   @see FLIExposeFrame
   @see FLIGetExposureStatus
   @see FLIFlushRow
   @see FLIGrabRow

*/
int FLIReadFrame(flicam_t cam, void *buff, size_t buffsize,
                 size_t *bytesgrabbed)
{
  _flicam_t *flicam = (_flicam_t *)cam;
  size_t row;
  size_t row_width;             /* in pixels */
  size_t row_size;              /* in bytes */
  int retval;
  _flidev_t *flidev;

  *bytesgrabbed = 0;

  if ((cam == NULL) || ((flidev = flicam->dev) == NULL))
    return -EINVAL;

#ifdef _DEBUG_
  fprintf(stderr, "Flushing %i rows above\n",
          flicam->image.ul.y - flidev->caminfo.array_area.ul.y);
#endif

  /* Flush rows above image */
  if ((retval = FLIFlushRow(flicam, flicam->image.ul.y -
                            flidev->caminfo.array_area.ul.y, 1)))
    return retval;

  row_width = flicam->image.lr.x - flicam->image.ul.x;
  row_size =  row_width * (flicam->bitdepth == FLI_MODE_8BIT ? 1 : 2);

#ifdef _DEBUG_
  fprintf(stderr, "Grabbing rows of image\n");
#endif

  /* Grab rows of image */
  for (row = 0; row < (size_t)(flicam->image.lr.y - flicam->image.ul.y); row++)
  {
    if ((row + 1) * row_size > buffsize)
      break;

    if ((retval = FLIGrabRow(cam, buff + row * row_size, row_width)))
      return retval;

    *bytesgrabbed += row_size;
  }

#ifdef _DEBUG_
  fprintf(stderr, "Flushing %i rows below\n", flidev->caminfo.array_area.lr.y
          - flicam->image.ul.y - flicam->image.lr.y);
#endif

  /* Flush rows below image */
  if ((retval = FLIFlushRow(flicam, flidev->caminfo.array_area.lr.y -
                            (flicam->image.ul.y + (flicam->image.lr.y -
                                                   flicam->image.ul.y) *
                             flicam->vbin), 1)))
    return retval;

  return 0;

}

/**
   Set the temperature of a given physical camera device.  This
   function sets the temperature of the CCD camera cold finger for
   physical device \texttt{dev} to \texttt{temperature} degrees
   Celsius.  The valid range of the \texttt{temperature} parameter is
   from -55 C to 45 C.

   @param dev Physical camera device to set temperature of.

   @param temperature Temperature in Celsius to set CCD camera cold
   finger to.

   @return Zero on success.
   @return Non-zero on failure.

   @see FLIGetTemperature

*/
int FLISetTemperature(flidev_t dev, double temperature)
{
  _flidev_t *flidev = (_flidev_t *)dev;
  u_int16_t ad;

  if (dev == NULL)
    return -EINVAL;

  if ((temperature < FLI_MIN_TEMP) || (temperature > FLI_MAX_TEMP))
    return -EINVAL;

  ad = (u_int16_t)((temperature - temp_intercept) / temp_slope);

  return WriteCommand(flidev, C_TEMP(ad), NULL);
}

/**
   Get the temperature of a given physical camera device.  This
   function places the temperature of the CCD camera cold finger of
   physical device \texttt{dev} in the location pointed to by
   \texttt{temperature}.

   @param dev Physical camera device to get temperature of.

   @param temperature Pointer to where the temperature will be placed.

   @return Zero on success.
   @return Non-zero on failure.

   @see FLISetTemperature

*/
int FLIGetTemperature(flidev_t dev, double *temperature)
{
  _flidev_t *flidev = (_flidev_t *)dev;
  u_int16_t ad;
  int retval;

  if ((dev == NULL) || (temperature == NULL))
    return -EINVAL;

  if ((retval = WriteCommand(flidev, C_TEMP(0x800), &ad)))
    return retval;

  *temperature = temp_slope * (double)(ad & 0x00ff) + temp_intercept;

  return 0;
}

/**
   Flush rows of a given virtual camera.  This function flushes
   \texttt{rows} rows of virtual camera \texttt{cam}, \texttt{repeat}
   times.

   @param cam Virtual camera to flush rows of.

   @param rows Number of rows to flush.

   @param repeat Number of times to flush each row.

   @return Zero on success.
   @return Non-zero on failure.

*/
int FLIFlushRow(flicam_t cam, int rows, int repeat)
{
  _flicam_t *flicam = (_flicam_t *)cam;
  _flidev_t *flidev;
  _fliport_t *fliport;
  int retval;
  int saved_rto;

  if ((cam == NULL) || ((flidev = flicam->dev) == NULL) ||
      ((fliport = flidev->port) == NULL))
    return -EINVAL;

  if ((rows < 0) ||
      (rows > MIN(FLI_MAX_ROWS, flidev->caminfo.array_area.lr.y -
                  flidev->caminfo.array_area.ul.y)))
    return -EINVAL;

  if ((repeat < FLI_MIN_REPEAT) || (repeat > FLI_MAX_REPEAT))
    return -EINVAL;

  /* Adjust read timeout */
  FLI_IOCTL(fliport->fd, FLI_GET_RTO, &saved_rto);
  FLI_IOCTL(fliport->fd, FLI_SET_RTO, &FLUSHROW_RTO);

  while (repeat-- > 0)
    if ((retval = WriteCommand(flidev, C_FLUSH(rows), NULL)))
      return retval;

  /* Restore read timeout to original value */
  FLI_IOCTL(fliport->fd, FLI_SET_RTO, &saved_rto);

  return 0;
}

/**
   Set the exposure time for a given virtual camera.  This function
   sets the exposure time for virtual camera \texttt{cam} to
   \texttt{exptime} msec.  The valid range of the \texttt{exptime}
   parameter is from 8 to 68669153.

   @param cam Virtual camera to set exposure time of.

   @param exptime Exposure time in msec.

   @return Zero on success.
   @return Non-zero on failure.

   @see FLIExposeFrame
   @see FLICancelExposure
   @see FLIGetExposureStatus

*/
int FLISetExposureTime(flicam_t cam, long exptime)
{
  _flicam_t *flicam = (_flicam_t *)cam;

  if (cam == NULL)
    return -EINVAL;

  if ((exptime < FLI_MIN_EXPOSURE_TIME) || (exptime > FLI_MAX_EXPOSURE_TIME))
    return -EINVAL;

  flicam->exposure_time = exptime;
  return 0;
}

/**
   Set the frame type for a given virtual camera.  This function sets
   the frame type for virtual camera \texttt{cam} to
   \texttt{frametype}.  The \texttt{frametype} parameter is either
   \texttt{FLI_FRAME_TYPE_NORMAL} for a normal frame where the shutter
   opens or \texttt{FLI_FRAME_TYPE_DARK} for a dark frame where the
   shutter remains closed.

   @param cam Virtual camera to set exposure time of.

   @param frametype Frame type: \texttt{FLI_FRAME_TYPE_NORMAL} or
   \texttt{FLI_FRAME_TYPE_DARK}.

   @return Zero on success.
   @return Non-zero on failure.

*/
int FLISetFrameType(flicam_t cam, fliframe_t frametype)
{
  _flicam_t *flicam = (_flicam_t *)cam;

  if (cam == NULL)
    return -EINVAL;

  if ((frametype < FLI_FRAME_TYPE_NORMAL) || (frametype > FLI_FRAME_TYPE_DARK))
    return -EINVAL;

  flicam->frametype = frametype;
  return 0;
}

/**
   Set the image area for a given virtual camera.  This function sets
   the image area for virtual camera \texttt{cam} to an area specified
   in terms of a upper-left point and a lower-right point.  The
   upper-left x-coordinate is \texttt{ul_x}, the upper-left
   y-coordinate is \texttt{ul_y}, the lower-right x-coordinate is
   \texttt{lr_x}, and the lower-right y-coordinate is \texttt{lr_y}.
   Note that the given lower-right coordinate must take into account
   the horizontal and vertical bin factor settings, but the upper-left
   coordinate is absolute.  In other words, the lower-right coordinate
   used to set the image area is a virtual point $(lr_x^\prime,
   lr_y^\prime)$ determined by:

   \[ lr_x^\prime = ul_x + (lr_x - ul_x)/hbin \]
   \[ lr_y^\prime = ul_y + (lr_y - ul_y)/vbin \]

   Where $(lr_x^\prime, lr_y^\prime)$ is the coordinate to pass to the
   \texttt{FLISetImageArea} function, $(ul_x, ul_y)$ and $(lr_x,
   lr_y)$ are the absolute coordinates of the desired image area,
   $hbin$ is the horizontal bin factor, and $vbin$ is the vertical bin
   factor.

   @param cam Virtual camera to set image area of.

   @param ul_x Upper-left x-coordinate of image area.

   @param ul_y Upper-left y-coordinate of image area.

   @param lr_x Lower-right x-coordinate of image area ($lr_x^\prime$
   from above).

   @param lr_y Lower-right y-coordinate of image area ($lr_y^\prime$
   from above).

   @return Zero on success.
   @return Non-zero on failure.

   @see FLIGetVisibleArea
   @see FLIGetArrayArea

*/
int FLISetImageArea(flicam_t cam, int ul_x, int ul_y, int lr_x, int lr_y)
{
  _flicam_t *flicam = (_flicam_t *)cam;
  _flidev_t *flidev;

  if ((cam == NULL) || ((flidev = flicam->dev) == NULL))
    return -EINVAL;

  if ((ul_x < flidev->caminfo.array_area.ul.x) ||
      (ul_y < flidev->caminfo.array_area.ul.y) ||
      (lr_x > flidev->caminfo.array_area.lr.x) ||
      (lr_y > flidev->caminfo.array_area.lr.y) ||
      (lr_x <= ul_x) ||
      (lr_y <= ul_y))
    return -EINVAL;

  flicam->image.ul.x = ul_x;
  flicam->image.ul.y = ul_y;
  flicam->image.lr.x = lr_x;
  flicam->image.lr.y = lr_y;

  return 0;
}

/**
   Set the horizontal bin factor for a given virtual camera.  This
   function sets the horizontal bin factor for virtual camera
   \texttt{cam} to \texttt{hbin}.  The valid range of the
   \texttt{hbin} parameter is from 1 to 4095.  Note that the
   horizontal bin factor effects the image area.

   @param cam Virtual camera to set horizontal bin factor of.

   @param hbin Horizontal bin factor.

   @return Zero on success.
   @return Non-zero on failure.

   @see FLISetVBin
   @see FLISetImageArea

*/
int FLISetHBin(flicam_t cam, int hbin)
{
  _flicam_t *flicam = (_flicam_t *)cam;

  if (cam == NULL)
    return -EINVAL;

  if ((hbin < FLI_MIN_HBIN) || (hbin > FLI_MAX_HBIN))
    return -EINVAL;

  flicam->hbin = hbin;

  return 0;
}

/**
   Set the vertical bin factor for a given virtual camera.  This
   function sets the vertical bin factor for virtual camera
   \texttt{cam} to \texttt{vbin}.  The valid range of the
   \texttt{vbin} parameter is from 1 to 4095.  Note that the vertical
   bin factor effects the image area.

   @param cam Virtual camera to set vertical bin factor of.

   @param vbin Vertical bin factor.

   @return Zero on success.
   @return Non-zero on failure.

   @see FLISetHBin
   @see FLISetImageArea

*/
int FLISetVBin(flicam_t cam, int vbin)
{
  _flicam_t *flicam = (_flicam_t *)cam;

  if (cam == NULL)
    return -EINVAL;

  if ((vbin < FLI_MIN_VBIN) || (vbin > FLI_MAX_VBIN))
    return -EINVAL;

  flicam->vbin = vbin;

  return 0;
}

/**
   Set the number of flushes for a given virtual camera.  This
   function sets the number of times the CCD array of virtual camera
   \texttt{cam} is flushed \emph{before} exposing a frame to
   \texttt{nflushes}.  The valid range of the \texttt{nflushes}
   parameter is from 1 to 4095.

   @param cam Virtual camera to set the number of flushes of.

   @param nflushes Number of times to flush CCD array before an
   exposure.

   @return Zero on success.
   @return Non-zero on failure.

*/
int FLISetNFlushes(flicam_t cam, int nflushes)
{
  _flicam_t *flicam = (_flicam_t *)cam;

  if (cam == NULL)
    return -EINVAL;

  if ((nflushes < FLI_MIN_FLUSHES) || (nflushes > FLI_MAX_FLUSHES))
    return -EINVAL;

  flicam->nflushes = nflushes;

  return 0;
}

/**
   Set the gray-scale bit depth for a given virtual camera.  This
   function sets the gray-scale bit depth of virtual camera
   \texttt{cam} to \texttt{bitdepth}.  The \texttt{bitdepth} parameter
   is either \texttt{FLI_MODE_8BIT} for 8-bit mode or
   \texttt{FLI_MODE_16BIT} for 16-bit mode.

   @param cam Virtual camera to set the number of flushes of.

   @param bitdepth Gray-scale bit depth: \texttt{FLI_MODE_8BIT} or
   \texttt{FLI_MODE_16BIT}.

   @return Zero on success.
   @return Non-zero on failure.

*/
int FLISetBitDepth(flicam_t cam, flibitdepth_t bitdepth)
{
  _flicam_t *flicam = (_flicam_t *)cam;

  if (cam == NULL)
    return -EINVAL;

  if ((bitdepth < FLI_MODE_8BIT) || (bitdepth > FLI_MODE_16BIT))
    return -EINVAL;

  flicam->bitdepth = bitdepth;

  return 0;
}
