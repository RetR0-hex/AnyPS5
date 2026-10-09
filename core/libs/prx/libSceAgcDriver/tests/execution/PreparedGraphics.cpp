#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderPreparation.hpp"
#include "SceShaders.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawCache.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderInputState.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <spirv/unified1/spirv.hpp>
#include <array>
#include <vector>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename TAction>
void Reject(TAction action, const char* expected) {
    try { action(); }
    catch (const std::exception& error) {
        Require(std::string(error.what()).find(expected) != std::string::npos, error.what());
        return;
    }
    throw std::runtime_error(std::string("invalid graphics ABI was accepted; expected: ") + expected);
}

struct Fixture {
    struct Header {
        Shader shader{};
        ShaderUserData users{};
    } header;
    alignas(256) std::array<std::uint32_t, 65> code{};
    std::shared_ptr<AgcDriver::DriverDetail::ShaderSnapshot> snapshot;

    void Initialize(std::uint8_t type) {
        header.shader.type = type;
        header.shader.user_data = &header.users;
        code.fill(0xffffffffu);
        code.back() = 0xbf810000u;
        const auto address = reinterpret_cast<std::uintptr_t>(code.data());
        const auto headerAddress = reinterpret_cast<std::uintptr_t>(&header);
        snapshot = std::make_shared<AgcDriver::DriverDetail::ShaderSnapshot>();
        snapshot->type = type;
        snapshot->codeAddress = address;
        snapshot->headerAddress = headerAddress;
        snapshot->code.assign(code.begin(), code.end());
        snapshot->header.resize(sizeof(header));
        std::memcpy(snapshot->header.data(), &header, sizeof(header));
    }

    void Bind(AgcDriver::QueueState& queue, std::uint32_t program, std::uint32_t resources) const {
        const auto address = snapshot->codeAddress + 256u;
        queue.shader[program] = static_cast<std::uint32_t>(address >> 8u);
        queue.shader[program + 1u] = static_cast<std::uint32_t>(address >> 40u);
        queue.shader[resources] = 16u << 1u;
    }
};

void CheckLazyCompute(AgcDriver::VulkanDevice& device) {
    using namespace AgcDriver::DriverDetail;
    using namespace ShaderRecompiler;
    Fixture fixture;
    fixture.Initialize(0);
    fixture.snapshot->code.assign(64, 0);
    fixture.snapshot->code[0] = 0xbf810000u;
    // A registered compute program has metadata but no compiled artifact until dispatch.
    RecompileRequest request{{ShaderStage::Compute, fixture.snapshot->codeAddress, fixture.snapshot->code, fixture.snapshot->headerAddress, fixture.snapshot->header}, {64, 0, {}, ShaderComputeStageInfo{{1, 1, 1}}, {}, {}}, device.ComputeTarget(64), {0, 0, 0, 128}};
    Require(fixture.snapshot->prepared->entries.empty(), "compute fixture started with prepared artifacts");
    static_cast<void>(InvocationFor(*fixture.snapshot, 0, request));
    Require(fixture.snapshot->prepared->entries.size() == 1, "first compute invocation did not retain its artifact");
    const auto handle = SourceHandleFor(*fixture.snapshot, 0, request);
    Require(handle == fixture.snapshot->prepared->entries.front().handle, "repeated compute lookup did not reuse the artifact");
    static_cast<void>(InvocationFor(*fixture.snapshot, 0, request));
    Require(fixture.snapshot->prepared->entries.size() == 1, "repeated compute dispatch duplicated its artifact");
    request.context.waveSize = 32;
    request.target = device.ComputeTarget(32);
    static_cast<void>(InvocationFor(*fixture.snapshot, 0, request));
    Require(fixture.snapshot->prepared->entries.size() == 2, "different compute wave size reused the wrong static ABI");
}

