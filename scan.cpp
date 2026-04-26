#include <string>
#include <vector>
#include <array>
#include <thread>
#include <atomic>
#include <filesystem>
#include <algorithm>
#include <numeric>
#include <unordered_map>
#include <cmath>
#include <bit>

#include <SDL3/SDL.h>
#include <vips/vips.h>

#include "scan.h"

// Hint to the compiler that a loop is independent and should be vectorized
#if defined(_MSC_VER)
    // MSVC style
    #define PRAGMA_IVDEP __pragma(loop(hint_parallel(0)))
#elif defined(__GNUC__) || defined(__clang__)
    // GCC and Clang style
    #define PRAGMA_IVDEP _Pragma("GCC ivdep")
#else
    // Fallback for other compilers
    #define PRAGMA_IVDEP
#endif

namespace fs = std::filesystem;

extern Uint32 g_scanDoneEvent;

static constexpr int BITS = 16;  // 16x16 blocks = 256-bit hash
using Blocks = std::array<float, BITS * BITS>;
struct alignas(32) BlockHash {
    uint64_t data[4];
};

// ── File gathering ──────────────────────────────────────────────────────────

static bool IsImageFile(const fs::path& p)
{
    if (!p.has_extension()) return false;
    std::string ext = p.extension().string();
    if (ext.size() < 4 || ext.size() > 5) return false; // Early discard
    for (char& c : ext) c = (char)::tolower((unsigned char)c);
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png"
        || ext == ".webp" || ext == ".tif"  || ext == ".tiff";
}

static std::vector<std::string> GatherFiles(const std::vector<std::string>& folders)
{
    std::vector<std::string> files;
    std::vector<fs::path> stack(folders.begin(), folders.end());
    while (!stack.empty()) 
    {
        fs::path dir = std::move(stack.back()); stack.pop_back();
        std::error_code ec;
        fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
        for (; !ec && it != fs::directory_iterator(); it.increment(ec))
        {
            try {
                if (it->is_directory())       stack.emplace_back(it->path());
                else if (it->is_regular_file() && IsImageFile(it->path()))
                    files.emplace_back(it->path().string());
            } catch (...) {}
        }
    }
    return files;
}

// ── Blockhash algorithm ─────────────────────────────────────────────────────

// Per-quadrant median threshold → pack 256 bits into 4×uint64.
// Uses lower+upper-middle average for even-length arrays.
static BlockHash FinalizeHash(Blocks& b)
{
    constexpr int Q = BITS * BITS / 4;  // 64, always even
    std::array<float, Q> tmp;
    BlockHash hash = {};
    for (int q = 0; q < 4; q++) 
    {
        std::copy_n(b.data() + q * Q, Q, tmp.begin());
        std::nth_element(tmp.begin(), tmp.begin() + Q / 2, tmp.end());
        float med2 = tmp[Q / 2];
        float med1 = *std::max_element(tmp.begin(), tmp.begin() + Q / 2);
        float med = (med1 + med2) / 2.0f;
        for (int i = 0; i < Q; i++) 
        {
            if (b[q * Q + i] >= med) hash.data[q] |= 1ULL << i;
        }
    }
    return hash;
}

// bmvbhash_even: image dims exactly divisible by BITS
static BlockHash BmvbHashEven(const uint8_t* data, int w, int h, int ch)
{
    const int bx = w / BITS, by = h / BITS;
    Blocks result = {};
    for (int y = 0; y < h; y++) 
    {
        int y_offset = (y / by) * BITS;
        const uint8_t* row_data = data + (y * w * ch);
        for (int x = 0; x < w; x++) 
        {
            int i = x * ch;
            float v = (ch == 4 && row_data[i + 3] == 0) ? 765.0f
                    : (float)(row_data[i] + row_data[i + 1] + row_data[i + 2]);
            result[y_offset + (x / bx)] += v;
        }
    }
    return FinalizeHash(result);
}

// bmvbhash: general case with sub-pixel weighted block accumulation
static BlockHash BmvbHash(const uint8_t* data, int w, int h, int ch)
{
    if (w % BITS == 0 && h % BITS == 0) return BmvbHashEven(data, w, h, ch);

    const float bw = (float)w / BITS, bh = (float)h / BITS;
    
    struct Weight { int i1, i2; float w1, w2; };
    // Image size is always capped by vips_thumbnail in ComputeBlockHash
    Weight wx[256], wy[256];

    for (int x = 0; x < w; x++) 
    {
        float xm = fmodf((float)(x + 1), bw);
        float xf = xm - floorf(xm);
        int bl = (int)((float)x / bw);
        int br = (xm - xf > 0.0f || (x + 1) == w) ? bl : (int)ceilf((float)x / bw);
        wx[x] = {bl, br, 1.0f - xf, xf};
    }
    for (int y = 0; y < h; y++) 
    {
        float ym = fmodf((float)(y + 1), bh);
        float yf = ym - floorf(ym);
        int bt = (int)((float)y / bh);
        int bb = (ym - yf > 0.0f || (y + 1) == h) ? bt : (int)ceilf((float)y / bh);
        wy[y] = {bt, bb, 1.0f - yf, yf};
    }

    Blocks blocks = {};
    for (int y = 0; y < h; y++) 
    {
        const auto& Y = wy[y];
        const uint8_t* row_data = data + (y * w * ch);
        for (int x = 0; x < w; x++) 
        {
            const auto& X = wx[x];
            int i = x * ch;
            float v = (ch == 4 && row_data[i + 3] == 0) ? 765.0f : (float)(row_data[i] + row_data[i + 1] + row_data[i + 2]);
            
            // Unrolled accumulation
            if (Y.i1 < BITS && X.i1 < BITS) blocks[Y.i1 * BITS + X.i1] += v * Y.w1 * X.w1;
            if (Y.i1 < BITS && X.i2 < BITS) blocks[Y.i1 * BITS + X.i2] += v * Y.w1 * X.w2;
            if (Y.i2 < BITS && X.i1 < BITS) blocks[Y.i2 * BITS + X.i1] += v * Y.w2 * X.w1;
            if (Y.i2 < BITS && X.i2 < BITS) blocks[Y.i2 * BITS + X.i2] += v * Y.w2 * X.w2;
        }
    }
    return FinalizeHash(blocks);
}

