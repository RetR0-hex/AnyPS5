#include "Ngs2Test.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

static uintptr_t Sampler(uintptr_t system, const std::vector<std::int16_t>& pcm, std::uint32_t repeats) {
    const auto voice = Voice(CreateRack(system, SCE_NGS2_RACK_ID_SAMPLER));
    Control(voice, SCE_NGS2_SAMPLER_VOICE_PARAM_SETUP, Ngs2SamplerVoiceSetupParam{{}, {SCE_NGS2_WAVEFORM_TYPE_PCM_I16L, 1, 48000, 0, 0, 0}});
    const Ngs2WaveformBlock block{0, pcm.size() * sizeof(std::int16_t), repeats, 0, static_cast<std::uint32_t>(pcm.size()), 0, 0x55};
    Control(voice, SCE_NGS2_SAMPLER_VOICE_PARAM_ADD_WAVEFORM_BLOCKS, Ngs2SamplerVoiceWaveformBlocksParam{{}, pcm.data(), 0, 1, &block});
    return voice;
}

static std::vector<std::uint32_t> callbackFlags;
static void APS5_VABI OnBlock(const Ngs2VoiceCallbackInfo* info) {
    Require(info->callback_data == 7 && info->user_data == 0x55);
    callbackFlags.push_back(info->flag);
}

static std::vector<std::int16_t> RenderI16(uintptr_t system) {
    std::vector<std::int16_t> out(Grain, -1);
    const Ngs2RenderBufferInfo info{out.data(), out.size() * sizeof(std::int16_t), SCE_NGS2_WAVEFORM_TYPE_PCM_I16L, 1};
    Require(sceNgs2SystemRender(system, &info, 1) == SCE_NGS2_OK);
    return out;
}

static void TestErrorsAndInfo() {
    Ngs2SystemOption option{};
    Require(sceNgs2SystemResetOption(&option) == SCE_NGS2_OK);
    Require(option.size == sizeof(option) && option.max_grain_samples == 512 && option.num_grain_samples == 256 && option.sample_rate == 48000);
    Require(sceNgs2SystemQueryBufferSize(&option, nullptr) == SCE_NGS2_ERROR_INVALID_OUT_ADDRESS);
    Require(sceNgs2RackQueryBufferSize(SCE_NGS2_RACK_ID_SAMPLER, nullptr, nullptr) == SCE_NGS2_ERROR_INVALID_OUT_ADDRESS);

    Ngs2SystemInfo info{};
    Require(sceNgs2SystemGetInfo(0x1234, &info, sizeof(info)) == SCE_NGS2_ERROR_INVALID_SYSTEM_HANDLE);
    Require(sceNgs2RackDestroy(0x1234, nullptr) == SCE_NGS2_ERROR_INVALID_RACK_HANDLE);

    const auto system = CreateSystem();
    CreateRack(system, SCE_NGS2_RACK_ID_SAMPLER);
    Require(sceNgs2SystemGetInfo(system, nullptr, sizeof(info)) == SCE_NGS2_ERROR_INVALID_OUT_ADDRESS);
    Require(sceNgs2SystemGetInfo(system, &info, sizeof(info) - 1) == SCE_NGS2_ERROR_INVALID_OUT_SIZE);
    Require(sceNgs2SystemGetInfo(system, &info, sizeof(info)) == SCE_NGS2_OK);
    Require(info.system_handle == system && info.uid != 0 && info.rack_count == 1 && info.sample_rate == 48000);
    Require(info.num_grain_samples == Grain && info.max_grain_samples == 512 && info.render_count == 0);

    Ngs2ContextBufferInfo released{};
    Require(sceNgs2SystemDestroy(system, &released) == SCE_NGS2_OK && released.host_buffer != nullptr);
    Require(sceNgs2SystemGetInfo(system, &info, sizeof(info)) == SCE_NGS2_ERROR_INVALID_SYSTEM_HANDLE);
}

