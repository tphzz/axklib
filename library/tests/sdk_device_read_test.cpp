#include "sdk_device_internal.hpp"
#include "sdk_internal.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "axklib/application/secure_random.hpp"
#include "axklib/media.hpp"

namespace {

class Transport final : public axk::app::DeviceTransport {
  public:
    std::vector<std::byte> bytes;
    std::vector<std::byte> layout;
    mutable std::size_t checks{};
    mutable std::size_t reads{};
    bool connected{true};
    bool fail_reads{};
    bool writable{};
    bool allow_writes{};
    std::function<void()> on_verify;
    std::uint64_t size() const noexcept override { return bytes.size(); }
    std::uint32_t sector_size() const noexcept override { return 512U; }
    std::string carrier_identity() const override { return "sdk-test-carrier"; }
    std::string media_identity() const override { return "sdk-test-media"; }
    axk::app::Result<void> verify_bound() const override {
        ++checks;
        if (on_verify)
            on_verify();
        if (!connected)
            return std::unexpected(axk::app::Error{"device_changed", "Synthetic disconnection"});
        return {};
    }
    axk::app::Result<void> verify_read_bound() const override {
        if (auto bound = verify_bound(); !bound)
            return bound;
        // Model the provider's fresh carrier-layout guard, not the SDK page cache.
        if (fail_reads || !std::equal(layout.begin(), layout.end(), bytes.begin()))
            return std::unexpected(axk::app::Error{"device_changed", "Synthetic layout guard failed"});
        return {};
    }
    axk::app::Result<void> authorize_writes() override {
        if (allow_writes) {
            writable = true;
            return {};
        }
        ADD_FAILURE() << "SDK read tests must not request writes";
        return std::unexpected(axk::app::Error{"read_only", "Synthetic read-only carrier"});
    }
    bool has_write_handle() const override { return writable; }
    axk::app::Result<void> write_exact_at(std::uint64_t offset, std::span<const std::byte> data) override {
        if (writable && offset <= size() && data.size() <= size() - offset) {
            std::ranges::copy(data, bytes.begin() + static_cast<std::ptrdiff_t>(offset));
            return {};
        }
        ADD_FAILURE() << "SDK read tests must not write";
        return std::unexpected(axk::app::Error{"read_only", "Synthetic read-only carrier"});
    }
    axk::app::Result<void> flush() override { return {}; }
    axk::Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> output) const override {
        ++reads;
        if (fail_reads || offset > size() || output.size() > size() - offset)
            return std::unexpected(
                axk::make_error(axk::ErrorCode::io_read_failed, axk::ErrorCategory::io, "Synthetic read failure"));
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), output.size(), output.begin());
        return {};
    }
};

class Provider final : public axk::app::DeviceProvider {
  public:
    std::shared_ptr<Transport> transport = std::make_shared<Transport>();
    axk::app::DeviceCandidate candidate() const {
        return {"sdk-test", "Synthetic SDK disk", "sdk-serial", transport->size(), 512U};
    }
    axk::app::Result<std::vector<axk::app::DeviceCandidate>> list() override { return std::vector{candidate()}; }
    axk::app::Result<axk::app::ConnectedDevice> connect(std::string_view, std::string_view, std::string_view) override {
        transport->layout.assign(transport->bytes.begin(), transport->bytes.begin() + 1024);
        return axk::app::ConnectedDevice{
            transport, candidate(), {{"disk", "Whole disk", 0U, transport->size(), 512U, false, {}}}};
    }
};

class SdkDeviceReads : public testing::Test {
  protected:
    std::filesystem::path directory =
        std::filesystem::temp_directory_path() / ("axk-sdk-device-" + *axk::app::secure_random_hex(8U));
    std::shared_ptr<Provider> provider = std::make_shared<Provider>();
    axk::device_host host;
    axk::image image;
    axk::operation_context context;
    std::string session_id;
    std::string wave;
    std::string volume;

