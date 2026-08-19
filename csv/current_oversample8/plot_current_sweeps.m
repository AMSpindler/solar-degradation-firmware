%% Current sweeps: measured vs theoretical linear ramp — 100..500 Hz
%
% Each CSV is a continuous 1 -> 5 A current ramp captured at a different sample
% rate. The applied current increased LINEARLY in time, so the THEORETICAL
% baseline is a straight line:
%
%        I_theory(t) = b*t + a          (a linear ramp)
%
% We fit that line by least squares (the ideal ramp the supply was applying)
% and compare the measured current to it. The scatter of the measured points
% ABOUT the line is the sensor noise (RMSE); the slope b is the ramp rate
% (A/s) and R^2 says how linear the sweep actually was.
%
% Two baseline modes (use_nominal):
%   false -> least-squares BEST-FIT ramp. The line slides/tilts to best match
%            the data, so residuals show noise + curvature about that best line.
%            Blind to absolute gain/offset (the fit absorbs them). Use this for
%            the noise / linearity analysis.
%   true  -> ANCHORED ramp: a straight line pinned to the sweep's start and end
%            current. By default the anchors are auto-derived from the data
%            (mean of the first/last edge_frac of samples), which reveals
%            mid-sweep BOW / nonlinearity (residuals arc away from 0 in the
%            middle if the ramp curved). If you instead set I_lo/I_hi below to
%            the KNOWN commanded currents at the first/last sample, it becomes
%            an ABSOLUTE accuracy check (measured vs commanded).

clear; clc; close all;

%% ---- Settings (edit these) --------------------------------------------
folder = fileparts(mfilename('fullpath'));      % this script's folder (CSVs live here)
if isempty(folder), folder = pwd; end
rates  = [100 200 300 400 500];                 % Hz labels = filename numbers
files  = arrayfun(@(r) sprintf('current_sweep_%dhz.csv', r), rates, ...
                  'UniformOutput', false);

use_nominal = false;        % false = least-squares fit; true = endpoint-anchored ramp
I_lo = [];  I_hi = [];      % [] = auto-anchor from data; set to KNOWN commanded
                            % currents (A) at first/last sample for absolute check
edge_frac = 0.02;           % fraction of samples averaged at each end for the anchor

if use_nominal, baseline_name = 'anchored ramp';
else,           baseline_name = 'best-fit ramp'; end

cols = lines(numel(rates)); % a distinct color per rate

%% ---- Load + fit each file ---------------------------------------------
S = struct('rate',{},'t',{},'i',{},'fit',{},'p',{},'R2',{},'rmse',{});
for k = 1:numel(files)
    T = readtable(fullfile(folder, files{k}));
    t = (T.timestamp_us - T.timestamp_us(1)) / 1e6;   % seconds from file start
    i = T.i_amps;                                      % measured current (A)

    if use_nominal
        ne = max(1, round(edge_frac * numel(i)));      % samples per end-window
        lo = I_lo;  hi = I_hi;                          % known commanded, or []
        if isempty(lo), lo = mean(i(1:ne));         end % actual start current
        if isempty(hi), hi = mean(i(end-ne+1:end)); end % actual end current
        p = [(hi - lo)/t(end), lo];                     % line through the anchors
    else
        p = polyfit(t, i, 1);                          % least-squares linear ramp
    end
    Ifit  = polyval(p, t);
    resid = i - Ifit;
    R2    = 1 - sum(resid.^2) / sum((i - mean(i)).^2);
    rmse  = sqrt(mean(resid.^2));

    S(k) = struct('rate',rates(k), 't',t, 'i',i, 'fit',Ifit, ...
                  'p',p, 'R2',R2, 'rmse',rmse);
    fprintf('%3d Hz: slope=%.4f A/s  intercept=%.3f A  R^2=%.5f  RMSE=%.4f A  (n=%d)\n', ...
            rates(k), p(1), p(2), R2, rmse, numel(t));
end

