#include "host.h"  // first: winsock2.h has to come before windows.h

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <codecapi.h>
#include <d3d11_4.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// Hardware encoders are asynchronous MFTs: they say when they want input and when output is
// ready through events. This callback collects those events so encoder_encode can wait on them
// with a timeout instead of blocking forever if the driver misbehaves.
class EventPump : public IMFAsyncCallback {
public:
    EventPump(IMFMediaEventGenerator *gen) : gen_(gen) {
        InitializeSRWLock(&lock_);
        signal_ = CreateEventW(NULL, FALSE, FALSE, NULL);
    }
    ~EventPump() { CloseHandle(signal_); }
    void start() { gen_->BeginGetEvent(this, NULL); }
    void stop() {
        AcquireSRWLockExclusive(&lock_);
        stopped_ = true;
        ReleaseSRWLockExclusive(&lock_);
    }
    // Takes one pending event of the given type; waits up to timeout_ms for it.
    bool take(MediaEventType type, DWORD timeout_ms, bool *failed) {
        ULONGLONG deadline = GetTickCount64() + timeout_ms;
        for (;;) {
            AcquireSRWLockExclusive(&lock_);
            int *count = type == METransformNeedInput ? &need_input_ : &have_output_;
            bool got = *count > 0;
            if (got) (*count)--;
            *failed = error_;
            ReleaseSRWLockExclusive(&lock_);
            if (got) return true;
            if (*failed) return false;
            ULONGLONG now = GetTickCount64();
            if (now >= deadline) return false;
            WaitForSingleObject(signal_, (DWORD)(deadline - now));
        }
    }

    STDMETHODIMP QueryInterface(REFIID iid, void **out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFAsyncCallback)) {
            *out = static_cast<IMFAsyncCallback *>(this);
            AddRef();
            return S_OK;
        }
        *out = NULL;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG n = InterlockedDecrement(&refs_);
        if (!n) delete this;
        return n;
    }
    STDMETHODIMP GetParameters(DWORD *, DWORD *) override { return E_NOTIMPL; }
    STDMETHODIMP Invoke(IMFAsyncResult *result) override {
        ComPtr<IMFMediaEvent> event;
        HRESULT hr = gen_->EndGetEvent(result, &event);
        AcquireSRWLockExclusive(&lock_);
        bool stopped = stopped_;
        if (FAILED(hr)) {
            error_ = !stopped;
        } else {
            MediaEventType type = MEUnknown;
            HRESULT status = S_OK;
            event->GetType(&type);
            event->GetStatus(&status);
            if (type == METransformNeedInput) need_input_++;
            else if (type == METransformHaveOutput) have_output_++;
            else if (type == MEError || FAILED(status)) error_ = true;
        }
        ReleaseSRWLockExclusive(&lock_);
        SetEvent(signal_);
        if (SUCCEEDED(hr) && !stopped) gen_->BeginGetEvent(this, NULL);
        return S_OK;
    }

private:
    LONG refs_ = 1;
    ComPtr<IMFMediaEventGenerator> gen_;  // a pending request keeps this alive until shutdown completes it
    SRWLOCK lock_;
    HANDLE signal_;
    int need_input_ = 0, have_output_ = 0;
    bool error_ = false, stopped_ = false;
};

#define POOL 3

struct Encoder {
    int codec, width, height, fps;
    char name[160];
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IMFTransform> mft;
    ComPtr<ICodecAPI> api;
    ComPtr<IMFMediaEventGenerator> events;
    ComPtr<IMFDXGIDeviceManager> manager;
    EventPump *pump;
    DWORD in_id, out_id;
    bool provides_samples;
    DWORD out_size;
    int64_t frame_no;

    // BGRA → NV12 (BT.709, limited range) on the GPU.
    ComPtr<ID3D11VideoDevice> vdev;
    ComPtr<ID3D11VideoContext> vctx;
    ComPtr<ID3D11VideoProcessorEnumerator> venum;
    ComPtr<ID3D11VideoProcessor> vp;
    ComPtr<ID3D11Texture2D> nv12[POOL];
    ComPtr<ID3D11VideoProcessorOutputView> out_view[POOL];
    int next_surface;
    ID3D11Texture2D *in_tex;           // the texture in_view was made for (not owned)
    ComPtr<ID3D11VideoProcessorInputView> in_view;

