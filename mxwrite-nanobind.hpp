#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/chrono.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/vector.h>

#include "mxwrite.hpp"

namespace nb = nanobind;

NB_MODULE(mxwrite_ext, m) {
    m.doc() = "Python bindings for MXWrite FFmpeg video writer";

    nb::class_<Frame_Data>(m, "FrameData").def(nb::init<>()).def_rw("capture_time", &Frame_Data::capture_time).def_prop_rw("data", [](Frame_Data &f) { return nb::capsule(f.data); }, [](Frame_Data &f, nb::capsule c) { f.data = c.data(); });

    nb::class_<EncoderInfo>(m, "EncoderInfo").def(nb::init<>()).def_rw("name", &EncoderInfo::name).def_rw("long_name", &EncoderInfo::long_name).def_rw("codec_name", &EncoderInfo::codec_name).def_rw("pixel_formats", &EncoderInfo::pixel_formats).def_rw("hardware", &EncoderInfo::hardware).def_rw("experimental", &EncoderInfo::experimental);

    nb::class_<EncoderOptionInfo>(m, "EncoderOptionInfo").def(nb::init<>()).def_rw("name", &EncoderOptionInfo::name).def_rw("type", &EncoderOptionInfo::type).def_rw("default_value", &EncoderOptionInfo::default_value).def_rw("minimum", &EncoderOptionInfo::minimum).def_rw("maximum", &EncoderOptionInfo::maximum).def_rw("choices", &EncoderOptionInfo::choices).def_rw("help", &EncoderOptionInfo::help);

    m.def("available_video_encoders", &available_video_encoders);
    m.def("video_encoder_options", &video_encoder_options, nb::arg("encoder_name"));

    nb::class_<EncodeOptions::HdrInfo>(m, "HdrInfo").def(nb::init<>()).def_rw("enabled", &EncodeOptions::HdrInfo::enabled).def_rw("color_primaries", &EncodeOptions::HdrInfo::color_primaries).def_rw("color_trc", &EncodeOptions::HdrInfo::color_trc).def_rw("color_space", &EncodeOptions::HdrInfo::color_space).def_rw("color_range", &EncodeOptions::HdrInfo::color_range).def_rw("mastering_display", &EncodeOptions::HdrInfo::mastering_display).def_rw("content_light", &EncodeOptions::HdrInfo::content_light);

    nb::class_<EncodeOptions>(m, "EncodeOptions").def(nb::init<>()).def_rw("preset", &EncodeOptions::preset).def_rw("tune", &EncodeOptions::tune).def_rw("crf", &EncodeOptions::crf).def_rw("bit_rate", &EncodeOptions::bit_rate).def_rw("codec", &EncodeOptions::codec).def_rw("ffmpeg_options", &EncodeOptions::ffmpeg_options).def_rw("realtime", &EncodeOptions::realtime).def_rw("block_when_full", &EncodeOptions::block_when_full).def_rw("hdr", &EncodeOptions::hdr);

    nb::class_<Writer>(m, "Writer")
        .def(nb::init<>())

        .def("open", nb::overload_cast<const std::string &, int, int, float, const char *>(&Writer::open), nb::arg("filename"), nb::arg("width"), nb::arg("height"), nb::arg("fps"), nb::arg("crf"))
        .def("open", nb::overload_cast<const std::string &, int, int, float, const EncodeOptions &>(&Writer::open), nb::arg("filename"), nb::arg("width"), nb::arg("height"), nb::arg("fps"), nb::arg("opts"))

        .def("open_ts", nb::overload_cast<const std::string &, int, int, float, const char *>(&Writer::open_ts), nb::arg("filename"), nb::arg("width"), nb::arg("height"), nb::arg("fps"), nb::arg("crf"))
        .def("open_ts", nb::overload_cast<const std::string &, int, int, float, const EncodeOptions &>(&Writer::open_ts), nb::arg("filename"), nb::arg("width"), nb::arg("height"), nb::arg("fps"), nb::arg("opts"))

        .def(
            "write",
            [](Writer &w, nb::ndarray<uint8_t, nb::c_contig, nb::device::cpu> arr) { w.write(arr.data()); },
            nb::arg("rgba_buffer"))

        .def(
            "write_at_pts",
            [](Writer &w, nb::ndarray<uint8_t, nb::c_contig, nb::device::cpu> arr, int64_t pts) { w.write_at_pts(arr.data(), pts); },
            nb::arg("rgba_buffer"),
            nb::arg("pts"))

        .def(
            "write_ts",
            [](Writer &w, nb::ndarray<uint8_t, nb::c_contig, nb::device::cpu> arr) { w.write_ts(arr.data()); },
            nb::arg("rgba_buffer"))

        .def(
            "write_hdr_rgba16",
            [](Writer &w, nb::ndarray<uint16_t, nb::c_contig, nb::device::cpu> arr) { w.write_hdr_rgba16(arr.data()); },
            nb::arg("rgba16_buffer"))

        .def(
            "write_hdr_rgba16_at_pts",
            [](Writer &w, nb::ndarray<uint16_t, nb::c_contig, nb::device::cpu> arr, int64_t pts) { w.write_hdr_rgba16_at_pts(arr.data(), pts); },
            nb::arg("rgba16_buffer"),
            nb::arg("pts"))

#ifdef MXWRITE_HAS_CUDA_COPY
        .def(
            "write_cuda_rgba",
            [](Writer &w, nb::ndarray<uint8_t, nb::c_contig, nb::device::cuda> arr, int src_stride, bool bottom_up) { return w.write_cuda_rgba(arr.data(), src_stride, bottom_up); },
            nb::arg("cuda_rgba_buffer"),
            nb::arg("src_stride"),
            nb::arg("bottom_up") = false)

        .def(
            "write_cuda_rgba_at_pts",
            [](Writer &w, nb::ndarray<uint8_t, nb::c_contig, nb::device::cuda> arr, int src_stride, int64_t pts, bool bottom_up) { return w.write_cuda_rgba_at_pts(arr.data(), src_stride, pts, bottom_up); },
            nb::arg("cuda_rgba_buffer"),
            nb::arg("src_stride"),
            nb::arg("pts"),
            nb::arg("bottom_up") = false)
#endif

        .def("close", &Writer::close)
        .def("is_open", &Writer::is_open)
        .def("is_hardware_encode", &Writer::is_hardware_encode)
        .def("set_block_when_full", &Writer::set_block_when_full, nb::arg("value"))
        .def("get_block_when_full", &Writer::get_block_when_full)
        .def("get_frame_count", &Writer::get_frame_count)
        .def("get_bytes_written", &Writer::get_bytes_written)
        .def("get_duration", &Writer::get_duration);

    m.def("transfer_audio", &transfer_audio, nb::arg("sourceAudioFile"), nb::arg("destVideoFile"));

    m.def("cleanup_contexts", [](nb::capsule source_ctx, nb::capsule dest_ctx, nb::capsule output_ctx) { cleanup_contexts(static_cast<AVFormatContext *>(source_ctx.data()), static_cast<AVFormatContext *>(dest_ctx.data()), static_cast<AVFormatContext *>(output_ctx.data())); });
}