// Load image with libvips C API and compute blockhash
static BlockHash ComputeBlockHash(const std::string& path)
{
    VipsImage* img = nullptr;

    if (vips_thumbnail(path.c_str(), &img, 256, "height", 256, "no_rotate", TRUE, nullptr) != 0)
        return {};

    // strip Alpha if it exists
    if (vips_image_get_bands(img) > 3) 
    {
        VipsImage* tmp = nullptr;
        if (vips_extract_band(img, &tmp, 0, "n", 3, nullptr) != 0) 
        {
            g_object_unref(img);
            return {};
        }
        g_object_unref(img);
        img = tmp;
    }

    size_t sz;
    void* mem = vips_image_write_to_memory(img, &sz); // vips only actually works on the image here
    int w = vips_image_get_width(img);
    int h = vips_image_get_height(img);
    int bands = vips_image_get_bands(img);
    g_object_unref(img);

    if (!mem) return {};

    BlockHash hash = BmvbHash(static_cast<const uint8_t*>(mem), w, h, bands);
    g_free(mem);
    return hash;
}

static bool IsZeroHash(const BlockHash& h)
{
    return (h.data[0] | h.data[1] | h.data[2] | h.data[3]) == 0;
}

// ── Hash cache ──────────────────────────────────────────────────────────────
// Persists between Go clicks so re-running with a different threshold skips hashing.
static std::vector<std::string> s_cachedFiles;
static std::vector<BlockHash>   s_cachedHashes;

void InvalidateScanCache()
{
    s_cachedFiles.clear();
    s_cachedHashes.clear();
}

// ── Background search thread ────────────────────────────────────────────────
static void SearchThread(std::vector<std::string> folders, float threshold)
{
    // slider 1 (strict) -> maxDist 4, slider 10 (relaxed) -> maxDist 40
    const int maxDist = (int)(threshold * 4.0f);

    if (s_cachedFiles.empty()) {
        // Fresh hash run 
        std::vector<std::string> files = GatherFiles(folders);
        const size_t n = files.size();

        const int nworkers = std::max(1, (int)std::thread::hardware_concurrency() - 1) - 1;
        vips_concurrency_set(1);
        vips_cache_set_max(0);

        std::vector<BlockHash> hashes(n);
        std::atomic<size_t> next(0);

        auto work = [&] {
            const size_t batch_size = 16;
            while (true) {
                size_t start_idx = next.fetch_add(batch_size);
                if (start_idx >= n) break;
                size_t end_idx = std::min(start_idx + batch_size, n);
                for (size_t i = start_idx; i < end_idx; ++i)
                    hashes[i] = ComputeBlockHash(files[i]);
            }
            vips_thread_shutdown();
        };

        std::vector<std::thread> workers;
        workers.reserve(nworkers);
        for (int i = 0; i < nworkers; i++) { workers.emplace_back(work); }
        work();
        for (auto& t : workers) { t.join(); }

        // Move into cache — no copies, local vectors are now empty
        s_cachedFiles  = std::move(files);
        s_cachedHashes = std::move(hashes);
    }

    // ── Union-Find (always runs, uses cached data directly) ─────────────────
    const size_t n = s_cachedFiles.size();
    std::vector<int> parent(n);
    std::iota(parent.begin(), parent.end(), 0);
    auto find = [&](int x) {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    };

    for (size_t i = 0; i < n; i++)
    {
        const BlockHash& hi = s_cachedHashes[i];
        PRAGMA_IVDEP
        for (size_t j = i + 1; j < n; j++) {
            const BlockHash& hj = s_cachedHashes[j];

            int hamming = std::popcount(hi.data[0] ^ hj.data[0]) +
                          std::popcount(hi.data[1] ^ hj.data[1]) +
                          std::popcount(hi.data[2] ^ hj.data[2]) +
                          std::popcount(hi.data[3] ^ hj.data[3]);

            if (hamming <= maxDist) {
                int rootI = find((int)i);
                int rootJ = find((int)j);
                if (rootI != rootJ) parent[rootI] = rootJ;
            }
        }
    }

    std::unordered_map<int, std::vector<std::string>> buckets;
    buckets.reserve(n);
    for (size_t i = 0; i < n; i++)
        buckets[find((int)i)].push_back(s_cachedFiles[i]);  // copy: cache stays intact

    auto* groups = new std::vector<std::vector<std::string>>();
    for (auto& [root, members] : buckets)
        if (members.size() > 1 && !IsZeroHash(s_cachedHashes[(size_t)root]))
            groups->push_back(std::move(members));

    SDL_Event ev = {};
    ev.type       = g_scanDoneEvent;
    ev.user.data1 = groups;
    SDL_PushEvent(&ev);
}

void StartDuplicateSearch(const std::vector<std::string>& folders, float threshold)
{
    std::thread(SearchThread, folders, threshold).detach();
}