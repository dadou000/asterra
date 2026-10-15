# Storm Lab

The project the lightning and storm-domain work is tested in: Earth, a placed storm with strong electrification and the lightning
settings (arcs, flash size, small-flash fraction, ground strikes) saved in `Config/storm_domain.json`, which Studio loads at start, so the
storm is there after every restart and hot handoff. The camera and clock are restored by `Config/session_state.json` (not tracked).

Launch it, place the storm and leave a night scene running with `python tools/storm_lab.py` (Studio itself opens a project given as its argument).

Terrain bakes and cloud volumes are generated on first launch and are not tracked.
