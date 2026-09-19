/*=====================================================================
    miner.cpp  --  Minesweeper.

    A classic 16-pixel-per-cell Minesweeper: three preset levels plus a
    custom field, question marks, chording, a clock, best times and the
    XYZZY peek.  Everything is drawn with GDI straight from the bitmaps
    in the resource file, and the whole program is one translation unit
    that links without the C run-time (see build.cmd).

    Board encoding -- one byte per cell, 32 bytes per row:

        0x80            the cell contains a mine
        0x40            the cell has been uncovered
        0x1F            index of the tile to blit (see BLK_* below)

  =====================================================================*/

#define WIN32_LEAN_AND_MEAN
#define WINVER          0x0501
#define _WIN32_WINNT    0x0501

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commctrl.h>
#include <mmsystem.h>
#include <objbase.h>            /* CoCreateInstance, for ITaskbarList2 */

#include "resource.h"

/*---------------------------------------------------------------------
    geometry
  -------------------------------------------------------------------*/
#define DX_BLK          16      /* a field cell                       */
#define DY_BLK          16
#define DX_LED          13      /* one seven-segment digit            */
#define DY_LED          23
#define DX_BUTTON       24      /* the smiley                         */
#define DY_BUTTON       24

#define X_FIELD         12      /* first pixel column of the field    */
#define Y_FIELD         55      /* first pixel row of the field       */

#define DX_WINDOW       24      /* client width  = cBlk * 16 + this   */
#define DY_WINDOW       67      /* client height = cRow * 16 + this   */

#define Y_LED           16      /* top of the LED digits              */
#define Y_BUTTON        16      /* top of the smiley                  */

/*---------------------------------------------------------------------
    Board limits.  The board is allocated to fit, so the custom dialog
    imposes no ceiling of its own: type whatever you like.  What stops
    you is the machine - FAllocBoard() works out whether the board, the
    flood-fill queue and the offscreen surface can all be had, and a
    field that is too big for them is refused without disturbing the
    game already in progress.
  -------------------------------------------------------------------*/
#define CBLK_MIN        9       /* narrowest field                    */
#define CROW_MIN        9       /* shortest field                     */
#define CMINE_MIN       10      /* fewest mines                       */

/* The one hard limit on a field: a window edge Windows will still
   place.  Everything else about how big a field may be is settled by
   asking the machine (see FSurfaceFits) rather than guessing, because
   a guess can only be wrong in one direction or the other.  At 16
   pixels a square this leaves room for 1,873 squares along each
   side. */
#define DXY_SURFACE_MAX 30000   /* largest client edge, in pixels     */

/* The largest window, in device pixels, the desktop compositor will
   still draw - see FWindowFitsSurface.  300 million pixels is about
   1.2GB of surface, comfortably inside where it was measured to stop
   drawing, and it is what holds the zoom down on a very large field.
   Every field the game will build is displayable at some zoom: the
   biggest allowed is 30,000 logical pixels a side, which at the 25%
   floor is 7,500 device pixels a side - 56 million, well under. */
#define CPX_WINDOW_MAX  300000000

/*---------------------------------------------------------------------
    zoom - ctrl + wheel, as a percentage of the 1:1 layout
  -------------------------------------------------------------------*/
#define ZOOM_MIN        25
#define ZOOM_STEP       25
#define DXY_ZOOM_MAX    30000   /* largest window edge zoom may ask for */

/*---------------------------------------------------------------------
    the Random button in the custom dialog picks each side from this
    range, then works out a sensible number of mines to go with it
  -------------------------------------------------------------------*/
#define RAND_SIDE_MIN   50
#define RAND_SIDE_MAX   200

/*---------------------------------------------------------------------
    ctrl+T corner peek: a two-by-two block - four pixels - in the very
    bottom-left of the client area.  It is painted straight onto the
    window in device pixels, so it stays four screen pixels whatever
    the zoom is doing to everything else.
  -------------------------------------------------------------------*/

/*---------------------------------------------------------------------
    tile indices inside the 16-tile block bitmap
  -------------------------------------------------------------------*/
#define BLK_0           0       /* uncovered, no neighbouring mines   */
#define BLK_8           8       /* uncovered, eight neighbours        */
#define BLK_GUESSDN     9       /* '?' held down                      */
#define BLK_BOMBUP      10      /* a mine, revealed after a loss      */
#define BLK_WRONG       11      /* a flag that was not a mine         */
#define BLK_EXPLODE     12      /* the mine that was stepped on       */
#define BLK_GUESSUP     13      /* '?'                                */
#define BLK_BOMBFLAG    14      /* flag                               */
#define BLK_BLANKUP     15      /* untouched cell                     */
#define BLK_BORDER      16      /* off-board sentinel                 */

#define MASK_BOMB       0x80
#define MASK_VISIT      0x40
#define MASK_SWEEP      0x20    /* scratch mark, see SweepRegion()     */
#define MASK_ICON       0x1F

/*---------------------------------------------------------------------
    smiley indices inside the 5-tile face bitmap
  -------------------------------------------------------------------*/
#define FACE_HAPPY      0
#define FACE_CAUTION    1       /* while a cell is held down          */
#define FACE_DEAD       2
#define FACE_WIN        3
#define FACE_DOWN       4       /* the button itself pressed          */

/*---------------------------------------------------------------------
    LED indices inside the 12-tile digit bitmap
  -------------------------------------------------------------------*/
#define LED_BLANK       10
#define LED_MINUS       11

/*---------------------------------------------------------------------
    fStatus bits
  -------------------------------------------------------------------*/
#define STATUS_PLAY     0x01    /* a game is in progress              */
#define STATUS_PAUSE    0x02    /* suspended                          */
#define STATUS_ICON     0x08    /* minimised                          */
#define STATUS_OVER     0x10    /* the game has finished              */

/*---------------------------------------------------------------------
    3D edge styles used by DrawBorder()
  -------------------------------------------------------------------*/
#define MINE_SUNK       0
#define MINE_RAISE      1
#define MINE_FLAT       2

#define ID_TIMER        1
#define ID_TIMER_AUTO   2       /* the auto-repeat while a button is held */

/*---------------------------------------------------------------------
    Holding the left button keeps clicking.  The first repeat waits a
    moment so an ordinary click - press and release - behaves exactly
    as it always did; past that it fires quickly, so the button can be
    held down and swept across the board.
  -------------------------------------------------------------------*/
#define AUTO_DELAY      300     /* ms before the first repeat          */
#define AUTO_RATE       10      /* ms between repeats after that       */

/*---------------------------------------------------------------------
    persisted settings
  -------------------------------------------------------------------*/
enum {
    INI_DIFFICULTY = 0, INI_MINES, INI_HEIGHT,  INI_WIDTH,
    INI_XPOS,           INI_YPOS,  INI_SOUND,   INI_MARK,
    INI_MENU,           INI_TICK,  INI_COLOR,
    INI_TIME1,          INI_NAME1, INI_TIME2,   INI_NAME2,
    INI_TIME3,          INI_NAME3, INI_ALREADYPLAYED,
    INI_COUNT
};

static const WCHAR *const c_rgszPref[INI_COUNT] = {
    L"Difficulty", L"Mines", L"Height",  L"Width",
    L"Xpos",       L"Ypos",  L"Sound",   L"Mark",
    L"Menu",       L"Tick",  L"Color",
    L"Time1",      L"Name1", L"Time2",   L"Name2",
    L"Time3",      L"Name3", L"AlreadyPlayed"
};

/* Settings and scores live in this file, in the folder the .exe is in.
   Nothing is ever written to the registry. */
static const WCHAR c_szIniName[] = L"winmine.ini";

/* difficulty presets: mines, height, width */
static const int c_rgPreset[3][3] = {
    { 10,  9,  9 },     /* beginner     */
    { 40, 16, 16 },     /* intermediate */
    { 99, 16, 30 }      /* expert       */
};

#define LEVEL_BEGIN     0
#define LEVEL_INTER     1
#define LEVEL_EXPERT    2
#define LEVEL_CUSTOM    3

/*=====================================================================
    globals
  =====================================================================*/
static HINSTANCE    g_hInst;
static HWND         g_hwnd;
static HMENU        g_hMenu;
static WCHAR        g_szIniFile[MAX_PATH + 16];

/* --- persisted preferences --------------------------------------- */
static int          g_wGameType = LEVEL_BEGIN;
static int          g_cMines    = 10;      /* configured mine count   */
static int          g_cRowCfg   = 9;       /* configured height       */
static int          g_cBlkCfg   = 9;       /* configured width        */
static int          g_xWindow   = 80;
static int          g_yWindow   = 80;
static int          g_fSound    = 0;
static int          g_fMark     = 1;
static int          g_fTick     = 0;
static int          g_fMenu     = 0;
static int          g_fColor    = 1;
static int          g_rgTime[3] = { 999, 999, 999 };
static WCHAR        g_rgszName[3][32];
static int          g_fUpdateReg;

/* --- the board, allocated to fit the field ------------------------ */
static BYTE        *g_rgBlk;               /* (cBlk+2) x (cRow+2)     */
static int          g_cStride;             /* bytes per board row     */
static int          g_cBlk = 9;            /* live width              */
static int          g_cRow = 9;            /* live height             */

/* --- game state -------------------------------------------------- */
static DWORD        g_fStatus;
static int          g_cBlkVisit;           /* cells uncovered so far  */
static int          g_cBlkTotal;           /* cells that must be got  */
static int          g_cSec;                /* elapsed seconds         */
static int          g_cBombLeft;           /* the left-hand counter   */
static int          g_iButtonCur = FACE_HAPPY;
static int          g_fTimer;
static int          g_fOldTimer;

/* --- mouse tracking ---------------------------------------------- */
static int          g_fBlockTrack;         /* a button is held down   */
static int          g_fChord;              /* two-button / shift mode */
static int          g_fIgnoreClick;        /* swallow activating click*/
static int          g_fInMenu;
static int          g_xCur = -1;
static int          g_yCur = -1;
static int          g_iXyzzy;

/* --- flood fill queue (a ring, sized to the field - see StepXY) --- */
static int         *g_rgxVisit;
static int         *g_rgyVisit;
static int          g_cVisitMax;
static int          g_iVisitHead;

/* --- zoom, and the 1:1 surface the interface is drawn on ---------- */
static int          g_nZoom = 100;         /* percent                 */
static HDC          g_hdcBack;
static HBITMAP      g_hbmBack;
static int          g_dxBack, g_dyBack;
static int          g_fBackValid;
static int          g_cBatch;              /* >0 while collecting     */
static RECT         g_rcBatch;             /* what is waiting to go   */
static int          g_fBatchRect;          /* g_rcBatch holds something */
static int          g_fBatchField;         /* squares are pending     */
static RECT         g_rcBatchField;        /* which ones, in squares  */

/* --- space-bar panning, the hand tool ----------------------------- */
static int          g_fSpaceDown;
static int          g_fPanning;
static int          g_fPanRight;           /* pan begun with button 2 */
static POINT        g_ptPanGrab;           /* cursor when grabbed     */
static POINT        g_ptPanOrigin;         /* window origin then      */
static HCURSOR      g_hcurArrow;
static HCURSOR      g_hcurPan;

/* --- how many digits the mine counter needs ----------------------- */
static int          g_cLedDigits = 3;

/* --- the ctrl+T corner peek --------------------------------------- */
static int          g_fCtrlDown;           /* tracked, not polled     */
static int          g_fPeek;
static int          g_xPeek = -1;
static int          g_yPeek = -1;
/* --- double-click, to walk to the next covered square ------------- */
static DWORD        g_tmLastClick;
static int          g_xLastClick = -1;
static int          g_yLastClick = -1;

static int          g_fPeekOn;             /* the block is painted    */
static RECT         g_rcPeekOn;            /* and this is where       */

/* --- window metrics ---------------------------------------------- */
static int          g_dxWindow;            /* SM_CXBORDER + 1         */
static int          g_dyCaption;           /* SM_CYCAPTION + 1        */
static int          g_dyMenu;              /* SM_CYMENU + 1           */
static int          g_dyBorder;            /* SM_CYBORDER + 1         */
static int          g_dxClient;            /* client width            */
static int          g_dyClient;            /* client height           */
static int          g_dyAdjust;            /* caption (+ menu) height */
static int          g_fFrozen;             /* suppress MoveWindow     */

/* --- drawing resources ------------------------------------------- */
static HGLOBAL      g_hresBlk, g_hresLed, g_hresFace;
static BITMAPINFO  *g_pbmiBlk, *g_pbmiLed, *g_pbmiFace;
static int          g_rgoffBlk[16];
static int          g_rgoffLed[12];
static int          g_rgoffFace[5];
static HDC          g_rghdcBlk[16];
static HBITMAP      g_rghbmBlk[16];
static HPEN         g_hpenShadow;

/* --- misc strings ------------------------------------------------ */
static WCHAR        g_szClass[32];
static WCHAR        g_szSeconds[32];       /* "%d seconds"            */
static WCHAR        g_szAnonymous[32];

/*---------------------------------------------------------------------
    The shell treats any foreground window that covers the whole
    monitor as a full-screen application and takes the taskbar out of
    always-on-top for it.  A board bigger than the screen crosses that
    line every time it is panned, so the taskbar ends up flickering in
    and out.  ITaskbarList2::MarkFullscreenWindow says "this is not a
    full-screen app" and settles it.

    The interface is reached through its vtable by hand: that costs one
    import from ole32 and no C run-time, which is what the build needs.
  -------------------------------------------------------------------*/
struct MinerTaskbar;

typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(struct MinerTaskbar *,
                                                const GUID *, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(struct MinerTaskbar *);
    ULONG   (STDMETHODCALLTYPE *Release)(struct MinerTaskbar *);
    HRESULT (STDMETHODCALLTYPE *HrInit)(struct MinerTaskbar *);
    HRESULT (STDMETHODCALLTYPE *AddTab)(struct MinerTaskbar *, HWND);
    HRESULT (STDMETHODCALLTYPE *DeleteTab)(struct MinerTaskbar *, HWND);
    HRESULT (STDMETHODCALLTYPE *ActivateTab)(struct MinerTaskbar *, HWND);
    HRESULT (STDMETHODCALLTYPE *SetActiveAlt)(struct MinerTaskbar *, HWND);
    HRESULT (STDMETHODCALLTYPE *MarkFullscreenWindow)(struct MinerTaskbar *,
                                                      HWND, BOOL);
} MinerTaskbarVtbl;

struct MinerTaskbar { MinerTaskbarVtbl *lpVtbl; };

static struct MinerTaskbar *g_ptbl;
static int                  g_fTaskbarTried;
static DWORD                g_msTaskbar;

/* --- HtmlHelp, resolved lazily on first use ----------------------- */
static HMODULE      g_hmodHelp;
static int          g_fHelpFailed;
static FARPROC      g_pfnHtmlHelp;

/*---------------------------------------------------------------------
    forward declarations
  -------------------------------------------------------------------*/
static void TrackMouse(int x, int y);
static void DoEnterName(void);
static void DoBestTimes(void);
static void ReportErr(UINT id);

/*---------------------------------------------------------------------
    Built without the C run-time (see build.cmd), the one library
    routine the compiler still wants to emit is memset, for the loop
    that blanks the board.  The volatile pointer is what stops the
    optimiser turning this loop back into a call to itself.
  -------------------------------------------------------------------*/
#ifdef MINER_NO_CRT
#ifdef _MSC_VER
#pragma function(memset)
#endif
extern "C" void *memset(void *pv, int c, size_t cb)
{
    volatile unsigned char *pb = (volatile unsigned char *)pv;
    while (cb--)
        *pb++ = (unsigned char)c;
    return pv;
}
#endif

