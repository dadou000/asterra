class_name RiverCouplingRuntimeStub
extends SparseHydrologyRuntime
## Test-only signal/ownership shell. The river coupling gate needs the real sparse
## atlas/scheduler and real cycle_completed boundary, but not a second SWE dispatcher.


func initialized_ok() -> bool:
	return atlas != null and scheduler != null


func busy() -> bool:
	return false


func advance_time(_dt_s: float) -> Error:
	return OK
