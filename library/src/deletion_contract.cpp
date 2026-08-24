#include "axklib/deletion.hpp"

std::string_view axk::object_deletion_role_name(ObjectDeletionRole role) noexcept {
    switch (role) {
    case ObjectDeletionRole::target:
        return "TARGET";
    case ObjectDeletionRole::referrer:
        return "REFERRER";
    case ObjectDeletionRole::dependency:
        return "DEPENDENCY";
    }
    return "DEPENDENCY";
}

std::string_view axk::object_deletion_status_name(ObjectDeletionStatus status) noexcept {
    switch (status) {
    case ObjectDeletionStatus::required:
        return "REQUIRED";
    case ObjectDeletionStatus::optional:
        return "OPTIONAL";
    case ObjectDeletionStatus::preserved:
        return "PRESERVED";
    case ObjectDeletionStatus::blocked:
        return "BLOCKED";
    }
    return "BLOCKED";
}

std::string_view axk::object_deletion_reference_effect_name(ObjectDeletionReferenceEffect effect) noexcept {
    switch (effect) {
    case ObjectDeletionReferenceEffect::blocking:
        return "BLOCKING";
    case ObjectDeletionReferenceEffect::removed:
        return "REMOVED";
    case ObjectDeletionReferenceEffect::preserved:
        return "PRESERVED";
    }
    return "PRESERVED";
}
