#include "axklib/application/device_sessions.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "axklib/application/device_xml.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/application/operation_registry.hpp"
#include "axklib/application/secure_random.hpp"
#include "axklib/application/write_operations.hpp"
#include "axklib/audio.hpp"
#include "axklib/sfs.hpp"
#include "axklib/writer.hpp"

namespace {
using namespace axk::app;

struct Storage {
    std::vector<std::byte> bytes = std::vector<std::byte>(1024U * 1024U, std::byte{0x42});
    std::size_t reads{};
    std::size_t bytes_read{};
    std::optional<std::pair<std::uint64_t, std::uint64_t>> pcm_region;
    std::uint64_t pcm_bytes_read{};
    std::size_t writes{};
    std::size_t upgrades{};
    bool fail_writes{};
    bool fail_flush_once{};
    bool disconnected{};
};

class FakeTransport final : public DeviceTransport {
  public:
    explicit FakeTransport(std::shared_ptr<Storage> storage) : storage_(std::move(storage)) {}
    std::uint64_t size() const noexcept override { return storage_->bytes.size(); }
    std::uint32_t sector_size() const noexcept override { return 512U; }
    std::string carrier_identity() const override { return "fake-usb-carrier"; }
    std::string media_identity() const override { return "fake-card-1"; }
    Result<void> authorize_writes() override {
        ++storage_->upgrades;
        return {};
    }
    Result<void> verify_bound() const override {
        if (storage_->disconnected)
            return std::unexpected(Error{"device_changed", "test device disconnected"});
        return {};
    }
    axk::Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> bytes) const override {
        ++storage_->reads;
        storage_->bytes_read += bytes.size();
        if (offset > size() || bytes.size() > size() - offset || storage_->disconnected)
            return std::unexpected(
                axk::make_error(axk::ErrorCode::io_read_failed, axk::ErrorCategory::io, "test read failed"));
        if (storage_->pcm_region) {
            const auto first = std::max(offset, storage_->pcm_region->first);
            const auto last = std::min(offset + bytes.size(), storage_->pcm_region->second);
            if (first < last)
                storage_->pcm_bytes_read += last - first;
        }
        std::copy_n(storage_->bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.size(), bytes.begin());
        return {};
    }
    Result<void> write_exact_at(std::uint64_t offset, std::span<const std::byte> bytes) override {
        ++storage_->writes;
        if (storage_->fail_writes)
            return std::unexpected(Error{"test_write_failed", "simulated write and restoration failure"});
        if (offset % 512U != 0U || bytes.size() % 512U != 0U || offset > size() || bytes.size() > size() - offset)
            return std::unexpected(Error{"test_alignment", "unaligned write"});
        std::copy(bytes.begin(), bytes.end(), storage_->bytes.begin() + static_cast<std::ptrdiff_t>(offset));
        return {};
    }
    Result<void> flush() override {
        if (storage_->fail_flush_once) {
            storage_->fail_flush_once = false;
            return std::unexpected(Error{"test_flush_failed", "simulated commit flush failure"});
        }
        return {};
    }

  private:
    std::shared_ptr<Storage> storage_;
};

class FakeProvider final : public DeviceProvider {
  public:
    std::shared_ptr<Storage> storage = std::make_shared<Storage>();
    std::size_t lists{};
    std::size_t connects{};
    std::uint64_t offset{};
    std::optional<DeviceScsiTarget> scsi;
    Result<std::vector<DeviceCandidate>> list() override {
        ++lists;
        return std::vector<DeviceCandidate>{{"candidate", "Fake card", "serial-1", storage->bytes.size(), 512U}};
    }
    Result<ConnectedDevice> connect(std::string_view, std::string_view, std::string_view) override {
        ++connects;
        return ConnectedDevice{
            std::make_shared<FakeTransport>(storage),
            {"candidate", "Fake card", "serial-1", storage->bytes.size(), 512U},
            {{"disk", "Test target", offset, storage->bytes.size() - offset, 512U, false, {}, scsi}}};
    }
};

class RecordedProgress final : public axk::ProgressSink {
  public:
    std::vector<axk::Progress> events;
    void report(const axk::Progress &value) noexcept override { events.push_back(value); }
};

