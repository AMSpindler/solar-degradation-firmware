%% Measured (firmware CSV) vs theoretical (resistor board) — STAIRCASE sweep
%
% The sweep was a staircase: 0 -> Vmax in fixed voltage steps, ~2 s per step.
% The firmware did NOT record the applied voltage, so we map each measured
% current PLATEAU to its known voltage step (0,1,...,Vmax) and compare to the
% theoretical I = V / R.
%
% This file also contains idle time and a gap before the sweep, so the script
% auto-detects the real sweep window (the burst after the largest time gap).
% If auto-detect misses, set t_start/t_end by hand below.

clear; clc; close all;

%% ---- Settings (edit these) --------------------------------------------
csvfile = 'noUSB_cal_fixed.csv';
R_ind   = [109.3, 128.7, 129.4, 129.5];   % measured resistors (ohms)
R       = 30.1;                            % measured parallel (ohms)
V_steps = 0:1:10;                          % voltage at each step (0..10 V, 1 V each)
subtract_zero = false;                     % true = subtract the 0 V reading (isolate slope)

t_start = [];  t_end = [];                 % [] = auto-detect the sweep window; else seconds

% Theoretical (V, A) table from boardsetup.txt — reference points
Vtab = 0.5:0.5:10;
Itab = [0.014 0.030 0.046 0.062 0.079 0.095 0.112 0.127 0.144 0.160 ...
        0.177 0.193 0.210 0.228 0.244 0.260 0.277 0.292 0.308 0.326];

Nsteps = numel(V_steps);

%% ---- Load -------------------------------------------------------------
T = readtable(csvfile);
t = (T.timestamp_us - T.timestamp_us(1)) / 1e6;   % seconds from file start
i = T.i_amps;                                      % measured current (A)
fprintf('Parallel R from resistors: %.2f ohm (using measured %.1f)\n', ...
        1/sum(1./R_ind), R);

%% ---- Isolate the sweep window -----------------------------------------
if isempty(t_start) || isempty(t_end)
    dt = diff(t);
    [gap, gi] = max(dt);                 % biggest time gap in the file
    if gap > 5                           % real gap => sweep is the burst after it
        sel = t > t(gi);
    else
        sel = true(size(t));
    end
else
    sel = t >= t_start & t <= t_end;
end
tw = t(sel);  iw = i(sel);
tw = tw - tw(1);                          % re-zero time within the sweep
fprintf('Sweep window: %.1f s of data (%d samples)\n', tw(end), numel(tw));

%% ---- Split into Nsteps equal-time segments, average middle 60% --------
edges = linspace(0, tw(end), Nsteps + 1);
Imeas = nan(Nsteps, 1);
for k = 1:Nsteps
    lo = edges(k)   + 0.20 * (edges(k+1) - edges(k));
    hi = edges(k+1) - 0.20 * (edges(k+1) - edges(k));
    Imeas(k) = mean(iw(tw >= lo & tw < hi));
end

offset = 0;
if subtract_zero
    offset = Imeas(1);
    Imeas  = Imeas - offset;
    fprintf('Subtracted 0 V offset of %+.3f A\n', offset);
end

Ith = V_steps(:) / R;                     % theoretical current per step

%% ---- Common y-limits so the separate plots are visually comparable -----
allI  = [Imeas(:) + offset; Ith(:); Itab(:)];
ylims = [min(0, min(allI)) - 0.02, max(allI) * 1.05];
Vline = linspace(0, max(V_steps), 100);

%% ---- Figure 1: measured current over the sweep (time domain) -----------
figure('Color', 'w');
plot(tw, iw, '.', 'Color', [0.6 0.75 0.95], 'MarkerSize', 3); hold on;
tc = (edges(1:end-1) + edges(2:end)) / 2;
plot(tc, Imeas + offset, 'ko', 'MarkerFaceColor', 'r', 'MarkerSize', 6);
xlabel('time in sweep (s)'); ylabel('current (A)'); grid on;
title('Measured current over sweep (samples + per-step means)');
legend({'samples', 'per-step mean'}, 'Location', 'northwest');
styleblack(gca);

