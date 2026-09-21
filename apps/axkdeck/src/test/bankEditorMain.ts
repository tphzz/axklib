import { mount } from 'svelte';
import '../app.css';
import SampleEditorHarness from './SampleEditorHarness.svelte';

mount(SampleEditorHarness, { target: document.getElementById('app')!, props: { bank: true } });
