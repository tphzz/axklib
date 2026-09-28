import { mount } from 'svelte';
import '../app.css';
import '../features/object-editor/editor.css';
import EnvelopeHandlesHarness from './EnvelopeHandlesHarness.svelte';

const scale = Number(new URLSearchParams(location.search).get('scale') ?? 1);
document.documentElement.style.zoom = String(scale);
mount(EnvelopeHandlesHarness, { target: document.getElementById('app')! });
