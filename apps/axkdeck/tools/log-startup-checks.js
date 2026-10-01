window.runLogStartupChecks = async function () {
    const failures = [];
    const wait = async (predicate) => {
        const until = Date.now() + 10000;
        while (!predicate()) {
            if (Date.now() > until) throw new Error('Logs startup did not become visible');
            await new Promise((resolve) => setTimeout(resolve, 50));
        }
    };
    await wait(() => !document.documentElement.hasAttribute('data-interface-scale-pending'));
    const app = document.getElementById('app');
    if (getComputedStyle(app).visibility !== 'visible') failures.push('Application root is hidden');
    const scenario = new URLSearchParams(location.search).get('logs-test') ?? 'populated';
    if (scenario === 'browser') {
        if (!app.textContent.includes('desktop application')) failures.push('Browser explanation is missing');
        return { failures };
    }
    if (!window.logStartup.showCalls) failures.push('Native window was never shown');
    if (scenario === 'scale-error') {
        if (!app.textContent.includes('The Logs window could not be loaded:')) failures.push('Startup error is missing');
        return { failures };
    }
    await wait(() => document.querySelector('.log-toolbar') && window.logStartup.reads > 0);
    if (scenario === 'read-error') {
        await wait(() => document.querySelector('[role="status"]').textContent.includes('Log files unavailable'));
    } else if (scenario === 'empty') {
        await wait(() => app.textContent.includes('No matching log entries'));
    } else {
        await wait(() => app.textContent.includes('Startup application record') && app.textContent.includes('Startup local server record'));
        window.logStartup.append();
        await wait(() => app.textContent.includes('Live application record'));
        window.logStartup.visibility(false);
        await new Promise((resolve) => setTimeout(resolve, 150));
        const reads = window.logStartup.reads;
        await new Promise((resolve) => setTimeout(resolve, 1100));
        if (window.logStartup.reads !== reads) failures.push('Hidden window kept polling');
        window.logStartup.visibility(true);
        await wait(() => window.logStartup.reads > reads);
        if (getComputedStyle(app).visibility !== 'visible') failures.push('Reopened Logs are hidden');
    }
    if (window.logStartup.calls.some((command) => /server_connection|use_local_server/.test(command))) failures.push('Logs started the workspace server');
    return { failures, scenario, reads: window.logStartup.reads, showCalls: window.logStartup.showCalls };
};