%% ---- Figure 2: I-V measured vs theoretical (OVERLAPPED) ----------------
figure('Color', 'w');
plot(V_steps, Imeas, 'bo-', 'LineWidth', 1.4, 'MarkerFaceColor', 'b', ...
     'DisplayName', 'measured (per step)'); hold on;
plot(Vline, Vline / R, 'r-', 'LineWidth', 1.6, ...
     'DisplayName', sprintf('theoretical  I = V/%.1f\\Omega', R));
plot(Vtab, Itab, 'ks', 'MarkerFaceColor', [1 0.85 0], 'MarkerSize', 6, ...
     'DisplayName', 'power supply i-v ref pts');
xlabel('applied voltage (V)'); ylabel('current (A)'); grid on;
legend('Location', 'northwest');
title('I-V: measured vs theoretical');
styleblack(gca);

%% ---- Figure 3: MEASURED I-V (alone) -----------------------------------
figure('Color', 'w');
plot(V_steps, Imeas, 'bo-', 'LineWidth', 1.6, 'MarkerFaceColor', 'b', ...
     'MarkerSize', 6, 'DisplayName', 'measured (per step)'); hold on;
plot(Vtab, Itab, 'ks', 'MarkerFaceColor', [1 0.85 0], 'MarkerSize', 6);
xlabel('applied voltage (V)'); ylabel('current (A)'); grid on;
ylim(ylims);
title('Measured I-V');
legend({sprintf('measured (per step)') 'power supply i-v ref pts'}, ...
    'Location', 'northwest');
styleblack(gca);

%% ---- Figure 4: THEORETICAL I-V (alone) --------------------------------
figure('Color', 'w');
plot(Vline, Vline / R, 'r-', 'LineWidth', 1.8); hold on;
plot(Vtab, Itab, 'ks', 'MarkerFaceColor', [1 0.85 0], 'MarkerSize', 6);
xlabel('applied voltage (V)'); ylabel('current (A)'); grid on;
ylim(ylims);
title(sprintf('Theoretical I-V  (I = V / %.1f \\Omega)', R));
legend({sprintf('theoretical I = V/%.1f\\Omega', R), 'power supply i-v ref pts'}, ...
       'Location', 'northwest');
styleblack(gca);

%% ---- Error table ------------------------------------------------------
fprintf('\n  V(V)  I_meas(A)  I_theory(A)   err(%%)\n');
fprintf(  '  ----  ---------  -----------  -------\n');
for k = 1:Nsteps
    if Ith(k) == 0
        fprintf('  %4.1f  %8.3f  %9.3f      --\n', V_steps(k), Imeas(k), Ith(k));
    else
        err = (Imeas(k) - Ith(k)) / Ith(k) * 100;
        fprintf('  %4.1f  %8.3f  %9.3f   %+6.1f\n', V_steps(k), Imeas(k), Ith(k), err);
    end
end

%% ---- helper: force axis numbers, labels, title, legend to BLACK -------
% (MATLAB's default axis text can render gray on some themes; this pins it.)
function styleblack(ax)
    set(ax, 'XColor', 'k', 'YColor', 'k', ...
            'GridColor', [0.8 0.8 0.8], 'GridAlpha', 0.9);
    set(get(ax, 'Title'),  'Color', 'k');   % title text
    set(get(ax, 'XLabel'), 'Color', 'k');   % x-axis label
    set(get(ax, 'YLabel'), 'Color', 'k');   % y-axis label
    lg = get(ax, 'Legend');
    if ~isempty(lg)
        set(lg, 'TextColor', 'w', 'Color', 'k', 'EdgeColor', [0.3 0.3 0.3]);   % legend entries
    end
end