/*
Cross-platform (Windows / macOS / Linux) multithreaded HEIC -> PDF converter.

Each .heic photo becomes a one-page PDF. The decoded image is JPEG-compressed
(the quality option controls that compression) and embedded directly into the
PDF using the DCTDecode filter, so libjpeg is still required.

Build with the included Makefile:
    make heic_converter_mt

Or by hand:

macOS (Homebrew: brew install libheif jpeg):
    g++ -std=c++17 -O2 -pthread -o heic_converter_mt heic_converter_mt.cpp \
      -I/opt/homebrew/include -L/opt/homebrew/lib \
      -I/opt/homebrew/opt/jpeg/include -L/opt/homebrew/opt/jpeg/lib \
      -lheif -ljpeg

Windows (MSYS2 MinGW-w64 shell, after
         pacman -S mingw-w64-ucrt-x86_64-libheif mingw-w64-ucrt-x86_64-libjpeg-turbo):
    g++ -std=c++17 -O2 -pthread -o heic_converter_mt.exe heic_converter_mt.cpp -lheif -ljpeg

Run examples:
  ./heic_converter_mt                          # convert .heic files in the current directory -> ./output (PDFs)
  ./heic_converter_mt ~/Pictures -q 85 -t 8    # explicit input dir, quality, threads
  ./heic_converter_mt --help
*/

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cctype>
#include <cerrno>
#include <string>
#include <vector>
#include <queue>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <iostream>

#include <sys/stat.h>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <direct.h>
#else
    #include <dirent.h>
    #include <unistd.h>
#endif

#include <libheif/heif.h>
#include <jpeglib.h>

// MSVC's <sys/stat.h> does not provide these POSIX macros
#if defined(_WIN32) && !defined(S_ISDIR)
    #define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#if defined(_WIN32) && !defined(S_ISREG)
    #define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif

// windows.h defines MAX_PATH (260), so use our own name for path buffers
#define PATH_BUF_SIZE 1024

typedef struct {
    struct heif_context* ctx;
    struct heif_image_handle* handle;
    struct heif_image* img;
    FILE* outfile;
    unsigned char* jpeg_buf;   // in-memory JPEG, embedded into the PDF
    unsigned long jpeg_size;
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
} ConversionContext;

static inline bool ends_with_heic_case_insensitive(const char* filename) {
    if (!filename) return false;
    const char* dot = strrchr(filename, '.');
    if (!dot) return false;

    // Compare ".heic" case-insensitively without relying on strcasecmp portability
    const char* ext = dot;
    const char* target = ".heic";
    while (*ext && *target) {
        if (std::tolower((unsigned char)*ext) != std::tolower((unsigned char)*target)) return false;
        ++ext; ++target;
    }
    return (*ext == '\0' && *target == '\0');
}

// Last path component, handling both '/' and '\' separators
static const char* path_basename(const char* path) {
    const char* slash = std::strrchr(path, '/');
#ifdef _WIN32
    const char* bslash = std::strrchr(path, '\\');
    if (!slash || (bslash && bslash > slash)) slash = bslash;
#endif
    return slash ? slash + 1 : path;
}

static inline bool ensure_directory(const char* path) {
    struct stat st;
    if (stat(path, &st) == 0) return S_ISDIR(st.st_mode);
#ifdef _WIN32
    return (_mkdir(path) == 0);
#else
    return (mkdir(path, 0755) == 0);
#endif
}

static void cleanup_context(ConversionContext* ctx) {
    if (!ctx) return;

    if (ctx->outfile) {
        fclose(ctx->outfile);
        ctx->outfile = nullptr;
    }

    if (ctx->img) {
        heif_image_release(ctx->img);
        ctx->img = nullptr;
    }

    if (ctx->handle) {
        heif_image_handle_release(ctx->handle);
        ctx->handle = nullptr;
    }

    if (ctx->ctx) {
        heif_context_free(ctx->ctx);
        ctx->ctx = nullptr;
    }

    jpeg_destroy_compress(&ctx->cinfo);

    // jpeg_mem_dest allocates this buffer for us; it is ours to free.
    if (ctx->jpeg_buf) {
        free(ctx->jpeg_buf);
        ctx->jpeg_buf = nullptr;
    }
}