/*---------------------------------------------------------------------
    The Microsoft C run-time's generator, written out longhand: this is
    bit-for-bit what msvcrt.dll's rand() does.  Having it here means the
    program needs no C run-time at all, which keeps the binary small.
  -------------------------------------------------------------------*/
static unsigned int g_randSeed = 1;

static void MinerSrand(unsigned int seed)
{
    g_randSeed = seed;
}

static int MinerRand(void)
{
    g_randSeed = g_randSeed * 214013u + 2531011u;
    return (int)((g_randSeed >> 16) & 0x7FFF);
}

/*=====================================================================
    small helpers
  =====================================================================*/
static inline BYTE *PblkAt(int x, int y)
{
    return &g_rgBlk[x + y * g_cStride];
}

/* decimal width of a non-negative number */
static int CDigits(int n)
{
    int c = 1;
    while (n >= 10) { n /= 10; c++; }
    return c;
}

static int ClampInt(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void LoadSz(UINT id, LPWSTR psz, int cch)
{
    if (LoadStringW(g_hInst, id, psz, cch) == 0)
        ReportErr(1001);
}

static void ReportErr(UINT id)
{
    WCHAR szText[256];
    WCHAR szTitle[256];

    if (id < 999) {
        LoadStringW(g_hInst, id, szText, 256);
    } else {
        LoadStringW(g_hInst, IDS_ERR_UNKNOWN, szTitle, 256);
        wsprintfW(szText, szTitle, id);
    }
    LoadStringW(g_hInst, IDS_ERR_TITLE, szTitle, 256);
    MessageBoxW(NULL, szText, szTitle, MB_ICONHAND);
}

/* the usable screen size, preferring the work area */
static int DxpScreen(int fVertical)
{
    int d = GetSystemMetrics(fVertical ? SM_CYFULLSCREEN : SM_CXFULLSCREEN);
    if (d != 0)
        return d;
    return GetSystemMetrics(fVertical ? SM_CYSCREEN : SM_CXSCREEN);
}

/*=====================================================================
    sound
  =====================================================================*/
enum { SOUND_TICK = 1, SOUND_WIN = 2, SOUND_LOSE = 3 };

/* 3 == the wave device answered, 2 == wanted but unavailable */
static int FTestSound(void)
{
    return PlaySoundW(NULL, NULL, SND_PURGE) ? 3 : 2;
}

static void KillSound(void)
{
    if (g_fSound == 3)
        PlaySoundW(NULL, NULL, SND_PURGE);
}

static void PlayTune(int iSound)
{
    UINT id;

    if (g_fSound != 3)
        return;
    switch (iSound) {
    case SOUND_TICK: id = ID_WAV_TICK; break;
    case SOUND_WIN:  id = ID_WAV_WIN;  break;
    case SOUND_LOSE: id = ID_WAV_LOSE; break;
    default: return;
    }
    PlaySoundW((LPCWSTR)(ULONG_PTR)id, g_hInst, SND_RESOURCE | SND_ASYNC);
}

/*=====================================================================
    preferences - winmine.ini, in the folder the .exe lives in
  =====================================================================*/

/* build the full path of winmine.ini beside the executable */
static void InitIniPath(void)
{
    DWORD cch = GetModuleFileNameW(NULL, g_szIniFile, MAX_PATH);

    if (cch == 0 || cch >= MAX_PATH) {
        lstrcpyW(g_szIniFile, c_szIniName);      /* fall back to the cwd */
        return;
    }
    while (cch > 0 && g_szIniFile[cch - 1] != L'\\' &&
                      g_szIniFile[cch - 1] != L'/')
        cch--;
    lstrcpyW(g_szIniFile + cch, c_szIniName);
}

/* WritePrivateProfileStringW only stores UTF-16 if the file already is
   UTF-16, so create it with a byte-order mark the first time.  Without
   this a player whose name is not plain ASCII would get it mangled. */
static void EnsureIniFile(void)
{
    static const BYTE bom[2] = { 0xFF, 0xFE };
    HANDLE hf;
    DWORD  cb;

    if (GetFileAttributesW(g_szIniFile) != INVALID_FILE_ATTRIBUTES)
        return;

    hf = CreateFileW(g_szIniFile, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                     FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE)
        return;
    WriteFile(hf, bom, sizeof(bom), &cb, NULL);
    CloseHandle(hf);
}

static int ReadIniInt(int iPref, int dflt, int lo, int hi)
{
    int v = (int)GetPrivateProfileIntW(g_szClass, c_rgszPref[iPref],
                                       (INT)dflt, g_szIniFile);
    return ClampInt(v, lo, hi);
}

static void ReadIniSz(int iPref, LPWSTR psz)
{
    GetPrivateProfileStringW(g_szClass, c_rgszPref[iPref], g_szAnonymous,
                             psz, 32, g_szIniFile);
}

static void WriteIniInt(int iPref, int v)
{
    WCHAR sz[16];
    wsprintfW(sz, L"%d", v);
    WritePrivateProfileStringW(g_szClass, c_rgszPref[iPref], sz, g_szIniFile);
}

static void WriteIniSz(int iPref, LPCWSTR psz)
{
    WritePrivateProfileStringW(g_szClass, c_rgszPref[iPref], psz, g_szIniFile);
}

static void WritePreferences(void)
{
    EnsureIniFile();

    WriteIniInt(INI_DIFFICULTY,     g_wGameType);
    WriteIniInt(INI_HEIGHT,         g_cRowCfg);
    WriteIniInt(INI_WIDTH,          g_cBlkCfg);
    WriteIniInt(INI_MINES,          g_cMines);
    WriteIniInt(INI_MARK,           g_fMark);
    WriteIniInt(INI_ALREADYPLAYED,  1);
    WriteIniInt(INI_COLOR,          g_fColor);
    WriteIniInt(INI_SOUND,          g_fSound);
    WriteIniInt(INI_XPOS,           g_xWindow);
    WriteIniInt(INI_YPOS,           g_yWindow);
    WriteIniInt(INI_TIME1,          g_rgTime[0]);
    WriteIniInt(INI_TIME2,          g_rgTime[1]);
    WriteIniInt(INI_TIME3,          g_rgTime[2]);
    WriteIniSz (INI_NAME1,          g_rgszName[0]);
    WriteIniSz (INI_NAME2,          g_rgszName[1]);
    WriteIniSz (INI_NAME3,          g_rgszName[2]);

    /* flush the cache Windows keeps for .ini files */
    WritePrivateProfileStringW(NULL, NULL, NULL, g_szIniFile);
}

static int FIsColourScreen(void)
{
    HWND hwndDesk = GetDesktopWindow();
    HDC  hdc      = GetDC(hwndDesk);
    int  bpp      = GetDeviceCaps(hdc, BITSPIXEL);
    ReleaseDC(hwndDesk, hdc);
    return bpp != 2;
}

static void ReadPreferences(void)
{
    g_cRow = g_cRowCfg = ReadIniInt(INI_HEIGHT, 9, CROW_MIN, 0x7FFFFFF);
    g_cBlk = g_cBlkCfg = ReadIniInt(INI_WIDTH,  9, CBLK_MIN, 0x7FFFFFF);
    g_wGameType        = ReadIniInt(INI_DIFFICULTY, 0,  0, 3);
    g_cMines           = ReadIniInt(INI_MINES, 10, CMINE_MIN, 0x7FFFFFF);
    g_xWindow          = ReadIniInt(INI_XPOS,      80,  0, 1024);
    g_yWindow          = ReadIniInt(INI_YPOS,      80,  0, 1024);
    g_fSound           = ReadIniInt(INI_SOUND,      0,  0, 3);
    g_fMark            = ReadIniInt(INI_MARK,       1,  0, 1);
    g_fTick            = ReadIniInt(INI_TICK,       0,  0, 1);
    g_fMenu            = ReadIniInt(INI_MENU,       0,  0, 2);
    g_rgTime[0]        = ReadIniInt(INI_TIME1,    999,  0, 999);
    g_rgTime[1]        = ReadIniInt(INI_TIME2,    999,  0, 999);
    g_rgTime[2]        = ReadIniInt(INI_TIME3,    999,  0, 999);
    ReadIniSz(INI_NAME1, g_rgszName[0]);
    ReadIniSz(INI_NAME2, g_rgszName[1]);
    ReadIniSz(INI_NAME3, g_rgszName[2]);
    g_fColor           = ReadIniInt(INI_COLOR, FIsColourScreen(), 0, 1);

    if (g_fSound == 3)
        g_fSound = FTestSound();
}

/* First run: write winmine.ini so everything read back later is there */
static void InitPreferences(void)
{
    int fAlreadyPlayed;

    MinerSrand(GetTickCount() & 0xFFFF);

    InitIniPath();
    LoadSz(IDS_NAME,      g_szClass,     32);
    LoadSz(IDS_SECONDS,   g_szSeconds,   32);
    LoadSz(IDS_ANONYMOUS, g_szAnonymous, 32);

    g_dyCaption = GetSystemMetrics(SM_CYCAPTION) + 1;
    g_dyMenu    = GetSystemMetrics(SM_CYMENU)    + 1;
    g_dyBorder  = GetSystemMetrics(SM_CYBORDER)  + 1;
    g_dxWindow  = GetSystemMetrics(SM_CXBORDER)  + 1;

    fAlreadyPlayed = ReadIniInt(INI_ALREADYPLAYED, 0, 0, 1);
    if (fAlreadyPlayed)
        return;

    g_cRowCfg   = 9;
    g_cBlkCfg   = 9;
    g_wGameType = LEVEL_BEGIN;
    g_cMines    = 10;
    g_xWindow   = 80;
    g_yWindow   = 80;
    g_fSound    = 0;
    g_fMark     = 1;
    g_fTick     = 0;
    g_fMenu     = 0;
    g_rgTime[0] = g_rgTime[1] = g_rgTime[2] = 999;
    lstrcpyW(g_rgszName[0], g_szAnonymous);
    lstrcpyW(g_rgszName[1], g_szAnonymous);
    lstrcpyW(g_rgszName[2], g_szAnonymous);
    g_fColor    = FIsColourScreen();

    WritePreferences();
}

/*=====================================================================
    bitmaps
  =====================================================================*/
/* the monochrome variant always follows the colour one */
static HRSRC FindBmp(int id)
{
    return FindResourceW(g_hInst,
                         MAKEINTRESOURCEW(id + (g_fColor == 0)),
                         (LPCWSTR)RT_BITMAP);
}

/* size in bytes of one tile of the given dimensions */
static int CbTile(int dx, int dy)
{
    int cBits = g_fColor ? 4 : 1;
    return ((((cBits * dx) + 31) >> 3) & ~3) * dy;
}

/* offset from the start of the BITMAPINFO to the first pixel */
static int CbHeader(void)
{
    return (int)sizeof(BITMAPINFOHEADER) +
           (g_fColor ? 16 : 2) * (int)sizeof(RGBQUAD);
}

static void FreeBmp(void)
{
    int i;

    if (g_hpenShadow) {
        DeleteObject(g_hpenShadow);
        g_hpenShadow = NULL;
    }
    for (i = 0; i < 16; i++) {
        if (g_rghdcBlk[i]) { DeleteDC(g_rghdcBlk[i]);     g_rghdcBlk[i] = NULL; }
        if (g_rghbmBlk[i]) { DeleteObject(g_rghbmBlk[i]); g_rghbmBlk[i] = NULL; }
    }
}

static BOOL FLoadBmp(void)
{
    HRSRC hrsrc;
    HDC   hdc;
    int   cb, i;

    g_hresBlk = g_hresLed = g_hresFace = NULL;

    if ((hrsrc = FindBmp(ID_BMP_BLOCKS)) != NULL)
        g_hresBlk = LoadResource(g_hInst, hrsrc);
    if ((hrsrc = FindBmp(ID_BMP_LED)) != NULL)
        g_hresLed = LoadResource(g_hInst, hrsrc);
    if ((hrsrc = FindBmp(ID_BMP_FACE)) != NULL)
        g_hresFace = LoadResource(g_hInst, hrsrc);

    if (!g_hresBlk || !g_hresLed || !g_hresFace)
        return FALSE;

    g_pbmiBlk  = (BITMAPINFO *)LockResource(g_hresBlk);
    g_pbmiLed  = (BITMAPINFO *)LockResource(g_hresLed);
    g_pbmiFace = (BITMAPINFO *)LockResource(g_hresFace);
    if (!g_pbmiBlk || !g_pbmiLed || !g_pbmiFace)
        return FALSE;

    g_hpenShadow = CreatePen(PS_SOLID, 1,
                             g_fColor ? RGB(0x80, 0x80, 0x80) : RGB(0, 0, 0));

    cb = CbTile(DX_BLK, DY_BLK);
    for (i = 0; i < 16; i++)
        g_rgoffBlk[i] = CbHeader() + i * cb;

    cb = CbTile(DX_LED, DY_LED);
    for (i = 0; i < 12; i++)
        g_rgoffLed[i] = CbHeader() + i * cb;

    cb = CbTile(DX_BUTTON, DY_BUTTON);
    for (i = 0; i < 5; i++)
        g_rgoffFace[i] = CbHeader() + i * cb;

    /* pre-render the sixteen field tiles into memory DCs */
    hdc = GetDC(g_hwnd);
    for (i = 0; i < 16; i++) {
        g_rghdcBlk[i] = CreateCompatibleDC(hdc);
        g_rghbmBlk[i] = CreateCompatibleBitmap(hdc, DX_BLK, DY_BLK);
        if (!g_rghdcBlk[i] || !g_rghbmBlk[i]) {
            ReleaseDC(g_hwnd, hdc);
            return FALSE;
        }
        SelectObject(g_rghdcBlk[i], g_rghbmBlk[i]);
        SetDIBitsToDevice(g_rghdcBlk[i], 0, 0, DX_BLK, DY_BLK, 0, 0,
                          0, DY_BLK,
                          (const BYTE *)g_pbmiBlk + g_rgoffBlk[i],
                          g_pbmiBlk, DIB_RGB_COLORS);
    }
    ReleaseDC(g_hwnd, hdc);
    return TRUE;
}

/*=====================================================================
    zoom, and the offscreen surface

    The interface is drawn 1:1 onto g_hdcBack and then stretched onto
    the window.  Doing it that way means the zoom scales *everything*
    by the same factor - the pixel artwork and the one-pixel 3D edges
    alike - and the drawing code below stays in plain 1:1 coordinates.
  =====================================================================*/
static int LogToDev(int v) { return MulDiv(v, g_nZoom, 100); }
static int DevToLog(int v) { return MulDiv(v, 100, g_nZoom); }

/* Tell the shell this window is not a full-screen application, so it
   leaves the taskbar on top however far the board spills off screen. */
static void ClearFullscreenClaim(void)
{
    static const GUID clsidTaskbarList =
        { 0x56FDF344, 0xFD6D, 0x11D0,
          { 0x95, 0x8A, 0x00, 0x60, 0x97, 0xC9, 0xA0, 0x90 } };
    static const GUID iidTaskbarList2 =
        { 0x602D4995, 0xB13A, 0x429B,
          { 0xA6, 0x6E, 0x19, 0x35, 0xE4, 0x4F, 0x43, 0x17 } };

    if (g_hwnd == NULL)
        return;

    if (g_ptbl == NULL) {
        if (g_fTaskbarTried)
            return;
        g_fTaskbarTried = 1;
        CoInitialize(NULL);
        if (CoCreateInstance(clsidTaskbarList, NULL, CLSCTX_INPROC_SERVER,
                             iidTaskbarList2, (void **)&g_ptbl) != S_OK ||
            g_ptbl == NULL) {
            g_ptbl = NULL;
            return;
        }
        g_ptbl->lpVtbl->HrInit(g_ptbl);
    }
    g_ptbl->lpVtbl->MarkFullscreenWindow(g_ptbl, g_hwnd, FALSE);
}

/* Would a window this big actually be drawn?

   The desktop compositor gives every window an offscreen surface of
   its own, and it will only go so large.  Past the limit nothing
   fails in a way a program can see: the window is created, it is on
   the taskbar, it answers messages and reports the size that was
   asked for - it is simply never drawn, so the game runs on, entirely
   correct and entirely invisible.  The limit belongs to the graphics
   stack and there is nowhere to ask for it, so this keeps well inside
   where it was measured to give out (about two gigabytes' worth). */
static BOOL FWindowFitsSurface(int nZoom)
{
    int cx = MulDiv(g_dxClient, nZoom, 100);
    int cy = MulDiv(g_dyClient, nZoom, 100);

    if (cx <= 0 || cy <= 0)
        return TRUE;
    return cy <= CPX_WINDOW_MAX / cx;      /* divided, so never overflows */
}

/* There is no zoom ceiling of principle - a board larger than the
   screen is what the space-bar pan is for.  Two practical ones do
   apply: an edge the window manager will still place, and a surface
   the compositor will still draw.  The second binds only on fields so
   large that the window runs to hundreds of megapixels; on any
   ordinary board the ceiling stays in the thousands of percent. */
static int ZoomMax(void)
{
    int zx, zy, lo, hi;

    if (g_dxClient <= 0 || g_dyClient <= 0)
        return ZOOM_MIN;
    zx = MulDiv(DXY_ZOOM_MAX, 100, g_dxClient);
    zy = MulDiv(DXY_ZOOM_MAX, 100, g_dyClient);
    if (zy < zx)
        zx = zy;
    if (zx < ZOOM_MIN)
        return ZOOM_MIN;
    if (FWindowFitsSurface(zx))
        return zx;

    /* the largest zoom whose window the compositor will still draw */
    lo = ZOOM_MIN;
    hi = zx;
    while (lo < hi) {
        int mid = lo + (hi - lo + 1) / 2;

        if (FWindowFitsSurface(mid))
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

/* The zoom the game opens at.  The program asks Windows for real
   pixels (see the manifest), so on a display running at 200% the
   artwork would otherwise come up half the size of everything around
   it.  Starting at the display's own scale puts it back at the size
   the rest of the desktop is drawn at - and because the scaling is
   whole-pixel nearest neighbour, it stays sharp instead of being
   stretched and blurred the way an unaware program would be. */
static int ZoomForDisplay(void)
{
    HWND hwndDesk = GetDesktopWindow();
    HDC  hdc      = GetDC(hwndDesk);
    int  dpi      = hdc ? GetDeviceCaps(hdc, LOGPIXELSX) : 96;

    if (hdc)
        ReleaseDC(hwndDesk, hdc);
    if (dpi < 96)
        dpi = 96;
    return MulDiv(dpi, 100, 96);
}

static void FreeBack(void)
{
    if (g_hdcBack) { DeleteDC(g_hdcBack);     g_hdcBack = NULL; }
    if (g_hbmBack) { DeleteObject(g_hbmBack); g_hbmBack = NULL; }
    g_dxBack = g_dyBack = 0;
    g_fBackValid = 0;
}

static BOOL FEnsureBack(void)
{
    HDC hdc;

    if (g_hdcBack && g_dxBack == g_dxClient && g_dyBack == g_dyClient)
        return TRUE;

    FreeBack();
    if (g_dxClient <= 0 || g_dyClient <= 0 || g_hwnd == NULL)
        return FALSE;

    hdc = GetDC(g_hwnd);
    if (hdc == NULL)
        return FALSE;
    g_hdcBack = CreateCompatibleDC(hdc);
    g_hbmBack = CreateCompatibleBitmap(hdc, g_dxClient, g_dyClient);
    ReleaseDC(g_hwnd, hdc);

    if (!g_hdcBack || !g_hbmBack) {
        FreeBack();
        return FALSE;
    }
    SelectObject(g_hdcBack, g_hbmBack);
    g_dxBack = g_dxClient;
    g_dyBack = g_dyClient;
    return TRUE;
}

/*---------------------------------------------------------------------
    Batching the copies to the window.

    Uncovering an empty area repaints every square it reaches, one at
    a time, and each one used to be its own GetDC / BitBlt /
    ReleaseDC.  On a large field a single click can open tens of
    thousands of squares, which made the trip to the window, not the
    drawing, the expensive part.  Between BeginBatch and EndBatch the
    squares are still drawn onto the surface as they go, but the
    copies are collected into one rectangle and sent over once.
  -------------------------------------------------------------------*/
static void DrawFieldRange(HDC hdc, int xFirst, int yFirst, int xLast,
                           int yLast);

static void BeginBatch(void)
{
    if (g_cBatch++ == 0) {
        g_fBatchRect  = 0;
        g_fBatchField = 0;
    }
}

/* Draw the squares a batch has collected.  Anything that shows the
   window mid-batch has to call this first, or it would show a surface
   the squares have not reached yet. */
static void FlushBatchField(void)
{
    int xPix, yPix, cx, cy;

    if (!g_fBatchField)
        return;
    g_fBatchField = 0;
    if (g_hdcBack == NULL)
        return;

    DrawFieldRange(g_hdcBack, g_rcBatchField.left, g_rcBatchField.top,
                   g_rcBatchField.right, g_rcBatchField.bottom);

    /* and note the ground they cover, so the copy to the window
       takes them in */
    xPix = g_rcBatchField.left * DX_BLK - 4;
    yPix = g_rcBatchField.top  * DY_BLK + 39;
    cx   = (g_rcBatchField.right  - g_rcBatchField.left + 1) * DX_BLK;
    cy   = (g_rcBatchField.bottom - g_rcBatchField.top  + 1) * DY_BLK;
    if (!g_fBatchRect) {
        g_fBatchRect = 1;
        g_rcBatch.left = xPix;      g_rcBatch.right  = xPix + cx;
        g_rcBatch.top  = yPix;      g_rcBatch.bottom = yPix + cy;
    } else {
        if (xPix < g_rcBatch.left)        g_rcBatch.left   = xPix;
        if (yPix < g_rcBatch.top)         g_rcBatch.top    = yPix;
        if (xPix + cx > g_rcBatch.right)  g_rcBatch.right  = xPix + cx;
        if (yPix + cy > g_rcBatch.bottom) g_rcBatch.bottom = yPix + cy;
    }
}

/* copy one 1:1 rectangle of the surface onto the window, scaled */
static void Present(int x, int y, int cx, int cy)
{
    HDC hdc;

    if (!g_hdcBack || g_hwnd == NULL)
        return;

    if (g_cBatch > 0) {
        /* plain comparisons, not UnionRect: this is on the path that
           runs once per square turned over */
        if (!g_fBatchRect) {
            g_fBatchRect = 1;
            g_rcBatch.left = x;  g_rcBatch.right  = x + cx;
            g_rcBatch.top  = y;  g_rcBatch.bottom = y + cy;
        } else {
            if (x < g_rcBatch.left)            g_rcBatch.left   = x;
            if (y < g_rcBatch.top)             g_rcBatch.top    = y;
            if (x + cx > g_rcBatch.right)      g_rcBatch.right  = x + cx;
            if (y + cy > g_rcBatch.bottom)     g_rcBatch.bottom = y + cy;
        }
        return;
    }

    hdc = GetDC(g_hwnd);
    if (hdc == NULL)
        return;

    if (g_nZoom == 100) {
        BitBlt(hdc, x, y, cx, cy, g_hdcBack, x, y, SRCCOPY);
    } else {
        int xd  = LogToDev(x);
        int yd  = LogToDev(y);
        int cxd = LogToDev(x + cx) - xd;
        int cyd = LogToDev(y + cy) - yd;

        /* Nearest neighbour, both ways.  HALFTONE would dither these
           flat greys into a checkerboard, which is not what the
           artwork should ever look like. */
        SetStretchBltMode(hdc, COLORONCOLOR);
        StretchBlt(hdc, xd, yd, cxd, cyd, g_hdcBack, x, y, cx, cy, SRCCOPY);
    }
    ReleaseDC(g_hwnd, hdc);
}

static void EndBatch(void)
{
    RECT rc;

    if (g_cBatch <= 0 || --g_cBatch > 0)
        return;
    FlushBatchField();
    if (!g_fBatchRect)
        return;
    rc = g_rcBatch;
    g_fBatchRect = 0;
    Present(rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);
}

/*=====================================================================
    drawing
  =====================================================================*/
static void DisplayBlk(int x, int y)
{
    int xPix = x * DX_BLK - 4;
    int yPix = y * DY_BLK + 39;

    if (!g_hdcBack)
        return;

    /* Inside a batch the square is only noted, not drawn.  Uncovering
       an empty area turns over every square it reaches, and a square
       at a time costs one drawing call each - on a large field a
       single click could spend seconds on it.  The block they cover
       is redrawn in one pass at the end of the batch instead, where
       the run-doubling in DrawFieldRow gets it for a fraction of the
       calls: an opened area is mostly one blank tile repeated. */
    if (g_cBatch > 0) {
        if (!g_fBatchField) {
            g_fBatchField = 1;
            g_rcBatchField.left = g_rcBatchField.right  = x;
            g_rcBatchField.top  = g_rcBatchField.bottom = y;
        } else {
            if (x < g_rcBatchField.left)   g_rcBatchField.left   = x;
            if (x > g_rcBatchField.right)  g_rcBatchField.right  = x;
            if (y < g_rcBatchField.top)    g_rcBatchField.top    = y;
            if (y > g_rcBatchField.bottom) g_rcBatchField.bottom = y;
        }
        return;      /* the block of squares is copied over at the end */
    }

    BitBlt(g_hdcBack, xPix, yPix, DX_BLK, DY_BLK,
           g_rghdcBlk[*PblkAt(x, y) & MASK_ICON], 0, 0, SRCCOPY);
    Present(xPix, yPix, DX_BLK, DY_BLK);
}

/* One row of the field.

   Drawing a square at a time costs one GDI call per square, and a
   call is about four microseconds however small the square is - so a
   999 x 999 field took four seconds to lay down, every time it had to
   be composed afresh.  A field is mostly flat, though, and a new one
   is entirely one tile, so this walks runs of the same tile and draws
   each run once, then doubles it along itself: a run of n squares
   costs log2(n) calls rather than n.  A fresh row of 999 is ten. */
static void DrawFieldRow(HDC hdc, int y, int yPix, int xFirst, int xLast)
{
    int x = xFirst;

    while (x <= xLast) {
        BYTE bTile = (BYTE)(*PblkAt(x, y) & MASK_ICON);
        int  xEnd  = x + 1;
        int  xPix  = X_FIELD + (x - 1) * DX_BLK;
        int  cRun, cDone;

        while (xEnd <= xLast &&
               (BYTE)(*PblkAt(xEnd, y) & MASK_ICON) == bTile)
            xEnd++;
        cRun = xEnd - x;

        BitBlt(hdc, xPix, yPix, DX_BLK, DY_BLK,
               g_rghdcBlk[bTile], 0, 0, SRCCOPY);
        /* copy what is down onto what is not, doubling each time; the
           source and destination never overlap, so one plain BitBlt
           within the surface does it */
        for (cDone = 1; cDone < cRun; cDone += cDone) {
            int cCopy = (cRun - cDone < cDone) ? cRun - cDone : cDone;

            BitBlt(hdc, xPix + cDone * DX_BLK, yPix,
                   cCopy * DX_BLK, DY_BLK, hdc, xPix, yPix, SRCCOPY);
        }
        x = xEnd;
    }
}

/* redraw a block of squares, by rows */
static void DrawFieldRange(HDC hdc, int xFirst, int yFirst, int xLast,
                           int yLast)
{
    int y;

    if (xFirst < 1) xFirst = 1;
    if (yFirst < 1) yFirst = 1;
    if (xLast > g_cBlk) xLast = g_cBlk;
    if (yLast > g_cRow) yLast = g_cRow;

    for (y = yFirst; y <= yLast; y++)
        DrawFieldRow(hdc, y, Y_FIELD + (y - 1) * DY_BLK, xFirst, xLast);
}

static void DrawField(HDC hdc)
{
    DrawFieldRange(hdc, 1, 1, g_cBlk, g_cRow);
}

static void DisplayField(void)
{
    if (!g_hdcBack)
        return;
    DrawField(g_hdcBack);
    Present(X_FIELD, Y_FIELD, g_cBlk * DX_BLK, g_cRow * DY_BLK);
}

static void DrawLed(HDC hdc, int x, int iLed)
{
    SetDIBitsToDevice(hdc, x, Y_LED, DX_LED, DY_LED, 0, 0, 0, DY_LED,
                      (const BYTE *)g_pbmiLed + g_rgoffLed[iLed],
                      g_pbmiLed, DIB_RGB_COLORS);
}

/* Three digits, as ever - but a field can now hold more than 999
   mines, so the readout grows a digit at a time to fit the count.  A
   minus sign costs one of them, which is what the classic display did
   too (ten flags too many reads "-10"). */
static void DrawBombCount(HDC hdc)
{
    DWORD dwLayout = GetLayout(hdc);
    int   rgLed[16];
    int   cDigits  = g_cLedDigits;
    int   n        = g_cBombLeft;
    int   fNeg     = 0;
    int   i, nMax;

    if (dwLayout & LAYOUT_RTL)
        SetLayout(hdc, 0);

    if (n < 0) { fNeg = 1; n = -n; }

    nMax = 1;
    for (i = fNeg ? 1 : 0; i < cDigits; i++)
        nMax *= 10;
    if (n >= nMax)
        n = nMax - 1;

    for (i = cDigits - 1; i >= 0; i--) {
        rgLed[i] = n % 10;
        n /= 10;
    }
    if (fNeg)
        rgLed[0] = LED_MINUS;

    for (i = 0; i < cDigits; i++)
        DrawLed(hdc, 17 + i * DX_LED, rgLed[i]);

    if (dwLayout & LAYOUT_RTL)
        SetLayout(hdc, dwLayout);
}

static void DisplayBombCount(void)
{
    if (!g_hdcBack)
        return;
    DrawBombCount(g_hdcBack);
    Present(17, Y_LED, g_cLedDigits * DX_LED, DY_LED);
}

static void DrawTime(HDC hdc)
{
    DWORD dwLayout = GetLayout(hdc);
    int   x        = g_dxClient - g_dxWindow;
    int   n        = g_cSec;

    if (dwLayout & LAYOUT_RTL)
        SetLayout(hdc, 0);

    DrawLed(hdc, x - 0x38, n / 100);
    DrawLed(hdc, x - 0x2B, (n % 100) / 10);
    DrawLed(hdc, x - 0x1E, (n % 100) % 10);

    if (dwLayout & LAYOUT_RTL)
        SetLayout(hdc, dwLayout);
}

static void DisplayTime(void)
{
    if (!g_hdcBack)
        return;
    DrawTime(g_hdcBack);
    Present(g_dxClient - g_dxWindow - 0x38, Y_LED, 3 * DX_LED, DY_LED);
}

static void DrawButton(HDC hdc, int iFace)
{
    SetDIBitsToDevice(hdc, (g_dxClient - DX_BUTTON) >> 1, Y_BUTTON,
                      DX_BUTTON, DY_BUTTON, 0, 0, 0, DY_BUTTON,
                      (const BYTE *)g_pbmiFace + g_rgoffFace[iFace],
                      g_pbmiFace, DIB_RGB_COLORS);
}

static void DisplayButton(int iFace)
{
    if (!g_hdcBack)
        return;
    DrawButton(g_hdcBack, iFace);
    Present((g_dxClient - DX_BUTTON) >> 1, Y_BUTTON, DX_BUTTON, DY_BUTTON);
}

/*---------------------------------------------------------------------
    The 3D edges.  Highlights are drawn with R2_WHITE (so the pen
    colour does not matter); shadows use a grey pen with R2_COPYPEN.
    LineTo() stops one pixel short of its end point, which is why the
    corners are stepped the way they are.
  -------------------------------------------------------------------*/
static void SetEdgePen(HDC hdc, int fHighlight)
{
    if (fHighlight & 1) {
        SetROP2(hdc, R2_WHITE);
    } else {
        SetROP2(hdc, R2_COPYPEN);
        SelectObject(hdc, g_hpenShadow);
    }
}

static void DrawBorder(HDC hdc, int x1, int y1, int x2, int y2,
                       int cThick, int iStyle)
{
    int c = 0;

    SetEdgePen(hdc, iStyle);

    if (cThick > 0) {
        int n = cThick;
        c = cThick;
        do {
            y2--;
            MoveToEx(hdc, x1, y2, NULL);
            LineTo(hdc, x1, y1);
            x1++;
            LineTo(hdc, x2, y1);
            x2--;
            y1++;
        } while (--n != 0);
    }

    if (iStyle < MINE_FLAT)
        SetEdgePen(hdc, iStyle ^ 1);

    for (; c != 0; c--) {
        y2++;
        MoveToEx(hdc, x1, y2, NULL);
        x1--;
        x2++;
        LineTo(hdc, x2, y2);
        y1--;
        LineTo(hdc, x2, y1);
    }
}

static void DrawBackground(HDC hdc)
{
    int dx = g_dxClient;
    int dy = g_dyClient;
    int xFace;

    /* outer frame */
    DrawBorder(hdc, 0, 0, dx - 1, dy - 1, 3, MINE_RAISE);
    /* the mine field */
    DrawBorder(hdc, 9, 0x34, dx - 10, dy - 10, 3, MINE_SUNK);
    /* the status panel */
    DrawBorder(hdc, 9, 9, dx - 10, 0x2D, 2, MINE_SUNK);
    /* the two LED displays; the left one grows with the mine count */
    DrawBorder(hdc, 0x10, 0x0F, 0x11 + g_cLedDigits * DX_LED, 0x27,
               1, MINE_SUNK);
    DrawBorder(hdc, (dx - g_dxWindow) - 0x39, 0x0F,
                    (dx - g_dxWindow) - 0x11, 0x27, 1, MINE_SUNK);
    /* the smiley */
    xFace = (dx - DX_BUTTON) >> 1;
    DrawBorder(hdc, xFace - 1, 0x0F, xFace + DX_BUTTON, 0x28, 1, MINE_FLAT);
}

static void DrawScreen(HDC hdc)
{
    RECT rc;

    /* Lay the face colour down first.  Drawing straight to the window
       used to get this for free from the class background brush; now
       that the interface is composed offscreen, the surface starts out
       undefined and has to be cleared here or every gap between the
       borders, digits and cells comes out black. */
    SetRect(&rc, 0, 0, g_dxClient, g_dyClient);
    FillRect(hdc, &rc, (HBRUSH)GetStockObject(LTGRAY_BRUSH));

    DrawBackground(hdc);
    DrawBombCount(hdc);
    DrawButton(hdc, g_iButtonCur);
    DrawTime(hdc);
    DrawField(hdc);
}

static void DisplayScreen(void)
{
    if (!FEnsureBack())
        return;
    DrawScreen(g_hdcBack);
    g_fBackValid = 1;
    Present(0, 0, g_dxClient, g_dyClient);
}

/*=====================================================================
    window sizing
  =====================================================================*/
#define ADJUST_SHOW     1
#define ADJUST_MOVE     2
#define ADJUST_PAINT    4
/* Repaint, but the interface itself has not changed - only the size
   it is being shown at.  The offscreen surface is drawn entirely in
   1:1 coordinates and the zoom is applied on the way out of it, so a
   zoom does not touch its contents and there is nothing to redraw:
   throwing it away and composing it again cost a quarter of a second
   a notch on a million-square field, for a picture identical to the
   one already in hand. */
#define ADJUST_RESCALE  8

/* TRUE when the menu bar has wrapped onto a second line */
static BOOL FMenuWrapped(void)
{
    RECT r0, r1;

    if (g_hMenu == NULL || GetMenu(g_hwnd) == NULL)
        return FALSE;
    if (!GetMenuItemRect(g_hwnd, g_hMenu, 0, &r0)) return FALSE;
    if (!GetMenuItemRect(g_hwnd, g_hMenu, 1, &r1)) return FALSE;
    return r0.top != r1.top;
}

/* place the frame so that the client area lands exactly on
   (g_xWindow, g_yWindow) and measures g_dxClient x g_dyClient */
/* fRepaint is for callers that are not going to invalidate the window
   themselves; when one is about to, letting MoveWindow repaint as
   well means painting the whole thing twice */
static void MoveToClient(BOOL fRepaint)
{
    RECT  rc;
    DWORD dwStyle = (DWORD)GetWindowLongPtrW(g_hwnd, GWL_STYLE);

    SetRect(&rc, 0, 0, LogToDev(g_dxClient), LogToDev(g_dyClient));
    AdjustWindowRect(&rc, dwStyle, GetMenu(g_hwnd) != NULL);
    MoveWindow(g_hwnd, g_xWindow + rc.left, g_yWindow + rc.top,
               rc.right - rc.left, rc.bottom - rc.top, fRepaint);
}

static void AdjustWindow(UINT wFlags)
{
    BOOL fWrapCheck = FALSE;
    BOOL fOwnPaint;
    int  d;

    if (g_hwnd == NULL)
        return;

    g_dyAdjust = g_dyCaption;
    if ((g_fMenu & 1) == 0) {
        g_dyAdjust = g_dyMenu + g_dyCaption;
        if (FMenuWrapped()) {
            g_dyAdjust += g_dyMenu;
            fWrapCheck = TRUE;
        }
    }

    g_dxClient = g_cBlk * DX_BLK + DX_WINDOW;
    g_dyClient = g_cRow * DY_BLK + DY_WINDOW;

    /* A new field changes what the zoom is allowed to be: going from
       a small board to a very large one at the same magnification can
       ask for a window past what the compositor will draw, and the
       window would simply stop appearing.  Bring the zoom back inside
       the ceiling rather than letting that happen. */
    g_nZoom = ClampInt(g_nZoom, ZOOM_MIN, ZoomMax());

    /* Pull the window back on screen if it hangs off the edge - but
       only when it would actually fit.  A board wider or taller than
       the screen can never be pulled on, and trying would drag it back
       to the top-left corner every time the zoom changed, throwing away
       wherever the player had panned to. */
    if (LogToDev(g_dxClient) <= DxpScreen(0)) {
        d = LogToDev(g_dxClient) + g_xWindow - DxpScreen(0);
        if (d > 0) {
            wFlags |= ADJUST_MOVE;
            g_xWindow -= d;
            if (g_xWindow < 0) g_xWindow = 0;
        }
    }
    if (LogToDev(g_dyClient) <= DxpScreen(1)) {
        d = LogToDev(g_dyClient) + g_yWindow - DxpScreen(1);
        if (d > 0) {
            wFlags |= ADJUST_MOVE;
            g_yWindow -= d;
            if (g_yWindow < 0) g_yWindow = 0;
        }
    }

    if (g_fFrozen)
        return;

    /* whether this call is going to invalidate the window afterwards */
    fOwnPaint = (wFlags & (ADJUST_PAINT | ADJUST_RESCALE)) != 0;

    if (wFlags & ADJUST_MOVE)
        MoveToClient(!fOwnPaint);

    /* a wider window may have un-wrapped the menu bar */
    if (fWrapCheck && !FMenuWrapped()) {
        g_dyAdjust -= g_dyMenu;
        MoveToClient(!fOwnPaint);
    }

    if (wFlags & (ADJUST_PAINT | ADJUST_RESCALE)) {
        if (wFlags & ADJUST_PAINT)
            g_fBackValid = 0;
        /* No erase: every pixel of the update region is about to be
           copied over from the surface, so filling the client with
           the background brush first is a second pass over the whole
           window for nothing - and on a large board that is the most
           expensive thing in the repaint, as well as what makes it
           flicker. */
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
}

/*=====================================================================
    menus
  =====================================================================*/
static void CheckItem(UINT id, int fCheck)
{
    CheckMenuItem(g_hMenu, id, fCheck ? MF_CHECKED : MF_UNCHECKED);
}

static void FixMenus(void)
{
    CheckItem(IDM_BEGIN,  g_wGameType == LEVEL_BEGIN);
    CheckItem(IDM_INTER,  g_wGameType == LEVEL_INTER);
    CheckItem(IDM_EXPERT, g_wGameType == LEVEL_EXPERT);
    CheckItem(IDM_CUSTOM, g_wGameType == LEVEL_CUSTOM);
    CheckItem(IDM_COLOR,  g_fColor);
    CheckItem(IDM_MARK,   g_fMark);
    CheckItem(IDM_SOUND,  g_fSound);
}

static void SetMenuBar(UINT fMenu)
{
    g_fMenu = (int)fMenu;
    FixMenus();
    SetMenu(g_hwnd, (g_fMenu & 1) ? NULL : g_hMenu);
    AdjustWindow(ADJUST_MOVE);
}

/*=====================================================================
    the board
  =====================================================================*/
/* Whether the machine will really give us an offscreen surface this
   big.  A fixed pixel budget here can only be wrong in one direction
   or the other - it used to turn away fields this machine could
   manage perfectly well - so the question is simply put to the
   graphics layer and the answer believed.  The surface in hand is
   dropped first, or a big field would be weighed against the memory
   the field it is replacing is still holding. */
static BOOL FSurfaceFits(int dx, int dy)
{
    HDC     hdc;
    HBITMAP hbm;

    FreeBack();
    hdc = GetDC(g_hwnd);
    if (hdc == NULL)
        return FALSE;
    hbm = CreateCompatibleBitmap(hdc, dx, dy);
    ReleaseDC(g_hwnd, hdc);
    if (hbm == NULL)
        return FALSE;
    DeleteObject(hbm);
    return TRUE;
}

/*---------------------------------------------------------------------
    Everything a field of this size needs: the board, the flood-fill
    queue and the offscreen surface.  It succeeds or fails as a unit,
    so a field the machine cannot manage is turned away with the game
    already in progress left untouched.
  -------------------------------------------------------------------*/
static BOOL FAllocBoard(int cBlk, int cRow)
{
    HANDLE hHeap = GetProcessHeap();
    BYTE  *pbNew;
    int   *pxNew, *pyNew;
    int    dx, dy, cCell;

    if (cBlk < 1 || cRow < 1)
        return FALSE;

    /* The edge limits are checked by division, so nothing overflows
       on the way to finding out that it would have.  Inside them a
       side is at most 1,873 squares, so cBlk * cRow is under four
       million and the rest of the arithmetic is comfortably inside an
       int. */
    if (cBlk > (DXY_SURFACE_MAX - DX_WINDOW) / DX_BLK) return FALSE;
    if (cRow > (DXY_SURFACE_MAX - DY_WINDOW) / DY_BLK) return FALSE;

    dx = cBlk * DX_BLK + DX_WINDOW;
    dy = cRow * DY_BLK + DY_WINDOW;
    if (!FSurfaceFits(dx, dy))
        return FALSE;

    cCell = cBlk * cRow;

    pbNew = (BYTE *)HeapAlloc(hHeap, 0, (SIZE_T)(cBlk + 2) * (cRow + 2));
    pxNew = (int *)HeapAlloc(hHeap, 0, (SIZE_T)(cCell + 2) * sizeof(int));
    pyNew = (int *)HeapAlloc(hHeap, 0, (SIZE_T)(cCell + 2) * sizeof(int));
    if (!pbNew || !pxNew || !pyNew) {
        if (pbNew) HeapFree(hHeap, 0, pbNew);
        if (pxNew) HeapFree(hHeap, 0, pxNew);
        if (pyNew) HeapFree(hHeap, 0, pyNew);
        return FALSE;
    }

    if (g_rgBlk)    HeapFree(hHeap, 0, g_rgBlk);
    if (g_rgxVisit) HeapFree(hHeap, 0, g_rgxVisit);
    if (g_rgyVisit) HeapFree(hHeap, 0, g_rgyVisit);

    g_rgBlk     = pbNew;
    g_rgxVisit  = pxNew;
    g_rgyVisit  = pyNew;
    g_cStride   = cBlk + 2;
    g_cVisitMax = cCell + 2;
    return TRUE;
}

static void InitBlks(void)
{
    int i;
    int cb = (g_cBlk + 2) * (g_cRow + 2);

    for (i = 0; i < cb; i++)
        g_rgBlk[i] = BLK_BLANKUP;

    for (i = 0; i < g_cBlk + 2; i++) {
        *PblkAt(i, 0)          = BLK_BORDER;
        *PblkAt(i, g_cRow + 1) = BLK_BORDER;
    }
    for (i = 0; i < g_cRow + 2; i++) {
        *PblkAt(0, i)          = BLK_BORDER;
        *PblkAt(g_cBlk + 1, i) = BLK_BORDER;
    }
}

/* set a cell's tile (keeping the mine bit) and repaint it */
static void ChangeBlk(int x, int y, BYTE bTile)
{
    BYTE *pblk = PblkAt(x, y);
    *pblk = (BYTE)((*pblk & 0xE0) | bTile);
    DisplayBlk(x, y);
}

static int CountBombs(int x, int y)
{
    int c = 0, i, j;

    for (j = y - 1; j <= y + 1; j++)
        for (i = x - 1; i <= x + 1; i++)
            if (*PblkAt(i, j) & MASK_BOMB)
                c++;
    return c;
}

static int CountMarks(int x, int y)
{
    int c = 0, i, j;

    for (j = y - 1; j <= y + 1; j++)
        for (i = x - 1; i <= x + 1; i++)
            if ((*PblkAt(i, j) & MASK_ICON) == BLK_BOMBFLAG)
                c++;
    return c;
}

/* reveal what is left: BLK_BOMBUP after a loss, BLK_BOMBFLAG after a
   win; flags that turned out to be wrong become BLK_WRONG */
static void ShowBombs(BYTE bTile)
{
    int x, y;

    for (y = 1; y <= g_cRow; y++) {
        for (x = 1; x <= g_cBlk; x++) {
            BYTE *pblk = PblkAt(x, y);
            BYTE  blk  = *pblk;

            if (blk & MASK_VISIT)
                continue;
            if (blk & MASK_BOMB) {
                if ((blk & MASK_ICON) != BLK_BOMBFLAG)
                    *pblk = (BYTE)((blk & 0xE0) | bTile);
            } else if ((blk & MASK_ICON) == BLK_BOMBFLAG) {
                *pblk = (BYTE)((blk & 0xE0) | BLK_WRONG);
            }
        }
    }
    DisplayField();
}

/* a covered cell drawn pushed in / popped out */
static void PushBlk(int x, int y)
{
    BYTE *pblk  = PblkAt(x, y);
    BYTE  bTile = (BYTE)(*pblk & MASK_ICON);

    if (bTile == BLK_GUESSUP)      bTile = BLK_GUESSDN;
    else if (bTile == BLK_BLANKUP) bTile = BLK_0;
    *pblk = (BYTE)((*pblk & 0xE0) | bTile);
}

static void PopBlk(int x, int y)
{
    BYTE *pblk  = PblkAt(x, y);
    BYTE  bTile = (BYTE)(*pblk & MASK_ICON);

    if (bTile == BLK_GUESSDN) bTile = BLK_GUESSUP;
    else if (bTile == BLK_0)  bTile = BLK_BLANKUP;
    *pblk = (BYTE)((*pblk & 0xE0) | bTile);
}

/*---------------------------------------------------------------------
    Flood fill through a ring buffer rather than recursion: empty cells
    push their coordinates and the caller walks the ring uncovering all
    eight neighbours of each.  The ring holds one entry per cell, so it
    can never lap itself and drop a pending square.
  -------------------------------------------------------------------*/
static void StepBlk(int x, int y)
{
    BYTE *pblk = PblkAt(x, y);
    BYTE  bTile;
    int   c;

    if (*pblk & MASK_VISIT)
        return;
    bTile = (BYTE)(*pblk & MASK_ICON);
    if (bTile == BLK_BORDER || bTile == BLK_BOMBFLAG)
        return;

    g_cBlkVisit++;
    c = CountBombs(x, y);
    *pblk = (BYTE)(c | MASK_VISIT);
    DisplayBlk(x, y);

    if (c == 0) {
        g_rgxVisit[g_iVisitHead] = x;
        g_rgyVisit[g_iVisitHead] = y;
        if (++g_iVisitHead == g_cVisitMax)
            g_iVisitHead = 0;
    }
}

static void StepXY(int x, int y)
{
    int i = 1;

    g_iVisitHead = 1;
    StepBlk(x, y);
    if (g_iVisitHead == 1)
        return;

    do {
        int xT = g_rgxVisit[i];
        int yT = g_rgyVisit[i];

        StepBlk(xT - 1, yT - 1);
        StepBlk(xT,     yT - 1);
        StepBlk(xT + 1, yT - 1);
        StepBlk(xT - 1, yT);
        StepBlk(xT + 1, yT);
        StepBlk(xT - 1, yT + 1);
        StepBlk(xT,     yT + 1);
        StepBlk(xT + 1, yT + 1);

        if (++i == g_cVisitMax)
            i = 0;
    } while (i != g_iVisitHead);
}

/*=====================================================================
    the game
  =====================================================================*/
static void UpdateBombCount(int d)
{
    g_cBombLeft += d;
    DisplayBombCount();
}

static void GameOver(int fWon)
{
    g_fTimer     = 0;
    g_iButtonCur = fWon ? FACE_WIN : FACE_DEAD;
    DisplayButton(g_iButtonCur);

    ShowBombs((BYTE)(fWon ? BLK_BOMBFLAG : BLK_BOMBUP));

    if (fWon && g_cBombLeft != 0)
        UpdateBombCount(-g_cBombLeft);

    PlayTune(fWon ? SOUND_WIN : SOUND_LOSE);
    g_fStatus = STATUS_OVER;

    if (fWon && g_wGameType != LEVEL_CUSTOM) {
        if (g_cSec < g_rgTime[g_wGameType]) {
            g_rgTime[g_wGameType] = g_cSec;
            DoEnterName();
            DoBestTimes();
        }
    }
}

static void StartGame(void)
{
    UINT wFlags;
    int  n, x, y, cCells;

    g_fTimer = 0;

    /* resize the board if the configured field changed */
    if (g_rgBlk == NULL || g_cBlkCfg != g_cBlk || g_cRowCfg != g_cRow) {
        if (!FAllocBoard(g_cBlkCfg, g_cRowCfg)) {
            ReportErr(IDS_ERR_MEMORY);
            if (g_rgBlk != NULL) {
                g_cBlkCfg = g_cBlk;         /* keep the field we have */
                g_cRowCfg = g_cRow;
            } else {                        /* nothing to fall back on */
                g_cMines  = c_rgPreset[LEVEL_BEGIN][0];
                g_cRowCfg = c_rgPreset[LEVEL_BEGIN][1];
                g_cBlkCfg = c_rgPreset[LEVEL_BEGIN][2];
                if (!FAllocBoard(g_cBlkCfg, g_cRowCfg))
                    return;
            }
        }
    }

    wFlags = (g_cBlkCfg == g_cBlk && g_cRowCfg == g_cRow)
             ? ADJUST_PAINT : (ADJUST_MOVE | ADJUST_PAINT);

    g_cBlk = g_cBlkCfg;
    g_cRow = g_cRowCfg;

    /* at least one square has to stay clear, or the placement below
       would never find a home for the last mine */
    cCells = g_cBlk * g_cRow;
    if (g_cMines > cCells - 1) g_cMines = cCells - 1;
    if (g_cMines < 1)          g_cMines = 1;

    g_cLedDigits = CDigits(g_cMines);
    if (g_cLedDigits < 3)
        g_cLedDigits = 3;

    InitBlks();
    g_iButtonCur = FACE_HAPPY;

    if (g_cMines * 2 <= cCells) {
        for (n = g_cMines; n > 0; n--) {
            do {
                x = MinerRand() % g_cBlk;
                y = MinerRand() % g_cRow;
            } while (*PblkAt(x + 1, y + 1) & MASK_BOMB);
            *PblkAt(x + 1, y + 1) |= MASK_BOMB;
        }
    } else {
        /* More than half the field is mines.  Picking at random would
           spend most of its time landing on squares already taken, so
           mine the lot and pick the gaps instead. */
        for (y = 1; y <= g_cRow; y++)
            for (x = 1; x <= g_cBlk; x++)
                *PblkAt(x, y) |= MASK_BOMB;
        for (n = cCells - g_cMines; n > 0; n--) {
            do {
                x = MinerRand() % g_cBlk;
                y = MinerRand() % g_cRow;
            } while ((*PblkAt(x + 1, y + 1) & MASK_BOMB) == 0);
            *PblkAt(x + 1, y + 1) &= (BYTE)~MASK_BOMB;
        }
    }

    g_cBlkTotal = cCells - g_cMines;
    g_cSec      = 0;
    g_cBombLeft = g_cMines;
    g_cBlkVisit = 0;
    g_fStatus   = STATUS_PLAY;

    UpdateBombCount(0);
    AdjustWindow(wFlags);
}

/* left click on an unmarked covered cell */
static void StepSquare(int x, int y)
{
    int fWon;

    if ((*PblkAt(x, y) & MASK_BOMB) == 0) {
        StepXY(x, y);
        if (g_cBlkVisit != g_cBlkTotal)
            return;
        fWon = 1;
    } else {
        /* the very first cell uncovered is never a mine: the mine is
           moved to the first free square.  Note the scan deliberately
           stops one short of the last row and column. */
        if (g_cBlkVisit == 0) {
            int i, j;

            if (g_cRow < 2)
                return;
            for (j = 1; j < g_cRow; j++) {
                for (i = 1; i < g_cBlk; i++) {
                    if ((*PblkAt(i, j) & MASK_BOMB) == 0) {
                        *PblkAt(x, y)  = BLK_BLANKUP;
                        *PblkAt(i, j) |= MASK_BOMB;
                        StepXY(x, y);
                        return;
                    }
                }
            }
            return;
        }
        ChangeBlk(x, y, (BYTE)(MASK_VISIT | BLK_EXPLODE));
        fWon = 0;
    }
    GameOver(fWon);
}

/* both buttons (or shift) over an uncovered numbered cell */
static void StepBlock(int x, int y)
{
    BYTE blk   = *PblkAt(x, y);
    int  fBoom = 0;
    int  i, j;

    if ((blk & MASK_VISIT) == 0 ||
        (blk & MASK_ICON) != (BYTE)CountMarks(x, y)) {
        TrackMouse(-2, -2);         /* just pop the cells back out */
        return;
    }

    for (j = y - 1; j <= y + 1; j++) {
        for (i = x - 1; i <= x + 1; i++) {
            BYTE b = *PblkAt(i, j);
            if ((b & MASK_ICON) == BLK_BOMBFLAG || (b & MASK_BOMB) == 0) {
                StepXY(i, j);
            } else {
                fBoom = 1;
                ChangeBlk(i, j, (BYTE)(MASK_VISIT | BLK_EXPLODE));
            }
        }
    }

    if (fBoom) {
        GameOver(0);
        return;
    }
    if (g_cBlkVisit != g_cBlkTotal)
        return;
    GameOver(1);
}

/* right click: blank -> flag -> '?' -> blank */
static void MarkSquare(int x, int y)
{
    BYTE blk, bTile;
    int  d      = 0;
    int  fCount = 1;

    if (x < 1 || y < 1 || x > g_cBlk || y > g_cRow)
        return;

    blk = *PblkAt(x, y);
    if (blk & MASK_VISIT)
        return;

    switch (blk & MASK_ICON) {
    case BLK_BOMBFLAG:
        d     = 1;
        bTile = (BYTE)(g_fMark ? BLK_GUESSUP : BLK_BLANKUP);
        break;
    case BLK_GUESSUP:
        bTile  = BLK_BLANKUP;
        fCount = 0;
        break;
    default:
        d     = -1;
        bTile = BLK_BOMBFLAG;
        break;
    }

    if (fCount)
        UpdateBombCount(d);
    ChangeBlk(x, y, bTile);

    if ((*PblkAt(x, y) & MASK_ICON) == BLK_BOMBFLAG &&
        g_cBlkVisit == g_cBlkTotal)
        GameOver(1);
}

/*---------------------------------------------------------------------
    Click an uncovered number and, if the squares still covered around
    it number exactly what the square says, every one of them has to be
    a mine - so flag the ones that are not flagged already.  Squares
    already carrying a flag count towards the total but are left alone;
    question marks count too, and become flags.

    This is the counterpart of chording: chording opens a number's
    neighbours once its flags add up, this flags them once its blanks
    add up.
  -------------------------------------------------------------------*/
static void FlagSquare(int x, int y)
{
    BYTE bTile = (BYTE)(*PblkAt(x, y) & MASK_ICON);
    int  cCovered = 0;
    int  cFlagged = 0;
    int  i, j;

    if ((*PblkAt(x, y) & MASK_VISIT) == 0)
        return;
    if (bTile < 1 || bTile > BLK_8)
        return;

    for (j = y - 1; j <= y + 1; j++) {
        for (i = x - 1; i <= x + 1; i++) {
            BYTE b = *PblkAt(i, j);
            if ((b & MASK_VISIT) || (b & MASK_ICON) == BLK_BORDER)
                continue;
            cCovered++;
        }
    }
    if (cCovered != (int)bTile)
        return;

    for (j = y - 1; j <= y + 1; j++) {
        for (i = x - 1; i <= x + 1; i++) {
            BYTE b = *PblkAt(i, j);
            if ((b & MASK_VISIT) || (b & MASK_ICON) == BLK_BORDER)
                continue;
            if ((b & MASK_ICON) == BLK_BOMBFLAG)
                continue;
            UpdateBombCount(-1);
            ChangeBlk(i, j, BLK_BOMBFLAG);
            cFlagged++;
        }
    }

    if (cFlagged && g_cBlkVisit == g_cBlkTotal)
        GameOver(1);
}

/*---------------------------------------------------------------------
    mouse tracking: push in / pop out the cell (or the 3x3 block when
    chording) under the cursor
  -------------------------------------------------------------------*/
static void TrackMouse(int x, int y)
{
    int xOld = g_xCur;
    int yOld = g_yCur;

    if (x == xOld && y == yOld)
        return;

    g_xCur = x;
    g_yCur = y;

    if (!g_fChord) {
        if (xOld > 0 && yOld > 0 && xOld <= g_cBlk && yOld <= g_cRow &&
            (*PblkAt(xOld, yOld) & MASK_VISIT) == 0) {
            PopBlk(xOld, yOld);
            DisplayBlk(xOld, yOld);
        }
        if (x > 0 && y > 0 && x <= g_cBlk && y <= g_cRow &&
            (*PblkAt(x, y) & MASK_VISIT) == 0 &&
            (*PblkAt(x, y) & MASK_ICON) != BLK_BOMBFLAG) {
            PushBlk(x, y);
            DisplayBlk(x, y);
        }
        return;
    }

    {
        BOOL fNew = (x >= 1 && y >= 1 && x <= g_cBlk && y <= g_cRow);
        BOOL fOld = (xOld >= 1 && yOld >= 1 && xOld <= g_cBlk && yOld <= g_cRow);
        int  yOld1, yOld2, yNew1, yNew2;
        int  xOld1, xOld2, xNew1, xNew2;
        int  i, j;

        yOld1 = (yOld - 1 < 2)       ? 1      : yOld - 1;
        yOld2 = (yOld + 1 >= g_cRow) ? g_cRow : yOld + 1;
        yNew1 = (y - 1 < 2)          ? 1      : y - 1;
        yNew2 = (y + 1 >= g_cRow)    ? g_cRow : y + 1;
        xOld1 = (xOld - 1 < 2)       ? 1      : xOld - 1;
        xOld2 = (xOld + 1 >= g_cBlk) ? g_cBlk : xOld + 1;
        xNew1 = (x - 1 < 2)          ? 1      : x - 1;
        xNew2 = (x + 1 >= g_cBlk)    ? g_cBlk : x + 1;

        if (fOld)
            for (j = yOld1; j <= yOld2; j++)
                for (i = xOld1; i <= xOld2; i++)
                    if ((*PblkAt(i, j) & MASK_VISIT) == 0)
                        PopBlk(i, j);
        if (fNew)
            for (j = yNew1; j <= yNew2; j++)
                for (i = xNew1; i <= xNew2; i++)
                    if ((*PblkAt(i, j) & MASK_VISIT) == 0)
                        PushBlk(i, j);
        if (fOld)
            for (j = yOld1; j <= yOld2; j++)
                for (i = xOld1; i <= xOld2; i++)
                    DisplayBlk(i, j);
        if (fNew)
            for (j = yNew1; j <= yNew2; j++)
                for (i = xNew1; i <= xNew2; i++)
                    DisplayBlk(i, j);
    }
}

/* a mouse button came back up over the field */
/* does this uncovered square have a covered one next to it? */
static BOOL FTouchesCovered(int x, int y)
{
    int i, j;

    for (j = y - 1; j <= y + 1; j++) {
        for (i = x - 1; i <= x + 1; i++) {
            BYTE b = *PblkAt(i, j);
            if ((b & MASK_ICON) == BLK_BORDER)
                continue;
            if ((b & MASK_VISIT) == 0)
                return TRUE;
        }
    }
    return FALSE;
}

/*---------------------------------------------------------------------
    Clicking anywhere in an uncovered area works the whole edge of it:
    every uncovered square in that area which still has a covered
    neighbour gets the same treatment a click on it by hand would give
    - flagged where the covered squares can only be mines, opened where
    the flags already add up.  One click then clears as much ground as
    the area's edge allows.

    Done in two passes.  The first floods the contiguous uncovered area
    and marks it with a spare bit of the board byte; the second walks
    the board acting on the marked squares and clearing the marks as it
    goes, so the marks never outlive the call even if the game ends
    part way through.  Only squares marked by the first pass are acted
    on, so ground opened during the sweep is left for the next click -
    one click is one pass over the edge, not a solver.
  -------------------------------------------------------------------*/
static void SweepRegion(int x, int y)
{
    int iHead = 0, iTail = 0;
    int i, j;

    if ((*PblkAt(x, y) & MASK_VISIT) == 0)
        return;

    /* pass one: flood the uncovered area, marking as we go.  The
       flood-fill queue is free here - StepXY is not running yet. */
    *PblkAt(x, y) |= MASK_SWEEP;
    g_rgxVisit[iTail] = x;
    g_rgyVisit[iTail] = y;
    iTail++;

    while (iHead < iTail) {
        int cx = g_rgxVisit[iHead];
        int cy = g_rgyVisit[iHead];
        iHead++;

        for (j = cy - 1; j <= cy + 1; j++) {
            for (i = cx - 1; i <= cx + 1; i++) {
                BYTE *p = PblkAt(i, j);

                if ((*p & MASK_ICON) == BLK_BORDER) continue;
                if ((*p & MASK_VISIT) == 0)         continue;
                if (*p & MASK_SWEEP)                continue;
                *p |= MASK_SWEEP;
                if (iTail < g_cVisitMax) {
                    g_rgxVisit[iTail] = i;
                    g_rgyVisit[iTail] = j;
                    iTail++;
                }
            }
        }
    }

    /* pass two: act on the edge of it */
    for (j = 1; j <= g_cRow; j++) {
        for (i = 1; i <= g_cBlk; i++) {
            BYTE *p = PblkAt(i, j);
            BYTE  blk;

            if ((*p & MASK_SWEEP) == 0)
                continue;
            *p &= (BYTE)~MASK_SWEEP;

            if ((g_fStatus & STATUS_PLAY) == 0)
                continue;               /* keep clearing, stop acting */
            if (!FTouchesCovered(i, j))
                continue;

            FlagSquare(i, j);
            if ((g_fStatus & STATUS_PLAY) == 0)
                continue;

            /* open the rest when the flags add up.  Checked here rather
               than letting StepBlock turn it down, because its refusal
               path resets the square being tracked under the cursor. */
            blk = *PblkAt(i, j);
            if ((blk & MASK_VISIT) &&
                (blk & MASK_ICON) == (BYTE)CountMarks(i, j))
                StepBlock(i, j);
        }
    }
}

/* What a left click does to the square under the cursor.  Shared by
   the button-up handler and by the auto-repeat while it is held. */
static void DoClickActionInner(void)
{
    BYTE blk;

    if ((g_fStatus & STATUS_PLAY) == 0)
        return;
    if (g_xCur < 1 || g_yCur < 1 || g_xCur > g_cBlk || g_yCur > g_cRow)
        return;

    if (g_fChord) {
        StepBlock(g_xCur, g_yCur);
        return;
    }

    blk = *PblkAt(g_xCur, g_yCur);
    if (blk & MASK_VISIT) {
        /* Clicking an uncovered square works the whole edge of the
           uncovered area it belongs to: each square on that edge is
           flagged where its covered neighbours can only be mines, and
           opened where its flags already add up.  Clicking one square
           on its own is just the one-square case of this. */
        SweepRegion(g_xCur, g_yCur);
    } else if ((blk & MASK_ICON) != BLK_BOMBFLAG) {
        StepSquare(g_xCur, g_yCur);
    }
}

/* Everything one click can set off - a flood fill, a chord, a sweep
   along the edge of an uncovered area, and the end of the game that
   any of them may bring - draws through here, so one click costs one
   copy to the window however many squares it turns over. */
static void DoClickAction(void)
{
    BeginBatch();
    DoClickActionInner();
    EndBatch();
}

/* the clock starts on the first square the player actually opens */
static void StartClockIfIdle(void)
{
    if (g_cBlkVisit == 0 && g_cSec == 0) {
        PlayTune(SOUND_TICK);
        g_cSec++;
        DisplayTime();
        g_fTimer = 1;
        if (SetTimer(g_hwnd, ID_TIMER, 1000, NULL) == 0)
            ReportErr(IDS_ERR_TIMER);
    }
}

/*---------------------------------------------------------------------
    Double-click an uncovered square: go to the next covered one.

    On a field of a million squares the last few that are still covered
    can be anywhere, and hunting for them by dragging the window about
    is miserable.  A double-click on a square that is already uncovered
    goes to the next covered one in reading order, wrapping round at
    the end, and brings both the window and the pointer to it.

    Only uncovered squares do this, so it never gets in the way of
    play: double-clicking a covered square opens it, exactly as two
    ordinary clicks always did.  That also means the pointer ends the
    jump sitting on a covered square - to go on to the one after, click
    an uncovered square nearby.

    Squares already carrying a flag are passed over: they are covered,
    but the player has said what they think is under them, and stopping
    at each of them would mean wading through every mine on the board
    to reach anything still undecided.
  -------------------------------------------------------------------*/
static BOOL FNextCovered(int xFrom, int yFrom, int *px, int *py)
{
    int x = xFrom, y = yFrom;
    int n, cCell = g_cBlk * g_cRow;

    if (g_rgBlk == NULL)
        return FALSE;

    for (n = 0; n < cCell; n++) {
        BYTE blk;

        if (++x > g_cBlk) {
            x = 1;
            if (++y > g_cRow)
                y = 1;
        }
        blk = *PblkAt(x, y);
        if ((blk & MASK_VISIT) == 0 && (blk & MASK_ICON) != BLK_BOMBFLAG) {
            *px = x;
            *py = y;
            return TRUE;
        }
    }
    return FALSE;               /* nothing left to go to */
}

/* Bring a square into view and put the pointer on it.  The window is
   only moved when the square is not already on screen - there is no
   sense throwing away where the player had got to when what they
   asked for is in front of them - and when it is moved the square is
   centred, so there is board visible all round it. */
static void GoToSquare(int x, int y)
{
    int xDev = LogToDev(x * DX_BLK - 4);
    int yDev = LogToDev(y * DY_BLK + 39);
    int cx   = LogToDev(x * DX_BLK + 12) - xDev;
    int cy   = LogToDev(y * DY_BLK + 55) - yDev;
    int dxScreen = DxpScreen(0);
    int dyScreen = DxpScreen(1);
    int xScr = g_xWindow + xDev;
    int yScr = g_yWindow + yDev;

    if (xScr < 0 || yScr < 0 ||
        xScr + cx > dxScreen || yScr + cy > dyScreen) {
        g_xWindow = (dxScreen - cx) / 2 - xDev;
        g_yWindow = (dyScreen - cy) / 2 - yDev;
        /* Repaint: a window bigger than the screen keeps only what it
           can when it moves and leaves the rest invalid, so moving it
           without asking for a repaint leaves the newly uncovered part
           of the board blank - which, after a jump right across a
           large field, is most of what the player can see. */
        MoveToClient(TRUE);
        ClearFullscreenClaim();
        xScr = g_xWindow + xDev;
        yScr = g_yWindow + yDev;
    }
    SetCursorPos(xScr + cx / 2, yScr + cy / 2);
}

/* Two clicks on the same square, close enough together to be one
   gesture.  Worked out here rather than with WM_LBUTTONDBLCLK so that
   the run of button messages the rest of the game reads stays exactly
   as it was.

   The times come from the messages, not the clock: what the first
   click set off may take a while - a sweep along the edge of a
   million opened squares is not quick - and timing from when that
   finished would make the pair look far apart and the double-click go
   unnoticed.  GetMessageTime is when the click actually happened, so
   how long the game spent on it does not come into it. */
static BOOL FSecondClick(int x, int y)
{
    DWORD tm = (DWORD)GetMessageTime();
    BOOL  f  = (x == g_xLastClick && y == g_yLastClick &&
                tm - g_tmLastClick <= GetDoubleClickTime());

    g_tmLastClick = tm;
    /* a detected pair ends there, so three clicks are not two pairs */
    g_xLastClick  = f ? -1 : x;
    g_yLastClick  = f ? -1 : y;
    return f;
}

static void DoButton1Up(void)
{
    if (g_xCur > 0 && g_yCur > 0 && g_xCur <= g_cBlk && g_yCur <= g_cRow) {
        StartClockIfIdle();
        if (g_fStatus & STATUS_PLAY) {
            int xNext, yNext;

            if (FSecondClick(g_xCur, g_yCur) &&
                (*PblkAt(g_xCur, g_yCur) & MASK_VISIT) &&
                FNextCovered(g_xCur, g_yCur, &xNext, &yNext))
                GoToSquare(xNext, yNext);
            else
                DoClickAction();
        } else {
            g_xCur = -2;
            g_yCur = -2;
        }
    }
    DisplayButton(g_iButtonCur);
}

static void DoTimer(void)
{
    if (g_fTimer && g_cSec < 999) {
        g_cSec++;
        DisplayTime();
        PlayTune(SOUND_TICK);
    }
}

static void PauseGame(void)
{
    KillSound();
    if ((g_fStatus & STATUS_PAUSE) == 0)
        g_fOldTimer = g_fTimer;
    if (g_fStatus & STATUS_PLAY)
        g_fTimer = 0;
    g_fStatus |= STATUS_PAUSE;
}

static void ResumeGame(void)
{
    if (g_fStatus & STATUS_PLAY)
        g_fTimer = g_fOldTimer;
    g_fStatus &= ~(DWORD)STATUS_PAUSE;
}

/*=====================================================================
    the smiley button - it runs its own little modal loop, as in the
    original, so that the rest of the window proc never sees the drag
  =====================================================================*/
static BOOL FButtonHit(LPARAM lParam)
{
    RECT  rc;
    MSG   msg;
    POINT pt;
    BOOL  fIn = TRUE;

    pt.x = GET_X_LPARAM(lParam);
    pt.y = GET_Y_LPARAM(lParam);

    rc.left   = LogToDev((g_dxClient - DX_BUTTON) >> 1);
    rc.right  = LogToDev(((g_dxClient - DX_BUTTON) >> 1) + DX_BUTTON);
    rc.top    = LogToDev(Y_BUTTON);
    rc.bottom = LogToDev(Y_BUTTON + DY_BUTTON);

    if (!PtInRect(&rc, pt))
        return FALSE;

    SetCapture(g_hwnd);
    DisplayButton(FACE_DOWN);
    MapWindowPoints(g_hwnd, NULL, (LPPOINT)&rc, 2);

    for (;;) {
        if (!PeekMessageW(&msg, g_hwnd, 0x0200, 0x020D, PM_REMOVE))
            continue;

        if (msg.message == WM_MOUSEMOVE) {
            BOOL f = PtInRect(&rc, msg.pt) ? TRUE : FALSE;
            if (f != fIn) {
                fIn = f;
                DisplayButton(f ? FACE_DOWN : g_iButtonCur);
            }
        } else if (msg.message == WM_LBUTTONUP) {
            if (fIn && PtInRect(&rc, msg.pt)) {
                g_iButtonCur = FACE_HAPPY;
                DisplayButton(FACE_HAPPY);
                StartGame();
            }
            break;
        }
    }
    ReleaseCapture();
    return TRUE;
}

/*=====================================================================
    dialogs
  =====================================================================*/
static const DWORD c_rgdwHelpPref[] = {
    ID_PREF_HEIGHT, 1000, ID_PREF_WIDTH, 1001, ID_PREF_MINE,      1002,
    112,            1000, 113,           1001, ID_PREF_MINETEXT,  1002,
    0, 0
};
static const DWORD c_rgdwHelpBest[] = {
    ID_BEST_RESET, 1003, ID_BEST_LBL1,  1004, ID_BEST_LBL2, 1004,
    ID_BEST_LBL3,  1004, ID_BEST_TIME1, 1004, ID_BEST_TIME2, 1004,
    ID_BEST_TIME3, 1004, ID_BEST_NAME1, 1004, ID_BEST_NAME2, 1004,
    ID_BEST_NAME3, 1004, 0, 0
};

/* a field of the custom dialog: a floor, but no ceiling.  Whatever
   comes back is handed to StartGame, which is the thing that knows
   whether a field that size can actually be built. */
static UINT GetDlgIntMin(HWND hDlg, int id, UINT lo)
{
    BOOL fOk;
    UINT v = GetDlgItemInt(hDlg, id, &fOk, FALSE);

    return (!fOk || v < lo) ? lo : v;
}

/*---------------------------------------------------------------------
    How many mines belong on a cBlk x cRow field.

    The presets run 12% (beginner), 16% (intermediate) and 21%
    (expert), so density climbs with size.  This carries that line on:
    16% at 2,500 squares up to 21% at 40,000 and flat after that -
    dense enough to stay interesting, short of the point where a big
    board turns into pure guesswork.  A little jitter keeps two presses
    of Random from producing the same game.
  -------------------------------------------------------------------*/
static int CMinesForField(int cBlk, int cRow)
{
    int cCells = cBlk * cRow;
    int nPerMil, cMines, nJitter;

    if (cCells <= 2500)       nPerMil = 160;
    else if (cCells >= 40000) nPerMil = 210;
    else nPerMil = 160 + MulDiv(cCells - 2500, 50, 40000 - 2500);

    cMines  = MulDiv(cCells, nPerMil, 1000);
    nJitter = cMines / 20;                      /* +/- 5% */
    if (nJitter > 0)
        cMines += (MinerRand() % (nJitter * 2 + 1)) - nJitter;

    if (cMines < CMINE_MIN)   cMines = CMINE_MIN;
    if (cMines > cCells - 1)  cMines = cCells - 1;
    return cMines;
}

static INT_PTR CALLBACK PrefDlgProc(HWND hDlg, UINT msg, WPARAM wParam,
                                    LPARAM lParam)
{
    switch (msg) {
    case WM_HELP:
        WinHelpW(((LPHELPINFO)lParam)->hItemHandle
                     ? (HWND)((LPHELPINFO)lParam)->hItemHandle : hDlg,
                 L"winmine.hlp", HELP_WM_HELP, (ULONG_PTR)c_rgdwHelpPref);
        return FALSE;

    case WM_CONTEXTMENU:
        WinHelpW((HWND)wParam, L"winmine.hlp", HELP_CONTEXTMENU,
                 (ULONG_PTR)c_rgdwHelpPref);
        return FALSE;

    case WM_INITDIALOG:
        SetDlgItemInt(hDlg, ID_PREF_HEIGHT, (UINT)g_cRowCfg, FALSE);
        SetDlgItemInt(hDlg, ID_PREF_WIDTH,  (UINT)g_cBlkCfg, FALSE);
        SetDlgItemInt(hDlg, ID_PREF_MINE,   (UINT)g_cMines,  FALSE);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_PREF_RANDOM: {
            int cBlk = RAND_SIDE_MIN +
                       MinerRand() % (RAND_SIDE_MAX - RAND_SIDE_MIN + 1);
            int cRow = RAND_SIDE_MIN +
                       MinerRand() % (RAND_SIDE_MAX - RAND_SIDE_MIN + 1);

            SetDlgItemInt(hDlg, ID_PREF_WIDTH,  (UINT)cBlk, FALSE);
            SetDlgItemInt(hDlg, ID_PREF_HEIGHT, (UINT)cRow, FALSE);
            SetDlgItemInt(hDlg, ID_PREF_MINE,
                          (UINT)CMinesForField(cBlk, cRow), FALSE);
            return TRUE;
        }
        case IDOK:
            g_cRowCfg = (int)GetDlgIntMin(hDlg, ID_PREF_HEIGHT, CROW_MIN);
            g_cBlkCfg = (int)GetDlgIntMin(hDlg, ID_PREF_WIDTH,  CBLK_MIN);
            g_cMines  = (int)GetDlgIntMin(hDlg, ID_PREF_MINE,   CMINE_MIN);
            EndDialog(hDlg, TRUE);
            return TRUE;

        case IDCANCEL:
            EndDialog(hDlg, TRUE);
            return TRUE;
        }
        return FALSE;
    }
    return FALSE;
}

static void SetBestField(HWND hDlg, int idTime, int cSec, LPCWSTR szName)
{
    WCHAR sz[64];
    wsprintfW(sz, g_szSeconds, cSec);
    SetDlgItemTextW(hDlg, idTime, sz);
    SetDlgItemTextW(hDlg, idTime + 1, szName);
}

static INT_PTR CALLBACK BestDlgProc(HWND hDlg, UINT msg, WPARAM wParam,
                                    LPARAM lParam)
{
    switch (msg) {
    case WM_HELP:
        WinHelpW(((LPHELPINFO)lParam)->hItemHandle
                     ? (HWND)((LPHELPINFO)lParam)->hItemHandle : hDlg,
                 L"winmine.hlp", HELP_WM_HELP, (ULONG_PTR)c_rgdwHelpBest);
        return FALSE;

    case WM_CONTEXTMENU:
        WinHelpW((HWND)wParam, L"winmine.hlp", HELP_CONTEXTMENU,
                 (ULONG_PTR)c_rgdwHelpBest);
        return FALSE;

    case WM_INITDIALOG:
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK:
        case IDCANCEL:
            EndDialog(hDlg, TRUE);
            return TRUE;
        case ID_BEST_RESET:
            g_rgTime[0] = g_rgTime[1] = g_rgTime[2] = 999;
            lstrcpyW(g_rgszName[0], g_szAnonymous);
            lstrcpyW(g_rgszName[1], g_szAnonymous);
            lstrcpyW(g_rgszName[2], g_szAnonymous);
            g_fUpdateReg = 1;
            break;
        default:
            return FALSE;
        }
        break;

    default:
        return FALSE;
    }

    SetBestField(hDlg, ID_BEST_TIME1, g_rgTime[0], g_rgszName[0]);
    SetBestField(hDlg, ID_BEST_TIME2, g_rgTime[1], g_rgszName[1]);
    SetBestField(hDlg, ID_BEST_TIME3, g_rgTime[2], g_rgszName[2]);
    return TRUE;
}

static INT_PTR CALLBACK EnterDlgProc(HWND hDlg, UINT msg, WPARAM wParam,
                                     LPARAM lParam)
{
    WCHAR sz[256];

    (void)lParam;

    switch (msg) {
    case WM_INITDIALOG:
        LoadSz((UINT)(IDS_BEST_BEGIN + g_wGameType), sz, 256);
        SetDlgItemTextW(hDlg, ID_ENTER_PROMPT, sz);
        SendMessageW(GetDlgItem(hDlg, ID_ENTER_NAME), EM_LIMITTEXT, 32, 0);
        SetDlgItemTextW(hDlg, ID_ENTER_NAME, g_rgszName[g_wGameType]);
        return TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == 0)
            return FALSE;
        if (LOWORD(wParam) > 2 && LOWORD(wParam) != 100 &&
            LOWORD(wParam) != 0x6D)
            return FALSE;
        GetDlgItemTextW(hDlg, ID_ENTER_NAME, g_rgszName[g_wGameType], 32);
        EndDialog(hDlg, TRUE);
        return TRUE;
    }
    return FALSE;
}

static void DoPref(void)
{
    DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(DLG_PREF), g_hwnd,
                    PrefDlgProc, 0);
    g_wGameType = LEVEL_CUSTOM;
    FixMenus();
    g_fUpdateReg = 1;
    StartGame();
}

