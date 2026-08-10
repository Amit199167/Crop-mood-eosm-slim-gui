/* Dual ISO interface */

extern WEAK_FUNC(ret_0) int dual_iso_set_enabled(bool enabled);

extern WEAK_FUNC(ret_0) int dual_iso_is_enabled();

extern WEAK_FUNC(ret_0) int dual_iso_is_active();

extern WEAK_FUNC(ret_0) int dual_iso_get_recovery_iso(); /* raw iso values */

extern WEAK_FUNC(ret_0) int dual_iso_set_recovery_iso(int raw_iso);

extern WEAK_FUNC(ret_0) int dual_iso_calc_dr_improvement(int iso1, int iso2); /* ev x100 */

extern WEAK_FUNC(ret_0) int dual_iso_get_dr_improvement(); /* with current settings */

extern WEAK_FUNC(ret_0) int dual_iso_slim_step_pair(int delta); /* EOS M slim: LV arrow ISO pairs */

/* EOS M slim: Live View ISO editor changes the second (recovery) ISO only. */
extern WEAK_FUNC(ret_0) int dual_iso_slim_step_recovery(int delta);