static void TestPcmBlockEnd() {
    const auto system = CreateSystem();
    const auto master = Mastering(system, 1);
    std::vector<std::int16_t> pcm;
    for (int i = 0; i < 12; i++) pcm.push_back(static_cast<std::int16_t>(i * 1000 - 4000));
    const auto sampler = Sampler(system, pcm, 0);
    Patch(sampler, master);
    Control(sampler, SCE_NGS2_VOICE_PARAM_CALLBACK, Ngs2VoiceCallbackParam{{}, OnBlock, 7, SCE_NGS2_VOICE_CALLBACK_FLAG_BLOCK_END, 0});
    Require(Flags(sampler) == 0);
    Event(sampler, SCE_NGS2_VOICE_EVENT_PLAY);
    Require(Flags(sampler) == SCE_NGS2_VOICE_STATE_FLAG_INUSE);

    callbackFlags.clear();
    auto out = RenderI16(system);
    for (std::uint32_t i = 0; i < Grain; i++) Require(out[i] == pcm[i]);
    Require(Flags(sampler) == (SCE_NGS2_VOICE_STATE_FLAG_INUSE | SCE_NGS2_VOICE_STATE_FLAG_PLAYING) && callbackFlags.empty());

    Ngs2SamplerVoiceState state{};
    Require(sceNgs2VoiceGetState(sampler, &state.voice_state, sizeof(state) - 8) == SCE_NGS2_ERROR_INVALID_OUT_SIZE);
    Require(sceNgs2VoiceGetState(sampler, &state.voice_state, sizeof(state)) == SCE_NGS2_OK);
    Require(state.num_decoded_samples == Grain && state.decoded_data_size == Grain * 2 && state.user_data == 0x55);
    Require(state.waveform_data == pcm.data() + Grain);

    out = RenderI16(system);
    for (std::uint32_t i = 0; i < Grain; i++) Require(out[i] == (i < 4 ? pcm[Grain + i] : 0));
    Require(callbackFlags.size() == 1 && callbackFlags[0] == SCE_NGS2_VOICE_CALLBACK_FLAG_BLOCK_END);
    Require(Flags(sampler) == 0);
    Require(sceNgs2VoiceGetState(sampler, &state.voice_state, sizeof(state)) == SCE_NGS2_OK);
    Require(state.num_decoded_samples == pcm.size() && state.waveform_data == pcm.data() + pcm.size());
    Require(sceNgs2SystemDestroy(system, nullptr) == SCE_NGS2_OK);
}

static void TestPitchAndRepeat() {
    const auto system = CreateSystem();
    const auto master = Mastering(system, 1);
    const std::vector<std::int16_t> pcm{0, 1000, 2000, 3000};
    const auto sampler = Sampler(system, pcm, 1);
    Patch(sampler, master);
    Control(sampler, SCE_NGS2_SAMPLER_VOICE_PARAM_PITCH, Ngs2SamplerVoicePitchParam{{}, 0.5f});
    Control(sampler, SCE_NGS2_VOICE_PARAM_CALLBACK,
            Ngs2VoiceCallbackParam{{}, OnBlock, 7, SCE_NGS2_VOICE_CALLBACK_FLAG_BLOCK_END | SCE_NGS2_VOICE_CALLBACK_FLAG_BLOCK_REPEAT, 0});
    Event(sampler, SCE_NGS2_VOICE_EVENT_PLAY);

    callbackFlags.clear();
    const std::int16_t first[Grain] = {0, 500, 1000, 1500, 2000, 2500, 3000, 1500};
    auto out = RenderI16(system);
    for (std::uint32_t i = 0; i < Grain; i++) Require(out[i] == first[i]);
    Require(callbackFlags.size() == 1 && callbackFlags[0] == SCE_NGS2_VOICE_CALLBACK_FLAG_BLOCK_REPEAT);

    const std::int16_t second[Grain] = {0, 500, 1000, 1500, 2000, 2500, 3000, 3000};
    out = RenderI16(system);
    for (std::uint32_t i = 0; i < Grain; i++) Require(out[i] == second[i]);
    Require(callbackFlags.size() == 2 && callbackFlags[1] == SCE_NGS2_VOICE_CALLBACK_FLAG_BLOCK_END);
    Require(Flags(sampler) == 0);
    Require(sceNgs2SystemDestroy(system, nullptr) == SCE_NGS2_OK);
}