static void DoEnterName(void)
{
    DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(DLG_ENTER), g_hwnd,
                    EnterDlgProc, 0);
    g_fUpdateReg = 1;
}

static void DoBestTimes(void)
{
    DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(DLG_BEST), g_hwnd,
                    BestDlgProc, 0);
}

static void DoAbout(void)
{
    WCHAR szName[128], szAuthor[128];
    HICON hIcon;

    LoadSz(IDS_ABOUT_NAME,   szName,   128);
    LoadSz(IDS_ABOUT_AUTHOR, szAuthor, 128);
    hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(ID_ICON_MAIN));
    ShellAboutW(g_hwnd, szName, szAuthor, hIcon);
}

/* HtmlHelp, loaded on demand - there is no .chm shipped with this */
static void DoHelp(int iTopic, DWORD dwCmd)
{
    typedef HWND (WINAPI *PFNHH)(HWND, LPCSTR, UINT, DWORD_PTR);
    char  szPath[MAX_PATH];
    DWORD cch;

    if (iTopic == 4) {
        lstrcpyA(szPath, "NTHelp.chm");
    } else {
        cch = GetModuleFileNameA(g_hInst, szPath, MAX_PATH - 8);
        if (cch == 0)
            return;
        if (cch > 4 && szPath[cch - 4] == '.')
            cch -= 4;
        lstrcpyA(szPath + cch, ".chm");
    }

    if (g_hmodHelp == NULL) {
        if (g_fHelpFailed)
            return;
        g_hmodHelp = LoadLibraryA("hhctrl.ocx");
        if (g_hmodHelp == NULL) {
            g_fHelpFailed = 1;
            return;
        }
    }
    if (g_pfnHtmlHelp == NULL) {
        g_pfnHtmlHelp = GetProcAddress(g_hmodHelp, (LPCSTR)14); /* HtmlHelpA */
        if (g_pfnHtmlHelp == NULL) {
            g_fHelpFailed = 1;
            return;
        }
    }
    ((PFNHH)(void *)g_pfnHtmlHelp)(GetDesktopWindow(), szPath, (UINT)dwCmd, 0);
}