class DeviceSessions : public testing::Test {
  protected:
    std::filesystem::path directory;
    std::shared_ptr<FakeProvider> provider = std::make_shared<FakeProvider>();
    void SetUp() override {
        auto random = secure_random_hex(8U);
        ASSERT_TRUE(random);
        directory = std::filesystem::temp_directory_path() / ("axk-device-tests-" + *random);
    }
    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
    std::string open(DeviceSessionManager &manager) {
        EXPECT_TRUE(manager.list("owner"));
        auto session = manager.connect("candidate", "owner");
        EXPECT_TRUE(session);
        return session ? session->session_id : std::string{};
    }
    void read_and_authorize(DeviceSessionManager &manager, const std::string &id) {
        auto source = manager.open_source(id, "disk", "owner");
        ASSERT_TRUE(source);
        std::vector<std::byte> metadata(512U);
        ASSERT_TRUE(source->reader->read_exact_at(0U, metadata));
        ASSERT_TRUE(source->seal({}, nullptr));
        ASSERT_TRUE(manager.authorize_writes(id, "disk", "owner", true));
    }
    void load_sfs(std::size_t frames) {
        std::filesystem::create_directories(directory);
        axk::Waveform waveform;
        waveform.format = {1U, 2U, 44'100U};
        waveform.frame_count = frames;
        waveform.pcm.resize(frames * 2U, std::byte{0x12});
        const auto audio_path = directory / "tone.wav";
        ASSERT_TRUE(axk::write_wav_atomic(audio_path, waveform, true));
        axk::VolumeSpec volume;
        volume.name = "Original";
        volume.waveforms.push_back({"wave", "Tone", audio_path, 60U, {}});
        axk::SampleSpec sample;
        sample.name = "Sample";
        sample.waveform_id = "wave";
        volume.samples.push_back(sample);
        const axk::HdsBuildManifest manifest{"1.0", 4U * 1024U * 1024U, {{"disk", {volume}}}};
        const auto path = directory / "fixture.hds";
        ASSERT_TRUE(axk::write_hds_image(manifest, path, true));
        const auto file = axk::FileReader::open(path);
        ASSERT_TRUE(file);
        provider->storage->bytes.resize(static_cast<std::size_t>((*file)->size()));
        ASSERT_TRUE((*file)->read_exact_at(0U, provider->storage->bytes));
    }
};

TEST_F(DeviceSessions, TargetWireMetadataPreservesExactOffsetsAndScsiIdentity) {
    provider->offset = 512U;
    provider->scsi = DeviceScsiTarget{2U, 2U, "codesrc", "CD-ROM", "6.0", "target-2"};
    DeviceSessionManager manager{directory, true, true, provider};
    auto registry = make_operation_registry();
    ASSERT_TRUE(bind_device_operations(registry, manager));
    OperationContext context{};
    context.owner_id = "owner";
    ASSERT_TRUE(registry.invoke("devices.list", nlohmann::json::object(), context));
    const auto connected =
        registry.invoke("devices.connect", {{"candidateId", "candidate"}, {"layout", "scsi2sd-v6"}}, context);
    ASSERT_TRUE(connected) << connected.error().message;
    const auto &target = connected->at("targets").at(0);
    EXPECT_EQ(target.at("offsetBytes").get<std::uint64_t>(), 512U);
    EXPECT_EQ(target.at("sizeBytes").get<std::uint64_t>(), provider->storage->bytes.size() - 512U);
    EXPECT_EQ(target.at("scsi"), (nlohmann::json{{"id", 2U},
                                                 {"deviceType", "OPTICAL"},
                                                 {"vendor", "codesrc"},
                                                 {"product", "CD-ROM"},
                                                 {"revision", "6.0"},
                                                 {"serial", "target-2"}}));
    EXPECT_EQ(provider->storage->writes, 0U);
    EXPECT_EQ(provider->storage->upgrades, 0U);
}

TEST_F(DeviceSessions, ConstructionDoesNotEnumerateOpenOrRecoverDevices) {
    DeviceSessionManager manager{directory, true, true, provider};
    EXPECT_EQ(provider->lists, 0U);
    EXPECT_EQ(provider->connects, 0U);
    EXPECT_EQ(provider->storage->reads, 0U);
    EXPECT_EQ(provider->storage->writes, 0U);
    EXPECT_FALSE(manager.connect("candidate", "owner"));
}

TEST_F(DeviceSessions, XmlInspectionDoesNotEnumerateConnectReadOrWrite) {
    DeviceSessionManager manager{directory, true, true, provider};
    auto registry = make_operation_registry();
    ASSERT_TRUE(bind_device_operations(registry, manager));
    const std::string xml =
        "<SCSI2SD><BoardConfig><selectionDelay>255</selectionDelay></BoardConfig>"
        "<SCSITarget id='0'><enabled>true</enabled><sdSectorStart>2048</sdSectorStart>"
        "<scsiSectors>1000</scsiSectors><bytesPerSector>512</bytesPerSector></SCSITarget>"
        "<SCSITarget id='2'><enabled>true</enabled><sdSectorStart>4096</sdSectorStart>"
        "<scsiSectors>100</scsiSectors><bytesPerSector>2048</bytesPerSector><deviceType>2</deviceType>"
        "</SCSITarget><SCSITarget id='3'><enabled>false</enabled></SCSITarget></SCSI2SD>";
    auto result = registry.invoke("devices.xml.inspect", {{"xml", xml}}, OperationContext{});
    if (native_device_provider()->available()) {
        ASSERT_TRUE(result);
        ASSERT_EQ(result->at("targets").size(), 3U);
        EXPECT_TRUE(result->at("targets").at(0).at("eligible").get<bool>());
        EXPECT_TRUE(result->at("targets").at(1).at("readOnly").get<bool>());
        EXPECT_FALSE(result->at("targets").at(2).at("eligible").get<bool>());
        EXPECT_FALSE(inspect_scsi2sd_xml("<SCSI2SD/>"));
    } else {
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, "devices_unavailable");
    }
    EXPECT_EQ(provider->lists, 0U);
    EXPECT_EQ(provider->connects, 0U);
    EXPECT_EQ(provider->storage->reads, 0U);
    EXPECT_EQ(provider->storage->writes, 0U);
}

