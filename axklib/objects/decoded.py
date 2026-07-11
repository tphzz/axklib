"""Decoded Yamaha object model and decode services."""

from __future__ import annotations

from dataclasses import dataclass, field

from axklib.containers import AxklibContainer
from axklib.model import AxklibObject, AxklibObjectFormat, DataQuality, DecodeIssue, FieldValue
from axklib.objects import current as object_current
from axklib.parameters import current as parameter_current


@dataclass(frozen=True)
class DecodedObject:
    object_key: str
    object_type: str
    name: str
    fields: dict[str, FieldValue[object]] = field(default_factory=dict)

    @property
    def decoded_kind(self) -> str:
        return self.__class__.__name__


@dataclass(frozen=True)
class DecodedSample(DecodedObject):
    pass


@dataclass(frozen=True)
class DecodedSampleBank(DecodedObject):
    pass


@dataclass(frozen=True)
class DecodedSampleBankAccessory(DecodedObject):
    pass


@dataclass(frozen=True)
class DecodedProgram(DecodedObject):
    pass


@dataclass(frozen=True)
class DecodedSequence(DecodedObject):
    pass


@dataclass(frozen=True)
class UnknownObject(DecodedObject):
    pass


@dataclass(frozen=True)
class ArtifactObject(DecodedObject):
    pass


@dataclass(frozen=True)
class DecodedObjectResult:
    raw_object: AxklibObject
    decoded: DecodedObject
    issues: tuple[DecodeIssue, ...] = ()


@dataclass(frozen=True)
class ObjectSet:
    results: tuple[DecodedObjectResult, ...]

    @property
    def issue_count(self) -> int:
        return sum(len(result.issues) for result in self.results)


def _field(
    name: str,
    value: object,
    *,
    raw_value: object | None = None,
    raw_offset: int | None = None,
    raw_size: int | None = None,
    quality: DataQuality = DataQuality.LIKELY,
    basis: str = "decoded object payload",
    notes: str = "",
    display_value: str = "",
) -> FieldValue[object]:
    return FieldValue(
        name=name,
        value=value,
        raw_value=value if raw_value is None else raw_value,
        raw_offset=raw_offset,
        raw_size=raw_size,
        quality=quality,
        basis=basis,
        notes=notes,
        display_value=display_value,
    )


def _payload_omitted_result(item: AxklibObject) -> DecodedObjectResult:
    issue = DecodeIssue(
        code="OBJECT_PAYLOAD_OMITTED",
        severity="info",
        object_key=item.object_key,
        message="Container was opened without payload bytes; only storage identity is decoded.",
        quality=DataQuality.KNOWN,
        basis="OpenOptions.include_payloads=False",
    )
    decoded = UnknownObject(
        object_key=item.object_key,
        object_type=item.type,
        name=item.name,
        fields={
            "payload_size": _field(
                "payload_size",
                item.payload_size,
                quality=DataQuality.KNOWN,
                basis="container metadata",
            )
        },
    )
    return DecodedObjectResult(raw_object=item, decoded=decoded, issues=(issue,))


def decode_smpl(item: AxklibObject) -> DecodedObjectResult:
    issues: list[DecodeIssue] = []
    if len(item.payload) < 0xAC:
        issues.append(
            DecodeIssue(
                code="OBJECT_BAD_HEADER_SIZE",
                severity="error",
                object_key=item.object_key,
                message=f"SMPL payload is too short for current compact metadata: {len(item.payload)} bytes",
                byte_start=0,
                byte_end=len(item.payload),
            )
        )
        return DecodedObjectResult(
            raw_object=item,
            decoded=DecodedSample(item.object_key, item.type, item.name),
            issues=tuple(issues),
        )

    metadata = object_current.decode_current_smpl_metadata(item.payload[:0xAC])
    fields = {
        "sample_rate": _field(
            "sample_rate",
            metadata.sample_rate_duplicate_0x07c,
            raw_offset=0x07C,
            raw_size=2,
            quality=DataQuality.LIKELY,
            basis="current SMPL compact metadata SMPL+0x07c",
        ),
        "root_key": _field(
            "root_key",
            metadata.root_key_midi_note_guess,
            raw_offset=0x07E,
            raw_size=1,
            quality=DataQuality.LIKELY,
            basis="current SMPL compact metadata SMPL+0x07e",
        ),
        "fine_tune": _field(
            "fine_tune",
            metadata.fine_tune_cents_guess,
            raw_offset=0x07F,
            raw_size=1,
            quality=DataQuality.LIKELY,
            basis="current SMPL compact metadata SMPL+0x07f",
        ),
        "loop_mode": _field(
            "loop_mode",
            metadata.loop_mode_candidate_0x085,
            raw_offset=0x085,
            raw_size=1,
            quality=DataQuality.KNOWN,
            basis="validated current SMPL loop-mode mapping",
            display_value=metadata.loop_mode_a4000_ui_label_guess,
        ),
        "loop_start": _field(
            "loop_start",
            metadata.loop_start_frame_0x096,
            raw_offset=0x096,
            raw_size=4,
            quality=DataQuality.LIKELY,
            basis="current SMPL compact metadata SMPL+0x096",
        ),
        "loop_length": _field(
            "loop_length",
            metadata.loop_length_frames_0x09a,
            raw_offset=0x09A,
            raw_size=4,
            quality=DataQuality.LIKELY,
            basis="current SMPL compact metadata SMPL+0x09a",
        ),
    }
    return DecodedObjectResult(
        raw_object=item,
        decoded=DecodedSample(item.object_key, item.type, item.name, fields),
        issues=tuple(issues),
    )


