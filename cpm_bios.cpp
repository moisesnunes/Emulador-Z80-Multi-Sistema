// cpm_bios.cpp — CP/M 2.2 BDOS emulation layer
//
// Every CALL 0x0005 in the 8080 program is intercepted by BDOSCall(). Console
// I/O goes through TerminalState (the 80×24 virtual ADM-3A terminal) instead
// of stdout/stdin, so everything appears in the ImGui window rendered by
// DrawTerminal() in GUI.cpp.
//
// Blocking functions (fn 1, fn 10) return without advancing PC/SP when the
// input queue is empty. The main loop in 8080Emulator.cpp retries them every
// iteration, rendering GUI frames in between so keyboard events keep flowing.

#include "cpm_bios.h"
#include <iostream>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <termios.h>

// CP/M SYS attribute is stored as the extended attribute "user.cpm.sys" = "1".
// R/O is mapped to POSIX write-permission bits (chmod) as before.
static void HostSetSys(const std::string &path, bool sys)
{
    if (sys)
        setxattr(path.c_str(), "user.cpm.sys", "1", 1, 0);
    else
        removexattr(path.c_str(), "user.cpm.sys");
}
static bool HostGetSys(const std::string &path)
{
    char v[2] = {};
    return getxattr(path.c_str(), "user.cpm.sys", v, 1) == 1 && v[0] == '1';
}

// ── BDOS error codes (returned in A; H mirrors A on error) ───────────────────
// CP/M 2.2 manual §5, table of BDOS function return values.
static constexpr uint8_t BDOS_OK = 0x00;         // success
static constexpr uint8_t BDOS_UNWRITTEN = 0x01;  // reading unwritten / unallocated data
static constexpr uint8_t BDOS_DISK_ERROR = 0x02; // physical I/O error / disk subsystem error
static constexpr uint8_t BDOS_CANT_CLOSE = 0x03; // cannot close current extent
static constexpr uint8_t BDOS_SEEK_UNW = 0x04;   // random read: seeking to unwritten extent
static constexpr uint8_t BDOS_DIR_FULL = 0x05;   // directory overflow during write
static constexpr uint8_t BDOS_REC_OVF = 0x06;    // random write: record count overflow (R2 != 0)
static constexpr uint8_t BDOS_WRITE_PROT = 0x07; // write-protected drive
static constexpr uint8_t BDOS_EOF = 0xFF;        // sequential read: physical end of file

// ── FCB field offsets ────────────────────────────────────────────────────────
static constexpr int FCB_DRIVE = 0;
static constexpr int FCB_NAME = 1; // 8 bytes, space-padded, uppercase
static constexpr int FCB_EXT = 9;  // 3 bytes, space-padded, uppercase
static constexpr int FCB_EX = 12;  // extent number (low byte)
static constexpr int FCB_S2 = 14;  // extent number (high byte)
static constexpr int FCB_RC = 15;
static constexpr int FCB_CR = 32; // current record within extent

// ── Open-file table ──────────────────────────────────────────────────────────
static constexpr int MAX_OPEN_FILES = 16;
struct OpenFile
{
    FILE *fp = nullptr;
    DskImage *dsk = nullptr; // non-null for DSK image files
    bool readOnly = false;
    char name[11] = {};      // 11-char space-padded CP/M name (DSK only)
    char hostPath[512] = {}; // host filesystem path (non-DSK files)
};
static OpenFile openFiles[MAX_OPEN_FILES];

static int AllocFileSlot()
{
    for (int i = 1; i < MAX_OPEN_FILES; i++)
        if (!openFiles[i].fp && !openFiles[i].dsk)
            return i;
    return 0;
}

// Find an already-open host-file slot by absolute path (for re-open rewind).
static int FindOpenSlotByPath(const char *path)
{
    for (int i = 1; i < MAX_OPEN_FILES; i++)
        if (openFiles[i].fp && strcmp(openFiles[i].hostPath, path) == 0)
            return i;
    return 0;
}

// Set FCB_RC to the number of 128-byte records in fp, capped at 128 for files >= 16 KB.
// Whitesmith C, LK80 linker, and Aztec C depend on this being set after open/make/write.
static void FCBSetRecordCount(intel8080 *cpu, uint16_t fcbAddr, FILE *fp)
{
    if (!fp)
        return;
    long cur = ftell(fp);
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, cur, SEEK_SET);
    if (size >= 16 * 1024)
        cpu->memory[fcbAddr + FCB_RC] = 128;
    else
        cpu->memory[fcbAddr + FCB_RC] = (uint8_t)((size + 127) / 128);
}

// ── Drive validation ─────────────────────────────────────────────────────────