%% ---- Figure 1: all measured sweeps + baselines (overlaid) -------------
figure('Color','w'); hold on;
for k = 1:numel(rates)
    plot(S(k).t, S(k).i, '.', 'Color', cols(k,:), 'MarkerSize', 3, ...
         'HandleVisibility','off');                             % measured dots
    plot(S(k).t, S(k).fit, '-', 'Color', cols(k,:), 'LineWidth', 1.6, ...
         'DisplayName', sprintf('%d Hz  (%.3f A/s)', rates(k), S(k).p(1)));
end
xlabel('time in sweep (s)'); ylabel('current (A)'); grid on;
title(['Current sweeps: measured (dots) + ' baseline_name ' (lines)']);
legend('Location','northwest');
styleblack(gca);

%% ---- Figure 2: per-rate measured vs baseline (subplots) ---------------
figure('Color','w');
tl = tiledlayout(2, 3, 'TileSpacing','compact', 'Padding','compact');
for k = 1:numel(rates)
    ax = nexttile; hold on;
    plot(S(k).t, S(k).i, '.', 'Color',[0.6 0.75 0.95], 'MarkerSize', 3);
    plot(S(k).t, S(k).fit, 'r-', 'LineWidth', 1.6);
    xlabel('time (s)'); ylabel('current (A)'); grid on;
    title(sprintf('%d Hz:  R^2=%.4f,  RMSE=%.3f A', rates(k), S(k).R2, S(k).rmse));
    legend({'measured', baseline_name}, 'Location','northwest');
    styleblack(ax);
end
title(tl, 'Measured current vs theoretical linear ramp, per sample rate');

%% ---- Figure 3: noise (RMSE about the ramp) vs sample rate -------------
figure('Color','w');
bar([S.rate], [S.rmse], 0.6, 'FaceColor',[0.35 0.55 0.85]);
xlabel('sample rate (Hz)'); ylabel('RMSE about linear ramp (A)'); grid on;
title('Deviation from theoretical ramp vs sample rate');
styleblack(gca);

%% ---- Figure 4: residuals (measured - baseline) vs time ---------------
% Flat cloud centered on 0 = pure random noise (averaging helps). A tilt or
% curve in the smoothed trend = SYSTEMATIC error (gain/offset/nonlinearity)
% that grows with current and that averaging CANNOT remove — fix by calibration.
figure('Color','w');
tl4 = tiledlayout(2, 3, 'TileSpacing','compact', 'Padding','compact');
for k = 1:numel(rates)
    ax = nexttile; hold on;
    r = S(k).i - S(k).fit;                              % residual (A)
    plot(S(k).t, r, '.', 'Color',[0.6 0.75 0.95], 'MarkerSize', 3);
    yline(0, 'k-', 'LineWidth', 1);                     % zero reference
    w = max(1, round(numel(r)/40));                     % ~40-point smoothing window
    plot(S(k).t, movmean(r, w), 'r-', 'LineWidth', 1.6);% trend under the noise
    xlabel('time (s)'); ylabel('residual (A)'); grid on;
    title(sprintf('%d Hz:  RMSE=%.3f A', rates(k), S(k).rmse));
    legend({'residual','smoothed trend'}, 'Location','northwest');
    styleblack(ax);
end
title(tl4, ['Residuals: measured - ' baseline_name '   (flat = noise; tilt/curve = bias)']);

%% ---- helper: black axis text, light-gray grid, white-on-black legend --
function styleblack(ax)
    set(ax, 'XColor','k', 'YColor','k', ...
            'GridColor',[0.8 0.8 0.8], 'GridAlpha',0.9);
    set(get(ax,'Title'),  'Color','k');
    set(get(ax,'XLabel'), 'Color','k');
    set(get(ax,'YLabel'), 'Color','k');
    lg = get(ax,'Legend');
    if ~isempty(lg)
        set(lg, 'TextColor','w', 'Color','k', 'EdgeColor',[0.3 0.3 0.3]);
    end
end