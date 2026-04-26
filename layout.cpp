#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <unordered_set>
#include <cstdio>
#include <cstring>

#include <SDL3/SDL.h>
#include <vips/vips.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef COLOR_BACKGROUND
#endif

#include "clay.h"
#include "layout.h"
#include "scan.h"

extern SDL_Window*   g_window;
extern SDL_Renderer* g_sdlRenderer;
extern Uint32        g_folderDialogEvent;
extern bool          g_mouseLeftDown, g_mouseLeftPressed, g_mouseRightPressed;
extern float         g_fps;
extern bool          g_scanning;
extern SDL_Texture*  g_folderIconTex;
extern SDL_Texture*  g_minimizeIconTex;
extern SDL_Texture*  g_maximizeIconTex;
extern SDL_Texture*  g_closeIconTex;
extern SDL_Texture*  g_markOverlayTex;
extern bool          g_shouldClose;
extern bool          g_pendingMaximize;
extern float         strictnessSlider;
extern std::string   currentPath, selectedImagePath, selectedImageRes, selectedImageSize;
extern std::vector<std::string> selectedFolders;
extern std::vector<std::vector<std::string>> duplicateGroups;

// ── Texture cache ────────────────────────────────────────────────────────────
static std::string NiceBytes(uintmax_t bytes);
// Both vectors are parallel to duplicateGroups[gi][ji]
static std::vector<std::vector<SDL_Texture*>> g_thumbTextures;
static std::vector<std::vector<SDL_Texture*>> g_previewTextures;
static int g_selGi = -1, g_selJi = -1;
static std::unordered_set<std::string> g_markedPaths;

// ── Async preview loader ─────────────────────────────────────────────────────
struct PreviewWork  { int gi, ji; std::string path; };
struct PreviewReady { int gi, ji; void* pixels; int w, h, ch; };

// At most this many images decompress simultaneously, bounding peak CPU RAM.
static constexpr int MAX_DONE_INFLIGHT = 2;
static constexpr int RESERVE_GROUPS = 512;

static std::mutex              g_loaderMtx;
static std::condition_variable g_loaderCV;
static std::queue<PreviewWork>   g_workQueue;
static std::vector<PreviewReady> g_doneQueue;
static bool                    g_workStop     = false;
static int                     g_doneInflight = 0;
static std::vector<std::thread> g_loaderThreads;

// Vips pipeline: flatten alpha, convert to sRGB UCHAR, write to tightly-packed CPU buffer.
// Frees img on all paths. Caller must g_free() the returned mem on success.
struct RawPixels { void* mem; int w, h, ch; };
static RawPixels VipsToRaw(VipsImage* img) 
{
    VipsImage* tmp = nullptr;

    // Discard alpha by only extracting the first 3 bands (RGB)
    if (vips_image_get_bands(img) > 3) 
    {
        if (vips_extract_band(img, &tmp, 0, "n", 3, nullptr) != 0) 
        {
            g_object_unref(img);
            return {nullptr, 0, 0, 0};
        }
        g_object_unref(img);
        img = tmp;
    }

    size_t sz;
    void* mem = vips_image_write_to_memory(img, &sz);
    int w = vips_image_get_width(img);
    int h = vips_image_get_height(img);
    int ch = vips_image_get_bands(img);

    g_object_unref(img);
    return {mem, w, h, ch};
}

// decode VipsImage* directly to a GPU texture. Frees img via VipsToRaw.
static SDL_Texture* LoadThumb(const std::string& path) {
    VipsImage* img = nullptr;
    if (vips_thumbnail(path.c_str(), &img, 150, nullptr) != 0) return nullptr;
    
    RawPixels raw = VipsToRaw(img);
    if (!raw.mem) return nullptr;
    SDL_PixelFormat fmt = (raw.ch == 4) ? SDL_PIXELFORMAT_RGBA32 : SDL_PIXELFORMAT_RGB24;
    SDL_Texture* tex = SDL_CreateTexture(g_sdlRenderer, fmt, SDL_TEXTUREACCESS_STATIC, raw.w, raw.h);
    if (tex) SDL_UpdateTexture(tex, nullptr, raw.mem, raw.w * raw.ch);
    g_free(raw.mem);
    return tex;
}

