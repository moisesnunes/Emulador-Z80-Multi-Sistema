// cpm_ccp.cpp — CP/M 2.2 Console Command Processor (CCP)
//
// Provides an "A>" prompt, built-in commands (DIR, ERA, REN, TYPE, USER),
// and loads arbitrary .COM files into the TPA so they can be executed.
// All I/O goes through TerminalState::putChar / inputQueue, keeping the
// ImGui rendering loop non-blocking.

#include "cpm_ccp.h"
#include <cstring>
#include <cctype>
#include <cstdio>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

// ── Helpers ──────────────────────────────────────────────────────────────────

static void CCPPrint(CPMState &cpm, const char *s)
{
    for (; *s; ++s)
    {
        if (*s == '\n')
            cpm.consoleOut('\r');
        cpm.consoleOut(*s);
    }
}

static std::string CCPUpper(std::string s)
{
    for (char &c : s)
        c = (char)toupper((unsigned char)c);
    return s;
}

// Expand $VAR and %VAR% in an already-uppercased line.
// USER is computed dynamically from cpm.currentUser; other vars come from ccpEnv.
static std::string CCPExpandVars(const CPMState &cpm, const std::string &line)
{
    auto lookup = [&](const std::string &name) -> std::string
    {
        if (name == "USER")
        {
            char buf[4];
            snprintf(buf, sizeof(buf), "%d", cpm.currentUser);
            return buf;
        }
        auto it = cpm.ccpEnv.find(name);
        return (it != cpm.ccpEnv.end()) ? it->second : "";
    };

    std::string result;
    result.reserve(line.size());
    for (size_t i = 0; i < line.size();)
    {
        if (line[i] == '$' && i + 1 < line.size() &&
            (isalpha((unsigned char)line[i + 1]) || line[i + 1] == '_'))
        {
            size_t end = i + 1;
            while (end < line.size() &&
                   (isalnum((unsigned char)line[end]) || line[end] == '_'))
                ++end;
            result += lookup(line.substr(i + 1, end - i - 1));
            i = end;
        }
        else if (line[i] == '%')
        {
            size_t close = line.find('%', i + 1);
            if (close != std::string::npos && close > i + 1)
            {
                result += lookup(line.substr(i + 1, close - i - 1));
                i = close + 1;
            }
            else
            {
                result += line[i++];
            }
        }
        else
        {
            result += line[i++];
        }
    }
    return result;
}

// Simple wildcard match: '?' = any char, '*' = any sequence.
static bool CCPMatch(const std::string &name, const std::string &pat)
{
    size_t ni = 0, pi = 0, star = std::string::npos, lastN = 0;
    while (ni < name.size())
    {
        if (pi < pat.size() && (pat[pi] == '?' || pat[pi] == name[ni]))
        {
            ni++;
            pi++;
        }
        else if (pi < pat.size() && pat[pi] == '*')
        {
            star = pi++;
            lastN = ni;
        }
        else if (star != std::string::npos)
        {
            pi = star + 1;
            ni = ++lastN;
        }
        else
        {
            return false;
        }
    }
    while (pi < pat.size() && pat[pi] == '*')
        pi++;
    return pi == pat.size();
}

// Parse "DRIVE:NAME.EXT" into an FCB at fcbAddr.
static void CCPSetupFCB(intel8080 *cpu, uint16_t fcbAddr, const std::string &token)
{
    memset(&cpu->memory[fcbAddr], 0, 36);
    memset(&cpu->memory[fcbAddr + 1], ' ', 11);
    if (token.empty())
        return;

    std::string t = CCPUpper(token);
    size_t i = 0;
    // Drive letter?
    if (t.size() >= 2 && t[1] == ':')
    {
        cpu->memory[fcbAddr] = (uint8_t)(t[0] - 'A' + 1);
        i = 2;
    }
    std::string rest = t.substr(i);
    auto dot = rest.find('.');
    std::string nm = (dot != std::string::npos) ? rest.substr(0, dot) : rest;
    std::string ex = (dot != std::string::npos) ? rest.substr(dot + 1) : "";

    // Expand '*' → fill remaining chars with '?' (per CP/M CCP CONVERT routine).
    bool starName = false;
    for (size_t k = 0; k < 8; k++)
    {
        if (starName || (k < nm.size() && nm[k] == '*'))
        {
            starName = true;
            cpu->memory[fcbAddr + 1 + k] = '?';
        }
        else if (k < nm.size())
        {
            cpu->memory[fcbAddr + 1 + k] = (uint8_t)nm[k];
        }
    }
    bool starExt = false;
    for (size_t k = 0; k < 3; k++)
    {
        if (starExt || (k < ex.size() && ex[k] == '*'))
        {
            starExt = true;
            cpu->memory[fcbAddr + 9 + k] = '?';
        }
        else if (k < ex.size())
        {
            cpu->memory[fcbAddr + 9 + k] = (uint8_t)ex[k];
        }
    }
}

// ── Drive resolution helper ───────────────────────────────────────────────────

// Given a token that may start with "D:", returns the host directory for that
// drive and strips the prefix from `token` in-place.
static const std::string &CCPResolveDir(CPMState &cpm, std::string &token)
{
    if (token.size() >= 2 && token[1] == ':')
    {
        int d = token[0] - 'A';
        token = token.substr(2);
        return cpm.driveDir(d);
    }
    return cpm.currentDiskDir();
}

// ── Built-in commands ─────────────────────────────────────────────────────────

static void CCPBuiltinDir(CPMState &cpm, const std::string &args)
{
    std::string pat = args.empty() ? "*.*" : CCPUpper(args);
    const std::string &dirPath = CCPResolveDir(cpm, pat);
    if (pat.empty())
        pat = "*.*";

    DIR *dir = opendir(dirPath.c_str());
    if (!dir)
    {
        CCPPrint(cpm, "No files\n");
        return;
    }

    char driveLetter = (char)('A' + cpm.currentDrive);
    char header[6];
    snprintf(header, sizeof(header), "%c: ", driveLetter);
    CCPPrint(cpm, "\n");
    CCPPrint(cpm, header);

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr)
    {
        if (ent->d_name[0] == '.')
            continue;
        std::string uname = CCPUpper(ent->d_name);
        if (!CCPMatch(uname, pat))
            continue;

        auto dotpos = uname.rfind('.');
        std::string nm = (dotpos != std::string::npos) ? uname.substr(0, dotpos) : uname;
        std::string ex = (dotpos != std::string::npos) ? uname.substr(dotpos + 1) : "   ";
        while (nm.size() < 8)
            nm += ' ';
        while (ex.size() < 3)
            ex += ' ';

        if (count > 0)
        {
            if (count % 4 == 0)
            { // new line — reprint drive header (matches real CP/M DIR)
                CCPPrint(cpm, "\n");
                CCPPrint(cpm, header);
            }
            else
            {
                CCPPrint(cpm, " : ");
            }
        }
        char entry[13];
        snprintf(entry, sizeof(entry), "%.8s %.3s", nm.c_str(), ex.c_str());
        CCPPrint(cpm, entry);
        count++;
    }
    closedir(dir);

    if (count == 0)
        CCPPrint(cpm, "No file");
    CCPPrint(cpm, "\n");
}

