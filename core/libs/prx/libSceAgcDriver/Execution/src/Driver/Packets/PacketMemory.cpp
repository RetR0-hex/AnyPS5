#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/SynchronizationStatistics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/DeferredLabels.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Eq/include/Event.hpp"
#include <cstdlib>
#include <cstring>
#include <limits>

namespace AgcDriver::DriverDetail {

bool Driver::preparePacketMemory(const Submission& submission, QueueState& queue, std::span<const std::uint32_t> packet, std::uint32_t header, std::uint32_t opcode, bool& wroteOnGpu, bool& endOfPipeInterrupt, bool& interruptDeferred, bool& drawPacket, bool& sampleDump) {
    static const bool drainAll = std::getenv("APS5_DRAIN_ALL") != nullptr;
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    wroteOnGpu = false;

    bool orderedAlready = false;

    const auto interruptSelect = (packet[2] >> 24u) & 7u;
    endOfPipeInterrupt = opcode == 0x49 && interruptSelect != 0 && interruptSelect != 3;
    interruptDeferred = false;
    if (!drainAll && endOfPipeInterrupt) {
        const auto label = Pm4::DecodeLabelWrite(packet);
        const bool storesNothing = (packet[2] >> 29u) == 0 || (packet[3] | (static_cast<std::uint64_t>(packet[4]) << 32u)) == 0;
        if (label.has_value() || storesNothing) {
            bumpEpoch(&EpochBumps::drains);
            GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Label);
            std::lock_guard gpuLock(GuestMemory::GpuMutex());
            const auto localDevice = device.Load();
            recordDeferredLabels(localDevice.get(), submission.queue);
            const bool workOpen = Graphics::Recorder::RecordedWorkSinceSubmit() != 0;
            int reason = localDevice != nullptr ? 0 : 1;
            if (label.has_value()) {
                const auto bytes = label->Bytes();
                const auto stamp = ++eventSerial;
                reason = localDevice != nullptr ? localDevice->WriteLabelOnGpu(label->address, bytes, stamp, submission.queue) : 4;
                if (reason == 1) GuestMemory::Write(label->address, bytes, 4);
                if (reason == 0 || reason == 1 || reason == 5 || reason == 6) {
                    noteLabelStore(label->address, bytes, stamp);
                    Graphics::Recorder::CloseLabelGroup(GuestMemory::TrackerGeneration());
                }
                countLabelOutcome(reason);
                ++immediateLabels;
            } else {
                ++noOpLabels;
            }
            if (reason == 0 || reason == 5 || reason == 6) {
                const auto queueId = submission.queue;
                interruptDeferred = localDevice->AfterRecordedWork([queueId] { AgcDriverDeliverEopInterrupt(queueId); }, submission.queue == 0);
                if (interruptDeferred && workOpen) localDevice->SubmitRecorded(submission.queue == 0);
                wroteOnGpu = true;
            } else if (reason == 1) {
                wroteOnGpu = true;
            }
        }
    }
    if (!drainAll && !endOfPipeInterrupt && (opcode == 0x49 || opcode == 0x37)) {
        if (const auto label = Pm4::DecodeLabelWrite(packet)) {
            const auto bytes = label->Bytes();
            if (DeferLabels() && bytes.size() <= DeferredLabel::Capacity && bytes.size() % 4 == 0 && label->address % 4 == 0) {

                auto& deferred = deferredLabels();
                if (deferred.labels.empty()) deferred.since = std::chrono::steady_clock::now();
                auto& entry = deferred.labels.emplace_back();
                entry.address = label->address;
                entry.size = bytes.size();
                std::memcpy(entry.bytes.data(), bytes.data(), bytes.size());
                ++queuedLabels;
                wroteOnGpu = true;
            } else {
                GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Label);
                std::lock_guard gpuLock(GuestMemory::GpuMutex());
                const auto localDevice = device.Load();

                recordDeferredLabels(localDevice.get(), submission.queue);
                const auto stamp = ++eventSerial;
                const auto reason = localDevice != nullptr ? localDevice->WriteLabelOnGpu(label->address, bytes, stamp, submission.queue) : 4;
                wroteOnGpu = reason == 0 || reason == 5 || reason == 6;
                if (reason == 1) {

                    GuestMemory::Write(label->address, bytes, 4);
                    wroteOnGpu = true;
                }
                if (wroteOnGpu) noteLabelStore(label->address, bytes, stamp);
                Graphics::Recorder::CloseLabelGroup(GuestMemory::TrackerGeneration());
                countLabelOutcome(reason);
                ++immediateLabels;
            }
        } else if (opcode == 0x49 ? ((packet[2] >> 29u) == 0 || (packet[3] | (static_cast<std::uint64_t>(packet[4]) << 32u)) == 0) : (packet[2] | (static_cast<std::uint64_t>(packet[3]) << 32u)) == 0) {
            orderedAlready = true;
            ++noOpLabels;
        } else {
            ++labelFallbacks[4];
        }
    }

    static const bool cpuStores = std::getenv("APS5_CPU_STORES") != nullptr;
    if (!drainAll && !cpuStores && (opcode == 0x40 || opcode == 0x50 || opcode == 0x83)) {
        constexpr std::size_t gpuStoreLimit = 65536;
        bool drained = true;
        // A small DMA_DATA memory-to-memory copy is otherwise resolved here on the CPU: ResolveStore
        // reads the source through the checked path, so a source that recorded GPU work still
        // writes makes the flush hook submit and wait for that batch (in Exit 9 the newest one,
        // ~5 unsignaled batches, ~12 ms per packet: [hooksync] "0x0 DMA_DATA unknown"), and a copy
        // whose destination or size is not a multiple of 4 is refused by the label store and
        // drains the whole device below. The CP runs DMA_DATA asynchronously, so nothing on the
        // CPU may observe the destination before the GPU reaches the packet; a transfer recorded
        // in queue order (the >64 KiB path's CopyBuffer, which barriers after every earlier
        // recorded write and notes the destination as a pending write, so a later CPU read of it
        // still waits through the hook) gives exactly the hardware's ordering without the wait.
        // The gate stays narrow: a settled, aligned source keeps the cheaper label store (no
        // transfer, no barriers). Queued labels are recorded first so a label over the source is
        // a pending write the transfer orders after. When CopyBuffer refuses (not imported:
        // path 2), the packet falls through to the CPU resolve below, unchanged.
        // APS5_NO_GPU_DMA_COPY=1 restores the CPU resolve for every small copy.
        static const bool gpuDmaCopy = std::getenv("APS5_NO_GPU_DMA_COPY") == nullptr;
        bool copiedOnGpu = false;
        if (gpuDmaCopy && opcode == 0x50) {
            const auto copy = Pm4::DecodeMemoryCopy(packet);
            if (copy.has_value() && copy->bytes <= gpuStoreLimit && GuestMemory::Accessible(reinterpret_cast<const void*>(copy->source), copy->bytes) && GuestMemory::Accessible(reinterpret_cast<const void*>(copy->destination), copy->bytes, true)) {
                const bool misaligned = copy->destination % 4 != 0 || copy->bytes % 4 != 0;
                GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Copy);
                std::lock_guard gpuLock(GuestMemory::GpuMutex());
                if (const auto localDevice = device.Load()) {
                    recordDeferredLabels(localDevice.get(), submission.queue);
                    // Read under the GPU mutex: the active recorder is only valid while it is held.
                    const auto* recorder = Graphics::Recorder::Active();
                    if (recorder != nullptr && (misaligned || recorder->PendingWriteOverlaps(copy->source, copy->bytes))) {
                        // cpuMax 0: never the in-place memcpy (that is the label store's job); the
                        // writer note is empty as on the >64 KiB path (no known destination value).
                        const auto outcome = localDevice->CopyBuffer(copy->destination, copy->source, copy->bytes, 0, std::numeric_limits<std::size_t>::max(), 0, 0, submission.queue, [](std::span<const std::byte>, std::uint64_t) {});
                        if (outcome.path == 1 || outcome.path == 3) {
                            copiedOnGpu = true;
                            wroteOnGpu = true;
                            drained = false;
                            ++dmaCopiesOnGpu;
                            if (outcome.synced) ++dmaCopiesSynced;
                        }
                    }
                }
            }
        }
        if (copiedOnGpu) {
            // Recorded above; neither the label store nor the drain applies.
        } else if (const auto store = Pm4::ResolveStore(packet, queue, gpuStoreLimit)) {
            const auto bytes = store->Bytes();
            if (bytes.empty()) {

                orderedAlready = true;
                drained = false;
            } else {

                GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Label);
                std::lock_guard gpuLock(GuestMemory::GpuMutex());
                const auto localDevice = device.Load();

                Graphics::StorageTexture::FlushPending(store->address, bytes.size(), nullptr, "packet store", Graphics::PublishScope::PartialUnits);

                recordDeferredLabels(localDevice.get(), submission.queue);
                const auto stamp = ++eventSerial;
                const auto reason = localDevice != nullptr ? localDevice->WriteLabelOnGpu(store->address, bytes, stamp, submission.queue) : 4;
                if (reason == 0 || reason == 1 || reason == 5 || reason == 6) noteLabelStore(store->address, bytes, stamp);
                if (reason == 0 || reason == 5 || reason == 6) {
                    if (reason == 0) ++storesOnGpu;
                    else ++storesBehindCompletions;
                    wroteOnGpu = true;
                    drained = false;
                } else if (reason == 1) {
                    GuestMemory::Write(store->address, bytes, 1);
                    ++storesOnCpu;
                    wroteOnGpu = true;
                    drained = false;
                }
                Graphics::Recorder::CloseLabelGroup(GuestMemory::TrackerGeneration());
            }
        }
        if (drained && opcode == 0x50) {
            const auto copy = Pm4::DecodeMemoryCopy(packet);
            if (copy.has_value() && copy->bytes > gpuStoreLimit && GuestMemory::Accessible(reinterpret_cast<const void*>(copy->source), copy->bytes) && GuestMemory::Accessible(reinterpret_cast<const void*>(copy->destination), copy->bytes, true)) {
                GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Copy);
                std::lock_guard gpuLock(GuestMemory::GpuMutex());
                if (const auto localDevice = device.Load()) {
                    recordDeferredLabels(localDevice.get(), submission.queue);
                    const auto outcome = localDevice->CopyBuffer(copy->destination, copy->source, copy->bytes, 0, std::numeric_limits<std::size_t>::max(), 0, 0, submission.queue, [](std::span<const std::byte>, std::uint64_t) {});
                    if (outcome.path == 1 || outcome.path == 3) {
                        wroteOnGpu = true;
                        drained = false;
                    }
                }
            }
        }
        if (drained) ++storesDrained;
    }

    static const bool drawDrain = std::getenv("APS5_DRAW_DRAIN") != nullptr;
    drawPacket = Pm4::DrawOpcode(opcode);
    sampleDump = opcode == 0x46 && (packet[1] & 0x3fu) == 0x39u;
    if (sampleDump && !drainAll) {
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Label);
        std::lock_guard gpuLock(GuestMemory::GpuMutex());
        if (const auto localDevice = device.Load()) {
            recordDeferredLabels(localDevice.get(), submission.queue);
            wroteOnGpu = localDevice->DumpSamplesOnGpu(packet[2] | (static_cast<std::uint64_t>(packet[3]) << 32u));
        }
    }

    static const bool syncFlip = std::getenv("APS5_SYNC_FLIP") != nullptr;
    const bool drains = drainAll ? ((Pm4::AccessesMemory(header) && opcode != 0x16) || opcode == 0x42 || opcode == 0x46 || opcode == 0x58 || header == FlipPacketHeader)
                                 : (!wroteOnGpu && !orderedAlready && (opcode == 0x49 || opcode == 0x37 || opcode == 0x40 || opcode == 0x45 || opcode == 0x50 || opcode == 0x83 || sampleDump || (drawPacket && drawDrain) || (header == FlipPacketHeader && syncFlip)));
    if (drains) {

        static const bool unlockedDrain = std::getenv("APS5_NO_UNLOCKED_DRAIN") == nullptr && !drainAll;

        bumpEpoch(&EpochBumps::drains);
        std::shared_ptr<VulkanDevice> draining;
        std::uint64_t epoch = 0;
        {
            GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Flush);
            std::lock_guard gpuLock(GuestMemory::GpuMutex());
            if (profile) {
                ++drainCounts[header == FlipPacketHeader ? 0xffffu : opcode];
                ++drainTotal;
                if (std::chrono::steady_clock::now() - lastSyncReport > std::chrono::seconds(10)) {
                    lastSyncReport = std::chrono::steady_clock::now();
                    reportSync();
                }
            }
            draining = device.Load();

            recordDeferredLabels(draining.get(), submission.queue);
            if (draining != nullptr) {
                if (unlockedDrain && draining->CanWaitUnlocked()) epoch = draining->SubmitAndEpoch();
                else draining->WaitIdle();
            }
            if (epoch == 0) draining.reset();
        }
        if (epoch != 0) {
            ++unlockedDrains;
            try {
                draining->WaitRecorded(epoch);
            } catch (...) {

                GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Flush);
                std::lock_guard gpuLock(GuestMemory::GpuMutex());
                draining.reset();
                throw;
            }
            GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Flush);
            std::lock_guard gpuLock(GuestMemory::GpuMutex());
            draining->ReapRecorded(epoch);
            draining.reset();
        }
    }
    return drains;
}

void Driver::dumpSampleCounters(std::uint64_t address) {
    std::uint64_t samples = 0;
    {
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Flush);
        std::lock_guard gpuLock(GuestMemory::GpuMutex());
        auto* recorder = Graphics::Recorder::Active();
        require(recorder != nullptr, "occlusion counters without the command recorder are not implemented");
        recorder->CountSamples();
        samples = recorder->SamplesTotal();
    }
    constexpr std::uint64_t ready = 1ull << 63u;
    for (std::uint64_t db = 0; db < 16; ++db) {
        const std::uint64_t value = ready | (db == 0 ? samples : 0u);
        GuestMemory::Write(address + db * 16u, std::as_bytes(std::span(&value, 1)), 8);
    }
}

}
