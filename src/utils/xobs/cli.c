/* SuperWASP command-line interface
 *
 * Jonathan Irwin (jmi@ast.cam.ac.uk)
 *
 */

#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>

#include <Xm/Label.h>
#include <Xm/PushB.h>
#include <Xm/RowColumn.h>
#include <Xm/Form.h>

#include "P_.h"
#include "astro.h"
#include "circum.h"
#include "telstatshm.h"
#include "configfile.h"
#include "xtools.h"
#include "strops.h"
#include "telenv.h"
#include "xobs.h"

#include "cli.h"
#include "cliutil.h"

extern Widget toplevel_w;

static void srvinput_cb (XtPointer client_data, int *fd, XtInputId *id);
static void cliinput_cb (XtPointer client_data, int *fd, XtInputId *id);
static void clioutput_cb (XtPointer client_data, int *fd, XtInputId *id);

static char sktpath[1024];
char cli_tmp_image[1024];
static int srvfd = -1;
static XtInputId srvid;
static unsigned char srvidon = 0;

/* List of connected client programs */
static struct cliclient *client_list = (struct cliclient *) NULL;

void initCli (void) {
  char *p;
  int rv, flags;
  struct sockaddr_un s_un;

  /* Initialise */
  cleanupCli();

  /* Get UNIX domain socket path */
  p = getXRes(toplevel_w, "CliSocket", "comm/cliskt");
  telfixpath(sktpath, p);

  /* Get temp image path */
  p = getXRes(toplevel_w, "CliTempImage", "/tmp/cli_temp.fits");
  telfixpath(cli_tmp_image, p);

  /* Create UNIX domain socket for clients */
  unlink(sktpath);

  srvfd = socket(AF_UNIX, SOCK_STREAM, 0);
  if(srvfd == -1)
    error(1, "socket: AF_UNIX");

  memset(&s_un, 0, sizeof(s_un));
  s_un.sun_family = AF_UNIX;
  strncpy(s_un.sun_path, sktpath, sizeof(s_un.sun_path));
  s_un.sun_path[sizeof(s_un.sun_path) - 1] = '\0';

  rv = bind(srvfd, (struct sockaddr *) &s_un, SUN_LEN(&s_un));
  if(rv == -1)
    error(1, "bind: %s", sktpath);

  flags = fcntl(srvfd, F_GETFL, 0);
  if(flags == -1)
    error(1, "fcntl: F_GETFL");

  flags |= O_NONBLOCK;

  if(fcntl(srvfd, F_SETFL, flags) == -1)
    error(1, "fcntl: F_SETFL");

  rv = listen(srvfd, 5);
  if(rv == -1)
    error(1, "listen");

  /* Add input handler */
  srvid = XtAppAddInput(app, srvfd, (XtPointer) XtInputReadMask,
			srvinput_cb, (XtPointer) NULL);
  srvidon = 1;
}

void cleanupClient (struct cliclient *c) {

  /* Check state of command execution */
  clientDisconnect(c);

  /* Remove input callbacks */
  if(c->readidon)
    XtRemoveInput(c->readid);
  if(c->writeidon)
    XtRemoveInput(c->writeid);

  /* Close FDs */
  if(c->fd != -1)
    close(c->fd);

  /* Unlink from the list */
  if(c->prev)
    c->prev->next = c->next;
  if(c->next)
    c->next->prev = c->prev;

  if(c == client_list)
    client_list = (c->next ? c->next : (struct cliclient *) NULL);

  /* Free structure */
  free((void *) c);
}

void cleanupCli (void) {
  struct cliclient *c;

  /* Cleanup clients */
  for(c = client_list; c; c = c->next)
    cleanupClient(c);

  /* Remove input handlers */
  if(srvidon)
    XtRemoveInput(srvid);

  /* Close sockets */
  if(srvfd != -1) {
    close(srvfd);
    unlink(sktpath);
  }

  /* Set variables */
  client_list = (struct cliclient *) NULL;
  srvidon = 0;
  srvfd = -1;
}

void cli_add_write (struct cliclient *c) {
  if(c->fd != -1 && c->outoff > 0) {
    c->writeid = XtAppAddInput(app, c->fd, (XtPointer) XtInputWriteMask,
			       clioutput_cb, (XtPointer) c);
    c->writeidon = 1;
  }
}

