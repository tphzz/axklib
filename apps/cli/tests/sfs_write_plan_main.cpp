#include "sfs_write_plan.hpp"

#include "axklib/error.hpp"

#include <exception>
#include <format>
#include <iostream>

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "usage: axk_sfs_write_plan <verified-full-target-backup.img> <new-write.plan>\n"
                     "Read-only test preparation. Never opens hardware or modifies SFS headers.\n";
        return 2;
    }
    try {
        const auto plan = axk::test::sfs_write::prepare(argv[1]);
        if (!plan) {
            std::cerr << axk::render_error(plan.error()) << '\n';
            return 1;
        }
        if (const auto saved = axk::test::sfs_write::save(argv[2], *plan); !saved) {
            std::cerr << axk::render_error(saved.error()) << '\n';
            return 1;
        }
        std::cout << std::format("Target: {} sectors of 512 bytes\nEligible free range: [{}, {})\n"
                                 "Write LBA: {} (byte offset {})\nMetadata guards: {}\nPlan: {}\n",
                                 plan->sector_count, plan->free_begin, plan->free_end, plan->lba, plan->lba * 512U,
                                 plan->guards.size(), argv[2]);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
