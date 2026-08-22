# Human-eye exposure calibration

`HumanEyeExposure` combines Godot's GPU auto-exposure meter with asymmetric
visual-adaptation timing. The renderer still measures the actual HDR frame; the
controller changes its response rate according to whether the observer is moving
toward a brighter or darker adaptation state.

## Timing model

- Bright adaptation uses a `0.20 s` exponential time constant (`5.0 s^-1`).
  Time-dependent visual-adaptation literature reports that the fast neural
  component completes in roughly 200 ms or less.
- Cone dark adaptation uses the measured exponential
  `0.996787282^(t / 0.28)`, equivalent to `0.01149 s^-1`.
- Rod adaptation begins after `420 s` and uses
  `0.999637396^((t - 420) / 0.28)`, equivalent to `0.001295 s^-1`.
- Returning to meaningful light resets accumulated rod adaptation immediately.

The cone/rod constants and phase boundary follow the biphasic fit reported in:

- Hirota et al., *Adaptive light: a lighting control method aligned with dark
  adaptation of human vision*, Scientific Reports 10, 2020:
  https://doi.org/10.1038/s41598-020-68119-7
- Pattanaik, Tumblin, Yee, and Greenberg, *Time-Dependent Visual Adaptation for
  Fast Realistic Image Display*, SIGGRAPH 2000:
  https://users.cs.northwestern.edu/~jet/Publications/Tumblin_Time00.pdf

## Meter bounds

The project does not enable physical light units, so no honest conversion from
its shader values to `cd/m^2` exists. The thresholds below are therefore exact
in Asterra's scene-linear renderer space, not claimed as universal biological
cutoffs:

- highlight meter clamp: `12.0`, matching the configured AgX white point;
- initial cone darkness floor: `1/64` (`0.015625`);
- fully adapted rod darkness floor: `1/512` (`0.001953125`);
- exposure target: `0.18` middle grey.

At the fixed ISO 100 baseline, Godot converts practical sensitivity to linear
luminance as `sensitivity / 800`. The controller therefore supplies exact ISO
meter bounds of `9600`, `12.5`, and `1.5625` respectively.

Godot's relevant API and reduction implementation:

- https://docs.godotengine.org/en/4.7/classes/class_cameraattributes.html
- https://docs.godotengine.org/en/4.7/classes/class_cameraattributespractical.html
- https://github.com/godotengine/godot/blob/master/servers/rendering/renderer_rd/shaders/effects/luminance_reduce.glsl