TEST_F(DeviceSessions, XmlInspectionMarksOverlappingTargetsReadOnly) {
    const auto result = inspect_scsi2sd_xml(
        "<SCSI2SD><BoardConfig/>"
        "<SCSITarget id='0'><enabled>true</enabled><sdSectorStart>2048</sdSectorStart>"
        "<scsiSectors>1000</scsiSectors><bytesPerSector>512</bytesPerSector></SCSITarget>"
        "<SCSITarget id='1'><enabled>true</enabled><sdSectorStart>2500</sdSectorStart>"
        "<scsiSectors>1000</scsiSectors><bytesPerSector>512</bytesPerSector></SCSITarget></SCSI2SD>");
    if (!native_device_provider()->available()) {
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, "devices_unavailable");
        return;
    }
    ASSERT_TRUE(result);
    ASSERT_EQ(result->size(), 2U);
    for (const auto &target : *result) {
        EXPECT_TRUE(target.eligible);
        EXPECT_TRUE(target.read_only);
        EXPECT_TRUE(std::ranges::any_of(target.notices, [](const std::string &notice) {
            return notice.find("overlaps SCSI ID") != std::string::npos;
        }));
    }
}

TEST_F(DeviceSessions, OwnershipAndAuthorizationAreExplicit) {
    DeviceSessionManager manager{directory, true, true, provider};
    const auto id = open(manager);
    EXPECT_FALSE(manager.status(id, "another-owner"));
    EXPECT_FALSE(manager.authorize_writes(id, "disk", "owner", true));
    EXPECT_FALSE(manager.begin_mutation(id, "disk", "owner", "operation-1"));
    read_and_authorize(manager, id);
    EXPECT_FALSE(manager.authorize_writes(id, "disk", "owner", false));
    EXPECT_FALSE(manager.begin_mutation(id, "disk", "owner", ""));
    EXPECT_EQ(provider->storage->writes, 0U);
    EXPECT_TRUE(manager.close(id, "owner"));
}

TEST_F(DeviceSessions, MetadataChangePreventsWriteUpgrade) {
    DeviceSessionManager manager{directory, true, true, provider};
    const auto id = open(manager);
    auto source = manager.open_source(id, "disk", "owner");
    ASSERT_TRUE(source);
    std::vector<std::byte> metadata(512U);
    ASSERT_TRUE(source->reader->read_exact_at(0U, metadata));
    ASSERT_TRUE(source->seal({}, nullptr));
    provider->storage->bytes[25U] = std::byte{0x44};
    EXPECT_FALSE(manager.authorize_writes(id, "disk", "owner", true));
    EXPECT_EQ(provider->storage->upgrades, 0U);
    EXPECT_EQ(provider->storage->writes, 0U);
}

TEST_F(DeviceSessions, SparseByteEditsPreserveCompleteSectorNeighborsAndDurableStatus) {
    DeviceSessionManager manager{directory, true, true, provider};
    const auto id = open(manager);
    read_and_authorize(manager, id);
    const auto before = provider->storage->bytes;
    auto mutation = manager.begin_mutation(id, "disk", "owner", "operation-1");
    ASSERT_TRUE(mutation);
    std::vector<AlterationJournalPatch> patches{
        {510U, std::vector<std::byte>(4U, std::byte{0x42}), std::vector<std::byte>(4U, std::byte{0x77})},
        {520U, {std::byte{0x42}}, {std::byte{0x11}}}};
    ASSERT_TRUE(manager.apply(*mutation, patches, {}, {}, {}, {}));
    auto expected = before;
    std::fill_n(expected.begin() + 510, 4U, std::byte{0x77});
    expected[520U] = std::byte{0x11};
    EXPECT_EQ(provider->storage->bytes, expected);
    EXPECT_EQ(provider->storage->writes, 1U);
    auto operations = manager.operations("owner");
    ASSERT_TRUE(operations);
    ASSERT_EQ(operations->size(), 1U);
    EXPECT_EQ(operations->front().operation_id, "operation-1");
    EXPECT_EQ(operations->front().state, "committed");
    mutation->reset();
    EXPECT_FALSE(manager.begin_mutation(id, "disk", "owner", "operation-1"));
}

TEST_F(DeviceSessions, FailedRestorationRetainsJournalWithoutAutomaticDeviceRecovery) {
    {
        DeviceSessionManager manager{directory, true, true, provider};
        const auto id = open(manager);
        read_and_authorize(manager, id);
        auto mutation = manager.begin_mutation(id, "disk", "owner", "operation-2");
        ASSERT_TRUE(mutation);
        provider->storage->fail_writes = true;
        std::vector<AlterationJournalPatch> patches{{4096U, {std::byte{0x42}}, {std::byte{0x11}}}};
        auto result = manager.apply(*mutation, patches, {}, {}, {}, {});
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, "device_recovery_required");
    }
    const auto writes = provider->storage->writes;
    DeviceSessionManager reopened{directory, true, true, provider};
    auto operations = reopened.operations("owner");
    ASSERT_TRUE(operations);
    ASSERT_EQ(operations->size(), 1U);
    EXPECT_EQ(operations->front().state, "recovery_required");
    EXPECT_EQ(provider->storage->writes, writes);
    provider->storage->fail_writes = false;
    const auto id = open(reopened);
    EXPECT_FALSE(reopened.recover("operation-2", id, "disk", "owner", false));
    ASSERT_TRUE(reopened.recover("operation-2", id, "disk", "owner", true));
    auto recovered = reopened.operations("owner");
    ASSERT_TRUE(recovered);
    EXPECT_EQ(recovered->front().state, "rolled_back");
}

