// ============================================================================
// IA-64 Hypervisor Web UI - Application Logic
// ============================================================================

const API_BASE_URL = 'http://localhost:8080/api/v1';
const REFRESH_INTERVAL = 3000; // 3 seconds

// Application state
let vms = [];
let selectedVmId = null;
let autoRefreshEnabled = true;
let autoRefreshTimer = null;
let vmToDelete = null;

// ============================================================================
// Initialization
// ============================================================================

document.addEventListener('DOMContentLoaded', function() {
    initializeUI();
    initializeEventListeners();
    checkServerConnection();
    startAutoRefresh();
});

function initializeUI() {
    // Set up modal close buttons
    document.querySelectorAll('.modal-close').forEach(btn => {
        btn.addEventListener('click', function() {
            const modal = this.closest('.modal');
            if (modal) {
                closeModal(modal.id);
            }
        });
    });

    // Close modals when clicking outside
    document.querySelectorAll('.modal').forEach(modal => {
        modal.addEventListener('click', function(e) {
            if (e.target === this) {
                closeModal(this.id);
            }
        });
    });

    // Tab switching
    document.querySelectorAll('.tab-button').forEach(btn => {
        btn.addEventListener('click', function() {
            switchTab(this.dataset.tab);
        });
    });

    // Template selection auto-fill
    document.getElementById('vm-template').addEventListener('change', function() {
        applyTemplate(this.value);
    });
    
    // Console auto-scroll toggle (will be initialized when modal opens)
    // Event listener added dynamically to avoid errors if element doesn't exist yet
}

function initializeEventListeners() {
    document.getElementById('btn-create-vm').addEventListener('click', () => openModal('modal-create-vm'));
    document.getElementById('btn-refresh').addEventListener('click', () => refreshVMList());
    document.getElementById('toggle-auto-refresh').addEventListener('change', function() {
        autoRefreshEnabled = this.checked;
        if (autoRefreshEnabled) {
            startAutoRefresh();
        } else {
            stopAutoRefresh();
        }
    });
}

// ============================================================================
// API Communication
// ============================================================================

async function apiRequest(method, endpoint, body = null) {
    try {
        const options = {
            method: method,
            headers: {
                'Content-Type': 'application/json'
            }
        };

        if (body) {
            options.body = JSON.stringify(body);
        }

        const response = await fetch(`${API_BASE_URL}${endpoint}`, options);
        const data = await response.json();

        if (!response.ok) {
            throw new Error(data.message || `HTTP ${response.status}`);
        }

        return data;
    } catch (error) {
        console.error('API request failed:', error);
        throw error;
    }
}

async function checkServerConnection() {
    try {
        await apiRequest('GET', '/health');
        updateServerStatus(true);
        refreshVMList();
    } catch (error) {
        updateServerStatus(false);
        showToast('Failed to connect to server', 'error');
    }
}

function updateServerStatus(connected) {
    const statusElement = document.getElementById('server-status');
    if (connected) {
        statusElement.textContent = 'Connected';
        statusElement.className = 'status-badge status-running';
    } else {
        statusElement.textContent = 'Disconnected';
        statusElement.className = 'status-badge status-error';
    }
}

// ============================================================================
// VM List Management
// ============================================================================

async function refreshVMList() {
    showLoading(true);
    try {
        const data = await apiRequest('GET', '/vms');
        vms = data.vms || [];
        updateServerStatus(true);
        renderVMList();
    } catch (error) {
        console.error('Failed to load VMs:', error);
        updateServerStatus(false);
        showToast('Failed to load VM list', 'error');
    } finally {
        showLoading(false);
    }
}

function renderVMList() {
    const tbody = document.getElementById('vm-list');
    const emptyState = document.getElementById('empty-state');
    const totalVmsElement = document.getElementById('total-vms');

    totalVmsElement.textContent = `${vms.length} VM${vms.length !== 1 ? 's' : ''}`;

    if (vms.length === 0) {
        tbody.innerHTML = '';
        emptyState.style.display = 'flex';
        return;
    }

    emptyState.style.display = 'none';

    tbody.innerHTML = vms.map(vm => `
        <tr data-vm-id="${vm.vm_id}" class="vm-row">
            <td class="vm-name">
                <strong>${escapeHtml(vm.name)}</strong>
            </td>
            <td class="vm-id" title="${vm.vm_id}">
                ${vm.vm_id.substring(0, 8)}...
            </td>
            <td>
                ${getStatusBadge(vm.state)}
            </td>
            <td>${vm.cpu_count || 1}</td>
            <td>${formatMemory(vm.memory_mb || 0)}</td>
            <td>${formatUptime(vm.uptime_seconds || 0)}</td>
            <td class="vm-actions">
                ${getActionButtons(vm)}
            </td>
        </tr>
    `).join('');

    // Attach event listeners to action buttons
    attachActionListeners();
}

