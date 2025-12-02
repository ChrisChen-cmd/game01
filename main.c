#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "msimg32.lib")

#define ARRAY_COUNT(x) ((int)(sizeof(x) / sizeof((x)[0])))

typedef struct BackBuffer {
    HDC hdc;
    HBITMAP bitmap;
    HBITMAP old_bitmap;
    int width;
    int height;
} BackBuffer;

typedef struct Player {
    float x;
    float y;
    float vx;
    float vy;
    float radius;
} Player;

typedef struct Orb {
    float x;
    float y;
    float vx;
    float vy;
    float radius;
    COLORREF color;
    bool harmful;
} Orb;

typedef struct Star {
    float x;
    float y;
    float speed;
} Star;

typedef struct GameState {
    bool initialized;
    bool game_over;
    int score;
    float elapsed;
    float difficulty;
    Player player;
    Orb hazards[28];
    Orb pickups[12];
    Star stars[64];
} GameState;

static BackBuffer g_back = {0};
static GameState g_game = {0};
static bool g_running = true;
static bool g_keys[256] = {0};

static float clampf(float v, float min, float max) {
    if (v < min) return min;
    if (v > max) return max;
    return v;
}

static float randf01(void) {
    return (float)rand() / (float)RAND_MAX;
}

static float rand_range(float min, float max) {
    return min + (max - min) * randf01();
}

static COLORREF mix_color(COLORREF a, COLORREF b, float t) {
    t = clampf(t, 0.0f, 1.0f);
    int ar = GetRValue(a), ag = GetGValue(a), ab = GetBValue(a);
    int br = GetRValue(b), bg = GetGValue(b), bb = GetBValue(b);
    int r = (int)(ar + (br - ar) * t);
    int g = (int)(ag + (bg - ag) * t);
    int b2 = (int)(ab + (bb - ab) * t);
    return RGB(r, g, b2);
}

static void destroy_backbuffer(BackBuffer *bb) {
    if (bb->hdc) {
        SelectObject(bb->hdc, bb->old_bitmap);
        DeleteObject(bb->bitmap);
        DeleteDC(bb->hdc);
    }
    bb->hdc = NULL;
    bb->bitmap = NULL;
    bb->old_bitmap = NULL;
    bb->width = 0;
    bb->height = 0;
}

static void create_backbuffer(HWND hwnd, BackBuffer *bb, int width, int height) {
    if (width <= 0 || height <= 0) return;
    destroy_backbuffer(bb);
    HDC window_dc = GetDC(hwnd);
    bb->hdc = CreateCompatibleDC(window_dc);
    bb->bitmap = CreateCompatibleBitmap(window_dc, width, height);
    bb->old_bitmap = (HBITMAP)SelectObject(bb->hdc, bb->bitmap);
    bb->width = width;
    bb->height = height;
    ReleaseDC(hwnd, window_dc);
}

static void draw_background(HDC dc, int width, int height) {
    TRIVERTEX verts[2];
    verts[0].x = 0;
    verts[0].y = 0;
    verts[0].Red = (USHORT)(12 << 8);
    verts[0].Green = (USHORT)(24 << 8);
    verts[0].Blue = (USHORT)(45 << 8);
    verts[0].Alpha = 0;

    verts[1].x = width;
    verts[1].y = height;
    verts[1].Red = (USHORT)(20 << 8);
    verts[1].Green = (USHORT)(46 << 8);
    verts[1].Blue = (USHORT)(78 << 8);
    verts[1].Alpha = 0;

    GRADIENT_RECT rect = {0, 1};
    GradientFill(dc, verts, 2, &rect, 1, GRADIENT_FILL_RECT_V);

    RECT haze = {0, (int)(height * 0.72f), width, height};
    HBRUSH haze_brush = CreateSolidBrush(RGB(20, 32, 60));
    FillRect(dc, &haze, haze_brush);
    DeleteObject(haze_brush);
}

static void draw_star_field(HDC dc, Star *stars, int count, int width, int height) {
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(180, 205, 230));
    HPEN old_pen = (HPEN)SelectObject(dc, pen);
    for (int i = 0; i < count; ++i) {
        MoveToEx(dc, (int)stars[i].x, (int)stars[i].y, NULL);
        LineTo(dc, (int)stars[i].x, (int)(stars[i].y + 1));
    }
    SelectObject(dc, old_pen);
    DeleteObject(pen);
}

