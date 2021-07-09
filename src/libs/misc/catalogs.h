/* prototypes for functions in catalogs.c */

#include "circum.h"

extern int searchCatalog(char catdir[], char catalog[], char source[], Obj *op, char message[]);
extern int readCatalog(char fn[], Obj **opp, char message[]);
extern int searchDirectory(char catdir[], char source[], Obj *op, char m[]);
