/**
 * @file mxwrite.hpp
 * @brief FFmpeg-based video writer used by MXWrite.
 */
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
#ifdef MXWRITE_HAS_CUDA_COPY
#include <cuda_runtime.h>
#endif

/**
 * @brief Queue entry that stores a frame pointer and its capture timestamp.
 */
struct Frame_Data {
    void *data; ///< Pointer to RGBA frame data owned by the producer.
    std::chrono::steady_clock::time_point capture_time; ///< Capture time for timestamp-based encoding.
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
 * codec:  "auto" (NVENC if available, else software), "software" (force software),
 *         "nvenc" (force resolution-selected NVENC), "h264_nvenc", or
 *         "hevc_nvenc". NVENC requests fall back to the matching software codec.
 * realtime: when true, applies low-latency settings (tune=zerolatency for x264,
 *           tune=ll + zerolatency=1 for NVENC). Overrides tune value.
 * block_when_full: when true, producer threads block if the encoder queue is
 *                  full instead of dropping frames.
 */
struct EncodeOptions {
    std::string preset = "medium"; ///< Encoder preset name.
    std::string tune = "";         ///< Optional tuning mode.
    int crf = 18;                   ///< Constant Rate Factor.
    std::string codec = "auto";     ///< Encoder selection policy or concrete NVENC codec.
    bool realtime = false;          ///< Enable low-latency settings.
    bool block_when_full = false;   ///< Block producer threads instead of dropping when the encoder queue is full.

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
        bool enabled = false; ///< Enables the HDR output path.
        int color_primaries = 0; ///< AVColorPrimaries value.
        int color_trc = 0; ///< AVColorTransferCharacteristic value.
        int color_space = 0; ///< AVColorSpace value.
        int color_range = 0; ///< AVColorRange value.
        /// Raw AVMasteringDisplayMetadata side-data bytes, or empty.
        std::vector<uint8_t> mastering_display;
        /// Raw AVContentLightMetadata side-data bytes, or empty.
        std::vector<uint8_t> content_light;
    } hdr;
};

/**
 * @brief FFmpeg-backed RGBA video writer.
 *
 * The writer accepts host RGBA buffers, optional CUDA device buffers, and
 * 16-bit HDR RGBA buffers. It can encode either frame-by-frame or from
 * timestamped frames, depending on the open mode.
 */
class Writer {
  public:
    /** @brief Construct a closed writer. */
    Writer() = default;

    /**
     * @brief Open an output file using the legacy CRF string interface.
     * @param filename Output file path.
     * @param width Output width in pixels.
     * @param height Output height in pixels.
     * @param fps Output frame rate.
     * @param crf Constant Rate Factor as a string.
     * @return true on success.
     */
    bool open(const std::string &filename, int width, int height, float fps, const char *crf);
    /**
     * @brief Open an output file using explicit encoder options.
     * @param filename Output file path.
     * @param width Output width in pixels.
     * @param height Output height in pixels.
     * @param fps Output frame rate.
     * @param opts Encoder configuration.
     * @return true on success.
     */
    bool open(const std::string &filename, int width, int height, float fps, const EncodeOptions &opts);
    /**
     * @brief Queue a host RGBA frame for immediate-mode encoding.
     * @param rgba_buffer Pointer to tightly packed RGBA8 pixels.
     */
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
     * @param rgba16_buffer Pointer to tightly packed RGBA16 pixels.
     */
    void write_hdr_rgba16(void *rgba16_buffer);
    /**
     * @brief Queue a CUDA RGBA frame for encoding.
     * @param cuda_rgba_buffer CUDA device pointer.
     * @param src_stride Source row pitch in bytes.
     * @param bottom_up Whether the source is stored bottom-up.
     * @return true if the frame was accepted.
     */
    bool write_cuda_rgba(void *cuda_rgba_buffer, int src_stride, bool bottom_up = false);
    /**
     * @brief Open a timestamp-based output stream using the legacy CRF string interface.
     * @param filename Output file path.
     * @param width Output width in pixels.
     * @param height Output height in pixels.
     * @param fps Nominal input frame rate.
     * @param crf Constant Rate Factor as a string.
     * @return true on success.
     */
    bool open_ts(const std::string &filename, int width, int height, float fps, const char *crf);
    /**
     * @brief Open a timestamp-based output stream using explicit encoder options.
     * @param filename Output file path.
     * @param width Output width in pixels.
     * @param height Output height in pixels.
     * @param fps Nominal input frame rate.
     * @param opts Encoder configuration.
     * @return true on success.
     */
    bool open_ts(const std::string &filename, int width, int height, float fps, const EncodeOptions &opts);
    /**
     * @brief Queue a host RGBA frame using capture timestamps.
     * @param rgba_buffer Pointer to tightly packed RGBA8 pixels.
     */
    void write_ts(void *rgba_buffer);
    /** @brief Close the writer and flush pending packets. */
    void close();
    /** @brief Check whether the writer is currently open. */
    bool is_open() const { return opened; }
    /// @brief True when the active encoder backend is hardware (NVENC).
    bool is_hardware_encode() const { return use_hw_encode; }
    /// @brief If true, producer threads block when the encoder queue is full
    /// instead of dropping frames. Intended for headless/batch transcoding
    /// where every input frame must reach the output. Default: false (drop).
    void set_block_when_full(bool value) { block_when_full = value; }
    /** @brief Check whether the encoder queue blocks instead of dropping frames. */
    bool get_block_when_full() const { return block_when_full; }
    /** @brief Return the number of frames submitted to the writer. */
    int64_t get_frame_count() const { return frame_count; }
    /** @brief Return the encoded duration in seconds. */
    double get_duration() const;
    /** @brief Close the writer on destruction if it is still open. */
    ~Writer() {
        if (is_open()) {
            close();
            opened = false;
        }
    }

