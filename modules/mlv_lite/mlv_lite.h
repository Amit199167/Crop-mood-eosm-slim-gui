/* mlv_lite interface */
extern WEAK_FUNC(ret_0) int which_output_format();
extern WEAK_FUNC(ret_0) int is_more_hacks_selected();
extern WEAK_FUNC(ret_0) int AeWbTask_Disabled();
extern WEAK_FUNC(ret_0) int mlv_raw_rec_busy();
/* EOS M slim: Settings → INFO Button = framing toggles ML framing preview (see crop_rec.c). */
extern WEAK_FUNC(ret_0) void mlv_lite_info_framing_toggle(void);
extern WEAK_FUNC(ret_0) void mlv_lite_info_framing_reset(void);
extern WEAK_FUNC(ret_0) void mlv_lite_set_small_hacks_more(void);