static void LoaderWorker() {
    vips_concurrency_set(1);
    vips_cache_set_max(0);
    while (true) {
        PreviewWork item;
        {
            std::unique_lock lk(g_loaderMtx);
            g_loaderCV.wait(lk, [] { return !g_workQueue.empty() || g_workStop; });
            if (g_workStop && g_workQueue.empty()) break;
            item = std::move(g_workQueue.front());
            g_workQueue.pop();
            // Wait for an inflight slot before decoding (bounds peak CPU RAM)
            g_loaderCV.wait(lk, [] { return g_doneInflight < MAX_DONE_INFLIGHT || g_workStop; });
            if (g_workStop) break;
            g_doneInflight++;
        }
        // Load at native resolution — no vips_thumbnail size cap.
        VipsImage* img = vips_image_new_from_file(item.path.c_str(), nullptr);
        RawPixels raw = img ? VipsToRaw(img) : RawPixels{};
        {
            std::lock_guard lk(g_loaderMtx);
            if (raw.mem)
                g_doneQueue.push_back({item.gi, item.ji, raw.mem, raw.w, raw.h, raw.ch});
            else
                g_doneInflight--;  // decode failed; release slot
        }
        g_loaderCV.notify_one();  // wake main thread (new item) or next worker (slot freed)
    }
    vips_thread_shutdown();
}

static void StopLoaderThreads() {
    {
        std::lock_guard lk(g_loaderMtx);
        g_workStop = true;
        while (!g_workQueue.empty()) g_workQueue.pop();
    }
    g_loaderCV.notify_all();
    for (auto& t : g_loaderThreads)
        if (t.joinable()) t.join();
    g_loaderThreads.clear();
    g_loaderThreads.reserve(MAX_DONE_INFLIGHT);
    for (auto& r : g_doneQueue)
        g_free(r.pixels);
    g_doneQueue.clear();
    g_doneQueue.reserve(MAX_DONE_INFLIGHT);
    g_doneInflight = 0;
}

void QueueAllPreviews() {
    StopLoaderThreads();  // also drains g_doneQueue
    {
        std::lock_guard lk(g_loaderMtx);
        g_workStop = false;
        for (int gi = 0; gi < (int)duplicateGroups.size(); gi++)
            for (int ji = 0; ji < (int)duplicateGroups[gi].size(); ji++)
                g_workQueue.push({gi, ji, duplicateGroups[gi][ji]});
    }
    int hw = (int)std::thread::hardware_concurrency();
    int avail = hw > 2 ? hw - 2 : 1;
    int n = avail < MAX_DONE_INFLIGHT ? avail : MAX_DONE_INFLIGHT;
    for (int i = 0; i < n; i++)
        g_loaderThreads.emplace_back(LoaderWorker);
    g_loaderCV.notify_all();
}

static void OnImageClicked(int gi, int ji) {
    g_selGi = gi;
    g_selJi = ji;
    selectedImagePath = duplicateGroups[gi][ji];
    selectedImageRes  = "";
    selectedImageSize = "";
    try {
        selectedImageSize = NiceBytes(std::filesystem::file_size(selectedImagePath));
    } catch (...) {}
    // header metadata is read immediately, pixel data is only decoded on demand (never triggered here).
    VipsImage* meta = vips_image_new_from_file(selectedImagePath.c_str(), "access", VIPS_ACCESS_SEQUENTIAL, nullptr);
    if (meta) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d x %d", vips_image_get_width(meta), vips_image_get_height(meta));
        selectedImageRes = buf;
        g_object_unref(meta);
    }
}

void RebuildThumbSlots() {
    size_t n = duplicateGroups.size();
    g_thumbTextures.reserve(RESERVE_GROUPS);
    g_previewTextures.reserve(RESERVE_GROUPS);
    g_thumbTextures.resize(n);
    g_previewTextures.resize(n);
    for (size_t i = 0; i < n; i++) {
        size_t m = duplicateGroups[i].size();
        if (g_thumbTextures[i].size()   != m) g_thumbTextures[i].assign(m, nullptr);
        if (g_previewTextures[i].size() != m) g_previewTextures[i].assign(m, nullptr);
    }
}

