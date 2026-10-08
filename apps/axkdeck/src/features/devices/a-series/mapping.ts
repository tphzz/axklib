import type { ObjectEditorDocument } from '../../object-editor/workflow.svelte';
import type { EditorValues } from '../../object-editor/draft.svelte';
import type { KeyboardRange, KeyboardZone, RangeBoundary } from '../../object-editor/keyboardMapping';
import {
    documentMappingState,
    mappingFingerprint,
    type MappingModel,
    type MappingWorkflow,
} from '../../program-mapping/model';
import { validRange, type MappingRole } from '../../program-mapping/protocol';
import { ProgramDraft } from './program/draft.svelte';
import { mappingLimits, mappingPatch, mappingZones } from './program/mappingModel';
import { BankDraft } from './bank/draft.svelte';
import { mappingMembers } from './bank/mappingMembers';
import { sampleMappingRange, sampleMappingPatch, sampleMappingAxes, rangeParameters } from './sample/mapping';

function actions(document: ObjectEditorDocument, workflow: MappingWorkflow) {
    return {
        undo: () => document.draft.undo(),
        redo: () => document.draft.redo(),
        save: () => workflow.save(document),
        discard: () => workflow.discard(document),
        recover: () => workflow.recover(document),
        inherit: (_boundary: RangeBoundary) => {},
        root: (_note: number) => {},
    };
}
function effectiveMember(bank: BankDraft, values: EditorValues): EditorValues {
    const effective = { ...values };
    for (const unit of bank.units)
        for (const key of unit.keys) if (bank.isOverridden(key)) effective[key] = bank.storedValues[key]!;
    return effective;
}
export function aSeriesMapping(
    role: MappingRole,
    document: ObjectEditorDocument,
    workflow: MappingWorkflow,
): MappingModel | null {
    const snapshot = document.detail?.editing;
    if (!snapshot) return null;
    if (role === 'program') {
        if (snapshot.profile !== 'a-series/program' || !(document.draft instanceof ProgramDraft)) return null;
        const draft = document.draft;
        const assignments = draft.assignments.filter((row) => row.name);
        const assignment = assignments.find((row) => row.id === document.programAssignmentId) ?? assignments[0];
        const limits = assignment ? mappingLimits(draft.values, assignment.id) : null;
        const state = documentMappingState(document, workflow);
        const names = new Map<string, number>();
        for (const row of assignments)
            names.set(`${row.kind}:${row.name}`, (names.get(`${row.kind}:${row.name}`) ?? 0) + 1);
        return {
            ...actions(document, workflow),
            fingerprint: mappingFingerprint([document]),
            data: {
                ...state,
                title: snapshot.programName,
                selectionLabel: 'Sample/Bank',
                selectionId: assignment?.id ?? null,
                selections: assignments.map((row) => ({
                    value: row.id,
                    label: `${row.name} (${row.kind === 'SBAC' ? 'Bank' : 'Sample'})${names.get(`${row.kind}:${row.name}`)! > 1 ? ` · ${row.id + 1}` : ''}`,
                })),
                targets: assignments.map((row) => {
                    const limits = mappingLimits(draft.values, row.id);
                    return {
                        ...state,
                        id: row.id,
                        limits: validRange(limits) ? limits : null,
                        editableAxes: { keys: true, velocity: true },
                        editable: state.editable && row.kind !== 'UNKNOWN' && validRange(limits),
                    };
                }),
                zones: mappingZones(snapshot, assignments, draft.values, assignment?.id ?? -1, (id) => {
                    const sample = workflow.find?.(document.sessionId, id);
                    return sample?.detail?.image.revision === document.detail?.image.revision ? sample : undefined;
                }),
                limits: limits && validRange(limits) ? limits : null,
                editable: state.editable && assignment?.kind !== 'UNKNOWN' && !!limits && validRange(limits),
                editableAxes: { keys: true, velocity: true },
                rootEditable: false,
                rangeLabel: 'Program limits',
                overrides: [],
            },
            select: (id) => {
                document.programAssignmentId = id;
            },
            patch: (range, boundaries, id = assignment!.id) => draft.patch(mappingPatch(id, range, boundaries)),
        };
    }
    if (role === 'sample') {
        if (snapshot.profile !== 'a-series/sample') return null;
        const limits = sampleMappingRange(document.draft.values);
        const state = documentMappingState(document, workflow);
        return {
            ...actions(document, workflow),
            fingerprint: mappingFingerprint([document]),
            data: {
                ...state,
                title: document.detail!.object.name,
                selectionLabel: 'Sample',
                selectionId: 0,
                selections: [],
                targets: [
                    {
                        ...state,
                        id: 0,
                        limits,
                        editableAxes: sampleMappingAxes(document),
                        editable: state.editable && !!limits,
                    },
                ],
                limits,
                zones: limits
                    ? [
                          {
                              ...limits,
                              id: 'sample',
                              selectionId: 0,
                              label: document.detail!.object.name,
                              selected: true,
                              root: Number(document.draft.values.root_key),
                          },
                      ]
                    : [],
                editable: state.editable && !!limits,
                editableAxes: sampleMappingAxes(document),
                rootEditable:
                    state.editable &&
                    'root_key' in document.draft.values &&
                    !snapshot.blockedParameters.includes('root_key'),
                rangeLabel: 'Sample range',
                overrides: [],
            },
            select: () => {},
            patch: (range, boundaries) => document.draft.patch(sampleMappingPatch(range, boundaries)),
            root: (note) => {
                if (!sampleMappingRange({ ...document.draft.values, root_key: note }))
                    throw new Error("This root would invert the Sample's Orig key range.");
                document.draft.endGesture();
                document.draft.set('root_key', note);
            },
        };
    }
    if (snapshot.profile !== 'a-series/sample-bank' || !(document.draft instanceof BankDraft) || !workflow.members)
        return null;
    const bank = document.draft;
    const members = mappingMembers(document.detail!);
    const documents = workflow.members
        .documents(document)
        .filter((member) => member.detail?.image.revision === document.detail?.image.revision);
    const byId = new Map(documents.map((member) => [member.detail!.object.id, member]));
    const index = Math.max(
        0,
        members.findIndex((member) => member.id === document.previewMemberId),
    );
    const selected = byId.get(members[index]?.id ?? '');
    const isBank = role === 'bank';
    const owner = isBank ? document : selected;
    const stored = selected?.draft.storedValues ?? {};
    const values = isBank
        ? {
              ...stored,
              velocity_low: bank.isOverridden('velocity_low') ? bank.storedValues.velocity_low! : 0,
              velocity_high: bank.isOverridden('velocity_high') ? bank.storedValues.velocity_high! : 127,
          }
        : stored;
    const limits = sampleMappingRange(values);
    const recovering = workflow.members.contains(document);
    const state = documentMappingState(recovering ? document : (owner ?? document), workflow);
    const zones: KeyboardZone[] = members.flatMap((member, memberIndex) => {
        const sourceValues = byId.get(member.id)?.draft.storedValues;
        if (!sourceValues) return [];
        const source = sampleMappingRange(sourceValues),
            effective = isBank ? sampleMappingRange(effectiveMember(bank, sourceValues)) : source;
        if (!source) return [];
        return [
            {
                ...(effective ?? source),
                empty: !effective,
                source: isBank ? source : undefined,
                id: member.id,
                selectionId: memberIndex,
                label: member.name,
                root: Number(sourceValues.root_key),
                selected: memberIndex === index,
            },
        ];
    });
    const changed = workflow.members.changed(document);
    const axes = isBank
        ? {
              keys: false,
              velocity:
                  sampleMappingAxes(document).velocity && !!bank.unit('velocity_low') && !!bank.unit('velocity_high'),
          }
        : selected
          ? sampleMappingAxes(selected)
          : { keys: false, velocity: false };
    const unresolved = (snapshot.bankOverrides?.members ?? []).filter(
        (member) => !members.some((known) => member.objectId === known.id),
    ).length;
    const pending = members.length - documents.length;
    const currentOwner = () => (isBank ? document : (byId.get(document.previewMemberId ?? '') ?? selected));
    return {
        ...actions(owner ?? document, workflow),
        undo: () => currentOwner()?.draft.undo(),
        redo: () => currentOwner()?.draft.redo(),
        fingerprint: mappingFingerprint([document, ...documents]),
        data: {
            ...state,
            title: `${isBank ? 'Bank' : 'Members of'} ${document.detail!.object.name}`,
            selectionLabel: isBank ? 'Preview sample' : 'Sample',
            selectionId: selected ? index : null,
            selections: members.map((member, index) => ({
                value: index,
                label: member.name,
                disabled: !byId.has(member.id),
            })),
            targets: members.flatMap((member, id) => {
                const sample = byId.get(member.id);
                if (!sample) return [];
                const targetState = documentMappingState(isBank || recovering ? document : sample, workflow);
                const targetLimits = sampleMappingRange(
                    isBank
                        ? {
                              ...sample.draft.storedValues,
                              velocity_low: values.velocity_low!,
                              velocity_high: values.velocity_high!,
                          }
                        : sample.draft.storedValues,
                );
                const targetAxes = isBank ? axes : sampleMappingAxes(sample);
                return [
                    {
                        ...targetState,
                        id,
                        limits: targetLimits,
                        editableAxes: targetAxes,
                        editable: targetState.editable && !!targetLimits && (targetAxes.keys || targetAxes.velocity),
                        canSave: isBank ? targetState.canSave : workflow.members!.canSave(document),
                        canDiscard: isBank ? targetState.canDiscard : !workflow.locked && changed.length > 0,
                    },
                ];
            }),
            limits,
            zones,
            editableAxes: axes,
            rootEditable: false,
            rangeLabel: isBank ? 'Bank overrides' : 'Stored Sample range',
            overrides: isBank
                ? (['velocityLow', 'velocityHigh'] as const).map((boundary) => ({
                      boundary,
                      label: boundary === 'velocityLow' ? 'Low velocity' : 'High velocity',
                      inherited: !bank.isOverridden(rangeParameters[boundary]),
                  }))
                : [],
            editable: state.editable && !!selected && !!limits && (axes.keys || axes.velocity),
            canSave: isBank ? state.canSave : workflow.members.canSave(document),
            canDiscard: isBank ? state.canDiscard : !workflow.locked && changed.length > 0,
            status:
                owner?.validation ||
                document.status ||
                (pending ? `Loading ${pending} samples` : unresolved ? `${unresolved} unresolved members` : '') ||
                (!isBank && changed.length ? `${changed.length} changed members` : state.status),
        },
        select: (id) => {
            document.previewMemberId = members[id]!.id;
        },
        patch: (range, boundaries, id = index) => {
            const sample = byId.get(members[id]?.id ?? '');
            if (!sample) throw new Error('This member is no longer available.');
            if (isBank) bank.member = { ...sample.draft.storedValues };
            (isBank ? document : sample).draft.patch(sampleMappingPatch(range, boundaries));
        },
        inherit: (boundary) => bank.clearOverride(rangeParameters[boundary]),
        save: () => (isBank ? workflow.save(document) : workflow.members!.save(document)),
        discard: () => (isBank ? workflow.discard(document) : workflow.members!.discard(document)),
        recover: () => workflow.recover(recovering ? document : (currentOwner() ?? document)),
    };
}