TEST_F(DeviceSessions, RecoveryRefusesUnexpectedBytesAndInvalidatesTheOwnedSession) {
    {
        DeviceSessionManager manager{directory, true, true, provider};
        const auto id = open(manager);
        read_and_authorize(manager, id);
        auto mutation = manager.begin_mutation(id, "disk", "owner", "operation-3");
        ASSERT_TRUE(mutation);
        provider->storage->fail_writes = true;
        std::vector<AlterationJournalPatch> patches{{4096U, {std::byte{0x42}}, {std::byte{0x11}}}};
        EXPECT_FALSE(manager.apply(*mutation, patches, {}, {}, {}, {}));
    }
    provider->storage->fail_writes = false;
    provider->storage->bytes[4100U] = std::byte{0x99};
    DeviceSessionManager manager{directory, true, true, provider};
    const auto id = open(manager);
    const auto writes = provider->storage->writes;
    EXPECT_FALSE(manager.recover("operation-3", id, "disk", "owner", true));
    EXPECT_EQ(provider->storage->writes, writes);
    auto state = manager.status(id, "owner");
    ASSERT_TRUE(state);
    EXPECT_TRUE(state->invalidated);
}

TEST_F(DeviceSessions, SequentialReadsAndGuardVerificationAreBatched) {
    DeviceSessionManager manager{directory, true, true, provider};
    const auto id = open(manager);
    auto source = manager.open_source(id, "disk", "owner");
    ASSERT_TRUE(source);
    std::vector<std::byte> bytes(256U * 1024U);
    ASSERT_TRUE(source->reader->read_exact_at(0U, bytes));
    ASSERT_TRUE(source->seal({}, nullptr));
    EXPECT_LE(provider->storage->reads, 5U);
}

TEST_F(DeviceSessions, SmallMetadataReadsUseFourKiBPagesAndGuardOnlyTouchedSectors) {
    DeviceSessionManager manager{directory, true, true, provider};
    const auto id = open(manager);
    auto source = manager.open_source(id, "disk", "owner");
    ASSERT_TRUE(source);
    std::array<std::byte, 1U> byte{};
    ASSERT_TRUE(source->reader->read_exact_at(0U, byte));
    ASSERT_TRUE(source->reader->read_exact_at(512U, byte));
    EXPECT_EQ(provider->storage->reads, 1U);
    EXPECT_EQ(provider->storage->bytes_read, 4096U);
    provider->storage->bytes[2048U] = std::byte{0x11};
    ASSERT_TRUE(source->seal({}, nullptr));
    EXPECT_TRUE(manager.authorize_writes(id, "disk", "owner", true));
}

TEST_F(DeviceSessions, MetadataCacheIsBoundedAndEvictsLeastRecentlyUsedPages) {
    provider->storage->bytes.resize(8U * 1024U * 1024U, std::byte{0x42});
    DeviceSessionManager manager{directory, true, true, provider};
    const auto id = open(manager);
    auto source = manager.open_source(id, "disk", "owner");
    ASSERT_TRUE(source);
    std::array<std::byte, 1U> byte{};
    for (std::uint64_t page = 0U; page < 1024U; ++page)
        ASSERT_TRUE(source->reader->read_exact_at(page * 4096U, byte));
    EXPECT_EQ(provider->storage->reads, 1024U);
    ASSERT_TRUE(source->reader->read_exact_at(0U, byte));
    ASSERT_TRUE(source->reader->read_exact_at(1024U * 4096U, byte));
    ASSERT_TRUE(source->reader->read_exact_at(0U, byte));
    EXPECT_EQ(provider->storage->reads, 1025U);
    ASSERT_TRUE(source->reader->read_exact_at(4096U, byte));
    EXPECT_EQ(provider->storage->reads, 1026U);
}

TEST_F(DeviceSessions, LargeReadsStreamFreshBytesWithoutEvictingMetadataPages) {
    DeviceSessionManager manager{directory, true, true, provider};
    const auto id = open(manager);
    auto source = manager.open_source(id, "disk", "owner");
    ASSERT_TRUE(source);
    std::array<std::byte, 1U> byte{};
    ASSERT_TRUE(source->reader->read_exact_at(0U, byte));
    ASSERT_TRUE(source->seal({}, nullptr));
    const auto reads = provider->storage->reads;
    provider->storage->bytes[512U] = std::byte{0x11};
    std::vector<std::byte> streamed(128U * 1024U);
    ASSERT_TRUE(source->reader->read_exact_at(0U, streamed));
    EXPECT_EQ(provider->storage->reads, reads + 2U);
    EXPECT_EQ(streamed[512U], std::byte{0x11});
    ASSERT_TRUE(source->reader->read_exact_at(0U, byte));
    EXPECT_EQ(provider->storage->reads, reads + 2U);
}

