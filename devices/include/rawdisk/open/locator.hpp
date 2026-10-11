// Locators: a replayable, human-readable description of a completed flow.
//
//   scsi2sd-v5?device.device=usb%3A...&volume.volume=3&xml.board=v5&xml.file=%2Fhome%2Fme%2Fboard.xml
//
// Grammar: <mode-id>[?<key>=<value>(&<key>=<value>)*]. Keys are "step.field" ([a-z0-9_.-]),
// values are percent-encoded (RFC 3986). Parsing is strict: bad escapes, duplicate or empty keys
// and unexpected characters are errors (Errc::locator_invalid). Replaying runs the flow with the
// same checks as an interactive one; a locator can never bypass them.
#pragma once

#include "rawdisk/open/flow.hpp"
#include "rawdisk/open/modes.hpp"

#include <string>
#include <string_view>

namespace rawdisk::open {

struct Locator {
    std::string mode;
    Answers answers; // "step.field" -> value
};

std::string format_locator(const Locator &locator); // canonical: keys sorted
Result<Locator> parse_locator(std::string_view text) noexcept;

// parse + find the mode + run().
Outcome<Opened> open(std::string_view locator, Access access, const Registry &registry = builtin_modes(),
                     Context context = {}, CancellationToken stop = {});

} // namespace rawdisk::open