// A drive is valid if it has an open DSK image OR if its host directory exists.
static bool DriveIsValid(const CPMState &cpm, uint8_t drv)
{
    if (drv >= MAX_DRIVES)
        return false;
    if (cpm.diskImages[drv] && cpm.diskImages[drv]->isOpen())
        return true;
    if (cpm.diskDirs[drv].empty())
        return false;
    struct stat st;
    return stat(cpm.diskDirs[drv].c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// ── DSK helpers ───────────────────────────────────────────────────────────────

// Return the DskImage for the drive referenced by FCB byte 0.
static DskImage *FCBGetDsk(intel8080 *cpu, CPMState &cpm, uint16_t fcb)
{
    uint8_t drv = cpu->memory[fcb];
    int d = (drv == 0) ? cpm.currentDrive : (int)(drv - 1);
    if (d < 0 || d >= MAX_DRIVES)
        d = 0;
    DskImage *dsk = cpm.diskImages[d];
    return (dsk && dsk->isOpen()) ? dsk : nullptr;
}

// Extract FCB bytes 1–11 as uppercase 11-char CP/M name (preserves '?' wildcards).
static void FCBGetName(intel8080 *cpu, uint16_t fcb, char name[11])
{
    for (int i = 0; i < 11; i++)
        name[i] = (char)toupper((unsigned char)(cpu->memory[fcb + 1 + i] & 0x7F));
}

// Scan the DSK directory for an entry matching (user, name[11], extentNum).
// Pass extentNum = -1 to match any extent (first found).
static int DskFindEntry(DskImage *dsk, uint8_t user, const char name[11], int extNum, int s2 = -1)
{
    uint8_t e[32];
    for (int i = 0; i < dsk->dirEntries(); i++)
    {
        dsk->dskRead(dsk->dirByteOff(i), e, 32);
        if (e[0] == 0xE5)
            continue;
        if (e[0] != user)
            continue;
        if (extNum >= 0 && e[12] != (uint8_t)extNum)
            continue;
        if (s2 >= 0 && e[14] != (uint8_t)s2)
            continue;
        bool ok = true;
        for (int j = 0; j < 11; j++)
            if ((e[1 + j] & 0x7F) != (uint8_t)name[j])
            {
                ok = false;
                break;
            }
        if (ok)
            return i;
    }
    return -1;
}

// Allocate a free directory slot (user byte = 0xE5).  Returns index or -1.
static int DskAllocDir(DskImage *dsk)
{
    uint8_t e[32];
    for (int i = 0; i < dsk->dirEntries(); i++)
    {
        dsk->dskRead(dsk->dirByteOff(i), e, 32);
        if (e[0] == 0xE5)
            return i;
    }
    return -1;
}

// Find a free data block by scanning all directory block pointers.
// Returns block number, or -1 if the disk is full.
static int DskAllocBlock(DskImage *dsk)
{
    int dirBlks = ((dsk->drm + 1) * 32 + dsk->bytesPerBlock() - 1) / dsk->bytesPerBlock();
    std::vector<bool> used(dsk->dsm + 1, false);
    for (int i = 0; i < dirBlks; i++)
        used[i] = true;

    uint8_t e[32];
    int ptrs = dsk->blockPtrs();
    for (int i = 0; i < dsk->dirEntries(); i++)
    {
        dsk->dskRead(dsk->dirByteOff(i), e, 32);
        if (e[0] == 0xE5)
            continue;
        for (int j = 0; j < ptrs; j++)
        {
            int blk = e[16 + j];
            if (blk > 0 && blk <= dsk->dsm)
                used[blk] = true;
        }
    }
    for (int i = dirBlks; i <= dsk->dsm; i++)
        if (!used[i])
            return i;
    return -1;
}

// Read 128-byte record from DSK at sequential position (ex, s2, cr) for file name[11].
static uint8_t DskReadRec(DskImage *dsk, uint8_t user, const char name[11],
                          uint8_t ex, uint8_t s2, uint8_t cr, uint8_t buf[128])
{
    int idx = DskFindEntry(dsk, user, name, (int)ex, (int)s2);
    if (idx < 0)
        return 0x01; // EOF: extent not found

    uint8_t e[32];
    dsk->dskRead(dsk->dirByteOff(idx), e, 32);

    int rpb = dsk->recsPerBlock();
    int bi = cr / rpb; // block pointer index (0–15)
    int ri = cr % rpb; // record within block (0–7)
    if (bi >= dsk->blockPtrs())
        return 0x01;

    uint8_t blk = e[16 + bi];
    if (blk == 0)
        return 0x01; // unallocated = EOF

    memset(buf, 0x1A, 128);
    dsk->dskRead(dsk->recByteOff(blk, ri), buf, 128);
    return 0x00;
}

// Write 128-byte record to DSK, allocating directory entries and blocks as needed.
static uint8_t DskWriteRec(DskImage *dsk, uint8_t user, const char name[11],
                           uint8_t ex, uint8_t s2, uint8_t cr, const uint8_t buf[128])
{
    if (dsk->readOnly)
        return BDOS_EOF; // 0xFF — callers map this to BDOS_WRITE_PROT

    int idx = DskFindEntry(dsk, user, name, (int)ex, (int)s2);
    uint8_t e[32];
    bool dirty = false;

    if (idx < 0)
    {
        idx = DskAllocDir(dsk);
        if (idx < 0)
            return 0x02; // directory full
        memset(e, 0, 32);
        e[0] = user;
        for (int j = 0; j < 11; j++)
            e[1 + j] = (uint8_t)name[j];
        e[12] = ex;
        e[14] = s2;
        dirty = true;
    }
    else
    {
        dsk->dskRead(dsk->dirByteOff(idx), e, 32);
    }

    int rpb = dsk->recsPerBlock();
    int bi = cr / rpb;
    int ri = cr % rpb;
    if (bi >= dsk->blockPtrs())
        return 0x02;

    uint8_t blk = e[16 + bi];
    if (blk == 0)
    {
        int nb = DskAllocBlock(dsk);
        if (nb < 0)
            return 0x02; // disk full
        e[16 + bi] = (uint8_t)nb;
        blk = (uint8_t)nb;
        dirty = true;
    }

    // RC = record count in the last used block of this extent.
    uint8_t rc = (uint8_t)(ri + 1);
    if (rc > e[15])
    {
        e[15] = rc;
        dirty = true;
    }

    if (dirty)
        dsk->dskWrite(dsk->dirByteOff(idx), e, 32);

    dsk->dskWrite(dsk->recByteOff(blk, ri), buf, 128);
    dsk->cache.flush();
    return 0x00;
}

// ── DskMount / DskUnmount ─────────────────────────────────────────────────────

// Known CP/M disk formats identified by file size.
// All images are treated as flat arrays of 128-byte logical sectors regardless
// of the original physical sector size (same convention used by cpmtools/z80pack).
// Values match the CP/M 2.2 DPB for each format.
static const struct KnownFmt
{
    long size;
    DskGeometry geo;
    const char *label;
} kFormats[] = {
    // IBM 3740 8" single-density — the CP/M 2.2 reference format
    // 77T × 26S × 128B = 256 256 bytes
    {256256L, {26, 3, 242, 63, 2, {}}, "IBM 8\" SD"},
    // IBM 8" double-density, single-sided
    // 77T × 26 phys × 256B → SPT=52 logical, 512 512 bytes
    {512512L, {52, 4, 242, 127, 2, {}}, "IBM 8\" DD/SS"},
    // IBM 8" double-density, double-sided (two-sided)
    // 154T × 52 logical × 128B = 1 025 024 bytes
    {1025024L, {52, 4, 493, 255, 2, {}}, "IBM 8\" DD/DS"},
    // Osborne 1 single-density
    // 40T × 10 phys × 256B → SPT=20 logical, 102 400 bytes
    {102400L, {20, 3, 91, 63, 3, {}}, "Osborne 1 SD"},
    // Kaypro II / 2'84 single-density (NOTE: same size as Osborne 1 DD —
    // use a .geo sidecar if the auto-detect picks the wrong format)
    // 40T × 10 phys × 512B → SPT=40 logical, 204 800 bytes
    {204800L, {40, 3, 194, 63, 1, {}}, "Kaypro II (5.25\" SD)"},
    // Kaypro 4 / 10 double-density
    // 80T × 10 phys × 512B → SPT=40 logical, 409 600 bytes
    {409600L, {40, 4, 196, 127, 1, {}}, "Kaypro 4 (5.25\" DD)"},
};
static constexpr int kFormatsCount = (int)(sizeof(kFormats) / sizeof(kFormats[0]));

// Parse a <path>.geo sidecar file into geo.  Returns true if the file exists.
// Format: one "key=value" per line.  Unrecognised keys are silently ignored.
// skew= takes a comma-separated list of physical sector indices, e.g.
//   skew=0,6,12,18,24,4,10,16,22,2,8,14,20,1,7,13,19,25,5,11,17,23,3,9,15,21
static bool ParseGeoFile(const std::string &imagePath, DskGeometry &geo)
{
    std::string geoPath = imagePath + ".geo";
    FILE *f = fopen(geoPath.c_str(), "r");
    if (!f)
        return false;

    char line[512];
    while (fgets(line, (int)sizeof(line), f))
    {
        char key[64] = {}, val[448] = {};
        if (sscanf(line, " %63[^= ] = %447[^\n\r]", key, val) != 2)
            continue;
        std::string k(key), v(val);
        if (k == "spt")
            geo.spt = atoi(v.c_str());
        else if (k == "bsh")
            geo.bsh = atoi(v.c_str());
        else if (k == "dsm")
            geo.dsm = atoi(v.c_str());
        else if (k == "drm")
            geo.drm = atoi(v.c_str());
        else if (k == "off")
            geo.off = atoi(v.c_str());
        else if (k == "skew")
        {
            geo.skew.clear();
            char *p = val;
            while (*p)
            {
                while (*p == ' ' || *p == ',')
                    ++p;
                if (!*p)
                    break;
                geo.skew.push_back(atoi(p));
                while (*p && *p != ',' && *p != ' ')
                    ++p;
            }
        }
    }
    fclose(f);
    return true;
}

bool DskMountWithGeometry(CPMState &cpm, int drive, const std::string &hostPath,
                          const DskGeometry &geo, bool readOnly)
{
    if (drive < 0 || drive >= MAX_DRIVES)
        return false;
    DskUnmount(cpm, drive);

    FILE *fp = readOnly ? nullptr : fopen(hostPath.c_str(), "rb+");
    bool ro = readOnly;
    if (!fp)
    {
        fp = fopen(hostPath.c_str(), "rb");
        ro = true;
    }
    if (!fp)
        return false;

    DskImage *dsk = new DskImage;
    dsk->fp = fp;
    dsk->readOnly = ro;
    dsk->path = hostPath;
    dsk->spt = geo.spt;
    dsk->bsh = geo.bsh;
    dsk->dsm = geo.dsm;
    dsk->drm = geo.drm;
    dsk->off = geo.off;
    dsk->skewTable = geo.skew;

    cpm.diskImages[drive] = dsk;
    return true;
}

bool DskMount(CPMState &cpm, int drive, const std::string &hostPath, bool readOnly)
{
    DskGeometry geo; // IBM 8" SD defaults

    // 1. Try sidecar <image>.geo — highest priority.
    if (!ParseGeoFile(hostPath, geo))
    {
        // 2. Auto-detect by file size.
        FILE *tmp = fopen(hostPath.c_str(), "rb");
        if (!tmp)
            return false;
        fseek(tmp, 0, SEEK_END);
        long sz = ftell(tmp);
        fclose(tmp);

        for (int i = 0; i < kFormatsCount; i++)
        {
            if (kFormats[i].size == sz)
            {
                geo = kFormats[i].geo;
                break;
            }
        }
        // 3. Fallback: defaults already set (IBM 8" SD).
    }

    return DskMountWithGeometry(cpm, drive, hostPath, geo, readOnly);
}

void DskUnmount(CPMState &cpm, int drive)
{
    if (drive < 0 || drive >= MAX_DRIVES)
        return;
    DskImage *dsk = cpm.diskImages[drive];
    if (!dsk)
        return;

    // Clear all open-file slots that reference this image.
    for (int i = 1; i < MAX_OPEN_FILES; i++)
        if (openFiles[i].dsk == dsk)
            openFiles[i] = {};

    // Remove their fcbSlotMap entries.
    std::vector<uint16_t> stale;
    for (auto &[addr, slot] : cpm.fcbSlotMap)
        if (slot > 0 && slot < MAX_OPEN_FILES && openFiles[slot].dsk == nullptr && cpm.fcbSlotMap.count(addr))
            stale.push_back(addr);
    // Simpler: just scan all slots for orphaned entries.
    stale.clear();
    for (auto &[addr, slot] : cpm.fcbSlotMap)
        if (slot > 0 && slot < MAX_OPEN_FILES &&
            !openFiles[slot].fp && !openFiles[slot].dsk)
            stale.push_back(addr);
    for (uint16_t a : stale)
        cpm.fcbSlotMap.erase(a);

    if (dsk->fp)
    {
        dsk->cache.flush();
        fclose(dsk->fp);
        dsk->fp = nullptr;
    }
    delete dsk;
    cpm.diskImages[drive] = nullptr;
}

void DskUnmountAll(CPMState &cpm)
{
    for (int i = 0; i < MAX_DRIVES; i++)
        DskUnmount(cpm, i);
}

// ── TerminalState ────────────────────────────────────────────────────────────

TerminalState::TerminalState() { clear(); }

void TerminalState::clear()
{
    for (int r = 0; r < ROWS; r++)
        memset(buffer[r], ' ', COLS);
    cursorX = cursorY = 0;
    escState = EscState::NORMAL;
    ansiParamCount = 0;
    ansiParamPending = false;
    ansiPrivate = false;
}

// Scroll the display up one line.
static void TermScroll(TerminalState &t)
{
    for (int r = 0; r < TerminalState::ROWS - 1; r++)
        memcpy(t.buffer[r], t.buffer[r + 1], TerminalState::COLS);
    memset(t.buffer[TerminalState::ROWS - 1], ' ', TerminalState::COLS);
    t.cursorY = TerminalState::ROWS - 1;
}

// Write one byte to the terminal, advancing the cursor and interpreting
// ADM-3A and ANSI/VT100 (CSI) escape sequences.
void TerminalState::putChar(char ch)
{
    uint8_t b = (uint8_t)ch;

    // ── Escape sequence continuation ─────────────────────────────────────
    switch (escState)
    {
    // ── ESC followed by a single letter ──────────────────────────────
    case EscState::ESC:
        switch (ch)
        {
        case '[':
            // ANSI/VT100 CSI introducer (all terminal types)
            ansiParamCount = 0;
            ansiParamPending = false;
            for (int i = 0; i < MAX_PARAMS; i++)
                ansiParams[i] = 0;
            escState = EscState::ESC_BRACKET;
            return;
        case '=':
            // ADM-3A / IBM 3101 alternate keypad / cursor-address introducer
            escState = EscState::ESC_EQ;
            return;
        case 'A':
            if (cursorY > 0)
                cursorY--;
            break;
        case 'B':
            if (cursorY < ROWS - 1)
                cursorY++;
            break;
        case 'C':
            if (cursorX < COLS - 1)
                cursorX++;
            break;
        case 'D':
            if (cursorX > 0)
                cursorX--;
            break;
        case 'E':
            clear();
            break;
        case 'H':
            cursorX = cursorY = 0;
            break;
        case 'I': // VT52 / Visual 200 / IBM 3101: reverse index (scroll down, cursor stays)
            if (termType == TermType::IBM3101 || termType == TermType::VISUAL200)
            {
                if (cursorY == 0)
                {
                    for (int r = ROWS - 1; r > 0; r--)
                        memcpy(buffer[r], buffer[r - 1], COLS);
                    memset(buffer[0], ' ', COLS);
                }
                else
                {
                    cursorY--;
                }
            }
            break;
        case 'J':
            // erase from cursor to end of screen (all terminals)
            memset(buffer[cursorY] + cursorX, ' ', COLS - cursorX);
            for (int r = cursorY + 1; r < ROWS; r++)
                memset(buffer[r], ' ', COLS);
            break;
        case 'K':
        case 'T':
            memset(buffer[cursorY] + cursorX, ' ', COLS - cursorX);
            break;
        case 'L': // VT52 / Visual 200 / IBM 3101: insert line at cursor row
            if (termType == TermType::IBM3101 || termType == TermType::VISUAL200)
            {
                for (int r = ROWS - 1; r > cursorY; r--)
                    memcpy(buffer[r], buffer[r - 1], COLS);
                memset(buffer[cursorY], ' ', COLS);
            }
            break;
        case 'M': // VT52 / Visual 200 / IBM 3101: delete line at cursor row
            if (termType == TermType::IBM3101 || termType == TermType::VISUAL200)
            {
                for (int r = cursorY; r < ROWS - 1; r++)
                    memcpy(buffer[r], buffer[r + 1], COLS);
                memset(buffer[ROWS - 1], ' ', COLS);
            }
            break;
        case 'Y':
            if (termType == TermType::IBM3101 || termType == TermType::VISUAL200)
            {
                // VT52-style cursor address: ESC Y (row+0x1F) (col+0x1F), 1-based
                escState = EscState::ESC_Y;
                return;
            }
            // ADM-3A: ESC Y = erase from cursor to end of screen
            memset(buffer[cursorY] + cursorX, ' ', COLS - cursorX);
            for (int r = cursorY + 1; r < ROWS; r++)
                memset(buffer[r], ' ', COLS);
            break;
        case 'O': // SS3 introducer (VT100 application mode)
            escState = EscState::ESC_O;
            return;
        case '7': // DECSC — save cursor position (VT100)
            savedCursorX = cursorX;
            savedCursorY = cursorY;
            break;
        case '8': // DECRC — restore cursor position (VT100)
            cursorX = savedCursorX;
            cursorY = savedCursorY;
            break;
        case 'c': // RIS — Reset to Initial State (VT100)
            clear();
            cursorX = cursorY = savedCursorX = savedCursorY = 0;
            break;
        case 'Z': // identify terminal — ignore
            break;
        case '*': // IBM 3101: clear display
        case ';': // IBM 3101: clear display (alternate)
            if (termType == TermType::IBM3101)
                clear();
            break;
        case '3':
        case '4':
        case '5':
        case '6':
        case 'x':
        case 'y': // Visual 200 / IBM 3101 video attributes — ignore
            break;
        default:
            break;
        }
        escState = EscState::NORMAL;
        return;

    // ── ADM-3A: ESC = row col cursor positioning ──────────────────────
    case EscState::ESC_EQ:
        escRow = std::max(0, (int)(b - 32));
        escState = EscState::ESC_EQ_ROW;
        return;
    case EscState::ESC_EQ_ROW:
        cursorY = std::min(escRow, ROWS - 1);
        cursorX = std::clamp((int)(b - 32), 0, COLS - 1);
        escState = EscState::NORMAL;
        return;

    // ── VT52 / Visual 200 / IBM 3101: ESC Y row col cursor positioning ──
    // Encoding: 1-based position + 0x1F, so 0x20 (space) = row/col 1.
    case EscState::ESC_Y:
        escRow = std::max(0, (int)b - 0x20); // 0-based row
        escState = EscState::ESC_Y_ROW;
        return;
    case EscState::ESC_Y_ROW:
        cursorY = std::min(escRow, ROWS - 1);
        cursorX = std::clamp((int)b - 0x20, 0, COLS - 1);
        escState = EscState::NORMAL;
        return;

    // ── VT100 SS3: ESC O {final} — application keypad/cursor/function keys ──
    case EscState::ESC_O:
        // Consume the final char; key injection is handled on input side.
        escState = EscState::NORMAL;
        return;

    // ── ANSI/VT100: ESC [ {params} {final} ───────────────────────────
    case EscState::ESC_BRACKET:
    {
        if (b >= '0' && b <= '9')
        {
            if (!ansiParamPending)
            {
                if (ansiParamCount < MAX_PARAMS)
                    ansiParamCount++;
                ansiParamPending = true;
            }
            ansiParams[ansiParamCount - 1] =
                ansiParams[ansiParamCount - 1] * 10 + (b - '0');
            return;
        }
        if (b == ';')
        {
            if (!ansiParamPending && ansiParamCount < MAX_PARAMS)
                ansiParamCount++; // empty field → implicit 0
            ansiParamPending = false;
            if (ansiParamCount < MAX_PARAMS)
                ansiParamCount++;
            return;
        }
        if (b == '?' || b == '>' || b == '=')
        {
            ansiPrivate = true; // DEC/private introducer — params still accumulate
            return;
        }

        // Final byte — commit and dispatch.
        ansiParamPending = false;

        // p1/p2: default to 1 (most cursor commands). p1z: raw 0-default for mode selects.
        int p1 = (ansiParamCount >= 1 && ansiParams[0] != 0) ? ansiParams[0] : 1;
        int p2 = (ansiParamCount >= 2 && ansiParams[1] != 0) ? ansiParams[1] : 1;
        int p1z = (ansiParamCount >= 1) ? ansiParams[0] : 0;

        switch (ch)
        {
        case 'H': // CUP — cursor position (1-based row, col)
        case 'f':
            cursorY = std::clamp(p1 - 1, 0, ROWS - 1);
            cursorX = std::clamp(p2 - 1, 0, COLS - 1);
            break;
        case 'A': // CUU — cursor up N
            cursorY = std::max(cursorY - p1, 0);
            break;
        case 'B': // CUD — cursor down N
            cursorY = std::min(cursorY + p1, ROWS - 1);
            break;
        case 'C': // CUF — cursor right N
            cursorX = std::min(cursorX + p1, COLS - 1);
            break;
        case 'D': // CUB — cursor left N
            cursorX = std::max(cursorX - p1, 0);
            break;
        case 'G': // CHA — cursor horizontal absolute (1-based)
            cursorX = std::clamp(p1 - 1, 0, COLS - 1);
            break;
        case 'd': // VPA — cursor vertical absolute (1-based)
            cursorY = std::clamp(p1 - 1, 0, ROWS - 1);
            break;
        case 'J': // ED — erase in display
            if (p1z == 0)
            {
                memset(buffer[cursorY] + cursorX, ' ', COLS - cursorX);
                for (int r = cursorY + 1; r < ROWS; r++)
                    memset(buffer[r], ' ', COLS);
            }
            else if (p1z == 1)
            {
                for (int r = 0; r < cursorY; r++)
                    memset(buffer[r], ' ', COLS);
                memset(buffer[cursorY], ' ', cursorX + 1);
            }
            else
            {
                clear();
            }
            break;
        case 'K': // EL — erase in line
            if (p1z == 0)
                memset(buffer[cursorY] + cursorX, ' ', COLS - cursorX);
            else if (p1z == 1)
                memset(buffer[cursorY], ' ', cursorX + 1);
            else
                memset(buffer[cursorY], ' ', COLS);
            break;
        case 'L': // IL — insert N lines (scroll region down)
        {
            int n = std::min(p1, ROWS - cursorY);
            for (int r = ROWS - 1; r >= cursorY + n; r--)
                memcpy(buffer[r], buffer[r - n], COLS);
            for (int r = cursorY; r < cursorY + n && r < ROWS; r++)
                memset(buffer[r], ' ', COLS);
            break;
        }
        case 'M': // DL — delete N lines (scroll region up)
        {
            int n = std::min(p1, ROWS - cursorY);
            for (int r = cursorY; r < ROWS - n; r++)
                memcpy(buffer[r], buffer[r + n], COLS);
            for (int r = ROWS - n; r < ROWS; r++)
                memset(buffer[r], ' ', COLS);
            break;
        }
        case 'P': // DCH — delete N characters at cursor
        {
            int n = std::min(p1, COLS - cursorX);
            memmove(buffer[cursorY] + cursorX,
                    buffer[cursorY] + cursorX + n,
                    COLS - cursorX - n);
            memset(buffer[cursorY] + COLS - n, ' ', n);
            break;
        }
        case '@': // ICH — insert N blank characters at cursor
        {
            int n = std::min(p1, COLS - cursorX);
            memmove(buffer[cursorY] + cursorX + n,
                    buffer[cursorY] + cursorX,
                    COLS - cursorX - n);
            memset(buffer[cursorY] + cursorX, ' ', n);
            break;
        }
        case 'E': // CNL — cursor next line
            cursorY = std::min(cursorY + p1, ROWS - 1);
            cursorX = 0;
            break;
        case 'F': // CPL — cursor prev line
            cursorY = std::max(cursorY - p1, 0);
            cursorX = 0;
            break;
        case 'S': // SU — scroll up N lines
        {
            int n = std::min(p1, ROWS);
            for (int r = 0; r < ROWS - n; r++)
                memcpy(buffer[r], buffer[r + n], COLS);
            for (int r = ROWS - n; r < ROWS; r++)
                memset(buffer[r], ' ', COLS);
            break;
        }
        case 'T': // SD — scroll down N lines
        {
            int n = std::min(p1, ROWS);
            for (int r = ROWS - 1; r >= n; r--)
                memcpy(buffer[r], buffer[r - n], COLS);
            for (int r = 0; r < n; r++)
                memset(buffer[r], ' ', COLS);
            break;
        }
        case 'X': // ECH — erase N characters at cursor (no cursor move)
        {
            int n = std::min(p1, COLS - cursorX);
            memset(buffer[cursorY] + cursorX, ' ', n);
            break;
        }
        case 'n': // DSR — Device Status Report
            if (p1z == 6)
            {
                // CPR: cursor position report → inject ESC [ row ; col R into input queue
                char resp[32];
                snprintf(resp, sizeof(resp), "\x1B[%d;%dR", cursorY + 1, cursorX + 1);
                for (const char *p = resp; *p; p++)
                    inputQueue.push_back((uint8_t)*p);
            }
            break;
        // Ignored: color/attributes, mode set/reset, scroll region, cursor save/restore
        case 'm':
        case 'h':
        case 'l':
        case 'r':
        case 's':
        case 'u':
            break;
        default:
            break;
        }
        ansiPrivate = false;
        escState = EscState::NORMAL;
        return;
    }

    default:
        break;
    }

    // ── Control codes ─────────────────────────────────────────────────────
    switch (b)
    {
    case 0x1B: // ESC — start of escape sequence
        escState = EscState::ESC;
        return;
    case 0x09: // HT — horizontal tab to next 8-column stop
        cursorX = std::min((cursorX + 8) & ~7, COLS - 1);
        return;
    case 0x0D: // CR
        cursorX = 0;
        return;
    case 0x0A: // LF — scroll if at bottom
        cursorY++;
        if (cursorY >= ROWS)
            TermScroll(*this);
        return;
    case 0x08: // BS — cursor left
        if (cursorX > 0)
            cursorX--;
        return;
    case 0x0B: // ^K — cursor up (ADM-3A)
        if (cursorY > 0)
            cursorY--;
        return;
    case 0x18: // ^X — cursor down (WordStar)
        if (cursorY < ROWS - 1)
            cursorY++;
        return;
    case 0x0C: // ^L — clear screen
        clear();
        return;
    case 0x1A: // ^Z — clear screen
        clear();
        return;
    case 0x19: // ^Y — erase to end of line
        memset(buffer[cursorY] + cursorX, ' ', COLS - cursorX);
        return;
    case 0x07: // ^G — bell, ignored
        return;
    default:
        break;
    }

    // ── Printable character ───────────────────────────────────────────────
    // Strip bit 7: WordStar/DataStar set it for reverse-video highlighting.
    uint8_t glyph = b & 0x7F;
    if (glyph >= 0x20 && glyph < 0x7F)
    {
        buffer[cursorY][cursorX] = (char)glyph;
        if (++cursorX >= COLS)
        {
            cursorX = 0;
            cursorY++;
            if (cursorY >= ROWS)
                TermScroll(*this);
        }
    }
}

// ── FCB helpers ──────────────────────────────────────────────────────────────

// Resolve the host directory for a given FCB: byte 0 == 0 → current drive,
// 1–4 → A:–D: explicitly.  All BDOS file functions must use this so that
// programs which hard-code a drive in the FCB (e.g. CC.COM writing to B:)
// land in the correct host subdirectory instead of always in currentDiskDir().
static const std::string &FCBDiskDir(intel8080 *cpu, CPMState &cpm, uint16_t fcbAddr)
{
    uint8_t drv = cpu->memory[fcbAddr];
    if (drv == 0)
        return cpm.currentDiskDir();
    return cpm.driveDir(drv - 1);
}

static std::string FCBToHostName(intel8080 *cpu, uint16_t fcbAddr)
{
    char name[9] = {}, ext[4] = {};
    for (int i = 0; i < 8; i++)
        name[i] = cpu->memory[fcbAddr + FCB_NAME + i] & 0x7F;
    for (int i = 0; i < 3; i++)
        ext[i] = cpu->memory[fcbAddr + FCB_EXT + i] & 0x7F;
    std::string sName(name), sExt(ext);
    while (!sName.empty() && sName.back() == ' ')
        sName.pop_back();
    while (!sExt.empty() && sExt.back() == ' ')
        sExt.pop_back();
    return sExt.empty() ? sName : sName + "." + sExt;
}

static long FCBFileOffset(intel8080 *cpu, uint16_t fcbAddr)
{
    uint32_t extent = (uint32_t)cpu->memory[fcbAddr + FCB_S2] * 32u + cpu->memory[fcbAddr + FCB_EX];
    return ((long)extent * 128 + cpu->memory[fcbAddr + FCB_CR]) * 128;
}

static void FCBAdvanceRecord(intel8080 *cpu, uint16_t fcbAddr)
{
    uint8_t &cr = cpu->memory[fcbAddr + FCB_CR];
    if (++cr >= 128)
    {
        cr = 0;
        if (++cpu->memory[fcbAddr + FCB_EX] >= 32)
        {
            cpu->memory[fcbAddr + FCB_EX] = 0;
            cpu->memory[fcbAddr + FCB_S2]++;
        }
    }
}

// ── Simulated serial port ─────────────────────────────────────────────────────

void SerialTick(CPMState &cpm)
{
    SimSerial &s = cpm.serial;
    if (!s.enabled())
        return;

    // ── Console mode: stdin → rxBuf, txBuf → stdout ───────────────────────
    if (s.consoleMode)
    {
        if (!s.consoleInitialized)
        {
            // Save terminal state and switch to raw non-blocking mode.
            tcgetattr(STDIN_FILENO, &s.consoleSavedTermios);
            struct termios raw = s.consoleSavedTermios;
            raw.c_iflag &= ~(ICRNL | IXON);
            raw.c_lflag &= ~(ECHO | ICANON); // keep ISIG so Ctrl+C works
            raw.c_cc[VMIN]  = 0;
            raw.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
            s.consoleInitialized = true;
        }

        // stdin → rxBuf
        uint8_t buf[64];
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n > 0)
            for (ssize_t i = 0; i < n; i++)
                s.rxBuf.push_back(buf[i]);

        // txBuf → stdout
        if (!s.txBuf.empty())
        {
            std::vector<uint8_t> out(s.txBuf.begin(), s.txBuf.end());
            ssize_t written = write(STDOUT_FILENO, out.data(), out.size());
            if (written > 0)
                s.txBuf.erase(s.txBuf.begin(), s.txBuf.begin() + written);
            fflush(stdout);
        }
        return;
    }

    // Get current time in milliseconds for baud rate throttling.
    static uint64_t prevTimeMs = 0;
    static bool timeInitialized = false;
    if (!timeInitialized)
    {
        prevTimeMs = (uint64_t)(std::time(nullptr) * 1000);
        timeInitialized = true;
    }
    uint64_t nowMs = (uint64_t)(std::time(nullptr) * 1000);
    if (nowMs < prevTimeMs)
        nowMs = prevTimeMs; // handle clock drift / overflow

    // Calculate bytes-per-millisecond based on baud rate.
    // 9600 baud = 1200 bytes/sec = 1.2 bytes/ms
    // Safe to allow ~1 byte per ms in most cases.
    double bytesPerMs = (double)s.baud / 8000.0; // 8000 bits per ms at full throttle
    int maxBytesPerMs = std::max(1, (int)(bytesPerMs + 0.5));

    // ── FIFO Mode ────────────────────────────────────────────────────────
    if (!s.fifoPath.empty())
    {
        // Initialize FIFO on first call.
        if (s.fifoFd < 0)
        {
            // Try to open the FIFO for non-blocking I/O.
            // Note: FIFOs require both read and write ends; we open in O_RDWR
            // to avoid blocking if the other side hasn't connected yet.
            int fd = open(s.fifoPath.c_str(), O_RDWR | O_NONBLOCK);
            if (fd >= 0)
            {
                s.fifoFd = fd;
                s.fifoReadEnd = true;
                s.fifoWriteEnd = true;
                s.rxBuf.clear();
                s.txBuf.clear();
            }
            // If open fails, keep trying on next tick.
        }

        if (s.fifoFd >= 0)
        {
            // Receive: FIFO → rxBuf (with throttling).
            uint8_t buf[256];
            ssize_t n = read(s.fifoFd, buf, sizeof(buf));
            if (n > 0)
            {
                // Add bytes to receive queue, respecting baud rate limit.
                for (ssize_t i = 0; i < n; i++)
                {
                    s.rxBuf.push_back(buf[i]);
                }
            }
            else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            {
                // FIFO error or EOF.
                close(s.fifoFd);
                s.fifoFd = -1;
                s.rxBuf.clear();
                s.txBuf.clear();
                return;
            }

            // Send: txBuf → FIFO (with throttling).
            // Only send up to maxBytesPerMs per tick to respect baud rate.
            if (!s.txBuf.empty())
            {
                // Reset byte counter each millisecond.
                if (nowMs > s.lastByteTimeMs)
                {
                    s.lastByteTimeMs = nowMs;
                    s.bytesThisMs = 0;
                }

                // Send bytes up to the throttle limit.
                int toSend = std::min((int)s.txBuf.size(), maxBytesPerMs - s.bytesThisMs);
                for (int i = 0; i < toSend; i++)
                {
                    uint8_t b = s.txBuf.front();
                    ssize_t written = write(s.fifoFd, &b, 1);
                    if (written > 0)
                    {
                        s.txBuf.pop_front();
                        s.bytesThisMs++;
                    }
                    else
                    {
                        // Can't write now (FIFO full?); stop trying this frame.
                        break;
                    }
                }
            }
        }
        return;
    }

    // ── TCP Mode (original implementation) ─────────────────────────────────
    // Start listening on first call.
    if (s.listenFd < 0)
    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return;
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        fcntl(fd, F_SETFL, O_NONBLOCK);
        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(s.port);
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
            listen(fd, 1) < 0)
        {
            close(fd);
            return;
        }
        s.listenFd = fd;
    }

    // Accept a new client if none connected.
    if (s.clientFd < 0)
    {
        struct sockaddr_in addr;
        socklen_t len = sizeof(addr);
        int fd = accept(s.listenFd, (struct sockaddr *)&addr, &len);
        if (fd >= 0)
        {
            fcntl(fd, F_SETFL, O_NONBLOCK);
            s.clientFd = fd;
            s.rxBuf.clear(); // discard stale input from previous session
        }
    }

    if (s.clientFd < 0)
        return;

    // Receive: client → rxBuf.
    uint8_t buf[256];
    ssize_t n = recv(s.clientFd, buf, sizeof(buf), 0);
    if (n > 0)
    {
        for (ssize_t i = 0; i < n; i++)
            s.rxBuf.push_back(buf[i]);
    }
    else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
    {
        close(s.clientFd);
        s.clientFd = -1;
        s.rxBuf.clear();
        s.txBuf.clear();
        return;
    }

    // Send: txBuf → client.
    if (!s.txBuf.empty())
    {
        if (s.baud > 0)
        {
            // Throttled: respect baud rate limit.
            if (nowMs > s.lastByteTimeMs)
            {
                s.lastByteTimeMs = nowMs;
                s.bytesThisMs = 0;
            }
            int toSend = std::min((int)s.txBuf.size(), maxBytesPerMs - s.bytesThisMs);
            std::vector<uint8_t> sendBuf(s.txBuf.begin(), s.txBuf.begin() + toSend);
            ssize_t sent = send(s.clientFd, sendBuf.data(), sendBuf.size(), MSG_NOSIGNAL);
            if (sent > 0)
            {
                s.txBuf.erase(s.txBuf.begin(), s.txBuf.begin() + sent);
                s.bytesThisMs += (int)sent;
            }
            else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            {
                close(s.clientFd);
                s.clientFd = -1;
            }
        }
        else
        {
            // No throttle (baud=0): flush entire buffer at once.
            std::vector<uint8_t> sendBuf(s.txBuf.begin(), s.txBuf.end());
            ssize_t sent = send(s.clientFd, sendBuf.data(), sendBuf.size(), MSG_NOSIGNAL);
            if (sent > 0)
                s.txBuf.erase(s.txBuf.begin(), s.txBuf.begin() + sent);
            else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            {
                close(s.clientFd);
                s.clientFd = -1;
            }
        }
    }
}