/*=====================================================================
    the window procedure
  =====================================================================*/
static const WCHAR c_szXyzzy[] = L"XYZZY";

/* client pixel -> cell index, undoing the zoom on the way */
#define XFromLp(lp)  ((DevToLog(GET_X_LPARAM(lp)) + 4) >> 4)
#define YFromLp(lp)  ((DevToLog(GET_Y_LPARAM(lp)) - 0x27) >> 4)

static void StartTracking(HWND hwnd, LPARAM lParam)
{
    SetCapture(hwnd);
    g_xCur = -1;
    g_yCur = -1;
    g_fBlockTrack = 1;
    DisplayButton(FACE_CAUTION);
    TrackMouse(XFromLp(lParam), YFromLp(lParam));
    /* arm the auto-repeat; the first one is a while off, so a plain
       click never turns into two */
    SetTimer(hwnd, ID_TIMER_AUTO, AUTO_DELAY, NULL);
}

static void ReleaseTracking(void)
{
    g_fBlockTrack = 0;
    KillTimer(g_hwnd, ID_TIMER_AUTO);
    ReleaseCapture();
    if (g_fStatus & STATUS_PLAY)
        DoButton1Up();
    else
        TrackMouse(-2, -2);
}

/*---------------------------------------------------------------------
    Panning: space + left-drag, or right-drag on an uncovered square.
    Both slide the window, which is how a board larger than the screen
    is got around.
  -------------------------------------------------------------------*/
