/* Installer for Mafia Aim Assist. Finds Steam and GOG installs, copies the mod, supports /uninstall. */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <wincrypt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define SUPPORTED_GAME_SHA256 "303eb95ee2de3433511ce0cb518921dcb96b62b0f64ebfcc436723ff5083f298"
#define MANIFEST_NAME L"MafiaAimAssist_install.txt"
#define BACKUP_DIR    L"MafiaAimAssist_backup"
#define MAX_CANDIDATES 16
#define PATH_CAP 1024

typedef struct FileSpec
{
    const wchar_t *source;   /* relative to <installer>\files */
    const wchar_t *target;   /* relative to the game folder */
    int xidi;                /* part of the optional controller package */
    int keepExisting;        /* never overwrite a file the user may have edited */
} FileSpec;

static const FileSpec kFiles[] = {
    {L"dinput8.dll",        L"dinput8.dll",        0, 0},
    {L"MafiaAimLogic.dll",  L"MafiaAimLogic.dll",  0, 0},
    {L"MafiaAimAssist.ini", L"MafiaAimAssist.ini", 0, 1},
    {L"xidi\\dinput.dll",   L"dinput.dll",         1, 0},
    {L"xidi\\Xidi.32.dll",  L"Xidi.32.dll",        1, 0},
    {L"xidi\\Xidi.ini",     L"Xidi.ini",           1, 1},
    {L"xidi\\LICENSE",      L"Xidi.LICENSE.txt",   1, 0},
};
#define FILE_COUNT (sizeof(kFiles) / sizeof(kFiles[0]))

static wchar_t g_candidates[MAX_CANDIDATES][PATH_CAP];
static int     g_candidateCount;
static int     g_quiet;

static void Say(const wchar_t *format, ...)
{
    wchar_t line[2048];
    DWORD written;
    va_list args;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    va_start(args, format);
    _vsnwprintf(line, 2047, format, args);
    va_end(args);
    line[2047] = L'\0';
    if (!WriteConsoleW(out, line, (DWORD)wcslen(line), &written, NULL))
        wprintf(L"%ls", line);
}

static int ReadLine(wchar_t *buffer, int capacity)
{
    DWORD read = 0;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    int length;
    buffer[0] = L'\0';
    if (ReadConsoleW(in, buffer, (DWORD)(capacity - 1), &read, NULL))
        buffer[read] = L'\0';
    else if (!fgetws(buffer, capacity, stdin))
        return 0;
    length = (int)wcslen(buffer);
    while (length > 0 && (buffer[length - 1] == L'\r' || buffer[length - 1] == L'\n' ||
                          buffer[length - 1] == L' ' || buffer[length - 1] == L'"'))
        buffer[--length] = L'\0';
    while (buffer[0] == L' ' || buffer[0] == L'"')
        memmove(buffer, buffer + 1, (wcslen(buffer)) * sizeof(wchar_t));
    return 1;
}

static int Ask(const wchar_t *question, int defaultYes)
{
    wchar_t answer[32];
    if (g_quiet)
        return defaultYes;
    Say(L"%ls [%ls] ", question, defaultYes ? L"Y/n" : L"y/N");
    if (!ReadLine(answer, 32) || answer[0] == L'\0')
        return defaultYes;
    return answer[0] == L'y' || answer[0] == L'Y';
}

static void Pause(void)
{
    wchar_t ignored[16];
    if (g_quiet)
        return;
    Say(L"\nPress Enter to close this window.");
    ReadLine(ignored, 16);
}

static int Exists(const wchar_t *path) { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }

static void Join(wchar_t *out, const wchar_t *directory, const wchar_t *name)
{
    size_t length = wcslen(directory);
    while (length > 0 && (directory[length - 1] == L'\\' || directory[length - 1] == L'/'))
        --length;
    _snwprintf(out, PATH_CAP - 1, L"%.*ls\\%ls", (int)length, directory, name);
    out[PATH_CAP - 1] = L'\0';
}

static int Sha256(const wchar_t *path, char *hex)
{
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    HANDLE file;
    BYTE buffer[65536], digest[32];
    DWORD read, length = 32;
    int ok = 0, i;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return 0;
    if (CryptAcquireContextW(&provider, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
        CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash))
    {
        while (ReadFile(file, buffer, sizeof(buffer), &read, NULL) && read)
            CryptHashData(hash, buffer, read, 0);
        if (CryptGetHashParam(hash, HP_HASHVAL, digest, &length, 0))
        {
            for (i = 0; i < 32; ++i)
                sprintf(hex + i * 2, "%02x", digest[i]);
            hex[64] = '\0';
            ok = 1;
        }
    }
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    CloseHandle(file);
    return ok;
}