void SerialClose(CPMState &cpm)
{
    if (cpm.serial.clientFd >= 0)
    {
        close(cpm.serial.clientFd);
        cpm.serial.clientFd = -1;
    }
    if (cpm.serial.listenFd >= 0)
    {
        close(cpm.serial.listenFd);
        cpm.serial.listenFd = -1;
    }
    if (cpm.serial.fifoFd >= 0)
    {
        close(cpm.serial.fifoFd);
        cpm.serial.fifoFd = -1;
    }
    if (cpm.serial.consoleMode && cpm.serial.consoleInitialized)
    {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &cpm.serial.consoleSavedTermios);
        cpm.serial.consoleInitialized = false;
    }
}

// ── CPMInit ──────────────────────────────────────────────────────────────────

void CPMInit(intel8080 *cpu, CPMState &cpm, const std::string &diskDir)
{
    cpm.diskDirs[0] = diskDir;
    for (int i = 1; i < MAX_DRIVES; i++)
    {
        char ltr[2] = {(char)('a' + i), '\0'};
        cpm.diskDirs[i] = diskDir + "/" + ltr;
    }
    // auto-create B:, C:, D: subdirectories (same behaviour as before).
    for (int i = 1; i <= 3; i++)
        mkdir(cpm.diskDirs[i].c_str(), 0755);
    cpm.dmaAddress = 0x0080;
    cpm.currentDrive = 0;
    cpm.currentUser = 0;
    cpm.running = true;
    cpm.ccpMode = false;
    cpm.ccpRunning = false;
    cpm.ccpPrompted = false;
    cpm.ccpLine.clear();
    cpm.fcbSlotMap.clear();

    // 0x0000: JMP BIOS_ADDR (WBOOT) — warm-boot vector.
    // 0x0001-0x0002 holds the BIOS base address so programs that do
    // LHLD 0x0001 to locate the BIOS jump table find the real stubs.
    cpu->memory[0x0000] = 0xC3;
    cpu->memory[0x0001] = (uint8_t)(BIOS_ADDR & 0xFF);
    cpu->memory[0x0002] = (uint8_t)(BIOS_ADDR >> 8);

    // BIOS stub table: 16 entries × 3 bytes each, each slot starts with RET
    // (intercepted by BIOSCall before execution).
    // Order: WBOOT CONST CONIN CONOUT LIST PUNCH READER HOME
    //        SELDSK SETTRK SETSEC SETDMA READ WRITE LISTST SECTRAN
    for (int i = 0; i < 16; i++)
        cpu->memory[BIOS_ADDR + i * 3] = 0xC9; // RET

    // 0x0005: BDOS entry stub — RET, intercepted before execution.
    cpu->memory[0x0005] = 0xC9;
    // 0x0006-0x0007: BDOS module address (little-endian).
    // Programs read LHLD 0x0006 to determine top of TPA.
    cpu->memory[0x0006] = (uint8_t)(BDOS_ADDR & 0xFF);
    cpu->memory[0x0007] = (uint8_t)(BDOS_ADDR >> 8);

    // RET at BDOS_ADDR catches programs that CALL there directly.
    cpu->memory[BDOS_ADDR] = 0xC9;

    // ── Fake disk data structures ─────────────────────────────────────────
    // DPB (Disk Parameter Block) at DPB_ADDR — standard 8-inch SD parameters.
    uint8_t *dpb = &cpu->memory[DPB_ADDR];
    dpb[0] = 26;
    dpb[1] = 0; // SPT: 26 sectors/track
    dpb[2] = 3; // BSH: block shift (512-byte blocks)
    dpb[3] = 7; // BLM: block mask
    dpb[4] = 0; // EXM: extent mask
    dpb[5] = 242;
    dpb[6] = 0; // DSM: 243 blocks
    dpb[7] = 63;
    dpb[8] = 0;     // DRM: 64 directory entries
    dpb[9] = 0xC0;  // AL0: first 2 blocks reserved
    dpb[10] = 0x00; // AL1
    dpb[11] = 16;
    dpb[12] = 0; // CKS: 16 checksum entries
    dpb[13] = 2;
    dpb[14] = 0; // OFF: 2 reserved tracks

    // DPH (Disk Parameter Header) at DPH_ADDR.
    auto w = [&](uint16_t addr, uint16_t val)
    {
        cpu->memory[addr] = (uint8_t)(val & 0xFF);
        cpu->memory[addr + 1] = (uint8_t)(val >> 8);
    };
    w(DPH_ADDR + 0, 0x0000); // XLT: no translation
    w(DPH_ADDR + 2, 0x0000); // scratch words
    w(DPH_ADDR + 4, 0x0000);
    w(DPH_ADDR + 6, 0x0000);
    w(DPH_ADDR + 8, DIRBUF_ADDR); // DIRBUF
    w(DPH_ADDR + 10, DPB_ADDR);   // DPB
    w(DPH_ADDR + 12, 0x0000);     // CSV: 0 = fixed media
    w(DPH_ADDR + 14, ALV_ADDR);   // ALV: allocation vector

    // ALV: all zeros = all blocks free (we use host filesystem, not blocks).
    memset(&cpu->memory[ALV_ADDR], 0, 32);

    // ── Zero page defaults ────────────────────────────────────────────────
    memset(&cpu->memory[0x005C], 0, 36);
    memset(&cpu->memory[0x005D], ' ', 11);
    memset(&cpu->memory[0x006C], 0, 36);
    memset(&cpu->memory[0x006D], ' ', 11);
    cpu->memory[0x0080] = 0x00; // empty command tail

    cpu->PC = 0x0100;
    cpu->SP = BDOS_ADDR - 2;
    cpu->memory[BDOS_ADDR - 2] = 0x00; // return address → warm-boot vector
    cpu->memory[BDOS_ADDR - 1] = 0x00;
}

void CPMCloseAllFiles(CPMState &cpm)
{
    for (auto &[addr, slot] : cpm.fcbSlotMap)
    {
        if (slot > 0 && slot < MAX_OPEN_FILES && openFiles[slot].fp)
        {
            fclose(openFiles[slot].fp);
            openFiles[slot] = {};
        }
    }
    cpm.fcbSlotMap.clear();

    if (cpm.readerFp)
    {
        fclose(cpm.readerFp);
        cpm.readerFp = nullptr;
    }
    if (cpm.punchFp)
    {
        fclose(cpm.punchFp);
        cpm.punchFp = nullptr;
    }
    if (cpm.printerFp)
    {
        fclose(cpm.printerFp);
        cpm.printerFp = nullptr;
    }
}

// ── BDOS function implementations ────────────────────────────────────────────