void ClearTextureCache() {
    StopLoaderThreads();  // also drains g_doneQueue
    for (auto& row : g_thumbTextures)
        for (auto* t : row)
            if (t) SDL_DestroyTexture(t);
    for (auto& row : g_previewTextures)
        for (auto* t : row)
            if (t) SDL_DestroyTexture(t);
    g_thumbTextures.clear();
    g_previewTextures.clear();
    g_selGi = g_selJi = -1;
    selectedImagePath.clear();
    g_markedPaths.clear();
}

static void SDLCALL FolderDialogCallback(void*, const char* const* filelist, int) {
    if (!filelist || !filelist[0]) return;
    auto* paths = new std::vector<std::string>();
    for (int i = 0; filelist[i]; i++) paths->emplace_back(filelist[i]);
    SDL_Event ev;
    SDL_memset(&ev, 0, sizeof(ev));
    ev.type       = g_folderDialogEvent;
    ev.user.data1 = paths;
    SDL_PushEvent(&ev);
}

void OpenFolderDialog() {
    const char* start = selectedFolders.empty() ? nullptr : selectedFolders.back().c_str();
    SDL_ShowOpenFolderDialog(FolderDialogCallback, nullptr, g_window, start, true);
}

void FindDuplicates() {}


static std::string NiceBytes(uintmax_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB"};
    int i = 0;
    double n = (double)bytes;
    while (n >= 1024.0 && i < 3) { n /= 1024.0; i++; }
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f %s", n, units[i]);
    return buf;
}

static void DeleteMarkedFiles() {
    if (g_markedPaths.empty()) return;
    std::unordered_set<std::string> toDelete = g_markedPaths;
    for (const auto& p : toDelete) {
        try { std::filesystem::remove(std::filesystem::u8path(p)); } catch (...) {}
    }
    std::vector<std::vector<std::string>> newGroups;
    for (auto& group : duplicateGroups) {
        std::vector<std::string> newGroup;
        for (auto& p : group)
            if (!toDelete.count(p)) newGroup.push_back(p);
        if (newGroup.size() >= 2)
            newGroups.push_back(std::move(newGroup));
    }
    duplicateGroups = std::move(newGroups);
    ClearTextureCache();  // also clears g_markedPaths
    RebuildThumbSlots();
    QueueAllPreviews();
}

