%% Voltage: instantaneous VBUS vs 8-sample VBUS_AVG — timing & noise
%
% Two folders next to this script:
%   voltage_vbus/      clouds_data_<rate>hz.csv   (PAC1951_STREAM_AVG 0, instantaneous)
%   voltage_vbus_avg/  clouds_data_<rate>hz.csv   (PAC1951_STREAM_AVG 1, 8-sample avg)
% Each run: external supply ramped 5.0 -> 25.7 V by tenths.
%
% NOISE from SUCCESSIVE DIFFERENCES (removes the ramp): for a slow ramp,
% consecutive samples differ mostly by noise, so the SPREAD of diff(v) is pure
% noise. Two independent noisy samples => Var(diff)=2*sigma^2 => sigma=std(diff)/sqrt(2).
% We use a robust form, sigma = 1.4826*MAD(diff)/sqrt(2), so occasional jumps
% (e.g. a reboot) don't inflate the estimate. (MAD = median abs deviation.)

clear; clc; close all;

folder = fileparts(mfilename('fullpath')); if isempty(folder), folder = pwd; end
rates  = [100 200 300 400 500];
sets   = {'voltage_vbus','voltage_vbus_avg'};
labels = {'VBUS (instantaneous)','VBUS\_AVG (8-sample)'};
cols   = lines(numel(rates));

%% ---- Load + measure every file ---------------------------------------
S = struct();
fprintf('%-18s %5s %10s %12s %8s\n','folder','rate','eff(Hz)','noise(mV)','n');
for s = 1:numel(sets)
    for k = 1:numel(rates)
        f = fullfile(folder, sets{s}, sprintf('clouds_data_%dhz.csv', rates(k)));
        T = readtable(f);
        t = (T.timestamp_us - T.timestamp_us(1)) / 1e6;    % seconds from start
        v = T.v_volts;
        dt  = diff(t);
        eff = 1 / median(dt(dt > 0));                      % effective sample rate (Hz)
        d    = diff(v);                                     % successive differences
        madd = median(abs(d - median(d)));                 % robust spread
        sig  = 1.4826 * madd / sqrt(2);                    % noise std (V)
        S(s,k).t = t; S(s,k).v = v; S(s,k).eff = eff; S(s,k).sigma = sig; S(s,k).n = numel(v);
        fprintf('%-18s %5d %10.1f %12.2f %8d\n', sets{s}, rates(k), eff, sig*1000, numel(v));
    end
end

%% ---- Figures 1 & 2: samples vs time, one per folder ------------------
% The ramp itself; the legend reports each file's EFFECTIVE rate + sample count,
% so you can confirm each CSV actually ran at (or plateaued below) its set rate.
for s = 1:numel(sets)
    figure('Color','w'); hold on;
    for k = 1:numel(rates)
        plot(S(s,k).t, S(s,k).v, '.', 'Color', cols(k,:), 'MarkerSize', 3, ...
             'DisplayName', sprintf('%d Hz set  (eff %.0f Hz, n=%d)', ...
                                    rates(k), S(s,k).eff, S(s,k).n));
    end
    xlabel('time in sweep (s)'); ylabel('voltage (V)'); grid on;
    title([labels{s} ' — samples vs time (5 \rightarrow 25.7 V ramp)']);
    legend('Location','northwest'); styleblack(gca);
end

%% ---- Figure 3: did each run hit its SET sampling rate? ---------------
% Effective rate (from median dt) vs the nominal rate. On the dashed line = hit
% it; below = the loop couldn't keep up (throughput ceiling).
figure('Color','w'); hold on;
plot(rates, rates, 'k--', 'LineWidth', 1.2, 'DisplayName', 'ideal (eff = set)');
eff1 = arrayfun(@(k) S(1,k).eff, 1:numel(rates));
eff2 = arrayfun(@(k) S(2,k).eff, 1:numel(rates));
plot(rates, eff1, 'o-', 'LineWidth', 1.6, 'MarkerFaceColor','auto', 'DisplayName', labels{1});
plot(rates, eff2, 's-', 'LineWidth', 1.6, 'MarkerFaceColor','auto', 'DisplayName', labels{2});
xlabel('set (nominal) rate (Hz)'); ylabel('measured effective rate (Hz)');
title('Did each CSV run at its set sampling rate?'); grid on;
legend('Location','northwest'); styleblack(gca);