static int SameContent(const wchar_t *a, const wchar_t *b)
{
    char ha[65], hb[65];
    return Sha256(a, ha) && Sha256(b, hb) && strcmp(ha, hb) == 0;
}

/* ---- finding the game --------------------------------------------------- */

static int LooksLikeGame(const wchar_t *directory)
{
    wchar_t path[PATH_CAP];
    Join(path, directory, L"Game.exe");
    if (!Exists(path))
        return 0;
    Join(path, directory, L"LS3DF.dll");
    return Exists(path);
}

static void AddCandidate(const wchar_t *directory)
{
    wchar_t clean[PATH_CAP];
    size_t length;
    int i;
    if (g_candidateCount >= MAX_CANDIDATES || !directory[0])
        return;
    wcsncpy(clean, directory, PATH_CAP - 1);
    clean[PATH_CAP - 1] = L'\0';
    for (i = 0; clean[i]; ++i)
        if (clean[i] == L'/')
            clean[i] = L'\\';
    length = wcslen(clean);
    while (length > 3 && clean[length - 1] == L'\\')
        clean[--length] = L'\0';
    if (!LooksLikeGame(clean))
        return;
    for (i = 0; i < g_candidateCount; ++i)
        if (_wcsicmp(g_candidates[i], clean) == 0)
            return;
    wcscpy(g_candidates[g_candidateCount++], clean);
}

static int ReadRegistry(HKEY root, const wchar_t *subKey, const wchar_t *name, wchar_t *out, DWORD capacity)
{
    HKEY key;
    DWORD type = 0, size = (capacity - 1) * sizeof(wchar_t);
    LONG status;
    if (RegOpenKeyExW(root, subKey, 0, KEY_READ | KEY_WOW64_32KEY, &key) != ERROR_SUCCESS)
        return 0;
    status = RegQueryValueExW(key, name, NULL, &type, (BYTE *)out, &size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_SZ)
        return 0;
    out[size / sizeof(wchar_t)] = L'\0';
    return 1;
}

static void AddSteamLibrary(const wchar_t *library)
{
    wchar_t path[PATH_CAP], nested[PATH_CAP];
    Join(path, library, L"steamapps\\common\\Mafia");
    AddCandidate(path);
    Join(nested, path, L"Mafia");
    AddCandidate(nested);
}