    void SetUp() override {
        const auto path = std::filesystem::path{AXK_SOURCE_ROOT} / "tests/fixtures/images/sampler-authored" /
                          "HD00_512_single_sbnk_authored.hds";
        std::ifstream input{path, std::ios::binary | std::ios::ate};
        ASSERT_TRUE(input);
        provider->transport->bytes.resize(static_cast<std::size_t>(input.tellg()));
        input.seekg(0);
        input.read(reinterpret_cast<char *>(provider->transport->bytes.data()),
                   static_cast<std::streamsize>(provider->transport->bytes.size()));
        ASSERT_TRUE(input);
        auto created = axk::device_host_factory::create(directory, provider, true);
        ASSERT_TRUE(created) << created.error().message;
        host = std::move(*created);
        ASSERT_TRUE(host.list());
        const auto connected = host.connect("sdk-test", "raw");
        ASSERT_TRUE(connected) << connected.error().message;
        session_id = connected->id;
        auto opened = host.open_image(session_id, "disk", context);
        ASSERT_TRUE(opened) << opened.error().message;
        image = std::move(*opened);
        const auto objects = image.objects(0U, 100U, context);
        ASSERT_TRUE(objects);
        const auto found = std::ranges::find(objects->items, "SMPL", &axk::object_info::type);
        ASSERT_NE(found, objects->items.end());
        wave = found->key;
        volume = found->volume_name;
    }
    void TearDown() override {
        image = {};
        host = {};
        std::filesystem::remove_all(directory);
    }
};

TEST_F(SdkDeviceReads, EachPayloadOperationVerifiesOnceEvenWithWarmPages) {
    ASSERT_TRUE(image.waveform_pcm(wave, context));
    auto before = provider->transport->checks;
    ASSERT_TRUE(image.preview(wave, 8U, context));
    EXPECT_EQ(provider->transport->checks, before + 1U);
    before = provider->transport->checks;
    ASSERT_TRUE(image.waveform_pcm(wave, context));
    EXPECT_EQ(provider->transport->checks, before + 1U);
    before = provider->transport->checks;
    ASSERT_TRUE(image.export_audio((directory / "export").string(), {}, context));
    EXPECT_EQ(provider->transport->checks, before + 1U);
    before = provider->transport->checks;
    const auto reads = provider->transport->reads;
    const auto snapshot = image.make_snapshot();
    ASSERT_TRUE(snapshot);
    EXPECT_TRUE(snapshot->objects(0U, 10U));
    EXPECT_EQ(provider->transport->checks, before);
    EXPECT_EQ(provider->transport->reads, reads);
}

TEST_F(SdkDeviceReads, WarmCacheCannotHideDisconnection) {
    ASSERT_TRUE(image.waveform_pcm(wave, context));
    provider->transport->connected = false;
    EXPECT_FALSE(image.preview(wave, 8U, context));
    EXPECT_FALSE(image.waveform_pcm(wave, context));
    EXPECT_FALSE(image.export_audio((directory / "export").string(), {}, context));
    EXPECT_FALSE(std::filesystem::exists(directory / "export"));
}

TEST_F(SdkDeviceReads, WarmCacheCannotHideLayoutReplacement) {
    ASSERT_TRUE(image.waveform_pcm(wave, context));
    provider->transport->bytes.front() ^= std::byte{1};
    EXPECT_FALSE(image.preview(wave, 8U, context));
    EXPECT_FALSE(image.waveform_pcm(wave, context));
    EXPECT_FALSE(image.export_audio((directory / "export").string(), {}, context));
}

TEST_F(SdkDeviceReads, ColdGuardFailureAndCancellationReleaseAccess) {
    context.cancel();
    const auto before = provider->transport->checks;
    EXPECT_FALSE(image.waveform_pcm(wave, context));
    EXPECT_EQ(provider->transport->checks, before);
    context.reset_cancel();
    EXPECT_TRUE(image.waveform_pcm(wave, context));
    provider->transport->fail_reads = true;
    EXPECT_FALSE(image.preview(wave, 8U, context));
    const auto status = host.status(session_id);
    ASSERT_TRUE(status);
    EXPECT_TRUE(status->invalidated);
}

TEST_F(SdkDeviceReads, ImageRetainsServicesAcrossHostMoveAndDestructionWithoutHoldingAnAccessLock) {
    auto moved = std::move(host);
    EXPECT_TRUE(moved.status(session_id));
    moved = {};
    EXPECT_TRUE(image.waveform_pcm(wave, context));
    const std::weak_ptr<Provider> lifetime = provider;
    provider.reset();
    EXPECT_FALSE(lifetime.expired());
    image = {};
    EXPECT_TRUE(lifetime.expired());
}