static void CCPBuiltinEra(CPMState &cpm, const std::string &args, bool confirmed = false)
{
    if (args.empty())
    {
        CCPPrint(cpm, "ERA requires a filename\n");
        return;
    }
    std::string pat = CCPUpper(args);
    const std::string &dirPath = CCPResolveDir(cpm, pat);
    if (pat.empty())
        pat = "*.*";

    // Require explicit confirmation before erasing all files (matches CP/M CCP ERASE).
    if (!confirmed && (pat == "*.*" || pat == "*"))
    {
        cpm.ccpEraConfirm = true;
        cpm.ccpEraPendingArgs = args; // preserve drive prefix if present
        cpm.ccpPrompted = false;
        return;
    }

    DIR *dir = opendir(dirPath.c_str());
    if (!dir)
        return;
    struct dirent *ent;
    bool found = false;
    while ((ent = readdir(dir)) != nullptr)
    {
        if (ent->d_name[0] == '.')
            continue;
        std::string uname = CCPUpper(ent->d_name);
        if (CCPMatch(uname, pat))
        {
            std::string path = dirPath + "/" + ent->d_name;
            remove(path.c_str());
            found = true;
        }
    }
    closedir(dir);
    if (!found)
        CCPPrint(cpm, "\nNo file\n");
}

static void CCPBuiltinRen(CPMState &cpm, const std::string &args)
{
    // Syntax: REN NEW=OLD  or  REN NEW OLD
    std::string a = CCPUpper(args);
    size_t sep = a.find('=');
    if (sep == std::string::npos)
        sep = a.find(' ');
    if (sep == std::string::npos)
    {
        CCPPrint(cpm, "REN: use NEW=OLD\n");
        return;
    }
    std::string newName = a.substr(0, sep);
    std::string oldName = a.substr(sep + 1);
    while (!oldName.empty() && oldName[0] == ' ')
        oldName = oldName.substr(1);
    std::string oldPath = cpm.currentDiskDir() + "/" + oldName;
    std::string newPath = cpm.currentDiskDir() + "/" + newName;
    if (rename(oldPath.c_str(), newPath.c_str()) != 0)
        CCPPrint(cpm, "REN failed\n");
}

static void CCPBuiltinType(CPMState &cpm, const std::string &args)
{
    if (args.empty())
    {
        CCPPrint(cpm, "TYPE requires a filename\n");
        return;
    }
    std::string filename = CCPUpper(args);
    const std::string &dirPath = CCPResolveDir(cpm, filename);
    std::string path = dirPath + "/" + filename;
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp)
    {
        CCPPrint(cpm, args.c_str());
        CCPPrint(cpm, ": not found\n");
        return;
    }
    CCPPrint(cpm, "\n");
    int ch;
    while ((ch = fgetc(fp)) != EOF)
    {
        if (ch == 0x1A)
            break; // CP/M EOF marker
        if (ch == '\n')
            cpm.consoleOut('\r');
        cpm.consoleOut((char)ch);
    }
    fclose(fp);
    CCPPrint(cpm, "\n");
}

static void CCPBuiltinCreate(CPMState &cpm, const std::string &args)
{
    if (args.empty())
    {
        CCPPrint(cpm, "CREATE requires a filename\n");
        return;
    }
    std::string filename = CCPUpper(args);
    const std::string &dirPath = CCPResolveDir(cpm, filename);
    std::string path = dirPath + "/" + filename;
    FILE *fp = fopen(path.c_str(), "wb");
    if (!fp)
    {
        CCPPrint(cpm, "CREATE failed\n");
        return;
    }
    fclose(fp);
}

static void CCPBuiltinStat(CPMState &cpm, const std::string &args)
{
    if (args.empty() || CCPUpper(args) == "DSK:")
    {
        const std::string &dirPath = cpm.currentDiskDir();
        struct statvfs vfs;
        if (statvfs(dirPath.c_str(), &vfs) == 0)
        {
            unsigned long bsize = vfs.f_bsize ? vfs.f_bsize : 512;
            unsigned long totalK = (unsigned long)(vfs.f_blocks * bsize / 1024);
            unsigned long freeK = (unsigned long)(vfs.f_bavail * bsize / 1024);
            char buf[128];
            char drive = (char)('A' + cpm.currentDrive);
            snprintf(buf, sizeof(buf),
                     "\nDrive %c: %luK of %luK available\n", drive, freeK, totalK);
            CCPPrint(cpm, buf);
        }
        else
        {
            CCPPrint(cpm, "\nDisk stat unavailable\n");
        }
    }
    else
    {
        std::string pat = CCPUpper(args);
        const std::string &dirPath = CCPResolveDir(cpm, pat);
        if (pat.empty())
            pat = "*.*";

        DIR *dir = opendir(dirPath.c_str());
        if (!dir)
        {
            CCPPrint(cpm, "No files\n");
            return;
        }

        CCPPrint(cpm, "\n");
        bool found = false;
        struct dirent *ent;
        while ((ent = readdir(dir)) != nullptr)
        {
            if (ent->d_name[0] == '.')
                continue;
            std::string uname = CCPUpper(ent->d_name);
            if (!CCPMatch(uname, pat))
                continue;
            std::string fullPath = dirPath + "/" + ent->d_name;
            struct stat st;
            if (stat(fullPath.c_str(), &st) == 0)
            {
                long recs = (st.st_size + 127) / 128;
                long kb = (recs + 7) / 8;
                char buf[64];
                snprintf(buf, sizeof(buf), "%-12s  %4ldK\n", uname.c_str(), kb);
                CCPPrint(cpm, buf);
                found = true;
            }
        }
        closedir(dir);
        if (!found)
            CCPPrint(cpm, "No files\n");
    }
}

