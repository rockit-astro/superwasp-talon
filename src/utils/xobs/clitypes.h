#ifndef __CLITYPES_H__
#define __CLITYPES_H__

/* Structure describing a single catalogue object */
struct cli_object {
  float ra;        /* RA (rad) */
  float dec;       /* Dec (rad) */
  float equinox;   /* Equinox of position */
  char coordtype;  /* Coordinate system (A or J) */
};

/* Structure describing rastering parameters */
struct cli_raster {
  int state;
  float size;
};

/* Structure describing an exposure */
struct cli_expose {
  int type;
#define CLI_EXPOSE_BIAS    1
#define CLI_EXPOSE_DARK    2
#define CLI_EXPOSE_FLAT    3
#define CLI_EXPOSE_OBJECT  4
  float dur;
};

/* Constants for zerosetting - ORed together
 * Telescope only for the moment - I'm lazy
 */
#define CLI_HOME_HA     0x01
#define CLI_HOME_DEC    0x02
#define CLI_HOME_ROT    0x04

#endif  /* __CLITYPES_H__ */
