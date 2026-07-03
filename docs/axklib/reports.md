# Reports

Report helpers serialize dataclass rows and schema manifests for CSV/JSON
outputs. These helpers keep user-facing report schemas explicit and testable.

::: axklib.reports
    options:
      members:
        - to_plain
        - row_to_dict
        - write_json
        - write_rows_json
        - write_csv
        - write_dict_csv

::: axklib.reports.schema
    options:
      members:
        - ReportColumnSchema
        - ReportSchemaManifest
        - make_schema_manifest
        - write_schema_manifest
        - write_schema_index