static void CCPBuiltinUser(CPMState &cpm, const std::string &args)
{
    if (args.empty())
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "User: %d\n", cpm.currentUser);
        CCPPrint(cpm, buf);
        return;
    }
    int n = atoi(args.c_str());
    if (n >= 0 && n <= 15)
        cpm.currentUser = (uint8_t)n;
    else
        CCPPrint(cpm, "User 0-15\n");
}

static void CCPBuiltinSet(CPMState &cpm, const std::string &args)
{
    auto printVar = [&](const std::string &name, const std::string &val)
    {
        CCPPrint(cpm, name.c_str());
        CCPPrint(cpm, "=");
        CCPPrint(cpm, val.c_str());
        CCPPrint(cpm, "\n");
    };

    if (args.empty())
    {
        char ubuf[4];
        snprintf(ubuf, sizeof(ubuf), "%d", cpm.currentUser);
        printVar("USER", ubuf);
        for (auto &kv : cpm.ccpEnv)
            printVar(kv.first, kv.second);
        return;
    }

    size_t eq = args.find('=');
    if (eq == std::string::npos)
    {
        std::string name = args;
        while (!name.empty() && name.back() == ' ')
            name.pop_back();
        if (name == "USER")
        {
            char buf[16];
            snprintf(buf, sizeof(buf), "USER=%d\n", cpm.currentUser);
            CCPPrint(cpm, buf);
        }
        else
        {
            auto it = cpm.ccpEnv.find(name);
            if (it != cpm.ccpEnv.end())
                printVar(name, it->second);
            else
            {
                CCPPrint(cpm, name.c_str());
                CCPPrint(cpm, ": undefined\n");
            }
        }
        return;
    }

    std::string name = args.substr(0, eq);
    while (!name.empty() && name.back() == ' ')
        name.pop_back();
    std::string val = args.substr(eq + 1);

    if (name == "USER")
    {
        CCPPrint(cpm, "USER: read-only, use USER command\n");
        return;
    }

    if (val.empty())
        cpm.ccpEnv.erase(name);
    else
        cpm.ccpEnv[name] = val;
}

static void CCPBuiltinMount(CPMState &cpm, const std::string &args)
{
    std::string a = args;
    while (!a.empty() && a[0] == ' ')
        a = a.substr(1);
    if (a.size() < 2 || a[1] != ':')
    {
        CCPPrint(cpm, "MOUNT: usage: MOUNT A: [file.img [spt bsh dsm drm off]]\n");
        return;
    }
    int drive = toupper((unsigned char)a[0]) - 'A';
    if (drive < 0 || drive >= MAX_DRIVES)
    {
        CCPPrint(cpm, "MOUNT: drive must be A-P\n");
        return;
    }
    std::string rest = a.substr(2);
    while (!rest.empty() && rest[0] == ' ')
        rest = rest.substr(1);

    if (rest.empty())
    {
        // Show mount status for this drive.
        if (cpm.diskImages[drive] && cpm.diskImages[drive]->isOpen())
        {
            DskImage *d = cpm.diskImages[drive];
            char buf[160];
            snprintf(buf, sizeof(buf), "%s%s\n  spt=%d bsh=%d dsm=%d drm=%d off=%d%s\n",
                     d->path.c_str(), d->readOnly ? " (R/O)" : "",
                     d->spt, d->bsh, d->dsm, d->drm, d->off,
                     d->skewTable.empty() ? "" : " skew");
            CCPPrint(cpm, buf);
        }
        else
        {
            CCPPrint(cpm, "No image mounted\n");
        }
        return;
    }

    // Split rest into path token and optional geometry integers.
    std::string path;
    int geoVals[5] = {0, 0, 0, 0, 0};
    int geoCount = 0;
    {
        // First token = path; remaining tokens = optional geometry (spt bsh dsm drm off).
        size_t sp = rest.find(' ');
        path = (sp == std::string::npos) ? rest : rest.substr(0, sp);
        if (sp != std::string::npos)
        {
            const char *p = rest.c_str() + sp;
            while (*p && geoCount < 5)
            {
                while (*p == ' ')
                    ++p;
                if (!*p)
                    break;
                geoVals[geoCount++] = atoi(p);
                while (*p && *p != ' ')
                    ++p;
            }
        }
    }

    if (path[0] != '/')
        path = cpm.diskDirs[0] + "/" + path;

    bool ok;
    if (geoCount == 5)
    {
        DskGeometry geo;
        geo.spt = geoVals[0];
        geo.bsh = geoVals[1];
        geo.dsm = geoVals[2];
        geo.drm = geoVals[3];
        geo.off = geoVals[4];
        ok = DskMountWithGeometry(cpm, drive, path, geo);
    }
    else
    {
        ok = DskMount(cpm, drive, path);
    }

    if (!ok)
    {
        CCPPrint(cpm, "MOUNT: cannot open image\n");
        return;
    }

    DskImage *d = cpm.diskImages[drive];
    char buf[160];
    snprintf(buf, sizeof(buf), "Drive %c: mounted%s  spt=%d bsh=%d dsm=%d drm=%d off=%d%s\n",
             'A' + drive, d->readOnly ? " (R/O)" : "",
             d->spt, d->bsh, d->dsm, d->drm, d->off,
             d->skewTable.empty() ? "" : " skew");
    CCPPrint(cpm, buf);
}

static void CCPBuiltinUmount(CPMState &cpm, const std::string &args)
{
    std::string a = args;
    while (!a.empty() && a[0] == ' ')
        a = a.substr(1);
    if (a.empty() || a[0] < 'A')
    {
        CCPPrint(cpm, "UMOUNT: usage: UMOUNT A:\n");
        return;
    }
    int drive = toupper((unsigned char)a[0]) - 'A';
    if (drive < 0 || drive >= MAX_DRIVES)
    {
        CCPPrint(cpm, "UMOUNT: drive must be A-P\n");
        return;
    }
    DskUnmount(cpm, drive);
    char buf[32];
    snprintf(buf, sizeof(buf), "Drive %c: unmounted\n", 'A' + drive);
    CCPPrint(cpm, buf);
}

