typedef enum {
    /* so none are referenced by default */
    GUI_NONE = 0,

    /* position */
    PCRA_W,
    PCDEC_W,
    PCHA_W,
    PCALT_W,
    PCAZ_W,
    PTRA_W,
    PTDEC_W,
    PTHA_W,
    PTALT_W,
    PTAZ_W,
    PDRA_W,
    PDDEC_W,
    PDHA_W,
    PDALT_W,
    PDAZ_W,

    /* control */
    CSTOP_W,
    CEXIT_W,
    CFHOME_W,
    CFLIM_W,
    CRELOAD_W,
    CCALIBA_W,
    CPADDLE_W,
    CBATCH_W,
    CCNFOFF_W,
    CSOUND_W,

    /* status */
    SBLT_W,
    STLT_W,
    SSLT_W,
    SHLT_W,
    SLLT_W,
    SWLT_W,
    SCLT_W,

    /* dome and shutter */
    DOLT_W,
    DCLT_W,
    DOPEN_W,
    DCLOSE_W,
    DAUTO_W,
    DAZLT_W,
    DAZL_W,
    DAZ_W,

    /* telescope */
    TSERV_W,
    TSTOW_W,
    TGOTO_W,
    THERE_W,
    TLOOK_W,
    TTRACK_W,
    TOBJ_W,
    TRA_W,
    TDEC_W,
    TEPOCH_W,
    THA_W,
    TALT_W,
    TAZ_W,

    /* info */
    ILT_W,
    IUT_W,
    IUTD_W,
    ILST_W,
    IJD_W,
    IMOON_W,
    ISUN_W,
    IDUSK_W,
    IDAWN_W,

    /* number of */
    N_W,
} GUIWidgets;

extern Widget g_w[N_W];

/* For RCS Only -- Do Not Edit
 * @(#) $RCSfile: widgets.h,v $ $Date: 2006/01/13 20:42:47 $ $Revision: 1.1.1.1 $ $Name:  $
 */