static void draw_soft_circle(HDC dc, float cx, float cy, float r, COLORREF base, COLORREF rim) {
    HPEN old_pen = (HPEN)SelectObject(dc, GetStockObject(NULL_PEN));

    float outer = r * 1.4f;
    HBRUSH rim_brush = CreateSolidBrush(rim);
    HBRUSH old_brush = (HBRUSH)SelectObject(dc, rim_brush);
    Ellipse(dc, (int)(cx - outer), (int)(cy - outer), (int)(cx + outer), (int)(cy + outer));

    float mid = r * 1.1f;
    HBRUSH mid_brush = CreateSolidBrush(mix_color(base, RGB(255, 255, 255), 0.2f));
    SelectObject(dc, mid_brush);
    Ellipse(dc, (int)(cx - mid), (int)(cy - mid), (int)(cx + mid), (int)(cy + mid));

    HBRUSH core_brush = CreateSolidBrush(base);
    SelectObject(dc, core_brush);
    Ellipse(dc, (int)(cx - r), (int)(cy - r), (int)(cx + r), (int)(cy + r));

    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(rim_brush);
    DeleteObject(mid_brush);
    DeleteObject(core_brush);
}

static void draw_trail(HDC dc, Player *p) {
    HPEN pen = CreatePen(PS_SOLID, 3, RGB(80, 180, 210));
    HPEN old_pen = (HPEN)SelectObject(dc, pen);
    MoveToEx(dc, (int)p->x, (int)p->y + (int)p->radius, NULL);
    LineTo(dc, (int)(p->x - p->vx * 0.08f), (int)(p->y + p->radius + 28));
    SelectObject(dc, old_pen);
    DeleteObject(pen);
}

static void draw_overlay(HDC target, int width, int height, BYTE alpha) {
    HDC mem = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, width, height);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
    HBRUSH brush = CreateSolidBrush(RGB(8, 12, 28));
    RECT rc = {0, 0, width, height};
    FillRect(mem, &rc, brush);
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, alpha, 0};
    AlphaBlend(target, 0, 0, width, height, mem, 0, 0, width, height, bf);
    DeleteObject(brush);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static void reset_orb(Orb *o, int width, int height, bool harmful, float difficulty) {
    o->radius = harmful ? rand_range(14.0f, 26.0f) : rand_range(10.0f, 18.0f);
    o->x = rand_range(o->radius, (float)width - o->radius);
    o->y = -rand_range(40.0f, 200.0f);
    o->vx = rand_range(-24.0f, 24.0f);
    o->vy = harmful ? rand_range(120.0f, 190.0f + difficulty * 30.0f)
                    : rand_range(80.0f, 120.0f);
    o->harmful = harmful;

    COLORREF palette_hazard[] = {
        RGB(255, 144, 164),
        RGB(255, 195, 113),
        RGB(250, 132, 176),
        RGB(255, 170, 130),
    };
    COLORREF palette_pickup[] = {
        RGB(120, 230, 210),
        RGB(140, 210, 255),
        RGB(170, 255, 200),
    };
    if (harmful) {
        o->color = palette_hazard[rand() % ARRAY_COUNT(palette_hazard)];
    } else {
        o->color = palette_pickup[rand() % ARRAY_COUNT(palette_pickup)];
    }
}

static void reset_game(GameState *game, int width, int height) {
    game->game_over = false;
    game->score = 0;
    game->elapsed = 0.0f;
    game->difficulty = 0.0f;
    game->player.x = width * 0.5f;
    game->player.y = height * 0.72f;
    game->player.vx = 0.0f;
    game->player.vy = 0.0f;
    game->player.radius = 18.0f;

    for (int i = 0; i < ARRAY_COUNT(game->hazards); ++i) {
        reset_orb(&game->hazards[i], width, height, true, game->difficulty);
        game->hazards[i].y = rand_range(-height, (float)height * 0.25f);
    }
    for (int i = 0; i < ARRAY_COUNT(game->pickups); ++i) {
        reset_orb(&game->pickups[i], width, height, false, game->difficulty);
        game->pickups[i].y = rand_range(-height, (float)height * 0.4f);
    }
    for (int i = 0; i < ARRAY_COUNT(game->stars); ++i) {
        game->stars[i].x = rand_range(0.0f, (float)width);
        game->stars[i].y = rand_range(0.0f, (float)height);
        game->stars[i].speed = rand_range(30.0f, 90.0f);
    }
    game->initialized = true;
}