function getStatusBadge(state) {
    const statusMap = {
        'running': 'status-running',
        'stopped': 'status-stopped',
        'paused': 'status-paused',
        'error': 'status-error'
    };
    const statusClass = statusMap[state] || 'status-unknown';
    return `<span class="status-badge ${statusClass}">${state}</span>`;
}

function getActionButtons(vm) {
    const buttons = [];

    if (vm.state === 'stopped') {
        buttons.push(`<button class="btn-icon btn-start" title="Start VM" data-action="start" data-vm-id="${vm.vm_id}">??</button>`);
    } else if (vm.state === 'running') {
        buttons.push(`<button class="btn-icon btn-pause" title="Pause VM" data-action="pause" data-vm-id="${vm.vm_id}">??</button>`);
        buttons.push(`<button class="btn-icon btn-stop" title="Stop VM" data-action="stop" data-vm-id="${vm.vm_id}">??</button>`);
    } else if (vm.state === 'paused') {
        buttons.push(`<button class="btn-icon btn-resume" title="Resume VM" data-action="resume" data-vm-id="${vm.vm_id}">??</button>`);
        buttons.push(`<button class="btn-icon btn-stop" title="Stop VM" data-action="stop" data-vm-id="${vm.vm_id}">??</button>`);
    }

    buttons.push(`<button class="btn-icon btn-info" title="VM Details" data-action="info" data-vm-id="${vm.vm_id}">??</button>`);
    buttons.push(`<button class="btn-icon btn-delete" title="Delete VM" data-action="delete" data-vm-id="${vm.vm_id}">???</button>`);

    return buttons.join('');
}

function attachActionListeners() {
    document.querySelectorAll('[data-action]').forEach(btn => {
        btn.addEventListener('click', async function(e) {
            e.stopPropagation();
            const action = this.dataset.action;
            const vmId = this.dataset.vmId;
            await handleVMAction(action, vmId);
        });
    });

    document.querySelectorAll('.vm-row').forEach(row => {
        row.addEventListener('click', function() {
            const vmId = this.dataset.vmId;
            showVMDetails(vmId);
        });
    });
}

// ============================================================================
// VM Actions
// ============================================================================

async function handleVMAction(action, vmId) {
    const vm = vms.find(v => v.vm_id === vmId);
    if (!vm) return;

    try {
        switch (action) {
            case 'start':
                await apiRequest('POST', `/vms/${vmId}/start`);
                showToast(`VM "${vm.name}" started`, 'success');
                break;
            case 'stop':
                await apiRequest('POST', `/vms/${vmId}/stop`);
                showToast(`VM "${vm.name}" stopped`, 'success');
                break;
            case 'pause':
                await apiRequest('POST', `/vms/${vmId}/pause`);
                showToast(`VM "${vm.name}" paused`, 'success');
                break;
            case 'resume':
                await apiRequest('POST', `/vms/${vmId}/resume`);
                showToast(`VM "${vm.name}" resumed`, 'success');
                break;
            case 'info':
                showVMDetails(vmId);
                return;
            case 'delete':
                showDeleteConfirmation(vmId);
                return;
        }
        await refreshVMList();
    } catch (error) {
        showToast(`Failed to ${action} VM: ${error.message}`, 'error');
    }
}

async function createVM() {
    const name = document.getElementById('vm-name').value.trim();
    const memory = parseInt(document.getElementById('vm-memory').value);
    const cpus = parseInt(document.getElementById('vm-cpus').value);
    const autoStart = document.getElementById('vm-auto-start').checked;

    if (!name) {
        showToast('Please enter a VM name', 'error');
        return;
    }

    try {
        const config = {
            name: name,
            config: {
                memory: { size_mb: memory },
                cpu: { count: cpus },
                storage: []
            },
            auto_start: autoStart
        };

        const result = await apiRequest('POST', '/vms', config);
        showToast(`VM "${name}" created successfully`, 'success');
        closeModal('modal-create-vm');
        document.getElementById('form-create-vm').reset();
        await refreshVMList();
    } catch (error) {
        showToast(`Failed to create VM: ${error.message}`, 'error');
    }
}

