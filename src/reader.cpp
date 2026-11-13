#include <windows.h>
#include <objidl.h>
#include <mmreg.h>
#include <ttpcomm/audio_format_probe_impl.h>
#include <ttpcomm/pcm_format.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <cwctype>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <stdexcept>
#include <vector>
#include <wrl/client.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/samplefmt.h>
}

namespace ttp::mkv {
namespace {
namespace core {
std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!size) throw std::runtime_error("Invalid UTF-8 metadata");
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}
}
using Microsoft::WRL::ComPtr;
constexpr GUID reader_id{0x30c7c165,0xc0a9,0x4204,{0x99,0x5d,0x45,0x6a,0x76,0x49,0x98,0xfb}};
constexpr GUID creator_id{0x476d15a5,0xd863,0x416a,{0x8a,0x59,0xa9,0xc7,0xd7,0x2c,0xe0,0x4e}};
constexpr GUID metadata_id{0x7ad84e00,0x5fef,0x4481,{0xb5,0x32,0xfb,0xbd,0x67,0x7e,0x67,0xc2}};
constexpr GUID thumbnail_id{0xb5e770af,0xdfb0,0x43e5,{0x9b,0x0c,0x3e,0xe9,0x8e,0x7b,0x62,0x48}};
constexpr GUID async_read_id{0x83e2bbbf,0x4fec,0x4e00,{0xa6,0x2c,0x62,0x61,0xfe,0x2b,0xa2,0xd4}};
constexpr GUID callback_id{0x1717a4a7,0xa3dc,0x416b,{0xa2,0x97,0x72,0x16,0x74,0x9b,0xd7,0xbf}};
constexpr size_t maximum_packet = 32U * 1024U * 1024U;
// These declarations preserve the recovered slots without extending an old
// plugin's virtual table. The independent public ABI copy is in include/.
struct Buffer : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetLength(DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE Capacity(DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Data(BYTE**, DWORD*) = 0;
};
struct Reader : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Open(IStream*, DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE Capabilities(DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Duration(DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Format(WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferSize(DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE CodecName(wchar_t**) = 0;
    virtual HRESULT STDMETHODCALLTYPE Bitrate(DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE BitDepth(WORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(IUnknown*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Start() = 0;
    virtual HRESULT STDMETHODCALLTYPE Stop() = 0;
    virtual HRESULT STDMETHODCALLTYPE Read(Buffer*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Seek(DWORD*) = 0;
};
struct Metadata : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Count(DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE At(DWORD, wchar_t**, wchar_t**) = 0;
    virtual HRESULT STDMETHODCALLTYPE Get(const char*, wchar_t**) = 0;
    virtual HRESULT STDMETHODCALLTYPE Set(const char*, const wchar_t*) = 0;
};
#pragma pack(push, 4)
struct Picture { DWORD size; const wchar_t* mime; const wchar_t* description; DWORD bytes; const BYTE* data; DWORD type; };
#pragma pack(pop)
static_assert(sizeof(Picture) == 24);
struct Thumbnail : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE PictureCount(DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE MaximumBytes(DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE MaximumCount(DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE PictureAt(DWORD, const Picture**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ReplacePicture(DWORD, const Picture*, DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddPicture(const Picture*) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemovePicture(DWORD) = 0;
};
struct Creator : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Create(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE Name(wchar_t**) = 0;
    virtual HRESULT STDMETHODCALLTYPE Extensions(wchar_t**) = 0;
};
struct Failure { HRESULT result; };
void Require(bool condition, HRESULT result = HRESULT_FROM_WIN32(ERROR_INVALID_DATA)) {
    if (!condition) throw Failure{result};
}
void Check(HRESULT result) { if (FAILED(result)) throw Failure{result}; }
template<class F> HRESULT Protect(F&& action) noexcept {
    try { return action(); }
    catch (const Failure& error) { return error.result; }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}
HRESULT Text(std::wstring_view value, wchar_t** output) noexcept {
    if (!output) return E_POINTER;
    *output = static_cast<wchar_t*>(CoTaskMemAlloc((value.size() + 1) * sizeof(wchar_t)));
    if (!*output) return E_OUTOFMEMORY;
    memcpy(*output, value.data(), value.size() * sizeof(wchar_t));
    (*output)[value.size()] = 0;
    return S_OK;
}
const char* Value(AVDictionary* dictionary, const char* key) {
    const auto* item = av_dict_get(dictionary, key, nullptr, 0);
    return item ? item->value : "";
}
bool Supported(AVCodecID id) {
    switch (id) {
    case AV_CODEC_ID_MP1: case AV_CODEC_ID_MP2: case AV_CODEC_ID_MP3:
    case AV_CODEC_ID_AAC: case AV_CODEC_ID_AC3: case AV_CODEC_ID_EAC3:
    case AV_CODEC_ID_DTS: case AV_CODEC_ID_FLAC: case AV_CODEC_ID_ALAC:
    case AV_CODEC_ID_OPUS: case AV_CODEC_ID_VORBIS:
    case AV_CODEC_ID_PCM_U8: case AV_CODEC_ID_PCM_S16LE: case AV_CODEC_ID_PCM_S16BE:
    case AV_CODEC_ID_PCM_S24LE: case AV_CODEC_ID_PCM_S24BE:
    case AV_CODEC_ID_PCM_S32LE: case AV_CODEC_ID_PCM_S32BE:
    case AV_CODEC_ID_PCM_F32LE: case AV_CODEC_ID_PCM_F64LE:
        return avcodec_find_decoder(id) != nullptr;
    default: return false;
    }
}
bool SupportedPcmDepth(const AVCodecParameters* parameters) {
    int expected{};
    switch (parameters->codec_id) {
    case AV_CODEC_ID_PCM_U8: expected = 8; break;
    case AV_CODEC_ID_PCM_S16LE: case AV_CODEC_ID_PCM_S16BE: expected = 16; break;
    case AV_CODEC_ID_PCM_S24LE: case AV_CODEC_ID_PCM_S24BE: expected = 24; break;
    case AV_CODEC_ID_PCM_S32LE: case AV_CODEC_ID_PCM_S32BE: case AV_CODEC_ID_PCM_F32LE: expected = 32; break;
    case AV_CODEC_ID_PCM_F64LE: expected = 64; break;
    default: return true;
    }
    // Upstream falls back to S16 for unrecognised Matroska PCM bit depths.
    // Never decode a declared 12/20/etc-bit payload with that wrong frame size.
    return parameters->bits_per_coded_sample == expected;
}
struct Cover {
    std::wstring mime, description, name;
    std::vector<BYTE> bytes;
    Picture view{};
    bool front{};
};

class Document {
    ComPtr<IStream> input_;
    ComPtr<IUnknown> async_;
    HANDLE event_{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    AVIOContext* io_{};
    AVFormatContext* container_{};
    AVCodecContext* decoder_{};
    AVFrame* frame_{av_frame_alloc()};
    AVPacket* packet_{av_packet_alloc()};
    uint64_t size_{}, byte_position_{};
    HRESULT io_error_{S_OK};
    int track_{-1}, sample_format_{-1};
    bool demux_end_{}, flush_sent_{}, decoded_end_{}, origin_known_{};
    int64_t origin_{}, decoded_position_{}, queue_start_{}, position_{};
    std::vector<BYTE> queue_;
    size_t queue_at_{};

    static int ReadIo(void* opaque, uint8_t* bytes, int count) noexcept {
        auto& self = *static_cast<Document*>(opaque);
        if (FAILED(self.io_error_)) return AVERROR_EXTERNAL;
        if (self.byte_position_ == self.size_) return AVERROR_EOF;
        const auto wanted = static_cast<ULONG>(std::min<uint64_t>(count, self.size_ - self.byte_position_));
        ULONG done{};
        const HRESULT status = Protect([&] {
            while (done < wanted) {
                ULONG got{};
                HRESULT result;
                if (self.async_) {
                    using ReadAsync = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, void*, ULONG, ULONG*, HANDLE, DWORD);
                    auto** table = *reinterpret_cast<void***>(self.async_.Get());
                    result = reinterpret_cast<ReadAsync>(table[14])(self.async_.Get(), bytes + done, wanted - done, &got, self.event_, 60000);
                } else result = self.input_->Read(bytes + done, wanted - done, &got);
                Check(result);
                Require(got && got <= wanted - done, STG_E_READFAULT);
                done += got;
            }
            return S_OK;
        });
        if (FAILED(status)) { self.io_error_ = status; return AVERROR_EXTERNAL; }
        self.byte_position_ += done;
        return static_cast<int>(done);
    }
    static int64_t SeekIo(void* opaque, int64_t offset, int origin) noexcept {
        auto& self = *static_cast<Document*>(opaque);
        if (origin == AVSEEK_SIZE) return static_cast<int64_t>(self.size_);
        origin &= ~AVSEEK_FORCE;
        const int64_t base = origin == SEEK_SET ? 0 : origin == SEEK_CUR ?
            static_cast<int64_t>(self.byte_position_) : origin == SEEK_END ? static_cast<int64_t>(self.size_) : -1;
        if (base < 0 || (offset > 0 && base > INT64_MAX - offset) ||
            (offset < 0 && offset < -base)) return AVERROR(EINVAL);
        const int64_t target = base + offset;
        if (target < 0 || static_cast<uint64_t>(target) > self.size_) return AVERROR(EINVAL);
        LARGE_INTEGER at{}; at.QuadPart = target;
        const HRESULT result = self.input_->Seek(at, STREAM_SEEK_SET, nullptr);
        if (FAILED(result)) { self.io_error_ = result; return AVERROR_EXTERNAL; }
        self.byte_position_ = static_cast<uint64_t>(target);
        return target;
    }
    void AvCheck(int result) const {
        Check(io_error_);
        if (result < 0) throw Failure{result == AVERROR(ENOMEM) ? E_OUTOFMEMORY :
            result == AVERROR_DECODER_NOT_FOUND || result == AVERROR_PATCHWELCOME ?
            HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) : HRESULT_FROM_WIN32(ERROR_INVALID_DATA)};
    }
    // Validate declared top-level bounds before the demuxer can interpret a
    // physically truncated finite Segment as ordinary end of input.
    void ValidateSegment() {
        std::array<BYTE, 16> bytes{};
        const auto vint = [&](bool id, uint64_t& at, bool& unknown) {
            Require(at < size_);
            AvCheck(static_cast<int>(SeekIo(this, static_cast<int64_t>(at), SEEK_SET) < 0 ? AVERROR_EXTERNAL : 0));
            AvCheck(ReadIo(this, bytes.data(), 1));
            const BYTE first = bytes[0]; Require(first != 0);
            unsigned length = 1, mask = 128;
            while (!(first & mask)) { ++length; mask >>= 1; }
            Require(length <= (id ? 4u : 8u) && length <= size_ - at);
            uint64_t value = id ? first : first & (mask - 1);
            if (length > 1) { AvCheck(ReadIo(this, bytes.data(), static_cast<int>(length - 1))); }
            for (unsigned i = 0; i + 1 < length; ++i) value = (value << 8) | bytes[i];
            at += length;
            unknown = !id && value == ((uint64_t(1) << (length * 7)) - 1);
            return value;
        };
        uint64_t at{}; bool unknown{};
        Require(vint(true, at, unknown) == 0x1a45dfa3);
        const uint64_t header = vint(false, at, unknown);
        Require(!unknown && header <= size_ - at && header <= 4096);
        at += header;
        for (unsigned count = 0; count < 1024; ++count) {
            const auto id = vint(true, at, unknown);
            const auto length = vint(false, at, unknown);
            Require(unknown || length <= size_ - at);
            if (id == 0x18538067) return;
            Require(!unknown && (id == 0xec || id == 0xbf));
            at += length;
        }
        Require(false);
    }
    void Negotiate() {
        Require(frame_->sample_rate > 0 && frame_->sample_rate <= 768000 &&
            frame_->ch_layout.nb_channels >= 1 && frame_->ch_layout.nb_channels <= 32);
        AVChannelLayout layout{};
        AvCheck(av_channel_layout_copy(&layout, &frame_->ch_layout));
        if (layout.order == AV_CHANNEL_ORDER_UNSPEC && layout.nb_channels <= 2)
            av_channel_layout_default(&layout, layout.nb_channels);
        const bool positioned = layout.order == AV_CHANNEL_ORDER_NATIVE && layout.u.mask <= MAXDWORD;
        // Matroska PCM may specify only the number of channels. Preserve its
        // interleave and explicitly publish an unspecified mask, never invent
        // speaker positions for an unpositioned multichannel recording.
        const bool valid_layout = positioned || layout.order == AV_CHANNEL_ORDER_UNSPEC;
        const DWORD channel_mask = positioned ? static_cast<DWORD>(layout.u.mask) : 0;
        av_channel_layout_uninit(&layout);
        Require(valid_layout, HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
        const auto packed = av_get_packed_sample_fmt(static_cast<AVSampleFormat>(frame_->format));
        WORD tag = WAVE_FORMAT_PCM, bits{}, valid_bits{};
        switch (packed) {
        case AV_SAMPLE_FMT_U8: bits = valid_bits = 8; break;
        case AV_SAMPLE_FMT_S16: bits = valid_bits = 16; break;
        case AV_SAMPLE_FMT_S32: bits = valid_bits = 32; break;
        case AV_SAMPLE_FMT_FLT: tag = WAVE_FORMAT_IEEE_FLOAT; bits = valid_bits = 32; break;
        case AV_SAMPLE_FMT_DBL: tag = WAVE_FORMAT_IEEE_FLOAT; bits = valid_bits = 64; break;
        default: Require(false, HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
        }
        if (tag == WAVE_FORMAT_PCM && decoder_->bits_per_raw_sample > 0 && decoder_->bits_per_raw_sample <= bits) {
            valid_bits = static_cast<WORD>(decoder_->bits_per_raw_sample);
            bits = static_cast<WORD>((valid_bits + 7) / 8 * 8);
        }
        const auto channels = static_cast<WORD>(frame_->ch_layout.nb_channels);
        const auto align = static_cast<WORD>(channels * (bits / 8));
        Require(uint64_t(frame_->sample_rate) * align <= MAXDWORD);
        const WAVEFORMATEX next{tag, channels, static_cast<DWORD>(frame_->sample_rate),
            static_cast<DWORD>(frame_->sample_rate) * align, align, bits, 0};
        if (sample_format_ >= 0) {
            Require(sample_format_ == frame_->format && mask == channel_mask && valid == valid_bits &&
                !memcmp(&wave, &next, sizeof(next)), HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
        } else {
            sample_format_ = frame_->format; wave = next; mask = channel_mask; valid = valid_bits;
        }
    }
    void QueueFrame() {
        Negotiate();
        Require(frame_->nb_samples > 0 && frame_->nb_samples <= 1048576);
        const auto* track = container_->streams[track_];
        int64_t start = decoded_position_;
        if (frame_->pts != AV_NOPTS_VALUE) {
            if (!origin_known_) { origin_ = frame_->pts; origin_known_ = true; }
            Require((origin_ <= 0 || frame_->pts >= INT64_MIN + origin_) &&
                (origin_ >= 0 || frame_->pts <= INT64_MAX + origin_));
            start = av_rescale_q(frame_->pts - origin_, track->time_base, AVRational{1, static_cast<int>(wave.nSamplesPerSec)});
            // Matroska timestamps are commonly rounded to milliseconds. Sample
            // counts remain authoritative inside that rounding tolerance.
            const int64_t tolerance = std::max<int64_t>(2, av_rescale_q(2, track->time_base,
                AVRational{1, static_cast<int>(wave.nSamplesPerSec)}));
            if (start >= decoded_position_ - tolerance && start <= decoded_position_ + tolerance)
                start = decoded_position_;
        }
        Require(start >= decoded_position_ && start <= INT64_MAX - frame_->nb_samples);
        queue_start_ = start;
        decoded_position_ = start + frame_->nb_samples;
        const size_t channels = wave.nChannels, output_bytes = wave.wBitsPerSample / 8;
        const size_t input_bytes = static_cast<size_t>(av_get_bytes_per_sample(static_cast<AVSampleFormat>(frame_->format)));
        Require(input_bytes >= output_bytes && uint64_t(frame_->nb_samples) * wave.nBlockAlign <= maximum_packet);
        queue_.resize(static_cast<size_t>(frame_->nb_samples) * wave.nBlockAlign);
        const bool planar = av_sample_fmt_is_planar(static_cast<AVSampleFormat>(frame_->format)) != 0;
        // Native integer decoders supply left-aligned S16/S32. Copy the high
        // container bytes, retaining non-byte valid bits without float math.
        // IEEE PCM is copied byte-for-byte (including -0 and subnormals).
        for (int sample = 0; sample < frame_->nb_samples; ++sample)
            for (size_t channel = 0; channel < channels; ++channel) {
                const BYTE* source = frame_->extended_data[planar ? channel : 0] +
                    (planar ? static_cast<size_t>(sample) : static_cast<size_t>(sample) * channels + channel) * input_bytes;
                memcpy(queue_.data() + (static_cast<size_t>(sample) * channels + channel) * output_bytes,
                    source + input_bytes - output_bytes, output_bytes);
            }
        queue_at_ = 0;
    }
    bool Fill() {
        queue_.clear(); queue_at_ = 0;
        for (;;) {
            if (decoded_end_) return false;
            const int received = avcodec_receive_frame(decoder_, frame_);
            if (received >= 0) {
                Require(!(frame_->flags & AV_FRAME_FLAG_CORRUPT) && !frame_->decode_error_flags);
                QueueFrame(); av_frame_unref(frame_); return true;
            }
            if (received == AVERROR_EOF) { decoded_end_ = true; return false; }
            if (received != AVERROR(EAGAIN)) AvCheck(received);
            if (demux_end_) {
                Require(!flush_sent_);
                AvCheck(avcodec_send_packet(decoder_, nullptr)); flush_sent_ = true; continue;
            }
            for (;;) {
                av_packet_unref(packet_);
                const int result = av_read_frame(container_, packet_);
                Check(io_error_);
                if (result == AVERROR_EOF) { demux_end_ = true; break; }
                AvCheck(result);
                if (packet_->stream_index != track_) continue;
                Require(packet_->size > 0 && static_cast<size_t>(packet_->size) <= maximum_packet &&
                    !(packet_->flags & AV_PKT_FLAG_CORRUPT));
                AvCheck(avcodec_send_packet(decoder_, packet_));
                av_packet_unref(packet_); break;
            }
        }
    }
    void AddMetadata(AVDictionary* dictionary) {
        const AVDictionaryEntry* item{};
        while ((item = av_dict_get(dictionary, "", item, AV_DICT_IGNORE_SUFFIX))) {
            if (!strcmp(item->key, "ttplayer_track_enabled") || !strcmp(item->key, "enc_key_id")) continue;
            if (strlen(item->key) > 32768 || strlen(item->value) > 1024 * 1024 || tags.size() >= 65536) continue;
            const auto key = core::Utf8ToWide(item->key), value = core::Utf8ToWide(item->value);
            auto found = std::find_if(tags.begin(), tags.end(), [&](const auto& field) { return !_wcsicmp(field.first.c_str(), key.c_str()); });
            if (found != tags.end()) found->second = value;
            else tags.emplace_back(key, value);
        }
    }
    void MetadataAndCovers() {
        AddMetadata(container_->metadata);
        AddMetadata(container_->streams[track_]->metadata);
        tags.emplace_back(L"AudioTrack", std::to_wstring(track_ + 1));
        for (unsigned i = 0; i < container_->nb_streams && covers.size() < 64; ++i) {
            const auto* stream = container_->streams[i];
            const auto* parameters = stream->codecpar;
            const char* mime = Value(stream->metadata, "mimetype");
            const bool png = !strcmp(mime, "image/png") || parameters->codec_id == AV_CODEC_ID_PNG;
            const bool jpeg = !strcmp(mime, "image/jpeg") || parameters->codec_id == AV_CODEC_ID_MJPEG;
            if (!png && !jpeg) continue;
            const BYTE* data{}; size_t bytes{};
            if (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) {
                data = stream->attached_pic.data; bytes = static_cast<size_t>(std::max(0, stream->attached_pic.size));
            } else if (parameters->codec_type == AVMEDIA_TYPE_ATTACHMENT) {
                data = parameters->extradata; bytes = static_cast<size_t>(std::max(0, parameters->extradata_size));
            }
            if (!data || !bytes || bytes > maximum_packet) continue;
            Cover cover;
            cover.mime = png ? L"image/png" : L"image/jpeg";
            cover.name = core::Utf8ToWide(Value(stream->metadata, "filename"));
            cover.description = core::Utf8ToWide(Value(stream->metadata, "title"));
            auto name = cover.name; std::transform(name.begin(), name.end(), name.begin(), towlower);
            cover.front = name == L"cover.jpg" || name == L"cover.png" || name.find(L"front") != name.npos;
            cover.bytes.assign(data, data + bytes); covers.push_back(std::move(cover));
        }
        std::stable_sort(covers.begin(), covers.end(), [](const Cover& a, const Cover& b) { return a.front > b.front; });
    }

public:
    WAVEFORMATEX wave{};
    DWORD mask{}, bitrate{}, duration_ms{};
    WORD valid{};
    std::wstring codec_name;
    std::string codec;
    std::vector<std::pair<std::wstring, std::wstring>> tags;
    std::vector<Cover> covers;
    ~Document() {
        avcodec_free_context(&decoder_);
        av_frame_free(&frame_); av_packet_free(&packet_);
        avformat_close_input(&container_);
        if (io_) { av_freep(&io_->buffer); avio_context_free(&io_); }
        if (event_) CloseHandle(event_);
    }
    void Open(IStream* input) {
        Require(input, E_POINTER); Require(frame_ && packet_ && event_, E_OUTOFMEMORY);
        input_ = input;
        input->QueryInterface(async_read_id, reinterpret_cast<void**>(async_.GetAddressOf()));
        LARGE_INTEGER zero{}; ULARGE_INTEGER size{};
        Check(input->Seek(zero, STREAM_SEEK_END, &size));
        size_ = size.QuadPart; Require(size_ >= 16 && size_ <= INT64_MAX);
        Check(input->Seek(zero, STREAM_SEEK_SET, nullptr));
        ttp::format::Snapshot prefix;
        Check(ttp::format::ReadSnapshot(input, prefix));
        const auto identified = ttp::format::Identify(prefix.request, ttp::format::Matroska);
        Require(identified.family == ttp::format::Matroska, HRESULT_FROM_WIN32(ERROR_BAD_FORMAT));
        ValidateSegment();
        Require(SeekIo(this, 0, SEEK_SET) == 0);
        auto* storage = static_cast<BYTE*>(av_malloc(32768)); Require(storage, E_OUTOFMEMORY);
        io_ = avio_alloc_context(storage, 32768, 0, this, ReadIo, nullptr, SeekIo);
        if (!io_) { av_free(storage); throw Failure{E_OUTOFMEMORY}; }
        container_ = avformat_alloc_context(); Require(container_, E_OUTOFMEMORY);
        container_->pb = io_; container_->flags |= AVFMT_FLAG_CUSTOM_IO;
        container_->max_streams = 256; container_->max_index_size = 8 * 1024 * 1024;
        container_->error_recognition = AV_EF_CRCCHECK | AV_EF_BITSTREAM | AV_EF_EXPLODE;
        AvCheck(avformat_open_input(&container_, nullptr, av_find_input_format("matroska"), nullptr));
        for (unsigned i = 0; i < container_->nb_streams; ++i) {
            auto* stream = container_->streams[i];
            const auto* parameters = stream->codecpar;
            if (parameters->codec_type != AVMEDIA_TYPE_AUDIO || !Supported(parameters->codec_id) || !SupportedPcmDepth(parameters) ||
                !strcmp(Value(stream->metadata, "ttplayer_track_enabled"), "0") ||
                *Value(stream->metadata, "enc_key_id")) continue;
            if (track_ < 0) track_ = static_cast<int>(i);
            if (stream->disposition & AV_DISPOSITION_DEFAULT) { track_ = static_cast<int>(i); break; }
        }
        Require(track_ >= 0, HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
        auto* selected = container_->streams[track_];
        const auto* parameters = selected->codecpar;
        Require(parameters->extradata_size >= 0 && static_cast<size_t>(parameters->extradata_size) <= maximum_packet);
        for (unsigned i = 0; i < container_->nb_streams; ++i)
            container_->streams[i]->discard = static_cast<int>(i) == track_ ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
        decoder_ = avcodec_alloc_context3(avcodec_find_decoder(parameters->codec_id)); Require(decoder_, E_OUTOFMEMORY);
        AvCheck(avcodec_parameters_to_context(decoder_, parameters));
        decoder_->pkt_timebase = selected->time_base;
        decoder_->thread_count = 1;
        decoder_->max_samples = 1048576;
        decoder_->err_recognition = AV_EF_CRCCHECK | AV_EF_BITSTREAM | AV_EF_EXPLODE;
        decoder_->error_concealment = 0;
        AvCheck(avcodec_open2(decoder_, nullptr, nullptr));
        Require(Fill()); // Negotiate the real HD rate/layout before publishing PCM.
        codec = avcodec_get_name(parameters->codec_id);
        const char* profile = avcodec_profile_name(parameters->codec_id, decoder_->profile);
        codec_name = core::Utf8ToWide(codec) + L"|Matroska / " + core::Utf8ToWide(profile ? profile : codec.c_str());
        if (parameters->bit_rate > 0) bitrate = static_cast<DWORD>(std::min<int64_t>(MAXDWORD, parameters->bit_rate));
        if (container_->duration > 0)
            duration_ms = static_cast<DWORD>(std::clamp<int64_t>(av_rescale_q(container_->duration, AV_TIME_BASE_Q, AVRational{1, 1000}), 0, MAXDWORD));
        MetadataAndCovers();
    }
    void Read(BYTE* output, size_t capacity, size_t& used) {
        used = 0;
        while (used < capacity) {
            if (queue_at_ == queue_.size() && !Fill()) break;
            if (position_ < queue_start_) {
                const size_t frames = static_cast<size_t>(std::min<int64_t>((capacity - used) / wave.nBlockAlign, queue_start_ - position_));
                const size_t bytes = frames * wave.nBlockAlign;
                memset(output + used, wave.wFormatTag == WAVE_FORMAT_PCM && wave.wBitsPerSample == 8 ? 128 : 0, bytes);
                position_ += frames; used += bytes;
            } else {
                const size_t bytes = std::min(capacity - used, queue_.size() - queue_at_);
                memcpy(output + used, queue_.data() + queue_at_, bytes);
                queue_at_ += bytes; used += bytes; position_ += bytes / wave.nBlockAlign;
            }
        }
    }
    void DiscardTo(uint64_t target) {
        std::vector<BYTE> scratch(static_cast<size_t>(wave.nBlockAlign) * 4096);
        while (static_cast<uint64_t>(position_) < target) {
            const size_t frames = static_cast<size_t>(std::min<uint64_t>(4096, target - position_));
            size_t used{};
            Read(scratch.data(), frames * wave.nBlockAlign, used);
            if (!used) break;
        }
    }
    DWORD PositionMs() const { return static_cast<DWORD>(std::min<int64_t>(MAXDWORD, av_rescale(position_, 1000, wave.nSamplesPerSec))); }
    HRESULT Callback(IUnknown* callback) {
        ComPtr<IUnknown> receiver;
        if (FAILED(input_->QueryInterface(callback_id, reinterpret_cast<void**>(receiver.GetAddressOf())))) return S_OK;
        using Set = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, IUnknown*);
        auto** table = *reinterpret_cast<void***>(receiver.Get());
        return reinterpret_cast<Set>(table[4])(receiver.Get(), callback);
    }
};

class MatroskaReader final : public Reader, public Metadata, public Thumbnail, public ttp::format::SourceInfo {
    LONG refs_{1};
    std::unique_ptr<Document> document_;
    ComPtr<IStream> stream_;
    DWORD flags_{};
    HRESULT error_{S_OK};
    ttp::format::Result detected_;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** output) override {
        if (!output) return E_POINTER; *output = nullptr;
        if (IsEqualIID(id, IID_IUnknown) || IsEqualIID(id, reader_id)) *output = static_cast<Reader*>(this);
        else if (IsEqualIID(id, metadata_id)) *output = static_cast<Metadata*>(this);
        else if (IsEqualIID(id, thumbnail_id)) *output = static_cast<Thumbnail*>(this);
        else if (IsEqualIID(id, ttp::format::iid_info)) *output = static_cast<ttp::format::SourceInfo*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override { const auto refs = InterlockedDecrement(&refs_); if (!refs) delete this; return refs; }
    HRESULT STDMETHODCALLTYPE Open(IStream* stream, DWORD flags) override {
        return Protect([&] {
            Require(!document_, E_UNEXPECTED);
            auto document = std::make_unique<Document>(); document->Open(stream);
            document_ = std::move(document); stream_ = stream; flags_ = flags;
            detected_.family = ttp::format::Matroska; detected_.confidence = ttp::format::Opened;
            ttp::format::Copy(detected_.container, "matroska");
            ttp::format::Copy(detected_.codec, document_->codec.c_str());
            ttp::format::Copy(detected_.extension, L".mkv"); ttp::format::Copy(detected_.aliases, L".mka");
            ttp::format::Copy(detected_.display, L"Matroska");
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE GetFormatInfo(ttp::format::Result* output) override {
        if (!output) return E_POINTER;
        if (output->size != sizeof(*output) || output->version != ttp::format::abi_version) return E_INVALIDARG;
        if (!document_) return E_UNEXPECTED;
        *output = detected_; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Capabilities(DWORD* output) override { if (!output) return E_POINTER; *output = document_ ? 2 : 0; return document_ ? S_OK : E_UNEXPECTED; }
    HRESULT STDMETHODCALLTYPE Duration(DWORD* output) override { if (!output) return E_POINTER; *output = document_ ? document_->duration_ms : 0; return document_ ? S_OK : E_UNEXPECTED; }
    HRESULT STDMETHODCALLTYPE Format(WAVEFORMATEX** output) override {
        if (!output) return E_POINTER; *output = nullptr; if (!document_) return E_UNEXPECTED;
        return ttp::pcm::copy_format(document_->wave, document_->mask, document_->valid, output);
    }
    HRESULT STDMETHODCALLTYPE BufferSize(DWORD* output) override { if (!output) return E_POINTER; *output = document_ ? document_->wave.nBlockAlign * 4096U : 0; return document_ ? S_OK : E_UNEXPECTED; }
    HRESULT STDMETHODCALLTYPE CodecName(wchar_t** output) override { return document_ ? Text(document_->codec_name, output) : E_UNEXPECTED; }
    HRESULT STDMETHODCALLTYPE Bitrate(DWORD* output) override { if (!output) return E_POINTER; *output = document_ ? document_->bitrate : 0; return document_ ? S_OK : E_UNEXPECTED; }
    HRESULT STDMETHODCALLTYPE BitDepth(WORD* output) override { if (!output) return E_POINTER; *output = document_ ? document_->valid : 0; return document_ ? S_OK : E_UNEXPECTED; }
    HRESULT STDMETHODCALLTYPE SetCallback(IUnknown* callback) override { return document_ ? document_->Callback(callback) : E_UNEXPECTED; }
    HRESULT STDMETHODCALLTYPE Start() override { return document_ ? S_OK : E_UNEXPECTED; }
    HRESULT STDMETHODCALLTYPE Stop() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Read(Buffer* buffer) override {
        if (!buffer) return E_POINTER;
        return Protect([&] {
            Check(buffer->SetLength(0)); Require(document_ && (flags_ & 1), E_UNEXPECTED); Check(error_);
            DWORD capacity{}, length{}; BYTE* output{};
            Check(buffer->Capacity(&capacity)); Check(buffer->Data(&output, &length));
            Require(output, E_POINTER); Require(capacity >= document_->wave.nBlockAlign, HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));
            capacity -= capacity % document_->wave.nBlockAlign;
            size_t used{};
            error_ = Protect([&] { document_->Read(output, capacity, used); return S_OK; });
            Check(buffer->SetLength(static_cast<DWORD>(used)));
            if (used) return S_OK; // Deliver valid preceding PCM, then sticky error.
            Check(error_); return S_FALSE;
        });
    }
    HRESULT STDMETHODCALLTYPE Seek(DWORD* milliseconds) override {
        if (!milliseconds) return E_POINTER;
        const HRESULT result = Protect([&] {
            Require(document_ && (flags_ & 1), E_UNEXPECTED);
            // Rebuild both demux and codec state, then replay/discard. This is
            // deliberately deterministic for Opus/Vorbis and MPEG reservoirs,
            // including files without Cues. No seek can leak old PCM or apply
            // CodecDelay a second time to an already-running decoder.
            auto next = std::make_unique<Document>(); next->Open(stream_.Get());
            Require(!memcmp(&next->wave, &document_->wave, sizeof(WAVEFORMATEX)) &&
                next->mask == document_->mask && next->valid == document_->valid);
            next->DiscardTo(uint64_t(*milliseconds) * next->wave.nSamplesPerSec / 1000);
            *milliseconds = next->PositionMs();
            document_ = std::move(next); error_ = S_OK; return S_OK;
        });
        // A failed replay has moved the shared IStream. Do not resume the old
        // buffered demuxer against that different physical cursor.
        if (FAILED(result)) error_ = result;
        return result;
    }
    HRESULT STDMETHODCALLTYPE Count(DWORD* output) override { if (!output) return E_POINTER; *output = document_ ? static_cast<DWORD>(document_->tags.size()) : 0; return document_ ? S_OK : E_UNEXPECTED; }
    HRESULT STDMETHODCALLTYPE At(DWORD index, wchar_t** key, wchar_t** value) override {
        if (!key || !value) return E_POINTER; *key = *value = nullptr;
        if (!document_) return E_UNEXPECTED; if (index >= document_->tags.size()) return E_INVALIDARG;
        const auto& field = document_->tags[index];
        const HRESULT status = Text(field.first, key); if (FAILED(status)) return status;
        const HRESULT second = Text(field.second, value); if (FAILED(second)) { CoTaskMemFree(*key); *key = nullptr; } return second;
    }
    HRESULT STDMETHODCALLTYPE Get(const char* key, wchar_t** value) override {
        if (!key || !value) return E_POINTER; *value = nullptr;
        return Protect([&] {
            Require(document_ != nullptr, E_UNEXPECTED); const auto name = core::Utf8ToWide(key);
            for (const auto& field : document_->tags) if (!_wcsicmp(field.first.c_str(), name.c_str())) return Text(field.second, value);
            return S_FALSE;
        });
    }
    HRESULT STDMETHODCALLTYPE Set(const char*, const wchar_t*) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE PictureCount(DWORD* output) override { if (!output) return E_POINTER; *output = document_ ? static_cast<DWORD>(document_->covers.size()) : 0; return document_ ? S_OK : E_UNEXPECTED; }
    HRESULT STDMETHODCALLTYPE MaximumBytes(DWORD* output) override { if (!output) return E_POINTER; *output = static_cast<DWORD>(maximum_packet); return S_OK; }
    HRESULT STDMETHODCALLTYPE MaximumCount(DWORD* output) override { if (!output) return E_POINTER; *output = 64; return S_OK; }
    HRESULT STDMETHODCALLTYPE PictureAt(DWORD index, const Picture** output) override {
        if (!output) return E_POINTER; *output = nullptr; if (!document_) return E_UNEXPECTED;
        if (index >= document_->covers.size()) return S_FALSE;
        auto& cover = document_->covers[index];
        cover.view = {sizeof(Picture), cover.mime.c_str(), cover.description.c_str(), static_cast<DWORD>(cover.bytes.size()), cover.bytes.data(), cover.front ? 3U : 0U};
        *output = &cover.view; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ReplacePicture(DWORD, const Picture*, DWORD) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE AddPicture(const Picture*) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE RemovePicture(DWORD) override { return STG_E_ACCESSDENIED; }
};

class MatroskaCreator final : public Creator, public ttp::format::ProbeSupport {
    LONG refs_{1};
    DWORD ProbeFamilies() const noexcept override { return ttp::format::Matroska; }
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** output) override {
        if (!output) return E_POINTER; *output = nullptr;
        if (IsEqualIID(id, IID_IUnknown) || IsEqualIID(id, creator_id)) *output = static_cast<Creator*>(this);
        else if (IsEqualIID(id, ttp::format::iid_probe)) *output = static_cast<ttp::format::Probe*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override { const auto refs = InterlockedDecrement(&refs_); if (!refs) delete this; return refs; }
    HRESULT STDMETHODCALLTYPE Create(void** output) override {
        if (!output) return E_POINTER; *output = nullptr;
        return Protect([&] { *output = static_cast<Reader*>(new MatroskaReader); return S_OK; });
    }
    HRESULT STDMETHODCALLTYPE Name(wchar_t** output) override { return Text(L"Matroska Reader", output); }
    HRESULT STDMETHODCALLTYPE Extensions(wchar_t** output) override { return Text(L"Matroska 音频文件 (*.mkv;*.mka)", output); }
};
constexpr GUID addin_id{0xeec6c534,0xfeba,0x421e,{0xaa,0x5d,0x10,0xac,0x66,0xf9,0x87,0x84}};
struct AddIn : IUnknown { virtual HRESULT STDMETHODCALLTYPE Enum(DWORD, GUID*, void**) = 0; };
class MatroskaAddIn final : public AddIn {
    LONG refs_{1};
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** output) override {
        if (!output) return E_POINTER; *output = nullptr;
        if (!IsEqualIID(id, IID_IUnknown) && !IsEqualIID(id, addin_id)) return E_NOINTERFACE;
        *output = static_cast<AddIn*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override { const auto refs = InterlockedDecrement(&refs_); if (!refs) delete this; return refs; }
    HRESULT STDMETHODCALLTYPE Enum(DWORD index, GUID* category, void** output) override {
        if (!output) return E_POINTER; *output = nullptr; if (!category) return E_POINTER;
        if (index != 0) return E_INVALIDARG;
        return Protect([&] { *category = creator_id; *output = static_cast<Creator*>(new MatroskaCreator); return S_OK; });
    }
};
} // namespace
} // namespace ttp::mkv

extern "C" HRESULT WINAPI ttpGetSoundAddIn(void** output) {
    if (!output) return E_POINTER; *output = nullptr;
    return ttp::mkv::Protect([&] { *output = static_cast<ttp::mkv::AddIn*>(new ttp::mkv::MatroskaAddIn); return S_OK; });
}
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