def decode_sbnk(item: AxklibObject) -> DecodedObjectResult:
    issues: list[DecodeIssue] = []
    try:
        members = parameter_current.decode_current_sbnk_members(item.payload)
    except Exception as exc:
        issues.append(
            DecodeIssue(
                code="OBJECT_DECODE_FAILED",
                severity="error",
                object_key=item.object_key,
                message=str(exc),
            )
        )
        members = None
    fields: dict[str, FieldValue[object]] = {}
    if members is not None:
        fields["bank_topology"] = _field(
            "bank_topology",
            members.bank_topology,
            quality=DataQuality.LIKELY,
            basis="current SBNK member window decoder",
        )
        fields["left_sample_name"] = _field(
            "left_sample_name",
            members.left.sample_name,
            raw_offset=members.left.sample_name_offset,
            raw_size=16,
            quality=DataQuality.LIKELY,
            basis="current SBNK left member name field",
        )
        fields["left_smpl_link_id"] = _field(
            "left_smpl_link_id",
            members.left.smpl_link_id,
            raw_offset=members.left.link_offset,
            raw_size=4,
            quality=DataQuality.LIKELY,
            basis="current SBNK left member link field",
        )
    return DecodedObjectResult(
        raw_object=item,
        decoded=DecodedSampleBank(item.object_key, item.type, item.name, fields),
        issues=tuple(issues),
    )


def decode_sbac(item: AxklibObject) -> DecodedObjectResult:
    issues: list[DecodeIssue] = []
    fields: dict[str, FieldValue[object]] = {}
    try:
        decoded = parameter_current.decode_current_sbac_fields(item.payload)
        fields["active_slot_count"] = _field(
            "active_slot_count",
            decoded.active_slot_count_0x144,
            raw_offset=0x144,
            raw_size=1,
            quality=DataQuality.LIKELY,
            basis="current SBAC slot count field",
        )
        fields["max_slot_count_from_payload"] = _field(
            "max_slot_count_from_payload",
            decoded.max_slot_count_from_payload,
            quality=DataQuality.KNOWN,
            basis="derived from SBAC payload size and slot stride",
        )
        if decoded.active_slot_count_0x144 > decoded.max_slot_count_from_payload:
            issues.append(
                DecodeIssue(
                    code="OBJECT_IMPOSSIBLE_SLOT_CAPACITY",
                    severity="warning",
                    object_key=item.object_key,
                    message="SBAC active slot count exceeds recovered payload capacity",
                    byte_start=0x144,
                    byte_end=0x145,
                )
            )
    except Exception as exc:
        issues.append(
            DecodeIssue(
                code="OBJECT_DECODE_FAILED",
                severity="error",
                object_key=item.object_key,
                message=str(exc),
            )
        )
    return DecodedObjectResult(
        raw_object=item,
        decoded=DecodedSampleBankAccessory(item.object_key, item.type, item.name, fields),
        issues=tuple(issues),
    )


def decode_prog(item: AxklibObject) -> DecodedObjectResult:
    issues: list[DecodeIssue] = []
    fields: dict[str, FieldValue[object]] = {}
    try:
        common = parameter_current.decode_prog_common_fields(item.payload)
        fields["control_record_count"] = _field(
            "control_record_count",
            len(common.control_records),
            quality=DataQuality.LIKELY,
            basis="current PROG common/control decoder",
        )
    except Exception as exc:
        issues.append(
            DecodeIssue(
                code="OBJECT_DECODE_FAILED",
                severity="error",
                object_key=item.object_key,
                message=str(exc),
            )
        )
    return DecodedObjectResult(
        raw_object=item,
        decoded=DecodedProgram(item.object_key, item.type, item.name, fields),
        issues=tuple(issues),
    )


def decode_object(item: AxklibObject) -> DecodedObjectResult:
    if item.object_format == AxklibObjectFormat.ALTERNATING_BYTE_ARTIFACT:
        issue = DecodeIssue(
            code="OBJECT_ARTIFACT_SALVAGE_REQUIRED",
            severity="info",
            object_key=item.object_key,
            message="Object is classified as an artifact/salvage format and is not decoded as normal current data.",
            quality=DataQuality.KNOWN,
            basis="object format classification",
        )
        return DecodedObjectResult(
            raw_object=item,
            decoded=ArtifactObject(item.object_key, item.type, item.name),
            issues=(issue,),
        )
    if not item.payload:
        return _payload_omitted_result(item)
    if not item.payload.startswith(object_current.OBJECT_MAGIC):
        issue = DecodeIssue(
            code="OBJECT_BAD_MAGIC",
            severity="error",
            object_key=item.object_key,
            message="Object payload does not start with FSFSDEV3SPLX magic.",
            byte_start=0,
            byte_end=min(len(item.payload), len(object_current.OBJECT_MAGIC)),
        )
        return DecodedObjectResult(
            raw_object=item,
            decoded=UnknownObject(item.object_key, item.type, item.name),
            issues=(issue,),
        )
    if item.type == "SMPL":
        return decode_smpl(item)
    if item.type == "SBNK":
        return decode_sbnk(item)
    if item.type == "SBAC":
        return decode_sbac(item)
    if item.type == "PROG":
        return decode_prog(item)
    if item.type == "SEQU":
        return DecodedObjectResult(
            raw_object=item,
            decoded=DecodedSequence(item.object_key, item.type, item.name),
        )
    return DecodedObjectResult(
        raw_object=item,
        decoded=UnknownObject(item.object_key, item.type, item.name),
    )


def decode_objects(container: AxklibContainer) -> ObjectSet:
    return ObjectSet(tuple(decode_object(item) for item in container.objects))


def result_issue_codes(result: DecodedObjectResult) -> str:
    return ";".join(issue.code for issue in result.issues)


def result_field_names(result: DecodedObjectResult) -> str:
    return ";".join(sorted(result.decoded.fields))
