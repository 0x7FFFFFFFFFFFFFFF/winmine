/* resource.h - resource identifiers */
#ifndef RESOURCE_H
#define RESOURCE_H

/* ---- icon ---- */
#define ID_ICON_MAIN        100

/* ---- bitmaps (colour id, monochrome id = colour id + 1) ---- */
#define ID_BMP_BLOCKS       410
#define ID_BMP_BLOCKS_MONO  411
#define ID_BMP_LED          420
#define ID_BMP_LED_MONO     421
#define ID_BMP_FACE         430
#define ID_BMP_FACE_MONO    431

/* ---- waves ---- */
#define ID_WAV_TICK         432
#define ID_WAV_WIN          433
#define ID_WAV_LOSE         434

/* ---- menu / accelerator ---- */
#define ID_MENU             500
#define ID_ACCEL            501

/* ---- menu commands ---- */
#define IDM_NEW             510
#define IDM_EXIT            512
#define IDM_BEGIN           521
#define IDM_INTER           522
#define IDM_EXPERT          523
#define IDM_CUSTOM          524
#define IDM_SOUND           526
#define IDM_MARK            527
#define IDM_BEST            528
#define IDM_COLOR           529
#define IDM_HELP            590
#define IDM_HELP_SEARCH     591
#define IDM_HELP_USING      592
#define IDM_ABOUT           593

/* ---- dialogs ---- */
#define DLG_PREF            80      /* Custom Field           */
#define DLG_ENTER           600     /* enter your name        */
#define DLG_BEST            700     /* Fastest Mine Sweepers  */

/* Custom Field controls */
#define ID_PREF_HEIGHTTEXT  112
#define ID_PREF_WIDTHTEXT   113
#define ID_PREF_MINETEXT    111
#define ID_PREF_HEIGHT      141
#define ID_PREF_WIDTH       142
#define ID_PREF_MINE        143
#define ID_PREF_RANDOM      144

/* Enter-name controls */
#define ID_ENTER_PROMPT     601
#define ID_ENTER_NAME       602

/* Best-times controls */
#define ID_BEST_TIME1       701
#define ID_BEST_NAME1       702
#define ID_BEST_TIME2       703
#define ID_BEST_NAME2       704
#define ID_BEST_TIME3       705
#define ID_BEST_NAME3       706
#define ID_BEST_RESET       707
#define ID_BEST_LBL1        708
#define ID_BEST_LBL2        709
#define ID_BEST_LBL3        710

/* ---- strings ---- */
#define IDS_NAME            1
#define IDS_ERR_TITLE       3
#define IDS_ERR_TIMER       4
#define IDS_ERR_MEMORY      5
#define IDS_ERR_UNKNOWN     6
#define IDS_SECONDS         7
#define IDS_ANONYMOUS       8
#define IDS_BEST_BEGIN      9
#define IDS_BEST_INTER      10
#define IDS_BEST_EXPERT     11
#define IDS_ABOUT_NAME      12
#define IDS_ABOUT_AUTHOR    13

#endif /* RESOURCE_H */
