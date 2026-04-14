/*
 * RayShell — a raylib-powered graphical terminal emulator for Windows
 *
 * Features
 * --------
 *  • Scrollable output buffer (mouse wheel, Page Up/Down)
 *  • Persistent command history (Up / Down arrows)
 *  • Full cursor navigation (Left / Right, Ctrl+Left/Right, Home, End)
 *  • Insert / Delete in the middle of the input line
 *  • Blinking text cursor
 *  • Ctrl+C clears current input; Ctrl+L clears screen
 *  • Built-ins: cd, cls/clear, help, exit/quit
 *  • Everything else is forwarded to cmd.exe and the output is captured
 *  • Resizable window
 */

/* Exclude Win32 subsystems whose names clash with raylib identifiers:
 *   NOGDI  – removes DrawText (GDI macro → DrawTextA) and other GDI exports
 *   NOUSER – removes CloseWindow (User32 function)
 * All functions we actually use (CreateProcess, CreatePipe, ReadFile, …)
 * live in kernel32 and remain available.
 */
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOUSER
#include <windows.h>

#include <raylib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ── tuneable constants ───────────────────────────────────────────────── */
#define MAX_LINES       2000
#define MAX_LINE_LEN    512
#define MAX_INPUT       512
#define MAX_HISTORY     200
#define FONT_SIZE       16
#define LINE_HEIGHT     20
#define PAD             8

/* ── colour palette (dark‑terminal style) ────────────────────────────── */
static const Color C_BG        = {15,  15,  20,  255};
static const Color C_INPUT_BG  = {22,  22,  30,  255};
static const Color C_TEXT      = {200, 200, 200, 255};
static const Color C_PROMPT    = {80,  200, 120, 255};
static const Color C_DIR       = {80,  160, 220, 255};
static const Color C_ERROR     = {220, 70,  70,  255};
static const Color C_SEP       = {50,  50,  60,  255};
static const Color C_CURSOR    = {220, 220, 220, 255};
static const Color C_SCROLLBAR = {90,  90,  110, 255};
static const Color C_BANNER    = {100, 180, 255, 255};

/* ── output buffer ───────────────────────────────────────────────────── */
typedef struct { char text[MAX_LINE_LEN]; Color color; } Line;

static Line g_lines[MAX_LINES];
static int  g_lineCount = 0;
static int  g_scroll    = 0;   /* lines scrolled from the bottom */

/* ── input state ─────────────────────────────────────────────────────── */
static char g_input[MAX_INPUT];
static int  g_inputLen  = 0;
static int  g_cursor    = 0;

/* ── history ─────────────────────────────────────────────────────────── */
static char g_hist[MAX_HISTORY][MAX_INPUT];
static int  g_histCount = 0;
static int  g_histIdx   = -1;  /* -1 = not browsing */

/* ── shell state ─────────────────────────────────────────────────────── */
static char g_cwd[MAX_PATH];

/* ── cursor blink ────────────────────────────────────────────────────── */
static double g_blinkTimer = 0.0;
static bool   g_blinkOn    = true;

/* ══════════════════════════════════════════════════════════════════════ */
/*  Output helpers                                                        */
/* ══════════════════════════════════════════════════════════════════════ */

static void PushLine(const char *text, Color c)
{
    if (g_lineCount >= MAX_LINES) {
        memmove(&g_lines[0], &g_lines[1], sizeof(Line) * (MAX_LINES - 1));
        g_lineCount = MAX_LINES - 1;
    }
    strncpy(g_lines[g_lineCount].text, text, MAX_LINE_LEN - 1);
    g_lines[g_lineCount].text[MAX_LINE_LEN - 1] = '\0';
    g_lines[g_lineCount].color = c;
    g_lineCount++;
    g_scroll = 0;   /* jump to bottom on new output */
}

