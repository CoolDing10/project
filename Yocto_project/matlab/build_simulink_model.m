% Programmatically builds the network queue plant used by the MPC model.
% Inputs are target bitrate and estimated link capacity (bit/s); output is
% queue delay in seconds. Run this script in MATLAB with Simulink installed.
model = 'adaptive_video_plant';
if bdIsLoaded(model), close_system(model, 0); end
new_system(model); open_system(model);

add_block('simulink/Sources/In1', [model '/TargetBitrate'], 'Position',[30 55 60 75]);
add_block('simulink/Sources/In1', [model '/Capacity'], 'Position',[30 125 60 145]);
add_block('simulink/Math Operations/Divide', [model '/Load'], 'Position',[105 65 145 135]);
add_block('simulink/Sources/Constant', [model '/SafetyMargin'], ...
    'Value','0.90','Position',[105 170 145 190]);
add_block('simulink/Math Operations/Sum', [model '/QueueSlope'], ...
    'Inputs','+-','Position',[190 90 220 125]);
add_block('simulink/Discrete/Discrete-Time Integrator', [model '/Queue'], ...
    'gainval','0.5','InitialCondition','0','LimitOutput','on', ...
    'UpperSaturationLimit','3','LowerSaturationLimit','0', ...
    'Position',[270 85 330 130]);
add_block('simulink/Sinks/Out1', [model '/QueueDelay'], 'Position',[385 100 415 120]);

add_line(model,'TargetBitrate/1','Load/1');
add_line(model,'Capacity/1','Load/2');
add_line(model,'Load/1','QueueSlope/1');
add_line(model,'SafetyMargin/1','QueueSlope/2');
add_line(model,'QueueSlope/1','Queue/1');
add_line(model,'Queue/1','QueueDelay/1');
set_param(model,'Solver','FixedStepDiscrete','FixedStep','0.5','StopTime','60');
save_system(model, [model '.slx']);
fprintf('Created %s.slx\n', model);
