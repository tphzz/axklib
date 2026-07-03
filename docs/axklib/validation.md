# Validation

Validation APIs report container, object, relationship, export, and sidecar
problems with stable issue codes. CLI commands can surface these reports
as structured output instead of treating parser failures as plain text.

::: axklib.validation
    options:
      members:
        - ValidationSeverity
        - ValidationScope
        - ValidationIssue
        - ValidationReport
        - validation_failed
        - validate_container
        - validate_path_results
        - validate_paths
        - validate_export_sidecars