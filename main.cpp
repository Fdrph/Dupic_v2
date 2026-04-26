#include <string>
#include <vector>
#include <cstdlib>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <SDL3_image/SDL_image.h>
#include <vips/vips8>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef COLOR_BACKGROUND
#endif

#define CLAY_IMPLEMENTATION
#include "clay.h"
#include "renderers/SDL3/clay_renderer_SDL3.c"
#include "layout.h"
#include "scan.h"

SDL_Window*            g_window        = nullptr;
SDL_Renderer*          g_sdlRenderer   = nullptr;
Clay_SDL3RendererData* g_rendererData  = nullptr;
SDL_Texture*           g_folderIconTex   = nullptr;
SDL_Texture*           g_minimizeIconTex = nullptr;
SDL_Texture*           g_maximizeIconTex = nullptr;
SDL_Texture*           g_closeIconTex    = nullptr;
SDL_Texture*           g_markOverlayTex  = nullptr;

bool     g_mouseLeftDown    = false;
bool     g_mouseLeftPressed = false;
bool     g_mouseRightPressed = false;
bool     g_alreadyRendered  = false;
bool     g_shouldClose      = false;
bool     g_pendingMaximize  = false;

uint64_t g_lastTicks         = 0;
float    g_fps               = 0.0f;
uint64_t g_targetFrameNS     = 0;
Uint32   g_folderDialogEvent = 0;
Uint32   g_scanDoneEvent     = 0;
bool     g_scanning          = false;

float       strictnessSlider  = 7.0f;
std::string currentPath       = "Select a folder...";
std::vector<std::string> selectedFolders;
std::string selectedImagePath;
std::string selectedImageRes;
std::string selectedImageSize;
std::vector<std::vector<std::string>> duplicateGroups;


static Clay_Dimensions SDL3_MeasureText(Clay_StringSlice text, Clay_TextElementConfig* config, void* userData) 
{
    TTF_Font* font = ((TTF_Font**)userData)[config->fontId];
    if (!font || text.length == 0) return {0, 0};
    TTF_SetFontSizeDPI(font, (float)config->fontSize, 72, 72);
    int w = 0, h = 0;
    TTF_GetStringSize(font, text.chars, (size_t)text.length, &w, &h);
    return {(float)w, (float)h};
}


static void RenderFrame(float dt) 
{
    int w, h;
    float mx, my;
    SDL_GetWindowSize(g_window, &w, &h);
    SDL_GetMouseState(&mx, &my);
    Clay_SetLayoutDimensions({(float)w, (float)h});
    Clay_SetPointerState({mx, my}, g_mouseLeftDown);
    UpdateLayout();
    SDL_SetRenderDrawColor(g_sdlRenderer, 0x2E, 0x2C, 0x29, 0xFF);
    SDL_RenderClear(g_sdlRenderer);
    Clay_RenderCommandArray cmds = Clay_EndLayout(dt);
    SDL_Clay_RenderClayCommands(g_rendererData, &cmds);
    SDL_RenderPresent(g_sdlRenderer);
    if (g_pendingMaximize) {
        g_pendingMaximize = false;
        if (SDL_GetWindowFlags(g_window) & SDL_WINDOW_MAXIMIZED)
            SDL_RestoreWindow(g_window);
        else
            SDL_MaximizeWindow(g_window);
    }
}

static SDL_HitTestResult HitTestCallback(SDL_Window* win, const SDL_Point* area, void*) 
{
    int w, h;
    SDL_GetWindowSize(win, &w, &h);
    const int BORDER = 8, TITLE_H = 30, BTNS_W = 151;

    if (!(SDL_GetWindowFlags(win) & SDL_WINDOW_MAXIMIZED)) {
        bool onL = area->x < BORDER, onR = area->x >= w - BORDER;
        bool onT = area->y < BORDER, onB = area->y >= h - BORDER;
        if (onT && onL) return SDL_HITTEST_RESIZE_TOPLEFT;
        if (onT && onR) return SDL_HITTEST_RESIZE_TOPRIGHT;
        if (onB && onL) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
        if (onB && onR) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
        if (onT)        return SDL_HITTEST_RESIZE_TOP;
        if (onB)        return SDL_HITTEST_RESIZE_BOTTOM;
        if (onL)        return SDL_HITTEST_RESIZE_LEFT;
        if (onR)        return SDL_HITTEST_RESIZE_RIGHT;
    }

    if (area->y < TITLE_H && area->x < w - BTNS_W) return SDL_HITTEST_DRAGGABLE;
    return SDL_HITTEST_NORMAL;
}