    uint8_t *sets_buf, *au_buf;
    size_t cap;
};

static void set_u32(ICodecAPI *api, const GUID &key, UINT32 v) {
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_UI4;
    var.ulVal = v;
    api->SetValue(&key, &var);
}

static void set_bool(ICodecAPI *api, const GUID &key, bool v) {
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_BOOL;
    var.boolVal = v ? VARIANT_TRUE : VARIANT_FALSE;
    api->SetValue(&key, &var);
}

static bool make_converter(Encoder *e, char *err, size_t err_cap) {
    if (FAILED(e->device.As(&e->vdev)) || FAILED(e->context.As(&e->vctx))) {
        snprintf(err, err_cap, "the GPU has no video processor");
        return false;
    }
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate = { (UINT)e->fps, 1 };
    cd.InputWidth = e->width;
    cd.InputHeight = e->height;
    cd.OutputFrameRate = { (UINT)e->fps, 1 };
    cd.OutputWidth = e->width;
    cd.OutputHeight = e->height;
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    if (FAILED(e->vdev->CreateVideoProcessorEnumerator(&cd, &e->venum)) ||
        FAILED(e->vdev->CreateVideoProcessor(e->venum.Get(), 0, &e->vp))) {
        snprintf(err, err_cap, "can't create a BGRA → NV12 video processor");
        return false;
    }
    for (int i = 0; i < POOL; i++) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = e->width;
        td.Height = e->height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC vd = {};
        vd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        if (FAILED(e->device->CreateTexture2D(&td, NULL, &e->nv12[i])) ||
            FAILED(e->vdev->CreateVideoProcessorOutputView(e->nv12[i].Get(), e->venum.Get(), &vd, &e->out_view[i]))) {
            snprintf(err, err_cap, "can't create NV12 surfaces");
            return false;
        }
    }
    // Screen pixels are full-range sRGB; the stream is BT.709 limited range, and says so.
    ComPtr<ID3D11VideoContext1> vctx1;
    if (SUCCEEDED(e->vctx.As(&vctx1))) {
        vctx1->VideoProcessorSetStreamColorSpace1(e->vp.Get(), 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        vctx1->VideoProcessorSetOutputColorSpace1(e->vp.Get(), DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
    } else {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in = {}, out = {};
        in.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
        out.YCbCr_Matrix = 1;  // BT.709
        out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        e->vctx->VideoProcessorSetStreamColorSpace(e->vp.Get(), 0, &in);
        e->vctx->VideoProcessorSetOutputColorSpace(e->vp.Get(), &out);
    }
    e->vctx->VideoProcessorSetStreamAutoProcessingMode(e->vp.Get(), 0, FALSE);
    e->vctx->VideoProcessorSetStreamFrameFormat(e->vp.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    return true;
}

static ComPtr<IMFMediaType> video_type(Encoder *e, const GUID &subtype, int bitrate) {
    ComPtr<IMFMediaType> t;
    MFCreateMediaType(&t);
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, subtype);
    MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, e->width, e->height);
    MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, e->fps, 1);
    MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    t->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    t->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    t->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    if (bitrate) t->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    return t;
}

// Picks the hardware encoder on the capture GPU, so frames stay in its memory. MFTEnum2 filters
// by adapter; MFTEnumEx would list every GPU's encoders without saying which is which.
static ComPtr<IMFActivate> find_encoder(int codec, LUID adapter, char *name, size_t name_cap) {
    MFT_REGISTER_TYPE_INFO in = { MFMediaType_Video, MFVideoFormat_NV12 };
    MFT_REGISTER_TYPE_INFO out = { MFMediaType_Video, codec == CODEC_HEVC ? MFVideoFormat_HEVC : MFVideoFormat_H264 };
    IMFActivate **list = NULL;
    UINT32 count = 0;
    ComPtr<IMFActivate> pick;
    ComPtr<IMFAttributes> filter;
    if (FAILED(MFCreateAttributes(&filter, 1)) ||
        FAILED(filter->SetBlob(MFT_ENUM_ADAPTER_LUID, (const UINT8 *)&adapter, sizeof adapter)) ||
        FAILED(MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &in, &out,
                        filter.Get(), &list, &count)))
        return pick;
    if (count) {
        pick = list[0];
        wchar_t *friendly = NULL;
        UINT32 len = 0;
        if (SUCCEEDED(pick->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &friendly, &len))) {
            snprintf(name, name_cap, "%ls", friendly);
            CoTaskMemFree(friendly);
        }
    }
    for (UINT32 i = 0; i < count; i++) list[i]->Release();
    CoTaskMemFree(list);
    return pick;
}