function showDeleteConfirmation(vmId) {
    const vm = vms.find(v => v.vm_id === vmId);
    if (!vm) return;

    vmToDelete = vmId;
    document.getElementById('delete-vm-name').textContent = vm.name;
    openModal('modal-confirm-delete');
}

async function confirmDelete() {
    if (!vmToDelete) return;

    const vm = vms.find(v => v.vm_id === vmToDelete);
    
    try {
        await apiRequest('DELETE', `/vms/${vmToDelete}`);
        showToast(`VM "${vm.name}" deleted`, 'success');
        closeModal('modal-confirm-delete');
        vmToDelete = null;
        await refreshVMList();
    } catch (error) {
        showToast(`Failed to delete VM: ${error.message}`, 'error');
    }
}

// ============================================================================
// VM Details
// ============================================================================

async function showVMDetails(vmId) {
    selectedVmId = vmId;
    const vm = vms.find(v => v.vm_id === vmId);
    if (!vm) return;

    try {
        const details = await apiRequest('GET', `/vms/${vmId}`);
        
        document.getElementById('details-vm-name').textContent = details.name;
        document.getElementById('details-vm-id').textContent = details.vm_id;
        document.getElementById('details-vm-state').innerHTML = getStatusBadge(details.state);
        document.getElementById('details-vm-memory').textContent = formatMemory(details.memory_mb || 0);
        document.getElementById('details-vm-cpus').textContent = details.cpu_count || 1;
        document.getElementById('details-vm-instructions').textContent = formatNumber(details.instruction_count || 0);
        document.getElementById('details-vm-uptime').textContent = formatUptime(details.uptime_seconds || 0);
        
        // Configuration tab
        document.getElementById('config-json').textContent = JSON.stringify(details.config || {}, null, 2);
        
        // Load logs
        await refreshLogs();
        
        openModal('modal-vm-details');
        switchTab('overview');
    } catch (error) {
        showToast(`Failed to load VM details: ${error.message}`, 'error');
    }
}

async function refreshLogs() {
    if (!selectedVmId) return;

    try {
        const logs = await apiRequest('GET', `/vms/${selectedVmId}/logs`);
        document.getElementById('vm-logs').textContent = logs.logs || 'No logs available';
    } catch (error) {
        document.getElementById('vm-logs').textContent = `Failed to load logs: ${error.message}`;
    }
}

function clearLogsView() {
    document.getElementById('vm-logs').textContent = 'No logs available';
}

// ============================================================================
// Console Output
// ============================================================================

let consoleAutoScroll = true;
let consoleRefreshTimer = null;

async function refreshConsole() {
    if (!selectedVmId) return;

    try {
        const response = await apiRequest('GET', `/vms/${selectedVmId}/console?count=1000`);
        
        const consoleOutput = document.getElementById('console-output');
        const lineCountEl = document.getElementById('console-line-count');
        const byteCountEl = document.getElementById('console-byte-count');
        
        // Update stats
        lineCountEl.textContent = `${response.total_lines || 0} lines`;
        byteCountEl.textContent = formatBytes(response.total_bytes || 0);
        
        // Render console lines
        if (response.lines && response.lines.length > 0) {
            consoleOutput.innerHTML = response.lines.map((line, index) => {
                const lineNum = response.start_line + index + 1;
                return `<div class="console-line"><span class="console-line-number">${lineNum}</span>${escapeHtml(line)}</div>`;
            }).join('');
            
            // Auto-scroll to bottom if enabled
            if (consoleAutoScroll) {
                consoleOutput.scrollTop = consoleOutput.scrollHeight;
            }
        } else {
            consoleOutput.innerHTML = '<div class="console-placeholder">No console output</div>';
        }
    } catch (error) {
        console.error('Failed to refresh console:', error);
        document.getElementById('console-output').innerHTML = 
            `<div class="console-placeholder">Failed to load console output: ${escapeHtml(error.message)}</div>`;
    }
}

function clearConsoleView() {
    document.getElementById('console-output').innerHTML = '<div class="console-placeholder">No console output</div>';
    document.getElementById('console-line-count').textContent = '0 lines';
    document.getElementById('console-byte-count').textContent = '0 bytes';
}