static void TestSubmixerMatrix() {
    const auto system = CreateSystem();
    const auto master = Mastering(system, 2);
    const auto submixer = Voice(CreateRack(system, SCE_NGS2_RACK_ID_SUBMIXER));
    Control(submixer, SCE_NGS2_SUBMIXER_VOICE_PARAM_SETUP, Ngs2SubmixerVoiceSetupParam{{}, 2, 0});
    Patch(submixer, master);
    const Ngs2VoiceCommand play{2, 0, 4, 0, {.u = SCE_NGS2_VOICE_EVENT_PLAY}};
    Require(sceNgs2VoiceRunCommands(submixer, &play, 1) == SCE_NGS2_OK);

    const std::vector<std::int16_t> pcm(Grain, 16384);
    const auto sampler = Sampler(system, pcm, 0);
    Patch(sampler, submixer);
    const float levels[2] = {1.0f, 0.5f};
    Control(sampler, SCE_NGS2_VOICE_PARAM_MATRIX_LEVELS, Ngs2VoiceMatrixLevelsParam{{}, 0, 2, levels});
    Control(sampler, SCE_NGS2_VOICE_PARAM_PORT_MATRIX, Ngs2VoicePortMatrixParam{{}, 0, 0});
    Control(sampler, SCE_NGS2_VOICE_PARAM_PORT_VOLUME, Ngs2VoicePortVolumeParam{{}, 0, 0.5f});
    Require(sceNgs2VoiceRunCommands(sampler, &play, 1) == SCE_NGS2_OK);

    std::vector<float> out(Grain * 2, -1.0f);
    const Ngs2RenderBufferInfo info{out.data(), out.size() * sizeof(float), SCE_NGS2_WAVEFORM_TYPE_PCM_F32L, 2};
    Require(sceNgs2SystemRender(system, &info, 1) == SCE_NGS2_OK);
    for (std::uint32_t i = 0; i < Grain; i++) Require(out[i * 2] == 0.25f && out[i * 2 + 1] == 0.125f);

    Ngs2SubmixerVoiceState state{};
    Require(sceNgs2VoiceGetState(submixer, &state.voice_state, sizeof(state)) == SCE_NGS2_OK);
    Require(state.voice_state.state_flags == (SCE_NGS2_VOICE_STATE_FLAG_INUSE | SCE_NGS2_VOICE_STATE_FLAG_PLAYING));

    Require(sceNgs2SystemRender(system, &info, 1) == SCE_NGS2_OK);
    for (float sample : out) Require(sample == 0.0f);
    Require(sceNgs2SystemDestroy(system, nullptr) == SCE_NGS2_OK);
}

static int allocations = 0;
static std::int32_t APS5_VABI Allocate(Ngs2ContextBufferInfo* info) {
    Require(info->host_buffer == nullptr && info->host_buffer_size != 0 && info->user_data == 9);
    info->host_buffer = std::calloc(1, info->host_buffer_size);
    allocations++;
    return SCE_NGS2_OK;
}
static std::int32_t APS5_VABI Release(Ngs2ContextBufferInfo* info) {
    Require(info->host_buffer != nullptr && info->user_data == 9);
    std::free(info->host_buffer);
    allocations--;
    return SCE_NGS2_OK;
}