Encoder *encoder_open(ID3D11Device *device, LUID adapter, int codec, int width, int height, int fps, int bitrate,
                      char *err, size_t err_cap) {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, [](PINIT_ONCE, PVOID, PVOID *) -> BOOL { return SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)); },
                        NULL, NULL);
    const char *codec_name = codec == CODEC_HEVC ? "HEVC" : "H.264";
    if (width % 2 || height % 2) {
        snprintf(err, err_cap, "%dx%d is odd-sized; 4:2:0 video needs even dimensions", width, height);
        return NULL;
    }
    Encoder *e = new Encoder();
    e->codec = codec;
    e->width = width;
    e->height = height;
    e->fps = fps;
    e->device = device;
    device->GetImmediateContext(&e->context);

    char mft_name[96] = "hardware encoder";
    ComPtr<IMFActivate> activate = find_encoder(codec, adapter, mft_name, sizeof mft_name);
    HRESULT hr = E_FAIL;
    ComPtr<IMFAttributes> attrs;
    UINT token = 0;
    if (!activate) {
        snprintf(err, err_cap, "no %s hardware encoder on this GPU", codec_name);
        goto fail;
    }
    if (FAILED(hr = activate->ActivateObject(IID_PPV_ARGS(&e->mft)))) {
        snprintf(err, err_cap, "can't start the %s encoder (0x%08lx)", codec_name, hr);
        goto fail;
    }
    e->mft->GetAttributes(&attrs);
    if (attrs) {
        attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    }
    if (FAILED(e->mft->GetStreamIDs(1, &e->in_id, 1, &e->out_id))) e->in_id = e->out_id = 0;

    MFCreateDXGIDeviceManager(&token, &e->manager);
    if (!e->manager || FAILED(e->manager->ResetDevice(device, token)) ||
        FAILED(hr = e->mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)e->manager.Get()))) {
        snprintf(err, err_cap, "the %s encoder can't use the capture GPU (0x%08lx)", codec_name, hr);
        goto fail;
    }

    // Rate control has to be set before the output type: CBR, low-latency, no B-frames, and
    // keyframes only on demand (periodic ones cause bitrate spikes).
    if (SUCCEEDED(e->mft.As(&e->api))) {
        set_u32(e->api.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
        set_u32(e->api.Get(), CODECAPI_AVEncCommonMeanBitRate, bitrate);
        set_u32(e->api.Get(), CODECAPI_AVEncCommonBufferSize, bitrate / 4);  // caps bursts at ~250 ms of link time
        set_bool(e->api.Get(), CODECAPI_AVLowLatencyMode, true);
        set_u32(e->api.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        set_u32(e->api.Get(), CODECAPI_AVEncMPVGOPSize, 0x7FFFFFFF);
        set_u32(e->api.Get(), CODECAPI_AVEncCommonQualityVsSpeed, 50);
    }
    {
        ComPtr<IMFMediaType> out = video_type(e, codec == CODEC_HEVC ? MFVideoFormat_HEVC : MFVideoFormat_H264, bitrate);
        out->SetUINT32(MF_MT_MPEG2_PROFILE, codec == CODEC_HEVC ? (UINT32)eAVEncH265VProfile_Main_420_8
                                                                : (UINT32)eAVEncH264VProfile_Main);
        if (FAILED(hr = e->mft->SetOutputType(e->out_id, out.Get(), 0))) {
            snprintf(err, err_cap, "the %s encoder won't take %dx%d @ %d fps (0x%08lx)", codec_name, width, height, fps, hr);
            goto fail;
        }
        ComPtr<IMFMediaType> in = video_type(e, MFVideoFormat_NV12, 0);
        if (FAILED(hr = e->mft->SetInputType(e->in_id, in.Get(), 0))) {
            snprintf(err, err_cap, "the %s encoder won't take NV12 input (0x%08lx)", codec_name, hr);
            goto fail;
        }
    }
    {
        MFT_OUTPUT_STREAM_INFO info = {};
        e->mft->GetOutputStreamInfo(e->out_id, &info);
        e->provides_samples = info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES);
        e->out_size = info.cbSize ? info.cbSize : width * height;
    }
    if (!make_converter(e, err, err_cap)) goto fail;

    if (FAILED(e->mft.As(&e->events))) {
        snprintf(err, err_cap, "the %s encoder isn't asynchronous", codec_name);
        goto fail;
    }
    e->pump = new EventPump(e->events.Get());
    e->pump->start();
    e->mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    e->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    e->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    snprintf(e->name, sizeof e->name, "%s (%s)", codec_name, mft_name);
    return e;

fail:
    encoder_close(e);
    return NULL;
}