TEST_F(DeviceSessions, MutationPlanningCachesMetadataButVerificationAlwaysReadsFreshBytes) {
    DeviceSessionManager manager{directory, true, true, provider};
    const auto id = open(manager);
    read_and_authorize(manager, id);
    auto mutation = manager.begin_mutation(id, "disk", "owner", "planning-cache");
    ASSERT_TRUE(mutation);
    provider->storage->reads = 0U;
    provider->storage->bytes_read = 0U;
    std::array<std::byte, 1U> byte{};
    ASSERT_TRUE((*mutation)->read_exact_at(4096U, byte));
    ASSERT_TRUE((*mutation)->read_exact_at(4100U, byte));
    EXPECT_EQ(provider->storage->reads, 1U);
    EXPECT_EQ(provider->storage->bytes_read, 4096U);
    provider->storage->bytes[4096U] = std::byte{0x11};
    EXPECT_FALSE((*mutation)->verify_unchanged());
    EXPECT_GT(provider->storage->reads, 1U);
    EXPECT_EQ(provider->storage->writes, 0U);
}

TEST_F(DeviceSessions, AuthorizationReportsFreshByteProgressAndIndeterminateOsPermission) {
    DeviceSessionManager manager{directory, true, true, provider};
    const auto id = open(manager);
    auto source = manager.open_source(id, "disk", "owner");
    ASSERT_TRUE(source);
    std::array<std::byte, 1U> byte{};
    ASSERT_TRUE(source->reader->read_exact_at(0U, byte));
    RecordedProgress progress;
    ASSERT_TRUE(source->seal({}, &progress));
    ASSERT_EQ(progress.events.back().completed, 512U);
    progress.events.clear();
    ASSERT_TRUE(manager.authorize_writes(id, "disk", "owner", true, {}, &progress));
    ASSERT_EQ(progress.events.size(), 5U);
    EXPECT_EQ(progress.events[0].label, "Verifying device metadata");
    EXPECT_EQ(progress.events[1].completed, 512U);
    EXPECT_EQ(progress.events[1].total, 512U);
    EXPECT_EQ(progress.events[2].label, "Requesting write permission");
    EXPECT_FALSE(progress.events[2].total);
    EXPECT_EQ(progress.events[3].label, "Verifying write access");
    EXPECT_EQ(progress.events[4].completed, 512U);
    EXPECT_EQ(progress.events[4].total, 512U);
}

TEST_F(DeviceSessions, CancelledVerificationNeverRequestsWritePermission) {
    DeviceSessionManager manager{directory, true, true, provider};
    const auto id = open(manager);
    auto source = manager.open_source(id, "disk", "owner");
    ASSERT_TRUE(source);
    std::array<std::byte, 1U> byte{};
    ASSERT_TRUE(source->reader->read_exact_at(0U, byte));
    ASSERT_TRUE(source->seal({}, nullptr));
    axk::CancellationSource cancellation;
    cancellation.cancel();
    const auto result = manager.authorize_writes(id, "disk", "owner", true, cancellation.token());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, "operation_cancelled");
    EXPECT_EQ(provider->storage->upgrades, 0U);
    EXPECT_EQ(provider->storage->writes, 0U);
}

TEST_F(DeviceSessions, CompletedOperationsCannotBeReplayedByRecovery) {
    DeviceSessionManager manager{directory, true, true, provider};
    const auto id = open(manager);
    read_and_authorize(manager, id);
    auto mutation = manager.begin_mutation(id, "disk", "owner", "terminal");
    ASSERT_TRUE(mutation);
    std::vector<AlterationJournalPatch> patches{{4096U, {std::byte{0x42}}, {std::byte{0x11}}}};
    ASSERT_TRUE(manager.apply(*mutation, patches, {}, {}, {}, {}));
    mutation->reset();
    const auto writes = provider->storage->writes;
    const auto replay = manager.recover("terminal", id, "disk", "owner", true);
    ASSERT_FALSE(replay);
    EXPECT_EQ(replay.error().code, "device_operation_terminal");
    EXPECT_EQ(provider->storage->writes, writes);
}

TEST_F(DeviceSessions, UnpublishedCrashDebrisDoesNotHideOperationStatus) {
    DeviceSessionManager manager{directory, true, true, provider};
    for (unsigned index = 0; index < 4100U; ++index)
        std::filesystem::create_directory(directory / "operations" / std::to_string(index));
    const auto operations = manager.operations("owner");
    ASSERT_TRUE(operations);
    EXPECT_TRUE(operations->empty());
    EXPECT_EQ(provider->storage->reads, 0U);
}

TEST_F(DeviceSessions, StatusReviewFencesUnstartedRequestsWithoutDeviceAccess) {
    DeviceSessionManager manager{directory, true, true, provider};
    const auto resolved = manager.resolve("lost-before-admission", "owner");
    ASSERT_TRUE(resolved);
    EXPECT_EQ(resolved->state, "no_write");
    EXPECT_EQ(provider->lists, 0U);
    EXPECT_EQ(provider->storage->reads, 0U);
    const auto id = open(manager);
    read_and_authorize(manager, id);
    EXPECT_FALSE(manager.begin_mutation(id, "disk", "owner", "lost-before-admission"));
    EXPECT_EQ(provider->storage->writes, 0U);
}