void CheckDeferredRegistration() {
    struct Header {
        Shader shader{};
        std::array<ShaderRegister, 6> registers{};
        ShaderSpecialRegs specials{};
        ShaderUserData users{};
    } header;
    // Libraries of shader permutations can include indirect compute calls that are never
    // dispatched. Registration must succeed, but ABI resolution must retain the refusal.
    alignas(256) const std::array<std::uint32_t, 4> code{0xbe8e0304u, 0xbe8f0305u, 0xbe8e210eu, 0xbf810000u};
    const auto address = reinterpret_cast<std::uintptr_t>(code.data());
    header.registers = {{{0x20c, static_cast<std::uint32_t>(address >> 8u)}, {0x20d, static_cast<std::uint32_t>(address >> 40u)}, {0x213, 6u << 1u}, {0x207, 1}, {0x208, 1}, {0x209, 1}}};
    header.shader.file_header = 0x34333231u;
    header.shader.version = 0x18u;
    header.shader.type = 0;
    header.shader.code = code.data();
    header.shader.header_size = sizeof(header);
    header.shader.shader_size = sizeof(code);
    header.shader.sh_registers = header.registers.data();
    header.shader.num_sh_registers = header.registers.size();
    header.shader.specials = &header.specials;
    header.shader.user_data = &header.users;
    AgcDriverRegisterShader_nid_postfix(&header.shader);
    // Shared code can be registered through two independently allocated copies of metadata.
    auto alias = header;
    alias.shader.sh_registers = alias.registers.data();
    alias.shader.specials = &alias.specials;
    alias.shader.user_data = &alias.users;
    AgcDriverRegisterShader_nid_postfix(&alias.shader);
    Shader sharedRegisters = alias.shader;
    sharedRegisters.sh_registers = header.registers.data();
    Reject([&] { AgcDriverResolveShaderAbi_nid_postfix(&sharedRegisters, {}, {}); }, "computed/data-dependent s_swappc_b64");
    header.registers[2].value ^= 2u;
    Reject([&] { AgcDriverResolveShaderAbi_nid_postfix(&sharedRegisters, {}, {}); }, "replaced shader header");
    header.registers[2].value ^= 2u;
    Shader shallow = alias.shader;
    // Link-time shallow copies omit resources; resolution must retain registered metadata.
    shallow.user_data = nullptr;
    Reject([&] { AgcDriverResolveShaderAbi_nid_postfix(&shallow, {}, {}); }, "computed/data-dependent s_swappc_b64");
    Reject([&] { AgcDriverResolveShaderAbi_nid_postfix(&header.shader, {}, {}); }, "computed/data-dependent s_swappc_b64");
    header.shader.file_header = 0;
    Reject([&] { AgcDriverRegisterShader_nid_postfix(&header.shader); }, "invalid shader header");

    // Split front-stage headers have no standalone program registers until linking.
    for (const std::uint8_t type : {4u, 5u}) {
        Fixture front;
        front.Initialize(type);
        front.header.shader.file_header = 0x34333231u;
        front.header.shader.version = 0x18u;
        front.header.shader.code = front.code.data();
        front.header.shader.header_size = sizeof(front.header);
        front.header.shader.shader_size = sizeof(front.code);
        AgcDriverRegisterShader_nid_postfix(&front.header.shader);
    }

    // Lookup must expose the original compiler reason, rather than an unrelated missing ABI.
    using namespace AgcDriver::DriverDetail;
    Fixture fixture;
    fixture.Initialize(0);
    fixture.snapshot->code.assign(code.begin(), code.end());
    fixture.snapshot->prepared->registrationFailure = "deferred indirect call";
    ShaderRecompiler::RecompileRequest request{};
    request.shader = {ShaderRecompiler::ShaderStage::Compute, fixture.snapshot->codeAddress, fixture.snapshot->code, fixture.snapshot->headerAddress, fixture.snapshot->header};
    Reject([&] { static_cast<void>(InvocationFor(*fixture.snapshot, 0, request)); }, "deferred indirect call");
    Reject([&] { static_cast<void>(SourceHandleFor(*fixture.snapshot, 0, request)); }, "deferred indirect call");
}