static void update_game(GameState *game, float dt, int width, int height) {
    if (!game->initialized) {
        reset_game(game, width, height);
    }

    if (game->game_over) {
        return;
    }

    game->elapsed += dt;
    game->difficulty += dt * 0.08f;
    game->score += (int)(dt * 12.0f);

    float input_x = 0.0f;
    float input_y = 0.0f;
    if (g_keys['A'] || g_keys[VK_LEFT]) input_x -= 1.0f;
    if (g_keys['D'] || g_keys[VK_RIGHT]) input_x += 1.0f;
    if (g_keys['W'] || g_keys[VK_UP]) input_y -= 1.0f;
    if (g_keys['S'] || g_keys[VK_DOWN]) input_y += 1.0f;

    const float accel = 720.0f;
    const float damping = 6.0f;

    game->player.vx += input_x * accel * dt;
    game->player.vy += input_y * accel * dt;
    game->player.vx -= game->player.vx * damping * dt;
    game->player.vy -= game->player.vy * damping * dt;

    game->player.x += game->player.vx * dt;
    game->player.y += game->player.vy * dt;

    float margin = game->player.radius + 8.0f;
    game->player.x = clampf(game->player.x, margin, width - margin);
    game->player.y = clampf(game->player.y, margin, height - margin);

    for (int i = 0; i < ARRAY_COUNT(game->hazards); ++i) {
        Orb *o = &game->hazards[i];
        o->x += o->vx * dt;
        o->y += (o->vy + sinf(game->elapsed * 1.5f + i) * 18.0f) * dt;
        if (o->y - o->radius > height + 40 || o->x < -40 || o->x > width + 40) {
            reset_orb(o, width, height, true, game->difficulty);
        }
    }

    for (int i = 0; i < ARRAY_COUNT(game->pickups); ++i) {
        Orb *o = &game->pickups[i];
        o->x += o->vx * dt * 0.5f;
        o->y += (o->vy + cosf(game->elapsed * 1.3f + i) * 12.0f) * dt;
        if (o->y - o->radius > height + 20 || o->x < -40 || o->x > width + 40) {
            reset_orb(o, width, height, false, game->difficulty);
        }
    }

    for (int i = 0; i < ARRAY_COUNT(game->stars); ++i) {
        Star *s = &game->stars[i];
        s->y += s->speed * dt;
        if (s->y > height) {
            s->y = -4.0f;
            s->x = rand_range(0.0f, (float)width);
        }
    }

    const float player_r = game->player.radius;
    for (int i = 0; i < ARRAY_COUNT(game->hazards); ++i) {
        Orb *o = &game->hazards[i];
        float dx = o->x - game->player.x;
        float dy = o->y - game->player.y;
        float dist2 = dx * dx + dy * dy;
        float rr = (o->radius + player_r) * (o->radius + player_r);
        if (dist2 < rr) {
            game->game_over = true;
            break;
        }
    }

    for (int i = 0; i < ARRAY_COUNT(game->pickups); ++i) {
        Orb *o = &game->pickups[i];
        float dx = o->x - game->player.x;
        float dy = o->y - game->player.y;
        float dist2 = dx * dx + dy * dy;
        float rr = (o->radius + player_r * 0.8f) * (o->radius + player_r * 0.8f);
        if (dist2 < rr) {
            game->score += 55;
            reset_orb(o, width, height, false, game->difficulty);
        }
    }
}

static void render_ui(HDC dc, int width, int height, const GameState *game, HFONT font_small, HFONT font_big) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(220, 230, 240));
    HFONT old_font = (HFONT)SelectObject(dc, font_small);

    char buffer[128];
    wsprintfA(buffer, "Score: %d", game->score);
    RECT rc = {12, 8, width, 32};
    DrawTextA(dc, buffer, -1, &rc, DT_LEFT | DT_TOP);

    wsprintfA(buffer, "Time: %.1fs", game->elapsed);
    rc.top = 28;
    DrawTextA(dc, buffer, -1, &rc, DT_LEFT | DT_TOP);

    if (game->game_over) {
        draw_overlay(dc, width, height, 110);
        SelectObject(dc, font_big);
        SetTextColor(dc, RGB(255, 212, 190));
        RECT center = {0, (int)(height * 0.38f), width, (int)(height * 0.38f) + 120};
        DrawTextA(dc, "Calm Flight Lost", -1, &center, DT_CENTER | DT_TOP);

        SelectObject(dc, font_small);
        SetTextColor(dc, RGB(200, 220, 240));
        RECT tip = {0, (int)(height * 0.55f), width, height};
        DrawTextA(dc, "Press R to drift again   |   Esc to quit", -1, &tip, DT_CENTER | DT_TOP);
    }

    SelectObject(dc, old_font);
}

