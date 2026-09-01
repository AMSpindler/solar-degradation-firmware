%% Post-process de-noising of instantaneous VBUS, vs hardware VBUS_AVG
%
% Software-averaging the noisy instantaneous data (voltage_vbus) reduces noise
% just like the hardware 8-sample average (voltage_vbus_avg): averaging N samples
% divides random noise by sqrt(N). This script shows raw vs post-filtered, the
% noise-vs-window curve, and a direct comparison of hardware averaging vs
% software post-averaging.
%
% NOTE: files logged without --fresh --overwrite can contain out-of-order and
% duplicated samples (MQTT QoS-1 replay). cleanload() below sorts by timestamp
% and drops duplicates so the time axis is monotonic.

clear; clc; close all;

folder = fileparts(mfilename('fullpath')); if isempty(folder), folder = pwd; end
RATE     = 200;                         % which rate file to analyze (100..500)
DEMO_WIN = 16;                          % window shown in Figs 1-2 (samples)
WINS     = [1 2 4 8 16 32 64 128];      % post-average windows to sweep (Fig 3)

[t,  v ] = cleanload(fullfile(folder,'voltage_vbus',    sprintf('clouds_data_%dhz.csv',RATE)));
[th, vh] = cleanload(fullfile(folder,'voltage_vbus_avg',sprintf('clouds_data_%dhz.csv',RATE)));

Wt    = max(201, round(RATE*2));        % trend window (~2 s) >> any test window
trend = movmean(v, Wt);
rstd  = @(x) 1.4826 * median(abs(x - median(x))) * 1000;   % robust std, mV

sig_raw = rstd(v - trend);
sig_hw  = rstd(vh - movmean(vh, Wt));   % hardware VBUS_AVG noise (mV)

%% ---- Figure 1: raw vs post-filtered ramp ----------------------------
vf = movmean(v, DEMO_WIN);
figure('Color','w'); hold on;
plot(t, v,  '.', 'Color',[0.85 0.6 0.6], 'MarkerSize',3, 'DisplayName','raw (instantaneous)');
plot(t, vf, '-', 'Color',[0.15 0.35 0.8], 'LineWidth',1.4, 'DisplayName',sprintf('movmean N=%d',DEMO_WIN));
xlabel('time in sweep (s)'); ylabel('voltage (V)'); grid on;
title(sprintf('Instantaneous VBUS @ %d Hz: raw vs %d-sample moving average',RATE,DEMO_WIN));
legend('Location','northwest'); styleblack(gca);

%% ---- Figure 2: noise band, raw vs filtered -------------------------
sig_demo = rstd(vf - trend);
figure('Color','w'); hold on;
plot(t, v-trend,  '.', 'Color',[0.90 0.55 0.55], 'MarkerSize',4, 'DisplayName',sprintf('raw (\\sigma=%.1f mV)',sig_raw));
plot(t, vf-trend, '.', 'Color',[0.25 0.45 0.85], 'MarkerSize',4, 'DisplayName',sprintf('N=%d (\\sigma=%.1f mV)',DEMO_WIN,sig_demo));
yline(0,'k-','LineWidth',1,'HandleVisibility','off');
xlabel('time in sweep (s)'); ylabel('residual: voltage - trend (V)'); grid on;
title(sprintf('Noise band shrinks with post-averaging @ %d Hz',RATE));
legend('Location','northeast'); styleblack(gca);
ym = 4*sig_raw/1000; ylim([-ym ym]);

%% ---- Figure 3: noise vs post-average window ------------------------
sig = arrayfun(@(N) rstd(movmean(v,N) - trend), WINS);
figure('Color','w'); hold on;
plot(WINS, sig, 'o-', 'LineWidth',1.8, 'DisplayName','post-averaged (measured)');
plot(WINS, sig(1)./sqrt(WINS), '--', 'LineWidth',1.4, 'DisplayName','\sigma_0/\surdN (theory)');
yline(sig_hw, 'g-', 'LineWidth',1.6, 'DisplayName',sprintf('hardware VBUS\\_AVG (%.1f mV)',sig_hw));
set(gca,'XScale','log','YScale','log');
xlabel('post-average window N (samples)'); ylabel('voltage noise \sigma (mV)');
title(sprintf('Post-filter noise vs window @ %d Hz',RATE));
grid on; legend('Location','southwest'); styleblack(gca);

%% ---- Figure 4: hardware VBUS_AVG vs software post-averaging --------
% Software post-average at DEMO_WIN, for a like-for-like smoothing comparison.
vp = movmean(v, DEMO_WIN); sig_p = rstd(vp - trend);
figure('Color','w'); hold on;
plot(t,  v  - trend,           '.', 'Color',[0.88 0.6 0.6], 'MarkerSize',3, ...
     'DisplayName', sprintf('raw instantaneous (\\sigma=%.1f mV)', sig_raw));
plot(t,  vp - trend,           '.', 'Color',[0.20 0.40 0.85], 'MarkerSize',4, ...
     'DisplayName', sprintf('software post-avg N=%d (\\sigma=%.1f mV)', DEMO_WIN, sig_p));
plot(th, vh - movmean(vh,Wt),  '.', 'Color',[0.20 0.65 0.35], 'MarkerSize',4, ...
     'DisplayName', sprintf('hardware VBUS\\_AVG (\\sigma=%.1f mV)', sig_hw));
yline(0,'k-','LineWidth',1,'HandleVisibility','off');
xlabel('time in sweep (s)'); ylabel('residual: voltage - trend (V)'); grid on;
title(sprintf('Hardware VBUS\\_AVG vs software post-averaging @ %d Hz',RATE));
legend('Location','northeast'); styleblack(gca);
ym = 4*sig_raw/1000; ylim([-ym ym]);

%% ---- Console -------------------------------------------------------
fprintf('Rate %d Hz | raw=%.2f mV  hardware VBUS_AVG=%.2f mV\n', RATE, sig_raw, sig_hw);
for i=1:numel(WINS), fprintf('   post N=%-4d -> %6.2f mV\n', WINS(i), sig(i)); end
fprintf('Software post-avg N=%d = %.2f mV  vs hardware VBUS_AVG = %.2f mV.\n', DEMO_WIN, sig_p, sig_hw);

%% ---- helpers -------------------------------------------------------
function [t, v] = cleanload(path)
    T = readtable(path);
    [uts, ia] = unique(T.timestamp_us);      % sort ascending + drop duplicate stamps
    v = T.v_volts(ia);
    t = (uts - uts(1)) / 1e6;
    dropped = height(T) - numel(uts);
    if dropped > 0
        fprintf('%s: dropped %d out-of-order/duplicate rows (kept %d)\n', ...
                path, dropped, numel(uts));
    end
end

function styleblack(ax)
    set(ax,'XColor','k','YColor','k','GridColor',[0.8 0.8 0.8],'GridAlpha',0.9);
    set(get(ax,'Title'),'Color','k'); set(get(ax,'XLabel'),'Color','k'); set(get(ax,'YLabel'),'Color','k');
    lg = get(ax,'Legend'); if ~isempty(lg), set(lg,'TextColor','w','Color','k','EdgeColor',[0.3 0.3 0.3]); end
end