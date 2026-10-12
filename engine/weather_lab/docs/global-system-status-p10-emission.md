+++
path = "/world/weather-lab/global-system-status-p10-emission"
title = "Storm domain: the physical emission of a lightning flash (timing, colour, brightness)"
kind = "concept"
status = "experimental"
summary = """
What the literature gives for the timing, temperature, colour and brightness of lightning, and the emission model built from it: a stepped leader that glows as it forms, a return stroke that runs along the main path only, dark twigs, later strokes in the same channel, continuing current, and a cooling from blue-white to orange; with the numbers, their sources and what is not known."""
keywords = ["return stroke", "stepped leader", "dart leader", "continuing current", "channel temperature", "blackbody colour", "optical power", "interstroke interval", "LightningEmission", "AssignPhysicalEmission"]
sources = [
  "engine/storm_electrification/src/LightningEmission.cpp",
  "engine/storm_electrification/include/orbit/storm_electrification/LightningEmission.hpp",
  "engine/optical_transport/include/orbit/optical_transport/Blackbody.hpp",
]
symbols = ["AssignPhysicalEmission", "EmissionParams", "BlackbodyLinearRgb"]
invariants = [
  "Every emission event cools with T(t) = (Tpeak - Tslow) exp(-t/tau1) + Tslow exp(-t/tau2) and radiates (T/Tpeak)^4 of its peak power; it is written as a few pulses with the mean temperature of each interval, and the colour of a pulse is the blackbody colour of its temperature.",
  "Only the main path (root to the ground contact, or to the end of the heaviest chain) is struck again after the first stroke; the first stroke also lights the side branches that carry a noticeable share of the charge; twigs are lit only by their own leader glow.",
  "The colour is the Planckian colour of the estimated temperature, an assumption: a lightning channel emits lines and continuum, and no measured chromaticity was found.",
  "The leader speed of the growth (formedAt) is a rendering choice (2.5e6 m/s in Studio, so that a 400-cell channel grows in about 100 ms); the measured average is 1 to 2e5 m/s.",
]
related = ["/world/weather-lab/global-system-status-p10-shape", "/rendering/unified-transport/storm-electrification"]
depends_on = ["/world/weather-lab/global-system-status-p10-shape"]
verify = ["ctest -R \"Orbit.StormElectrification\"", "frame_strike and pause_on_strike on, time.set rate 0.01: orange leader fragments, one blue-white flash of the channel and its branches, an orange afterglow, then thin strikes of the main path alone"]
verified = "8588c38"
+++

**What was asked for (the user, from a screenshot and a photograph):** a flash whose branches do not all look like the main one. The branches glow as the leader makes them, each reaching its own peak and fading; when the main channel attaches, the return stroke follows at once, the branches that have no electrical importance vanish, and later strokes may come down the same channel. Colours should follow the temperature of the channel: hot and blue-white for a strong stroke, fading to orange as it cools, the cooling longer (not the flash) for a bigger current.

**What the literature says** (single searches, not a review; where a source is secondary it is said):

