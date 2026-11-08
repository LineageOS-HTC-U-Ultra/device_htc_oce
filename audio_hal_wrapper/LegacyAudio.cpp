/* SPDX-License-Identifier: Apache-2.0 */
#define LOG_TAG "OceLegacyAudio"
#include <hardware/audio.h>
#include <log/log.h>
#include <dlfcn.h>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <new>

// The HTC blob implements the C Audio HAL v2.0 ABI. Fields added to newer
// stream structs are not part of that ABI; their offsets in blob-owned streams
// may contain private data, not callbacks. Wrap each backend stream in a
// zero-initialized current struct and forward callbacks using the original
// backend stream pointer.
namespace {
struct Input { audio_stream_in_t api{}; audio_stream_in_t* legacy; };
struct Output { audio_stream_out_t api{}; audio_stream_out_t* legacy; };
struct Device { audio_hw_device_t api{}; audio_hw_device_t* legacy; };

template<class P> audio_stream* legacyCommon(const audio_stream* s) {
    return &reinterpret_cast<const P*>(s)->legacy->common;
}
audio_hw_device_t* legacyDevice(const audio_hw_device_t* d) {
    return reinterpret_cast<const Device*>(d)->legacy;
}
audio_stream_in_t* legacyInput(const audio_stream_in_t* s) {
    return reinterpret_cast<const Input*>(s)->legacy;
}
audio_stream_out_t* legacyOutput(const audio_stream_out_t* s) {
    return reinterpret_cast<const Output*>(s)->legacy;
}

template<class P> void common(P* p) {
    auto& c = p->api.common;
    c.get_sample_rate = [](const audio_stream* s) {
        auto* b = legacyCommon<P>(s); return b->get_sample_rate ? b->get_sample_rate(b) : 0u;
    };
    c.set_sample_rate = [](audio_stream* s, uint32_t rate) {
        auto* b = legacyCommon<P>(s); return b->set_sample_rate ? b->set_sample_rate(b, rate) : -ENOSYS;
    };
    c.get_buffer_size = [](const audio_stream* s) -> size_t {
        auto* b = legacyCommon<P>(s); return b->get_buffer_size ? b->get_buffer_size(b) : 0;
    };
    c.get_channels = [](const audio_stream* s) -> audio_channel_mask_t {
        auto* b = legacyCommon<P>(s); return b->get_channels ? b->get_channels(b) : AUDIO_CHANNEL_NONE;
    };
    c.get_format = [](const audio_stream* s) -> audio_format_t {
        auto* b = legacyCommon<P>(s); return b->get_format ? b->get_format(b) : AUDIO_FORMAT_INVALID;
    };
    c.set_format = [](audio_stream* s, audio_format_t format) {
        auto* b = legacyCommon<P>(s); return b->set_format ? b->set_format(b, format) : -ENOSYS;
    };
    c.standby = [](audio_stream* s) {
        auto* b = legacyCommon<P>(s); return b->standby ? b->standby(b) : -ENOSYS;
    };
    c.dump = [](const audio_stream* s, int fd) {
        auto* b = legacyCommon<P>(s); return b->dump ? b->dump(b, fd) : -ENOSYS;
    };
    c.get_device = [](const audio_stream* s) -> audio_devices_t {
        auto* b = legacyCommon<P>(s); return b->get_device ? b->get_device(b) : AUDIO_DEVICE_NONE;
    };
    c.set_device = [](audio_stream* s, audio_devices_t device) {
        auto* b = legacyCommon<P>(s); return b->set_device ? b->set_device(b, device) : -ENOSYS;
    };
    c.set_parameters = [](audio_stream* s, const char* kv) {
        auto* b = legacyCommon<P>(s); return b->set_parameters ? b->set_parameters(b, kv) : -ENOSYS;
    };
    c.get_parameters = [](const audio_stream* s, const char* keys) -> char* {
        auto* b = legacyCommon<P>(s); return b->get_parameters ? b->get_parameters(b, keys) : nullptr;
    };
    c.add_audio_effect = [](const audio_stream* s, effect_handle_t effect) {
        auto* b = legacyCommon<P>(s); return b->add_audio_effect ? b->add_audio_effect(b, effect) : -ENOSYS;
    };
    c.remove_audio_effect = [](const audio_stream* s, effect_handle_t effect) {
        auto* b = legacyCommon<P>(s); return b->remove_audio_effect ? b->remove_audio_effect(b, effect) : -ENOSYS;
    };
}

int openInput(audio_hw_device_t* d, audio_io_handle_t handle, audio_devices_t devices,
              audio_config* config, audio_stream_in_t** out, audio_input_flags_t flags,
              const char* address, audio_source_t source) {
    *out = nullptr;
    auto* b = legacyDevice(d);
    if (!b->open_input_stream || !b->close_input_stream) return -ENOSYS;
    audio_stream_in_t* real = nullptr;
    int status = b->open_input_stream(b, handle, devices, config, &real, flags, address, source);
    if (status != 0) return status;
    if (!real) return -EIO;
    auto* p = new (std::nothrow) Input;
    if (!p) { b->close_input_stream(b, real); return -ENOMEM; }
    p->legacy = real; common(p);
    p->api.set_gain = [](audio_stream_in_t* s, float gain) {
        auto* r = legacyInput(s); return r->set_gain ? r->set_gain(r, gain) : -ENOSYS;
    };
    p->api.read = [](audio_stream_in_t* s, void* buffer, size_t bytes) -> ssize_t {
        auto* r = legacyInput(s); return r->read ? r->read(r, buffer, bytes) : -ENOSYS;
    };
    p->api.get_input_frames_lost = [](audio_stream_in_t* s) {
        auto* r = legacyInput(s); return r->get_input_frames_lost ? r->get_input_frames_lost(r) : 0u;
    };
    p->api.get_capture_position = [](const audio_stream_in_t* s, int64_t* frames, int64_t* time) {
        auto* r = legacyInput(s); return r->get_capture_position ? r->get_capture_position(r, frames, time) : -ENOSYS;
    };
    // MMAP and microphone/metadata extensions were not present in this blob.
    *out = &p->api;
    return 0;
}

int openOutput(audio_hw_device_t* d, audio_io_handle_t handle, audio_devices_t devices,
               audio_output_flags_t flags, audio_config* config, audio_stream_out_t** out,
               const char* address) {
    *out = nullptr;
    auto* b = legacyDevice(d);
    if (!b->open_output_stream || !b->close_output_stream) return -ENOSYS;
    audio_stream_out_t* real = nullptr;
    int status = b->open_output_stream(b, handle, devices, flags, config, &real, address);
    if (status != 0) return status;
    if (!real) return -EIO;
    auto* p = new (std::nothrow) Output;
    if (!p) { b->close_output_stream(b, real); return -ENOMEM; }
    p->legacy = real; common(p);
    p->api.get_latency = [](const audio_stream_out_t* s) {
        auto* r = legacyOutput(s); return r->get_latency ? r->get_latency(r) : 0u;
    };
    p->api.set_volume = [](audio_stream_out_t* s, float left, float right) {
        auto* r = legacyOutput(s); return r->set_volume ? r->set_volume(r, left, right) : -ENOSYS;
    };
    p->api.write = [](audio_stream_out_t* s, const void* buffer, size_t bytes) -> ssize_t {
        auto* r = legacyOutput(s); return r->write ? r->write(r, buffer, bytes) : -ENOSYS;
    };
    p->api.get_render_position = [](const audio_stream_out_t* s, uint32_t* frames) {
        auto* r = legacyOutput(s); return r->get_render_position ? r->get_render_position(r, frames) : -ENOSYS;
    };
    p->api.get_next_write_timestamp = [](const audio_stream_out_t* s, int64_t* time) {
        auto* r = legacyOutput(s); return r->get_next_write_timestamp ? r->get_next_write_timestamp(r, time) : -ENOSYS;
    };
    if (real->set_callback) {
        p->api.set_callback = [](audio_stream_out_t* s, stream_callback_t callback, void* cookie) {
            auto* r = legacyOutput(s); return r->set_callback ? r->set_callback(r, callback, cookie) : -ENOSYS;
        };
    }
    if (real->pause) {
        p->api.pause = [](audio_stream_out_t* s) {
            auto* r = legacyOutput(s); return r->pause ? r->pause(r) : -ENOSYS;
        };
    }
    if (real->resume) {
        p->api.resume = [](audio_stream_out_t* s) {
            auto* r = legacyOutput(s); return r->resume ? r->resume(r) : -ENOSYS;
        };
    }
    if (real->drain) {
        p->api.drain = [](audio_stream_out_t* s, audio_drain_type_t type) {
            auto* r = legacyOutput(s); return r->drain ? r->drain(r, type) : -ENOSYS;
        };
    }
    p->api.flush = [](audio_stream_out_t* s) {
        auto* r = legacyOutput(s); return r->flush ? r->flush(r) : -ENOSYS;
    };
    p->api.get_presentation_position = [](const audio_stream_out_t* s, uint64_t* frames, timespec* time) {
        auto* r = legacyOutput(s); return r->get_presentation_position ? r->get_presentation_position(r, frames, time) : -ENOSYS;
    };
    *out = &p->api;
    return 0;
}

int wrapDevice(audio_hw_device_t* b, hw_device_t** out) {
    *out = nullptr;
    if (b->common.version != AUDIO_DEVICE_API_VERSION_2_0) {
        ALOGE("Unexpected HTC C audio API version %#x", b->common.version);
        b->common.close(&b->common); return -EINVAL;
    }
    auto* p = new (std::nothrow) Device;
    if (!p) { b->common.close(&b->common); return -ENOMEM; }
    p->legacy = b;
    auto& a = p->api;
    a.common = b->common;
    a.common.close = [](hw_device_t* d) {
        auto* p = reinterpret_cast<Device*>(d);
        int result = p->legacy->common.close(&p->legacy->common); delete p; return result;
    };
    a.get_supported_devices = [](const audio_hw_device_t* d) -> uint32_t {
        auto* r = legacyDevice(d); return r->get_supported_devices ? r->get_supported_devices(r) : 0u;
    };
    a.init_check = [](const audio_hw_device_t* d) {
        auto* r = legacyDevice(d); return r->init_check ? r->init_check(r) : -ENOSYS;
    };
    a.set_voice_volume = [](audio_hw_device_t* d, float v) {
        auto* r = legacyDevice(d); return r->set_voice_volume ? r->set_voice_volume(r, v) : -ENOSYS;
    };
    a.set_master_volume = [](audio_hw_device_t* d, float v) {
        auto* r = legacyDevice(d); return r->set_master_volume ? r->set_master_volume(r, v) : -ENOSYS;
    };
    a.get_master_volume = [](audio_hw_device_t* d, float* v) {
        auto* r = legacyDevice(d); return r->get_master_volume ? r->get_master_volume(r, v) : -ENOSYS;
    };
    a.set_mode = [](audio_hw_device_t* d, audio_mode_t mode) {
        auto* r = legacyDevice(d); return r->set_mode ? r->set_mode(r, mode) : -ENOSYS;
    };
    a.set_mic_mute = [](audio_hw_device_t* d, bool v) {
        auto* r = legacyDevice(d); return r->set_mic_mute ? r->set_mic_mute(r, v) : -ENOSYS;
    };
    a.get_mic_mute = [](const audio_hw_device_t* d, bool* v) {
        auto* r = legacyDevice(d); return r->get_mic_mute ? r->get_mic_mute(r, v) : -ENOSYS;
    };
    a.set_parameters = [](audio_hw_device_t* d, const char* kv) {
        auto* r = legacyDevice(d); return r->set_parameters ? r->set_parameters(r, kv) : -ENOSYS;
    };
    a.get_parameters = [](const audio_hw_device_t* d, const char* keys) -> char* {
        auto* r = legacyDevice(d); return r->get_parameters ? r->get_parameters(r, keys) : nullptr;
    };
    a.get_input_buffer_size = [](const audio_hw_device_t* d, const audio_config* c) -> size_t {
        auto* r = legacyDevice(d); return r->get_input_buffer_size ? r->get_input_buffer_size(r, c) : 0;
    };
    a.open_input_stream = openInput;
    a.close_input_stream = [](audio_hw_device_t* d, audio_stream_in_t* s) {
        auto* p = reinterpret_cast<Input*>(s); auto* r = legacyDevice(d);
        r->close_input_stream(r, p->legacy); delete p;
    };
    a.open_output_stream = openOutput;
    a.close_output_stream = [](audio_hw_device_t* d, audio_stream_out_t* s) {
        auto* p = reinterpret_cast<Output*>(s); auto* r = legacyDevice(d);
        r->close_output_stream(r, p->legacy); delete p;
    };
    a.dump = [](const audio_hw_device_t* d, int fd) {
        auto* r = legacyDevice(d); return r->dump ? r->dump(r, fd) : -ENOSYS;
    };
    a.set_master_mute = [](audio_hw_device_t* d, bool v) {
        auto* r = legacyDevice(d); return r->set_master_mute ? r->set_master_mute(r, v) : -ENOSYS;
    };
    a.get_master_mute = [](audio_hw_device_t* d, bool* v) {
        auto* r = legacyDevice(d); return r->get_master_mute ? r->get_master_mute(r, v) : -ENOSYS;
    };
    *out = &a.common;
    ALOGI("Opened HTC API2 through zero-initialized C ABI adapter");
    return 0;
}
int open(const hw_module_t*, const char* name, hw_device_t** out) {
    *out = nullptr;
    if (strcmp(name, AUDIO_HARDWARE_INTERFACE)) return -EINVAL;
    static std::once_flag once;
    static const hw_module_t* module = nullptr;
    std::call_once(once, [] {
        // Deliberately bypass property-based lookup so this adapter cannot load itself.
        void* handle = dlopen("/vendor/lib/hw/audio.primary.msm8996.so", RTLD_NOW | RTLD_LOCAL);
        if (!handle) { ALOGE("Cannot load HTC audio module: %s", dlerror()); return; }
        module = static_cast<const hw_module_t*>(dlsym(handle, HAL_MODULE_INFO_SYM_AS_STR));
        if (!module || !module->methods || !module->methods->open) {
            ALOGE("HTC module has no open entry"); module = nullptr;
        }
        // Retain the DSO for the process lifetime, as libhardware does.
    });
    if (!module) return -ENODEV;
    hw_device_t* raw = nullptr;
    int status = module->methods->open(module, name, &raw);
    if (status != 0) return status;
    if (!raw) return -EIO;
    return wrapDevice(reinterpret_cast<audio_hw_device_t*>(raw), out);
}
hw_module_methods_t methods = { .open = open };
} // namespace

extern "C" {
audio_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = AUDIO_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = AUDIO_HARDWARE_MODULE_ID,
        .name = "HTC U Ultra legacy C audio adapter",
        .author = "HTC U Ultra community",
        .methods = &methods,
    },
};
}