/* Push a block of text, splitting on newlines */
static void PushText(const char *text, Color c)
{
    char buf[MAX_LINE_LEN];
    const char *s = text;
    while (*s) {
        const char *nl  = strchr(s, '\n');
        int          len = nl ? (int)(nl - s) : (int)strlen(s);
        if (len >= MAX_LINE_LEN) len = MAX_LINE_LEN - 1;
        memcpy(buf, s, (size_t)len);
        buf[len] = '\0';
        PushLine(buf, c);
        if (!nl) return;
        s = nl + 1;
    }
    if (s == text) PushLine("", c); /* empty string → blank line */
}

/* ══════════════════════════════════════════════════════════════════════ */
/*  Built-in commands                                                     */
/* ══════════════════════════════════════════════════════════════════════ */

static void DoCD(const char *path)
{
    if (!path || !*path) {
        path = getenv("USERPROFILE");
        if (!path) path = "C:\\";
    }
    if (!SetCurrentDirectoryA(path)) {
        char err[MAX_LINE_LEN];
        snprintf(err, sizeof(err), "cd: '%s': no such directory", path);
        PushLine(err, C_ERROR);
    } else {
        GetCurrentDirectoryA(MAX_PATH, g_cwd);
    }
}

static bool HandleBuiltin(const char *raw, char *argv[], int argc)
{
    (void)raw;

    if (!strcmp(argv[0], "exit") || !strcmp(argv[0], "quit")) {
        CloseWindow();
        exit(0);
    }
    if (!strcmp(argv[0], "cls") || !strcmp(argv[0], "clear")) {
        g_lineCount = 0;
        g_scroll    = 0;
        return true;
    }
    if (!strcmp(argv[0], "cd")) {
        DoCD(argc > 1 ? argv[1] : NULL);
        return true;
    }
    if (!strcmp(argv[0], "help")) {
        PushLine("Built-in commands:", C_BANNER);
        PushLine("  cd <dir>    Change the working directory", C_TEXT);
        PushLine("  cls/clear   Clear the screen", C_TEXT);
        PushLine("  exit/quit   Close RayShell", C_TEXT);
        PushLine("  help        Print this message", C_TEXT);
        PushLine("", C_TEXT);
        PushLine("Keyboard shortcuts:", C_BANNER);
        PushLine("  Enter          Execute command", C_TEXT);
        PushLine("  Up / Down      Browse command history", C_TEXT);
        PushLine("  Left / Right   Move cursor", C_TEXT);
        PushLine("  Ctrl+Left/Right  Jump word", C_TEXT);
        PushLine("  Home / End     Jump to start / end of input", C_TEXT);
        PushLine("  Page Up/Down   Scroll output", C_TEXT);
        PushLine("  Ctrl+C         Clear current input", C_TEXT);
        PushLine("  Ctrl+L         Clear screen", C_TEXT);
        PushLine("  Mouse wheel    Scroll output", C_TEXT);
        PushLine("", C_TEXT);
        PushLine("Anything else is forwarded to cmd.exe.", C_TEXT);
        return true;
    }
    return false;
}

/* ══════════════════════════════════════════════════════════════════════ */
/*  Process execution with output capture                                 */
/* ══════════════════════════════════════════════════════════════════════ */