// Wrap a JPEG-compressed RGB image in a minimal one-page PDF. The JPEG bytes
// are embedded verbatim via the DCTDecode filter (no re-encoding). The page is
// sized to the image so one pixel maps to one PDF point. Returns true on success.
static bool write_jpeg_as_pdf(FILE* out, const unsigned char* jpeg,
                              unsigned long jpeg_len, int width, int height) {
    long offsets[6] = {0};

    if (std::fprintf(out, "%%PDF-1.4\n") < 0) return false;

    offsets[1] = std::ftell(out);
    std::fprintf(out, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");

    offsets[2] = std::ftell(out);
    std::fprintf(out, "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n");

    offsets[3] = std::ftell(out);
    std::fprintf(out, "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] "
                      "/Resources << /XObject << /Im0 4 0 R >> >> /Contents 5 0 R >>\nendobj\n",
                 width, height);

    offsets[4] = std::ftell(out);
    std::fprintf(out, "4 0 obj\n<< /Type /XObject /Subtype /Image /Width %d /Height %d "
                      "/ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode "
                      "/Length %lu >>\nstream\n",
                 width, height, jpeg_len);
    if (std::fwrite(jpeg, 1, jpeg_len, out) != jpeg_len) return false;
    std::fprintf(out, "\nendstream\nendobj\n");

    // Content stream: draw the image scaled to fill the page.
    char content[128];
    int clen = std::snprintf(content, sizeof(content),
                             "q\n%d 0 0 %d 0 0 cm\n/Im0 Do\nQ\n", width, height);
    offsets[5] = std::ftell(out);
    std::fprintf(out, "5 0 obj\n<< /Length %d >>\nstream\n", clen);
    std::fwrite(content, 1, (size_t)clen, out);
    std::fprintf(out, "endstream\nendobj\n");

    long xref = std::ftell(out);
    std::fprintf(out, "xref\n0 6\n0000000000 65535 f \n");
    for (int i = 1; i <= 5; i++) {
        std::fprintf(out, "%010ld 00000 n \n", offsets[i]);
    }
    std::fprintf(out, "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n%ld\n%%%%EOF\n", xref);

    return (std::ferror(out) == 0);
}

static bool convert_heic_to_pdf(const char* input_path, const char* output_dir, int jpeg_quality) {
    ConversionContext ctx{};
    struct heif_error err{};

    // Init JPEG error handler
    ctx.cinfo.err = jpeg_std_error(&ctx.jerr);
    jpeg_create_compress(&ctx.cinfo);

    // Ensure cleanup no matter how we exit
    struct Guard {
        ConversionContext* c;
        ~Guard() { cleanup_context(c); }
    } guard{&ctx};

    // Init HEIF context
    ctx.ctx = heif_context_alloc();
    if (!ctx.ctx) {
        std::fprintf(stderr, "Failed to allocate HEIF context\n");
        return false;
    }

    // Read HEIC file
    err = heif_context_read_from_file(ctx.ctx, input_path, nullptr);
    if (err.code != heif_error_Ok) {
        std::fprintf(stderr, "Could not read HEIC file: %s\n", input_path);
        return false;
    }

    // Get primary image handle
    err = heif_context_get_primary_image_handle(ctx.ctx, &ctx.handle);
    if (err.code != heif_error_Ok) {
        std::fprintf(stderr, "Could not get primary image handle: %s\n", input_path);
        return false;
    }

    // Decode
    err = heif_decode_image(ctx.handle, &ctx.img,
                            heif_colorspace_RGB,
                            heif_chroma_interleaved_RGB,
                            nullptr);
    if (err.code != heif_error_Ok) {
        std::fprintf(stderr, "Could not decode image: %s\n", input_path);
        return false;
    }

    int stride = 0;
    const uint8_t* data = heif_image_get_plane_readonly(ctx.img, heif_channel_interleaved, &stride);
    if (!data || stride <= 0) {
        std::fprintf(stderr, "Could not get decoded plane: %s\n", input_path);
        return false;
    }

    const int width  = heif_image_get_width(ctx.img, heif_channel_interleaved);
    const int height = heif_image_get_height(ctx.img, heif_channel_interleaved);
    if (width <= 0 || height <= 0) {
        std::fprintf(stderr, "Invalid image dimensions: %s\n", input_path);
        return false;
    }

    // Build output filename: output_dir/base_name.pdf
    const char* base = path_basename(input_path);

    std::string base_name(base);
    auto dotpos = base_name.find_last_of('.');
    if (dotpos != std::string::npos) base_name.resize(dotpos);

    char output_path[PATH_BUF_SIZE];
    std::snprintf(output_path, sizeof(output_path), "%s/%s.pdf", output_dir, base_name.c_str());

    // Compress the image to JPEG in memory, then embed it in a PDF.
    jpeg_mem_dest(&ctx.cinfo, &ctx.jpeg_buf, &ctx.jpeg_size);
    ctx.cinfo.image_width = width;
    ctx.cinfo.image_height = height;
    ctx.cinfo.input_components = 3;
    ctx.cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&ctx.cinfo);
    jpeg_set_quality(&ctx.cinfo, jpeg_quality, TRUE);
    jpeg_start_compress(&ctx.cinfo, TRUE);

    // Write scanlines
    while (ctx.cinfo.next_scanline < ctx.cinfo.image_height) {
        JSAMPROW row_pointer = (JSAMPROW)&data[ctx.cinfo.next_scanline * stride];
        jpeg_write_scanlines(&ctx.cinfo, &row_pointer, 1);
    }

    jpeg_finish_compress(&ctx.cinfo);

    // Open output file and wrap the JPEG in a one-page PDF.
    ctx.outfile = std::fopen(output_path, "wb");
    if (!ctx.outfile) {
        std::fprintf(stderr, "Could not create output file: %s\n", output_path);
        return false;
    }

    if (!write_jpeg_as_pdf(ctx.outfile, ctx.jpeg_buf, ctx.jpeg_size, width, height)) {
        std::fprintf(stderr, "Could not write PDF: %s\n", output_path);
        return false;
    }

    return true;
}