static void CCPBuiltinHelp(CPMState &cpm)
{
    CCPPrint(cpm, "  DIR  [spec]              list files (wildcards: *.*, *.COM)\n");
    CCPPrint(cpm, "  ERA  <spec>              delete file(s)\n");
    CCPPrint(cpm, "  REN  new=old             rename file\n");
    CCPPrint(cpm, "  TYPE <file>              display text file\n");
    CCPPrint(cpm, "  CREATE <file>            create empty file\n");
    CCPPrint(cpm, "  SAVE n <file>            save n*256 bytes from TPA (0x0100)\n");
    CCPPrint(cpm, "  DUMP <file>              hex dump of file\n");
    CCPPrint(cpm, "  PIP  dest=src            copy file\n");
    CCPPrint(cpm, "  STAT [spec|DSK:]         file or disk usage info\n");
    CCPPrint(cpm, "  MOUNT A: [f.img]         mount disk image (auto-detect geometry)\n");
    CCPPrint(cpm, "  MOUNT A: f.img           spt bsh dsm drm off\n");
    CCPPrint(cpm, "  UMOUNT A:                unmount drive\n");
    CCPPrint(cpm, "  A: B: C: D:              change current drive\n");
    CCPPrint(cpm, "  ED     <file>            line editor (A/D/I/L/E/Q)\n");
    CCPPrint(cpm, "  SUBMIT <file>            run batch .SUB file\n");
    CCPPrint(cpm, "  USER   [n]               set/show user area (0-15)\n");
    CCPPrint(cpm, "  SET    [var[=val]]       set/show env vars ($VAR or %%VAR%%)\n");
    CCPPrint(cpm, "  PATH=A: B:               search A: then B: for .COM files\n");
    CCPPrint(cpm, "  VER                      show emulator version\n");
    CCPPrint(cpm, "  CLS                      clear screen\n");
    CCPPrint(cpm, "  READER [path]            set/show reader device (fn 03h)\n");
    CCPPrint(cpm, "  PUNCH  [path]            set/show punch output (fn 04h)\n");
    CCPPrint(cpm, "  PRINTER [path]           set/show printer output (fn 05h)\n");
    CCPPrint(cpm, "  HELP | ?                 show this help\n");
    CCPPrint(cpm, "  cmd > file               redirect output to file\n");
    CCPPrint(cpm, "  cmd < file               redirect input from file\n");
    CCPPrint(cpm, "  cmd1 | cmd2              pipe output of cmd1 into cmd2\n");
    CCPPrint(cpm, "\n");
}

static void CCPBuiltinSave(intel8080 *cpu, CPMState &cpm, const std::string &args)
{
    // SAVE n filename — save n*256 bytes of TPA starting at 0x0100.
    size_t sp = args.find(' ');
    if (sp == std::string::npos)
    {
        CCPPrint(cpm, "SAVE: use SAVE n filename\n");
        return;
    }
    int pages = atoi(args.substr(0, sp).c_str());
    std::string filename = CCPUpper(args.substr(sp + 1));
    while (!filename.empty() && filename[0] == ' ')
        filename = filename.substr(1);
    if (pages <= 0 || pages > 255)
    {
        CCPPrint(cpm, "SAVE: invalid page count (1-255)\n");
        return;
    }
    const std::string &dirPath = CCPResolveDir(cpm, filename);
    std::string path = dirPath + "/" + filename;
    FILE *fp = fopen(path.c_str(), "wb");
    if (!fp)
    {
        CCPPrint(cpm, "SAVE: cannot create file\n");
        return;
    }
    fwrite(&cpu->memory[0x0100], 1, (size_t)pages * 256, fp);
    fclose(fp);
}

static void CCPBuiltinDump(CPMState &cpm, const std::string &args)
{
    if (args.empty())
    {
        CCPPrint(cpm, "DUMP: filename required\n");
        return;
    }
    std::string filename = CCPUpper(args);
    const std::string &dirPath = CCPResolveDir(cpm, filename);
    std::string path = dirPath + "/" + filename;
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp)
    {
        CCPPrint(cpm, args.c_str());
        CCPPrint(cpm, ": not found\n");
        return;
    }
    CCPPrint(cpm, "\n");
    uint8_t buf[16];
    uint16_t addr = 0;
    size_t n;
    while ((n = fread(buf, 1, 16, fp)) > 0)
    {
        char line[80];
        int pos = snprintf(line, sizeof(line), "%04X  ", addr);
        for (size_t i = 0; i < 16; i++)
        {
            if (i < n)
                pos += snprintf(line + pos, sizeof(line) - pos, "%02X ", buf[i]);
            else
                pos += snprintf(line + pos, sizeof(line) - pos, "   ");
            if (i == 7)
                line[pos++] = ' ';
        }
        line[pos++] = ' ';
        for (size_t i = 0; i < n; i++)
            line[pos++] = (buf[i] >= 0x20 && buf[i] < 0x7F) ? (char)buf[i] : '.';
        line[pos] = '\0';
        CCPPrint(cpm, line);
        CCPPrint(cpm, "\n");
        addr = (uint16_t)(addr + 16);
    }
    fclose(fp);
}

static void CCPBuiltinPip(CPMState &cpm, const std::string &args)
{
    // PIP dest=src  —  basic file copy.
    std::string a = CCPUpper(args);
    size_t eq = a.find('=');
    if (eq == std::string::npos)
    {
        CCPPrint(cpm, "PIP: use PIP dest=src\n");
        return;
    }
    std::string destName = a.substr(0, eq);
    std::string srcName = a.substr(eq + 1);
    while (!srcName.empty() && srcName[0] == ' ')
        srcName = srcName.substr(1);
    const std::string destDir = CCPResolveDir(cpm, destName);
    const std::string srcDir = CCPResolveDir(cpm, srcName);
    FILE *src = fopen((srcDir + "/" + srcName).c_str(), "rb");
    if (!src)
    {
        CCPPrint(cpm, srcName.c_str());
        CCPPrint(cpm, ": not found\n");
        return;
    }
    FILE *dst = fopen((destDir + "/" + destName).c_str(), "wb");
    if (!dst)
    {
        fclose(src);
        CCPPrint(cpm, destName.c_str());
        CCPPrint(cpm, ": cannot create\n");
        return;
    }
    uint8_t buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0)
        fwrite(buf, 1, n, dst);
    fclose(src);
    fclose(dst);
}

static void CCPBuiltinSubmit(CPMState &cpm, const std::string &args)
{
    if (args.empty())
    {
        CCPPrint(cpm, "SUBMIT: filename required\n");
        return;
    }
    std::string filename = CCPUpper(args);
    const std::string &dirPath = CCPResolveDir(cpm, filename);
    // Try plain name first, then with .SUB extension.
    std::string path = dirPath + "/" + filename;
    FILE *fp = fopen(path.c_str(), "r");
    if (!fp)
    {
        path = dirPath + "/" + filename + ".SUB";
        fp = fopen(path.c_str(), "r");
    }
    if (!fp)
    {
        CCPPrint(cpm, args.c_str());
        CCPPrint(cpm, ": not found\n");
        return;
    }
    cpm.submitQueue.clear();
    char buf[130];
    while (fgets(buf, sizeof(buf), fp))
    {
        std::string line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        if (!line.empty())
            cpm.submitQueue.push_back(CCPUpper(line));
    }
    fclose(fp);
    char msg[48];
    snprintf(msg, sizeof(msg), "%d command(s) queued\n", (int)cpm.submitQueue.size());
    CCPPrint(cpm, msg);
}