/* Connection from client program */

static void srvinput_cb (XtPointer client_data, int *fd, XtInputId *id) {
  int clifd;
  struct sockaddr_un cliaddr;
  socklen_t clialen;
  struct cliclient *c, *t;

  /* Allocate new client structure */
  c = (struct cliclient *) malloc(sizeof(struct cliclient));
  if(!c) {
    syswarn("malloc");
    return;
  }

  /* Accept connection */
  clifd = accept(srvfd, (struct sockaddr *) &cliaddr, &clialen);
  if(clifd == -1) {
    syswarn("accept");
    return;
  }

  /* Initialise it */
  c->fd = clifd;
  c->readidon = 0;
  c->writeidon = 0;
  c->inoff = 0;
  c->inrem = sizeof(c->inbuf);
  c->outoff = 0;
  c->outrem = sizeof(c->outbuf);
  c->cmd = 0;
  c->next = (struct cliclient *) NULL;

  /* Add client input handler */
  c->readid = XtAppAddInput(app, clifd, (XtPointer) XtInputReadMask,
			    cliinput_cb, (XtPointer) c);
  c->readidon = 1;

  /* Add to the list */
  if(client_list) {
    for(t = client_list; t && t->next; t = t->next)
      ;
    
    t->next = c;
    c->prev = t;
  }
  else {
    client_list = c;
    c->prev = (struct cliclient *) NULL;
  }
}

/* Input from client program */

static void cliinput_cb (XtPointer client_data, int *fd, XtInputId *id) {
  struct cliclient *c;
  int rv, i, ir, paylen = 0;
  char errstr[ERRSTR_LEN];

  /* Get client structure */
  c = (struct cliclient *) client_data;

  /* Read */
  rv = read(c->fd, &(c->inbuf[c->inoff]), c->inrem);
  if(rv == -1) {
    if(errno == EINTR)
      return;
    else if(errno == EWOULDBLOCK)
      return;
    else if(errno == ECONNRESET)
      /* Same as returning zero */
      rv = 0;
    else {
      syswarn("read");
      rv = 0;
    }
  }

  if(rv == 0) {
    /* Disconnected: ditch output and close socket */
    cleanupClient(c);
    return;
  }

  c->inoff += rv;
  c->inrem -= rv;

  /* Process command buffer contents */
  for(i = 0; i < c->inoff; i++) {
    ir = c->inoff - i - 1;
    
    if(ir < sizeof(int))
      break;
    
    memcpy(&paylen, &(c->inbuf[i+1]), sizeof(int));
    
    if(ir < paylen)
      break;
    
#ifdef CLI_DEBUG
    msg("DEBUG: Executing command %d with paylen %d", c->inbuf[i], paylen);
#endif    

    if(docommand(c, c->inbuf[i], paylen, &(c->inbuf[i+1+sizeof(int)]), errstr))
      warning("docommand: %s", errstr);

#ifdef CLI_DEBUG
    msg("DEBUG: reply length %d", c->outoff);
#endif

    i += sizeof(int) + paylen;
  }
  
  memmove(&(c->inbuf[0]), &(c->inbuf[i]), c->inoff - i);
  
  c->inoff -= i;
  c->inrem += i;
  
  /* Setup write handler */
  cli_add_write(c);
}

/* Output to client program */

static void clioutput_cb (XtPointer client_data, int *fd, XtInputId *id) {
  struct cliclient *c;
  int rv;

  /* Get client structure */
  c = (struct cliclient *) client_data;

  if(c->outoff > 0) {
    rv = write(c->fd, c->outbuf, c->outoff);
    if(rv == -1) {
      if(errno == EINTR || errno == EWOULDBLOCK)
	return;
      else if(errno == EPIPE || errno == ECONNRESET) {
	/* Disconnected: ditch buffer contents and close socket */
	cleanupClient(c);
	return;
      }
      else {
	syswarn("write");
	cleanupClient(c);
	return;
      }
    }
    
    if(rv > 0)
      memmove(&(c->outbuf[0]), &(c->outbuf[rv]), c->outoff - rv);
    
    c->outoff -= rv;
    c->outrem += rv;
  }

  /* Remove write handler */
  if(c->outoff < 1) {
    XtRemoveInput(c->writeid);
    c->writeidon = 0;
  }
}
