let messageQueue = [];
let messageIsActive = false;
let messageTimeout = null;
let messageStartTime = 0;
const fastMessageDuration = 1000;

function processMessageQueue() {
    if (messageIsActive || messageQueue.length === 0) return;
    messageIsActive = true;
    const { type, message, duration } = messageQueue.shift();
    const messageDiv = document.querySelector('.message');
    if (!messageDiv) {
        console.error('Message div not found');
        messageIsActive = false;
        return;
    }
    messageDiv.className = 'message ' + type;
    messageDiv.innerHTML = `<p>${message}</p>`;
    messageStartTime = Date.now();
    const showTime = messageQueue.length > 0 ? fastMessageDuration : (duration || 2000);
    messageTimeout = setTimeout(() => {
        messageDiv.className = 'message';
        messageDiv.innerHTML = '';
        messageIsActive = false;
        processMessageQueue();
    }, showTime);
}

function message(type, message, duration) {
    const messageDiv = document.querySelector('.message');
    if (!messageDiv) {
        console.error('Message div not found');
        return;
    }
    if (type === 'none') {
        messageDiv.className = 'message';
        messageDiv.innerHTML = '';
        messageIsActive = false;
        processMessageQueue();
        return;
    }
    messageQueue.push({ type, message, duration });
    if (messageIsActive) {
        const elapsed = Date.now() - messageStartTime;
        if (elapsed < fastMessageDuration) {
            clearTimeout(messageTimeout);
            messageTimeout = setTimeout(() => {
                messageDiv.className = 'message';
                messageDiv.innerHTML = '';
                messageIsActive = false;
                processMessageQueue();
            }, fastMessageDuration - elapsed);
        } else {
            clearTimeout(messageTimeout);
            messageIsActive = false;
            processMessageQueue();
        }
    } else {
        processMessageQueue();
    }
}

function msToTime(ms) {
    let totalSeconds = Math.floor(ms / 1000);
    let hours = Math.floor(totalSeconds / 3600);
    let minutes = Math.floor((totalSeconds % 3600) / 60);
    let seconds = totalSeconds % 60;
    return `${hours}:${minutes.toString().padStart(2, '0')}:${seconds.toString().padStart(2, '0')}`;
}

async function fetchStatuses() {
    return Promise.all([
        fetch('/status.json')
        .then(r => {
            if (!r.ok) {message('error', 'Failed to fetch status', 2000); return {}; }
            return r.json();
        }).catch(() => ({})),
        fetch('/wifi-status.json').then(r => {
            if (!r.ok) {message('error', 'Failed to fetch WiFi status', 2000); return {};}
            return r.json();
        }).catch(() => ({}))
    ]).then(([status, wifi]) => {
        return { status: status, wifi: wifi };
    });
}

function updateFooter() {
    fetchStatuses().then(json => {
        const wifiStatus = json.wifi.connected ? 'Connected' : 'Disconnected';
        const wifiColor = json.wifi.connected ? 'color: #388e3c;' : 'color: #c62828;';
        let html = `
        <span style="${wifiColor}">WiFi: ${wifiStatus}</span>
        <span>IP: ${json.wifi.ip || 'N/A'}</span>
        <span>Heap: ${json.status.totalHeap - json.status.freeHeap || 'N/A'}/${json.status.totalHeap || 'N/A'} bytes</span>
        <span>Uptime: ${msToTime(json.status.uptime) || 'N/A'}</span>
        <span>FW: ${json.status.version || 'N/A'}</span>
        <span style="font-size:0.9em;">Last update: ${new Date().toLocaleTimeString()}</span>
        `;
        document.getElementById('status').innerHTML = html;
    });
}

fetch("/nav.html")
    .then(response => response.text())
    .then(html => document.getElementById("nav").innerHTML = html);

setInterval(updateFooter, 5000); // Update every 5 seconds
updateFooter();