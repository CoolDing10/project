% Adaptive video controller reference experiment.
% Produces matlab_results.csv and controller_comparison.png.
clear; clc;

dt = 0.5;
t = (0:dt:60-dt)';
capacity = 8e6 * ones(size(t));
capacity(t >= 15 & t < 30) = 1.5e6;
capacity(t >= 30 & t < 45) = 5e6;
tail = t >= 45;
capacity(tail) = 3.5e6 + 2.0e6 .* sin(t(tail) .* 0.7);
loss = 0.005 * ones(size(t));
loss(capacity < 2e6) = 0.045;
jitter_ms = 8 * ones(size(t));
jitter_ms(capacity < 2e6) = 38;

modes = ["fixed", "pid", "mpc"];
results = struct();
for mode = modes
    bitrate = zeros(size(t)); queue = zeros(size(t)); gop = zeros(size(t));
    solve_ms = zeros(size(t)); bitrate(1) = 2.5e6; gop(1) = 50;
    integral = 0; previous_error = 0;
    for k = 2:numel(t)
        queue(k) = max(0, min(3, queue(k-1) + ...
            (bitrate(k-1) / max(capacity(k), 1) - 0.90) * dt));
        tic;
        switch mode
            case "fixed"
                bitrate(k) = 2.5e6; gop(k) = 50;
            case "pid"
                error = 0.080 - queue(k);
                integral = max(-0.5, min(0.5, integral + error * dt));
                derivative = (error - previous_error) / dt;
                previous_error = error;
                target = bitrate(k-1) + 1.3 * capacity(k) * error + ...
                    0.20 * capacity(k) * integral + 0.02 * capacity(k) * derivative;
                target = min(target * (1 - min(0.45, loss(k) * 3)), capacity(k) * 0.90);
                bitrate(k) = quantize_rate(target);
                gop(k) = 30; if loss(k) > 0.03, gop(k) = 15; end
            otherwise
                [bitrate(k), gop(k)] = mpc_step(bitrate(k-1), queue(k), ...
                    capacity(k), loss(k), jitter_ms(k), dt);
        end
        solve_ms(k) = toc * 1000;
    end
    results.(mode).bitrate = bitrate;
    results.(mode).queue = queue;
    results.(mode).gop = gop;
    results.(mode).solve_ms = solve_ms;
end

output = table(t, capacity, loss, results.fixed.bitrate, results.pid.bitrate, ...
    results.mpc.bitrate, results.fixed.queue * 1000, results.pid.queue * 1000, ...
    results.mpc.queue * 1000, results.mpc.gop, results.mpc.solve_ms, ...
    'VariableNames', {'time_s','capacity_bps','loss','fixed_bitrate','pid_bitrate', ...
    'mpc_bitrate','fixed_queue_ms','pid_queue_ms','mpc_queue_ms','mpc_gop','mpc_solve_ms'});
writetable(output, 'matlab_results.csv');

figure('Color', 'w', 'Position', [100 100 1000 650]);
subplot(2,1,1);
plot(t, capacity/1e3, 'k--', t, results.fixed.bitrate/1e3, ...
    t, results.pid.bitrate/1e3, t, results.mpc.bitrate/1e3, 'LineWidth', 1.2);
grid on; ylabel('kbps'); legend('capacity','fixed','PID','MPC','Location','best');
title('Adaptive bitrate under the same network trace');
subplot(2,1,2);
plot(t, results.fixed.queue*1000, t, results.pid.queue*1000, ...
    t, results.mpc.queue*1000, 'LineWidth', 1.2);
grid on; xlabel('time (s)'); ylabel('estimated queue (ms)');
legend('fixed','PID','MPC','Location','best');
exportgraphics(gcf, 'controller_comparison.png');
fprintf('MPC solve time: median %.3f ms, max %.3f ms\n', ...
    median(results.mpc.solve_ms), max(results.mpc.solve_ms));
assert(max(results.mpc.solve_ms) <= 20, 'MPC solve time exceeded 20 ms target');

function rate = quantize_rate(target)
ladder = [800 1200 1600 2200 3000 4500 6000] * 1e3;
[~, index] = min(abs(ladder - target)); rate = ladder(index);
end

function [first_rate, first_gop] = mpc_step(previous_rate, initial_queue, ...
    bandwidth, loss, jitter_ms, dt)
rates = [800 1200 1600 2200 3000 4500 6000] * 1e3;
gops = [10 15 30 60]; horizon = 5; beam_width = 24;
uncertainty = min(0.45, max(0.08, 0.08 + loss*2.5 + jitter_ms/250));
predicted = max(800e3, bandwidth * (1-uncertainty));
beam = [0 initial_queue previous_rate 0 30]; % cost, queue, prev, first rate/gop
for step = 1:horizon
    expanded = zeros(size(beam,1)*numel(rates)*numel(gops), 5); row = 1;
    for b = 1:size(beam,1)
        for rate = rates
            for gop = gops
                load = rate / predicted;
                queue = min(3, max(0, beam(b,2) + (load-0.88)*dt));
                quality = log(rate/800e3);
                change = log(rate/max(1,beam(b,3)));
                overflow = max(0,load-1);
                cost = beam(b,1) + 65*queue^2 + 35*overflow^2 + ...
                    4*change^2 + 18*loss*(gop/15)*load + 0.55*30/gop - 1.7*quality;
                first_rate_value = beam(b,4); first_gop_value = beam(b,5);
                if step == 1, first_rate_value = rate; first_gop_value = gop; end
                expanded(row,:) = [cost queue rate first_rate_value first_gop_value]; row=row+1;
            end
        end
    end
    expanded = sortrows(expanded,1);
    beam = expanded(1:min(beam_width,size(expanded,1)),:);
end
first_rate = beam(1,4); first_gop = beam(1,5);
end