function startConsoleAutoRefresh() {
    if (consoleRefreshTimer) {
        clearInterval(consoleRefreshTimer);
    }
    consoleRefreshTimer = setInterval(() => {
        if (selectedVmId && document.getElementById('tab-console').classList.contains('active')) {
            refreshConsole();
        }
    }, 2000); // Refresh every 2 seconds
}

function stopConsoleAutoRefresh() {
    if (consoleRefreshTimer) {
        clearInterval(consoleRefreshTimer);
        consoleRefreshTimer = null;
    }
}

function exportConfig() {
    const config = document.getElementById('config-json').textContent;
    const blob = new Blob([config], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = `vm-${selectedVmId}-config.json`;
    a.click();
    URL.revokeObjectURL(url);
}

// ============================================================================
// UI Utilities
// ============================================================================

function openModal(modalId) {
    document.getElementById(modalId).classList.add('show');
}

function closeModal(modalId) {
    document.getElementById(modalId).classList.remove('show');
}

function switchTab(tabName) {
    document.querySelectorAll('.tab-button').forEach(btn => {
        btn.classList.toggle('active', btn.dataset.tab === tabName);
    });
    
    document.querySelectorAll('.tab-content').forEach(content => {
        content.classList.toggle('active', content.id === `tab-${tabName}`);
    });
    
    // Handle console tab activation
    if (tabName === 'console') {
        refreshConsole();
        startConsoleAutoRefresh();
        
        // Initialize console auto-scroll toggle if not already done
        const autoScrollCheckbox = document.getElementById('console-auto-scroll');
        if (autoScrollCheckbox && !autoScrollCheckbox.hasAttribute('data-initialized')) {
            autoScrollCheckbox.setAttribute('data-initialized', 'true');
            autoScrollCheckbox.addEventListener('change', function() {
                consoleAutoScroll = this.checked;
            });
        }
    } else {
        stopConsoleAutoRefresh();
    }
}

function applyTemplate(template) {
    const templates = {
        minimal: { memory: 128, cpus: 1 },
        standard: { memory: 512, cpus: 2 },
        server: { memory: 2048, cpus: 4 }
    };

    const config = templates[template];
    if (config) {
        document.getElementById('vm-memory').value = config.memory;
        document.getElementById('vm-cpus').value = config.cpus;
    }
}

function showLoading(show) {
    document.getElementById('loading-indicator').style.display = show ? 'flex' : 'none';
}

function showToast(message, type = 'info') {
    const container = document.getElementById('toast-container');
    const toast = document.createElement('div');
    toast.className = `toast toast-${type}`;
    toast.textContent = message;
    
    container.appendChild(toast);
    
    setTimeout(() => {
        toast.classList.add('show');
    }, 10);
    
    setTimeout(() => {
        toast.classList.remove('show');
        setTimeout(() => {
            container.removeChild(toast);
        }, 300);
    }, 3000);
}

// ============================================================================
// Auto-refresh
// ============================================================================

function startAutoRefresh() {
    stopAutoRefresh();
    if (autoRefreshEnabled) {
        autoRefreshTimer = setInterval(refreshVMList, REFRESH_INTERVAL);
    }
}

function stopAutoRefresh() {
    if (autoRefreshTimer) {
        clearInterval(autoRefreshTimer);
        autoRefreshTimer = null;
    }
}

// ============================================================================
// Formatting Utilities
// ============================================================================

function formatMemory(mb) {
    if (mb >= 1024) {
        return `${(mb / 1024).toFixed(1)} GB`;
    }
    return `${mb} MB`;
}

function formatUptime(seconds) {
    if (seconds < 60) {
        return `${seconds}s`;
    } else if (seconds < 3600) {
        const minutes = Math.floor(seconds / 60);
        return `${minutes}m`;
    } else if (seconds < 86400) {
        const hours = Math.floor(seconds / 3600);
        const minutes = Math.floor((seconds % 3600) / 60);
        return `${hours}h ${minutes}m`;
    } else {
        const days = Math.floor(seconds / 86400);
        const hours = Math.floor((seconds % 86400) / 3600);
        return `${days}d ${hours}h`;
    }
}

function formatNumber(num) {
    return num.toLocaleString();
}

function formatBytes(bytes) {
    if (bytes === 0) return '0 Bytes';
    const k = 1024;
    const sizes = ['Bytes', 'KB', 'MB', 'GB'];
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    return Math.round((bytes / Math.pow(k, i)) * 100) / 100 + ' ' + sizes[i];
}

function escapeHtml(text) {
    const div = document.createElement('div');
    div.textContent = text;
    return div.innerHTML;
}