static void BDOS_SystemReset(intel8080 * /*cpu*/, CPMState &cpm)
{
    cpm.terminal.putChar('\r');
    cpm.terminal.putChar('\n');
    if (cpm.ccpMode)
    {
        cpm.ccpRunning = true;
        cpm.ccpPrompted = false;
    }
    else
    {
        cpm.running = false;
    }
}

static void BDOS_ConsoleInput(intel8080 *cpu, CPMState &cpm)
{
    // Queue non-empty guaranteed by the early-return guard in BDOSCall.
    uint8_t ch = cpm.terminal.inputQueue.front();
    cpm.terminal.inputQueue.pop_front();
    cpu->A = ch;
    cpm.terminal.putChar((char)ch); // BDOS echoes the character
}

static void BDOS_ConsoleOutput(intel8080 *cpu, CPMState &cpm)
{
    cpm.consoleOut((char)cpu->E);
}

// Direct Console I/O (fn 6):
//   E = 0xFF → non-blocking read (returns 0 if queue empty)
//   E = 0xFE → console status (0xFF=ready, 0x00=not ready)
//   E = other → write character
static void BDOS_DirectConsoleIO(intel8080 *cpu, CPMState &cpm)
{
    if (cpu->E == 0xFF)
    {
        if (cpm.terminal.inputQueue.empty())
        {
            cpu->A = 0x00;
        }
        else
        {
            cpu->A = cpm.terminal.inputQueue.front();
            cpm.terminal.inputQueue.pop_front();
        }
    }
    else if (cpu->E == 0xFE)
    {
        cpu->A = cpm.terminal.inputQueue.empty() ? 0x00 : 0xFF;
    }
    else
    {
        cpm.consoleOut((char)cpu->E);
    }
}

static void BDOS_PrintString(intel8080 *cpu, CPMState &cpm)
{
    uint16_t addr = ((uint16_t)cpu->D << 8) | cpu->E;
    size_t printed = 0;
    const size_t MAX_PRINT = 4096;

    while (addr < 65536 && cpu->memory[addr] != '$' && printed < MAX_PRINT)
    {
        cpm.consoleOut((char)cpu->memory[addr++]);
        printed++;
    }
}

static void BDOS_ConsoleStatus(intel8080 *cpu, CPMState &cpm)
{
    cpu->A = cpm.terminal.inputQueue.empty() ? 0x00 : 0xFF;
}

static void BDOS_Version(intel8080 *cpu, CPMState & /*cpm*/)
{
    cpu->H = 0x00;
    cpu->L = 0x22;
    cpu->A = 0x22; // CP/M 2.2
}

static void BDOS_ResetDisk(intel8080 * /*cpu*/, CPMState &cpm)
{
    cpm.dmaAddress = 0x0080;
    cpm.currentDrive = 0;
    cpm.writeProtectedDrives = 0; // reset clears all write-protects
}

static void BDOS_SelectDisk(intel8080 *cpu, CPMState &cpm)
{
    uint8_t drv = cpu->E & 0x0F;
    if (!DriveIsValid(cpm, drv))
    {
        cpu->A = 0xFF;
        return;
    }
    cpm.currentDrive = drv;
    cpu->H = cpu->L = cpu->A = 0x00;
}

static void BDOS_OpenFile(intel8080 *cpu, CPMState &cpm)
{
    uint16_t fcbAddr = ((uint16_t)cpu->D << 8) | cpu->E;
    DskImage *dsk = FCBGetDsk(cpu, cpm, fcbAddr);
    if (dsk)
    {
        char name[11];
        FCBGetName(cpu, fcbAddr, name);
        int entIdx = DskFindEntry(dsk, (uint8_t)cpm.currentUser, name, 0);
        if (entIdx < 0)
        {
            cpu->H = 0x01;
            cpu->A = 0xFF;
            return;
        } // file not found
        uint8_t e[32];
        dsk->dskRead(dsk->dirByteOff(entIdx), e, 32);
        int slot = AllocFileSlot();
        if (!slot)
        {
            cpu->H = 0xFF;
            cpu->A = 0xFF;
            return;
        } // no FCB slots
        openFiles[slot].fp = nullptr;
        openFiles[slot].dsk = dsk;
        openFiles[slot].readOnly = dsk->readOnly || (e[9] & 0x80) != 0;
        memcpy(openFiles[slot].name, name, 11);
        cpm.fcbSlotMap[fcbAddr] = slot;
        cpu->memory[fcbAddr + FCB_EX] = cpu->memory[fcbAddr + FCB_CR] = 0;
        cpu->memory[fcbAddr + FCB_EXT] = (cpu->memory[fcbAddr + FCB_EXT] & 0x7F) | (e[9] & 0x80);
        cpu->memory[fcbAddr + FCB_EXT + 1] = (cpu->memory[fcbAddr + FCB_EXT + 1] & 0x7F) | (e[10] & 0x80);
        cpu->A = 0x00;
        return;
    }
    std::string path = FCBDiskDir(cpu, cpm, fcbAddr) + "/" + FCBToHostName(cpu, fcbAddr);

    // If this path is already open (e.g. ASM.COM opens the same file twice), rewind and reuse.
    int slot = FindOpenSlotByPath(path.c_str());
    if (slot)
    {
        fseek(openFiles[slot].fp, 0, SEEK_SET);
        cpm.fcbSlotMap[fcbAddr] = slot;
        cpu->memory[fcbAddr + FCB_EX] = cpu->memory[fcbAddr + FCB_CR] = 0;
        cpu->memory[fcbAddr + FCB_S2] = 0;
        FCBSetRecordCount(cpu, fcbAddr, openFiles[slot].fp);
        cpu->A = 0x00;
        return;
    }

    slot = AllocFileSlot();
    if (!slot)
    {
        cpu->H = 0xFF;
        cpu->A = 0xFF; // no FCB slots
        return;
    }
    FILE *fp = fopen(path.c_str(), "rb+");
    bool ro = false;
    if (!fp)
    {
        fp = fopen(path.c_str(), "rb");
        ro = true;
    }
    if (!fp)
    {
        cpu->H = 0x01;
        cpu->A = 0xFF; // file not found
        return;
    }
    openFiles[slot].fp = fp;
    openFiles[slot].dsk = nullptr;
    openFiles[slot].readOnly = ro;
    strncpy(openFiles[slot].hostPath, path.c_str(), sizeof(openFiles[slot].hostPath) - 1);
    cpm.fcbSlotMap[fcbAddr] = slot;
    cpu->memory[fcbAddr + FCB_EX] = cpu->memory[fcbAddr + FCB_CR] = 0;
    cpu->memory[fcbAddr + FCB_S2] = 0;
    if (ro)
        cpu->memory[fcbAddr + FCB_EXT + 0] |= 0x80;
    else
        cpu->memory[fcbAddr + FCB_EXT + 0] &= ~0x80;
    if (HostGetSys(path))
        cpu->memory[fcbAddr + FCB_EXT + 1] |= 0x80;
    else
        cpu->memory[fcbAddr + FCB_EXT + 1] &= ~0x80;
    FCBSetRecordCount(cpu, fcbAddr, fp);
    cpu->A = 0x00;
}

static void BDOS_CloseFile(intel8080 *cpu, CPMState &cpm)
{
    uint16_t fcbAddr = ((uint16_t)cpu->D << 8) | cpu->E;
    auto it = cpm.fcbSlotMap.find(fcbAddr);
    if (it == cpm.fcbSlotMap.end())
    {
        // Pro Pascal v2.1 closes files that exist but were never opened by this FCB.
        std::string path = FCBDiskDir(cpu, cpm, fcbAddr) + "/" + FCBToHostName(cpu, fcbAddr);
        struct stat st;
        if (stat(path.c_str(), &st) == 0)
            cpu->A = 0x00; // file exists — treat as success
        else
        {
            cpu->H = 0xFF;
            cpu->A = 0xFF;
        } // file not found
        return;
    }
    int slot = it->second;
    if (slot > 0 && slot < MAX_OPEN_FILES)
    {
        if (openFiles[slot].fp)
        {
            fclose(openFiles[slot].fp);
        }
        // DSK files: image FILE* stays open; just clear the slot.
        openFiles[slot] = {};
    }
    cpm.fcbSlotMap.erase(it);
    cpu->A = 0x00;
}

// Returns true if `filename` (uppercase, dot-separated) matches the FCB wildcard pattern.
// The FCB name/ext fields use '?' as a single-character wildcard.
static bool FCBMatchesFile(intel8080 *cpu, uint16_t fcbAddr, const std::string &filename)
{
    auto dot = filename.rfind('.');
    std::string namePart = (dot != std::string::npos) ? filename.substr(0, dot) : filename;
    std::string extPart = (dot != std::string::npos) ? filename.substr(dot + 1) : "";
    while (namePart.size() < 8)
        namePart += ' ';
    while (extPart.size() < 3)
        extPart += ' ';
    if (namePart.size() > 8 || extPart.size() > 3)
        return false;
    for (int i = 0; i < 8; i++)
    {
        char p = (char)(cpu->memory[fcbAddr + FCB_NAME + i] & 0x7F);
        if (p == '?')
            continue;
        if ((unsigned char)namePart[i] != (unsigned char)toupper((unsigned char)p))
            return false;
    }
    for (int i = 0; i < 3; i++)
    {
        char p = (char)(cpu->memory[fcbAddr + FCB_EXT + i] & 0x7F);
        if (p == '?')
            continue;
        if ((unsigned char)extPart[i] != (unsigned char)toupper((unsigned char)p))
            return false;
    }
    return true;
}

// Write a 32-byte CP/M directory entry for `filename` into the DMA buffer and DIRBUF.
// attr: bit0 = R/O (sets bit 7 of ext[0]), bit1 = SYS (sets bit 7 of ext[1]).
static void FillDMAWithEntry(intel8080 *cpu, CPMState &cpm, const std::string &filename, uint8_t attr = 0)
{
    auto dot = filename.rfind('.');
    std::string namePart = (dot != std::string::npos) ? filename.substr(0, dot) : filename;
    std::string extPart = (dot != std::string::npos) ? filename.substr(dot + 1) : "";
    uint8_t *dma = &cpu->memory[cpm.dmaAddress];
    memset(dma, 0, 32);
    dma[0] = 0x00; // user 0, active
    for (int i = 0; i < 8; i++)
        dma[1 + i] = (i < (int)namePart.size()) ? (uint8_t)namePart[i] : ' ';
    for (int i = 0; i < 3; i++)
        dma[9 + i] = (i < (int)extPart.size()) ? (uint8_t)extPart[i] : ' ';
    if (attr & 0x01)
        dma[9] |= 0x80; // R/O flag
    if (attr & 0x02)
        dma[10] |= 0x80; // SYS flag
    // Mirror into DIRBUF so programs that read it directly get valid data.
    if (cpm.dmaAddress != DIRBUF_ADDR)
        memcpy(&cpu->memory[DIRBUF_ADDR], dma, 32);
}

static void BDOS_SearchFirst(intel8080 *cpu, CPMState &cpm)
{
    uint16_t fcbAddr = ((uint16_t)cpu->D << 8) | cpu->E;
    SearchContext &ctx = cpm.searchContexts[fcbAddr];
    ctx.results.clear();
    ctx.currentIndex = 0;

    DskImage *dsk = FCBGetDsk(cpu, cpm, fcbAddr);
    if (dsk)
    {
        char pat[11];
        FCBGetName(cpu, fcbAddr, pat);
        uint8_t e[32];
        for (int i = 0; i < dsk->dirEntries(); i++)
        {
            dsk->dskRead(dsk->dirByteOff(i), e, 32);
            if (e[0] == 0xE5 || e[0] != (uint8_t)cpm.currentUser)
                continue;
            if (e[12] != 0)
                continue; // only first extent
            bool match = true;
            for (int j = 0; j < 11; j++)
                if (pat[j] != '?' && (e[1 + j] & 0x7F) != (uint8_t)pat[j])
                {
                    match = false;
                    break;
                }
            if (!match)
                continue;
            char nm[9] = {}, ex[4] = {};
            for (int j = 0; j < 8; j++)
                nm[j] = (char)(e[1 + j] & 0x7F);
            for (int j = 0; j < 3; j++)
                ex[j] = (char)(e[9 + j] & 0x7F);
            std::string sn(nm), se(ex);
            while (!sn.empty() && sn.back() == ' ')
                sn.pop_back();
            while (!se.empty() && se.back() == ' ')
                se.pop_back();
            uint8_t fileAttr = 0;
            if (e[9] & 0x80)
                fileAttr |= 0x01; // R/O
            if (e[10] & 0x80)
                fileAttr |= 0x02; // SYS
            ctx.results.push_back({se.empty() ? sn : sn + "." + se, fileAttr});
        }
    }
    else
    {
        const std::string &dirPath = FCBDiskDir(cpu, cpm, fcbAddr);
        DIR *dir = opendir(dirPath.c_str());
        if (!dir)
        {
            cpm.searchContexts.erase(fcbAddr);
            cpu->H = 0xFF;
            cpu->A = 0xFF; // cannot open directory
            return;
        }
        struct dirent *ent;
        while ((ent = readdir(dir)) != nullptr)
        {
            if (ent->d_name[0] == '.')
                continue;
            std::string upper = ent->d_name;
            for (char &c : upper)
                c = (char)toupper((unsigned char)c);
            if (!FCBMatchesFile(cpu, fcbAddr, upper))
                continue;
            std::string fullPath = dirPath + "/" + ent->d_name;
            struct stat fst;
            uint8_t fileAttr = 0;
            if (stat(fullPath.c_str(), &fst) == 0 && !(fst.st_mode & S_IWUSR))
                fileAttr |= 0x01; // R/O
            if (HostGetSys(fullPath))
                fileAttr |= 0x02; // SYS
            ctx.results.push_back({upper, fileAttr});
        }
        closedir(dir);
    }

    if (ctx.results.empty())
    {
        cpm.searchContexts.erase(fcbAddr);
        cpu->H = 0x01;
        cpu->A = 0xFF; // no files found
        return;
    }
    const auto &first = ctx.results[ctx.currentIndex++];
    FillDMAWithEntry(cpu, cpm, first.first, first.second);
    cpu->A = 0x00;
}

static void BDOS_SearchNext(intel8080 *cpu, CPMState &cpm)
{
    uint16_t fcbAddr = ((uint16_t)cpu->D << 8) | cpu->E;
    auto it = cpm.searchContexts.find(fcbAddr);
    if (it == cpm.searchContexts.end() || it->second.currentIndex >= (int)it->second.results.size())
    {
        cpu->H = 0x01;
        cpu->A = 0xFF; // no more files
        return;
    }
    SearchContext &ctx = it->second;
    const auto &next = ctx.results[ctx.currentIndex++];
    FillDMAWithEntry(cpu, cpm, next.first, next.second);
    cpu->A = 0x00;
}

static void BDOS_ReadSequential(intel8080 *cpu, CPMState &cpm)
{
    uint16_t fcbAddr = ((uint16_t)cpu->D << 8) | cpu->E;
    auto it = cpm.fcbSlotMap.find(fcbAddr);
    int slot = (it != cpm.fcbSlotMap.end()) ? it->second : 0;
    if (slot <= 0)
    {
        cpu->H = 0x06;
        cpu->A = 0x09;
        return;
    } // FCB not open

    if (openFiles[slot].dsk)
    {
        uint8_t buf[128];
        cpu->A = DskReadRec(openFiles[slot].dsk, (uint8_t)cpm.currentUser,
                            openFiles[slot].name,
                            cpu->memory[fcbAddr + FCB_EX],
                            cpu->memory[fcbAddr + FCB_S2],
                            cpu->memory[fcbAddr + FCB_CR], buf);
        if (cpu->A == 0x00)
        {
            memcpy(&cpu->memory[cpm.dmaAddress], buf, 128);
            FCBAdvanceRecord(cpu, fcbAddr);
        }
        else
        {
            cpu->H = cpu->A; // 0x01 = EOF
        }
        return;
    }

    if (!openFiles[slot].fp)
    {
        cpu->H = 0x06;
        cpu->A = 0x09;
        return;
    } // FCB not open
    fseek(openFiles[slot].fp, FCBFileOffset(cpu, fcbAddr), SEEK_SET);
    uint8_t buf[128];
    memset(buf, 0x1A, 128);
    size_t nread = fread(buf, 1, 128, openFiles[slot].fp);
    if (nread == 0)
    {
        uint8_t err = ferror(openFiles[slot].fp) ? BDOS_DISK_ERROR : BDOS_EOF;
        cpu->H = cpu->A = err;
        return;
    }
    memcpy(&cpu->memory[cpm.dmaAddress], buf, 128);
    FCBAdvanceRecord(cpu, fcbAddr);
    cpu->A = 0x00;
}