static void render_game(HWND hwnd, BackBuffer *bb, GameState *game, HFONT font_small, HFONT font_big) {
    if (!bb->hdc || bb->width <= 0 || bb->height <= 0) return;

    draw_background(bb->hdc, bb->width, bb->height);
    draw_star_field(bb->hdc, game->stars, ARRAY_COUNT(game->stars), bb->width, bb->height);

    draw_trail(bb->hdc, &game->player);
    draw_soft_circle(bb->hdc, game->player.x, game->player.y, game->player.radius,
                     RGB(120, 220, 255), RGB(40, 120, 200));

    for (int i = 0; i < ARRAY_COUNT(game->pickups); ++i) {
        draw_soft_circle(bb->hdc, game->pickups[i].x, game->pickups[i].y, game->pickups[i].radius,
                         game->pickups[i].color, mix_color(game->pickups[i].color, RGB(20, 40, 80), 0.3f));
    }
    for (int i = 0; i < ARRAY_COUNT(game->hazards); ++i) {
        draw_soft_circle(bb->hdc, game->hazards[i].x, game->hazards[i].y, game->hazards[i].radius,
                         game->hazards[i].color, mix_color(game->hazards[i].color, RGB(12, 18, 40), 0.5f));
    }

    render_ui(bb->hdc, bb->width, bb->height, game, font_small, font_big);

    HDC window_dc = GetDC(hwnd);
    BitBlt(window_dc, 0, 0, bb->width, bb->height, bb->hdc, 0, 0, SRCCOPY);
    ReleaseDC(hwnd, window_dc);
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_DESTROY:
            g_running = false;
            PostQuitMessage(0);
            return 0;
        case WM_SIZE: {
            int width = LOWORD(lParam);
            int height = HIWORD(lParam);
            create_backbuffer(hwnd, &g_back, width, height);
        } break;
        case WM_KEYDOWN:
            if (wParam < 256) g_keys[wParam] = true;
            if (wParam == VK_ESCAPE) {
                g_running = false;
                DestroyWindow(hwnd);
            }
            if (wParam == 'R' && g_game.game_over) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                reset_game(&g_game, rc.right - rc.left, rc.bottom - rc.top);
            }
            return 0;
        case WM_KEYUP:
            if (wParam < 256) g_keys[wParam] = false;
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE prev, LPSTR cmd, int show) {
    (void)prev;
    (void)cmd;
    srand((unsigned int)time(NULL));

    const wchar_t CLASS_NAME[] = L"CalmFlightWindow";
    WNDCLASS wc = {0};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    if (!RegisterClass(&wc)) {
        MessageBox(NULL, L"Failed to register window class", L"Error", MB_ICONERROR);
        return 0;
    }

    DWORD style = WS_OVERLAPPEDWINDOW;
    RECT desired = {0, 0, 960, 600};
    AdjustWindowRect(&desired, style, FALSE);
    HWND hwnd = CreateWindowEx(
        0, CLASS_NAME, L"Calm Flight - C & Win32",
        style, CW_USEDEFAULT, CW_USEDEFAULT,
        desired.right - desired.left,
        desired.bottom - desired.top,
        NULL, NULL, hInstance, NULL);

    if (!hwnd) {
        MessageBox(NULL, L"Failed to create window", L"Error", MB_ICONERROR);
        return 0;
    }

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    RECT rc;
    GetClientRect(hwnd, &rc);
    create_backbuffer(hwnd, &g_back, rc.right - rc.left, rc.bottom - rc.top);

    HFONT font_small = CreateFont(
        -18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, ANSI_CHARSET,
        OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        FF_SWISS, TEXT("Segoe UI"));
    HFONT font_big = CreateFont(
        -36, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET,
        OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        FF_SWISS, TEXT("Segoe UI"));

    LARGE_INTEGER freq = {0};
    LARGE_INTEGER last = {0};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);

    MSG msg;
    while (g_running) {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float dt = (float)(now.QuadPart - last.QuadPart) / (float)freq.QuadPart;
        if (dt > 0.05f) dt = 0.05f;
        last = now;

        RECT client;
        GetClientRect(hwnd, &client);
        int width = client.right - client.left;
        int height = client.bottom - client.top;
        update_game(&g_game, dt, width, height);
        render_game(hwnd, &g_back, &g_game, font_small, font_big);
    }

    destroy_backbuffer(&g_back);
    DeleteObject(font_small);
    DeleteObject(font_big);
    return 0;
}