void UpdateLayout() {
    // Drain: upload decoded pixel buffers to GPU, release inflight slots.
    {
        std::lock_guard lk(g_loaderMtx);
        for (auto& r : g_doneQueue) {
            SDL_PixelFormat fmt = (r.ch == 4) ? SDL_PIXELFORMAT_RGBA32 : SDL_PIXELFORMAT_RGB24;
            SDL_Texture* tex = SDL_CreateTexture(g_sdlRenderer, fmt, SDL_TEXTUREACCESS_STATIC, r.w, r.h);
            if (tex) SDL_UpdateTexture(tex, nullptr, r.pixels, r.w * r.ch);
            g_free(r.pixels);
            if (tex && r.gi < (int)g_previewTextures.size() &&
                       r.ji < (int)g_previewTextures[r.gi].size() &&
                       !g_previewTextures[r.gi][r.ji])
                g_previewTextures[r.gi][r.ji] = tex;
            else if (tex)
                SDL_DestroyTexture(tex);
            g_doneInflight--;
        }
        g_doneQueue.clear();
    }
    g_loaderCV.notify_all();

    const Clay_Color cBg       = {46,  44,  41,  255};
    const Clay_Color cTitle    = {53,  53,  53,  255};
    const Clay_Color cBtn      = {68,  68,  68,  255};
    const Clay_Color cHover    = {80,  80,  80,  255};
    const Clay_Color cText     = {255, 255, 255, 255};
    const Clay_Color cTextDim  = {177, 177, 177, 255};
    const Clay_Color cTextGray = {197, 197, 197, 255};
    const Clay_Color cPath     = {129, 128, 126, 255};
    const Clay_Color cProps    = {51,  51,  51,  255};
    const Clay_Color cNone     = {0,   0,   0,   0};

    Clay_BeginLayout();

    CLAY(CLAY_ID("AppContainer"), {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                    .layoutDirection = CLAY_TOP_TO_BOTTOM },
        .backgroundColor = cBg
    }) {

        // Title bar
        CLAY(CLAY_ID("TitleBar"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(30) },
                        .padding = {.left=16, .right=16},
                        .childAlignment = {.x=CLAY_ALIGN_X_LEFT, .y=CLAY_ALIGN_Y_CENTER} },
            .backgroundColor = cTitle
        }) {
            CLAY_TEXT(CLAY_STRING("Dupic"), CLAY_TEXT_CONFIG({ .textColor = cText, .fontSize = 15 }));

            CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(0) } } }) {}

            {
                static char fpsBuf[16];
                snprintf(fpsBuf, sizeof(fpsBuf), "%.0f fps", g_fps);
                Clay_String s = { .length = (int32_t)strlen(fpsBuf), .chars = fpsBuf };
                CLAY_TEXT(s, CLAY_TEXT_CONFIG({ .textColor = {120, 120, 120, 255}, .fontSize = 12 }));
            }

            CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_FIXED(12), CLAY_SIZING_FIXED(0) } } }) {}

            // Minimize
            CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_FIXED(45), CLAY_SIZING_GROW(0) },
                                       .childAlignment = {.x=CLAY_ALIGN_X_CENTER, .y=CLAY_ALIGN_Y_CENTER} },
                           .backgroundColor = Clay_Hovered() ? cHover : cNone }) {
                if (Clay_Hovered() && g_mouseLeftPressed) 
                {
                    #ifdef _WIN32
                    HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(g_window),
                                     SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
                    if (hwnd) ShowWindow(hwnd, SW_MINIMIZE);
                    #else
                    SDL_MinimizeWindow(g_window);
                    #endif
                }
                CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_FIXED(16), CLAY_SIZING_FIXED(16) } },
                               .image = { .imageData = g_minimizeIconTex } }) {}
            }

            // Maximize / Restore
            CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_FIXED(45), CLAY_SIZING_GROW(0) },
                                       .childAlignment = {.x=CLAY_ALIGN_X_CENTER, .y=CLAY_ALIGN_Y_CENTER} },
                           .backgroundColor = Clay_Hovered() ? cHover : cNone }) {
                if (Clay_Hovered() && g_mouseLeftPressed)
                    g_pendingMaximize = true;
                if (SDL_GetWindowFlags(g_window) & SDL_WINDOW_MAXIMIZED)
                    CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_FIXED(16), CLAY_SIZING_FIXED(16) } },
                                   .image = { .imageData = g_maximizeIconTex } }) {}
                else
                    CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_FIXED(16), CLAY_SIZING_FIXED(16) } },
                                   .image = { .imageData = g_maximizeIconTex } }) {}
            }

            // Close
            CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_FIXED(45), CLAY_SIZING_GROW(0) },
                                       .childAlignment = {.x=CLAY_ALIGN_X_CENTER, .y=CLAY_ALIGN_Y_CENTER} },
                           .backgroundColor = Clay_Hovered() ? Clay_Color{196, 43, 28, 255} : cNone }) {
                if (Clay_Hovered() && g_mouseLeftPressed) { g_shouldClose = true; }
                CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_FIXED(16), CLAY_SIZING_FIXED(16) } },
                               .image = { .imageData = g_closeIconTex } }) {}
            }
        }

        // Control bar
        CLAY(CLAY_ID("ControlBar"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(50) },
                        .padding = {.left=10, .right=10}, .childGap = 10,
                        .childAlignment = {.x=CLAY_ALIGN_X_LEFT, .y=CLAY_ALIGN_Y_CENTER} },
            .backgroundColor = cBg,
            .border = { .color = {68, 68, 68, 255}, .width = { .bottom = 4 } }
        }) {
            CLAY(CLAY_ID("SetFolder"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(45), CLAY_SIZING_FIXED(40) },
                             .padding = {.left=5, .right=5, .top=5, .bottom=5} },
                .backgroundColor = Clay_Hovered() ? cHover : cNone,
                .cornerRadius = CLAY_CORNER_RADIUS(4),
                .image = { .imageData = g_folderIconTex }
            }) {
                if (Clay_Hovered() && g_mouseLeftPressed) OpenFolderDialog();
            }

            CLAY(CLAY_ID("PathDisplay"), {
                .layout = { .sizing = { CLAY_SIZING_PERCENT(0.3f), CLAY_SIZING_FIXED(30) },
                             .padding = {.left=5, .right=5},
                             .childAlignment = {.x=CLAY_ALIGN_X_LEFT, .y=CLAY_ALIGN_Y_CENTER} },
                .backgroundColor = cPath,
                .cornerRadius = CLAY_CORNER_RADIUS(2)
            }) {
                Clay_String s = { .length = (int32_t)currentPath.size(), .chars = currentPath.c_str() };
                CLAY_TEXT(s, CLAY_TEXT_CONFIG({ .textColor = cText, .fontSize = 15 }));
            }

            CLAY_TEXT(CLAY_STRING("Strict"), CLAY_TEXT_CONFIG({ .textColor = cTextGray, .fontSize = 14 }));

            CLAY(CLAY_ID("Slider"), {
                .layout = { .sizing = { CLAY_SIZING_PERCENT(0.20f), CLAY_SIZING_FIXED(6) },
                             .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
                .backgroundColor = {211, 211, 211, 180},
                .cornerRadius = CLAY_CORNER_RADIUS(3)
            }) {
                static bool s_sliderDragging = false;
                Clay_BoundingBox sb = Clay_GetElementData(Clay_GetElementId(CLAY_STRING("Slider"))).boundingBox;
                if (sb.width > 0) {
                    float mx, my;
                    SDL_GetMouseState(&mx, &my);
                    if (g_mouseLeftPressed && mx >= sb.x && mx <= sb.x + sb.width &&
                        my >= sb.y - 8.0f && my <= sb.y + sb.height + 8.0f)
                        s_sliderDragging = true;
                    if (!g_mouseLeftDown)
                        s_sliderDragging = false;
                    if (s_sliderDragging) {
                        float t = (mx - sb.x) / sb.width;
                        if (t < 0.0f) t = 0.0f;
                        if (t > 1.0f) t = 1.0f;
                        strictnessSlider = 1.0f + t * 9.0f;
                    }
                }
                float t = (strictnessSlider - 1.0f) / 9.0f;
                float tSpace = (sb.width > 14.0f) ? t * (sb.width - 14.0f) / sb.width : 0.0f;
                CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_PERCENT(tSpace), CLAY_SIZING_GROW(0) } } }) {}
                CLAY(CLAY_ID("SliderThumb"), {
                    .layout = { .sizing = { CLAY_SIZING_FIXED(14), CLAY_SIZING_FIXED(14) } },
                    .backgroundColor = {94, 94, 94, 255},
                    .cornerRadius = CLAY_CORNER_RADIUS(7)
                }) {}
            }

            CLAY_TEXT(CLAY_STRING("Relaxed"), CLAY_TEXT_CONFIG({ .textColor = cTextGray, .fontSize = 14 }));

            CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(0) } } }) {}

            CLAY(CLAY_ID("HelpBtn"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(25), CLAY_SIZING_FIXED(25) },
                             .childAlignment = {.x=CLAY_ALIGN_X_CENTER, .y=CLAY_ALIGN_Y_CENTER} },
                .backgroundColor = Clay_Hovered() ? cHover : cBtn,
                .cornerRadius = CLAY_CORNER_RADIUS(12)
            }) {
                CLAY_TEXT(CLAY_STRING("?"), CLAY_TEXT_CONFIG({ .textColor = cText, .fontSize = 15 }));
            }

            CLAY(CLAY_ID("GoBtn"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(70), CLAY_SIZING_FIXED(30) },
                             .childAlignment = {.x=CLAY_ALIGN_X_CENTER, .y=CLAY_ALIGN_Y_CENTER} },
                .backgroundColor = Clay_Hovered() ? cHover : cBtn,
                .cornerRadius = CLAY_CORNER_RADIUS(2)
            }) {
                if (Clay_Hovered() && g_mouseLeftPressed && !selectedFolders.empty() && !g_scanning) {
                    duplicateGroups.clear();
                    ClearTextureCache();
                    g_scanning = true;
                    StartDuplicateSearch(selectedFolders, strictnessSlider);
                }
                CLAY_TEXT(g_scanning ? CLAY_STRING("...") : CLAY_STRING("Go"),
                    CLAY_TEXT_CONFIG({ .textColor = cText, .fontSize = 15 }));
            }

            CLAY(CLAY_ID("DeleteBtn"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(130), CLAY_SIZING_FIXED(30) },
                             .childAlignment = {.x=CLAY_ALIGN_X_CENTER, .y=CLAY_ALIGN_Y_CENTER} },
                .backgroundColor = Clay_Hovered() ? cHover : cBtn,
                .cornerRadius = CLAY_CORNER_RADIUS(2)
            }) {
                if (Clay_Hovered() && g_mouseLeftPressed && !g_markedPaths.empty())
                    DeleteMarkedFiles();
                CLAY_TEXT(CLAY_STRING("Delete Marked"), CLAY_TEXT_CONFIG({ .textColor = cText, .fontSize = 15 }));
            }
        }

        // Main content
        CLAY(CLAY_ID("SectionsContainer"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                        .layoutDirection = CLAY_LEFT_TO_RIGHT }
        }) {

            // Left panel: duplicate list + scrollbar
            CLAY(CLAY_ID("Section1_Wrap"), {
                .layout = { .sizing = { CLAY_SIZING_PERCENT(0.5f), CLAY_SIZING_GROW(0) },
                             .layoutDirection = CLAY_LEFT_TO_RIGHT },
                .border = { .color = {68, 68, 68, 255}, .width = { .right = 4 } }
            }) {
            CLAY(CLAY_ID("Section1_ScrollTable"), {
                .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                             .layoutDirection = CLAY_TOP_TO_BOTTOM },
                .clip = { .horizontal = true, .vertical = true, .childOffset = Clay_GetScrollOffset() }
            }) {
                if (g_scanning) {
                    CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                                               .childAlignment = {.x=CLAY_ALIGN_X_CENTER, .y=CLAY_ALIGN_Y_CENTER} } }) {
                        CLAY_TEXT(CLAY_STRING("Scanning..."), CLAY_TEXT_CONFIG({ .textColor = {160, 160, 160, 255}, .fontSize = 20 }));
                    }
                } else if (duplicateGroups.empty()) {
                    CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                                               .childAlignment = {.x=CLAY_ALIGN_X_CENTER, .y=CLAY_ALIGN_Y_CENTER} } }) {
                        CLAY_TEXT(CLAY_STRING("No duplicates found"), CLAY_TEXT_CONFIG({ .textColor = cText, .fontSize = 20 }));
                    }
                } else {
                    if (g_thumbTextures.size() != duplicateGroups.size()) RebuildThumbSlots();
                    for (size_t gi = 0; gi < duplicateGroups.size(); gi++) {
                        // Group separator
                        CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(2) } },
                                       .backgroundColor = {68, 68, 68, 255} }) {}
                        CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                                                   .layoutDirection = CLAY_LEFT_TO_RIGHT } }) {
                            for (size_t ji = 0; ji < duplicateGroups[gi].size(); ji++) {
                                const auto& imgPath = duplicateGroups[gi][ji];
                                SDL_Texture*& thumb = g_thumbTextures[gi][ji];
                                if (!thumb) thumb = LoadThumb(imgPath);
                                float tw = 150, th = 150;
                                if (thumb) {
                                    float fw, fh;
                                    SDL_GetTextureSize(thumb, &fw, &fh);
                                    tw = fw;
                                    th = fh;
                                }
                                bool selected  = (imgPath == selectedImagePath);
                                bool isMarked  = g_markedPaths.count(imgPath) > 0;
                                CLAY(CLAY_IDI("Thumb", (int)(gi * 1024 + ji)), {
                                    .layout = { .sizing = { CLAY_SIZING_FIXED(tw + 10), CLAY_SIZING_FIXED(th + 10) },
                                                .padding = {.left=5, .right=5, .top=5, .bottom=5} },
                                    .backgroundColor = isMarked  ? Clay_Color{80, 20, 20, 255}
                                                     : selected  ? Clay_Color{80,120,180,255}
                                                     : (Clay_Hovered() ? cHover : cNone)
                                }) {
                                    if (Clay_Hovered() && g_mouseLeftPressed)
                                        OnImageClicked((int)gi, (int)ji);
                                    if (Clay_Hovered() && g_mouseRightPressed) {
                                        if (isMarked) g_markedPaths.erase(imgPath);
                                        else          g_markedPaths.insert(imgPath);
                                    }
                                    if (thumb)
                                        CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_FIXED(tw), CLAY_SIZING_FIXED(th) } },
                                                       .image = { .imageData = thumb } }) {}
                                    else
                                        CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_FIXED(tw), CLAY_SIZING_FIXED(th) } },
                                                       .backgroundColor = {60, 60, 60, 255} }) {}
                                    if (isMarked) {
                                        CLAY(CLAY_IDI("ThumbMark", (int)(gi * 1024 + ji)), {
                                            .layout = { .sizing = { CLAY_SIZING_FIXED(tw + 10), CLAY_SIZING_FIXED(th + 10) },
                                                        .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } },
                                            .backgroundColor = {0, 0, 0, 160},
                                            .floating = { .offset = { 0.0f, 0.0f },
                                                          .attachPoints = { .element = CLAY_ATTACH_POINT_LEFT_TOP,
                                                                            .parent  = CLAY_ATTACH_POINT_LEFT_TOP },
                                                          .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH,
                                                          .attachTo = CLAY_ATTACH_TO_PARENT,
                                                          .clipTo = CLAY_CLIP_TO_ATTACHED_PARENT }
                                        }) {
                                            CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_FIXED(40), CLAY_SIZING_FIXED(40) } },
                                                           .image   = { .imageData = g_markOverlayTex } }) {}
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            // Scrollbar
            {
                Clay_ScrollContainerData sd = Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("Section1_ScrollTable")));
                float trackH   = sd.found ? sd.scrollContainerDimensions.height : 0.0f;
                float contentH = sd.found ? sd.contentDimensions.height : 0.0f;
                bool  scroll   = sd.found && contentH > trackH + 1.0f;
                float thumbH   = scroll ? (trackH * trackH / contentH) : trackH;
                if (thumbH < 20.0f) thumbH = 20.0f;
                float scrollY  = (scroll && sd.scrollPosition) ? -sd.scrollPosition->y : 0.0f;
                float maxScroll = contentH - trackH;
                float ratio    = (scroll && maxScroll > 0.0f) ? scrollY / maxScroll : 0.0f;
                float thumbTop = scroll ? ratio * (trackH - thumbH) : 0.0f;
                if (thumbTop < 0.0f) thumbTop = 0.0f;
                if (thumbTop + thumbH > trackH) thumbTop = trackH - thumbH;
                CLAY(CLAY_ID("ScrollTrack"), {
                    .layout = { .sizing = { CLAY_SIZING_FIXED(8), CLAY_SIZING_GROW(0) },
                                .layoutDirection = CLAY_TOP_TO_BOTTOM },
                    .backgroundColor = scroll ? Clay_Color{35,35,35,255} : Clay_Color{0,0,0,0}
                }) {
                    if (scroll) {
                        CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(thumbTop) } } }) {}
                        CLAY(CLAY_ID("ScrollThumb"), {
                            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(thumbH) } },
                            .backgroundColor = {110, 110, 110, 220},
                            .cornerRadius = CLAY_CORNER_RADIUS(4)
                        }) {}
                    }
                }
            }
            } // Section1_Wrap

            // Right panel: preview + actions
            CLAY(CLAY_ID("Section2"), {
                .layout = { .sizing = { CLAY_SIZING_PERCENT(0.5f), CLAY_SIZING_GROW(0) },
                            .layoutDirection = CLAY_TOP_TO_BOTTOM }
            }) {
                CLAY(CLAY_ID("ImageContainer"), {
                    .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                                .childAlignment = {.x=CLAY_ALIGN_X_CENTER, .y=CLAY_ALIGN_Y_CENTER} }
                }) {
                    if (selectedImagePath.empty()) {
                        CLAY_TEXT(CLAY_STRING("Select an image"), CLAY_TEXT_CONFIG({ .textColor = cText, .fontSize = 20 }));
                    } else {
                        SDL_Texture* prev = (g_selGi >= 0 && g_selJi >= 0) ? g_previewTextures[g_selGi][g_selJi] : nullptr;
                        if (prev) {
                            float pw, ph;
                            SDL_GetTextureSize(prev, &pw, &ph);
                            Clay_BoundingBox box = Clay_GetElementData(Clay_GetElementId(CLAY_STRING("ImageContainer"))).boundingBox;
                            if (box.width <= 1.0f || box.height <= 1.0f) {
                                int ww, wh;
                                SDL_GetWindowSize(g_window, &ww, &wh);
                                box.width  = ww * 0.5f;
                                box.height = (float)wh - 100.0f;
                            }
                            float sx = (box.width  - 10.0f) / pw;
                            float sy = (box.height - 10.0f) / ph;
                            float sc = sx < sy ? sx : sy;
                            if (sc > 1.0f) sc = 1.0f;  // never upscale; native size if it fits
                            CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_FIXED(pw * sc), CLAY_SIZING_FIXED(ph * sc) } },
                                           .image = { .imageData = prev } }) {}
                        } else {
                            CLAY_TEXT(CLAY_STRING("Loading..."), CLAY_TEXT_CONFIG({ .textColor = cText, .fontSize = 20 }));
                        }
                    }
                }

                CLAY(CLAY_ID("ActionButtons"), {
                    .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(40) },
                                .padding = {.left=5, .right=5, .top=5, .bottom=5}, .childGap = 10 },
                    .backgroundColor = {56, 56, 56, 255}
                }) {
                    {
                        bool selMarked = !selectedImagePath.empty() && g_markedPaths.count(selectedImagePath) > 0;
                        CLAY(CLAY_ID("MarkBtn"), {
                            .layout = { .sizing = { CLAY_SIZING_PERCENT(0.5f), CLAY_SIZING_GROW(0) },
                                         .childAlignment = {.x=CLAY_ALIGN_X_CENTER, .y=CLAY_ALIGN_Y_CENTER} },
                            .backgroundColor = Clay_Hovered() ? cHover : cBtn,
                            .cornerRadius = CLAY_CORNER_RADIUS(2)
                        }) {
                            if (Clay_Hovered() && g_mouseLeftPressed && !selectedImagePath.empty()) {
                                if (selMarked) g_markedPaths.erase(selectedImagePath);
                                else           g_markedPaths.insert(selectedImagePath);
                            }
                            CLAY_TEXT(selMarked ? CLAY_STRING("Unmark") : CLAY_STRING("Mark for deletion"),
                                CLAY_TEXT_CONFIG({ .textColor = cText, .fontSize = 14 }));
                        }
                    }

                    CLAY(CLAY_ID("LocateBtn"), {
                        .layout = { .sizing = { CLAY_SIZING_PERCENT(0.5f), CLAY_SIZING_GROW(0) },
                                     .childAlignment = {.x=CLAY_ALIGN_X_CENTER, .y=CLAY_ALIGN_Y_CENTER} },
                        .backgroundColor = Clay_Hovered() ? cHover : cBtn,
                        .cornerRadius = CLAY_CORNER_RADIUS(2)
                    }) {
                        CLAY_TEXT(CLAY_STRING("Locate on Disk"), CLAY_TEXT_CONFIG({ .textColor = cText, .fontSize = 14 }));
                    }
                }

                CLAY(CLAY_ID("ImageProperties"), {
                    .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(30) },
                                .padding = {.left=10, .right=10},
                                .childAlignment = {.x=CLAY_ALIGN_X_LEFT, .y=CLAY_ALIGN_Y_CENTER} },
                    .backgroundColor = cProps
                }) {
                    Clay_String sPath = { .length = (int32_t)selectedImagePath.size(), .chars = selectedImagePath.c_str() };
                    Clay_String sRes  = { .length = (int32_t)selectedImageRes.size(),  .chars = selectedImageRes.c_str()  };
                    Clay_String sSize = { .length = (int32_t)selectedImageSize.size(), .chars = selectedImageSize.c_str() };
                    CLAY_TEXT(sPath, CLAY_TEXT_CONFIG({ .textColor = cTextDim, .fontSize = 14 }));
                    CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(0) } } }) {}
                    CLAY_TEXT(sRes,  CLAY_TEXT_CONFIG({ .textColor = cTextDim, .fontSize = 14 }));
                    CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_FIXED(20), CLAY_SIZING_FIXED(0) } } }) {}
                    CLAY_TEXT(sSize, CLAY_TEXT_CONFIG({ .textColor = cTextDim, .fontSize = 14 }));
                }
            }
        }
    }
}
