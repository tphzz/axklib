#include "axklib/alteration.hpp"

#include <array>

namespace axk {

std::string_view operation_type_name(const AlterationOperationData &operation) noexcept {
    constexpr std::array names{
        std::string_view{"delete_volume"},
        std::string_view{"insert_volume"},
        std::string_view{"delete_sbnk"},
        std::string_view{"insert_sbnk"},
        std::string_view{"update_sbnk_parameters"},
        std::string_view{"insert_waveform"},
        std::string_view{"delete_waveform"},
        std::string_view{"rename_waveform"},
        std::string_view{"rename_sbnk"},
        std::string_view{"delete_sbac"},
        std::string_view{"insert_sbac"},
        std::string_view{"assign_sbac_members"},
        std::string_view{"rename_sbac"},
        std::string_view{"delete_program"},
        std::string_view{"insert_program"},
        std::string_view{"rename_program"},
        std::string_view{"delete_sequence"},
        std::string_view{"insert_sequence"},
        std::string_view{"rename_sequence"},
        std::string_view{"rename_volume"},
        std::string_view{"rename_partition"},
        std::string_view{"repair_object_placements"},
        std::string_view{"import_tx16w_disk_set"},
        std::string_view{"clear_program_assignments"},
    };
    return names[operation.index()];
}

} // namespace axk