static void CCPBuiltinED(CPMState &cpm, const std::string &args)
{
    if (args.empty())
    {
        CCPPrint(cpm, "ED: filename required\n");
        return;
    }
    std::string filename = CCPUpper(args);
    const std::string &dirPath = CCPResolveDir(cpm, filename);
    std::string path = dirPath + "/" + filename;

    cpm.editorLines.clear();
    FILE *fp = fopen(path.c_str(), "r");
    if (fp)
    {
        char buf[258];
        while (fgets(buf, sizeof(buf), fp))
        {
            std::string line(buf);
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                line.pop_back();
            cpm.editorLines.push_back(line);
        }
        fclose(fp);
    }
    cpm.editorFilePath = path;
    cpm.editorActive = true;
    cpm.editorModified = false;
    cpm.editorCmdBuf.clear();
    CCPPrint(cpm, "\n*");
}

// Process one frame of ED input.  Returns true if the editor exited (saved/quit).
static bool CCPTickEditor(CPMState &cpm)
{
    while (!cpm.terminal.inputQueue.empty())
    {
        uint8_t ch = cpm.terminal.inputQueue.front();
        cpm.terminal.inputQueue.erase(cpm.terminal.inputQueue.begin());

        if (ch == 0x0D || ch == 0x0A)
        {
            std::string cmd = cpm.editorCmdBuf;
            cpm.editorCmdBuf.clear();
            CCPPrint(cpm, "\n");

            if (cmd.empty())
            {
                CCPPrint(cpm, "*");
                continue;
            }
            char op = (char)toupper((unsigned char)cmd[0]);
            std::string arg = cmd.size() > 1 ? cmd.substr(1) : "";
            while (!arg.empty() && arg[0] == ' ')
                arg = arg.substr(1);

            if (op == 'E')
            { // Exit + save
                FILE *out = fopen(cpm.editorFilePath.c_str(), "w");
                if (out)
                {
                    for (auto &ln : cpm.editorLines)
                        fprintf(out, "%s\n", ln.c_str());
                    fclose(out);
                }
                cpm.editorActive = false;
                cpm.editorLines.clear();
                return true;
            }
            else if (op == 'Q')
            { // Quit, no save
                cpm.editorActive = false;
                cpm.editorLines.clear();
                return true;
            }
            else if (op == 'A')
            { // Append line
                cpm.editorLines.push_back(arg);
                cpm.editorModified = true;
                CCPPrint(cpm, "*");
            }
            else if (op == 'D')
            { // Delete line n
                int n = arg.empty() ? 0 : (atoi(arg.c_str()) - 1);
                if (n >= 0 && n < (int)cpm.editorLines.size())
                {
                    cpm.editorLines.erase(cpm.editorLines.begin() + n);
                    cpm.editorModified = true;
                }
                CCPPrint(cpm, "*");
            }
            else if (op == 'L')
            { // List lines
                for (size_t i = 0; i < cpm.editorLines.size(); i++)
                {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%4zu: ", i + 1);
                    CCPPrint(cpm, buf);
                    CCPPrint(cpm, cpm.editorLines[i].c_str());
                    CCPPrint(cpm, "\n");
                }
                CCPPrint(cpm, "*");
            }
            else if (op == 'I')
            { // Insert before line n
                int n = arg.empty() ? 0 : (atoi(arg.c_str()) - 1);
                if (n < 0)
                    n = 0;
                if (n > (int)cpm.editorLines.size())
                    n = (int)cpm.editorLines.size();
                cpm.editorLines.insert(cpm.editorLines.begin() + n, "");
                cpm.editorModified = true;
                CCPPrint(cpm, "*");
            }
            else
            {
                CCPPrint(cpm, "?\n*");
            }
        }
        else if (ch == 0x08 || ch == 0x7F)
        {
            if (!cpm.editorCmdBuf.empty())
            {
                cpm.editorCmdBuf.pop_back();
                cpm.terminal.putChar('\b');
                cpm.terminal.putChar(' ');
                cpm.terminal.putChar('\b');
            }
        }
        else if (ch >= 0x20)
        {
            cpm.editorCmdBuf += (char)ch;
            cpm.terminal.putChar((char)ch);
        }
    }
    return false; // still editing
}

// ── .COM loader ───────────────────────────────────────────────────────────────