static void FindSteam(void)
{
    wchar_t steam[PATH_CAP] = L"", vdfPath[PATH_CAP];
    HANDLE file;
    DWORD size, read;
    char *text;
    wchar_t *wide;
    wchar_t *line, *context = NULL;
    int wideLength;

    if (!ReadRegistry(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", steam, PATH_CAP) &&
        !ReadRegistry(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath", steam, PATH_CAP))
        return;
    AddSteamLibrary(steam);
    Join(vdfPath, steam, L"steamapps\\libraryfolders.vdf");
    file = CreateFileW(vdfPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return;
    size = GetFileSize(file, NULL);
    if (size == INVALID_FILE_SIZE || size > 4 * 1024 * 1024)
    {
        CloseHandle(file);
        return;
    }
    text = (char *)malloc(size + 1);
    wide = (wchar_t *)malloc((size + 1) * sizeof(wchar_t));
    if (!text || !wide || !ReadFile(file, text, size, &read, NULL))
    {
        free(text); free(wide); CloseHandle(file);
        return;
    }
    CloseHandle(file);
    wideLength = MultiByteToWideChar(CP_UTF8, 0, text, (int)read, wide, (int)size);
    wide[wideLength > 0 ? wideLength : 0] = L'\0';
    for (line = wcstok_s(wide, L"\n", &context); line; line = wcstok_s(NULL, L"\n", &context))
    {
        wchar_t *key = wcsstr(line, L"\"path\"");
        wchar_t *open, *close, library[PATH_CAP];
        size_t i, out = 0;
        if (!key)
            continue;
        open = wcschr(key + 6, L'"');
        if (!open)
            continue;
        close = wcschr(open + 1, L'"');
        if (!close)
            continue;
        for (i = 1; open + i < close && out < PATH_CAP - 1; ++i)
        {
            if (open[i] == L'\\' && open + i + 1 < close && open[i + 1] == L'\\')
                ++i;
            library[out++] = open[i];
        }
        library[out] = L'\0';
        AddSteamLibrary(library);
    }
    free(text);
    free(wide);
}

static void FindGog(void)
{
    wchar_t path[PATH_CAP];
    static const wchar_t *defaults[] = {L"C:\\GOG Games\\Mafia", L"C:\\Program Files (x86)\\GOG Galaxy\\Games\\Mafia",
                                        L"D:\\GOG Games\\Mafia"};
    int i;
    if (ReadRegistry(HKEY_LOCAL_MACHINE, L"SOFTWARE\\GOG.com\\Games\\1595659240", L"path", path, PATH_CAP))
        AddCandidate(path);
    for (i = 0; i < 3; ++i)
        AddCandidate(defaults[i]);
}

/* ---- install / uninstall --------------------------------------------------- */

static void SourceRoot(wchar_t *out)
{
    DWORD length = GetModuleFileNameW(NULL, out, PATH_CAP - 16);
    while (length > 0 && out[length - 1] != L'\\')
        --length;
    out[length] = L'\0';
    wcscat(out, L"files");
}

static int Install(const wchar_t *game, int withXidi, int force)
{
    wchar_t source[PATH_CAP], gamePath[PATH_CAP], from[PATH_CAP], to[PATH_CAP], backup[PATH_CAP];
    wchar_t manifestPath[PATH_CAP];
    char hash[65];
    FILE *manifest;
    size_t i;
    int conflicts = 0;

    SourceRoot(source);
    Join(from, source, L"dinput8.dll");
    if (!Exists(from))
    {
        Say(L"\nThe \"files\" folder was not found next to the installer.\nExtract the whole zip first, then run Install.exe from the extracted folder.\n");
        return 1;
    }

    Join(gamePath, game, L"Game.exe");
    if (Sha256(gamePath, hash) && strcmp(hash, SUPPORTED_GAME_SHA256) != 0)
    {
        Say(L"\nThis Game.exe is not the build the mod was made for (the memory layout is build specific).\n");
        Say(L"  expected SHA-256: %hs\n  found:            %hs\n", SUPPORTED_GAME_SHA256, hash);
        Say(L"The Steam and GOG releases of Mafia (version 1.3) are supported.\n");
        if (!force && !Ask(L"Install anyway? The aim assist will probably do nothing.", 0))
            return 1;
    }

    for (i = 0; i < FILE_COUNT; ++i)
    {
        if (kFiles[i].xidi && !withXidi)
            continue;
        Join(from, source, kFiles[i].source);
        Join(to, game, kFiles[i].target);
        if (!Exists(from))
        {
            Say(L"Missing package file: %ls\n", kFiles[i].source);
            return 1;
        }
        if (Exists(to) && !kFiles[i].keepExisting && !SameContent(from, to) &&
            (_wcsicmp(kFiles[i].target, L"dinput8.dll") == 0 || _wcsicmp(kFiles[i].target, L"dinput.dll") == 0))
            ++conflicts;
    }
    if (conflicts)
    {
        Say(L"\nThe game folder already has a dinput8.dll or dinput.dll from something else (for example the\n");
        Say(L"Widescreen Fix / an ASI loader). Installing replaces it; the original is kept in %ls\\ and\n", BACKUP_DIR);
        Say(L"restored by /uninstall, but those other mods will stop loading while this one is installed.\n");
        if (!force && !Ask(L"Continue?", 0))
            return 1;
    }

    Join(manifestPath, game, MANIFEST_NAME);
    manifest = _wfopen(manifestPath, L"a");
    if (!manifest)
    {
        Say(L"\nCannot write to %ls\nIf the game is in Program Files, run Install.exe as administrator.\n", game);
        return 1;
    }
    Join(backup, game, BACKUP_DIR);

    for (i = 0; i < FILE_COUNT; ++i)
    {
        int existed;
        if (kFiles[i].xidi && !withXidi)
            continue;
        Join(from, source, kFiles[i].source);
        Join(to, game, kFiles[i].target);
        existed = Exists(to);
        if (existed && kFiles[i].keepExisting)
        {
            Say(L"  kept existing   %ls\n", kFiles[i].target);
            continue;
        }
        if (existed && !SameContent(from, to))
        {
            wchar_t saved[PATH_CAP];
            CreateDirectoryW(backup, NULL);
            Join(saved, backup, kFiles[i].target);
            if (!Exists(saved) && CopyFileW(to, saved, TRUE))
                fwprintf(manifest, L"B %ls\n", kFiles[i].target);
        }
        if (!CopyFileW(from, to, FALSE))
        {
            Say(L"  FAILED          %ls (error %lu). Close the game and try again.\n", kFiles[i].target,
                GetLastError());
            fclose(manifest);
            return 1;
        }
        fwprintf(manifest, L"F %ls\n", kFiles[i].target);
        Say(L"  installed       %ls\n", kFiles[i].target);
    }
    fclose(manifest);
    return 0;
}

static int Uninstall(const wchar_t *game)
{
    wchar_t manifestPath[PATH_CAP], backup[PATH_CAP], line[PATH_CAP], target[PATH_CAP], saved[PATH_CAP];
    FILE *manifest;
    Join(manifestPath, game, MANIFEST_NAME);
    manifest = _wfopen(manifestPath, L"r");
    if (!manifest)
    {
        Say(L"No installation record found in %ls\n", game);
        return 1;
    }
    Join(backup, game, BACKUP_DIR);
    while (fgetws(line, PATH_CAP, manifest))
    {
        size_t length = wcslen(line);
        while (length > 0 && (line[length - 1] == L'\n' || line[length - 1] == L'\r'))
            line[--length] = L'\0';
        if (length < 3)
            continue;
        Join(target, game, line + 2);
        if (line[0] == L'F')
        {
            if (DeleteFileW(target))
                Say(L"  removed   %ls\n", line + 2);
        }
    }
    rewind(manifest);
    while (fgetws(line, PATH_CAP, manifest))
    {
        size_t length = wcslen(line);
        while (length > 0 && (line[length - 1] == L'\n' || line[length - 1] == L'\r'))
            line[--length] = L'\0';
        if (length >= 3 && line[0] == L'B')
        {
            Join(target, game, line + 2);
            Join(saved, backup, line + 2);
            if (MoveFileExW(saved, target, MOVEFILE_REPLACE_EXISTING))
                Say(L"  restored  %ls\n", line + 2);
        }
    }
    fclose(manifest);
    DeleteFileW(manifestPath);
    RemoveDirectoryW(backup);
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    wchar_t chosen[PATH_CAP] = L"";
    int uninstall = 0, list = 0, force = 0, xidiChoice = -1, i, status;

    for (i = 1; i < argc; ++i)
    {
        if (_wcsicmp(argv[i], L"/uninstall") == 0) uninstall = 1;
        else if (_wcsicmp(argv[i], L"/list") == 0) list = 1;
        else if (_wcsicmp(argv[i], L"/force") == 0) force = 1;
        else if (_wcsicmp(argv[i], L"/quiet") == 0) g_quiet = 1;
        else if (_wcsicmp(argv[i], L"/xidi") == 0) xidiChoice = 1;
        else if (_wcsicmp(argv[i], L"/noxidi") == 0) xidiChoice = 0;
        else if (argv[i][0] != L'/') wcsncpy(chosen, argv[i], PATH_CAP - 1);
    }

    Say(L"Mafia Aim Assist installer\n==========================\n\n");
    if (chosen[0])
        AddCandidate(chosen);
    else
    {
        FindSteam();
        FindGog();
    }
    if (list)
    {
        for (i = 0; i < g_candidateCount; ++i)
            Say(L"%ls\n", g_candidates[i]);
        return 0;
    }

    if (g_candidateCount == 0)
    {
        if (chosen[0])
        {
            Say(L"That folder does not contain Mafia (Game.exe and LS3DF.dll were not found).\n");
            Pause();
            return 1;
        }
        Say(L"Could not find Mafia automatically.\n");
        if (g_quiet)
            return 1;
        Say(L"Paste the game folder (the one that contains Game.exe and LS3DF.dll; for Steam it is usually\n"
            L"...\\steamapps\\common\\Mafia\\Mafia): ");
        ReadLine(chosen, PATH_CAP);
        AddCandidate(chosen);
        if (g_candidateCount == 0)
        {
            Say(L"\nThat folder does not contain Mafia.\n");
            Pause();
            return 1;
        }
    }

    wcscpy(chosen, g_candidates[0]);
    if (g_candidateCount > 1)
    {
        wchar_t answer[16];
        int pick = 0;
        Say(L"Found several installs:\n");
        for (i = 0; i < g_candidateCount; ++i)
            Say(L"  %d) %ls\n", i + 1, g_candidates[i]);
        if (!g_quiet)
        {
            Say(L"Choose one [1-%d]: ", g_candidateCount);
            if (ReadLine(answer, 16))
                pick = _wtoi(answer) - 1;
            if (pick < 0 || pick >= g_candidateCount)
                pick = 0;
        }
        wcscpy(chosen, g_candidates[pick]);
    }
    Say(L"Game folder: %ls\n\n", chosen);

    if (uninstall)
    {
        status = Uninstall(chosen);
        Say(status == 0 ? L"\nUninstalled.\n" : L"\nNothing was removed.\n");
        Pause();
        return status;
    }

    if (xidiChoice < 0)
        xidiChoice = Ask(L"Install Xidi (needed for the Xbox controller with Steam Input)?", 1);
    status = Install(chosen, xidiChoice, force);
    if (status == 0)
        Say(L"\nDone. Next, set the in-game controls as described in the README (Aim: Mouse Y axis / Mouse X axis).\n");
    Pause();
    return status;
}