  private:
    bool opened{false}; ///< Internal open-state flag.
    int width = 0; ///< Output width in pixels.
    int height = 0; ///< Output height in pixels.
    int fps_num = 0; ///< Output FPS numerator.
    int fps_den = 0; ///< Output FPS denominator.
    int64_t frame_count = 0; ///< Frames submitted so far.
    double last_duration = 0.0; ///< Cached duration from the last encode step.
    AVFormatContext *format_ctx = nullptr; ///< Active container context.
    AVCodecContext *codec_ctx = nullptr; ///< Active codec context.
    AVStream *stream = nullptr; ///< Output video stream.
    AVFrame *frameYUV = nullptr; ///< Software-converted YUV frame.
    AVFrame *frameRGBA = nullptr; ///< Staging RGBA frame.
    AVFrame *frame10 = nullptr;            ///< YUV420P10LE frame used for HDR output.
    AVFrame *upload_sw_frame = nullptr; ///< Software upload frame used by CUDA/hardware paths.
    AVBufferRef *hw_device_ctx = nullptr; ///< Hardware device context, when available.
    AVBufferRef *hw_frames_ctx = nullptr; ///< Hardware frames pool, when available.
    bool use_hw_encode = false; ///< True when hardware encoding is active.
#ifdef MXWRITE_HAS_CUDA_COPY
    // Dedicated stream so the producer's RGBA→hwframe copy does not serialise
    // with the renderer's default-stream work or with the encoder thread.
    cudaStream_t cuda_upload_stream = nullptr;
#endif
    bool hdr_output = false;              ///< True when HDR (HEVC Main10/PQ) output is active.
    EncodeOptions::HdrInfo hdr_info;      ///< HDR metadata captured at open() time.
    SwsContext *sws_ctx = nullptr; ///< Frame conversion context.
    AVRational time_base; ///< Stream time base.
    /** @brief Convert a frame rate into a rational numerator/denominator pair. */
    void calculateFPSFraction(float fps, int &fps_num, int &fps_den);
    std::chrono::steady_clock::time_point recordingStart; ///< Start time for timestamp mode.

    std::queue<AVFrame *> encode_queue; ///< Pending encoded frames.
    // Deep enough to absorb encoder hiccups (~4s at 30fps, ~2s at 60fps).
    // Memory cost is bounded by the NVENC frame pool / sw RGBA frame buffer.
    static constexpr size_t MAX_QUEUE_SIZE = 120;
    std::condition_variable queue_cv; ///< Signals queue availability.
    std::jthread encode_thread; ///< Background encoder thread.

    std::mutex queue_mutex{}; ///< Guards the frame queue.
    std::mutex writer_mutex{}; ///< Guards writer state transitions.
    bool stop_requested = false; ///< Signals encoder shutdown.
    std::atomic<bool> block_when_full{false}; ///< Queue backpressure mode.

    /** @brief Shared implementation for open() and open_ts(). */
    bool openInternal(const std::string &filename, int w, int h, float fps, const EncodeOptions &opts, bool ts_mode);
    /** @brief Initialize CUDA/NVENC resources when hardware encoding is selected. */
    bool initHardwareEncoding();
    /** @brief Start the background encoder thread. */
    void startEncoderThread();
    /** @brief Stop the background encoder thread. */
    void stopEncoderThread();
    /** @brief Encoder thread main loop. */
    void encodeLoop(std::stop_token stop_token);
    /** @brief Encode and write one frame. */
    void encodeAndWriteFrame(AVFrame *in_frame);
    /** @brief Drain packets from the codec into the container. */
    void drainEncoderPackets();
    /** @brief Release a frame allocated for the encode queue. */
    void releaseFrame(AVFrame *f);
};

/**
 * @brief Copy audio from one video file to another.
 * @param sourceAudioFile Input media file containing the audio stream.
 * @param destVideoFile Output video file to receive the audio stream.
 */
extern void transfer_audio(std::string_view sourceAudioFile, std::string_view destVideoFile);
/**
 * @brief Free FFmpeg format contexts used during transfer operations.
 * @param source_ctx Source format context.
 * @param dest_ctx Destination format context.
 * @param output_ctx Output format context.
 */
extern void cleanup_contexts(AVFormatContext *source_ctx, AVFormatContext *dest_ctx, AVFormatContext *output_ctx);

#endif