// Re-render on resize events. Without this the UI would look frozen until we let go of the mouse.
static bool SDLCALL ResizeEventWatcher(void*, SDL_Event* ev) 
{
    if (ev->type == SDL_EVENT_WINDOW_RESIZED || ev->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) 
    {
        float dt = g_lastTicks ? (float)(SDL_GetTicks() - g_lastTicks) / 1000.0f : 0.016f;
        g_mouseLeftPressed = false;  // prevent button re-trigger during re-entrant render
        RenderFrame(dt);
        g_alreadyRendered = true;
    }
    return false;
}

// This sets the target framerate (in nanoseconds) based on the current display's refresh rate.
static void UpdateFrameTarget() 
{
    SDL_DisplayID          disp = SDL_GetDisplayForWindow(g_window);
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(disp);
    float hz = (mode && mode->refresh_rate > 0.0f) ? mode->refresh_rate : 60.0f;
    g_targetFrameNS = (uint64_t)(1e9 / hz);
}

int main(int argc, char* argv[]) 
{
    
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) return -1;
    if (!TTF_Init()) return -1;

    g_window = SDL_CreateWindow("Dupic", 1280, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS);
    if (!g_window) return -1;

    g_sdlRenderer = SDL_CreateRenderer(g_window, nullptr);
    if (!g_sdlRenderer) return -1;

    SDL_SetRenderDrawBlendMode(g_sdlRenderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderVSync(g_sdlRenderer, 0);
    SDL_SetWindowMinimumSize(g_window, 1280, 720);
    SDL_SetWindowHitTest(g_window, HitTestCallback, nullptr);
    UpdateFrameTarget();

    // Set Windows icon
    #ifdef _WIN32
    {
        HINSTANCE hInst = GetModuleHandle(nullptr);
        HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(g_window),
                         SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        if (hwnd) {
            SendMessage(hwnd, WM_SETICON, ICON_BIG,
                (LPARAM)LoadImage(hInst, MAKEINTRESOURCE(1), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
            SendMessage(hwnd, WM_SETICON, ICON_SMALL,
                (LPARAM)LoadImage(hInst, MAKEINTRESOURCE(1), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
        }
    }
    #endif

    const char* base = SDL_GetBasePath();
    char path[512];
 
    // Load font
    TTF_Font* fonts[1] = {};
    SDL_snprintf(path, sizeof(path), "%sassets/fonts/Inter-VariableFont.ttf", base);
    fonts[0] = TTF_OpenFont(path, 16.0f);
    if (!fonts[0]) return -1; 
   
    // Load folder icon svg
    SDL_snprintf(path, sizeof(path), "%sassets/folder-icon-white.svg", base ? base : "");
    SDL_Surface* surf = IMG_LoadSizedSVG_IO(SDL_IOFromFile(path, "rb"), 35, 30);
    if (surf) 
    {
        g_folderIconTex = SDL_CreateTextureFromSurface(g_sdlRenderer, surf);
        SDL_SetTextureBlendMode(g_folderIconTex, SDL_BLENDMODE_BLEND);
        SDL_DestroySurface(surf);
    }

    // Load titlebar icon SVGs
    auto loadSvgIcon = [&](const char* name, int w, int h) -> SDL_Texture* {
        SDL_snprintf(path, sizeof(path), "%sassets/%s", base ? base : "", name);
        SDL_Surface* s = IMG_LoadSizedSVG_IO(SDL_IOFromFile(path, "rb"), w, h);
        if (!s) return nullptr;
        SDL_Texture* t = SDL_CreateTextureFromSurface(g_sdlRenderer, s);
        if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_DestroySurface(s);
        return t;
    };
    g_minimizeIconTex = loadSvgIcon("minimize.svg", 16, 16);
    g_maximizeIconTex = loadSvgIcon("maximize.svg", 16, 16);
    g_closeIconTex    = loadSvgIcon("close.svg",    16, 16);
    g_markOverlayTex  = loadSvgIcon("close.svg",    40, 40);
    
    TTF_TextEngine* textEngine = TTF_CreateRendererTextEngine(g_sdlRenderer);
    Clay_SDL3RendererData rdData = { g_sdlRenderer, textEngine, fonts };
    g_rendererData = &rdData;

    uint64_t clayMemSz = Clay_MinMemorySize();
    Clay_Initialize(
        Clay_CreateArenaWithCapacityAndMemory(clayMemSz, (char*)malloc(clayMemSz)),
        Clay_Dimensions{1280, 720},
        Clay_ErrorHandler{}
    );
    Clay_SetMeasureTextFunction(SDL3_MeasureText, fonts);

    if (VIPS_INIT("Dupic")) return -1;

    g_folderDialogEvent = SDL_RegisterEvents(1);
    g_scanDoneEvent     = SDL_RegisterEvents(1);

    SDL_AddEventWatch(ResizeEventWatcher, nullptr);

    g_lastTicks = SDL_GetTicks();
    uint64_t fpsTimerTick = g_lastTicks;
    int fpsFrames = 0;

    // Main loop
    while (!g_shouldClose) 
    {
        uint64_t frameStart = SDL_GetTicksNS();
        g_mouseLeftPressed  = false;
        g_mouseRightPressed = false;
        g_alreadyRendered   = false;
        float scrollY       = 0.0f;

        SDL_Event ev;
        bool quit = false;
        while (SDL_PollEvent(&ev)) 
        {
            switch (ev.type) {
                case SDL_EVENT_QUIT:               
                    quit = true;
                    break;
                case SDL_EVENT_MOUSE_WHEEL:        
                    scrollY = ev.wheel.y * 20.0f;
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    if (ev.button.button == SDL_BUTTON_LEFT)  { g_mouseLeftDown = true; g_mouseLeftPressed = true; }
                    if (ev.button.button == SDL_BUTTON_RIGHT) { g_mouseRightPressed = true; }
                    break;
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    if (ev.button.button == SDL_BUTTON_LEFT) { g_mouseLeftDown = false; }
                    break;
                case SDL_EVENT_WINDOW_MOUSE_LEAVE:
                    Clay_SetPointerState({-1.0f, -1.0f}, false);
                    break;
                case SDL_EVENT_WINDOW_DISPLAY_CHANGED: 
                    UpdateFrameTarget(); 
                    break;
                default:
                    if (ev.type == g_folderDialogEvent && ev.user.data1) 
                    {
                        auto* paths = static_cast<std::vector<std::string>*>(ev.user.data1);
                        selectedFolders = std::move(*paths);
                        delete paths;
                        currentPath = selectedFolders.size() == 1
                            ? selectedFolders[0]
                            : std::to_string(selectedFolders.size()) + " folders selected";
                        InvalidateScanCache();

                    } else if (ev.type == g_scanDoneEvent && ev.user.data1)
                    {
                        auto* groups = static_cast<std::vector<std::vector<std::string>>*>(ev.user.data1);
                        duplicateGroups = std::move(*groups);
                        delete groups;
                        g_scanning = false;
                        RebuildThumbSlots();
                        QueueAllPreviews();
                    }
                    break;
            }
        }
        if (quit) break;

        uint64_t now = SDL_GetTicks();
        float dt = (float)(now - g_lastTicks) / 1000.0f;
        if (dt <= 0.0f) dt = 0.016f;
        g_lastTicks = now;

        fpsFrames++;
        if (now - fpsTimerTick >= 1000) {
            g_fps = fpsFrames * 1000.0f / (float)(now - fpsTimerTick);
            fpsFrames = 0;
            fpsTimerTick = now;
        }

        Clay_UpdateScrollContainers(true, {0.0f, scrollY}, dt);
        if (!g_alreadyRendered) RenderFrame(dt);

        uint64_t elapsed = SDL_GetTicksNS() - frameStart;
        if (g_targetFrameNS > elapsed) SDL_DelayNS(g_targetFrameNS - elapsed);
    }

    // Cleanup
    SDL_RemoveEventWatch(ResizeEventWatcher, nullptr);
    ClearTextureCache();
    if (g_folderIconTex)   SDL_DestroyTexture(g_folderIconTex);
    if (g_minimizeIconTex) SDL_DestroyTexture(g_minimizeIconTex);
    if (g_maximizeIconTex) SDL_DestroyTexture(g_maximizeIconTex);
    if (g_closeIconTex)    SDL_DestroyTexture(g_closeIconTex);
    if (g_markOverlayTex)  SDL_DestroyTexture(g_markOverlayTex);

    TTF_CloseFont(fonts[0]);
    TTF_DestroyRendererTextEngine(textEngine);
    SDL_DestroyRenderer(g_sdlRenderer);
    SDL_DestroyWindow(g_window);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
