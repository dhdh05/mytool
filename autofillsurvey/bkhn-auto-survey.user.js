// ==UserScript==
// @name         BKHN Auto Survey Evaluation (DevExpress Bypass)
// @namespace    https://github.com/your-username/bkhn-auto-survey
// @version      1.1.0
// @description  Tự động hóa đánh giá rèn luyện/khảo sát lớp học trên hệ thống ctt-sis của ĐH Bách Khoa Hà Nội. Hỗ trợ bypass giao diện DevExpress, có Control Panel Shadow DOM cô lập.
// @author       Senior Web Developer
// @match        *://ctt-sis.hust.edu.vn/Surveys/*
// @match        *://ctt-daotao.hust.edu.vn/Surveys/*
// @grant        GM_setValue
// @grant        GM_getValue
// @run-at       document-end
// ==/UserScript==

(function () {
    'use strict';

    // ==========================================
    // 1. CONFIGURATION & CONSTANTS
    // ==========================================
    const CONFIG = {
        storageKeys: {
            isEnabled: 'bkhn_survey_enabled',
            scanIntervalMs: 'bkhn_survey_scan_interval',
            reloadWaitMs: 'bkhn_survey_reload_wait',
            strategy: 'bkhn_survey_strategy'
        },
        defaults: {
            isEnabled: true,
            scanIntervalMs: 2000,
            reloadWaitMs: 4000,
            strategy: 'max' // 'max' | 'realistic' | 'random'
        },
        keywords: {
            averageScore: ['bình thường', 'từ trên 2 giờ đến 3 giờ', 'không thay đổi'], // Các từ khóa chọn mức vừa phải
            submitBtn: ['gửi khảo sát', 'gửi']
        },
        selectors: {
            labelWrap: 'label.dx-wrap',
            tableGroup: 'table[id*="_RB"]',
            fakeRadioBtn: 'span[class*="dxeIRadioButton"]',
            submitCandidates: 'span, div, b, a',
            clickableWrapper: '.dxbButton, a, div'
        }
    };

    // ==========================================
    // 2. UI MANAGER (SHADOW DOM CONTROL PANEL)
    // ==========================================
    class UIManager {
        /**
         * @param {SurveyAutomator} automator - Instance of the automator core
         */
        constructor(automator) {
            this.automator = automator;
            this.host = null;
            this.shadow = null;
            this.settings = {};
            
            this.loadSettings();
            this.initUI();
        }

        /**
         * Load settings from Tampermonkey GM Storage or fallback to defaults
         */
        loadSettings() {
            this.settings.isEnabled = GM_getValue(CONFIG.storageKeys.isEnabled, CONFIG.defaults.isEnabled);
            this.settings.scanIntervalMs = parseInt(GM_getValue(CONFIG.storageKeys.scanIntervalMs, CONFIG.defaults.scanIntervalMs), 10);
            this.settings.reloadWaitMs = parseInt(GM_getValue(CONFIG.storageKeys.reloadWaitMs, CONFIG.defaults.reloadWaitMs), 10);
            this.settings.strategy = GM_getValue(CONFIG.storageKeys.strategy, CONFIG.defaults.strategy);
        }

        /**
         * Save a setting to GM Storage
         * @param {string} key - Storage key name
         * @param {any} value - Value to store
         */
        saveSetting(key, value) {
            this.settings[key] = value;
            GM_setValue(CONFIG.storageKeys[key], value);
        }

        /**
         * Initialize Shadow DOM UI and append it to the document body
         */
        initUI() {
            // Create host element
            this.host = document.createElement('div');
            this.host.id = 'bkhn-survey-host';
            document.body.appendChild(this.host);

            // Create open Shadow Root
            this.shadow = this.host.attachShadow({ mode: 'open' });

            // Injected Styles
            const styles = `
                :host {
                    all: initial;
                }
                .bkhn-container {
                    position: fixed;
                    bottom: 20px;
                    right: 20px;
                    z-index: 2147483647;
                    font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
                    color: #e5e7eb;
                    user-select: none;
                }
                .bkhn-panel {
                    width: 290px;
                    background: rgba(17, 17, 23, 0.85);
                    backdrop-filter: blur(12px);
                    -webkit-backdrop-filter: blur(12px);
                    border: 1px solid rgba(255, 255, 255, 0.08);
                    border-radius: 16px;
                    box-shadow: 0 10px 30px rgba(0, 0, 0, 0.4);
                    overflow: hidden;
                    transition: all 0.4s cubic-bezier(0.175, 0.885, 0.32, 1.275);
                    display: flex;
                    flex-direction: column;
                }
                .bkhn-panel.minimized {
                    width: 50px;
                    height: 50px;
                    border-radius: 50%;
                    cursor: pointer;
                    background: linear-gradient(135deg, #7c3aed 0%, #4f46e5 100%);
                    box-shadow: 0 4px 15px rgba(124, 58, 237, 0.4);
                    justify-content: center;
                    align-items: center;
                    border: none;
                }
                .bkhn-panel.minimized .panel-content,
                .bkhn-panel.minimized .panel-header {
                    display: none;
                }
                .bkhn-panel.minimized .panel-toggle-icon {
                    display: flex;
                    font-size: 22px;
                    animation: pulse 2s infinite;
                    justify-content: center;
                    align-items: center;
                    width: 100%;
                    height: 100%;
                }
                .panel-toggle-icon {
                    display: none;
                }
                @keyframes pulse {
                    0% { transform: scale(1); }
                    50% { transform: scale(1.1); }
                    100% { transform: scale(1); }
                }

                .panel-header {
                    display: flex;
                    justify-content: space-between;
                    align-items: center;
                    padding: 12px 16px;
                    background: linear-gradient(135deg, rgba(124, 58, 237, 0.15) 0%, rgba(79, 70, 229, 0.15) 100%);
                    border-bottom: 1px solid rgba(255, 255, 255, 0.06);
                }
                .panel-title {
                    font-size: 13.5px;
                    font-weight: 700;
                    letter-spacing: 0.5px;
                    background: linear-gradient(90deg, #c084fc, #818cf8);
                    -webkit-background-clip: text;
                    -webkit-text-fill-color: transparent;
                    display: flex;
                    align-items: center;
                    gap: 6px;
                }
                .panel-minimize-btn {
                    background: none;
                    border: none;
                    color: rgba(255, 255, 255, 0.5);
                    cursor: pointer;
                    font-size: 18px;
                    line-height: 1;
                    padding: 2px 6px;
                    border-radius: 4px;
                    transition: all 0.2s;
                }
                .panel-minimize-btn:hover {
                    color: #ffffff;
                    background: rgba(255, 255, 255, 0.1);
                }

                .panel-content {
                    padding: 16px;
                    display: flex;
                    flex-direction: column;
                    gap: 14px;
                }

                .form-group {
                    display: flex;
                    flex-direction: column;
                    gap: 5px;
                }
                .form-row {
                    display: flex;
                    gap: 10px;
                }
                .form-row .form-group {
                    flex: 1;
                }
                label {
                    font-size: 10px;
                    font-weight: 600;
                    color: rgba(255, 255, 255, 0.5);
                    text-transform: uppercase;
                    letter-spacing: 0.5px;
                }

                input, select {
                    background: rgba(255, 255, 255, 0.05);
                    border: 1px solid rgba(255, 255, 255, 0.1);
                    border-radius: 8px;
                    color: #f3f4f6;
                    padding: 7px 10px;
                    font-size: 12.5px;
                    outline: none;
                    transition: all 0.2s;
                    font-family: inherit;
                }
                input:focus, select:focus {
                    border-color: #7c3aed;
                    background: rgba(255, 255, 255, 0.08);
                    box-shadow: 0 0 0 2px rgba(124, 58, 237, 0.25);
                }
                select option {
                    background: #111117;
                    color: #f3f4f6;
                }

                .switch-group {
                    display: flex;
                    justify-content: space-between;
                    align-items: center;
                    background: rgba(255, 255, 255, 0.02);
                    padding: 8px 12px;
                    border-radius: 10px;
                    border: 1px solid rgba(255, 255, 255, 0.04);
                }
                .switch-label-desc {
                    display: flex;
                    flex-direction: column;
                    gap: 2px;
                }
                .switch-label-title {
                    font-size: 12.5px;
                    font-weight: 600;
                }
                .switch-label-sub {
                    font-size: 9.5px;
                    color: rgba(255, 255, 255, 0.4);
                }
                .switch {
                    position: relative;
                    display: inline-block;
                    width: 40px;
                    height: 22px;
                }
                .switch input {
                    opacity: 0;
                    width: 0;
                    height: 0;
                }
                .slider {
                    position: absolute;
                    cursor: pointer;
                    top: 0;
                    left: 0;
                    right: 0;
                    bottom: 0;
                    background-color: rgba(255, 255, 255, 0.1);
                    transition: .3s;
                    border-radius: 22px;
                }
                .slider:before {
                    position: absolute;
                    content: "";
                    height: 16px;
                    width: 16px;
                    left: 3px;
                    bottom: 3px;
                    background-color: white;
                    transition: .3s;
                    border-radius: 50%;
                    box-shadow: 0 2px 4px rgba(0,0,0,0.3);
                }
                input:checked + .slider {
                    background: linear-gradient(135deg, #7c3aed 0%, #4f46e5 100%);
                }
                input:checked + .slider:before {
                    transform: translateX(18px);
                }

                .btn-primary {
                    background: linear-gradient(135deg, #7c3aed 0%, #4f46e5 100%);
                    color: white;
                    border: none;
                    border-radius: 8px;
                    padding: 9px;
                    font-weight: 600;
                    font-size: 12.5px;
                    cursor: pointer;
                    transition: all 0.2s;
                    display: flex;
                    justify-content: center;
                    align-items: center;
                    gap: 6px;
                    box-shadow: 0 4px 12px rgba(124, 58, 237, 0.2);
                }
                .btn-primary:hover {
                    transform: translateY(-1px);
                    box-shadow: 0 6px 15px rgba(124, 58, 237, 0.35);
                }
                .btn-primary:active {
                    transform: translateY(1px);
                }

                .status-console {
                    background: rgba(0, 0, 0, 0.25);
                    border: 1px solid rgba(255, 255, 255, 0.05);
                    border-radius: 8px;
                    padding: 8px 10px;
                    font-size: 11px;
                    font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
                    min-height: 38px;
                    max-height: 52px;
                    overflow-y: auto;
                    color: #c084fc;
                    word-break: break-word;
                    line-height: 1.4;
                }
                .status-console.success { color: #34d399; }
                .status-console.error { color: #f87171; }
                .status-console.warn { color: #fbbf24; }
            `;

            // HTML Element Generation
            const container = document.createElement('div');
            container.className = 'bkhn-container';

            const panel = document.createElement('div');
            panel.className = 'bkhn-panel';

            panel.innerHTML = `
                <div class="panel-toggle-icon">⚡</div>
                <div class="panel-header">
                    <div class="panel-title">⚡ BKHN Auto Survey</div>
                    <button class="panel-minimize-btn" title="Thu gọn/Mở rộng">━</button>
                </div>
                <div class="panel-content">
                    <div class="switch-group">
                        <div class="switch-label-desc">
                            <span class="switch-label-title">Tự động quét</span>
                            <span class="switch-label-sub">Tự điền & nộp khi thấy form</span>
                        </div>
                        <label class="switch">
                            <input type="checkbox" id="bkhn-toggle-auto" ${this.settings.isEnabled ? 'checked' : ''}>
                            <span class="slider"></span>
                        </label>
                    </div>

                    <div class="form-group">
                        <label for="bkhn-strategy">Chiến thuật khảo sát</label>
                        <select id="bkhn-strategy">
                            <option value="max" ${this.settings.strategy === 'max' ? 'selected' : ''}>Max Điểm (4-5 sao)</option>
                            <option value="realistic" ${this.settings.strategy === 'realistic' ? 'selected' : ''}>Thực tế (Trộn Khá/Tốt)</option>
                            <option value="random" ${this.settings.strategy === 'random' ? 'selected' : ''}>Ngẫu nhiên hoàn toàn</option>
                        </select>
                    </div>

                    <div class="form-row">
                        <div class="form-group">
                            <label for="bkhn-scan-interval">Độ trễ quét (ms)</label>
                            <input type="number" id="bkhn-scan-interval" min="500" max="10000" step="100" value="${this.settings.scanIntervalMs}">
                        </div>
                        <div class="form-group">
                            <label for="bkhn-reload-wait">Trễ Reload (ms)</label>
                            <input type="number" id="bkhn-reload-wait" min="1000" max="15000" step="500" value="${this.settings.reloadWaitMs}">
                        </div>
                    </div>

                    <button class="btn-primary" id="bkhn-btn-trigger">
                        <span>▶</span> Điền Khảo Sát Ngay
                    </button>

                    <div class="status-console" id="bkhn-console">Sẵn sàng.</div>
                </div>
            `;

            const styleSheet = document.createElement('style');
            styleSheet.textContent = styles;

            this.shadow.appendChild(styleSheet);
            this.shadow.appendChild(container);
            container.appendChild(panel);

            // Bind Event Listeners
            this.bindEvents(panel, container);
        }

        /**
         * Bind domestic event listeners to UI components
         * @param {HTMLElement} panel - The panel container element
         * @param {HTMLElement} container - Wrapper container element
         */
        bindEvents(panel, container) {
            const toggleAuto = this.shadow.querySelector('#bkhn-toggle-auto');
            const selectStrategy = this.shadow.querySelector('#bkhn-strategy');
            const inputScan = this.shadow.querySelector('#bkhn-scan-interval');
            const inputReload = this.shadow.querySelector('#bkhn-reload-wait');
            const btnTrigger = this.shadow.querySelector('#bkhn-btn-trigger');
            const btnMinimize = this.shadow.querySelector('.panel-minimize-btn');
            const miniIcon = this.shadow.querySelector('.panel-toggle-icon');

            // Minimize toggle click
            const toggleMinimize = () => {
                panel.classList.toggle('minimized');
                if (panel.classList.contains('minimized')) {
                    btnMinimize.textContent = '✚';
                } else {
                    btnMinimize.textContent = '━';
                }
            };
            btnMinimize.addEventListener('click', (e) => {
                e.stopPropagation();
                toggleMinimize();
            });
            miniIcon.addEventListener('click', (e) => {
                e.stopPropagation();
                toggleMinimize();
            });

            // Toggle auto execution
            toggleAuto.addEventListener('change', (e) => {
                const checked = e.target.checked;
                this.saveSetting('isEnabled', checked);
                this.writeConsole(`Đã ${checked ? 'BẬT' : 'TẮT'} chế độ tự động điền.`, checked ? 'success' : '');
                
                if (checked) {
                    this.automator.start();
                } else {
                    this.automator.stop();
                }
            });

            // Strategy dropdown change
            selectStrategy.addEventListener('change', (e) => {
                const strategy = e.target.value;
                this.saveSetting('strategy', strategy);
                this.writeConsole(`Chiến thuật mới: ${selectStrategy.options[selectStrategy.selectedIndex].text}`, 'success');
            });

            // Scan Interval validation and save
            inputScan.addEventListener('change', (e) => {
                let val = parseInt(e.target.value, 10);
                if (isNaN(val) || val < 500) val = 500;
                if (val > 10000) val = 10000;
                e.target.value = val;
                this.saveSetting('scanIntervalMs', val);
                this.writeConsole(`Độ trễ quét cập nhật: ${val}ms`);
                
                // Restart scanner if running to apply new interval
                if (this.settings.isEnabled) {
                    this.automator.start();
                }
            });

            // Reload wait validation and save
            inputReload.addEventListener('change', (e) => {
                let val = parseInt(e.target.value, 10);
                if (isNaN(val) || val < 1000) val = 1000;
                if (val > 15000) val = 15000;
                e.target.value = val;
                this.saveSetting('reloadWaitMs', val);
                this.writeConsole(`Độ trễ Reload cập nhật: ${val}ms`);
            });

            // Manual trigger button
            btnTrigger.addEventListener('click', () => {
                this.writeConsole('Đang kích hoạt điền khảo sát...', 'warn');
                const success = this.automator.forceExecute();
                if (success) {
                    this.writeConsole('Khảo sát đã điền hoàn tất!', 'success');
                } else {
                    this.writeConsole('Không tìm thấy form hoặc form chưa sẵn sàng.', 'error');
                }
            });
        }

        /**
         * Outputs structured message logs to the panel UI Console
         * @param {string} text - Message text
         * @param {string} [type] - Styling type ('success' | 'warn' | 'error' | '')
         */
        writeConsole(text, type = '') {
            const consoleBox = this.shadow.querySelector('#bkhn-console');
            if (consoleBox) {
                consoleBox.className = 'status-console';
                if (type) consoleBox.classList.add(type);
                consoleBox.innerText = text;
                consoleBox.scrollTop = consoleBox.scrollHeight;
            }
            console.log(`[BKHN UI] [${type.toUpperCase() || 'INFO'}] ${text}`);
        }
    }

    // ==========================================
    // 3. CORE AUTOMATION CLASS (SurveyAutomator)
    // ==========================================
    class SurveyAutomator {
        constructor() {
            this.scanner = null;
            this.config = CONFIG;
            this.ui = null;
        }

        /**
         * Bootstraps UIManager and schedules the scanning process
         */
        init() {
            this.ui = new UIManager(this);
            this.start();
        }

        /**
         * Starts scanning the page at the configured interval
         */
        start() {
            this.stop(); // Clear any running scanner beforehand
            
            if (!this.ui.settings.isEnabled) {
                this.ui.writeConsole("Tự động quét đang tắt. Hãy bật lên hoặc ấn nút điền thủ công.");
                return;
            }

            this.ui.writeConsole("Đang quét tìm form khảo sát...");
            this.scanner = setInterval(() => this.processPage(), this.ui.settings.scanIntervalMs);
        }

        /**
         * Suspends scanning intervals
         */
        stop() {
            if (this.scanner) {
                clearInterval(this.scanner);
                this.scanner = null;
            }
        }

        /**
         * Master handler executed on each scan interval tick
         */
        processPage() {
            if (this.checkCompletionStatus()) {
                this.stop();
                this.notifySuccess();
                return;
            }

            const answerLabels = document.querySelectorAll(this.config.selectors.labelWrap);
            if (answerLabels.length === 0) return; // Survey form hasn't fully rendered yet, skip this tick

            this.stop(); // Survey found. Halt scanner to prevent duplicate execution during filling
            this.ui.writeConsole("Phát hiện form khảo sát. Đang xử lý...", 'success');

            try {
                const questionGroups = this.groupQuestionsByTarget(answerLabels);
                this.autoFillAnswers(questionGroups, this.ui.settings.strategy);
                this.submitForm();
            } catch (error) {
                this.ui.writeConsole(`Lỗi: ${error.message}`, 'error');
                console.error(error);
            }
        }

        /**
         * Manually forces execution of filling logic regardless of auto settings
         * @returns {boolean} Whether action completed successfully
         */
        forceExecute() {
            const answerLabels = document.querySelectorAll(this.config.selectors.labelWrap);
            if (answerLabels.length === 0) return false;

            try {
                const questionGroups = this.groupQuestionsByTarget(answerLabels);
                this.autoFillAnswers(questionGroups, this.ui.settings.strategy);
                return true;
            } catch (error) {
                console.error(error);
                return false;
            }
        }

        /**
         * Validates if there are active survey questionnaires on the document
         * @returns {boolean} True if 0 surveys remain
         */
        checkCompletionStatus() {
            const bodyText = document.body.innerText;
            return bodyText.includes('Bạn đang có 0 phiếu') || bodyText.includes('không có khảo sát');
        }

        /**
         * Emits finishing notes when everything is completed successfully
         */
        notifySuccess() {
            this.ui.writeConsole("🎉 Tuyệt vời! Đã hoàn thành toàn bộ phiếu khảo sát.", "success");
            setTimeout(() => {
                alert("✅ Tuyệt vời! Bạn đã hoàn thành toàn bộ phiếu khảo sát học phần kỳ này.");
            }, 500);
        }

        /**
         * Groups input labels dynamically to match DevExpress table mappings
         * @param {NodeList} labels - All labels targeting choice answers
         * @returns {Object} Grouped label mappings keyed by base question IDs
         */
        groupQuestionsByTarget(labels) {
            const groups = {};
            labels.forEach(label => {
                const table = label.closest(this.config.selectors.tableGroup);
                if (table) {
                    // Robust extraction logic replacing substring(0, lastIndexOf('_RB'))
                    // Match pattern: table.id starts with prefix and gets separated at _RB (followed by digits or end of word)
                    const match = table.id.match(/^(.+?)_RB/);
                    const questionId = match ? match[1] : table.id;
                    
                    if (!groups[questionId]) {
                        groups[questionId] = [];
                    }
                    groups[questionId].push(label);
                }
            });
            return groups;
        }

        /**
         * Core decision block. Fills options matching selected strategy rules
         * @param {Object} groups - Mappings of target question IDs to array of choice elements
         * @param {string} strategy - Chosen policy ('max' | 'realistic' | 'random')
         */
        autoFillAnswers(groups, strategy) {
            for (const key in groups) {
                const options = groups[key];
                let targetIndex = options.length - 1; // Default selector (highest rank)

                // Scan if there are specific average options needed (e.g. standard informational inputs)
                let matchedKeywordIndex = -1;
                for (let i = 0; i < options.length; i++) {
                    const optionText = options[i].innerText.toLowerCase();
                    const shouldPickAverage = this.config.keywords.averageScore.some(keyword => optionText.includes(keyword));
                    
                    if (shouldPickAverage) {
                        matchedKeywordIndex = i;
                        break;
                    }
                }

                if (matchedKeywordIndex !== -1) {
                    // Always pick average if it maps to keyword exclusions
                    targetIndex = matchedKeywordIndex;
                } else {
                    // Strategy application
                    if (strategy === 'realistic') {
                        if (options.length >= 3) {
                            // Realistic distribution: 75% Very Good (length - 1), 25% Good (length - 2)
                            targetIndex = Math.random() < 0.75 ? options.length - 1 : options.length - 2;
                        } else {
                            targetIndex = options.length - 1;
                        }
                    } else if (strategy === 'random') {
                        targetIndex = Math.floor(Math.random() * options.length);
                    } else {
                        // Max strategy
                        targetIndex = options.length - 1;
                    }
                }

                // Simulate DevExpress click bypass
                const selectedOption = options[targetIndex];
                const trRow = selectedOption.closest('tr');
                const fakeRadio = trRow ? trRow.querySelector(this.config.selectors.fakeRadioBtn) : null;

                if (fakeRadio) {
                    fakeRadio.click();
                } else {
                    selectedOption.click(); // Safety fallback trigger
                }
            }
        }

        /**
         * Submits the active questionnaire and redirects the page
         */
        submitForm() {
            const elements = document.querySelectorAll(this.config.selectors.submitCandidates);
            const submitBtn = Array.from(elements).find(el => {
                const text = el.innerText.toLowerCase().trim();
                return this.config.keywords.submitBtn.includes(text) && el.children.length === 0;
            });

            if (submitBtn) {
                const clickableWrapper = submitBtn.closest(this.config.selectors.clickableWrapper) || submitBtn;
                this.ui.writeConsole("Đang nộp phiếu khảo sát...", 'warn');
                clickableWrapper.click();

                // Wait for ASP.NET PostBack then clean reload by redefining window.location.href
                setTimeout(() => {
                    this.ui.writeConsole("Nộp thành công. Đang tải trang tiếp theo...", 'success');
                    window.location.href = window.location.href;
                }, this.ui.settings.reloadWaitMs);
                
            } else {
                this.ui.writeConsole("Không tìm thấy nút Gửi khảo sát. Vui lòng tự ấn gửi.", 'warn');
            }
        }
    }

    // ==========================================
    // 4. BOOTSTRAP INITIALIZATION
    // ==========================================
    // Check if DOM is loaded or wait for it
    const run = () => {
        const app = new SurveyAutomator();
        app.init();
    };

    if (document.readyState === 'complete' || document.readyState === 'interactive') {
        run();
    } else {
        window.addEventListener('DOMContentLoaded', run);
    }

})();
