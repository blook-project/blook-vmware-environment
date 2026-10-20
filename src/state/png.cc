// blook-vmware-environment: host side image decoding (WIC) and pixel statistics.
#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "blook/bvmenv/classify.h"
#include "blook/bvmenv/process.h"
#include "blook/bvmenv/util.h"

namespace blook::bvmenv {
namespace {

template <class T>
class ComPtr {
   public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    T* get() const { return ptr_; }
    T** put() {
        reset();
        return &ptr_;
    }
    T* operator->() const { return ptr_; }
    void reset() {
        if (ptr_) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

   private:
    T* ptr_ = nullptr;
};

struct CoInit {
    bool ok = false;
    CoInit() { ok = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)); }
    ~CoInit() {
        if (ok)
            CoUninitialize();
    }
};

bool decode_png(const fs::path& path, UINT* width, UINT* height, std::vector<std::uint8_t>* bgr) {
    CoInit com;
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(factory.put())))) {
        return false;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, decoder.put()))) {
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.put())))
        return false;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(converter.put())))
        return false;
    if (FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat24bppBGR,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        return false;
    }
    UINT w = 0, h = 0;
    converter->GetSize(&w, &h);
    if (w == 0 || h == 0 || w > 16384 || h > 16384)
        return false;
    const UINT stride = w * 3;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(stride) * h);
    if (FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()),
                                     pixels.data()))) {
        return false;
    }
    *width = w;
    *height = h;
    *bgr = std::move(pixels);
    return true;
}

std::uint64_t fnv1a(std::uint64_t hash, std::uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ULL;
    return hash;
}

}  // namespace

std::optional<ScreenStats> analyze_image(const fs::path& png, std::string* error) {
    UINT width = 0, height = 0;
    std::vector<std::uint8_t> pixels;
    if (!decode_png(png, &width, &height, &pixels)) {
        if (error)
            *error = "failed to decode image " + to_utf8(png.wstring());
        return std::nullopt;
    }
    ScreenStats stats;
    stats.width = static_cast<int>(width);
    stats.height = static_cast<int>(height);

    // Sample on a fixed grid: plenty for classification and fast even for 4K.
    const int step_x = std::max(1, static_cast<int>(width) / 160);
    const int step_y = std::max(1, static_cast<int>(height) / 100);

    std::uint64_t hash = 1469598103934665603ULL;
    long long count = 0, black = 0, white = 0, blue = 0, gray = 0, colored = 0, dark = 0;
    double sum_r = 0, sum_g = 0, sum_b = 0;

    for (int y = 0; y < static_cast<int>(height); y += step_y) {
        const std::uint8_t* row = pixels.data() + static_cast<std::size_t>(y) * width * 3;
        for (int x = 0; x < static_cast<int>(width); x += step_x) {
            const std::uint8_t b = row[x * 3 + 0];
            const std::uint8_t g = row[x * 3 + 1];
            const std::uint8_t r = row[x * 3 + 2];
            ++count;
            sum_r += r;
            sum_g += g;
            sum_b += b;
            const int max_c = std::max({r, g, b});
            const int min_c = std::min({r, g, b});
            const int luma = (r + g + b) / 3;
            if (max_c < 24)
                ++black;
            if (min_c > 220)
                ++white;
            if (luma < 64)
                ++dark;
            if (b > 90 && b > r + 30 && b > g + 20) {
                ++blue;
            } else if (max_c - min_c < 20) {
                ++gray;
            } else if (max_c - min_c >= 40) {
                ++colored;
            }
            // Coarse fingerprint on an 8-bit luma thumbnail grid.
            if ((x % (step_x * 4) == 0) && (y % (step_y * 4) == 0)) {
                hash = fnv1a(hash, static_cast<std::uint64_t>(luma >> 3));
            }
        }
    }
    if (count == 0) {
        if (error)
            *error = "empty image";
        return std::nullopt;
    }
    const double n = static_cast<double>(count);
    stats.black_ratio = static_cast<double>(black) / n;
    stats.white_ratio = static_cast<double>(white) / n;
    stats.blue_ratio = static_cast<double>(blue) / n;
    stats.gray_ratio = static_cast<double>(gray) / n;
    stats.colored_ratio = static_cast<double>(colored) / n;
    stats.dark_ratio = static_cast<double>(dark) / n;
    stats.mean_r = sum_r / n;
    stats.mean_g = sum_g / n;
    stats.mean_b = sum_b / n;
    stats.fingerprint = hash;
    stats.sampled_pixels = static_cast<int>(count);
    return stats;
}

}  // namespace blook::bvmenv