static void BDOS_WriteSequential(intel8080 *cpu, CPMState &cpm)
{
    uint16_t fcbAddr = ((uint16_t)cpu->D << 8) | cpu->E;
    auto it = cpm.fcbSlotMap.find(fcbAddr);
    int slot = (it != cpm.fcbSlotMap.end()) ? it->second : 0;
    if (slot <= 0)
    {
        cpu->H = 0x06;
        cpu->A = 0x09;
        return;
    } // FCB not open

    if (openFiles[slot].dsk)
    {
        cpu->A = DskWriteRec(openFiles[slot].dsk, (uint8_t)cpm.currentUser,
                             openFiles[slot].name,
                             cpu->memory[fcbAddr + FCB_EX],
                             cpu->memory[fcbAddr + FCB_S2],
                             cpu->memory[fcbAddr + FCB_CR],
                             &cpu->memory[cpm.dmaAddress]);
        if (cpu->A == 0x00)
            FCBAdvanceRecord(cpu, fcbAddr);
        else
            cpu->H = (cpu->A == BDOS_EOF) ? BDOS_WRITE_PROT : cpu->A; // 0xFF=R/O, else pass through
        return;
    }

    if (!openFiles[slot].fp)
    {
        cpu->H = 0x06;
        cpu->A = 0x09;
        return;
    } // FCB not open
    if (openFiles[slot].readOnly)
    {
        cpu->H = cpu->A = BDOS_WRITE_PROT;
        return;
    }
    fseek(openFiles[slot].fp, FCBFileOffset(cpu, fcbAddr), SEEK_SET);
    if (fwrite(&cpu->memory[cpm.dmaAddress], 1, 128, openFiles[slot].fp) < 128)
    {
        cpu->H = cpu->A = BDOS_DISK_ERROR;
        return;
    }
    fflush(openFiles[slot].fp);
    FCBAdvanceRecord(cpu, fcbAddr);
    FCBSetRecordCount(cpu, fcbAddr, openFiles[slot].fp); // Whitesmith C A80.COM requires this
    cpu->A = 0x00;
}

static void BDOS_MakeFile(intel8080 *cpu, CPMState &cpm)
{
    uint16_t fcbAddr = ((uint16_t)cpu->D << 8) | cpu->E;
    DskImage *dsk = FCBGetDsk(cpu, cpm, fcbAddr);
    if (dsk)
    {
        if (dsk->readOnly)
        {
            cpu->H = BDOS_WRITE_PROT;
            cpu->A = BDOS_EOF;
            return;
        } // write-protected
        char name[11];
        FCBGetName(cpu, fcbAddr, name);
        // Erase any existing entries for this filename first.
        uint8_t e[32];
        for (int i = 0; i < dsk->dirEntries(); i++)
        {
            dsk->dskRead(dsk->dirByteOff(i), e, 32);
            if (e[0] == 0xE5 || e[0] != (uint8_t)cpm.currentUser)
                continue;
            bool match = true;
            for (int j = 0; j < 11; j++)
                if ((e[1 + j] & 0x7F) != (uint8_t)name[j])
                {
                    match = false;
                    break;
                }
            if (match)
            {
                e[0] = 0xE5;
                dsk->dskWrite(dsk->dirByteOff(i), e, 32);
            }
        }
        int idx = DskAllocDir(dsk);
        if (idx < 0)
        {
            cpu->H = BDOS_DIR_FULL;
            cpu->A = BDOS_EOF;
            return;
        } // directory full
        memset(e, 0, 32);
        e[0] = (uint8_t)cpm.currentUser;
        for (int j = 0; j < 11; j++)
            e[1 + j] = (uint8_t)name[j];
        dsk->dskWrite(dsk->dirByteOff(idx), e, 32);
        dsk->cache.flush();
        int slot = AllocFileSlot();
        if (!slot)
        {
            cpu->H = 0xFF;
            cpu->A = 0xFF;
            return;
        } // no FCB slots
        openFiles[slot].fp = nullptr;
        openFiles[slot].dsk = dsk;
        openFiles[slot].readOnly = false;
        memcpy(openFiles[slot].name, name, 11);
        cpm.fcbSlotMap[fcbAddr] = slot;
        cpu->memory[fcbAddr + FCB_EX] = cpu->memory[fcbAddr + FCB_CR] = 0;
        cpu->A = 0x00;
        return;
    }
    std::string path = FCBDiskDir(cpu, cpm, fcbAddr) + "/" + FCBToHostName(cpu, fcbAddr);
    int slot = AllocFileSlot();
    if (!slot)
    {
        cpu->H = 0xFF;
        cpu->A = 0xFF; // no FCB slots
        return;
    }
    FILE *fp = fopen(path.c_str(), "wb+");
    if (!fp)
    {
        cpu->H = 0xFF;
        cpu->A = 0xFF; // cannot create file
        return;
    }
    openFiles[slot].fp = fp;
    openFiles[slot].dsk = nullptr;
    openFiles[slot].readOnly = false;
    strncpy(openFiles[slot].hostPath, path.c_str(), sizeof(openFiles[slot].hostPath) - 1);
    cpm.fcbSlotMap[fcbAddr] = slot;
    cpu->memory[fcbAddr + FCB_EX] = cpu->memory[fcbAddr + FCB_CR] = 0;
    cpu->memory[fcbAddr + FCB_S2] = 0;
    cpu->memory[fcbAddr + FCB_RC] = 0;
    cpu->A = 0x00;
}

static void BDOS_EraseFile(intel8080 *cpu, CPMState &cpm)
{
    uint16_t fcbAddr = ((uint16_t)cpu->D << 8) | cpu->E;
    DskImage *dsk = FCBGetDsk(cpu, cpm, fcbAddr);
    if (dsk)
    {
        if (dsk->readOnly)
        {
            cpu->H = BDOS_WRITE_PROT;
            cpu->A = BDOS_EOF;
            return;
        } // write-protected
        char pat[11];
        FCBGetName(cpu, fcbAddr, pat);
        uint8_t e[32];
        bool erased = false;
        for (int i = 0; i < dsk->dirEntries(); i++)
        {
            dsk->dskRead(dsk->dirByteOff(i), e, 32);
            if (e[0] == 0xE5 || e[0] != (uint8_t)cpm.currentUser)
                continue;
            bool match = true;
            for (int j = 0; j < 11; j++)
                if (pat[j] != '?' && (e[1 + j] & 0x7F) != (uint8_t)pat[j])
                {
                    match = false;
                    break;
                }
            if (match)
            {
                e[0] = 0xE5;
                dsk->dskWrite(dsk->dirByteOff(i), e, 32);
                erased = true;
            }
        }
        if (erased)
            dsk->cache.flush();
        if (!erased)
            cpu->H = 0x01; // file not found
        cpu->A = erased ? 0x00 : 0xFF;
        return;
    }
    std::string path = FCBDiskDir(cpu, cpm, fcbAddr) + "/" + FCBToHostName(cpu, fcbAddr);
    if (remove(path.c_str()) != 0)
    {
        cpu->H = 0x01;
        cpu->A = 0xFF;
    } // file not found
    else
        cpu->A = 0x00;
}

// Rename FCB layout: bytes 1-11 = old name, bytes 17-27 = new name.
static void BDOS_RenameFile(intel8080 *cpu, CPMState &cpm)
{
    uint16_t fcbAddr = ((uint16_t)cpu->D << 8) | cpu->E;
    DskImage *dsk = FCBGetDsk(cpu, cpm, fcbAddr);
    if (dsk)
    {
        if (dsk->readOnly)
        {
            cpu->H = BDOS_WRITE_PROT;
            cpu->A = BDOS_EOF;
            return;
        } // write-protected
        char oldName[11], newName[11];
        FCBGetName(cpu, fcbAddr, oldName);
        FCBGetName(cpu, fcbAddr + 16, newName);
        uint8_t e[32];
        bool renamed = false;
        for (int i = 0; i < dsk->dirEntries(); i++)
        {
            dsk->dskRead(dsk->dirByteOff(i), e, 32);
            if (e[0] == 0xE5 || e[0] != (uint8_t)cpm.currentUser)
                continue;
            bool match = true;
            for (int j = 0; j < 11; j++)
                if ((e[1 + j] & 0x7F) != (uint8_t)oldName[j])
                {
                    match = false;
                    break;
                }
            if (match)
            {
                for (int j = 0; j < 11; j++)
                    e[1 + j] = (uint8_t)newName[j];
                dsk->dskWrite(dsk->dirByteOff(i), e, 32);
                renamed = true;
            }
        }
        if (renamed)
            dsk->cache.flush();
        if (!renamed)
            cpu->H = 0x01; // file not found
        cpu->A = renamed ? 0x00 : 0xFF;
        return;
    }
    std::string oldPath = FCBDiskDir(cpu, cpm, fcbAddr) + "/" + FCBToHostName(cpu, fcbAddr);
    std::string newPath = FCBDiskDir(cpu, cpm, fcbAddr + 16) + "/" + FCBToHostName(cpu, fcbAddr + 16);
    if (rename(oldPath.c_str(), newPath.c_str()) != 0)
    {
        cpu->H = 0x01;
        cpu->A = 0xFF;
    }
    else
        cpu->A = 0x00;
}

// Translate absolute record number to DSK (ex, s2, cr) and update FCB.
static void DskRandomIO(DskImage *dsk, intel8080 *cpu, CPMState &cpm,
                        uint16_t fcbAddr, uint32_t rec, bool write)
{
    int rpe = dsk->recsPerBlock() * dsk->blockPtrs(); // records per extent (128)
    uint8_t ex = (uint8_t)((rec / (uint32_t)rpe) & 0x1F);
    uint8_t s2 = (uint8_t)((rec / (uint32_t)rpe) >> 5);
    uint8_t cr = (uint8_t)(rec % (uint32_t)rpe);
    uint8_t buf[128];
    if (write)
    {
        cpu->A = DskWriteRec(dsk, (uint8_t)cpm.currentUser,
                             openFiles[cpm.fcbSlotMap[fcbAddr]].name,
                             ex, s2, cr, &cpu->memory[cpm.dmaAddress]);
    }
    else
    {
        memset(buf, 0x1A, 128);
        cpu->A = DskReadRec(dsk, (uint8_t)cpm.currentUser,
                            openFiles[cpm.fcbSlotMap[fcbAddr]].name,
                            ex, s2, cr, buf);
        if (cpu->A == 0x00)
            memcpy(&cpu->memory[cpm.dmaAddress], buf, 128);
    }
    if (cpu->A == 0x00)
    {
        cpu->memory[fcbAddr + FCB_EX] = ex;
        cpu->memory[fcbAddr + FCB_S2] = s2;
        cpu->memory[fcbAddr + FCB_CR] = cr;
    }
}

// Random read: FCB bytes 33-35 (R0/R1/R2) encode the record number.
static void BDOS_ReadRandom(intel8080 *cpu, CPMState &cpm)
{
    uint16_t fcbAddr = ((uint16_t)cpu->D << 8) | cpu->E;
    auto it = cpm.fcbSlotMap.find(fcbAddr);
    int slot = (it != cpm.fcbSlotMap.end()) ? it->second : 0;

    // dxForth 4.56: open/close/readrandom sequence — auto-open if not in slot map.
    if (slot <= 0)
    {
        std::string path = FCBDiskDir(cpu, cpm, fcbAddr) + "/" + FCBToHostName(cpu, fcbAddr);
        slot = FindOpenSlotByPath(path.c_str());
        if (!slot)
        {
            slot = AllocFileSlot();
            if (!slot)
            {
                cpu->H = 0x06;
                cpu->A = 0x06;
                return;
            }
            bool ro = false;
            FILE *fp = fopen(path.c_str(), "rb+");
            if (!fp)
            {
                fp = fopen(path.c_str(), "rb");
                ro = true;
            }
            if (!fp)
            {
                cpu->H = 0x06;
                cpu->A = 0x06;
                return;
            }
            openFiles[slot].fp = fp;
            openFiles[slot].dsk = nullptr;
            openFiles[slot].readOnly = ro;
            strncpy(openFiles[slot].hostPath, path.c_str(), sizeof(openFiles[slot].hostPath) - 1);
        }
        cpm.fcbSlotMap[fcbAddr] = slot;
    }

    uint32_t rec = (uint32_t)cpu->memory[fcbAddr + 33] | ((uint32_t)cpu->memory[fcbAddr + 34] << 8) | ((uint32_t)cpu->memory[fcbAddr + 35] << 16);

    if (openFiles[slot].dsk)
    {
        DskRandomIO(openFiles[slot].dsk, cpu, cpm, fcbAddr, rec, false);
        if (cpu->A != 0x00)
            cpu->H = cpu->A; // 0x01 = EOF
        return;
    }

    if (!openFiles[slot].fp)
    {
        cpu->H = 0x06;
        cpu->A = 0x09;
        return;
    } // FCB not open
    // R2 != 0 means record number > 65535 — overflow per CP/M spec.
    if (cpu->memory[fcbAddr + 35] != 0)
    {
        cpu->H = cpu->A = BDOS_REC_OVF;
        return;
    }
    long long offset = (long long)rec * 128;
    if (offset > 0x7FFFFFFF)
    {
        cpu->H = cpu->A = BDOS_SEEK_UNW;
        return;
    }
    fseek(openFiles[slot].fp, (long)offset, SEEK_SET);
    uint8_t buf[128];
    memset(buf, 0x1A, 128);
    size_t nread = fread(buf, 1, 128, openFiles[slot].fp);
    if (nread == 0)
    {
        // Physical I/O error vs. seeking past end of file (unwritten extent).
        uint8_t err = ferror(openFiles[slot].fp) ? BDOS_DISK_ERROR : BDOS_SEEK_UNW;
        cpu->H = cpu->A = err;
        return;
    }
    for (int i = 0; i < 128; i++)
        cpu->memory[cpm.dmaAddress + i] = buf[i];
    // CP/M spec: after random read, sequential position == same record (not +1).
    uint32_t extent = rec / 128;
    cpu->memory[fcbAddr + FCB_EX] = (uint8_t)(extent & 0x1F);
    cpu->memory[fcbAddr + FCB_S2] = (uint8_t)(extent >> 5);
    cpu->memory[fcbAddr + FCB_CR] = (uint8_t)(rec % 128);
    cpu->A = 0x00;
}

static void BDOS_WriteRandom(intel8080 *cpu, CPMState &cpm)
{
    uint16_t fcbAddr = ((uint16_t)cpu->D << 8) | cpu->E;
    auto it = cpm.fcbSlotMap.find(fcbAddr);
    int slot = (it != cpm.fcbSlotMap.end()) ? it->second : 0;
    if (slot <= 0)
    {
        cpu->H = 0x06;
        cpu->A = 0x09;
        return;
    } // FCB not open
    if (openFiles[slot].readOnly)
    {
        cpu->H = cpu->A = BDOS_WRITE_PROT;
        return;
    }

    // R2 != 0 means record number > 65535 — overflow per CP/M spec.
    if (cpu->memory[fcbAddr + 35] != 0)
    {
        cpu->H = cpu->A = BDOS_REC_OVF;
        return;
    }
    uint32_t rec = (uint32_t)cpu->memory[fcbAddr + 33] | ((uint32_t)cpu->memory[fcbAddr + 34] << 8);

    if (openFiles[slot].dsk)
    {
        DskRandomIO(openFiles[slot].dsk, cpu, cpm, fcbAddr, rec, true);
        if (cpu->A != 0x00)
            cpu->H = (cpu->A == BDOS_EOF) ? BDOS_WRITE_PROT : cpu->A;
        return;
    }

    if (!openFiles[slot].fp)
    {
        cpu->H = 0x06;
        cpu->A = 0x09;
        return;
    } // FCB not open
    fseek(openFiles[slot].fp, rec * 128LL, SEEK_SET);
    if (fwrite(&cpu->memory[cpm.dmaAddress], 1, 128, openFiles[slot].fp) < 128)
    {
        cpu->H = cpu->A = BDOS_DISK_ERROR;
        return;
    }
    fflush(openFiles[slot].fp);
    cpu->memory[fcbAddr + FCB_EX] = (uint8_t)((rec / 128) & 0x1F);
    cpu->memory[fcbAddr + FCB_S2] = (uint8_t)(rec / (128 * 32));
    cpu->memory[fcbAddr + FCB_CR] = (uint8_t)(rec % 128);
    cpu->A = 0x00;
}

