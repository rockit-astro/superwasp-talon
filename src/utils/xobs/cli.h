#ifndef __CLI_H__
#define __CLI_H__

/* Structure to store client information */
struct cliclient
{
    int fd;
    XtInputId readid;
    unsigned char readidon;
    XtInputId writeid;
    unsigned char writeidon;

    char inbuf[16384];
    int inoff;
    int inrem;
    char outbuf[16384];
    int outoff;
    int outrem;

    int cmd; /* command this client is executing */

    struct cliclient *next;
    struct cliclient *prev;
};

/* In cli.c */
void cli_add_write(struct cliclient *c);

/* In clicommands.c */
void initCliCommand(void);
void clientDisconnect(struct cliclient *c);
int docommand(struct cliclient *c, unsigned char cmd, int paylen, unsigned char *paybuf, char *errstr);

#endif /* __CLI_H__ */