static void RunProcess(const char *cmdLine)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE hR, hW;
    if (!CreatePipe(&hR, &hW, &sa, 0)) {
        PushLine("error: CreatePipe failed", C_ERROR);
        return;
    }
    /* prevent child from inheriting the read end */
    SetHandleInformation(hR, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {0};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = hW;
    si.hStdError  = hW;
    si.hStdInput  = NULL;

    PROCESS_INFORMATION pi = {0};
    char cmd[MAX_INPUT + 16];
    snprintf(cmd, sizeof(cmd), "cmd.exe /C %s", cmdLine);

    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(hW);   /* we are done writing; child has its own handle */

    if (!ok) {
        char err[MAX_LINE_LEN];
        snprintf(err, sizeof(err),
                 "'%s' is not recognized as a command (error %lu)",
                 cmdLine, GetLastError());
        PushLine(err, C_ERROR);
        CloseHandle(hR);
        return;
    }

    /* read output line by line */
    char rbuf[4096];
    DWORD nRead;
    char lineBuf[MAX_LINE_LEN];
    int  linePos = 0;

    while (ReadFile(hR, rbuf, sizeof(rbuf) - 1, &nRead, NULL) && nRead > 0) {
        for (DWORD i = 0; i < nRead; i++) {
            char ch = rbuf[i];
            if (ch == '\r') continue;
            if (ch == '\n') {
                lineBuf[linePos] = '\0';
                PushLine(lineBuf, C_TEXT);
                linePos = 0;
            } else {
                if (linePos < MAX_LINE_LEN - 1)
                    lineBuf[linePos++] = ch;
            }
        }
    }
    if (linePos > 0) {
        lineBuf[linePos] = '\0';
        PushLine(lineBuf, C_TEXT);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hR);
}

/* ══════════════════════════════════════════════════════════════════════ */
/*  Command submission                                                    */
/* ══════════════════════════════════════════════════════════════════════ */

static void Submit(void)
{
    /* echo with prompt so the buffer shows what was typed */
    char echo[MAX_LINE_LEN];
    snprintf(echo, sizeof(echo), "%s> %s", g_cwd, g_input);
    PushLine(echo, C_DIR);

    if (g_inputLen > 0) {
        /* append to history (skip consecutive duplicates) */
        if (g_histCount == 0 ||
            strcmp(g_hist[g_histCount - 1], g_input) != 0)
        {
            if (g_histCount < MAX_HISTORY) {
                strncpy(g_hist[g_histCount++], g_input, MAX_INPUT - 1);
            } else {
                memmove(g_hist[0], g_hist[1],
                        sizeof(g_hist[0]) * (MAX_HISTORY - 1));
                strncpy(g_hist[MAX_HISTORY - 1], g_input, MAX_INPUT - 1);
            }
        }

        /* tokenise for built-in detection */
        char copy[MAX_INPUT];
        strncpy(copy, g_input, MAX_INPUT - 1);
        copy[MAX_INPUT - 1] = '\0';

        char *argv[64];
        int   argc = 0;
        char *tok  = strtok(copy, " \t");
        while (tok && argc < 63) { argv[argc++] = tok; tok = strtok(NULL, " \t"); }
        argv[argc] = NULL;

        if (argc > 0 && !HandleBuiltin(g_input, argv, argc))
            RunProcess(g_input);
    }

    g_histIdx   = -1;
    g_input[0]  = '\0';
    g_inputLen  = 0;
    g_cursor    = 0;
}

/* ══════════════════════════════════════════════════════════════════════ */
/*  Cursor helpers                                                        */
/* ══════════════════════════════════════════════════════════════════════ */

static void ResetBlink(void) { g_blinkOn = true; g_blinkTimer = 0.0; }

/* Jump one word left (Ctrl+Left) */
static void WordLeft(void)
{
    while (g_cursor > 0 && g_input[g_cursor - 1] == ' ') g_cursor--;
    while (g_cursor > 0 && g_input[g_cursor - 1] != ' ') g_cursor--;
}

/* Jump one word right (Ctrl+Right) */
static void WordRight(void)
{
    while (g_cursor < g_inputLen && g_input[g_cursor] == ' ')  g_cursor++;
    while (g_cursor < g_inputLen && g_input[g_cursor] != ' ')  g_cursor++;
}

/* ══════════════════════════════════════════════════════════════════════ */
/*  main                                                                  */
/* ══════════════════════════════════════════════════════════════════════ */