TEST_F(DeviceSessions, StatusReviewDuringPlanningPreventsADeferredWrite) {
    DeviceSessionManager manager{directory, true, true, provider};
    const auto id = open(manager);
    read_and_authorize(manager, id);
    auto mutation = manager.begin_mutation(id, "disk", "owner", "lost-during-planning");
    ASSERT_TRUE(mutation);
    const auto resolved = manager.resolve("lost-during-planning", "owner");
    ASSERT_TRUE(resolved);
    EXPECT_EQ(resolved->state, "no_write");
    std::vector<AlterationJournalPatch> patches{{4096U, {std::byte{0x42}}, {std::byte{0x11}}}};
    const auto result = manager.apply(*mutation, patches, {}, {}, {}, {});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, "device_operation_resolved");
    EXPECT_EQ(provider->storage->writes, 0U);
}

TEST_F(DeviceSessions, OldReadersRejectCachedAndUncachedReadsAfterASave) {
    DeviceSessionManager manager{directory, true, true, provider};
    const auto id = open(manager);
    auto source = manager.open_source(id, "disk", "owner");
    ASSERT_TRUE(source);
    std::array<std::byte, 1U> byte{};
    ASSERT_TRUE(source->reader->read_exact_at(0U, byte));
    ASSERT_TRUE(source->seal({}, nullptr));
    ASSERT_TRUE(manager.authorize_writes(id, "disk", "owner", true));
    auto mutation = manager.begin_mutation(id, "disk", "owner", "reader-revision");
    ASSERT_TRUE(mutation);
    ASSERT_TRUE((*mutation)->read_exact_at(4096U, byte));
    std::vector<AlterationJournalPatch> patches{{4096U, {std::byte{0x42}}, {std::byte{0x11}}}};
    ASSERT_TRUE(manager.apply(*mutation, patches, {}, {}, {}, {}));
    ASSERT_TRUE((*mutation)->read_exact_at(4096U, byte));
    EXPECT_EQ(byte.front(), std::byte{0x11});
    mutation->reset();
    EXPECT_FALSE(source->reader->read_exact_at(0U, byte));
    EXPECT_FALSE(source->reader->read_exact_at(512U * 1024U, byte));
    EXPECT_FALSE(source->seal({}, nullptr));
    auto current = manager.open_source(id, "disk", "owner");
    ASSERT_TRUE(current);
    ASSERT_TRUE(current->reader->read_exact_at(4096U, byte));
    EXPECT_EQ(byte.front(), std::byte{0x11});
}

TEST_F(DeviceSessions, LargePayloadOriginalsStreamWithoutConsumingMetadataGuardCapacity) {
    provider->storage->bytes.resize(40U * 1024U * 1024U, std::byte{0x42});
    DeviceSessionManager manager{directory, true, true, provider};
    const auto id = open(manager);
    read_and_authorize(manager, id);
    auto mutation = manager.begin_mutation(id, "disk", "owner", "large-payload");
    ASSERT_TRUE(mutation);
    const auto count = provider->storage->bytes.size() - 4096U;
    auto replacement = std::make_shared<axk::MemoryReader>(std::vector<std::byte>(count, std::byte{0x11}));
    std::vector<AlterationJournalPatch> patches{{4096U, {*mutation, 4096U, count}, {replacement, 0U, count}}};
    ASSERT_TRUE(manager.apply(*mutation, patches, {}, {}, {}, {}));
    EXPECT_EQ(provider->storage->bytes.back(), std::byte{0x11});
    EXPECT_EQ(provider->storage->bytes.front(), std::byte{0x42});
}

