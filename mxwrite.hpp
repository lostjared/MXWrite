#ifndef FFWRITE_HPP
#define FFWRITE_HPP
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
#include <chrono>
#include <condition_variable>
#include <atomic>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <cstdint>

struct Frame_Data {
    void *data;
    std::chrono::steady_clock::time_point capture_time;
};

/**
 * @brief User-configurable video encoder quality options.
 *
 * preset: x264 preset name — ultrafast, superfast, veryfast, faster, fast,
 *         medium, slow, slower, veryslow. Mapped to NVENC p1..p7.
 * tune:   x264 tune — empty string (none), film, animation, grain, stillimage,
 *         psnr, ssim, fastdecode, zerolatency.
 * crf:    Constant Rate Factor, 0 (lossless) .. 51 (worst). 18 is visually
 *         near-lossless; 23 is default for x264; 28 is typical "small file".
 *         For NVENC this is forwarded as `cq`.
 * codec:  "auto" (NVENC if available, else x264), "software" (force x264),
 *         "nvenc" (force NVENC; falls back to x264 if NVENC unavailable).
 * realtime: when true, applies low-latency settings (tune=zerolatency for x264,
 *           tune=ll + zerolatency=1 for NVENC). Overrides tune value.
 */
struct EncodeOptions {
    std::string preset = "medium";
    std::string tune = "";
    int crf = 18;
    std::string codec = "auto";
    bool realtime = false;

    /**
     * @brief HDR output options.
     *
     * When @ref HdrInfo::enabled is true, the writer switches to a dedicated
     * HEVC Main10 + BT.2020 output path that:
     *  - Encodes with libx265 at 10-bit (AV_PIX_FMT_YUV420P10LE).
     *  - Tags the stream with BT.2020 primaries, BT.2020 non-constant luminance
     *    matrix, and SMPTE ST.2084 (PQ) transfer.
     *  - Converts incoming 8-bit sRGB RGBA shader output into PQ-encoded
     *    10-bit YUV, placing SDR-range content at the 100-nit reference level
     *    inside the PQ signal (SDR-in-HDR-container).
     *  - Copies @ref mastering_display and @ref content_light side data from
     *    the input stream when provided, so player HDR metadata is preserved.
     *
     * This mode is intended for use when the *input* video is HDR; the 8-bit
     * GL pipeline cannot reconstruct the original highlight precision, but the
     * resulting file is a correctly-tagged HDR container.
     */
    struct HdrInfo {
        bool enabled = false;
        int color_primaries = 0;  ///< AVColorPrimaries (e.g. AVCOL_PRI_BT2020).
        int color_trc = 0;         ///< AVColorTransferCharacteristic (PQ/HLG).
        int color_space = 0;       ///< AVColorSpace (e.g. AVCOL_SPC_BT2020_NCL).
        int color_range = 0;       ///< AVColorRange (limited/full).
        /// Raw AVMasteringDisplayMetadata side-data bytes, or empty.
        std::vector<uint8_t> mastering_display;
        /// Raw AVContentLightMetadata side-data bytes, or empty.
        std::vector<uint8_t> content_light;
    } hdr;
};

class Writer {
  public:
    Writer() = default;

    bool open(const std::string &filename, int width, int height, float fps, const char *crf);
    bool open(const std::string &filename, int width, int height, float fps, const EncodeOptions &opts);
    void write(void *rgba_buffer);
    /**
     * @brief Write a 16-bit RGBA frame that is already PQ- or HLG-encoded in
     *        BT.2020 primaries (8 bytes/pixel: R16,G16,B16,A16, little-endian
     *        unsigned normalised).
     *
     * The data is expected to originate from the HDR GPU encode pass: it is
     * BT.2020 primaries with a non-linear PQ (or HLG) transfer already
     * applied, so this path skips colour-space conversion and only performs
     *   (a) the BT.2020 non-constant-luminance RGB'->YCbCr' matrix, and
     *   (b) 16-bit -> 10-bit limited-range scaling,
     * producing AV_PIX_FMT_YUV420P10LE for libx265 Main10. Requires the
     * writer to have been opened with @ref EncodeOptions::HdrInfo::enabled.
     */
    void write_hdr_rgba16(void *rgba16_buffer);
    bool write_cuda_rgba(void *cuda_rgba_buffer, int src_stride, bool bottom_up = false);
    bool open_ts(const std::string &filename, int width, int height, float fps, const char *crf);
    bool open_ts(const std::string &filename, int width, int height, float fps, const EncodeOptions &opts);
    void write_ts(void *rgba_buffer);
    void close();
    bool is_open() const { return opened; }
    /// @brief True when the active encoder backend is hardware (NVENC).
    bool is_hardware_encode() const { return use_hw_encode; }
    /// @brief If true, producer threads block when the encoder queue is full
    /// instead of dropping frames. Intended for headless/batch transcoding
    /// where every input frame must reach the output. Default: false (drop).
    void set_block_when_full(bool value) { block_when_full = value; }
    bool get_block_when_full() const { return block_when_full; }
    int64_t get_frame_count() const { return frame_count; }
    double get_duration() const;
    ~Writer() {
        if (is_open()) {
            close();
            opened = false;
        }
    }

  private:
    bool opened{false};
    int width = 0;
    int height = 0;
    int fps_num = 0;
    int fps_den = 0;
    int64_t frame_count = 0;
    double last_duration = 0.0;
    AVFormatContext *format_ctx = nullptr;
    AVCodecContext *codec_ctx = nullptr;
    AVStream *stream = nullptr;
    AVFrame *frameYUV = nullptr;
    AVFrame *frameRGBA = nullptr;
    AVFrame *frame10 = nullptr;            ///< YUV420P10LE frame used for HDR output.
    AVFrame *upload_sw_frame = nullptr;
    AVBufferRef *hw_device_ctx = nullptr;
    AVBufferRef *hw_frames_ctx = nullptr;
    bool use_hw_encode = false;
    bool hdr_output = false;              ///< True when HDR (HEVC Main10/PQ) output is active.
    EncodeOptions::HdrInfo hdr_info;      ///< HDR metadata captured at open() time.
    SwsContext *sws_ctx = nullptr;
    AVRational time_base;
    void calculateFPSFraction(float fps, int &fps_num, int &fps_den);
    std::chrono::steady_clock::time_point recordingStart;

    std::queue<AVFrame *> encode_queue;
    static constexpr size_t MAX_QUEUE_SIZE = 30;
    std::condition_variable queue_cv;
    std::jthread encode_thread;

    std::mutex queue_mutex{};
    std::mutex writer_mutex{};
    bool stop_requested = false;
    std::atomic<bool> block_when_full{false};

    bool openInternal(const std::string &filename, int w, int h, float fps, const EncodeOptions &opts, bool ts_mode);
    bool initHardwareEncoding();
    void startEncoderThread();
    void stopEncoderThread();
    void encodeLoop(std::stop_token stop_token);
    void encodeAndWriteFrame(AVFrame *in_frame);
    void drainEncoderPackets();
    void releaseFrame(AVFrame *f);
};

extern void transfer_audio(std::string_view sourceAudioFile, std::string_view destVideoFile);
extern void cleanup_contexts(AVFormatContext *source_ctx, AVFormatContext *dest_ctx, AVFormatContext *output_ctx);

#endif