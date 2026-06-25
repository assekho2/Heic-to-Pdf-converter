/*
Cross-platform (Windows / macOS / Linux) HEIC -> PDF converter.

Each .heic photo becomes a one-page PDF. The decoded image is JPEG-compressed
(the quality option controls that compression) and embedded directly into the
PDF using the DCTDecode filter, so libjpeg is still required.

Build with the included Makefile:
    make heic_converter

Or by hand:

macOS (Homebrew: brew install libheif jpeg):
    gcc -O2 -o heic_converter heic_converter.c \
        -I/opt/homebrew/include -L/opt/homebrew/lib \
        -I/opt/homebrew/opt/jpeg/include -L/opt/homebrew/opt/jpeg/lib \
        -lheif -ljpeg

Windows (MSYS2 MinGW-w64 shell, after
         pacman -S mingw-w64-ucrt-x86_64-libheif mingw-w64-ucrt-x86_64-libjpeg-turbo):
    gcc -O2 -o heic_converter.exe heic_converter.c -lheif -ljpeg

Run with:
    ./heic_converter
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>

#include <libheif/heif.h>
#include <jpeglib.h>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <direct.h>
    #define strcasecmp _stricmp
#else
    #include <dirent.h>
    #include <unistd.h>
#endif

/* windows.h defines MAX_PATH (260), so use our own name for path buffers */
#define PATH_BUF_SIZE 1024

typedef struct {
    struct heif_context* ctx;
    struct heif_image_handle* handle;
    struct heif_image* img;
    FILE* outfile;
    unsigned char* jpeg_buf;   /* in-memory JPEG, embedded into the PDF */
    unsigned long jpeg_size;
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
} ConversionContext;

static inline int has_heic_extension(const char* filename) {
    const char* ext = strrchr(filename, '.');
    if (!ext) return 0;
    return (strcasecmp(ext, ".HEIC") == 0);  // Case-insensitive comparison
}

/* Last path component, handling both '/' and '\' separators */
static const char* path_basename(const char* path) {
    const char* slash = strrchr(path, '/');
#ifdef _WIN32
    const char* bslash = strrchr(path, '\\');
    if (!slash || (bslash && bslash > slash)) slash = bslash;
#endif
    return slash ? slash + 1 : path;
}

static inline int ensure_directory(const char* path) {
    struct stat st;
    if (stat(path, &st) == 0) return 1;  // Already exists
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
        ctx->outfile = NULL;
    }

    if (ctx->img) {
        heif_image_release(ctx->img);
        ctx->img = NULL;
    }

    if (ctx->handle) {
        heif_image_handle_release(ctx->handle);
        ctx->handle = NULL;
    }

    if (ctx->ctx) {
        heif_context_free(ctx->ctx);
        ctx->ctx = NULL;
    }

    jpeg_destroy_compress(&ctx->cinfo);

    /* jpeg_mem_dest allocates this buffer for us; it is ours to free. */
    if (ctx->jpeg_buf) {
        free(ctx->jpeg_buf);
        ctx->jpeg_buf = NULL;
    }
}

/* Wrap a JPEG-compressed RGB image in a minimal one-page PDF. The JPEG bytes
   are embedded verbatim via the DCTDecode filter (no re-encoding). The page is
   sized to the image so one pixel maps to one PDF point. Returns 1 on success. */