// fn 36: Set Random Record — loads R0/R1/R2 from current sequential position (EX/S2/CR).
// Used by linkers and database programs to switch from sequential to random I/O mid-file.
static void BDOS_SetRandomRecord(intel8080 *cpu, CPMState &cpm)
{
    uint16_t fcbAddr = ((uint16_t)cpu->D << 8) | cpu->E;
    uint32_t rec = (uint32_t)cpu->memory[fcbAddr + FCB_S2] * (128u * 32u) + (uint32_t)cpu->memory[fcbAddr + FCB_EX] * 128u + (uint32_t)cpu->memory[fcbAddr + FCB_CR];
    cpu->memory[fcbAddr + 33] = (uint8_t)(rec & 0xFF);
    cpu->memory[fcbAddr + 34] = (uint8_t)((rec >> 8) & 0xFF);
    cpu->memory[fcbAddr + 35] = (uint8_t)((rec >> 16) & 0xFF);
}

static void BDOS_GetCurrentDisk(intel8080 *cpu, CPMState &cpm)
{
    cpu->A = cpm.currentDrive;
}

static void BDOS_SetDMAAddress(intel8080 *cpu, CPMState &cpm)
{
    uint16_t addr = ((uint16_t)cpu->D << 8) | cpu->E;
    if (addr >= 0x0080 && addr < BDOS_ADDR)
        cpm.dmaAddress = addr;
    // Invalid addresses are silently ignored (keep current DMA).
}

static void BDOS_UserNumber(intel8080 *cpu, CPMState &cpm)
{
    if (cpu->E == 0xFF)
    {
        cpu->A = cpm.currentUser;
    }
    else
    {
        cpm.currentUser = cpu->E & 0x0F;
        cpu->A = 0x00;
    }
}

// ── Peripheral I/O ────────────────────────────────────────────────────────────
// fn 3: Reader Input — read one byte from serial port (if connected) or reader file.
// Returns 0x1A (CP/M EOF) when no data is available.
static void BDOS_ReaderInput(intel8080 *cpu, CPMState &cpm)
{
    if (cpm.serial.enabled() && cpm.serial.connected() && !cpm.serial.rxBuf.empty())
    {
        cpu->A = cpm.serial.rxBuf.front();
        cpm.serial.rxBuf.pop_front();
        return;
    }
    if (!cpm.readerFp && !cpm.readerPath.empty())
        cpm.readerFp = fopen(cpm.readerPath.c_str(), "rb");
    if (cpm.readerFp)
    {
        int ch = fgetc(cpm.readerFp);
        cpu->A = (ch == EOF) ? 0x1A : (uint8_t)ch;
    }
    else
    {
        cpu->A = 0x1A;
    }
}
// fn 4: Punch Output — send one byte to serial port (if connected) or punch file.
static void BDOS_PunchOutput(intel8080 *cpu, CPMState &cpm)
{
    if (cpm.serial.enabled() && cpm.serial.connected())
    {
        cpm.serial.txBuf.push_back(cpu->E);
        return;
    }
    if (!cpm.punchFp)
    {
        std::string path = !cpm.punchPath.empty()
                               ? cpm.punchPath
                               : (!cpm.diskDirs[0].empty() ? cpm.diskDirs[0] : ".") + "/CPM.PUN";
        cpm.punchFp = fopen(path.c_str(), "ab");
    }
    if (cpm.punchFp)
        fputc(cpu->E, cpm.punchFp);
}
// fn 5: List Output — append one byte to configured printer device or CPM.LST.
static void BDOS_ListOutput(intel8080 *cpu, CPMState &cpm)
{
    if (!cpm.printerFp)
    {
        std::string path = !cpm.printerPath.empty()
                               ? cpm.printerPath
                               : (!cpm.diskDirs[0].empty() ? cpm.diskDirs[0] : ".") + "/CPM.LST";
        cpm.printerFp = fopen(path.c_str(), "ab");
    }
    if (cpm.printerFp)
        fputc(cpu->E, cpm.printerFp);
    cpm.printerBuffer += (char)cpu->E;
}
// fn 7: Get IOBYTE — read memory location 0x0003 (the standard CP/M IOBYTE).
static void BDOS_GetIOByte(intel8080 *cpu, CPMState &) { cpu->A = cpu->memory[0x0003]; }
// fn 8: Set IOBYTE — write E to memory location 0x0003.
static void BDOS_SetIOByte(intel8080 *cpu, CPMState &) { cpu->memory[0x0003] = cpu->E; }
// fn 24: Return Login Vector — bitmask of logged-in drives (bit 0 = A:, etc.)
static void BDOS_LoginVector(intel8080 *cpu, CPMState &cpm)
{
    uint16_t mask = 0;
    for (int i = 0; i < MAX_DRIVES; i++)
        if (!cpm.diskDirs[i].empty() || (cpm.diskImages[i] && cpm.diskImages[i]->isOpen()))
            mask |= (uint16_t)(1u << i);
    if (mask == 0)
        mask = 0x0001;
    cpu->H = (uint8_t)(mask >> 8);
    cpu->L = (uint8_t)(mask & 0xFF);
    cpu->A = cpu->L;
}
// fn 28: Write Protect Disk — mark current drive as read-only until next reset.
static void BDOS_WriteProtectDisk(intel8080 *cpu, CPMState &cpm)
{
    cpm.writeProtectedDrives |= (uint16_t)(1u << cpm.currentDrive);
    cpu->A = 0x00;
}
// fn 27: Get Alloc Address — build allocation bitmap for the current drive.
static void BDOS_GetAllocAddr(intel8080 *cpu, CPMState &cpm)
{
    uint8_t *alv = &cpu->memory[ALV_ADDR];
    memset(alv, 0, 32);

    DskImage *dsk = cpm.diskImages[cpm.currentDrive];
    if (dsk && dsk->isOpen())
    {
        int dirBlks = ((dsk->drm + 1) * 32 + dsk->bytesPerBlock() - 1) / dsk->bytesPerBlock();
        for (int i = 0; i < dirBlks; i++)
            alv[i / 8] |= (uint8_t)(0x80u >> (i % 8));
        uint8_t e[32];
        int ptrs = dsk->blockPtrs();
        for (int i = 0; i < dsk->dirEntries(); i++)
        {
            dsk->dskRead(dsk->dirByteOff(i), e, 32);
            if (e[0] == 0xE5)
                continue;
            for (int j = 0; j < ptrs; j++)
            {
                int blk = e[16 + j];
                if (blk > 0 && blk <= dsk->dsm)
                    alv[blk / 8] |= (uint8_t)(0x80u >> (blk % 8));
            }
        }
    }
    else
    {
        alv[0] = 0xC0; // blocks 0-1 always reserved (directory area)
        const std::string &dirPath = cpm.currentDiskDir();
        constexpr int BLOCK_SIZE = 1024;
        int nextBlock = 2;
        DIR *dir = opendir(dirPath.c_str());
        if (dir)
        {
            struct dirent *ent;
            while ((ent = readdir(dir)) != nullptr)
            {
                if (ent->d_name[0] == '.')
                    continue;
                std::string fp = dirPath + "/" + ent->d_name;
                struct stat st;
                if (stat(fp.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0)
                {
                    int blks = (int)((st.st_size + BLOCK_SIZE - 1) / BLOCK_SIZE);
                    for (int b = 0; b < blks && nextBlock < 243; b++, nextBlock++)
                        alv[nextBlock / 8] |= (uint8_t)(0x80u >> (nextBlock % 8));
                }
            }
            closedir(dir);
        }
    }

    cpu->H = (uint8_t)(ALV_ADDR >> 8);
    cpu->L = (uint8_t)(ALV_ADDR & 0xFF);
    cpu->A = cpu->L;
}
// fn 29: Get R/O Vector — return bitmask of write-protected drives.
static void BDOS_GetROVector(intel8080 *cpu, CPMState &cpm)
{
    cpu->H = (uint8_t)(cpm.writeProtectedDrives >> 8);
    cpu->L = (uint8_t)(cpm.writeProtectedDrives & 0xFF);
    cpu->A = cpu->L;
}
// fn 30: Set File Attributes — map R/O bit (FCB ext byte 9, bit 7)
static void BDOS_SetFileAttribs(intel8080 *cpu, CPMState &cpm)
{
    uint16_t fcbAddr = ((uint16_t)cpu->D << 8) | cpu->E;
    DskImage *dsk = FCBGetDsk(cpu, cpm, fcbAddr);
    if (dsk)
    {
        char name[11];
        FCBGetName(cpu, fcbAddr, name);
        bool ro = (cpu->memory[fcbAddr + FCB_EXT + 0] & 0x80) != 0;
        bool sys = (cpu->memory[fcbAddr + FCB_EXT + 1] & 0x80) != 0;
        uint8_t e[32];
        for (int i = 0; i < dsk->dirEntries(); i++)
        {
            dsk->dskRead(dsk->dirByteOff(i), e, 32);
            if (e[0] == 0xE5)
                continue;
            bool match = true;
            for (int j = 0; j < 11; j++)
                if ((e[1 + j] & 0x7F) != (uint8_t)name[j])
                {
                    match = false;
                    break;
                }
            if (!match)
                continue;
            if (ro)
                e[9] |= 0x80;
            else
                e[9] &= 0x7F;
            if (sys)
                e[10] |= 0x80;
            else
                e[10] &= 0x7F;
            dsk->dskWrite(dsk->dirByteOff(i), e, 32);
        }
        dsk->cache.flush();
        cpu->A = 0x00;
        return;
    }
    std::string path = FCBDiskDir(cpu, cpm, fcbAddr) + "/" + FCBToHostName(cpu, fcbAddr);
    bool ro = (cpu->memory[fcbAddr + FCB_EXT + 0] & 0x80) != 0;
    bool sys = (cpu->memory[fcbAddr + FCB_EXT + 1] & 0x80) != 0;
    chmod(path.c_str(), ro ? 0444 : 0644);
    HostSetSys(path, sys);
    cpu->A = 0x00;
}
// fn 31: Get Disk Parms — return HL = DPH_ADDR
static void BDOS_GetDiskParms(intel8080 *cpu, CPMState &)
{
    cpu->H = (uint8_t)(DPH_ADDR >> 8);
    cpu->L = (uint8_t)(DPH_ADDR & 0xFF);
    cpu->A = cpu->L;
}
// fn 37: Reset Drive — no-op (drives don't have physical state)
static void BDOS_ResetDrive(intel8080 *cpu, CPMState &) { cpu->H = cpu->L = cpu->A = 0x00; }
// fn 38 (0x26): Access Drive — attempt to access a drive; stub returns success.
static void BDOS_AccessDrive(intel8080 *cpu, CPMState &) { cpu->A = 0x00; }
// fn 39 (0x27): Free Drive — release previously accessed drive; stub.
static void BDOS_FreeDrive(intel8080 *, CPMState &) {}
// fn 41 (0x29): Parse Filename — parse a string at DE into the FCB at HL.
// Returns in A the number of characters consumed. Used by some programs (e.g. MBASIC).
static void BDOS_ParseFilename(intel8080 *cpu, CPMState &)
{
    uint16_t srcAddr = ((uint16_t)cpu->D << 8) | cpu->E;
    uint16_t fcbAddr = ((uint16_t)cpu->H << 8) | cpu->L;

    memset(&cpu->memory[fcbAddr], 0, 36);
    memset(&cpu->memory[fcbAddr + 1], ' ', 11);

    int i = 0;
    while (i < 256 && cpu->memory[srcAddr + i] == ' ')
        i++; // skip leading spaces

    // Collect the token (stop at space, control char, or '$').
    std::string token;
    while (i < 256 && cpu->memory[srcAddr + i] > 0x20 && cpu->memory[srcAddr + i] != '$')
        token += (char)cpu->memory[srcAddr + i++];

    size_t pos = 0;
    if (token.size() >= 2 && token[1] == ':')
    {
        cpu->memory[fcbAddr] = (uint8_t)(toupper((unsigned char)token[0]) - 'A' + 1);
        pos = 2;
    }
    std::string rest = token.substr(pos);
    auto dot = rest.find('.');
    std::string nm = (dot != std::string::npos) ? rest.substr(0, dot) : rest;
    std::string ex = (dot != std::string::npos) ? rest.substr(dot + 1) : "";
    for (size_t j = 0; j < std::min(nm.size(), (size_t)8); j++)
        cpu->memory[fcbAddr + 1 + j] = (uint8_t)toupper((unsigned char)nm[j]);
    for (size_t j = 0; j < std::min(ex.size(), (size_t)3); j++)
        cpu->memory[fcbAddr + 9 + j] = (uint8_t)toupper((unsigned char)ex[j]);

    cpu->A = (uint8_t)i;
    cpu->L = (uint8_t)i;
    cpu->H = 0;
}
// fn 40: Write Random with Zero Fill — like WriteRandom but gaps are zero-filled.
// On Linux, fseek past EOF already produces a sparse file that reads as zeros,
// so the only difference from WriteRandom is explicitly zeroing any unwritten gap.
// For DSK images we treat it identically to WriteRandom (block gaps read as zero).
static void BDOS_WriteRandomZeroFill(intel8080 *cpu, CPMState &cpm)
{
    uint16_t fcbAddr = ((uint16_t)cpu->D << 8) | cpu->E;
    auto it = cpm.fcbSlotMap.find(fcbAddr);
    int slot = (it != cpm.fcbSlotMap.end()) ? it->second : 0;
    if (slot <= 0)
    {
        cpu->H = 0x06;
        cpu->A = 0x09;
        return;
    } // FCB not open
    if (openFiles[slot].readOnly)
    {
        cpu->H = cpu->A = BDOS_WRITE_PROT;
        return;
    }

    // R2 != 0 means record number > 65535 — overflow per CP/M spec.
    if (cpu->memory[fcbAddr + 35] != 0)
    {
        cpu->H = cpu->A = BDOS_REC_OVF;
        return;
    }
    uint32_t rec = (uint32_t)cpu->memory[fcbAddr + 33] | ((uint32_t)cpu->memory[fcbAddr + 34] << 8);

    if (openFiles[slot].dsk)
    {
        DskRandomIO(openFiles[slot].dsk, cpu, cpm, fcbAddr, rec, true);
        if (cpu->A != 0x00)
            cpu->H = (cpu->A == BDOS_EOF) ? BDOS_WRITE_PROT : cpu->A;
        return;
    }

    if (!openFiles[slot].fp)
    {
        cpu->H = 0x06;
        cpu->A = 0x09;
        return;
    } // FCB not open
    // Zero-fill any gap between current EOF and the target record.
    fseek(openFiles[slot].fp, 0, SEEK_END);
    long eof = ftell(openFiles[slot].fp);
    long target = (long)rec * 128L;
    if (target > eof)
    {
        static const uint8_t zeros[128] = {};
        for (long pos = eof; pos < target; pos += 128)
        {
            if (fwrite(zeros, 1, 128, openFiles[slot].fp) < 128)
            {
                cpu->H = cpu->A = BDOS_DISK_ERROR;
                return;
            }
        }
    }
    fseek(openFiles[slot].fp, target, SEEK_SET);
    if (fwrite(&cpu->memory[cpm.dmaAddress], 1, 128, openFiles[slot].fp) < 128)
    {
        cpu->H = cpu->A = BDOS_DISK_ERROR;
        return;
    }
    fflush(openFiles[slot].fp);
    cpu->memory[fcbAddr + FCB_EX] = (uint8_t)((rec / 128) & 0x1F);
    cpu->memory[fcbAddr + FCB_S2] = (uint8_t)(rec / (128 * 32));
    cpu->memory[fcbAddr + FCB_CR] = (uint8_t)(rec % 128);
    cpu->A = 0x00;
}

static void BDOS_ComputeFileSize(intel8080 *cpu, CPMState &cpm)
{
    uint16_t fcbAddr = ((uint16_t)cpu->D << 8) | cpu->E;
    DskImage *dsk = FCBGetDsk(cpu, cpm, fcbAddr);
    if (dsk)
    {
        char name[11];
        FCBGetName(cpu, fcbAddr, name);
        int rpb = dsk->recsPerBlock(), ptrs = dsk->blockPtrs();
        int rpe = rpb * ptrs;
        uint8_t e[32];
        int maxExt = -1;
        uint32_t maxRecs = 0;
        for (int i = 0; i < dsk->dirEntries(); i++)
        {
            dsk->dskRead(dsk->dirByteOff(i), e, 32);
            if (e[0] == 0xE5 || e[0] != (uint8_t)cpm.currentUser)
                continue;
            bool match = true;
            for (int j = 0; j < 11; j++)
                if ((e[1 + j] & 0x7F) != (uint8_t)name[j])
                {
                    match = false;
                    break;
                }
            if (!match)
                continue;
            int extNum = (int)(e[12] & 0x1F) + ((int)(e[14] & 0x3F) << 5);
            if (extNum > maxExt)
            {
                maxExt = extNum;
                int lastBlk = -1;
                for (int j = ptrs - 1; j >= 0; j--)
                    if (e[16 + j])
                    {
                        lastBlk = j;
                        break;
                    }
                maxRecs = (lastBlk >= 0) ? (uint32_t)lastBlk * rpb + e[15] : 0;
            }
        }
        uint32_t recs = (maxExt >= 0) ? (uint32_t)maxExt * rpe + maxRecs : 0;
        cpu->memory[fcbAddr + 33] = (uint8_t)(recs & 0xFF);
        cpu->memory[fcbAddr + 34] = (uint8_t)((recs >> 8) & 0xFF);
        cpu->memory[fcbAddr + 35] = (uint8_t)((recs >> 16) & 0xFF);
        if (maxExt < 0)
            cpu->H = 0x01; // file not found
        cpu->A = (maxExt >= 0) ? 0x00 : 0xFF;
        return;
    }
    std::string path = FCBDiskDir(cpu, cpm, fcbAddr) + "/" + FCBToHostName(cpu, fcbAddr);
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp)
    {
        cpu->H = 0x01;
        cpu->A = 0xFF; // file not found
        return;
    }
    fseek(fp, 0, SEEK_END);
    uint32_t recs = (uint32_t)((ftell(fp) + 127) / 128);
    fclose(fp);
    cpu->memory[fcbAddr + 33] = (uint8_t)(recs & 0xFF);
    cpu->memory[fcbAddr + 34] = (uint8_t)((recs >> 8) & 0xFF);
    cpu->memory[fcbAddr + 35] = (uint8_t)((recs >> 16) & 0xFF);
    cpu->A = 0x00;
}