static void BeginPan(HWND hwnd, int fRight)
{
    RECT rc;

    GetCursorPos(&g_ptPanGrab);
    GetWindowRect(hwnd, &rc);
    g_ptPanOrigin.x = rc.left;
    g_ptPanOrigin.y = rc.top;
    g_fPanning  = 1;
    g_fPanRight = fRight;
    SetCapture(hwnd);
    SetCursor(g_hcurPan);
}

static void EndPan(void)
{
    g_fPanning = 0;
    ReleaseCapture();
    ClearFullscreenClaim();
}

/* is the square under this point already uncovered? */
static BOOL FRevealedAt(int x, int y)
{
    if (g_rgBlk == NULL)
        return FALSE;
    if (x < 1 || y < 1 || x > g_cBlk || y > g_cRow)
        return FALSE;
    return (*PblkAt(x, y) & MASK_VISIT) != 0;
}

/*---------------------------------------------------------------------
    The XYZZY cheat.  Typing X-Y-Z-Z-Y advances g_iXyzzy to 5; pressing
    SHIFT then flips it to 17 (5 ^ 0x14), which arms it - pressing SHIFT
    again disarms it.  While it is armed (or while it sits at 5 and CTRL
    is held down) every mouse move over the field pokes the single pixel
    in the top-left corner of the *screen*: black when the cell under the
    cursor hides a mine, white when it does not.
  -------------------------------------------------------------------*/