void encoder_close(Encoder *e) {
    if (!e) return;
    if (e->mft) {
        e->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        e->mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }
    if (e->pump) {
        e->pump->stop();
        e->pump->Release();
    }
    ComPtr<IMFShutdown> shutdown;
    if (e->mft && SUCCEEDED(e->mft.As(&shutdown))) shutdown->Shutdown();
    free(e->sets_buf);
    free(e->au_buf);
    delete e;
}

int encoder_codec(Encoder *e) { return e->codec; }
const char *encoder_name(Encoder *e) { return e->name; }

void encoder_set_bitrate(Encoder *e, int bitrate) {
    if (!e->api) return;
    set_u32(e->api.Get(), CODECAPI_AVEncCommonMeanBitRate, bitrate);
    set_u32(e->api.Get(), CODECAPI_AVEncCommonBufferSize, bitrate / 4);
}

static bool is_parameter_set(int codec, uint8_t header) {
    if (codec == CODEC_HEVC) {
        int t = (header >> 1) & 0x3F;
        return t == 32 || t == 33 || t == 34;   // VPS SPS PPS
    }
    int t = header & 0x1F;
    return t == 7 || t == 8;                     // SPS PPS
}

static bool is_delimiter(int codec, uint8_t header) {
    return codec == CODEC_HEVC ? ((header >> 1) & 0x3F) == 35 : (header & 0x1F) == 9;
}

/// Annex B → parameter sets + length-prefixed access unit, the way VideoToolbox hands them over.
/// Parameter sets from `extra` (the output type's sequence header) are used when a keyframe
/// doesn't carry its own.
static void split(Encoder *e, const uint8_t *d, size_t size, bool key, const uint8_t *extra, size_t extra_len,
                  FormatFn on_format, FrameFn on_frame, void *ctx) {
    if (size + extra_len + 64 > e->cap) {
        e->cap = (size + extra_len) * 2 + 64;
        e->sets_buf = (uint8_t *)realloc(e->sets_buf, e->cap);
        e->au_buf = (uint8_t *)realloc(e->au_buf, e->cap);
    }
    const uint8_t *sets[16];
    uint32_t sizes[16];
    int nsets = 0;
    size_t sets_len = 0, au_len = 0;

    for (int pass = 0; pass < 2; pass++) {
        const uint8_t *p0 = pass == 0 ? d : extra, *end = p0 + (pass == 0 ? size : extra_len);
        if (pass == 1 && (!key || nsets || !extra)) break;
        const uint8_t *nal = NULL;
        for (const uint8_t *p = p0;;) {
            // Find the next start code (or the end), which also ends the current NAL.
            const uint8_t *next = p;
            while (next + 3 <= end && !(next[0] == 0 && next[1] == 0 && next[2] == 1)) next++;
            if (next + 3 > end) next = end;
            if (nal) {
                const uint8_t *nal_end = next;
                while (nal_end > nal && nal_end < end && nal_end[-1] == 0) nal_end--;  // zero_byte of a 4-byte code
                if (next == end) nal_end = end;
                size_t len = nal_end - nal;
                if (len > 0 && !is_delimiter(e->codec, nal[0])) {
                    if (is_parameter_set(e->codec, nal[0])) {
                        if (nsets < 16) {
                            memcpy(e->sets_buf + sets_len, nal, len);
                            sets[nsets] = e->sets_buf + sets_len;
                            sizes[nsets++] = (uint32_t)len;
                            sets_len += len;
                        }
                    } else if (pass == 0) {
                        put_u32(e->au_buf + au_len, (uint32_t)len);
                        memcpy(e->au_buf + au_len + 4, nal, len);
                        au_len += 4 + len;
                    }
                }
            }
            if (next == end) break;
            nal = next + 3;
            p = nal;
        }
    }
    if (key && nsets) on_format(ctx, e->codec, sets, sizes, nsets);
    if (au_len) on_frame(ctx, key, e->au_buf, au_len);
}