// ── Line-input helper (fn 10) ────────────────────────────────────────────────
// Accumulates characters from the terminal input queue, echoing each one, until
// a CR is received. Returns true when the line is complete and the FCB is filled.
static bool HandleLineInput(intel8080 *cpu, CPMState &cpm)
{
    uint16_t bufAddr = ((uint16_t)cpu->D << 8) | cpu->E;

    if (!cpm.lineInputActive)
    {
        cpm.lineInputActive = true;
        cpm.lineInputFCB = bufAddr;
        cpm.lineInputAccum.clear();
    }

    uint8_t maxLen = cpu->memory[cpm.lineInputFCB];

    while (!cpm.terminal.inputQueue.empty())
    {
        uint8_t ch = cpm.terminal.inputQueue.front();
        cpm.terminal.inputQueue.pop_front();

        if (ch == 0x0D)
        {
            // CR — line is complete; write result into the buffer FCB
            size_t len = std::min(cpm.lineInputAccum.size(), (size_t)maxLen);
            cpu->memory[cpm.lineInputFCB + 1] = (uint8_t)len;
            for (size_t i = 0; i < len; i++)
                cpu->memory[cpm.lineInputFCB + 2 + i] = (uint8_t)cpm.lineInputAccum[i];
            cpm.terminal.putChar('\r');
            cpm.terminal.putChar('\n');
            cpm.lineInputActive = false;
            return true;
        }
        else if ((ch == 0x08 || ch == 0x7F) && !cpm.lineInputAccum.empty())
        {
            // Backspace / DEL rubout — erase last character on screen and in accumulator
            cpm.lineInputAccum.pop_back();
            cpm.terminal.putChar('\b');
            cpm.terminal.putChar(' ');
            cpm.terminal.putChar('\b');
        }
        else if (ch >= 0x20 && ch < 0x7F && cpm.lineInputAccum.size() < maxLen)
        {
            cpm.lineInputAccum += (char)ch;
            cpm.terminal.putChar((char)ch); // echo
        }
    }
    return false; // still waiting for CR
}

// ── Simulate RET ─────────────────────────────────────────────────────────────
// Pop the 16-bit return address pushed by CALL 0x0005 and jump back to caller.
static void DoRet(intel8080 *cpu)
{
    uint16_t lo = cpu->memory[cpu->SP];
    uint16_t hi = cpu->memory[cpu->SP + 1];
    cpu->SP += 2;
    cpu->PC = (hi << 8) | lo;
}

// ── BIOS peripheral helpers ───────────────────────────────────────────────────

static void BIOS_ListOutput(intel8080 *cpu, CPMState &cpm)
{
    if (!cpm.printerFp)
    {
        std::string path = !cpm.printerPath.empty()
                               ? cpm.printerPath
                               : (!cpm.diskDirs[0].empty() ? cpm.diskDirs[0] : ".") + "/CPM.LST";
        cpm.printerFp = fopen(path.c_str(), "ab");
    }
    if (cpm.printerFp)
        fputc(cpu->C, cpm.printerFp);
    cpm.printerBuffer += (char)cpu->C;
}

static void BIOS_PunchOutput(intel8080 *cpu, CPMState &cpm)
{
    if (!cpm.punchFp)
    {
        std::string path = !cpm.punchPath.empty()
                               ? cpm.punchPath
                               : (!cpm.diskDirs[0].empty() ? cpm.diskDirs[0] : ".") + "/CPM.PUN";
        cpm.punchFp = fopen(path.c_str(), "ab");
    }
    if (cpm.punchFp)
        fputc(cpu->C, cpm.punchFp);
}

// Raw track/sector read from a mounted DskImage into a 128-byte buffer.
// Sectors are 1-based (CP/M convention); applies skew translation if present.
static uint8_t BiosRawRead(DskImage *dsk, uint16_t track, uint8_t sector, uint8_t *buf)
{
    if (!dsk || !dsk->isOpen())
        return BDOS_DISK_ERROR;
    int physSec = (int)sector - 1;
    if (!dsk->skewTable.empty() && physSec < (int)dsk->skewTable.size())
        physSec = dsk->skewTable[physSec];
    long off = ((long)track * dsk->spt + physSec) * 128L;
    fseek(dsk->fp, off, SEEK_SET);
    size_t n = fread(buf, 1, 128, dsk->fp);
    if (n < 128)
        memset(buf + n, 0x1A, 128 - n);
    return BDOS_OK;
}

static uint8_t BiosRawWrite(DskImage *dsk, uint16_t track, uint8_t sector, const uint8_t *buf)
{
    if (!dsk || !dsk->isOpen())
        return BDOS_DISK_ERROR;
    if (dsk->readOnly)
        return BDOS_WRITE_PROT;
    int physSec = (int)sector - 1;
    if (!dsk->skewTable.empty() && physSec < (int)dsk->skewTable.size())
        physSec = dsk->skewTable[physSec];
    long off = ((long)track * dsk->spt + physSec) * 128L;
    fseek(dsk->fp, off, SEEK_SET);
    fwrite(buf, 1, 128, dsk->fp);
    fflush(dsk->fp);
    return BDOS_OK;
}

// ── BIOSCall — direct BIOS stub dispatch ─────────────────────────────────────

bool BIOSCall(intel8080 *cpu, CPMState &cpm)
{
    int func = (cpu->PC - BIOS_ADDR) / 3;

    // CONIN blocks until a character is available (same strategy as BDOS fn 1).
    if (func == 2 && cpm.terminal.inputQueue.empty())
        return cpm.running;

    switch (func)
    {
    case 0: // WBOOT
        if (cpm.ccpMode)
        {
            cpm.ccpRunning = true;
            cpm.ccpPrompted = false;
        }
        else
        {
            cpm.running = false;
        }
        return cpm.running; // no DoRet — warm boot never returns

    case 1: // CONST — console status
        cpu->A = cpm.terminal.inputQueue.empty() ? 0x00 : 0xFF;
        break;

    case 2: // CONIN — queue guaranteed non-empty here
        cpu->A = cpm.terminal.inputQueue.front();
        cpm.terminal.inputQueue.pop_front();
        break;

    case 3: // CONOUT — character in C
        cpm.consoleOut((char)cpu->C);
        break;

    case 4: // LIST — printer output
        BIOS_ListOutput(cpu, cpm);
        break;

    case 5: // PUNCH — punch output
        BIOS_PunchOutput(cpu, cpm);
        break;

    case 6: // READER — reader input
        BDOS_ReaderInput(cpu, cpm);
        break;

    case 7: // HOME — seek drive to track 0
        cpm.biosTrack = 0;
        break;

    case 8: // SELDSK — select drive (C = drive 0=A); return HL = DPH or 0 if invalid
    {
        uint8_t drv = cpu->C;
        if (DriveIsValid(cpm, drv))
        {
            cpm.biosDrive = drv;
            cpu->H = (uint8_t)(DPH_ADDR >> 8);
            cpu->L = (uint8_t)(DPH_ADDR & 0xFF);
        }
        else
        {
            cpu->H = cpu->L = 0x00;
        }
        break;
    }

    case 9: // SETTRK — BC = track number
        cpm.biosTrack = (uint16_t)((cpu->B << 8) | cpu->C);
        break;

    case 10: // SETSEC — C = sector number (1-based)
        cpm.biosSector = cpu->C;
        break;

    case 11: // SETDMA — BC = DMA address
        cpm.biosDMA = (uint16_t)((cpu->B << 8) | cpu->C);
        break;

    case 12: // READ — 128 bytes from disk into DMA buffer
    {
        DskImage *dsk = (cpm.biosDrive < MAX_DRIVES) ? cpm.diskImages[cpm.biosDrive] : nullptr;
        cpu->A = BiosRawRead(dsk, cpm.biosTrack, cpm.biosSector, cpu->memory + cpm.biosDMA);
        break;
    }

    case 13: // WRITE — 128 bytes from DMA buffer to disk
    {
        DskImage *dsk = (cpm.biosDrive < MAX_DRIVES) ? cpm.diskImages[cpm.biosDrive] : nullptr;
        cpu->A = BiosRawWrite(dsk, cpm.biosTrack, cpm.biosSector, cpu->memory + cpm.biosDMA);
        break;
    }

    case 14: // LISTST — printer status (always ready)
        cpu->A = 0xFF;
        break;

    case 15: // SECTRAN — translate logical sector BC; return HL = physical sector
    {
        DskImage *dsk = (cpm.biosDrive < MAX_DRIVES) ? cpm.diskImages[cpm.biosDrive] : nullptr;
        uint16_t logSec = (uint16_t)((cpu->B << 8) | cpu->C);
        if (dsk && !dsk->skewTable.empty() && logSec < (uint16_t)dsk->skewTable.size())
            logSec = (uint16_t)dsk->skewTable[logSec];
        cpu->H = (uint8_t)(logSec >> 8);
        cpu->L = (uint8_t)(logSec & 0xFF);
        break;
    }

    default:
        break;
    }

    DoRet(cpu);
    return cpm.running;
}

// ── BDOSCall — dispatch ───────────────────────────────────────────────────────

