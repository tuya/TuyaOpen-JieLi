# TAL audio subsystem library (user-supplied)

`audio_subsys.a` and the headers in `tuyaos_adapter/include/tal_audio_subsys/`
come from the customer's JieLi TAL audio package (`jieli_lib_v2.zip` and
`include_tal_audio_subsys.zip`, 2026-10). The archive is pi32v2 LLVM bitcode
(clang 4.0.1) and is linked by the JIELI SDK's LTO wrapper.

What the archive actually exports (verified with llvm-nm):

- `rnn_vad_create/init/process/destroy/start/stop/set_callback`
- `keyword_tflite_config_init/get_mem_size/create/add_keyword/forward_pcm/destroy`
- `audio_subsys_speexdsp_wrap_*`
- C++ `wekws::CtcBeamSearch` / `CtcGreedyKws` / `KeywordSpottingTflite`

The high-level `audio_subsys_manager_*` / `audio_subsys_kws_tflm_*` /
`audio_subsys_vad_*` declarations in the headers are NOT exported by this
archive; the TKL adapter must use the lower-level engines above.

The archive embeds the nhty ("你好涂鸦") TFLite model
(`g_fsmn_epoch17_nhty_hty_int8_model_data`, 136,392 bytes). It carries no
token/word table, so `keyword_tflite_add_keyword()` still needs token IDs
from the training/export side.