/* ---------------- Thread pool (simple work queue) ---------------- */

class WorkQueue {
public:
    void push(std::string item) {
        {
            std::lock_guard<std::mutex> lk(m_);
            q_.push(std::move(item));
        }
        cv_.notify_one();
    }

    // Returns false when no more work and closed
    bool pop(std::string& out) {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return closed_ || !q_.empty(); });
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop();
        return true;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lk(m_);
            closed_ = true;
        }
        cv_.notify_all();
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::queue<std::string> q_;
    bool closed_ = false;
};

static std::vector<std::string> list_heic_files(const char* input_dir) {
    std::vector<std::string> files;

#ifdef _WIN32
    char pattern[PATH_BUF_SIZE];
    std::snprintf(pattern, sizeof(pattern), "%s\\*", input_dir);

    WIN32_FIND_DATAA find_data;
    HANDLE find = FindFirstFileA(pattern, &find_data);
    if (find == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "Could not open input directory '%s' (error %lu)\n",
                     input_dir, GetLastError());
        return files;
    }

    do {
        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!ends_with_heic_case_insensitive(find_data.cFileName)) continue;

        char fullpath[PATH_BUF_SIZE];
        std::snprintf(fullpath, sizeof(fullpath), "%s/%s", input_dir, find_data.cFileName);
        files.emplace_back(fullpath);
    } while (FindNextFileA(find, &find_data));

    FindClose(find);
#else
    DIR* dir = opendir(input_dir);
    if (!dir) {
        std::fprintf(stderr, "Could not open input directory '%s': %s\n", input_dir, std::strerror(errno));
        return files;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (!ends_with_heic_case_insensitive(entry->d_name)) continue;

        char fullpath[PATH_BUF_SIZE];
        std::snprintf(fullpath, sizeof(fullpath), "%s/%s", input_dir, entry->d_name);

        // Skip non-regular files
        struct stat st;
        if (stat(fullpath, &st) != 0) continue;
        if (!S_ISREG(st.st_mode)) continue;

        files.emplace_back(fullpath);
    }

    closedir(dir);
#endif

    return files;
}

static int parse_int(const char* s, int fallback) {
    if (!s) return fallback;
    char* end = nullptr;
    long v = std::strtol(s, &end, 10);
    if (!end || *end != '\0') return fallback;
    if (v > INT32_MAX) return fallback;
    if (v < INT32_MIN) return fallback;
    return (int)v;
}

#ifndef APP_VERSION
#define APP_VERSION "1.0.0"
#endif