static void DoXyzzy(WPARAM wParam, LPARAM lParam)
{
    HDC hdc;

    if (g_iXyzzy < 5)
        return;
    if (g_iXyzzy == 5 && (wParam & MK_CONTROL) == 0)
        return;

    g_xCur = XFromLp(lParam);
    g_yCur = YFromLp(lParam);
    if (g_xCur < 1 || g_yCur < 1 || g_xCur > g_cBlk || g_yCur > g_cRow)
        return;

    hdc = GetDC(NULL);
    SetPixel(hdc, 0, 0, (*PblkAt(g_xCur, g_yCur) & MASK_BOMB)
                        ? RGB(0, 0, 0) : RGB(255, 255, 255));
    ReleaseDC(NULL, hdc);
}

/*---------------------------------------------------------------------
    The ctrl+T peek.  Same question as XYZZY - is the square under the
    cursor a mine - but answered in the corner of the window instead of
    on the desktop: red for yes, green for no.  The block is filled in
    device pixels, deliberately outside the zoomed surface, so it stays
    exactly four screen pixels however far the interface is zoomed in.
  -------------------------------------------------------------------*/
/* Where the block goes: over the whole of the square under the
   cursor.  It used to sit in the corner of the window, which was fine
   while the window was small - but a board can be far wider than the
   screen, and then the corner of the window is off the edge of it and
   the answer is somewhere the player cannot see.  Covering the square
   itself keeps it under their eye wherever on the board they are, and
   at whatever zoom: the block is the square, so it grows and shrinks
   with it. */
