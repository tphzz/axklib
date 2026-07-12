# Data Model

These are the stable model types shared by container readers, relationship
builders, validators, and exporters. They keep object identity, sampler-facing
placement, raw payload origin, and quality labels together.

```mermaid
classDiagram
    AxklibContainer --> AxklibContainerRef : ref
    AxklibContainer --> AxklibVolumeRef : volumes
    AxklibContainer --> AxklibObject : objects
    AxklibObject --> AxklibObjectRef : ref
    AxklibObject --> AxklibObjectHeader : header
    AxklibObject --> AxklibQuality : quality
    AxklibObject --> FieldValue : decoded fields
    FieldValue --> FieldQuality : quality
    DecodeIssue --> DataQuality : quality
    AxklibQuality --> DataQuality : level

    class AxklibContainer {
      kind
      source_path
      objects
    }
    class AxklibObject {
      type
      name
      payload_offset
      payload_size
    }
    class AxklibObjectRef {
      source_image
      scope_key
      object_key
    }
    class FieldValue {
      display_value
      raw_value
      offset
      size
    }
```

::: axklib.model
    options:
      members:
        - DataQuality
        - AxklibObjectFormat
        - AxklibObjectType
        - AxklibContainerKind
        - AxklibQuality
        - AxklibContainerRef
        - AxklibVolumeRef
        - AxklibObjectRef
        - AxklibObjectHeader
        - FieldQuality
        - FieldValue
        - DecodeIssue
        - AxklibObject