static void print_usage(const char* prog) {
    std::printf(
        "heic_converter_mt %s - batch HEIC to PDF converter\n"
        "\n"
        "Usage: %s [options] [input_dir]\n"
        "\n"
        "Converts every .heic file in input_dir (default: current directory)\n"
        "to a one-page .pdf in the output directory, using multiple threads.\n"
        "\n"
        "Options:\n"
        "  -o, --output <dir>   output directory (default: output)\n"
        "  -q, --quality <n>    image quality 1-100 (default: 90)\n"
        "  -t, --threads <n>    worker threads (default: CPU cores)\n"
        "  -h, --help           show this help and exit\n"
        "  -V, --version        show version and exit\n",
        APP_VERSION, prog);
}

int main(int argc, char** argv) {
    const char* input_dir = ".";
    const char* output_dir = "output";
    int jpeg_quality = 90;

    unsigned int hw = std::thread::hardware_concurrency();
    int threads = (hw == 0) ? 4 : (int)hw;

    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "-h") == 0 || std::strcmp(arg, "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (std::strcmp(arg, "-V") == 0 || std::strcmp(arg, "--version") == 0) {
            std::printf("heic_converter_mt %s\n", APP_VERSION);
            return 0;
        } else if (std::strcmp(arg, "-o") == 0 || std::strcmp(arg, "--output") == 0) {
            if (++i >= argc) { std::fprintf(stderr, "%s requires a directory argument\n", arg); return 2; }
            output_dir = argv[i];
        } else if (std::strcmp(arg, "-q") == 0 || std::strcmp(arg, "--quality") == 0) {
            if (++i >= argc) { std::fprintf(stderr, "%s requires a number argument\n", arg); return 2; }
            jpeg_quality = parse_int(argv[i], -1);
        } else if (std::strcmp(arg, "-t") == 0 || std::strcmp(arg, "--threads") == 0) {
            if (++i >= argc) { std::fprintf(stderr, "%s requires a number argument\n", arg); return 2; }
            threads = parse_int(argv[i], -1);
        } else if (arg[0] == '-' && arg[1] != '\0') {
            std::fprintf(stderr, "Unknown option: %s (try --help)\n", arg);
            return 2;
        } else {
            input_dir = arg;
        }
    }

    if (jpeg_quality < 1 || jpeg_quality > 100) {
        std::fprintf(stderr, "Invalid quality value. Use 1-100.\n");
        return 2;
    }
    if (threads < 1) {
        std::fprintf(stderr, "Invalid thread count. Use a positive number.\n");
        return 2;
    }

    if (!ensure_directory(output_dir)) {
        std::fprintf(stderr, "Failed to create output directory '%s'\n", output_dir);
        return 1;
    }

    std::vector<std::string> files = list_heic_files(input_dir);
    if (files.empty()) {
        std::printf("No HEIC files found in '%s'.\n", input_dir);
        return 0;
    }

    std::printf("Found %zu HEIC files. Converting with quality %d using %d threads...\n",
                files.size(), jpeg_quality, threads);

    WorkQueue wq;
    std::atomic<int> converted_ok{0};
    std::atomic<int> converted_fail{0};

    std::mutex print_mtx;

    // Worker function
    auto worker = [&] {
        std::string path;
        while (wq.pop(path)) {
            bool ok = convert_heic_to_pdf(path.c_str(), output_dir, jpeg_quality);
            if (ok) converted_ok.fetch_add(1, std::memory_order_relaxed);
            else converted_fail.fetch_add(1, std::memory_order_relaxed);


            //int done = converted_ok.load(std::memory_order_relaxed) + converted_fail.load(std::memory_order_relaxed);
            //if ((done % 25) == 0) {
            //    std::lock_guard<std::mutex> lk(print_mtx);
            //    std::printf("Progress: %d/%zu done\n", done, files.size());
            //}
        }
    };

    // Start workers
    std::vector<std::thread> pool;
    pool.reserve((size_t)threads);
    for (int i = 0; i < threads; i++) pool.emplace_back(worker);

    // Enqueue work
    for (auto& f : files) wq.push(std::move(f));
    wq.close();

    // Join
    for (auto& t : pool) t.join();

    int ok = converted_ok.load();
    int fail = converted_fail.load();

    std::printf("Done. Converted OK: %d, Failed: %d\n", ok, fail);
    return (fail == 0) ? 0 : 1;
}