static BOOL FPeekRect(LPRECT prc, int x, int y)
{
    if (g_hwnd == NULL || x < 1 || y < 1 || x > g_cBlk || y > g_cRow)
        return FALSE;

    prc->left   = LogToDev(x * DX_BLK - 4);
    prc->top    = LogToDev(y * DY_BLK + 39);
    prc->right  = LogToDev(x * DX_BLK - 4 + DX_BLK);
    prc->bottom = LogToDev(y * DY_BLK + 39 + DY_BLK);
    return TRUE;
}

/* Put back whatever the board had where the block was.  The block is
   placed in device pixels and so need not line up with the 1:1
   surface underneath; the logical rectangle covering it is taken a
   pixel wider all round so no edge of it can survive. */
static void ErasePeek(void)
{
    int xl, yl, xr, yb;

    if (!g_fPeekOn || g_hwnd == NULL)
        return;
    g_fPeekOn = 0;

    xl = ClampInt(DevToLog(g_rcPeekOn.left)   - 1, 0, g_dxClient);
    yl = ClampInt(DevToLog(g_rcPeekOn.top)    - 1, 0, g_dyClient);
    xr = ClampInt(DevToLog(g_rcPeekOn.right)  + 2, xl + 1, g_dxClient);
    yb = ClampInt(DevToLog(g_rcPeekOn.bottom) + 2, yl + 1, g_dyClient);
    Present(xl, yl, xr - xl, yb - yl);
}

static void DrawPeek(void)
{
    RECT   rc;
    HDC    hdc;
    HBRUSH hbr;

    if (!g_fPeek || g_hwnd == NULL || g_rgBlk == NULL)
        return;
    if (!FPeekRect(&rc, g_xPeek, g_yPeek))
        return;

    hdc = GetDC(g_hwnd);
    if (hdc == NULL)
        return;
    hbr = CreateSolidBrush((*PblkAt(g_xPeek, g_yPeek) & MASK_BOMB)
                           ? RGB(255, 0, 0) : RGB(0, 255, 0));
    if (hbr) {
        FillRect(hdc, &rc, hbr);
        DeleteObject(hbr);
    }
    ReleaseDC(g_hwnd, hdc);
    g_rcPeekOn = rc;
    g_fPeekOn  = 1;
}

static void UpdatePeek(LPARAM lParam)
{
    int x, y;

    if (!g_fPeek)
        return;
    x = XFromLp(lParam);
    y = YFromLp(lParam);
    if (x == g_xPeek && y == g_yPeek && g_fPeekOn)
        return;                    /* same square - leave it alone */

    ErasePeek();                   /* lift it off the square it was on */
    g_xPeek = x;
    g_yPeek = y;
    DrawPeek();
}

static void TogglePeek(HWND hwnd)
{
    g_fPeek = !g_fPeek;
    if (g_fPeek) {
        POINT pt;
        if (GetCursorPos(&pt) && ScreenToClient(hwnd, &pt)) {
            g_xPeek = (DevToLog(pt.x) + 4) >> 4;
            g_yPeek = (DevToLog(pt.y) - 0x27) >> 4;
        }
        DrawPeek();
    } else {
        ErasePeek();            /* put the square back as it was */
    }
}