static void TestSampleRate() {
    const auto system = CreateSystem();
    Require(sceNgs2SystemSetSampleRate(0x1234, 96000) == SCE_NGS2_ERROR_INVALID_SYSTEM_HANDLE);
    bool threw = false;
    try {
        sceNgs2SystemSetSampleRate(system, 0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    Require(threw);
    Require(sceNgs2SystemSetSampleRate(system, 96000) == SCE_NGS2_OK);
    Ngs2SystemInfo info{};
    Require(sceNgs2SystemGetInfo(system, &info, sizeof(info)) == SCE_NGS2_OK && info.sample_rate == 96000);

    const auto master = Mastering(system, 1);
    const std::vector<std::int16_t> pcm{0, 1000, 2000, 3000};
    const auto sampler = Sampler(system, pcm, 1);
    Patch(sampler, master);
    Event(sampler, SCE_NGS2_VOICE_EVENT_PLAY);
    const std::int16_t expected[Grain] = {0, 500, 1000, 1500, 2000, 2500, 3000, 1500};
    const auto out = RenderI16(system);
    for (std::uint32_t i = 0; i < Grain; i++) Require(out[i] == expected[i]);
    Require(sceNgs2SystemDestroy(system, nullptr) == SCE_NGS2_OK);
}

static void TestUserData() {
    uintptr_t value = 1;
    Require(sceNgs2SystemSetUserData(0x1234, 5) == SCE_NGS2_ERROR_INVALID_SYSTEM_HANDLE);
    Require(sceNgs2SystemGetUserData(0x1234, &value) == SCE_NGS2_ERROR_INVALID_SYSTEM_HANDLE && value == 1);
    const auto system = CreateSystem();
    Require(sceNgs2SystemGetUserData(system, &value) == SCE_NGS2_OK && value == 0);
    Require(sceNgs2SystemSetUserData(system, 0xfedcba9876543210) == SCE_NGS2_OK);
    Require(sceNgs2SystemGetUserData(system, &value) == SCE_NGS2_OK && value == 0xfedcba9876543210);
    bool threw = false;
    try {
        sceNgs2SystemGetUserData(system, nullptr);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    Require(threw);
    Require(sceNgs2SystemDestroy(system, nullptr) == SCE_NGS2_OK);
}

static void TestLock() {
    const auto system = CreateSystem();
    Require(sceNgs2SystemLock(0x1234) == SCE_NGS2_ERROR_INVALID_SYSTEM_HANDLE);
    Require(sceNgs2SystemUnlock(0x1234) == SCE_NGS2_ERROR_INVALID_SYSTEM_HANDLE);
    Ngs2SystemInfo info{};
    std::thread free([&] { Require(sceNgs2SystemGetInfo(system, &info, sizeof(info)) == SCE_NGS2_OK); });
    free.join();

    Require(sceNgs2SystemLock(system) == SCE_NGS2_OK);
    std::atomic<bool> done = false;
    std::thread blocked([&] {
        Require(sceNgs2SystemGetInfo(system, &info, sizeof(info)) == SCE_NGS2_OK);
        done = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Require(!done);
    Require(sceNgs2SystemUnlock(system) == SCE_NGS2_OK);
    blocked.join();
    Require(done);
    Require(sceNgs2SystemDestroy(system, nullptr) == SCE_NGS2_OK);
}

static void TestAllocator() {
    const Ngs2BufferAllocator allocator{Allocate, Release, 9};
    uintptr_t system = 0;
    Require(sceNgs2SystemCreateWithAllocator(nullptr, &allocator, &system) == SCE_NGS2_OK && allocations == 1);
    uintptr_t sampler = 0;
    uintptr_t master = 0;
    Require(sceNgs2RackCreateWithAllocator(system, SCE_NGS2_RACK_ID_SAMPLER, nullptr, &allocator, &sampler) == SCE_NGS2_OK);
    Require(sceNgs2RackCreateWithAllocator(system, SCE_NGS2_RACK_ID_MASTERING, nullptr, &allocator, &master) == SCE_NGS2_OK && allocations == 3);
    Ngs2ContextBufferInfo released{};
    Require(sceNgs2RackDestroy(sampler, &released) == SCE_NGS2_OK && allocations == 2 && released.host_buffer == nullptr);
    Require(sceNgs2SystemDestroy(system, nullptr) == SCE_NGS2_OK && allocations == 0);
    Require(sceNgs2RackDestroy(master, nullptr) == SCE_NGS2_ERROR_INVALID_RACK_HANDLE);
}

static void PutLe(std::vector<std::uint8_t>& out, std::uint32_t value, int bytes) {
    for (int i = 0; i < bytes; i++) out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

static void PutTag(std::vector<std::uint8_t>& out, const char* tag) {
    out.insert(out.end(), tag, tag + 4);
}

struct WavSpec {
    std::uint16_t tag = 1;
    std::uint16_t channels = 1;
    std::uint32_t sampleRate = 48000;
    std::uint16_t bits = 16;
    std::uint16_t blockAlign = 2;
    bool extensible = false;
    std::uint32_t loops = 0;
};

static std::vector<std::uint8_t> WavFile(const WavSpec& spec, const std::vector<std::uint8_t>& data) {
    static constexpr std::uint8_t SubtypeTail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
    std::vector<std::uint8_t> file;
    PutTag(file, "RIFF");
    PutLe(file, 0, 4);
    PutTag(file, "WAVE");
    PutTag(file, "fmt ");
    PutLe(file, spec.extensible ? 40 : 16, 4);
    PutLe(file, spec.extensible ? 0xfffe : spec.tag, 2);
    PutLe(file, spec.channels, 2);
    PutLe(file, spec.sampleRate, 4);
    PutLe(file, spec.sampleRate * spec.blockAlign, 4);
    PutLe(file, spec.blockAlign, 2);
    PutLe(file, spec.bits, 2);
    if (spec.extensible) {
        PutLe(file, 22, 2);
        PutLe(file, spec.bits, 2);
        PutLe(file, 0, 4);
        PutLe(file, spec.tag, 2);
        file.insert(file.end(), SubtypeTail, SubtypeTail + sizeof(SubtypeTail));
    }
    if (spec.loops != 0) {
        PutTag(file, "smpl");
        PutLe(file, 60, 4);
        for (int i = 0; i < 7; i++) PutLe(file, 0, 4);
        PutLe(file, spec.loops, 4);
        PutLe(file, 0, 4);
        for (int i = 0; i < 6; i++) PutLe(file, 0, 4);
    }
    PutTag(file, "data");
    PutLe(file, static_cast<std::uint32_t>(data.size()), 4);
    file.insert(file.end(), data.begin(), data.end());
    const auto riffSize = static_cast<std::uint32_t>(file.size() - 8);
    std::memcpy(file.data() + 4, &riffSize, sizeof(riffSize));
    return file;
}

static std::vector<std::uint8_t> Bytes(const std::vector<std::int16_t>& samples) {
    std::vector<std::uint8_t> bytes(samples.size() * sizeof(std::int16_t));
    std::memcpy(bytes.data(), samples.data(), bytes.size());
    return bytes;
}

static void TestParsePcm() {
    const std::vector<std::int16_t> stereo = {100, -100, 200, -200, 300, -300};
    const auto file = WavFile({1, 2, 44100, 16, 4}, Bytes(stereo));
    Ngs2WaveformInfo info{};
    Require(sceNgs2ParseWaveformData(file.data(), file.size(), &info) == SCE_NGS2_OK);
    Require(info.format.waveform_type == SCE_NGS2_WAVEFORM_TYPE_PCM_I16L && info.format.num_channels == 2 && info.format.sample_rate == 44100);
    Require(info.format.config_data == 0 && info.data_offset == file.size() - 12 && info.data_size == 12 && info.num_samples == 3);
    Require(info.audio_unit_size == 4 && info.num_audio_unit_samples == 1 && info.num_audio_unit_per_frame == 1);
    Require(info.audio_frame_size == 4 && info.num_audio_frame_samples == 1 && info.num_delay_samples == 0);
    Require(info.loop_begin_position == 0 && info.loop_end_position == 0);
    Require(info.num_blocks == 1 && info.block[0].data_offset == info.data_offset && info.block[0].data_size == 12);
    Require(info.block[0].num_skip_samples == 0 && info.block[0].num_samples == 3 && info.block[0].num_repeats == 0);

    const std::vector<std::uint8_t> floats(4 * 4, 0);
    Require(sceNgs2ParseWaveformData(WavFile({3, 1, 48000, 32, 4}, floats).data(), WavFile({3, 1, 48000, 32, 4}, floats).size(), &info) == SCE_NGS2_OK);
    Require(info.format.waveform_type == SCE_NGS2_WAVEFORM_TYPE_PCM_F32L && info.num_samples == 4 && info.audio_unit_size == 4);
    const auto extensible = WavFile({1, 1, 48000, 16, 2, true}, Bytes(stereo));
    Require(sceNgs2ParseWaveformData(extensible.data(), extensible.size(), &info) == SCE_NGS2_OK);
    Require(info.format.waveform_type == SCE_NGS2_WAVEFORM_TYPE_PCM_I16L && info.num_samples == 6 && info.data_offset == extensible.size() - 12);

    const auto badAlign = WavFile({1, 2, 48000, 16, 2}, Bytes(stereo));
    Require(sceNgs2ParseWaveformData(badAlign.data(), badAlign.size(), &info) == SCE_NGS2_ERROR_INVALID_WAVEFORM_FORMAT);
    const auto noChannels = WavFile({1, 0, 48000, 16, 0}, Bytes(stereo));
    Require(sceNgs2ParseWaveformData(noChannels.data(), noChannels.size(), &info) == SCE_NGS2_ERROR_INVALID_WAVEFORM_FORMAT);
    const auto tooManyChannels = WavFile({1, 9, 48000, 16, 18}, Bytes(stereo));
    Require(sceNgs2ParseWaveformData(tooManyChannels.data(), tooManyChannels.size(), &info) == SCE_NGS2_ERROR_INVALID_WAVEFORM_FORMAT);
    const auto noRate = WavFile({1, 1, 0, 16, 2}, Bytes(stereo));
    Require(sceNgs2ParseWaveformData(noRate.data(), noRate.size(), &info) == SCE_NGS2_ERROR_INVALID_WAVEFORM_FORMAT);

    for (const WavSpec& unsupported : {WavSpec{1, 1, 48000, 8, 1}, WavSpec{1, 1, 48000, 24, 3}, WavSpec{3, 1, 48000, 64, 8}, WavSpec{1, 1, 48000, 16, 2, false, 1}}) {
        const auto wav = WavFile(unsupported, Bytes(stereo));
        bool threw = false;
        try {
            sceNgs2ParseWaveformData(wav.data(), wav.size(), &info);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        Require(threw);
    }
}

static void TestPlayParsedPcm() {
    const std::vector<std::int16_t> pcm = {1000, 2000, 3000, 4000, 5000};
    const auto file = WavFile({}, Bytes(pcm));
    Ngs2WaveformInfo info{};
    Require(sceNgs2ParseWaveformData(file.data(), file.size(), &info) == SCE_NGS2_OK);

    const auto system = CreateSystem();
    const auto master = Mastering(system, 1);
    const auto voice = Voice(CreateRack(system, SCE_NGS2_RACK_ID_SAMPLER));
    Control(voice, SCE_NGS2_SAMPLER_VOICE_PARAM_SETUP, Ngs2SamplerVoiceSetupParam{{}, info.format});
    Control(voice, SCE_NGS2_SAMPLER_VOICE_PARAM_ADD_WAVEFORM_BLOCKS, Ngs2SamplerVoiceWaveformBlocksParam{{}, file.data(), 0, info.num_blocks, info.block});
    Patch(voice, master);
    Event(voice, SCE_NGS2_VOICE_EVENT_PLAY);
    const auto out = RenderI16(system);
    for (std::size_t i = 0; i < pcm.size(); i++) Require(out[i] == pcm[i]);
    Require(out[pcm.size()] == 0);
    Require(sceNgs2SystemDestroy(system, nullptr) == SCE_NGS2_OK);
}

static uintptr_t StereoChain(uintptr_t system) {
    const auto master = Mastering(system, 2);
    const auto submixer = Voice(CreateRack(system, SCE_NGS2_RACK_ID_SUBMIXER));
    Control(submixer, SCE_NGS2_SUBMIXER_VOICE_PARAM_SETUP, Ngs2SubmixerVoiceSetupParam{{}, 2, 0});
    Patch(submixer, master);
    Event(submixer, SCE_NGS2_VOICE_EVENT_PLAY);
    const std::vector<std::int16_t> pcm(Grain, 16384);
    const auto sampler = Sampler(system, pcm, 0);
    Patch(sampler, submixer);
    const float levels[2] = {1.0f, 0.5f};
    Control(sampler, SCE_NGS2_VOICE_PARAM_MATRIX_LEVELS, Ngs2VoiceMatrixLevelsParam{{}, 0, 2, levels});
    Control(sampler, SCE_NGS2_VOICE_PARAM_PORT_MATRIX, Ngs2VoicePortMatrixParam{{}, 0, 0});
    Event(sampler, SCE_NGS2_VOICE_EVENT_PLAY);
    return master;
}

static void TestMasteringIntoWiderBuffer() {
    const auto system = CreateSystem();
    StereoChain(system);
    std::vector<float> out(Grain * 8, -1.0f);
    const Ngs2RenderBufferInfo info{out.data(), out.size() * sizeof(float), SCE_NGS2_WAVEFORM_TYPE_PCM_F32L, 8};
    Require(sceNgs2SystemRender(system, &info, 1) == SCE_NGS2_OK);
    for (std::uint32_t i = 0; i < Grain; i++) {
        Require(out[i * 8] == 0.5f && out[i * 8 + 1] == 0.25f);
        for (std::uint32_t channel = 2; channel < 8; channel++) Require(out[i * 8 + channel] == 0.0f);
    }

    std::vector<std::int16_t> pcm16(Grain * 8, -1);
    const Ngs2RenderBufferInfo info16{pcm16.data(), pcm16.size() * sizeof(std::int16_t), SCE_NGS2_WAVEFORM_TYPE_PCM_I16L, 8};
    Require(sceNgs2SystemRender(system, &info16, 1) == SCE_NGS2_OK);
    Require(sceNgs2SystemDestroy(system, nullptr) == SCE_NGS2_OK);

    const auto narrow = CreateSystem();
    StereoChain(narrow);
    std::vector<float> mono(Grain, 0.0f);
    const Ngs2RenderBufferInfo monoInfo{mono.data(), mono.size() * sizeof(float), SCE_NGS2_WAVEFORM_TYPE_PCM_F32L, 1};
    bool threw = false;
    try {
        sceNgs2SystemRender(narrow, &monoInfo, 1);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    Require(threw);
    Require(sceNgs2SystemDestroy(narrow, nullptr) == SCE_NGS2_OK);
}

int main() {
    TestParsePcm();
    TestPlayParsedPcm();
    TestErrorsAndInfo();
    TestPcmBlockEnd();
    TestPitchAndRepeat();
    TestSubmixerMatrix();
    TestMasteringIntoWiderBuffer();
    TestSampleRate();
    TestUserData();
    TestLock();
    TestAllocator();
    return 0;
}