static int write_jpeg_as_pdf(FILE* out, const unsigned char* jpeg,
                             unsigned long jpeg_len, int width, int height) {
    long offsets[6] = {0};

    if (fprintf(out, "%%PDF-1.4\n") < 0) return 0;

    offsets[1] = ftell(out);
    fprintf(out, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");

    offsets[2] = ftell(out);
    fprintf(out, "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n");

    offsets[3] = ftell(out);
    fprintf(out, "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] "
                 "/Resources << /XObject << /Im0 4 0 R >> >> /Contents 5 0 R >>\nendobj\n",
            width, height);

    offsets[4] = ftell(out);
    fprintf(out, "4 0 obj\n<< /Type /XObject /Subtype /Image /Width %d /Height %d "
                 "/ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode "
                 "/Length %lu >>\nstream\n",
            width, height, jpeg_len);
    if (fwrite(jpeg, 1, jpeg_len, out) != jpeg_len) return 0;
    fprintf(out, "\nendstream\nendobj\n");

    /* Content stream: draw the image scaled to fill the page. */
    char content[128];
    int clen = snprintf(content, sizeof(content),
                        "q\n%d 0 0 %d 0 0 cm\n/Im0 Do\nQ\n", width, height);
    offsets[5] = ftell(out);
    fprintf(out, "5 0 obj\n<< /Length %d >>\nstream\n", clen);
    fwrite(content, 1, (size_t)clen, out);
    fprintf(out, "endstream\nendobj\n");

    long xref = ftell(out);
    fprintf(out, "xref\n0 6\n0000000000 65535 f \n");
    for (int i = 1; i <= 5; i++) {
        fprintf(out, "%010ld 00000 n \n", offsets[i]);
    }
    fprintf(out, "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n%ld\n%%%%EOF\n", xref);

    return (ferror(out) == 0);
}

static int convert_heic_to_pdf(const char* input_path, const char* output_dir, int jpeg_quality) {
    ConversionContext ctx = {0};  // Zero-initialize all members
    struct heif_error err;
    int success = 0;

    // Initialize JPEG error handler first
    ctx.cinfo.err = jpeg_std_error(&ctx.jerr);
    jpeg_create_compress(&ctx.cinfo);

    // Initialize HEIF context
    if (!(ctx.ctx = heif_context_alloc())) {
        fprintf(stderr, "Failed to allocate HEIF context\n");
        goto cleanup;
    }

    // Read HEIC file
    if ((err = heif_context_read_from_file(ctx.ctx, input_path, NULL)).code != heif_error_Ok) {
        fprintf(stderr, "Could not read HEIC file: %s\n", input_path);
        goto cleanup;
    }

    // Get primary image handle
    if ((err = heif_context_get_primary_image_handle(ctx.ctx, &ctx.handle)).code != heif_error_Ok) {
        fprintf(stderr, "Could not get primary image handle\n");
        goto cleanup;
    }

    // Decode the image
    if ((err = heif_decode_image(ctx.handle, &ctx.img, heif_colorspace_RGB,
                                heif_chroma_interleaved_RGB, NULL)).code != heif_error_Ok) {
        fprintf(stderr, "Could not decode image\n");
        goto cleanup;
    }

    // Get image data
    int stride;
    const uint8_t* data = heif_image_get_plane_readonly(ctx.img, heif_channel_interleaved, &stride);

    int width = heif_image_get_width(ctx.img, heif_channel_interleaved);
    int height = heif_image_get_height(ctx.img, heif_channel_interleaved);

    // Create output filename
    char output_filename[PATH_BUF_SIZE];
    const char* base_name = path_basename(input_path);

    snprintf(output_filename, sizeof(output_filename), "%s/%.*s.pdf",
             output_dir, (int)(strrchr(base_name, '.') - base_name), base_name);

    // Compress the image to JPEG in memory, then embed it in a PDF.
    jpeg_mem_dest(&ctx.cinfo, &ctx.jpeg_buf, &ctx.jpeg_size);
    ctx.cinfo.image_width = width;
    ctx.cinfo.image_height = height;
    ctx.cinfo.input_components = 3;
    ctx.cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&ctx.cinfo);
    jpeg_set_quality(&ctx.cinfo, jpeg_quality, TRUE);
    jpeg_start_compress(&ctx.cinfo, TRUE);

    // Write scan lines
    while (ctx.cinfo.next_scanline < ctx.cinfo.image_height) {
        JSAMPROW row_pointer = (JSAMPROW)&data[ctx.cinfo.next_scanline * stride];
        jpeg_write_scanlines(&ctx.cinfo, &row_pointer, 1);
    }

    jpeg_finish_compress(&ctx.cinfo);

    // Open output file and wrap the JPEG in a one-page PDF.
    if (!(ctx.outfile = fopen(output_filename, "wb"))) {
        fprintf(stderr, "Could not create output file: %s\n", output_filename);
        goto cleanup;
    }

    if (!write_jpeg_as_pdf(ctx.outfile, ctx.jpeg_buf, ctx.jpeg_size, width, height)) {
        fprintf(stderr, "Could not write PDF: %s\n", output_filename);
        goto cleanup;
    }

    success = 1;

cleanup:
    cleanup_context(&ctx);
    return success;
}

/* Convert every .heic file in input_dir. Returns the number converted, or -1
   if the directory could not be opened. */
static int process_directory(const char* input_dir, const char* output_dir, int jpeg_quality) {
    char input_path[PATH_BUF_SIZE];
    int files_processed = 0;

#ifdef _WIN32
    char pattern[PATH_BUF_SIZE];
    WIN32_FIND_DATAA find_data;
    HANDLE find;

    snprintf(pattern, sizeof(pattern), "%s\\*", input_dir);
    find = FindFirstFileA(pattern, &find_data);
    if (find == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Could not open %s directory (error %lu)\n", input_dir, GetLastError());
        return -1;
    }

    do {
        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!has_heic_extension(find_data.cFileName)) continue;

        snprintf(input_path, sizeof(input_path), "%s/%s", input_dir, find_data.cFileName);
        if (convert_heic_to_pdf(input_path, output_dir, jpeg_quality)) {
            files_processed++;
        }
    } while (FindNextFileA(find, &find_data));

    FindClose(find);
#else
    DIR* dir = opendir(input_dir);
    struct dirent* entry;

    if (!dir) {
        fprintf(stderr, "Could not open %s directory: %s\n", input_dir, strerror(errno));
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (has_heic_extension(entry->d_name)) {
            snprintf(input_path, sizeof(input_path), "%s/%s", input_dir, entry->d_name);
            if (convert_heic_to_pdf(input_path, output_dir, jpeg_quality)) {
                files_processed++;
            }
        }
    }

    closedir(dir);
#endif

    return files_processed;
}

int main(void) {
    const char* input_dir = "Photos";
    const char* output_dir = "output";
    int files_processed;
    int jpeg_quality;

    printf("Enter image quality (1-100, recommended 75-95): ");
    if (scanf("%d", &jpeg_quality) != 1 || jpeg_quality < 1 || jpeg_quality > 100) {
        fprintf(stderr, "Invalid quality value. Please enter a number between 1 and 100.\n");
        return 1;
    }
    printf("Using image quality: %d\n", jpeg_quality);

    if (!ensure_directory(output_dir)) {
        fprintf(stderr, "Failed to create output directory\n");
        return 1;
    }

    printf("Converting files...\n");

    files_processed = process_directory(input_dir, output_dir, jpeg_quality);
    if (files_processed < 0) {
        return 1;
    }

    if (files_processed == 0) {
        printf("No HEIC files found in the Photos directory.\n");
    } else {
        printf("Successfully converted %d photos to PDF format.\n", files_processed);
    }

    return 0;
}