static LRESULT CALLBACK MineWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                    LPARAM lParam)
{
    switch (msg) {

    case WM_DESTROY:
        KillTimer(g_hwnd, ID_TIMER);
        PostQuitMessage(0);
        break;

    case WM_ACTIVATE:
        if (wParam == WA_CLICKACTIVE)
            g_fIgnoreClick = 1;
        break;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        if (FEnsureBack()) {
            if (!g_fBackValid) {
                DrawScreen(g_hdcBack);
                g_fBackValid  = 1;
                g_fBatchField = 0;   /* just drawn, by other means */
            } else {
                /* a dialog can run its own message loop in the middle
                   of a batch - the end of a game asking for a name,
                   say - so make sure the squares are on the surface
                   before any of it is shown */
                FlushBatchField();
            }
            if (g_nZoom == 100) {
                BitBlt(hdc, ps.rcPaint.left, ps.rcPaint.top,
                       ps.rcPaint.right - ps.rcPaint.left,
                       ps.rcPaint.bottom - ps.rcPaint.top,
                       g_hdcBack, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
            } else {
                /* Only the part that needs repainting.  This used to
                   stretch the whole interface across every time, which
                   on a large field meant a quarter of a second of work
                   to put back a single square.  The logical rectangle
                   is taken a pixel wide either side so that rounding
                   cannot leave a seam, and the destination is derived
                   from it so the scaling lines up exactly with the
                   piecemeal copies Present makes. */
                int xl = DevToLog(ps.rcPaint.left);
                int yl = DevToLog(ps.rcPaint.top);
                int xr = DevToLog(ps.rcPaint.right) + 1;
                int yb = DevToLog(ps.rcPaint.bottom) + 1;
                int xd, yd;

                xl = ClampInt(xl - 1, 0, g_dxClient);
                yl = ClampInt(yl - 1, 0, g_dyClient);
                xr = ClampInt(xr + 1, xl + 1, g_dxClient);
                yb = ClampInt(yb + 1, yl + 1, g_dyClient);
                xd = LogToDev(xl);
                yd = LogToDev(yl);

                SetStretchBltMode(hdc, COLORONCOLOR);
                StretchBlt(hdc, xd, yd,
                           LogToDev(xr) - xd, LogToDev(yb) - yd,
                           g_hdcBack, xl, yl, xr - xl, yb - yl, SRCCOPY);
            }
        }
        EndPaint(hwnd, &ps);
        g_fPeekOn = 0;          /* the repaint took the block with it */
        DrawPeek();
        return 0;
    }

    case WM_GETMINMAXINFO: {
        /* Windows caps a window at roughly the size of the work area
           unless it is told otherwise, which clips big custom fields
           along the bottom and right and quietly stops the zoom where
           it was asked to keep going.  It also refuses to go narrow
           enough for a small board zoomed right out, which left a bare
           strip beside the board.  Lift both ends: a board bigger than
           the screen is what the space-bar pan is for. */
        LPMINMAXINFO pmmi = (LPMINMAXINFO)lParam;

        pmmi->ptMaxTrackSize.x = DXY_ZOOM_MAX;
        pmmi->ptMaxTrackSize.y = DXY_ZOOM_MAX;
        pmmi->ptMinTrackSize.x = 1;
        pmmi->ptMinTrackSize.y = 1;
        return 0;
    }

    case WM_MOUSEWHEEL:
        /* ctrl + wheel zooms the whole interface */
        if (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) {
            int nOld  = g_nZoom;
            int nStep = (GET_WHEEL_DELTA_WPARAM(wParam) > 0) ? ZOOM_STEP
                                                             : -ZOOM_STEP;
            int xLog, yLog;
            MSG msgT;

            /* Take every wheel notch already queued and apply them as
               one change.  Resizing a window the size of a large
               board is the expensive part of a zoom, and doing it
               once per notch meant a flick of the wheel queued up
               seconds of work - during which the wheel appears to do
               nothing at all.  Notches without ctrl are left where
               they are; they are not ours. */
            while (PeekMessageW(&msgT, hwnd, WM_MOUSEWHEEL, WM_MOUSEWHEEL,
                                PM_NOREMOVE)) {
                if ((GET_KEYSTATE_WPARAM(msgT.wParam) & MK_CONTROL) == 0)
                    break;
                PeekMessageW(&msgT, hwnd, WM_MOUSEWHEEL, WM_MOUSEWHEEL,
                             PM_REMOVE);
                nStep += (GET_WHEEL_DELTA_WPARAM(msgT.wParam) > 0)
                         ? ZOOM_STEP : -ZOOM_STEP;
                lParam = msgT.lParam;   /* anchor on the latest pointer */
            }

            g_nZoom = ClampInt(nOld + nStep, ZOOM_MIN, ZoomMax());
            if (g_nZoom == nOld)
                return 0;

            /* Zoom about the pointer: work out which square of the
               board it is over, then place the window so that same
               square stays under it afterwards.  Without this the
               top-left corner stays put and everything the player was
               looking at slides away off the bottom right.
               WM_MOUSEWHEEL reports screen coordinates, and
               g_xWindow/g_yWindow are where the client area starts on
               screen, so the difference is the offset into the client. */
            xLog = MulDiv(GET_X_LPARAM(lParam) - g_xWindow, 100, nOld);
            yLog = MulDiv(GET_Y_LPARAM(lParam) - g_yWindow, 100, nOld);
            g_xWindow = GET_X_LPARAM(lParam) - MulDiv(xLog, g_nZoom, 100);
            g_yWindow = GET_Y_LPARAM(lParam) - MulDiv(yLog, g_nZoom, 100);

            AdjustWindow(ADJUST_MOVE | ADJUST_RESCALE);
            ClearFullscreenClaim();
            return 0;
        }
        break;

    case WM_SETCURSOR:
        if ((g_fSpaceDown || g_fPanning) && LOWORD(lParam) == HTCLIENT) {
            SetCursor(g_hcurPan);
            return TRUE;
        }
        break;

    case WM_KILLFOCUS:
        g_fSpaceDown = 0;
        g_fCtrlDown  = 0;
        if (g_fPanning)
            EndPan();
        if (g_fBlockTrack) {
            g_fBlockTrack = 0;
            KillTimer(hwnd, ID_TIMER_AUTO);
        }
        break;

    case WM_KEYUP:
        if (wParam == VK_CONTROL)
            g_fCtrlDown = 0;
        if (wParam == VK_SPACE) {
            g_fSpaceDown = 0;
            if (g_fPanning) {
                g_fPanning = 0;
                ReleaseCapture();
            }
            SetCursor(g_hcurArrow);
        }
        break;

    case WM_MOVE:
        if ((g_fStatus & STATUS_ICON) == 0) {
            g_xWindow = GET_X_LPARAM(lParam);
            g_yWindow = GET_Y_LPARAM(lParam);
        }
        break;

    case WM_TIMER:
        if (wParam == ID_TIMER_AUTO) {
            /* The left button is still down: keep clicking wherever
               the pointer has got to, so it can be swept across the
               board.  The first repeat came after AUTO_DELAY; from
               here on it runs at AUTO_RATE. */
            SetTimer(hwnd, ID_TIMER_AUTO, AUTO_RATE, NULL);

            /* Stop when the button has gone up.  g_fBlockTrack and the
               capture say that directly - GetKeyState would not, since
               it reports a snapshot from the last input message this
               thread took off its own queue. */
            if (!g_fBlockTrack || GetCapture() != hwnd ||
                (g_fStatus & STATUS_PLAY) == 0) {
                KillTimer(hwnd, ID_TIMER_AUTO);
                return 0;
            }
            StartClockIfIdle();
            DoClickAction();
            DisplayButton(g_iButtonCur);
            return 0;
        }
        DoTimer();
        return 0;

    case WM_ENTERMENULOOP:
        g_fInMenu = 1;
        break;

    case WM_EXITMENULOOP:
        g_fInMenu = 0;
        break;

    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_MINIMIZE) {
            PauseGame();
            g_fStatus |= (STATUS_PAUSE | STATUS_ICON);
        } else if ((wParam & 0xFFF0) == SC_RESTORE) {
            g_fStatus &= ~(DWORD)(STATUS_PAUSE | STATUS_ICON);
            ResumeGame();
            g_fIgnoreClick = 0;
        }
        break;

    case WM_KEYDOWN:
        switch (wParam) {
        case VK_SPACE:
            /* the hand tool: hold space, then drag with the left
               button to slide the window around */
            if (!g_fSpaceDown) {
                g_fSpaceDown = 1;
                SetCursor(g_hcurPan);
            }
            break;

        case VK_CONTROL:
            g_fCtrlDown = 1;
            g_iXyzzy = 0;
            break;

        case 'T':
            /* the tracked flag, not GetKeyState: that reports a
               snapshot from when this thread last took an input
               message off its own queue */
            if (g_fCtrlDown || (GetKeyState(VK_CONTROL) & 0x8000)) {
                TogglePeek(hwnd);
                break;
            }
            if (g_iXyzzy < 5)
                g_iXyzzy = (c_szXyzzy[g_iXyzzy] == (WCHAR)wParam)
                           ? g_iXyzzy + 1 : 0;
            break;

        case VK_SHIFT:
            if (g_iXyzzy > 4)
                g_iXyzzy ^= 0x14;
            break;

        case VK_F4:
            if (g_fSound > 1) {
                if (g_fSound == 3) {
                    KillSound();
                    g_fSound = 2;
                } else {
                    g_fSound = FTestSound();
                }
            }
            break;

        case VK_F5:
            if (g_fMenu != 0)
                SetMenuBar(1);
            break;

        case VK_F6:
            if (g_fMenu != 0)
                SetMenuBar(2);
            break;

        default:
            if (g_iXyzzy < 5)
                g_iXyzzy = (c_szXyzzy[g_iXyzzy] == (WCHAR)wParam)
                           ? g_iXyzzy + 1 : 0;
            break;
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_NEW:
            StartGame();
            break;

        case IDM_EXIT:
            ShowWindow(g_hwnd, SW_HIDE);
            SendMessageW(g_hwnd, WM_SYSCOMMAND, SC_CLOSE, 0);
            return 0;

        case IDM_BEGIN:
        case IDM_INTER:
        case IDM_EXPERT:
            g_wGameType = (int)LOWORD(wParam) - IDM_BEGIN;
            g_cMines    = c_rgPreset[g_wGameType][0];
            g_cRowCfg   = c_rgPreset[g_wGameType][1];
            g_cBlkCfg   = c_rgPreset[g_wGameType][2];
            StartGame();
            g_fUpdateReg = 1;
            SetMenuBar((UINT)g_fMenu);
            break;

        case IDM_CUSTOM:
            DoPref();
            break;

        case IDM_SOUND:
            if (g_fSound == 0) {
                g_fSound = FTestSound();
            } else {
                KillSound();
                g_fSound = 0;
            }
            g_fUpdateReg = 1;
            SetMenuBar((UINT)g_fMenu);
            break;

        case IDM_MARK:
            g_fMark = !g_fMark;
            g_fUpdateReg = 1;
            SetMenuBar((UINT)g_fMenu);
            break;

        case IDM_COLOR:
            g_fColor = !g_fColor;
            FreeBmp();
            if (!FLoadBmp()) {
                ReportErr(IDS_ERR_MEMORY);
                ShowWindow(g_hwnd, SW_HIDE);
                SendMessageW(g_hwnd, WM_SYSCOMMAND, SC_CLOSE, 0);
                return 0;
            }
            DisplayScreen();
            g_fUpdateReg = 1;
            SetMenuBar((UINT)g_fMenu);
            break;

        case IDM_BEST:
            DoBestTimes();
            break;

        case IDM_HELP:        DoHelp(3, 0); break;
        case IDM_HELP_SEARCH: DoHelp(1, 2); break;
        case IDM_HELP_USING:  DoHelp(4, 0); break;

        case IDM_ABOUT:
            DoAbout();
            return 0;
        }
        break;

    /*--------------------------------------------------------------*/
    case WM_LBUTTONDOWN:
        if (g_fSpaceDown) {
            BeginPan(hwnd, 0);
            return 0;
        }
        if (g_fIgnoreClick) { g_fIgnoreClick = 0; return 0; }
        if (FButtonHit(lParam))             return 0;
        if ((g_fStatus & STATUS_PLAY) == 0) break;
        g_fChord = ((wParam & (MK_RBUTTON | MK_SHIFT)) != 0);
        StartTracking(hwnd, lParam);
        return 0;

    case WM_MBUTTONDOWN:
        if (g_fIgnoreClick) { g_fIgnoreClick = 0; return 0; }
        if ((g_fStatus & STATUS_PLAY) == 0) break;
        g_fChord = 1;
        StartTracking(hwnd, lParam);
        return 0;

    case WM_RBUTTONDOWN:
        if (g_fIgnoreClick) { g_fIgnoreClick = 0; return 0; }
        /* Right-dragging a square that is already uncovered slides the
           window, the same as space + left-drag.  Right-clicking an
           uncovered square had no meaning before, so nothing is lost -
           covered squares still take a flag.  This runs before the
           game-over check because panning is about looking around, not
           about playing. */
        if (!g_fBlockTrack && !g_fPanning &&
            FRevealedAt(XFromLp(lParam), YFromLp(lParam))) {
            BeginPan(hwnd, 1);
            return 0;
        }
        if ((g_fStatus & STATUS_PLAY) == 0) break;
        if (g_fBlockTrack) {
            TrackMouse(-3, -3);
            g_fChord = 1;
            PostMessageW(g_hwnd, WM_MOUSEMOVE, wParam, lParam);
            return 0;
        }
        if ((wParam & MK_LBUTTON) == 0) {
            if (!g_fInMenu)
                MarkSquare(XFromLp(lParam), YFromLp(lParam));
            return 0;
        }
        StartTracking(hwnd, lParam);
        return 0;

    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
        if (g_fPanning) {
            if ((msg == WM_RBUTTONUP) == (g_fPanRight != 0))
                EndPan();
            return 0;
        }
        if (g_fBlockTrack)
            ReleaseTracking();
        break;

    case WM_MOUSEMOVE:
        if (g_fPanning) {
            POINT pt;
            if (GetCursorPos(&pt)) {
                SetWindowPos(hwnd, NULL,
                             g_ptPanOrigin.x + (pt.x - g_ptPanGrab.x),
                             g_ptPanOrigin.y + (pt.y - g_ptPanGrab.y),
                             0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                /* The shell re-decides whether this is a full-screen
                   app every time the window moves, so the claim has to
                   be renewed as we drag or the taskbar flickers.  It is
                   a cross-process call, so not on every single mouse
                   message - a few times a second is enough to keep the
                   taskbar from ever dropping. */
                if (GetTickCount() - g_msTaskbar >= 100) {
                    g_msTaskbar = GetTickCount();
                    ClearFullscreenClaim();
                }
            }
            return 0;
        }
        UpdatePeek(lParam);
        if (g_fBlockTrack) {
            if ((g_fStatus & STATUS_PLAY) == 0)
                ReleaseTracking();
            else
                TrackMouse(XFromLp(lParam), YFromLp(lParam));
            break;
        }
        if (g_iXyzzy != 0)
            DoXyzzy(wParam, lParam);
        break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/*=====================================================================
    entry point
  =====================================================================*/
static int RunApp(HINSTANCE hInst, int nCmdShow)
{
    WNDCLASSW wc;
    MSG       msg;
    HACCEL    hAccel;
    INITCOMMONCONTROLSEX icc;
    RECT      rc;

    g_hInst = hInst;
    g_nZoom = ZoomForDisplay();
    g_hcurArrow = LoadCursorW(NULL, IDC_ARROW);
    g_hcurPan   = LoadCursorW(NULL, IDC_SIZEALL);
    InitPreferences();

    g_fFrozen = (nCmdShow == SW_SHOWMINIMIZED ||
                 nCmdShow == SW_SHOWMINNOACTIVE);

    icc.dwSize = sizeof(icc);
    icc.dwICC  = 0x000016FD;        /* every ICC_ class we might use */
    InitCommonControlsEx(&icc);

    wc.style         = 0;
    wc.lpfnWndProc   = MineWndProc;
    wc.cbClsExtra    = 0;
    wc.cbWndExtra    = 0;
    wc.hInstance     = g_hInst;
    wc.hIcon         = LoadIconW(g_hInst, MAKEINTRESOURCEW(ID_ICON_MAIN));
    wc.hCursor       = g_hcurArrow;
    wc.hbrBackground = (HBRUSH)GetStockObject(LTGRAY_BRUSH);
    wc.lpszMenuName  = NULL;
    wc.lpszClassName = g_szClass;

    if (!RegisterClassW(&wc))
        return 0;

    g_hMenu = LoadMenuW(g_hInst, MAKEINTRESOURCEW(ID_MENU));
    hAccel  = LoadAcceleratorsW(g_hInst, MAKEINTRESOURCEW(ID_ACCEL));

    ReadPreferences();

    g_dxClient = g_cBlk * DX_BLK + DX_WINDOW;
    g_dyClient = g_cRow * DY_BLK + DY_WINDOW;

    /* The opening zoom follows the display's own scale, which on a
       200% monitor doubles the window - enough, on a large saved
       field, to put it past what the compositor will draw.  The
       field size is only known now, so this is the first point the
       ceiling can be applied; without it the game came up invisible
       and, because the field is remembered, came up invisible again
       every time after that. */
    g_nZoom = ClampInt(g_nZoom, ZOOM_MIN, ZoomMax());

    SetRect(&rc, 0, 0, LogToDev(g_dxClient), LogToDev(g_dyClient));
    AdjustWindowRect(&rc, WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                     (g_fMenu & 1) == 0);

    g_hwnd = CreateWindowExW(0, g_szClass, g_szClass,
                             WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             g_xWindow + rc.left, g_yWindow + rc.top,
                             rc.right - rc.left, rc.bottom - rc.top,
                             NULL, NULL, g_hInst, NULL);
    if (g_hwnd == NULL) {
        ReportErr(1000);
        return 0;
    }

    if (!FLoadBmp()) {
        ReportErr(IDS_ERR_MEMORY);
        return 0;
    }

    SetMenuBar((UINT)g_fMenu);
    StartGame();
    ClearFullscreenClaim();

    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    UpdateWindow(g_hwnd);
    g_fFrozen = 0;

    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (!TranslateAcceleratorW(g_hwnd, hAccel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    FreeBmp();
    FreeBack();
    KillSound();
    if (g_fUpdateReg)
        WritePreferences();

    return (int)msg.wParam;
}

/* nCmdShow the way the loader passed it, without the C runtime's help */
static int RunFromStartupInfo(void)
{
    STARTUPINFOW si;
    int nCmdShow = SW_SHOWNORMAL;

    GetStartupInfoW(&si);
    if (si.dwFlags & STARTF_USESHOWWINDOW)
        nCmdShow = si.wShowWindow;

    return RunApp(GetModuleHandleW(NULL), nCmdShow);
}

/*---------------------------------------------------------------------
    Three ways in, so the same source builds under any of the link
    models build.cmd uses:

      MinerEntry  the raw PE entry point, used when the program is
                  linked with -nostartfiles / -nodefaultlibs.  Nothing
                  here needs the C run-time's start-up code: every
                  global is constant- or zero-initialised, and the two
                  library routines used (memset, rand) are defined in
                  this file.
      WinMain     the MSVC / -mwindows entry.
      main        mingw's default entry (compiled out when MINER_NO_CRT
                  is defined, because gcc makes main() call __main(),
                  which drags the whole start-up machinery back in).
  -------------------------------------------------------------------*/
extern "C" void MinerEntry(void)
{
    ExitProcess((UINT)RunFromStartupInfo());
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine,
                   int nCmdShow)
{
    (void)hPrev; (void)lpCmdLine;
    return RunApp(hInst, nCmdShow);
}

#ifndef MINER_NO_CRT
int main(void)
{
    return RunFromStartupInfo();
}
#endif
