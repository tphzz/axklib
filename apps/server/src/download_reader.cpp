#include "download_reader.hpp"

#include <limits>

axk::app::Result<std::vector<std::byte>>
axk::server::detail::read_verified_download(const app::SandboxFile &file, std::uint64_t offset, std::uint64_t length,
                                            std::shared_ptr<const DownloadReadHooks> hooks) {
    if (offset > file.size || length > file.size - offset ||
        length > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return std::unexpected{app::Error{"file_unavailable", "sandbox file range is outside the retained file"}};
    }
    if (hooks && hooks->after_open_before_read)
        hooks->after_open_before_read();
    std::vector<std::byte> bytes(static_cast<std::size_t>(length));
    if (const auto read = file.reader->read_exact_at(offset, bytes); !read) {
        return std::unexpected{app::Error{"file_unavailable", "sandbox file content cannot be read"}};
    }
    if (const auto unchanged = file.verify_unchanged(); !unchanged)
        return std::unexpected{unchanged.error()};
    return bytes;
}