bool CCPLoadCom(intel8080 *cpu, CPMState &cpm,
                const std::string &name, const std::string &args)
{
    std::string uname = CCPUpper(name);
    bool hasDrivePrefix = (uname.size() >= 2 && uname[1] == ':' &&
                           isalpha((unsigned char)uname[0]));
    const std::string &comDir = CCPResolveDir(cpm, uname);

    // Try NAME.COM, then NAME as-is (might already carry extension).
    std::string path = comDir + "/" + uname + ".COM";
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp)
    {
        path = comDir + "/" + uname;
        fp = fopen(path.c_str(), "rb");
    }

    // If not found and no explicit drive given, search PATH drives.
    if (!fp && !hasDrivePrefix)
    {
        auto it = cpm.ccpEnv.find("PATH");
        if (it != cpm.ccpEnv.end())
        {
            const std::string &pathVal = it->second;
            for (size_t i = 0; i < pathVal.size() && !fp;)
            {
                while (i < pathVal.size() &&
                       (pathVal[i] == ' ' || pathVal[i] == ';'))
                    ++i;
                if (i + 1 < pathVal.size() &&
                    isalpha((unsigned char)pathVal[i]) && pathVal[i + 1] == ':')
                {
                    int d = toupper((unsigned char)pathVal[i]) - 'A';
                    i += 2;
                    if (d >= 0 && d < MAX_DRIVES && !cpm.diskDirs[d].empty())
                    {
                        path = cpm.diskDirs[d] + "/" + uname + ".COM";
                        fp = fopen(path.c_str(), "rb");
                        if (!fp)
                        {
                            path = cpm.diskDirs[d] + "/" + uname;
                            fp = fopen(path.c_str(), "rb");
                        }
                    }
                }
                else
                    ++i;
            }
        }
    }

    if (!fp)
    {
        CCPPrint(cpm, name.c_str());
        CCPPrint(cpm, "?\n");
        return false;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    // Resident code must fit below the overlay region (or below BDOS if none defined).
    uint16_t residentTop = cpm.overlayBase ? cpm.overlayBase : BDOS_ADDR;
    if (size > (long)(residentTop - 0x0100))
    {
        fclose(fp);
        CCPPrint(cpm, "Program too large\n");
        return false;
    }

    // Clear only the resident code area [0x0100..residentTop).
    // The overlay region [overlayBase..overlayTop) is preserved so a warm
    // re-run does not force the overlay loader to re-fetch cached overlays.
    memset(&cpu->memory[0x0100], 0, residentTop - 0x0100);
    fread(&cpu->memory[0x0100], 1, (size_t)size, fp);
    fclose(fp);

    CPMCloseAllFiles(cpm);

    // Command tail at 0x0080: length byte + " " + args.
    std::string tail = args.empty() ? "" : (" " + args);
    size_t tlen = std::min(tail.size(), (size_t)126);
    cpu->memory[0x0080] = (uint8_t)tlen;
    memcpy(&cpu->memory[0x0081], tail.c_str(), tlen);
    if (tlen < 127)
        cpu->memory[0x0081 + tlen] = 0x0D;

    // FCBs from first two whitespace-separated tokens in args.
    std::string a1, a2;
    size_t sp = args.find(' ');
    if (sp == std::string::npos)
    {
        a1 = args;
    }
    else
    {
        a1 = args.substr(0, sp);
        a2 = args.substr(sp + 1);
        while (!a2.empty() && a2[0] == ' ')
            a2 = a2.substr(1);
    }
    CCPSetupFCB(cpu, 0x005C, a1);
    CCPSetupFCB(cpu, 0x006C, a2);

    // Reset CPU.
    cpu->PC = 0x0100;
    cpu->SP = BDOS_ADDR - 2;
    cpu->memory[BDOS_ADDR - 2] = 0x00; // return address → warm-boot (0x0000)
    cpu->memory[BDOS_ADDR - 1] = 0x00;
    cpm.dmaAddress = 0x0080;
    cpm.lineInputActive = false;

    return true;
}

// ── I/O redirection & pipe helpers ───────────────────────────────────────────

struct CCPSegment
{
    std::string cmd;
    std::string args;
    std::string inFile;
    std::string outFile;
};

// Tokenize: split on whitespace; treat '<' '>' '|' as lone tokens.
static std::vector<std::string> CCPTokenize(const std::string &line)
{
    std::vector<std::string> tokens;
    std::string tok;
    for (char c : line)
    {
        if (c == ' ' || c == '\t')
        {
            if (!tok.empty())
            {
                tokens.push_back(tok);
                tok.clear();
            }
        }
        else if (c == '<' || c == '>' || c == '|')
        {
            if (!tok.empty())
            {
                tokens.push_back(tok);
                tok.clear();
            }
            tokens.push_back(std::string(1, c));
        }
        else
        {
            tok += c;
        }
    }
    if (!tok.empty())
        tokens.push_back(tok);
    return tokens;
}

// Split a tokenized command line into pipe segments, each with optional < >.
static std::vector<CCPSegment> CCPParseSegments(const std::string &uline)
{
    auto tokens = CCPTokenize(uline);
    std::vector<CCPSegment> segs;
    CCPSegment cur;
    std::vector<std::string> words;
    bool wantIn = false, wantOut = false;

    auto flush = [&]()
    {
        if (!words.empty())
        {
            cur.cmd = words[0];
            for (size_t i = 1; i < words.size(); i++)
            {
                if (i > 1)
                    cur.args += ' ';
                cur.args += words[i];
            }
        }
        segs.push_back(cur);
        cur = CCPSegment{};
        words.clear();
        wantIn = wantOut = false;
    };

    for (auto &t : tokens)
    {
        if (t == "|")
        {
            flush();
        }
        else if (t == "<")
        {
            wantIn = true;
            wantOut = false;
        }
        else if (t == ">")
        {
            wantOut = true;
            wantIn = false;
        }
        else if (wantIn)
        {
            cur.inFile = t;
            wantIn = false;
        }
        else if (wantOut)
        {
            cur.outFile = t;
            wantOut = false;
        }
        else
        {
            words.push_back(t);
        }
    }
    flush();
    return segs;
}

// Inject a host file's contents into the terminal input queue (for < redirect).
static void CCPInjectInputFile(CPMState &cpm, const std::string &filename)
{
    std::string tok = filename;
    const std::string &dir = CCPResolveDir(cpm, tok);
    FILE *fp = fopen((dir + "/" + tok).c_str(), "rb");
    if (!fp)
    {
        CCPPrint(cpm, tok.c_str());
        CCPPrint(cpm, ": not found\n");
        return;
    }
    int c;
    while ((c = fgetc(fp)) != EOF && c != 0x1A)
        cpm.terminal.inputQueue.push_back((uint8_t)c);
    cpm.terminal.inputQueue.push_back(0x1A); // Ctrl-Z = CP/M EOF
    fclose(fp);
}

