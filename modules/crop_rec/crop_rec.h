/* crop_rec interface */
extern WEAK_FUNC(ret_0) int analog_gain_is_acive();
extern WEAK_FUNC(ret_0) int crop_rec_is_enabled();
extern WEAK_FUNC(ret_0) int crop_rec_request_preview_recovery();
extern WEAK_FUNC(ret_0) int is_LCD_Output();
extern WEAK_FUNC(ret_0) int is_480p_Output();
extern WEAK_FUNC(ret_0) int is_1080i_Full_Output();
extern WEAK_FUNC(ret_0) int is_1080i_Info_Output();
/* Core Live View touch editor interface.  control: 0=crop/mode+resolution,
 * 1=frame rate, 2=bit depth. */
extern WEAK_FUNC(ret_0) int crop_rec_touch_adjust(int control, int delta);
extern WEAK_FUNC(ret_0) int crop_rec_touch_get_value(int control, int slot,
                                                      char *value, int size,
                                                      int *enabled);
/* Custom page Movie controls: 0=Mode, 1=Aspect Ratio, 2=Preset. */
extern WEAK_FUNC(ret_0) int crop_rec_custom_adjust(int control, int delta);
