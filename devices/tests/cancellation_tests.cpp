#include "rawdisk/cancellation.hpp"
#include "test_util.hpp"

#include <thread>

using namespace rawdisk;

int main() {
    CHECK(!CancellationToken{}.is_cancelled());

    CancellationSource source;
    const auto token = source.token();
    const auto copied = token;
    CHECK(!token.is_cancelled());
    source.cancel();
    CHECK(token.is_cancelled());
    CHECK(copied.is_cancelled());
    source.cancel();
    CHECK(token.is_cancelled());

    CancellationToken retained;
    {
        CancellationSource scoped;
        retained = scoped.token();
        scoped.cancel();
    }
    CHECK(retained.is_cancelled());

    CancellationSource independent;
    CHECK(!independent.token().is_cancelled());

    CancellationSource threaded;
    const auto cross_thread = threaded.token();
    std::thread worker([&] { threaded.cancel(); });
    worker.join();
    CHECK(cross_thread.is_cancelled());

    return finish_tests();
}