// Dispatch a single command (built-in or .COM) — returns true if .COM loaded.
static bool CCPDispatch(intel8080 *cpu, CPMState &cpm,
                        const std::string &cmd, const std::string &args)
{
    if (cmd.size() == 2 && cmd[1] == ':' && cmd[0] >= 'A' && cmd[0] <= 'P')
    {
        cpm.currentDrive = (uint8_t)(cmd[0] - 'A');
        return false;
    }
    if (cmd == "DIR" || cmd == "LS")
    {
        CCPBuiltinDir(cpm, args);
        return false;
    }
    if (cmd == "ERA" || cmd == "DEL")
    {
        CCPBuiltinEra(cpm, args);
        return false;
    }
    if (cmd == "REN" || cmd == "MV")
    {
        CCPBuiltinRen(cpm, args);
        return false;
    }
    if (cmd == "TYPE" || cmd == "CAT")
    {
        CCPBuiltinType(cpm, args);
        return false;
    }
    if (cmd == "CREATE" || cmd == "NEW")
    {
        CCPBuiltinCreate(cpm, args);
        return false;
    }
    if (cmd == "USER")
    {
        CCPBuiltinUser(cpm, args);
        return false;
    }
    if (cmd == "SET")
    {
        CCPBuiltinSet(cpm, args);
        return false;
    }
    if (cmd == "STAT")
    {
        CCPBuiltinStat(cpm, args);
        return false;
    }
    if (cmd == "ED")
    {
        CCPBuiltinED(cpm, args);
        return false;
    }
    if (cmd == "MOUNT")
    {
        CCPBuiltinMount(cpm, args);
        return false;
    }
    if (cmd == "UMOUNT")
    {
        CCPBuiltinUmount(cpm, args);
        return false;
    }
    if (cmd == "HELP" || cmd == "?")
    {
        CCPBuiltinHelp(cpm);
        return false;
    }
    if (cmd == "SAVE")
    {
        CCPBuiltinSave(cpu, cpm, args);
        return false;
    }
    if (cmd == "DUMP")
    {
        CCPBuiltinDump(cpm, args);
        return false;
    }
    if (cmd == "PIP")
    {
        CCPBuiltinPip(cpm, args);
        return false;
    }
    if (cmd == "SUBMIT")
    {
        CCPBuiltinSubmit(cpm, args);
        return false;
    }
    if (cmd == "CLS")
    {
        cpm.terminal.clear();
        return false;
    }
    if (cmd == "VER")
    {
        CCPPrint(cpm, "CP/M 2.2 Emulator\n");
        return false;
    }
    if (cmd == "READER")
    {
        if (args.empty())
        {
            CCPPrint(cpm, "reader: ");
            CCPPrint(cpm, cpm.readerPath.empty() ? "(none)" : cpm.readerPath.c_str());
            CCPPrint(cpm, "\n");
        }
        else
        {
            if (cpm.readerFp)
            {
                fclose(cpm.readerFp);
                cpm.readerFp = nullptr;
            }
            cpm.readerPath = args;
        }
        return false;
    }
    if (cmd == "PUNCH")
    {
        if (args.empty())
        {
            CCPPrint(cpm, "punch: ");
            CCPPrint(cpm, cpm.punchPath.empty() ? "(default: CPM.PUN)" : cpm.punchPath.c_str());
            CCPPrint(cpm, "\n");
        }
        else
        {
            if (cpm.punchFp)
            {
                fclose(cpm.punchFp);
                cpm.punchFp = nullptr;
            }
            cpm.punchPath = args;
        }
        return false;
    }
    if (cmd == "PRINTER")
    {
        if (args.empty())
        {
            CCPPrint(cpm, "printer: ");
            CCPPrint(cpm, cpm.printerPath.empty() ? "(default: CPM.LST)" : cpm.printerPath.c_str());
            CCPPrint(cpm, "\n");
        }
        else
        {
            if (cpm.printerFp)
            {
                fclose(cpm.printerFp);
                cpm.printerFp = nullptr;
            }
            cpm.printerPath = args;
        }
        return false;
    }
    return CCPLoadCom(cpu, cpm, cmd, args);
}

// Parse a command line for < > |, set up redirects, and run.
// Returns true if a .COM was loaded and the CPU should take over.
static bool CCPParseAndDispatch(intel8080 *cpu, CPMState &cpm,
                                const std::string &uline)
{
    auto segs = CCPParseSegments(CCPExpandVars(cpm, uline));
    if (segs.empty() || segs[0].cmd.empty())
        return false;

    CCPSegment &s0 = segs[0];
    bool hasPipe = segs.size() > 1 && !segs[1].cmd.empty();

    // Input redirect for stage 1.
    if (!s0.inFile.empty())
        CCPInjectInputFile(cpm, s0.inFile);

    // Output redirect for stage 1.
    if (!s0.outFile.empty())
    {
        std::string tok = s0.outFile;
        const std::string &dir = CCPResolveDir(cpm, tok);
        cpm.redirectOut = fopen((dir + "/" + tok).c_str(), "w");
    }

    // Pipe: capture stage-1 output; stash stage-2 info for later.
    if (hasPipe)
    {
        cpm.pipeCapture = true;
        cpm.pipeBuffer.clear();
        cpm.pipeStage2Cmd = segs[1].cmd;
        cpm.pipeStage2Args = segs[1].args;
        cpm.pipeStage2OutFile = segs[1].outFile;
        cpm.pipeStage2InFile = segs[1].inFile;
        cpm.pipeStage2Ready = false;
    }

    bool running = CCPDispatch(cpu, cpm, s0.cmd, s0.args);

    if (!running)
    {
        // Stage 1 was a built-in — clean up and optionally run stage 2 now.
        if (cpm.redirectOut)
        {
            fclose(cpm.redirectOut);
            cpm.redirectOut = nullptr;
        }
        if (hasPipe)
        {
            cpm.pipeCapture = false;
            for (auto b : cpm.pipeBuffer)
                cpm.terminal.inputQueue.push_back(b);
            cpm.pipeBuffer.clear();
            if (!cpm.pipeStage2InFile.empty())
                CCPInjectInputFile(cpm, cpm.pipeStage2InFile);
            if (!cpm.pipeStage2OutFile.empty())
            {
                std::string tok = cpm.pipeStage2OutFile;
                const std::string &dir = CCPResolveDir(cpm, tok);
                cpm.redirectOut = fopen((dir + "/" + tok).c_str(), "w");
            }
            bool r2 = CCPDispatch(cpu, cpm, cpm.pipeStage2Cmd, cpm.pipeStage2Args);
            cpm.pipeStage2Cmd.clear();
            cpm.pipeStage2Args.clear();
            cpm.pipeStage2OutFile.clear();
            cpm.pipeStage2InFile.clear();
            if (!r2 && cpm.redirectOut)
            {
                fclose(cpm.redirectOut);
                cpm.redirectOut = nullptr;
            }
            return r2;
        }
    }
    // .COM loaded — cleanup happens when CCPTick resumes after warm-boot.
    return running;
}

// ── CCPInit ───────────────────────────────────────────────────────────────────

