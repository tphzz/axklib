# Objects

Object helpers decode generic Yamaha object headers and current-format `SMPL`
metadata. Raw current `SBNK`, `SBAC`, and `PROG` parameter windows are documented
under **Diagnostic APIs** for callers that need lower-level inspection data.

::: axklib.objects.current
    options:
      members:
        - CurrentSmplMetadata
        - summarize_object_header
        - summarize_current_smpl_compact_metadata
        - decode_current_smpl_metadata
