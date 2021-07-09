#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "running.h"

#include "cliutil.h"

extern char *progname;

void fatal(int code, const char *fmt, ...)
{
    va_list ap;

    if (progname[0])
        (void)fprintf(stderr, "%s: ", progname);
    va_start(ap, fmt);
    if (fmt)
        (void)vfprintf(stderr, fmt, ap);
    va_end(ap);
    (void)fprintf(stderr, "\n");

    unlock_running(progname, 0);
    abort();
    // exit(code);
}

void warning(const char *fmt, ...)
{
    va_list ap;

    if (progname[0])
        (void)fprintf(stderr, "%s: ", progname);
    va_start(ap, fmt);
    if (fmt)
        (void)vfprintf(stderr, fmt, ap);
    va_end(ap);
    (void)fprintf(stderr, "\n");
}

void error(int code, const char *fmt, ...)
{
    va_list ap;

    if (progname[0])
        (void)fprintf(stderr, "%s: ", progname);
    va_start(ap, fmt);
    if (fmt)
    {
        (void)vfprintf(stderr, fmt, ap);
        (void)fprintf(stderr, ": ");
    }
    va_end(ap);
    (void)fprintf(stderr, "%s\n", strerror(errno));

    unlock_running(progname, 0);
    abort();
    // exit(code);
}

void syswarn(const char *fmt, ...)
{
    va_list ap;

    if (progname[0])
        (void)fprintf(stderr, "%s: ", progname);
    va_start(ap, fmt);
    if (fmt)
    {
        (void)vfprintf(stderr, fmt, ap);
        (void)fprintf(stderr, ": ");
    }
    va_end(ap);
    (void)fprintf(stderr, "%s\n", strerror(errno));
}

void report_err(char *errstr, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(errstr, ERRSTR_LEN, fmt, ap);
    va_end(ap);
}

void report_syserr(char *errstr, const char *fmt, ...)
{
    va_list ap;
    int rv;

    va_start(ap, fmt);
    rv = vsnprintf(errstr, ERRSTR_LEN, fmt, ap);
    va_end(ap);

    if (rv != -1 && rv < ERRSTR_LEN)
        (void)snprintf(errstr + rv, ERRSTR_LEN - rv, ": %s", strerror(errno));
}

char *sstrip(char *str)
{
    char *p;

    /* First remove whitespace from start of string */
    while (*str != '\0' && isspace((unsigned char)*str))
        str++;

    if (*str == '\0')
        return (str);

    /* Remove whitespace from end of string */
    p = str + strlen(str) - 1;

    while (p > str && isspace((unsigned char)*p))
        p--;

    if (p == str && isspace((unsigned char)*p))
        *p = '\0';
    else
        *(p + 1) = '\0';

    return (str);
}