%% ---- Figure 4: noise vs sample rate (comparison + theory) ------------
% VBUS vs VBUS_AVG noise across rates, plus the sqrt(8) prediction. Flat lines
% => noise is FLOOR-limited (rate doesn't change it). VBUS_AVG below the theory
% line => it has hit the ~1-LSB quantization floor (even better than sqrt(8)).
sig1 = arrayfun(@(k) S(1,k).sigma*1000, 1:numel(rates));   % mV
sig2 = arrayfun(@(k) S(2,k).sigma*1000, 1:numel(rates));
figure('Color','w'); hold on;
plot(rates, sig1, 'o-', 'LineWidth', 1.8, 'DisplayName', 'VBUS (instantaneous)');
plot(rates, sig2, 's-', 'LineWidth', 1.8, 'DisplayName', 'VBUS\_AVG (measured)');
plot(rates, sig1/sqrt(8), '--', 'LineWidth', 1.4, 'DisplayName', 'VBUS / \surd8 (theory)');
xlabel('sample rate (Hz)'); ylabel('voltage noise \sigma (mV)');
title('Voltage noise vs sample rate'); grid on;
ylim([0, max(sig1)*1.15]); legend('Location','east'); styleblack(gca);

%% ---- Figure 5: residual (noise) vs time — SEE the band shrink -------
% Detrend each run by subtracting a short centered moving average (the local
% ramp), leaving just noise. Overlay both folders at one rate so the tight
% VBUS_AVG cloud sits visibly inside the wide VBUS cloud.
REP = 200; ki = find(rates == REP, 1);
figure('Color','w'); hold on;
res = cell(1,2); sg = [S(1,ki).sigma S(2,ki).sigma]*1000;
for s = 1:2
    w = max(5, round(0.3 * S(s,ki).eff));       % ~0.3 s window; centered mean of a
    res{s} = S(s,ki).v - movmean(S(s,ki).v, w); % locally-linear ramp = noise only
end
plot(S(1,ki).t, res{1}, '.', 'Color',[0.90 0.55 0.55], 'MarkerSize',4, ...
     'DisplayName', sprintf('VBUS  (\\sigma=%.1f mV)', sg(1)));
plot(S(2,ki).t, res{2}, '.', 'Color',[0.25 0.45 0.85], 'MarkerSize',4, ...
     'DisplayName', sprintf('VBUS\\_AVG  (\\sigma=%.1f mV)', sg(2)));
yline(0, 'k-', 'LineWidth', 1);
xlabel('time in sweep (s)'); ylabel('residual: voltage - local trend (V)'); grid on;
title(sprintf('Noise band around the ramp @ %d Hz (VBUS\\_AVG sits inside VBUS)', REP));
legend('Location','northeast'); styleblack(gca);
ymax = 4 * sg(1) / 1000; ylim([-ymax ymax]);    % symmetric, scaled to the wider band

%% ---- Console: mean noise + improvement factor -----------------------
m1 = mean(sig1); m2 = mean(sig2);
fprintf('\nMean noise:  VBUS = %.2f mV   VBUS_AVG = %.2f mV   improvement = %.2fx\n', ...
        m1, m2, m1/m2);
fprintf('sqrt(8) theory would predict %.2fx; measured is higher because VBUS_AVG\n', sqrt(8));
fprintf('has dropped to ~1 LSB (quantization floor).\n');

%% ---- helper: black axis text, light-gray grid, white-on-black legend --
function styleblack(ax)
    set(ax, 'XColor','k', 'YColor','k', 'GridColor',[0.8 0.8 0.8], 'GridAlpha',0.9);
    set(get(ax,'Title'),  'Color','k');
    set(get(ax,'XLabel'), 'Color','k');
    set(get(ax,'YLabel'), 'Color','k');
    lg = get(ax,'Legend');
    if ~isempty(lg)
        set(lg, 'TextColor','w', 'Color','k', 'EdgeColor',[0.3 0.3 0.3]);
    end
end