void Check(AgcDriver::VulkanDevice& device, AgcDriver::Graphics::ShaderPath path, const std::filesystem::path& dump) {
    using namespace AgcDriver::DriverDetail;
    using namespace ShaderRecompiler;
    Fixture front;
    Fixture back;
    Fixture domain;
    Fixture fragment;
    const bool tessellation = path == AgcDriver::Graphics::ShaderPath::Tessellation;
    const bool mesh = path == AgcDriver::Graphics::ShaderPath::Geometry;
    front.Initialize(tessellation ? 5u : mesh ? 4u : 2u);
    back.Initialize(tessellation ? 7u : 6u);
    domain.Initialize(2u);
    fragment.Initialize(1u);
    const std::array<std::uint32_t, 7> pixelCode{0xc8020002u, 0xc8060102u, 0xc80a0202u, 0xc80e0302u, 0xf800180fu, 0x03020100u, 0xbf810000u};
    fragment.snapshot->code.resize(64);
    fragment.snapshot->code.insert(fragment.snapshot->code.end(), pixelCode.begin(), pixelCode.end());
    ShaderRegistry registry;
    for (const auto* fixture : {&front, &back, &domain, &fragment}) registry.emplace(fixture->snapshot->codeAddress, fixture->snapshot);
    AgcDriver::QueueState queue{};
    // A nonzero misc-vector route must match between link preparation and execution,
    // even when the registered header does not carry the active context override.
    queue.context[0x207] = 1u << 21u;
    queue.context[0x8e] = 0xfu;
    queue.context[0x8f] = 0xfu;
    front.Bind(queue, tessellation ? 0x148u : 0xc8u, tessellation ? 0x10bu : 0x8bu);
    if (tessellation || mesh) back.Bind(queue, tessellation ? 0x108u : 0x88u, tessellation ? 0x10bu : 0x8bu);
    if (tessellation) domain.Bind(queue, 0xc8u, 0x8bu);
    fragment.Bind(queue, 0x8u, 0xbu);
    alignas(8) const std::array<std::uint32_t, 2> merged{};
    const auto mergedAddress = reinterpret_cast<std::uintptr_t>(merged.data());
    const auto pointerBase = tessellation ? 0x102u : 0x82u;
    queue.shader[pointerBase] = static_cast<std::uint32_t>(mergedAddress);
    queue.shader[pointerBase + 1u] = static_cast<std::uint32_t>(mergedAddress >> 32u);
    DrawDecode prepared{};
    prepared.state.stages.path = path;
    prepared.state.stages.vertexWaveSize = tessellation || mesh ? 64u : 32u;
    prepared.state.stages.fragmentWaveSize = 32u;
    if (mesh) prepared.state.stages.mesh = MeshConfiguration{4, 1, 3, 3, 1, 64, 128, 0, 4};
    if (tessellation) prepared.state.stages.tessellation = TessellationConfiguration{3, 4, 1, 2, 2};
    prepared.pixel.wave32 = true;
    prepared.pixel.interpolatorCount = 2;
    prepared.pixel.interpolatorSettings[0] = 0x403u;
    prepared.pixel.interpolatorSettings[1] = 0x220u;
    prepared.pixel.targetOutputMode[0] = 9;
    prepared.pixel.targetExportMapping.fill(0xe4u);
    DecodeGraphicsPrograms(prepared, queue, registry, true, true);
    DrawDecode draw{};
    draw.state = prepared.state;
    draw.pixel = prepared.pixel;
    DecodeGraphicsPrograms(draw, queue, registry, false, true);
    Require(prepared.programs.size() == draw.programs.size() && prepared.roles == draw.roles, "prepared graphics programs differ from draw programs");
    for (std::size_t index = 0; index < prepared.programs.size(); ++index) {
        const auto& expected = prepared.programs[index];
        const auto& actual = draw.programs[index];
        Require(expected.codeOffset == 64u && expected.binary.code.size() == (prepared.roles[index] == ProgramRole::Fragment ? pixelCode.size() : 1u) && expected.binary.code[0] == (prepared.roles[index] == ProgramRole::Fragment ? pixelCode[0] : 0xbf810000u), "graphics entry point did not trim the code prefix");
        Require(expected.binary.stage == actual.binary.stage && expected.firstUserSgpr == actual.firstUserSgpr && expected.userData.size() == actual.userData.size(), "graphics preparation changed the user SGPR ABI");
    }
    auto target = device.Target();
    std::vector<std::uint32_t> capabilities(target.supportedCapabilities.begin(), target.supportedCapabilities.end());
    std::vector<std::string_view> extensions(target.supportedExtensions.begin(), target.supportedExtensions.end());
    if (mesh) {
        capabilities.push_back(spv::CapabilityMeshShadingEXT);
        extensions.push_back("SPV_EXT_mesh_shader");
        target.supportedCapabilities = capabilities;
        target.supportedExtensions = extensions;
        target.mesh = MeshTargetLimits{{128, 1, 1}, 128, 32768, 256, 256, 128, 32768, 1, 1};
    }
    const auto stages = PrepareGraphicsStages(prepared, target);
    Require(stages.size() == (tessellation ? 4u : 2u), "graphics preparation compiled the wrong stages");
    for (const auto& stage : stages) stage.snapshot->prepared->entries.push_back(stage.entry);
    const auto& pixelArtifact = GetPreparedArtifact(*stages.back().entry.handle);
    Require(!pixelArtifact.fragmentParameters.empty() && pixelArtifact.fragmentParameters.front().sourceLocation == 3u, "prepared fragment lost interpolant mapping");
    if (!mesh && !tessellation) {
        const auto frontCount = front.snapshot->prepared->entries.size();
        const auto pixelCount = fragment.snapshot->prepared->entries.size();
        Reject([&] {
            ShaderPreparationTransaction transaction;
            transaction.Edit(*front.snapshot).entries.push_back(stages.front().entry);
            transaction.Edit(*fragment.snapshot).entries.push_back(stages.back().entry);
            auto unsupported = target;
            unsupported.tessellation.reset();
            ResolvePreparedGraphics(*front.snapshot, fragment.snapshot, 7, unsupported);
            transaction.Commit();
        }, "tessellation shaders are unavailable");
        Require(front.snapshot->prepared->entries.size() == frontCount && fragment.snapshot->prepared->entries.size() == pixelCount && front.snapshot->prepared->rectangles.empty() && front.snapshot->prepared->fragments.empty() && !front.snapshot->prepared->rectangleRequested, "failed rectangle compilation published part of the stage group");
        for (const bool primitiveFirst : {false, true}) {
            front.snapshot->prepared->rectangles.clear();
            front.snapshot->prepared->rectangleProgress.clear();
            front.snapshot->prepared->fragments.clear();
            front.snapshot->prepared->rectangleRequested = false;
            if (primitiveFirst) ResolvePreparedGraphics(*front.snapshot, {}, 7, target);
            ResolvePreparedGraphics(*front.snapshot, fragment.snapshot, 0, target);
            if (!primitiveFirst) ResolvePreparedGraphics(*front.snapshot, {}, 17, target);
            const auto vertexId = GetPreparedArtifact(*stages.front().entry.handle).variantId;
            const auto rectangle = PreparedRectangle(*front.snapshot, vertexId, pixelArtifact.variantId);
            Require(!rectangle.control.spirv.empty() && !rectangle.evaluation.spirv.empty(), "separate helpers did not prepare rectangle shaders");
            const auto count = front.snapshot->prepared->rectangles.size();
            ResolvePreparedGraphics(*front.snapshot, fragment.snapshot, 7, target);
            Require(front.snapshot->prepared->rectangles.size() == count, "repeated helpers duplicated rectangle shaders");
        }
        auto changed = prepared;
        changed.state.stages.vertexWaveSize = 64;
        changed.pixel.interpolatorSettings[0] = 0x404u;
        const auto newStages = PrepareGraphicsStages(changed, target);
        front.snapshot->prepared->entries.push_back(newStages.front().entry);
        ResolvePreparedGraphics(*front.snapshot, {}, 7, target);
        const auto newVertexId = GetPreparedArtifact(*newStages.front().entry.handle).variantId;
        Require(newVertexId != GetPreparedArtifact(*stages.front().entry.handle).variantId, "rectangle test did not change the vertex variant");
        static_cast<void>(PreparedRectangle(*front.snapshot, newVertexId, pixelArtifact.variantId));
        fragment.snapshot->prepared->entries.push_back(newStages.back().entry);
        ResolvePreparedGraphics(*front.snapshot, fragment.snapshot, 0, target);
        const auto newPixelId = GetPreparedArtifact(*newStages.back().entry.handle).variantId;
        Require(newPixelId != pixelArtifact.variantId, "rectangle test did not change the fragment variant");
        static_cast<void>(PreparedRectangle(*front.snapshot, newVertexId, newPixelId));
        auto temporary = std::make_shared<ShaderSnapshot>();
        temporary->prepared->entries = fragment.snapshot->prepared->entries;
        ResolvePreparedGraphics(*front.snapshot, temporary, 0, target);
        std::weak_ptr<const ShaderSnapshot> expired = temporary;
        temporary.reset();
        Require(expired.expired(), "helper link retained a shader snapshot");
        ResolvePreparedGraphics(*front.snapshot, {}, 7, target);
        Require(front.snapshot->prepared->fragments.size() == 1, "expired helper link was not removed");
        Require(front.snapshot->prepared->rectangleProgress.size() == 1 && !front.snapshot->prepared->rectangleProgress.front().fragment.expired(), "expired rectangle progress was not removed");
    }
    if (!dump.empty()) {
        for (std::size_t index = 0; index < stages.size(); ++index) {
            const auto& words = GetPreparedArtifact(*stages[index].entry.handle).spirv;
            const auto name = dump / (std::to_string(static_cast<unsigned>(path)) + "-" + std::to_string(index) + ".spv");
            std::ofstream output(name, std::ios::binary);
            output.write(reinterpret_cast<const char*>(words.data()), static_cast<std::streamsize>(words.size() * sizeof(std::uint32_t)));
            Require(static_cast<bool>(output), "cannot write prepared graphics SPIR-V");
        }
    }
    auto incomplete = prepared;
    incomplete.roles.pop_back();
    Reject([&] { static_cast<void>(PrepareGraphicsStages(incomplete, target)); }, "program roles are incomplete");
    auto wrongEntry = prepared;
    wrongEntry.programs.front().binary.codeAddress += 4;
    Reject([&] { static_cast<void>(PrepareGraphicsStages(wrongEntry, target)); }, "entry point differs");
    std::vector<LinkedProgram> linked;
    std::vector<MemoryRegion> memory;
    for (std::size_t index = 0; index < draw.programs.size(); ++index) {
        const auto& program = draw.programs[index];
        linked.push_back({draw.roles[index], program.binary, program.userDataBase, program.firstUserSgpr, program.userData});
        memory.insert(memory.end(), program.memory.begin(), program.memory.end());
    }
    for (std::size_t index = 0; index < draw.programs.size(); ++index) {
        if (draw.roles[index] == ProgramRole::GeometryBack) continue;
        const auto& program = draw.programs[index];
        const bool pixel = program.binary.stage == ShaderStage::Fragment;
        std::optional<ShaderVertexStageInfo> vertex;
        if (!pixel) {
            vertex = AgcDriver::Graphics::DecodeVertexStageInfo(program.binary.header, program.binary.headerAddress, program.userData, nullptr, true);
            vertex->paClVsOutCntl = draw.paClVsOutCntl;
        }
        RecompileRequest request{program.binary, {pixel ? 32u : prepared.state.stages.vertexWaveSize, program.firstUserSgpr, program.userData, {}, pixel ? std::optional(draw.pixel) : std::nullopt, vertex, memory}, target, {0, 0, 0, mesh ? MeshDrawPushOffsetBytes : 128u}, GraphicsCompileContext{program.firstUserSgpr, linked, prepared.state.stages.mesh, prepared.state.stages.tessellation, {0, 3, 4, 1}}};
        static_cast<void>(InvocationFor(*program.snapshot, program.codeOffset, request));
        request.layout.pushConstantOffsetBytes = 4;
        request.layout.pushConstantSizeBytes -= 4;
        static_cast<void>(InvocationFor(*program.snapshot, program.codeOffset, request));
        request.layout.pushConstantOffsetBytes = 2;
        Reject([&] { static_cast<void>(InvocationFor(*program.snapshot, program.codeOffset, request)); }, "artifact is missing");
        request.layout = {0, 0, 0, mesh ? MeshDrawPushOffsetBytes : 128u};
        if (pixel) request.context.pixel->interpolatorSettings[0] ^= 1u;
        else if (tessellation) ++request.graphics->tessellation->outputControlPoints;
        else if (mesh) ++request.graphics->mesh->maxVertices;
        else ++request.context.userDataBaseRegister;
        Reject([&] { static_cast<void>(InvocationFor(*program.snapshot, program.codeOffset, request)); }, "artifact is missing");
    }
    // A depth-only draw retains nonzero PS resource registers from the last color pass.
    // Those SGPRs must not become part of the synthetic shader's prepared interface.
    Fixture nullPixel;
    nullPixel.Initialize(1u);
    nullPixel.snapshot->codeAddress = NullPixelProgramAddress();
    nullPixel.snapshot->code.assign(64, 0);
    nullPixel.snapshot->code[0] = 0xbf810000u;
    registry.emplace(nullPixel.snapshot->codeAddress, nullPixel.snapshot);
    auto depthOnly = queue;
    depthOnly.shader[0x008] = 0;
    depthOnly.shader[0x009] = 0;
    depthOnly.context[0x8e] = 0;
    depthOnly.context[0x8f] = 0;
    DrawDecode nullDraw{};
    nullDraw.state = prepared.state;
    DecodeGraphicsPrograms(nullDraw, depthOnly, registry, false, true);
    Require(nullDraw.programs.back().userData.empty(), "null pixel shader inherited stale user SGPRs");
    Require(nullDraw.programs.back().snapshot == nullPixel.snapshot, "depth-only pass did not select the null pixel shader");
    if (!mesh && !tessellation) {
        nullDraw.state.stages.fragmentWaveSize = 64;
        nullDraw.pixel = AgcDriver::Graphics::DecodePixelStageInfo(depthOnly.context, {}, true);
        const auto nullStages = PrepareGraphicsStages(nullDraw, target);
        for (const auto& stage : nullStages) stage.snapshot->prepared->entries.push_back(stage.entry);
        // Interpolant linking can precede primitive-state creation. Keep the null pairing
        // when linking without a primitive type, then resolve rectangle helpers later.
        ResolvePreparedGraphics(*front.snapshot, nullPixel.snapshot, 0, target);
        ResolvePreparedGraphics(*front.snapshot, {}, 7, target);
        const auto vertexId = GetPreparedArtifact(*nullStages.front().entry.handle).variantId;
        const auto pixelId = GetPreparedArtifact(*nullStages.back().entry.handle).variantId;
        static_cast<void>(PreparedRectangle(*front.snapshot, vertexId, pixelId));
        const auto count = front.snapshot->prepared->rectangles.size();
        ResolvePreparedGraphics(*front.snapshot, nullPixel.snapshot, 7, target);
        Require(front.snapshot->prepared->rectangles.size() == count, "null pixel pairing duplicated rectangle artifacts");
        // A depth-only draw need not have passed through an SDK rectangle link call.
        front.snapshot->prepared->rectangles.clear();
        RecompileResult vertexResult, pixelResult;
        static_cast<CompiledShaderArtifact&>(vertexResult) = GetPreparedArtifact(*nullStages.front().entry.handle);
        static_cast<CompiledShaderArtifact&>(pixelResult) = GetPreparedArtifact(*nullStages.back().entry.handle);
        const auto lazy = PreparedRectangle(*front.snapshot, vertexResult, pixelResult, target);
        Require(!lazy.control.spirv.empty() && !lazy.evaluation.spirv.empty(), "unlinked null pixel rectangle did not prepare helpers");
        static_cast<void>(PreparedRectangle(*front.snapshot, vertexResult, pixelResult, target));
        Require(front.snapshot->prepared->rectangles.size() == 1, "repeated rectangle draw duplicated helpers");
    }
    ShaderRegistry invalidRegistry;
    DrawDecode invalid{};
    invalid.state = prepared.state;
    Reject([&] { DecodeGraphicsPrograms(invalid, queue, invalidRegistry, true, true); }, "registered");
    const auto frontBase = tessellation ? 0x148u : 0xc8u;
    queue.shader[frontBase + 1u] |= 0x100u;
    Reject([&] { DecodeGraphicsPrograms(invalid, queue, registry, true, true); }, "reserved graphics program address");
}

}

int main(int argc, char** argv) {
    try {
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Require(argc <= 2, "invalid test arguments");
        const auto dump = argc == 2 ? std::filesystem::path(argv[1]) : std::filesystem::path{};
        Check(*device, AgcDriver::Graphics::ShaderPath::Vertex, dump);
        Check(*device, AgcDriver::Graphics::ShaderPath::Geometry, dump);
        Check(*device, AgcDriver::Graphics::ShaderPath::Tessellation, dump);
        CheckLazyCompute(*device);
        // Driver registration owns another Vulkan device; release the fixture device first.
        device.reset();
        CheckDeferredRegistration();
        // Release the driver's device before process-wide Vulkan/SDL state is finalized.
        AgcDriverShutdown_nid_postfix();
        std::cout << "prepared graphics ABI tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try { AgcDriverShutdown_nid_postfix(); } catch (...) {}
        return 1;
    }
}
