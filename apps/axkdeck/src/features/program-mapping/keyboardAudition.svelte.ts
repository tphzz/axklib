import { onDestroy } from 'svelte';
import { editorAudio } from '../object-editor/audioContext';
import type { ObjectEditorDocument } from '../object-editor/workflow.svelte';
import type { MappingRole } from './protocol';
import { userFacingMessage } from '../../lib/userFacingMessage';

export function mappingKeyboard(document: () => ObjectEditorDocument, role: () => MappingRole) {
    const audio = editorAudio()?.mapping;
    const state = $state({ velocity: 100 });
    let token = '';
    const release = () => {
        if (token) audio?.release(token);
        token = '';
    };
    const press = audio
        ? (note: number) => {
              release();
              const owner = document(),
                  identity = crypto.randomUUID();
              token = identity;
              if (
                  owner.phase !== 'editable' ||
                  owner.conflict ||
                  (owner.draft.dirty ? owner.validation : Object.values(owner.inputErrors).find(Boolean))
              ) {
                  release();
                  return;
              }
              void audio.play(owner, role(), identity, note, state.velocity).catch((error) => {
                  if (token === identity) {
                      owner.status = userFacingMessage(error);
                      release();
                  }
              });
          }
        : undefined;
    $effect(() => {
        document().detail;
        document().phase;
        document().draft.revision;
        release();
    });
    onDestroy(release);
    return { state, press, release };
}