TEST_F(SdkDeviceReads, PayloadLeasesSerializeAndReleaseAfterCompletion) {
    using namespace std::chrono_literals;
    std::promise<void> entered;
    std::promise<void> release;
    const auto gate = release.get_future().share();
    std::atomic_uint calls{};
    provider->transport->on_verify = [&] {
        if (++calls == 1U) {
            entered.set_value();
            gate.wait();
        }
    };
    auto first = std::async(std::launch::async, [&] {
        axk::operation_context read;
        return image.preview(wave, 8U, read);
    });
    const auto started = entered.get_future().wait_for(2s);
    if (started != std::future_status::ready) {
        release.set_value();
        static_cast<void>(first.get());
        FAIL() << "Payload did not acquire verified access";
    }
    std::promise<void> second_started;
    auto second = std::async(std::launch::async, [&] {
        axk::operation_context read;
        second_started.set_value();
        return image.waveform_pcm(wave, read);
    });
    second_started.get_future().wait();
    EXPECT_EQ(second.wait_for(40ms), std::future_status::timeout);
    release.set_value();
    EXPECT_TRUE(first.get());
    EXPECT_TRUE(second.get());
    EXPECT_EQ(calls, 2U);
    provider->transport->on_verify = {};
    EXPECT_TRUE(host.status(session_id));
}

TEST_F(SdkDeviceReads, CarrierCloseRequiresReleasingImages) {
    EXPECT_FALSE(host.close(session_id));
    image = {};
    EXPECT_TRUE(host.close(session_id));
}

TEST_F(SdkDeviceReads, OldImageKeepsItsRevisionAfterAnAppliedEdit) {
    provider->transport->allow_writes = true;
    ASSERT_TRUE(host.authorize_writes(session_id, "disk", true));
    const nlohmann::json manifest{{"schema_version", "1.0"},
                                  {"operations", nlohmann::json::array({{{"id", "rename"},
                                                                         {"type", "rename_volume"},
                                                                         {"partition_index", 0U},
                                                                         {"volume_name", volume},
                                                                         {"new_volume_name", "SDK rename"}}})}};
    const auto applied = host.apply(session_id, "disk", "sdk-revision-test", manifest.dump(), context);
    ASSERT_TRUE(applied) << applied.error().message;
    const auto stale = image.waveform_pcm(wave, context);
    ASSERT_FALSE(stale);
    EXPECT_NE(stale.error().message.find("image_revision_stale"), std::string::npos);
    auto reopened = host.open_image(session_id, "disk", context);
    ASSERT_TRUE(reopened) << reopened.error().message;
    EXPECT_TRUE(reopened->waveform_pcm(wave, context));
}

TEST_F(SdkDeviceReads, VerifiedLeaseStaysAliveDuringEveryPayloadRead) {
    class Reader final : public axk::RandomAccessReader {
      public:
        axk::MemoryReader data;
        bool watch{};
        bool active{};
        mutable std::size_t reads{};
        explicit Reader(std::vector<std::byte> bytes) : data(std::move(bytes)) {}
        std::uint64_t size() const noexcept override { return data.size(); }
        axk::Result<void> read_exact_at(std::uint64_t at, std::span<std::byte> bytes) const override {
            if (watch) {
                ++reads;
                EXPECT_TRUE(active) << "Operation access was released before its payload read";
            }
            return data.read_exact_at(at, bytes);
        }
    };
    auto reader = std::make_shared<Reader>(provider->transport->bytes);
    auto media = axk::open_media(reader, "test.hds");
    ASSERT_TRUE(media);
    auto checked = axk::image_factory::open(std::move(*media), context, [reader](const auto &) {
        reader->active = true;
        return axk::result<std::shared_ptr<void>>{std::shared_ptr<void>{new int{}, [reader](void *value) {
                                                                            delete static_cast<int *>(value);
                                                                            reader->active = false;
                                                                        }}};
    });
    ASSERT_TRUE(checked);
    reader->watch = true;
    const auto read = [&](auto operation) {
        reader->reads = 0U;
        EXPECT_TRUE(operation());
        EXPECT_GT(reader->reads, 0U);
        EXPECT_FALSE(reader->active);
    };
    read([&] { return checked->preview(wave, 8U, context); });
    read([&] { return checked->waveform_pcm(wave, context); });
    read([&] { return checked->export_audio((directory / "guarded-export").string(), {}, context); });
}

} // namespace