bool BDOSCall(intel8080 *cpu, CPMState &cpm)
{
    // ── Blocking: Console Input (fn 1) ───────────────────────────────────
    // If the input queue is empty, return without touching PC/SP. The main
    // loop will call us again next iteration once a key has been queued.
    if (cpu->C == 1 && cpm.terminal.inputQueue.empty())
        return cpm.running;

    // ── Blocking: Read Console Buffer (fn 10) ────────────────────────────
    // HandleLineInput accumulates characters across multiple iterations until
    // the user presses Enter. Same early-return strategy as fn 1.
    if (cpu->C == 10)
    {
        if (!HandleLineInput(cpu, cpm))
            return cpm.running;
        DoRet(cpu);
        return cpm.running;
    }

    // ── Normal dispatch ──────────────────────────────────────────────────
    cpu->H = 0x00; // preset for 8-bit returns; 16-bit functions override
    switch (cpu->C)
    {
    case 0:
        BDOS_SystemReset(cpu, cpm);
        break;
    case 1:
        BDOS_ConsoleInput(cpu, cpm);
        break;
    case 2:
        BDOS_ConsoleOutput(cpu, cpm);
        break;
    case 3:
        BDOS_ReaderInput(cpu, cpm);
        break;
    case 4:
        BDOS_PunchOutput(cpu, cpm);
        break;
    case 5:
        BDOS_ListOutput(cpu, cpm);
        break;
    case 6:
        BDOS_DirectConsoleIO(cpu, cpm);
        break;
    case 7:
        BDOS_GetIOByte(cpu, cpm);
        break;
    case 8:
        BDOS_SetIOByte(cpu, cpm);
        break;
    case 9:
        BDOS_PrintString(cpu, cpm);
        break;
    case 11:
        BDOS_ConsoleStatus(cpu, cpm);
        break;
    case 12:
        BDOS_Version(cpu, cpm);
        break;
    case 13:
        BDOS_ResetDisk(cpu, cpm);
        break;
    case 14:
        BDOS_SelectDisk(cpu, cpm);
        break;
    case 15:
        BDOS_OpenFile(cpu, cpm);
        break;
    case 16:
        BDOS_CloseFile(cpu, cpm);
        break;
    case 17:
        BDOS_SearchFirst(cpu, cpm);
        break;
    case 18:
        BDOS_SearchNext(cpu, cpm);
        break;
    case 19:
        BDOS_EraseFile(cpu, cpm);
        break;
    case 20:
        BDOS_ReadSequential(cpu, cpm);
        break;
    case 21:
        BDOS_WriteSequential(cpu, cpm);
        break;
    case 22:
        BDOS_MakeFile(cpu, cpm);
        break;
    case 23:
        BDOS_RenameFile(cpu, cpm);
        break;
    case 24:
        BDOS_LoginVector(cpu, cpm);
        break;
    case 25:
        BDOS_GetCurrentDisk(cpu, cpm);
        break;
    case 26:
        BDOS_SetDMAAddress(cpu, cpm);
        break;
    case 27:
        BDOS_GetAllocAddr(cpu, cpm);
        break;
    case 28:
        BDOS_WriteProtectDisk(cpu, cpm);
        break;
    case 29:
        BDOS_GetROVector(cpu, cpm);
        break;
    case 30:
        BDOS_SetFileAttribs(cpu, cpm);
        break;
    case 31:
        BDOS_GetDiskParms(cpu, cpm);
        break;
    case 32:
        BDOS_UserNumber(cpu, cpm);
        break;
    case 33:
        BDOS_ReadRandom(cpu, cpm);
        break;
    case 34:
        BDOS_WriteRandom(cpu, cpm);
        break;
    case 35:
        BDOS_ComputeFileSize(cpu, cpm);
        break;
    case 36:
        BDOS_SetRandomRecord(cpu, cpm);
        break;
    case 37:
        BDOS_ResetDrive(cpu, cpm);
        break;
    case 38:
        BDOS_AccessDrive(cpu, cpm);
        break;
    case 39:
        BDOS_FreeDrive(cpu, cpm);
        break;
    case 40:
        BDOS_WriteRandomZeroFill(cpu, cpm);
        break;
    case 41:
        BDOS_ParseFilename(cpu, cpm);
        break;

    case 96:
    { // BDOS_LoadOverlay — load a binary file into CPU memory at any address.
        // Input:  DE = target load address in 8080 memory.
        //         HL = FCB address (byte 0 = drive, bytes 1-11 = 8.3 name).
        // Output: A  = 0x00 success, 0xFF error (not found / too large).
        //         HL = bytes loaded (0 on error).
        uint16_t destAddr = (uint16_t)((cpu->D << 8) | cpu->E);
        uint16_t fcbAddr = (uint16_t)((cpu->H << 8) | cpu->L);

        if (destAddr >= BDOS_ADDR)
        {
            cpu->A = 0xFF;
            cpu->H = 0;
            break;
        }

        uint32_t bytesLoaded = 0;
        DskImage *dsk = FCBGetDsk(cpu, cpm, fcbAddr);

        if (dsk)
        {
            // DSK path: walk extents using DskReadRec until EOF (0x01).
            char name[11];
            FCBGetName(cpu, fcbAddr, name);
            uint8_t ex = 0, s2 = 0, cr = 0;
            uint8_t buf[128];
            while (destAddr + bytesLoaded + 128 <= BDOS_ADDR)
            {
                uint8_t r = DskReadRec(dsk, cpm.currentUser, name, ex, s2, cr, buf);
                if (r != BDOS_OK)
                    break; // 0x01 = EOF, anything else = error
                memcpy(&cpu->memory[destAddr + bytesLoaded], buf, 128);
                bytesLoaded += 128;
                if (++cr >= 128)
                {
                    cr = 0;
                    if (++ex >= 32)
                    {
                        ex = 0;
                        s2++;
                    }
                }
            }
            if (bytesLoaded == 0)
            {
                cpu->A = 0xFF;
                cpu->H = 0;
                break;
            }
        }
        else
        {
            // Host filesystem path.
            const std::string &dir = FCBDiskDir(cpu, cpm, fcbAddr);
            std::string filename = FCBToHostName(cpu, fcbAddr);
            std::string path = dir.empty() ? filename : dir + "/" + filename;
            FILE *fp = fopen(path.c_str(), "rb");
            if (!fp)
            {
                cpu->A = 0xFF;
                cpu->H = 0;
                break;
            }
            fseek(fp, 0, SEEK_END);
            long fileSize = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            if (fileSize <= 0 || (long)destAddr + fileSize > (long)BDOS_ADDR)
            {
                fclose(fp);
                cpu->A = 0xFF;
                cpu->H = 0;
                break;
            }
            bytesLoaded = (uint32_t)fread(&cpu->memory[destAddr], 1, (size_t)fileSize, fp);
            fclose(fp);
        }

        cpu->H = (uint8_t)(bytesLoaded >> 8);
        cpu->A = cpu->L = (uint8_t)(bytesLoaded & 0xFF);
        break;
    }

    case 97:
    { // BDOS_QueryOverlayRegion — return the configured overlay window.
        // Input:  none.
        // Output: HL = overlayBase (0 if none configured).
        //         DE = overlayTop  (= BDOS_ADDR when no overlay region).
        uint16_t base = cpm.overlayBase;
        uint16_t top = base ? cpm.overlayTop() : BDOS_ADDR;
        cpu->H = (uint8_t)(base >> 8);
        cpu->A = cpu->L = (uint8_t)(base & 0xFF);
        cpu->D = (uint8_t)(top >> 8);
        cpu->E = (uint8_t)(top & 0xFF);
        break;
    }

    case 105:
    { // CP/M 3.0 Get/Set Date+Time
        // Buffer at DE: [day_lo, day_hi, hours_bcd, minutes_bcd, seconds_bcd]
        // Day count: 1 = Jan 1, 1978 (CP/M 3.0 epoch).
        time_t now = time(nullptr);
        struct tm *t = localtime(&now);

        // Days since Jan 1 1978.
        struct tm epoch = {};
        epoch.tm_year = 78;
        epoch.tm_mon = 0;
        epoch.tm_mday = 1;
        int days = (int)((now - mktime(&epoch)) / 86400) + 1;

        auto bcd = [](int v) -> uint8_t
        { return (uint8_t)(((v / 10) << 4) | (v % 10)); };

        uint16_t buf = cpu->D << 8 | cpu->E;
        cpu->memory[buf + 0] = (uint8_t)(days & 0xFF);
        cpu->memory[buf + 1] = (uint8_t)(days >> 8);
        cpu->memory[buf + 2] = bcd(t->tm_hour);
        cpu->memory[buf + 3] = bcd(t->tm_min);
        cpu->memory[buf + 4] = bcd(t->tm_sec);
        cpu->A = 0;
        break;
    }
    default:
        std::cerr << "[BDOS] fn " << (int)cpu->C
                  << " @ PC=0x" << std::hex << cpu->PC << std::dec << "\n";
        cpu->A = cpu->H = cpu->L = 0x00;
        break;
    }

    // CP/M 2.2: B always mirrors H, L always mirrors A.
    // 16-bit return functions set A=L before reaching here.
    cpu->B = cpu->H;
    cpu->L = cpu->A;

    DoRet(cpu);
    return cpm.running;
}

// ── Save / Load state ─────────────────────────────────────────────────────────

static void writeStr(FILE *fp, const std::string &s)
{
    uint32_t len = (uint32_t)s.size();
    fwrite(&len, 4, 1, fp);
    fwrite(s.data(), 1, len, fp);
}

static std::string readStr(FILE *fp)
{
    uint32_t len = 0;
    fread(&len, 4, 1, fp);
    if (len > 4096)
        return "";
    std::string s(len, '\0');
    fread(s.data(), 1, len, fp);
    return s;
}

bool SaveCPMState(intel8080 *cpu, CPMState &cpm, const std::string &path)
{
    FILE *fp = fopen(path.c_str(), "wb");
    if (!fp)
        return false;

    fwrite("CPM8080\0", 1, 8, fp);
    uint32_t version = 5;
    fwrite(&version, 4, 1, fp);

    // CPU registers
    uint8_t regs[7] = {cpu->A, cpu->B, cpu->C, cpu->D, cpu->E, cpu->H, cpu->L};
    fwrite(regs, 1, 7, fp);
    uint8_t flags[5] = {cpu->sf, cpu->zf, cpu->acf, cpu->pf, cpu->cf};
    fwrite(flags, 1, 5, fp);
    uint8_t misc[2] = {cpu->halted, cpu->interrupts};
    fwrite(misc, 1, 2, fp);
    fwrite(&cpu->cycles, 4, 1, fp);
    fwrite(&cpu->cyclesInterrupt, 4, 1, fp);
    fwrite(cpu->IOPorts, 1, 256, fp);
    fwrite(&cpu->shiftRegister, 2, 1, fp);
    fwrite(&cpu->shiftOffset, 1, 1, fp);
    uint8_t arcade = cpu->arcadeMode ? 1 : 0;
    fwrite(&arcade, 1, 1, fp);
    fwrite(&cpu->SP, 2, 1, fp);
    fwrite(&cpu->PC, 2, 1, fp);
    fwrite(cpu->memory, 1, 0x10000, fp);

    // CPM state
    fwrite(&cpm.dmaAddress, 2, 1, fp);
    fwrite(&cpm.currentDrive, 1, 1, fp);
    fwrite(&cpm.currentUser, 1, 1, fp);
    uint8_t bools[5] = {cpm.running, cpm.ccpMode, cpm.ccpRunning,
                        cpm.ccpPrompted, cpm.lineInputActive};
    fwrite(bools, 1, 5, fp);
    fwrite(&cpm.writeProtectedDrives, 2, 1, fp); // uint16_t, 16-drive bitmask
    fwrite(&cpm.lineInputFCB, 2, 1, fp);
    int32_t si = 0; // searchIndex removed; placeholder for file format compat
    fwrite(&si, 4, 1, fp);

    for (int i = 0; i < MAX_DRIVES; i++)
        writeStr(fp, cpm.diskDirs[i]);
    writeStr(fp, cpm.ccpLine);
    writeStr(fp, cpm.lineInputAccum);

    // Version 3: DSK image paths for all MAX_DRIVES drives.
    for (int i = 0; i < MAX_DRIVES; i++)
    {
        std::string p = (cpm.diskImages[i] && cpm.diskImages[i]->isOpen())
                            ? cpm.diskImages[i]->path
                            : "";
        uint8_t ro = (cpm.diskImages[i] && cpm.diskImages[i]->readOnly) ? 1 : 0;
        writeStr(fp, p);
        fwrite(&ro, 1, 1, fp);
    }

    // Version 4: BIOS registers, submit queue, editor state, peripheral paths.
    fwrite(&cpm.biosDrive, 1, 1, fp);
    fwrite(&cpm.biosTrack, 2, 1, fp);
    fwrite(&cpm.biosSector, 1, 1, fp);
    fwrite(&cpm.biosDMA, 2, 1, fp);

    uint32_t sqSize = (uint32_t)cpm.submitQueue.size();
    fwrite(&sqSize, 4, 1, fp);
    for (auto &s : cpm.submitQueue)
        writeStr(fp, s);

    uint8_t edFlags[2] = {cpm.editorActive, cpm.editorModified};
    fwrite(edFlags, 1, 2, fp);
    writeStr(fp, cpm.editorFilePath);
    writeStr(fp, cpm.editorCmdBuf);
    uint32_t nLines = (uint32_t)cpm.editorLines.size();
    fwrite(&nLines, 4, 1, fp);
    for (auto &l : cpm.editorLines)
        writeStr(fp, l);

    writeStr(fp, cpm.readerPath);
    writeStr(fp, cpm.punchPath);
    writeStr(fp, cpm.printerPath);

    // Version 5: ccpEnv map, printerBuffer, serial port number.
    uint32_t envSize = (uint32_t)cpm.ccpEnv.size();
    fwrite(&envSize, 4, 1, fp);
    for (auto &kv : cpm.ccpEnv)
    {
        writeStr(fp, kv.first);
        writeStr(fp, kv.second);
    }
    writeStr(fp, cpm.printerBuffer);
    fwrite(&cpm.serial.port, 2, 1, fp);

    fclose(fp);
    return true;
}

bool LoadCPMState(intel8080 *cpu, CPMState &cpm, const std::string &path)
{
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp)
        return false;

    char magic[8];
    fread(magic, 1, 8, fp);
    if (memcmp(magic, "CPM8080", 7) != 0)
    {
        fclose(fp);
        return false;
    }
    uint32_t version;
    fread(&version, 4, 1, fp);
    if (version < 1 || version > 5)
    {
        fclose(fp);
        return false;
    }

    CPMCloseAllFiles(cpm);

    uint8_t regs[7];
    fread(regs, 1, 7, fp);
    cpu->A = regs[0];
    cpu->B = regs[1];
    cpu->C = regs[2];
    cpu->D = regs[3];
    cpu->E = regs[4];
    cpu->H = regs[5];
    cpu->L = regs[6];
    uint8_t flags[5];
    fread(flags, 1, 5, fp);
    cpu->sf = flags[0];
    cpu->zf = flags[1];
    cpu->acf = flags[2];
    cpu->pf = flags[3];
    cpu->cf = flags[4];
    uint8_t misc[2];
    fread(misc, 1, 2, fp);
    cpu->halted = misc[0];
    cpu->interrupts = misc[1];
    fread(&cpu->cycles, 4, 1, fp);
    fread(&cpu->cyclesInterrupt, 4, 1, fp);
    fread(cpu->IOPorts, 1, 256, fp);
    fread(&cpu->shiftRegister, 2, 1, fp);
    fread(&cpu->shiftOffset, 1, 1, fp);
    uint8_t arcade;
    fread(&arcade, 1, 1, fp);
    cpu->arcadeMode = arcade != 0;
    fread(&cpu->SP, 2, 1, fp);
    fread(&cpu->PC, 2, 1, fp);
    fread(cpu->memory, 1, 0x10000, fp);

    fread(&cpm.dmaAddress, 2, 1, fp);
    fread(&cpm.currentDrive, 1, 1, fp);
    fread(&cpm.currentUser, 1, 1, fp);
    if (version >= 3)
    {
        // Version 3: 5 bools + uint16_t writeProtectedDrives
        uint8_t bools[5];
        fread(bools, 1, 5, fp);
        cpm.running = bools[0];
        cpm.ccpMode = bools[1];
        cpm.ccpRunning = bools[2];
        cpm.ccpPrompted = bools[3];
        cpm.lineInputActive = bools[4];
        fread(&cpm.writeProtectedDrives, 2, 1, fp);
    }
    else
    {
        // Version 2 (4-drive): 6 bools, last one is writeProtectedDrives (uint8_t)
        uint8_t bools[6];
        fread(bools, 1, 6, fp);
        cpm.running = bools[0];
        cpm.ccpMode = bools[1];
        cpm.ccpRunning = bools[2];
        cpm.ccpPrompted = bools[3];
        cpm.lineInputActive = bools[4];
        cpm.writeProtectedDrives = bools[5];
    }
    cpm.submitQueue.clear();
    fread(&cpm.lineInputFCB, 2, 1, fp);
    int32_t si;
    fread(&si, 4, 1, fp); // placeholder (searchIndex removed)
    (void)si;
    cpm.searchContexts.clear();
    cpm.fcbSlotMap.clear();

    int nDrives = (version >= 3) ? MAX_DRIVES : 4;
    for (int i = 0; i < nDrives; i++)
        cpm.diskDirs[i] = readStr(fp);
    cpm.ccpLine = readStr(fp);
    cpm.lineInputAccum = readStr(fp);

    // Version 2+: restore DSK image mounts.
    if (version >= 2)
    {
        DskUnmountAll(cpm);
        for (int i = 0; i < nDrives; i++)
        {
            std::string p = readStr(fp);
            uint8_t ro = 0;
            fread(&ro, 1, 1, fp);
            if (!p.empty())
                DskMount(cpm, i, p, ro != 0);
        }
    }

    // Version 4: BIOS registers, submit queue, editor state, peripheral paths.
    if (version >= 4)
    {
        fread(&cpm.biosDrive, 1, 1, fp);
        fread(&cpm.biosTrack, 2, 1, fp);
        fread(&cpm.biosSector, 1, 1, fp);
        fread(&cpm.biosDMA, 2, 1, fp);

        uint32_t sqSize = 0;
        fread(&sqSize, 4, 1, fp);
        cpm.submitQueue.clear();
        for (uint32_t i = 0; i < sqSize; i++)
            cpm.submitQueue.push_back(readStr(fp));

        uint8_t edFlags[2] = {};
        fread(edFlags, 1, 2, fp);
        cpm.editorActive = edFlags[0] != 0;
        cpm.editorModified = edFlags[1] != 0;
        cpm.editorFilePath = readStr(fp);
        cpm.editorCmdBuf = readStr(fp);
        uint32_t nLines = 0;
        fread(&nLines, 4, 1, fp);
        cpm.editorLines.clear();
        for (uint32_t i = 0; i < nLines; i++)
            cpm.editorLines.push_back(readStr(fp));

        cpm.readerPath = readStr(fp);
        cpm.punchPath = readStr(fp);
        cpm.printerPath = readStr(fp);
    }

    // Version 5: ccpEnv, printerBuffer, serial port.
    if (version >= 5)
    {
        uint32_t envSize = 0;
        fread(&envSize, 4, 1, fp);
        cpm.ccpEnv.clear();
        for (uint32_t i = 0; i < envSize && i < 256; i++)
        {
            std::string k = readStr(fp);
            std::string v = readStr(fp);
            cpm.ccpEnv[k] = v;
        }
        cpm.printerBuffer = readStr(fp);
        fread(&cpm.serial.port, 2, 1, fp);
    }

    fclose(fp);
    return true;
}