static bool drain_output(Encoder *e, FormatFn on_format, FrameFn on_frame, void *ctx) {
    for (int attempt = 0; attempt < 4; attempt++) {
        MFT_OUTPUT_DATA_BUFFER out = {};
        out.dwStreamID = e->out_id;
        ComPtr<IMFSample> own;
        if (!e->provides_samples) {
            ComPtr<IMFMediaBuffer> buf;
            MFCreateSample(&own);
            MFCreateMemoryBuffer(e->out_size, &buf);
            own->AddBuffer(buf.Get());
            out.pSample = own.Get();
        }
        DWORD status = 0;
        HRESULT hr = e->mft->ProcessOutput(0, 1, &out, &status);
        if (out.pEvents) out.pEvents->Release();
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            // The encoder renegotiated its output type; take what it offers and ask again.
            ComPtr<IMFMediaType> t;
            if (SUCCEEDED(e->mft->GetOutputAvailableType(e->out_id, 0, &t))) e->mft->SetOutputType(e->out_id, t.Get(), 0);
            continue;
        }
        if (FAILED(hr)) return false;
        IMFSample *sample = out.pSample;
        ComPtr<IMFMediaBuffer> buf;
        BYTE *data = NULL;
        DWORD len = 0;
        bool ok = SUCCEEDED(sample->ConvertToContiguousBuffer(&buf)) && SUCCEEDED(buf->Lock(&data, NULL, &len));
        if (ok) {
            bool key = MFGetAttributeUINT32(sample, MFSampleExtension_CleanPoint, FALSE) != FALSE;
            UINT8 *extra = NULL;
            UINT32 extra_len = 0;
            ComPtr<IMFMediaType> type;
            if (key && SUCCEEDED(e->mft->GetOutputCurrentType(e->out_id, &type)))
                type->GetAllocatedBlob(MF_MT_MPEG_SEQUENCE_HEADER, &extra, &extra_len);
            split(e, data, len, key, extra, extra_len, on_format, on_frame, ctx);
            CoTaskMemFree(extra);
            buf->Unlock();
        }
        if (e->provides_samples && sample) sample->Release();
        return ok;
    }
    return false;
}

bool encoder_encode(Encoder *e, ID3D11Texture2D *bgra, bool keyframe, FormatFn on_format, FrameFn on_frame,
                    void *ctx) {
    if (!bgra) return false;
    if (bgra != e->in_tex) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC vd = {};
        vd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        e->in_view.Reset();
        if (FAILED(e->vdev->CreateVideoProcessorInputView(bgra, e->venum.Get(), &vd, &e->in_view))) return false;
        e->in_tex = bgra;
    }
    int i = e->next_surface;
    e->next_surface = (i + 1) % POOL;
    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = e->in_view.Get();
    if (FAILED(e->vctx->VideoProcessorBlt(e->vp.Get(), e->out_view[i].Get(), 0, 1, &stream))) return false;

    ComPtr<IMFSample> sample;
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(MFCreateSample(&sample)) ||
        FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), e->nv12[i].Get(), 0, FALSE, &buffer)))
        return false;
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(e->frame_no * 10000000ll / e->fps);
    sample->SetSampleDuration(10000000ll / e->fps);
    e->frame_no++;

    bool failed = false;
    if (!e->pump->take(METransformNeedInput, 1000, &failed)) {
        host_log("encoder: %s waiting for input", failed ? "error" : "timed out");
        return false;
    }
    if (keyframe && e->api) set_u32(e->api.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
    HRESULT hr = e->mft->ProcessInput(e->in_id, sample.Get(), 0);
    if (FAILED(hr)) {
        host_log("encoder: input rejected (0x%08lx)", hr);
        return false;
    }
    // Low-latency mode: one frame in, one frame out.
    if (!e->pump->take(METransformHaveOutput, 1000, &failed)) {
        host_log("encoder: %s waiting for output", failed ? "error" : "timed out");
        return false;
    }
    return drain_output(e, on_format, on_frame, ctx);
}
