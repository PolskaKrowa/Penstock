// Penstock's embedded superoptimiser.
//
// This is deliberately compiled by the stage-A bootstrap compiler and linked
// into stage-C Penstock.  Freshet owns the optimiser's control loop; the small
// native ABI below supplies LLVM/Z3/Alive2 operations which are not part of
// the Freshet language runtime.
extern fn penstock_so_begin(path: str, level: i64, budget_ms: i64,
                            objective: i64) -> i64;
extern fn penstock_so_step(state: i64) -> i64;
extern fn penstock_so_finish(state: i64) -> i64;

export fn penstock_freshet_superopt(path: str, level: i64,
                                  budget_ms: i64, objective: i64) -> i64 {
    let state: i64 = penstock_so_begin(path, level, budget_ms, objective);
    if (state == 0) { return 0; }

    // Every iteration examines at most one candidate.  The search and
    // acceptance policy therefore remain visible in Freshet rather than
    // hiding the optimiser behind a Souper/opt subprocess.
    while (penstock_so_step(state) != 0) {
    }

    return penstock_so_finish(state);
}
