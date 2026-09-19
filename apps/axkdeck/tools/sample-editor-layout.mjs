import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
const { chromium } = await import(process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright');
const base = process.argv[2] ?? 'http://127.0.0.1:5191';
const output = resolve(process.argv[3] ?? '../../../build/logs/sample-editor/00001');
await mkdir(output, {recursive:true});
const browser = await chromium.launch({executablePath:process.env.CHROMIUM_PATH, headless:true});
const results = [];
try {
    for (const width of [390, 800, 1280]) {
        const page = await browser.newPage({viewport:{width,height:720}});
        const errors = [];
        page.on('pageerror', error => errors.push(error.message));
        await page.goto(`${base}/tools/layout-fixtures/sample-editor.html`);
        if (width === 1280) {
            const audio = await page.evaluate(async () => {
                const { prepareSampleDraft } = await import('/src/features/devices/a-series/sample/audition.ts');
                const content = new ArrayBuffer(44 + 128);
                const wav = new DataView(content);
                const text = (offset, value) => [...value].forEach((c,i) => wav.setUint8(offset+i,c.charCodeAt(0)));
                text(0,'RIFF'); wav.setUint32(4,164,true); text(8,'WAVE'); text(12,'fmt ');
                wav.setUint32(16,16,true); wav.setUint16(20,1,true); wav.setUint16(22,1,true);
                wav.setUint32(24,44100,true); wav.setUint32(28,88200,true); wav.setUint16(32,2,true); wav.setUint16(34,16,true);
                text(36,'data'); wav.setUint32(40,128,true);
                for(let i=0;i<64;i++) wav.setInt16(44+i*2, i*100,true);
                let deleted = 0;
                const transport = {
                    prepareAuditionBundle: async (_s,_o,_signal,stored) => {
                        if (!stored) throw new Error('Preview must request stored PCM');
                        return {auditionId:'test',contentSizeBytes:172,clips:[{loopModeLabel:'One-shot',lanes:[{
                            sampleRate:44100,frameCount:64,contentOffsetBytes:0,wavSizeBytes:172
                        }]}]};
                    },
                    readAuditionContent:async()=>content,
                    deleteAudition:async()=>{deleted++;},
                };
                const context = new AudioContext({sampleRate:44100});
                try {
                    const entry = await prepareSampleDraft(transport,1,'sample',{
                        editable:true,blockedParameters:[],maximumFrames:64,canEditPlayback:true,
                        parameters:{loop_mode:4,loop_start_frame:0,loop_length_frames:0,root_key:60,level:127,pan:0},
                        playbackWindow:{start_frame:10,length_frames:32}
                    },{'playback.start_frame':10,'playback.length_frames':32,loop_mode:4,loop_start_frame:0,loop_length_frames:0,
                        root_key:60,level:127,pan:0},60,context,new AbortController().signal);
                    return {frames:entry.buffer.length, first:entry.buffer.getChannelData(0)[0], last:entry.buffer.getChannelData(0)[31],deleted};
                } finally { await context.close(); }
            });
            assert.equal(audio.frames,32);
            assert.equal(audio.deleted,1);
            assert(Math.abs(audio.first - 1000/32768/Math.sqrt(2)) < 0.0001);
            assert(Math.abs(audio.last - 4100/32768/Math.sqrt(2)) < 0.0001);
        }
        const save = page.getByRole('button', {name:'Save', exact:true});
        await save.waitFor();
        assert(await save.isDisabled());
        const pixels = await page.locator('canvas').first().evaluate(canvas => {
            const data = canvas.getContext('2d').getImageData(0,0,canvas.width,canvas.height).data;
            return data.filter((v,i) => i % 4 === 3 && v > 0).length;
        });
        assert(pixels > 100, 'waveform must render actual pixels');
        const start = page.getByRole('slider', {name:'Wave start',exact:true});
        await start.focus(); await start.press('ArrowRight');
        assert(!(await save.isDisabled()));
        await page.getByRole('button', {name:'Undo Sample edit'}).click();
        assert(await save.isDisabled());
        await page.screenshot({path:resolve(output, `trim-${width}.png`)});
        await page.getByRole('tab', {name:'Map/Out',exact:true}).click();
        const level = page.getByRole('spinbutton', {name:'Level',exact:true});
        await level.fill('81');
        assert(!(await save.isDisabled()));
        await page.getByRole('button', {name:'Close image', exact:true}).click();
        await page.getByRole('dialog').waitFor();
        await page.keyboard.press('Escape');
        await page.getByRole('dialog').waitFor({state:'detached'});
        assert.equal(await page.getByLabel('Closed', {exact:true}).textContent(), 'false');
        assert.equal(await page.getByLabel('Write count').textContent(), '0');
        await page.getByRole('button', {name:'Select B',exact:true}).click();
        await page.getByRole('tab', {name:'Map/Out',exact:true}).click();
        assert.equal(await level.inputValue(), '100');
        await page.getByRole('button', {name:'Select A',exact:true}).click();
        await page.waitForFunction(() => document.querySelector('input[aria-label="Level"]')?.value === '81');
        await save.click();
        await page.waitForFunction(() => document.querySelector('output')?.textContent === '1');
        await page.waitForFunction(() => [...document.querySelectorAll('button')].find(n => n.textContent.trim() === 'Save')?.disabled);
        for (const name of ['Filter', 'EG', 'LFO', 'MIDI/CTRL']) {
            await page.getByRole('tab',{name,exact:true}).click();
            assert(await page.getByRole('tabpanel').isVisible());
            assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), 'no horizontal overflow');
            await page.screenshot({path:resolve(output, `${name.replace('/','-')}-${width}.png`)});
        }
        assert.deepEqual(errors, []);
        await page.getByRole('tab', {name:'Map/Out',exact:true}).click();
        await level.fill('70');
        await page.getByRole('button', {name:'Close image',exact:true}).click();
        await page.getByRole('button', {name:'Discard and continue',exact:true}).click();
        assert.equal(await page.getByLabel('Closed', {exact:true}).textContent(), 'true');
        results.push({width,pixels,writes:1});
        await page.close();
    }
    await writeFile(resolve(output,'results.json'), JSON.stringify(results,null,2)+'\n');
} finally { await browser.close(); }