int main(void)
{
    GetCurrentDirectoryA(MAX_PATH, g_cwd);

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    InitWindow(940, 640, "RayShell");
    SetTargetFPS(60);
    SetExitKey(KEY_NULL);   /* don't close on ESC */

    /* welcome banner */
    PushLine("RayShell  --  Raylib-powered graphical terminal", C_BANNER);
    char ver[128];
    snprintf(ver, sizeof(ver), "raylib %s  |  type 'help' for commands", RAYLIB_VERSION);
    PushLine(ver, C_TEXT);
    PushLine("", C_TEXT);

    /* ── main loop ─────────────────────────────────────────────────── */
    while (!WindowShouldClose()) {

        int W = GetScreenWidth();
        int H = GetScreenHeight();

        /* geometry */
        int inputH   = FONT_SIZE + PAD * 2 + 2;
        int outH     = H - inputH - 1;
        int visLines = (outH - PAD) / LINE_HEIGHT;
        if (visLines < 1) visLines = 1;

        /* cursor blink */
        g_blinkTimer += GetFrameTime();
        if (g_blinkTimer >= 0.55) { g_blinkOn = !g_blinkOn; g_blinkTimer = 0.0; }

        /* scroll clamp helper */
        #define CLAMP_SCROLL() do { \
            int _ms = g_lineCount - visLines; if (_ms < 0) _ms = 0; \
            if (g_scroll < 0)   g_scroll = 0; \
            if (g_scroll > _ms) g_scroll = _ms; \
        } while(0)

        /* mouse wheel */
        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) {
            g_scroll -= (int)(wheel * 3);
            CLAMP_SCROLL();
        }

        /* ── keyboard ──────────────────────────────────────────────── */
        bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
        int k;
        while ((k = GetKeyPressed())) {

            if (k == KEY_ENTER) {
                Submit();

            } else if (k == KEY_BACKSPACE) {
                if (g_cursor > 0) {
                    memmove(&g_input[g_cursor - 1], &g_input[g_cursor],
                            (size_t)(g_inputLen - g_cursor + 1));
                    g_cursor--;
                    g_inputLen--;
                }
                ResetBlink();

            } else if (k == KEY_DELETE) {
                if (g_cursor < g_inputLen) {
                    memmove(&g_input[g_cursor], &g_input[g_cursor + 1],
                            (size_t)(g_inputLen - g_cursor));
                    g_inputLen--;
                }

            } else if (k == KEY_LEFT) {
                if (ctrl) WordLeft(); else if (g_cursor > 0) g_cursor--;
                ResetBlink();

            } else if (k == KEY_RIGHT) {
                if (ctrl) WordRight(); else if (g_cursor < g_inputLen) g_cursor++;
                ResetBlink();

            } else if (k == KEY_HOME) {
                g_cursor = 0;
                ResetBlink();

            } else if (k == KEY_END) {
                g_cursor = g_inputLen;
                ResetBlink();

            } else if (k == KEY_UP) {
                if (g_histCount > 0) {
                    if (g_histIdx < g_histCount - 1) g_histIdx++;
                    strncpy(g_input,
                            g_hist[g_histCount - 1 - g_histIdx],
                            MAX_INPUT - 1);
                    g_inputLen = (int)strlen(g_input);
                    g_cursor   = g_inputLen;
                }

            } else if (k == KEY_DOWN) {
                if (g_histIdx > 0) {
                    g_histIdx--;
                    strncpy(g_input,
                            g_hist[g_histCount - 1 - g_histIdx],
                            MAX_INPUT - 1);
                    g_inputLen = (int)strlen(g_input);
                    g_cursor   = g_inputLen;
                } else {
                    g_histIdx  = -1;
                    g_input[0] = '\0';
                    g_inputLen = 0;
                    g_cursor   = 0;
                }

            } else if (k == KEY_PAGE_UP) {
                g_scroll += visLines / 2;
                CLAMP_SCROLL();

            } else if (k == KEY_PAGE_DOWN) {
                g_scroll -= visLines / 2;
                CLAMP_SCROLL();

            } else if (k == KEY_C && ctrl) {
                /* Ctrl+C — clear input */
                g_input[0] = '\0';
                g_inputLen = 0;
                g_cursor   = 0;

            } else if (k == KEY_L && ctrl) {
                /* Ctrl+L — clear screen */
                g_lineCount = 0;
                g_scroll    = 0;
            }
        }

        /* typed printable characters */
        int ch;
        while ((ch = GetCharPressed())) {
            if (ch >= 32 && g_inputLen < MAX_INPUT - 1) {
                memmove(&g_input[g_cursor + 1], &g_input[g_cursor],
                        (size_t)(g_inputLen - g_cursor + 1));
                g_input[g_cursor] = (char)ch;
                g_cursor++;
                g_inputLen++;
                ResetBlink();
            }
        }

        /* ── draw ──────────────────────────────────────────────────── */
        BeginDrawing();
        ClearBackground(C_BG);

        /* output lines */
        int startLine = g_lineCount - visLines - g_scroll;
        if (startLine < 0) startLine = 0;
        int endLine = startLine + visLines;
        if (endLine > g_lineCount) endLine = g_lineCount;

        int textAreaW = W - PAD * 2 - 10; /* leave room for scrollbar */

        for (int i = startLine; i < endLine; i++) {
            int y = PAD + (i - startLine) * LINE_HEIGHT;
            const char *txt = g_lines[i].text;

            if (MeasureText(txt, FONT_SIZE) <= textAreaW) {
                DrawText(txt, PAD, y, FONT_SIZE, g_lines[i].color);
            } else {
                /* truncate with ellipsis if too wide */
                char trunc[MAX_LINE_LEN];
                strncpy(trunc, txt, MAX_LINE_LEN - 4);
                trunc[MAX_LINE_LEN - 4] = '\0';
                int tlen = (int)strlen(trunc);
                while (tlen > 3 && MeasureText(trunc, FONT_SIZE) > textAreaW) {
                    trunc[--tlen] = '\0';
                }
                strcat(trunc, "...");
                DrawText(trunc, PAD, y, FONT_SIZE, g_lines[i].color);
            }
        }

        /* scrollbar track + thumb */
        if (g_lineCount > visLines) {
            int trackY = PAD;
            int trackH = outH - PAD * 2;
            int barH   = trackH * visLines / g_lineCount;
            if (barH < 8) barH = 8;
            int maxS   = g_lineCount - visLines;
            double ratio = maxS > 0 ? (double)(maxS - g_scroll) / maxS : 1.0;
            int barY   = trackY + (int)((trackH - barH) * ratio);
            DrawRectangle(W - 6, trackY, 4, trackH, C_SEP);
            DrawRectangle(W - 6, barY,   4, barH,   C_SCROLLBAR);
        }

        /* separator */
        DrawRectangle(0, outH, W, 1, C_SEP);

        /* input area background */
        DrawRectangle(0, outH + 1, W, inputH, C_INPUT_BG);

        /* prompt */
        char prompt[MAX_PATH + 4];
        snprintf(prompt, sizeof(prompt), "%s> ", g_cwd);
        int promptW = MeasureText(prompt, FONT_SIZE);
        DrawText(prompt, PAD, outH + 1 + PAD, FONT_SIZE, C_DIR);

        /* typed input */
        DrawText(g_input, PAD + promptW, outH + 1 + PAD, FONT_SIZE, C_TEXT);

        /* cursor */
        if (g_blinkOn) {
            char before[MAX_INPUT];
            strncpy(before, g_input, (size_t)g_cursor);
            before[g_cursor] = '\0';
            int cx = PAD + promptW + MeasureText(before, FONT_SIZE);
            DrawRectangle(cx, outH + 1 + PAD, 2, FONT_SIZE, C_CURSOR);
        }

        EndDrawing();
    }

    CloseWindow();
    return 0;
}