TEST_F(DeviceSessions, OpeningAndSampleRenameDoNotReadUnrelatedPcm) {
    std::size_t small_open_bytes{};
    std::size_t small_rename_bytes{};
    for (const auto frames : {3U, 512U * 1024U}) {
        ASSERT_NO_FATAL_FAILURE(load_sfs(frames));
        const auto geometry =
            axk::open_image(std::make_shared<axk::MemoryReader>(provider->storage->bytes), "fixture.hds");
        ASSERT_TRUE(geometry);
        const auto &partition = geometry->partitions().front();
        const auto cluster_bytes = geometry->superblock().sector_size_bytes * partition.sectors_per_cluster;
        const auto index_bytes = static_cast<std::uint64_t>(cluster_bytes) * partition.directory_index_span_clusters;
        const auto wave = std::ranges::find(partition.records, "SMPL", &axk::IndexRecord::object_type);
        ASSERT_NE(wave, partition.records.end());
        ASSERT_EQ(wave->extents.size(), 1U);
        const auto physical =
            static_cast<std::uint64_t>(partition.start_sector) * geometry->superblock().sector_size_bytes +
            static_cast<std::uint64_t>(wave->extents.front().cluster_offset) * cluster_bytes;
        // Boundary pages may include PCM beside the header or the next object.
        // Interior PCM pages must never be fetched by this metadata-only edit.
        const auto pcm_start = ((physical + 0xacU + 4095U) / 4096U) * 4096U;
        const auto pcm_end = ((physical + wave->data_size) / 4096U) * 4096U;
        provider->storage->pcm_region.reset();
        provider->storage->pcm_bytes_read = 0U;
        if (pcm_start < pcm_end)
            provider->storage->pcm_region.emplace(pcm_start, pcm_end);
        DeviceSessionManager devices{directory / std::to_string(frames), true, true, provider};
        const auto id = open(devices);
        const auto sandbox = Sandbox::create({});
        ASSERT_TRUE(sandbox);
        ImageSessionManager images{
            *sandbox,      8U,  100U,    std::chrono::minutes{15}, std::chrono::steady_clock::now, nullptr,
            1024U * 1024U, 16U, &devices};
        ImageSourceRef source;
        source.kind = ImageSourceKind::device;
        source.device_session_id = id;
        source.device_target_id = "disk";
        provider->storage->bytes_read = 0U;
        RecordedProgress progress;
        const auto image = images.open(source, "owner", {}, &progress);
        ASSERT_TRUE(image) << image.error().message;
        const auto open_bytes = provider->storage->bytes_read;
        EXPECT_LE(open_bytes, 4U * index_bytes + 64U * 1024U);
        EXPECT_EQ(provider->storage->pcm_bytes_read, 0U);
        for (const auto label : {"Reading filesystem metadata", "Reading object metadata", "Verifying device metadata"})
            EXPECT_TRUE(std::ranges::any_of(progress.events, [&](const auto &event) {
                return event.label == label && event.total && event.completed == event.total;
            })) << label;
        ASSERT_TRUE(devices.authorize_writes(id, "disk", "owner", true));
        UploadStore uploads{directory / "uploads", 1024U, 1024U, 1U, 1024U, std::chrono::minutes{1}};
        AlterationJournalStore journals{directory / "file-journals"};
        auto registry = make_operation_registry();
        ASSERT_TRUE(bind_session_write_operations(registry, *sandbox, uploads, images, journals));
        const nlohmann::json manifest{{"schema_version", "1.0"},
                                      {"operations", nlohmann::json::array({{{"id", "rename"},
                                                                             {"type", "rename_sbnk"},
                                                                             {"partition_index", 0U},
                                                                             {"volume_name", "Original"},
                                                                             {"sample_name", "Sample"},
                                                                             {"new_sample_name", "Renamed"}}})}};
        std::vector<nlohmann::json> diagnostics;
        OperationContext context{"owner", "request", {}, nullptr, {}, {}, "sample-rename"};
        context.diagnostic = [&](const auto &event) { diagnostics.push_back(event); };
        provider->storage->bytes_read = 0U;
        const auto renamed = registry.invoke(
            "images.alter",
            {{"imageId", image->image_id}, {"expectedRevision", 1U}, {"manifest", {{"inline", manifest}}}}, context);
        ASSERT_TRUE(renamed) << renamed.error().message;
        const auto rename_bytes = provider->storage->bytes_read;
        EXPECT_LE(rename_bytes, 20U * index_bytes + 128U * 1024U);
        EXPECT_EQ(provider->storage->pcm_bytes_read, 0U);
        ASSERT_FALSE(diagnostics.empty());
        EXPECT_EQ(diagnostics.back().at("phase").get<std::string>(), "total");
        EXPECT_GT(diagnostics.back().at("deviceIo").at("physicalReads").get<std::uint64_t>(), 0U);
        EXPECT_GT(diagnostics.back().at("deviceIo").at("cacheHits").get<std::uint64_t>(), 0U);
        const auto samples = images.objects(image->image_id, "owner", 100U, {}, "SBNK");
        ASSERT_TRUE(samples);
        ASSERT_EQ(samples->items.size(), 1U);
        EXPECT_EQ(samples->items.front().name, "Renamed");
        if (frames == 3U) {
            small_open_bytes = open_bytes;
            small_rename_bytes = rename_bytes;
        } else {
            EXPECT_LE(open_bytes, small_open_bytes + 16U * 1024U);
            EXPECT_LE(rename_bytes, small_rename_bytes + 32U * 1024U);
        }
        RecordProperty(frames == 3U ? "smallOpenBytes" : "largeOpenBytes", std::to_string(open_bytes));
        RecordProperty(frames == 3U ? "smallRenameBytes" : "largeRenameBytes", std::to_string(rename_bytes));
        RecordProperty("indexBytes", std::to_string(index_bytes));
        RecordProperty(frames == 3U ? "smallDiagnostics" : "largeDiagnostics", nlohmann::json(diagnostics).dump());
        ASSERT_TRUE(images.close(image->image_id, "owner"));
        ASSERT_TRUE(devices.close(id, "owner"));
    }
}