void CCPInit(intel8080 *cpu, CPMState &cpm, const std::string &diskDir)
{
    CPMInit(cpu, cpm, diskDir);
    cpm.ccpMode = true;
    cpm.ccpRunning = true;

    CCPPrint(cpm, "\n");
    CCPPrint(cpm, "  +------------------------------------------+\n");
    CCPPrint(cpm, "  |    CP/M 2.2  --  Intel 8080 Emulator     |\n");
    CCPPrint(cpm, "  |    Copyright (c) Moises Nunes            |\n");
    CCPPrint(cpm, "  +------------------------------------------+\n");
    CCPPrint(cpm, "\n");

    int tpaKb = (BDOS_ADDR - 0x0100) / 1024;
    char buf[128];
    snprintf(buf, sizeof(buf), "  TPA: %dK  (0x0100 - 0x%04X)\n", tpaKb, BDOS_ADDR - 1);
    CCPPrint(cpm, buf);
    if (cpm.overlayBase)
    {
        int resKb = (cpm.overlayBase - 0x0100) / 1024;
        uint16_t ovTop = cpm.overlayTop();
        int ovKb = (ovTop - cpm.overlayBase) / 1024;
        snprintf(buf, sizeof(buf),
                 "  Resident: %dK  (0x0100 - 0x%04X)    Overlay: %dK  (0x%04X - 0x%04X)\n",
                 resKb, cpm.overlayBase - 1, ovKb, cpm.overlayBase, ovTop - 1);
        CCPPrint(cpm, buf);
    }

    CCPPrint(cpm, "  Drives:");
    for (int i = 0; i < MAX_DRIVES; i++)
    {
        if (!cpm.diskDirs[i].empty() || (cpm.diskImages[i] && cpm.diskImages[i]->isOpen()))
        {
            char dbuf[8];
            snprintf(dbuf, sizeof(dbuf), "  %c:", 'A' + i);
            CCPPrint(cpm, dbuf);
            if (cpm.diskImages[i] && cpm.diskImages[i]->isOpen())
                CCPPrint(cpm, "[DSK]");
        }
    }
    CCPPrint(cpm, "\n");

    CCPPrint(cpm, "\n  Type HELP or ? for a list of commands.\n\n");
}

// ── CCPTick ───────────────────────────────────────────────────────────────────

bool CCPTick(intel8080 *cpu, CPMState &cpm)
{
    // Handle ED line-editor if active.
    if (cpm.editorActive)
    {
        if (CCPTickEditor(cpm))
            cpm.ccpPrompted = false;
        return false;
    }

    // ── Post-.COM cleanup (runs once on the first CCPTick after warm-boot) ──
    // Close any output redirect left open by the previous .COM.
    if (cpm.redirectOut)
    {
        fclose(cpm.redirectOut);
        cpm.redirectOut = nullptr;
    }

    // If stage 1 of a pipe was a .COM, its output is now in pipeBuffer.
    // Switch to capture-off and queue stage 2 immediately.
    if (cpm.pipeCapture)
    {
        cpm.pipeCapture = false;
        cpm.pipeStage2Ready = true;
    }

    if (cpm.pipeStage2Ready)
    {
        cpm.pipeStage2Ready = false;
        // Feed captured output as stdin for stage 2.
        for (auto b : cpm.pipeBuffer)
            cpm.terminal.inputQueue.push_back(b);
        cpm.pipeBuffer.clear();
        // Stage-2 < redirect (appended after pipe data).
        if (!cpm.pipeStage2InFile.empty())
        {
            CCPInjectInputFile(cpm, cpm.pipeStage2InFile);
            cpm.pipeStage2InFile.clear();
        }
        // Stage-2 > redirect.
        if (!cpm.pipeStage2OutFile.empty())
        {
            std::string tok = cpm.pipeStage2OutFile;
            const std::string &dir = CCPResolveDir(cpm, tok);
            cpm.redirectOut = fopen((dir + "/" + tok).c_str(), "w");
            cpm.pipeStage2OutFile.clear();
        }
        std::string cmd2 = cpm.pipeStage2Cmd, args2 = cpm.pipeStage2Args;
        cpm.pipeStage2Cmd.clear();
        cpm.pipeStage2Args.clear();
        bool r2 = CCPDispatch(cpu, cpm, cmd2, args2);
        if (!r2 && cpm.redirectOut)
        {
            fclose(cpm.redirectOut);
            cpm.redirectOut = nullptr;
        }
        return r2;
    }

    // Inject next SUBMIT line as if typed by the user.
    if (!cpm.submitQueue.empty() && cpm.terminal.inputQueue.empty())
    {
        std::string line = cpm.submitQueue.front();
        cpm.submitQueue.erase(cpm.submitQueue.begin());
        for (char c : line)
            cpm.terminal.inputQueue.push_back((uint8_t)c);
        cpm.terminal.inputQueue.push_back(0x0D);
    }

    // Display prompt once per command cycle (or ERA confirmation).
    if (!cpm.ccpPrompted)
    {
        if (cpm.ccpEraConfirm)
            CCPPrint(cpm, "All (y/n)?");
        else
        {
            char prompt[6];
            snprintf(prompt, sizeof(prompt), "\r%c>", 'A' + cpm.currentDrive);
            CCPPrint(cpm, prompt);
        }
        cpm.ccpPrompted = true;
        cpm.ccpLine.clear();
    }

    // Drain the input queue one character at a time (non-blocking).
    while (!cpm.terminal.inputQueue.empty())
    {
        uint8_t ch = cpm.terminal.inputQueue.front();
        cpm.terminal.inputQueue.erase(cpm.terminal.inputQueue.begin());

        if (ch == 0x0D || ch == 0x0A)
        { // Enter
            cpm.terminal.putChar('\r');
            cpm.terminal.putChar('\n');
            std::string line = cpm.ccpLine;
            cpm.ccpLine.clear();
            cpm.ccpPrompted = false;

            if (cpm.ccpEraConfirm)
            {
                cpm.ccpEraConfirm = false;
                if (CCPUpper(line) == "Y")
                    CCPBuiltinEra(cpm, cpm.ccpEraPendingArgs, /*confirmed=*/true);
                cpm.ccpEraPendingArgs.clear();
                return false;
            }

            if (line.empty())
                return false;

            return CCPParseAndDispatch(cpu, cpm, CCPUpper(line));
        }
        else if (ch == 0x08 || ch == 0x7F)
        { // Backspace / DEL
            if (!cpm.ccpLine.empty())
            {
                cpm.ccpLine.pop_back();
                cpm.terminal.putChar('\b');
                cpm.terminal.putChar(' ');
                cpm.terminal.putChar('\b');
            }
        }
        else if (ch == 0x15)
        { // ^U — erase line
            for (size_t i = 0; i < cpm.ccpLine.size(); i++)
            {
                cpm.terminal.putChar('\b');
                cpm.terminal.putChar(' ');
                cpm.terminal.putChar('\b');
            }
            cpm.ccpLine.clear();
        }
        else if (ch == 0x03)
        { // ^C — redisplay prompt
            CCPPrint(cpm, "\n");
            cpm.ccpLine.clear();
            cpm.ccpPrompted = false;
        }
        else if (ch >= 0x20 && cpm.ccpLine.size() < 126)
        {
            char c = (char)toupper((unsigned char)ch);
            cpm.ccpLine += c;
            cpm.terminal.putChar(c);
        }
    }
    return false;
}