| Quantity | Value | Source |
| --- | --- | --- |
| Stepped leader: step length, pause, speed | 3 to 200 m (typically 50 m); 5 to 100 us between steps (10 to 200 in another review; video: 5 m and 16 us); mean 1 to 2e5 m/s, measured 0.8e5 to 19e5 | [NOAA JetStream](https://www.noaa.gov/jetstream/lightning/how-lightning-is-created/jetstream-max-lightning-process-keeping-in-step), [Hill et al. 2011](https://agupubs.onlinelibrary.wiley.com/doi/full/10.1029/2011jd015818), [Atmosphere 2017](https://doi.org/10.3390/atmos8120245) |
| Leader brightness | 20 to 100 times fainter than the return stroke (a patent, secondary) | [US patent 6243242](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/6243242) |
| Abandoned leader branches | brief reconnections ("sparks") peak at 1 to 3 times their parent within 40 us and reach a few hundred metres at most; luminosity pulses fall to about 10 % within 50 m | [Hill et al. 2011](https://agupubs.onlinelibrary.wiley.com/doi/full/10.1029/2011jd015818) |
| Branches after the attachment | lit by the return stroke; a competing branch used by a later stroke is often dark during the earlier one; residual glow of an old stroke ends 12 ms before the next dart-stepped leader | [Hill et al. 2011](https://agupubs.onlinelibrary.wiley.com/doi/full/10.1029/2011jd015818), [Petersen & Beasley 2013](https://agupubs.onlinelibrary.wiley.com/doi/full/10.1002/2013jd019910) |
| Dart leader speed | measured 1 to 2e6 m/s (quoted up to 1e7) | [Nature Sci. Rep. 2022](https://www.nature.com/articles/s41598-022-10366-x) |
| Peak current | first stroke about 2.3 to 2.5 times the subsequent ones (30 kA and 12 kA are typical) | [First versus subsequent strokes](https://www.researchgate.net/publication/241061470_First_versus_subsequent_return-stroke_current_and_field_peaks_in_negative_cloud-to-ground_lightning_discharges), [Parameters of lightning strokes: a review](https://www.researchgate.net/publication/3274941_Parameters_of_lightning_strokes_A_review) |
| Strokes and interval | typical interval 35 ms; mean 4.6 strokes per flash in Florida (76 flashes), 6.4 in New Mexico (83); a flash has 3 to 4 subsequent strokes | [Univ. of Arizona lecture 15](http://www.atmo.arizona.edu/students/courselinks/spring13/atmo589/ATMO489_online/lecture_15/lect15a_lightning_pt2.html) |
| Continuing current | 53 % of 4495 strokes had one of at least 3 ms, 7 % longer than 40 ms (up to 714 ms); CIGRE: 27 % of negative flashes | [Vaisala study](https://www.vaisala.com/sites/default/files/documents/Presence%20of%20Continuing%20Current%20in%20Negative%20Cloud-to-Ground%20Flashes.pdf) |
| Optical power | first stroke 2.3e9 W peak, subsequent 4.8e8 W; per metre 1e6 W/m first, 2.5e5 W/m subsequent; rocket-triggered 0.25 to 9.5 MW/m (mean 2.4); 10 to 90 % light risetime 1 to 60 us (1.2 us in a triggered one) | [Guo & Krider 1982](https://agupubs.onlinelibrary.wiley.com/doi/10.1029/JC087iC11p08913), [Quick & Krider 2017](https://agupubs.onlinelibrary.wiley.com/doi/full/10.1002/2017JD027363) |
| Channel temperature | peak 28 to 31 kK in five of ten strokes (36 kK the hottest, Orville 1968, via a secondary summary); 34.8 +- 8.3 kK mean of 23 strokes; a photometric study 17.6 kK; core 30 kK falling to 500 K at 10 cm | [Boggs et al. 2021](https://agupubs.onlinelibrary.wiley.com/doi/full/10.1029/2020JD034438), [Orville 1968](https://journals.ametsoc.org/jas/article/25/5/827/17614/A-High-Speed-Time-Resolved-Spectroscopic-Study-of) |
| Cooling | about 100 kK/ms in the first millisecond without continuing current, faster for larger currents; 20 kK held for 200 to 400 us; with continuing current 20 kK for at least 10 ms cooling 1.5 kK/ms | [Boggs et al. 2021](https://agupubs.onlinelibrary.wiley.com/doi/full/10.1029/2020JD034438) |
| Spectrum in time | ionic lines (N II, O II) dominate the first 20 us, then continuum, then neutral atoms (H-alpha, O I, N I), which fade over 150 us and more | [Walker & Christian 2017](https://agupubs.onlinelibrary.wiley.com/doi/full/10.1002/2016JD026419) |
| Intracloud | recoil leaders (K changes) form tens of ms after current cutoff in the branched channel of a positive leader, spaced of order 10 ms | [Mazur 2016](https://www.sciencedirect.com/science/article/abs/pii/S0304388616300407), [Winn et al. 2011](https://agupubs.onlinelibrary.wiley.com/doi/full/10.1029/2011JD015998) |

**Not found:** a measured chromaticity or colour temperature of lightning (the colours here are the blackbody colours of the estimated temperature, which is a modelling choice: 30 kK is blue-white, 6500 K white, 3500 K orange; the real spectrum has lines, and the late glow is rich in H-alpha red); a quantified fade time of a branch just before the return stroke; the distribution of continuing current amplitudes.

**The model** (`LightningEmission.cpp`; the numbers above in `EmissionParams`):
- *Leader*: every segment glows as it forms, 9000 K, 2e4 W/m (a fiftieth of a first stroke), cooling in 150 us then 1.5 ms toward 3500 K.
- *Attachment*: a return stroke runs along the main path at 1.5e8 m/s; the current falls to half toward the root; the first stroke also lights each branch with at least 1.2 % of the charge, at up to half the current, starting where it leaves the main path; the others are dark.
- *Current to light*: peak power per metre 1e6 W/m (I / 30 kA)^1.5 (this ties the 1e6 and 2.5e5 W/m figures to 30 and 12 kA); peak temperature 30 kK (I / 30 kA)^0.25 between 8 and 36 kK; tau1 = 250 us (slightly shorter for larger currents, as measured); tau2 = 2 ms (I / 10 kA)^0.7 between 0.5 and 30 ms, so a bigger current leaves a longer, not brighter, orange glow; the glow is (T/Tpeak)^4 of the peak, so the cooled channel is about 5e-4 of a stroke.
- *Later strokes*: about three more, 35 ms apart (log-normal), each preceded by a dim dart leader (2e6 m/s, 5e4 W/m, 12 kK) down the main path, current 12 kA (log-normal); half of the strokes are followed by a continuing current (3 to 40 ms, 14 % of those 40 to 300 ms) that keeps the whole path glowing at 7000 K, then cools. An intracloud channel gets the discharge of the whole tree and two to five recoil events along its main path about 12 ms apart.

**Seen:** at a rate of 0.01 the sequence is orange leader fragments at the growing tip, one blue-white flash of the channel with its branches, an orange afterglow, then thin strokes of the main channel alone. Tests: the heavy branch flashes too, dimmer than the main path; a twig carrying no charge is not heated by the return stroke; later strokes come tens of milliseconds after the first; the channel cools to a few thousand kelvin over milliseconds; 30 kK is blue-white, 6500 K white and 3500 K orange.

**Not done:** a later stroke taking a side branch (the observation that a stroke can follow a different branch is not modelled: later strokes always take the main path); positive flashes (one stroke, long continuing current); the leader's real speed (the growth is faster than measured so that the whole flash is watchable); sprites and spectral rendering: the light on the clouds takes the blackbody colour of the pulses.

**Cloud-to-ground strikes (requested next step).** The leader reached the ground in only 2.5 % of the flashes (6 of 240), because a channel grown toward strong field in air without charge has no reason to descend. `LeaderParams::groundSeekChance` makes that share of the flashes descending leaders: their steps going down are multiplied by 1 + `groundSeekBias` (12) times the downward component, and they do not stop for the charge budget or for crossing air with no charge (the stagnation rule), only when they connect or reach the cell cap. A grounded flash gets the full sequence of the emission model: the return stroke up the whole path, the dim dart leaders and later strokes, continuing current. Studio: `ground_strike_chance` (0 to 1, default 0.25) in `storm.lightning` and `orbit_storm_lightning`; the flashes per electrification step are capped at 2 (each costs a potential solve over the window), so the worker keeps up at 1x (9 s of work per 10 storm seconds). Measured at 1x for 120 s: 48 flashes, 18 reaching the ground (37 %, more than the 25 % asked for because the plain growth also grounds now and then); test: 2 of 3 seeking flashes ground, 0 of 3 plain ones. Not verified in a picture: with every flash seeking the ground the screenshots show several flashes at once and no clean single bolt, so how a ground strike looks is for the user to judge; frame strike and strike pause treat all flashes alike.

