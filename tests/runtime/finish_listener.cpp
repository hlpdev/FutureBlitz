#include "internal.hpp"

#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

namespace {

/// Ends what the runtime started once the tests are done, as a program's end does, so the I/O
/// thread stops before its std::thread goes.
class finish_runtime_t : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunEnded(const Catch::TestRunStats& /*stats*/) override {
        fb::runtime::run_finish();
    }
};

} // namespace

CATCH_REGISTER_LISTENER(finish_runtime_t)