TEST_F(DeviceSessions, SfsEditingUsesTheSameSessionPipelineWithoutCopyingTheDevice) {
    ASSERT_NO_FATAL_FAILURE(load_sfs(3U));
    DeviceSessionManager devices{directory / "devices", true, true, provider};
    const auto id = open(devices);
    const auto sandbox = Sandbox::create({});
    ASSERT_TRUE(sandbox);
    ImageSessionManager images{
        *sandbox,      8U,  100U,    std::chrono::minutes{15}, std::chrono::steady_clock::now, nullptr,
        1024U * 1024U, 16U, &devices};
    ImageSourceRef source;
    source.kind = ImageSourceKind::device;
    source.device_session_id = id;
    source.device_target_id = "disk";
    const auto opened = images.open(source, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    EXPECT_EQ(opened->source.kind, ImageSourceKind::device);
    ASSERT_TRUE(devices.authorize_writes(id, "disk", "owner", true));
    UploadStore uploads{directory / "uploads", 1024U, 1024U, 1U, 1024U, std::chrono::minutes{1}};
    AlterationJournalStore journals{directory / "file-journals"};
    auto registry = make_operation_registry();
    ASSERT_TRUE(bind_session_write_operations(registry, *sandbox, uploads, images, journals));
    const nlohmann::json edit{{"schema_version", "1.0"},
                              {"operations", nlohmann::json::array({{{"id", "rename"},
                                                                     {"type", "rename_volume"},
                                                                     {"partition_index", 0U},
                                                                     {"volume_name", "Original"},
                                                                     {"new_volume_name", "Edited"}}})}};
    OperationContext context{"owner", "request", {}, nullptr, {}, {}, "known-device-save"};
    const auto original_bytes = provider->storage->bytes;
    provider->storage->fail_flush_once = true;
    auto rollback_context = context;
    rollback_context.mutation_id = "rolled-back-save";
    const auto failed = registry.invoke(
        "images.alter", {{"imageId", opened->image_id}, {"expectedRevision", 1U}, {"manifest", {{"inline", edit}}}},
        rollback_context);
    ASSERT_FALSE(failed);
    EXPECT_EQ(provider->storage->bytes, original_bytes);
    const auto restored_samples = images.objects(opened->image_id, "owner", 100U, {}, "SBNK");
    ASSERT_TRUE(restored_samples) << restored_samples.error().message;
    ASSERT_EQ(restored_samples->items.size(), 1U);
    const auto restored_preview = images.preview(opened->image_id, "owner", restored_samples->items.front().id, 16U);
    ASSERT_TRUE(restored_preview) << restored_preview.error().message;
    ASSERT_FALSE(restored_preview->lanes.empty());
    const auto result = registry.invoke(
        "images.alter", {{"imageId", opened->image_id}, {"expectedRevision", 1U}, {"manifest", {{"inline", edit}}}},
        context);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result->at("revision").get<std::uint64_t>(), 2U);
    const auto operations = devices.operations("owner");
    ASSERT_TRUE(operations);
    ASSERT_EQ(operations->size(), 2U);
    const auto saved = std::ranges::find_if(
        *operations, [](const auto &operation) { return operation.operation_id == "known-device-save"; });
    ASSERT_NE(saved, operations->end());
    EXPECT_EQ(saved->state, "committed");
    const auto samples = images.objects(opened->image_id, "owner", 100U, {}, "SBNK");
    ASSERT_TRUE(samples);
    ASSERT_EQ(samples->items.size(), 1U);
    const auto detail = images.object_detail(opened->image_id, "owner", samples->items.front().id);
    ASSERT_TRUE(detail) << detail.error().message;
    const nlohmann::json parameters{
        {"schema_version", "1.0"},
        {"operations", nlohmann::json::array({{{"id", "parameters"},
                                               {"type", "update_sbnk_parameters"},
                                               {"partition_index", 0U},
                                               {"volume_name", "Edited"},
                                               {"sample_name", "Sample"},
                                               {"expected_payload_sha256", detail->at("editing").at("payloadSha256")},
                                               {"parameters", {{"level", 80U}}}}})}};
    context.mutation_id = "known-parameter-save";
    const auto parameter_save = registry.invoke(
        "images.alter",
        {{"imageId", opened->image_id}, {"expectedRevision", 2U}, {"manifest", {{"inline", parameters}}}}, context);
    ASSERT_TRUE(parameter_save) << parameter_save.error().message;
    ASSERT_TRUE(images.close(opened->image_id, "owner"));
    ASSERT_TRUE(devices.close(id, "owner"));
    const auto reopened_id = open(devices);
    source.device_session_id = reopened_id;
    const auto reopened = images.open(source, "owner");
    ASSERT_TRUE(reopened) << reopened.error().message;
    const auto root = images.content(reopened->image_id, "owner", 100U);
    ASSERT_TRUE(root);
    const auto contents = images.content(reopened->image_id, "owner", 100U, {}, root->items.front().id);
    ASSERT_TRUE(contents);
    EXPECT_TRUE(std::ranges::any_of(contents->items, [](const auto &item) { return item.name == "Edited"; }));
    const auto reopened_samples = images.objects(reopened->image_id, "owner", 100U, {}, "SBNK");
    ASSERT_TRUE(reopened_samples);
    ASSERT_EQ(reopened_samples->items.size(), 1U);
    const auto reopened_detail = images.object_detail(reopened->image_id, "owner", reopened_samples->items.front().id);
    ASSERT_TRUE(reopened_detail);
    EXPECT_EQ(reopened_detail->at("editing").at("parameters").at("level").get<unsigned>(), 80U);
    EXPECT_TRUE(images.preview(reopened->image_id, "owner", reopened_samples->items.front().id, 16U));
}
} // namespace
